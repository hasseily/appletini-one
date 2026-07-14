/*
 * apple_cycle_egress.c -- Polling ring reader. See apple_cycle_egress.h
 * for the contract.
 *
 * Drains 64-bit AppleCycleRecord entries from a ring buffer in PS DDR
 * (written by the FPGA over AXI HP0). Updates a 128 KB shadow of Apple
 * video memory split as two 64 KB banks (main + aux) for the renderer.
 *
 * Polled from the main.c main loop -- no IRQ.
 */

#include <stdint.h>
#include <string.h>

#include "xil_cache.h"    /* Xil_DCacheFlushRange */
#include "xil_mmu.h"      /* Xil_SetTlbAttributes, NORM_NONCACHE */

#include "../lib/common.h"
#include "../lib/uart.h"

#include "apple_cycle_egress.h"

/* --- Public state -------------------------------------------------- */
volatile uint8_t  *const g_main_bank = (volatile uint8_t *)ACE_MAIN_BANK_ADDR;
volatile uint8_t  *const g_aux_bank  = (volatile uint8_t *)ACE_AUX_BANK_ADDR;

volatile uint32_t g_resync_pending = 0U;

volatile uint32_t g_records_processed  = 0U;
volatile uint32_t g_gap_markers_seen   = 0U;
volatile uint32_t g_bus_writes_seen    = 0U;
volatile uint32_t g_frame_records_seen = 0U;
volatile uint32_t g_poll_calls         = 0U;
volatile uint32_t g_oversized_drains   = 0U;

/* --- Internal state ------------------------------------------------- */
/* PS-side mirror of the FPGA's consumer pointer. We track it locally and
 * batch-write to ACE_REG_CFG_CONSUMER_PTR at the end of each drain pass. */
static uint32_t s_consumer_ptr;

/* --- Macros for ring access ---------------------------------------- */
#define ACE_RING_SLOT(off) \
    (*(volatile uint64_t *)(uintptr_t)(ACE_RING_BASE + (off)))
#define ACE_PRODUCER_SLOT() \
    (*(volatile uint32_t *)(uintptr_t)ACE_PRODUCER_PTR_ADDR)

/* --- Init ----------------------------------------------------------- */
int apple_cycle_egress_init(void)
{
    uint32_t i;

    /* (1) MMU. Mark the ring + producer-ptr 1 MB section non-cacheable
     *     so PS reads see FPGA HP0 writes immediately. The shadow bank
     *     section (0x3F100000) is intentionally left cacheable -- only
     *     this CPU writes/reads it, and the renderer (2b.2) wants the
     *     cache line speedup. */
    Xil_SetTlbAttributes(ACE_MMU_RING_SECTION, NORM_NONCACHE);

    /* (2) Zero producer-ptr slot. */
    REG_WRITE(ACE_PRODUCER_PTR_ADDR, 0U);

    /* (3) Zero the ring buffer. Non-cacheable: plain stores reach DDR. */
    for (i = 0U; i < (ACE_RING_SIZE_BYTES / 8U); i++) {
        ACE_RING_SLOT(i * 8U) = 0ULL;
    }

    /* (4) Zero shadow banks. Under AMP this init runs on CPU0 but
     * the actual writes happen on CPU1's egress poll, and reads
     * happen on CPU1's renderer. CPU0's L1 holding dirty zero
     * lines can later evict over CPU1's writes via L2/DDR, so
     * flush+invalidate after the memset to clear CPU0's L1 of
     * those addresses. CPU0 doesn't touch the shadow banks again,
     * so no further L1 lines get pulled in. */
    memset((void *)g_main_bank, 0, ACE_BANK_BYTES);
    memset((void *)g_aux_bank,  0, ACE_BANK_BYTES);
    Xil_DCacheFlushRange((INTPTR)g_main_bank, ACE_BANK_BYTES);
    Xil_DCacheFlushRange((INTPTR)g_aux_bank,  ACE_BANK_BYTES);
    __asm__ volatile ("dsb sy" ::: "memory");

    /* (5) Program egress config with cfg_enable=0 to gate writes during
     *     setup. */
    REG_WRITE(ACE_REG_CFG_ENABLE,         0U);
    REG_WRITE(ACE_REG_CFG_RING_BASE,      ACE_RING_BASE);
    REG_WRITE(ACE_REG_CFG_RING_SIZE_LOG2, ACE_RING_SIZE_LOG2);
    REG_WRITE(ACE_REG_CFG_PRODUCER_ADDR,  ACE_PRODUCER_PTR_ADDR);
    REG_WRITE(ACE_REG_CFG_CONSUMER_PTR,   0U);

    /* (6) Strobe internal egress reset. Diagnostic counters remain cumulative. */
    REG_WRITE(ACE_REG_CFG_RESET_PULSE, 1U);

    /* (7) Initialise PS-side state. Producer slot is 0 -> empty ring. */
    s_consumer_ptr  = 0U;
    /* (8) Enable. From here on the FPGA may write records. */
    REG_WRITE(ACE_REG_CFG_ENABLE, 1U);

    uart_puts(UART0_BASE,
        "apple_cycle_egress_init: ring 0x3F010000+64KB, producer 0x3F000000, "
        "shadows 0x3F100000/0x3F110000\r\n");
    return 0;
}

/* --- AMP secondary-core MMU setup ----------------------------------- */
void apple_cycle_egress_amp_secondary_init(void)
{
    /* Same TLB override as in apple_cycle_egress_init step (1).
     * Each core has its own MMU table so this must run on every
     * core that polls the ring or reads the producer-pointer slot.
     * Nothing else from init is repeated -- the PL register
     * configuration (cfg_enable, ring base, producer addr) is
     * write-once at boot and was already done by CPU0. */
    Xil_SetTlbAttributes(ACE_MMU_RING_SECTION, NORM_NONCACHE);
}

/* --- Poll ----------------------------------------------------------- */
void apple_cycle_egress_poll(void)
{
    uint32_t producer;
    uint32_t consumer;
    uint32_t budget = ACE_POLL_RECORD_CAP;

    g_poll_calls++;

    /* (1) Snapshot producer pointer. Non-cacheable load -- hits DDR. */
    producer = ACE_PRODUCER_SLOT();

    /* (2) Defensive validation. Bad producer -> skip this poll. */
    if ((producer & 7U) != 0U) {
        return;
    }
    if (producer >= ACE_RING_SIZE_BYTES) {
        return;
    }

    consumer = s_consumer_ptr;
    if (consumer == producer) {
        return;  /* empty */
    }

    /* (3) Drain. */
    while (consumer != producer) {
        uint64_t rec;

        if (budget == 0U) {
            g_oversized_drains++;
            break;
        }
        budget--;

        rec = ACE_RING_SLOT(consumer);

        if (rec == 0ULL) {
            /* Gap marker: both halves zero. The FPGA emits this on ring-full
             * or on-chip FIFO drop. The renderer clears resync_pending at the
             * next clean frame boundary. */
            g_gap_markers_seen++;
            g_resync_pending = 1U;
        } else {
            switch (ace_record_kind(rec)) {
            case ACE_RECORD_KIND_LEGACY:
                if (ace_addr_decode_en(rec)) {
                    uint32_t a = ace_addr_decode(rec);
                    uint8_t  d = ace_data(rec);
                    /* addr_decode bit 16 = 0 -> main, 1 -> aux. Low 16 bits
                     * index into the 64 KB bank. */
                    if ((a & 0x010000U) != 0U) {
                        g_aux_bank[a & 0xFFFFU] = d;
                    } else {
                        g_main_bank[a & 0xFFFFU] = d;
                    }
                    g_bus_writes_seen++;
                }

                if (ace_frame_en(rec)) {
                    g_frame_records_seen++;
                }
                break;

            case ACE_RECORD_KIND_IO_WRITE:
            case ACE_RECORD_KIND_SOFTSW_ACCESS:
            default:
                break;
            }
        }

        /* Forward records to the renderer when it is linked. */
        if (apple_cycle_renderer_on_record) {
            apple_cycle_renderer_on_record(rec);
        }

        g_records_processed++;
        consumer = (consumer + ACE_RECORD_BYTES) & ACE_RING_MASK;
    }

    /* (4) Publish new consumer pointer. Single AxiSimple write. */
    s_consumer_ptr = consumer;
    REG_WRITE(ACE_REG_CFG_CONSUMER_PTR, s_consumer_ptr);
}
