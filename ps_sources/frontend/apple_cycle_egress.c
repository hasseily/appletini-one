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
#include "apple_cycle_renderer.h"
#include "linear_text_overlay.h"

/* --- Public state -------------------------------------------------- */
volatile uint8_t  *const g_main_bank = (volatile uint8_t *)ACE_MAIN_BANK_ADDR;
volatile uint8_t  *const g_aux_bank  = (volatile uint8_t *)ACE_AUX_BANK_ADDR;

volatile uint32_t g_resync_pending = 0U;

volatile uint32_t g_records_processed  = 0U;
volatile uint32_t g_gap_markers_seen   = 0U;
volatile uint32_t g_bus_writes_seen    = 0U;
volatile uint32_t g_video_shadow_generation = 1U;
volatile uint32_t g_legacy_video_shadow_generation = 1U;
volatile uint32_t g_frame_records_seen = 0U;
volatile uint32_t g_poll_calls         = 0U;
volatile uint32_t g_ace_ring_cpu_cycles   = 0U;
volatile uint32_t g_ace_look_cpu_cycles   = 0U;
volatile uint32_t g_ace_render_cpu_cycles = 0U;
static uint32_t s_sample_divider = 0U;
/* Exact time of the renderer's frame work. Weak, as the renderer hooks are:
 * a build with no renderer has no such counter. */
extern volatile uint32_t g_acr_frame_cpu_cycles __attribute__((weak));

static inline uint32_t ace_frame_work_cycles(void)
{
    return (&g_acr_frame_cpu_cycles != NULL) ? g_acr_frame_cpu_cycles : 0U;
}

/* PMU cycle counter. CPU1 starts it in its main(). In a host build this
 * gives zero. */
static inline uint32_t ace_cpu_cycles(void)
{
#if defined(__arm__)
    uint32_t v;
    __asm__ volatile ("mrc p15, 0, %0, c9, c13, 0" : "=r"(v));
    return v;
#else
    return 0U;
#endif
}
volatile uint32_t g_oversized_drains   = 0U;

/* --- Internal state ------------------------------------------------- */
/* PS-side mirror of the FPGA's consumer pointer. */
static uint32_t s_consumer_ptr;

/* Cached lookahead batch. The ring is non-cacheable, so rereading two future
 * records for every rendered cycle costs far more than the phase correction.
 * Copy each record once, then do all lookahead work from this cached array. */
#define ACE_LOOKAHEAD_BATCH_RECORDS 256U
#define ACE_CONSUMER_RELEASE_BYTES (2048U * ACE_RECORD_BYTES)
static uint64_t s_lookahead_batch[ACE_LOOKAHEAD_BATCH_RECORDS];
static void (*s_checkpoint)(void);

void apple_cycle_egress_set_checkpoint(void (*checkpoint)(void))
{
    s_checkpoint = checkpoint;
}

/* --- Macros for ring access ---------------------------------------- */
#define ACE_RING_SLOT(off) \
    (*(volatile uint64_t *)(uintptr_t)(ACE_RING_BASE + (off)))
#define ACE_PRODUCER_SLOT() \
    (*(volatile uint32_t *)(uintptr_t)ACE_PRODUCER_PTR_ADDR)

/* --- Init ----------------------------------------------------------- */
int apple_cycle_egress_init(void)
{
    uint32_t i;

    /* (1) MMU. Mark the ring and producer-pointer sections non-cacheable
     *     so PS reads see FPGA HP0 writes immediately. The shadow bank
     *     section (0x3F100000) is intentionally left cacheable -- only
     *     this CPU writes/reads it, and the renderer (2b.2) wants the
     *     cache line speedup. */
    Xil_SetTlbAttributes(ACE_MMU_CONTROL_SECTION, NORM_NONCACHE);
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
        "apple_cycle_egress_init: ring 0x3F200000+1MB, producer 0x3F000000, "
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
    Xil_SetTlbAttributes(ACE_MMU_CONTROL_SECTION, NORM_NONCACHE);
    Xil_SetTlbAttributes(ACE_MMU_RING_SECTION, NORM_NONCACHE);
}

static int egress_frame_records_are_consecutive(uint64_t prior, uint64_t next)
{
    const uint32_t prior_line = ace_line_in_frame(prior);
    const uint32_t prior_cycle = ace_cycle_in_line(prior);
    const uint32_t next_line = ace_line_in_frame(next);
    const uint32_t next_cycle = ace_cycle_in_line(next);

    if (prior_cycle < 64u) {
        return next_line == prior_line && next_cycle == prior_cycle + 1u;
    }
    if (next_cycle != 0u) {
        return 0;
    }
    if (next_line == prior_line + 1u) {
        return 1;
    }
    return next_line == 0u && (prior_line == 261u || prior_line == 311u);
}

/* Find two future frame snapshots in the cached batch. A C029 write, gap, or
 * broken coordinate sequence marks a real boundary, so dispatch with the
 * snapshots already found. Running out of cached records is not a boundary;
 * the caller reloads from the current consumer position or waits for the next
 * producer update. Future records never update the video shadow here. */
static int egress_find_phase_lookahead(const uint64_t *records,
                                       uint32_t record_count,
                                       uint32_t current_index,
                                       uint64_t *next1,
                                       uint64_t *next2)
{
    uint32_t scan = current_index + 1u;
    uint64_t prior = records[current_index];
    uint32_t found = 0u;

    *next1 = 0ULL;
    *next2 = 0ULL;

    while (scan < record_count) {
        const uint64_t candidate = records[scan];

        if (candidate == 0ULL || ace_record_kind(candidate) == ACE_RECORD_KIND_GS_STATE ||
            ace_record_kind(candidate) == ACE_RECORD_KIND_GS_IO) {
            return 1;
        }
        if (ace_record_kind(candidate) == ACE_RECORD_KIND_IO_WRITE &&
            ace_io_addr(candidate) == 0xC029u) {
            return 1;
        }
        if (ace_record_kind(candidate) == ACE_RECORD_KIND_LEGACY &&
            ace_frame_en(candidate)) {
            if (!egress_frame_records_are_consecutive(prior, candidate)) {
                return 1;
            }
            if (found == 0u) {
                *next1 = candidate;
            } else {
                *next2 = candidate;
                return 1;
            }
            prior = candidate;
            found++;
        }
        scan++;
    }

    return 0;
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
    uint32_t released_consumer = consumer;

    /* (3) Drain in cached batches. Each non-cacheable ring slot is read once
     * in the common path; phase lookahead then reads normal cached memory. */
    while (consumer != producer) {
        uint32_t batch_count = 0u;
        uint32_t batch_scan = consumer;
        uint32_t processed = 0u;
        int batch_reaches_producer;

        if (budget == 0U) {
            g_oversized_drains++;
            break;
        }

        const uint32_t ring_start = ace_cpu_cycles();
        while (batch_scan != producer &&
               batch_count < ACE_LOOKAHEAD_BATCH_RECORDS) {
            s_lookahead_batch[batch_count++] = ACE_RING_SLOT(batch_scan);
            batch_scan = (batch_scan + ACE_RECORD_BYTES) & ACE_RING_MASK;
            if ((batch_count & 7U) == 0U && s_checkpoint != NULL) {
                s_checkpoint();
            }
        }
        g_ace_ring_cpu_cycles += ace_cpu_cycles() - ring_start;
        batch_reaches_producer = batch_scan == producer;

        for (uint32_t i = 0u; i < batch_count && budget != 0u; ++i) {
            const uint64_t rec = s_lookahead_batch[i];
            uint64_t next1 = 0ULL;
            uint64_t next2 = 0ULL;
            int lookahead_ready = 1;
            const uint32_t sample =
                ((++s_sample_divider) & ((1U << ACE_SAMPLE_SHIFT) - 1U)) == 0U;

            if (apple_cycle_renderer_needs_phase_lookahead &&
                apple_cycle_renderer_on_record_lookahead &&
                apple_cycle_renderer_needs_phase_lookahead(rec)) {
                const uint32_t look_start = sample ? ace_cpu_cycles() : 0U;
                lookahead_ready = egress_find_phase_lookahead(
                    s_lookahead_batch, batch_count, i, &next1, &next2);
                if (sample) {
                    g_ace_look_cpu_cycles +=
                        (ace_cpu_cycles() - look_start) << ACE_SAMPLE_SHIFT;
                }
            }
            if (!lookahead_ready) {
                /* Normally only the final two frame records reach here. If
                 * an abnormal stream has no two frame records in a complete
                 * 256-record batch, dispatch without them so it cannot pin
                 * the consumer forever. */
                if (i == 0u && !batch_reaches_producer &&
                    batch_count == ACE_LOOKAHEAD_BATCH_RECORDS) {
                    lookahead_ready = 1;
                } else {
                    break;
                }
            }
            budget--;
            if ((budget & 7U) == 0U && s_checkpoint != NULL) {
                s_checkpoint();
            }

            if (apple_cycle_renderer_before_record) {
                apple_cycle_renderer_before_record(rec);
            }

            if (rec == 0ULL) {
                /* Gap marker: both halves zero. The FPGA emits this on
                 * ring-full or on-chip FIFO drop. */
                g_gap_markers_seen++;
                g_resync_pending = 1U;
                linear_text_overlay_capture_on_gap();
            } else {
                switch (ace_record_kind(rec)) {
                case ACE_RECORD_KIND_LEGACY:
                case ACE_RECORD_KIND_GS_SHADOW:
                    if (ace_addr_decode_en(rec)) {
                        uint32_t a = ace_addr_decode(rec);
                        uint8_t  d = ace_data(rec);
                        if ((a & 0x010000U) != 0U) {
                            g_aux_bank[a & 0xFFFFU] = d;
                            if (ace_record_kind(rec) == ACE_RECORD_KIND_LEGACY &&
                                (a & 0xFFFFU) == 0x9DF8U) {
                                apple_cycle_renderer_note_aux_ctrl_write(d);
                            }
                        } else {
                            g_main_bank[a & 0xFFFFU] = d;
                        }
                        if (ace_record_kind(rec) == ACE_RECORD_KIND_LEGACY)
                            linear_text_overlay_capture_on_write(a, d);
                        if ((uint16_t)(a & 0xFFFFU) >= 0x2000U &&
                            (uint16_t)(a & 0xFFFFU) <= 0x9FFFU) {
                            g_video_shadow_generation++;
                        }
                        const uint16_t video_addr =
                            (uint16_t)(a & 0xFFFFU);
                        if ((video_addr >= 0x0400U &&
                             video_addr <= 0x0BFFU) ||
                            (video_addr >= 0x2000U &&
                             video_addr <= 0x9FFFU)) {
                            g_legacy_video_shadow_generation++;
                        }
                        g_bus_writes_seen++;
                    }

                    if (ace_record_kind(rec) == ACE_RECORD_KIND_LEGACY && ace_frame_en(rec)) {
                        g_frame_records_seen++;
                    }
                    break;

                case ACE_RECORD_KIND_IO_WRITE:
                    if (ace_io_addr(rec) == 0xC0F3U) {
                        linear_text_overlay_capture_on_command(
                            ace_io_data(rec));
                    }
                    break;
                case ACE_RECORD_KIND_SOFTSW_ACCESS:
                default:
                    break;
                }
            }

            /* Apply only this record's shadow write before dispatch. Future
             * cached records contribute switch bits, never pixels. */
            {
                const uint32_t render_start = sample ? ace_cpu_cycles() : 0U;
                const uint32_t frame_start =
                    sample ? ace_frame_work_cycles() : 0U;
                if (apple_cycle_renderer_on_record_lookahead) {
                    apple_cycle_renderer_on_record_lookahead(rec, next1, next2);
                } else if (apple_cycle_renderer_on_record) {
                    apple_cycle_renderer_on_record(rec);
                }
                if (sample) {
                    /* The frame work is counted exactly elsewhere. */
                    g_ace_render_cpu_cycles +=
                        ((ace_cpu_cycles() - render_start) -
                         (ace_frame_work_cycles() - frame_start))
                            << ACE_SAMPLE_SHIFT;
                }
            }

            g_records_processed++;
            consumer = (consumer + ACE_RECORD_BYTES) & ACE_RING_MASK;
            processed++;
        }

        if (processed == 0u) {
            break;
        }

        /* A large snapshot can take much longer to render than it takes
         * the FPGA to fill the remaining space. Release processed records
         * during a long drain so the producer can reuse their ring slots.
         * Keep any undispatched lookahead tail in the ring for the next batch. */
        if (((consumer - released_consumer) & ACE_RING_MASK) >=
            ACE_CONSUMER_RELEASE_BYTES) {
            REG_WRITE(ACE_REG_CFG_CONSUMER_PTR, consumer);
            released_consumer = consumer;
        }
    }

    /* (4) Release the remainder of the processed records. */
    s_consumer_ptr = consumer;
    if (consumer != released_consumer) {
        REG_WRITE(ACE_REG_CFG_CONSUMER_PTR, consumer);
    }
}
