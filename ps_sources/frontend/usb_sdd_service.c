/******************************************************************************
 * usb_sdd_service.c -- see usb_sdd_service.h.
 *
 * Ownership model: the storage service owns the USB0 controller instance
 * and its GIC wiring permanently. This service borrows the instance while
 * the SDD personality is active (storage soft-disconnected first), swaps
 * in its own device config / endpoint handlers, and hands everything back
 * on stop. main.c routes usb0 polling to exactly one of the two services
 * based on usb0_personality_get().
 ******************************************************************************/

#include <stdio.h>
#include <string.h>

#include "xusbps.h"
#include "xusbps_hw.h"
#include "xusbps_ch9.h"
#include "xil_cache.h"
#include "xil_printf.h"

#include "usb_sdd_vendor.h"

#include "../lib/common.h"
#include "../lib/uart.h"
#include "card_control_regs.h"
#include "usb_sdd_service.h"
#include "usb0_personality.h"
#include "usb_storage_service.h"
#include "no_slot_clock_control.h"
#include "../lib/rtc_pcf8563.h"

/* ------------------------------------------------------------------ */
/* Constants                                                            */
/* ------------------------------------------------------------------ */

#define SDD_IRQ_MASK (XUSBPS_IXR_UR_MASK | XUSBPS_IXR_UI_MASK | \
                      XUSBPS_IXR_PC_MASK | XUSBPS_IXR_UE_MASK)

#define SDD_EVENTS_PER_MSG      4000U  /* 16 KB WinUSB transfers */
#define SDD_MSG_MAX_WORDS       (2U + SDD_EVENTS_PER_MSG)
#define SDD_MSG_MAX_BYTES       (SDD_MSG_MAX_WORDS * 4U)

/* The main loop is vblank-paced (~50 Hz), so per-poll capacity must
 * clear the full Apple bus rate (~1.02M events/s) with margin:
 * 14 slots x 4000 events x 50 Hz = 2.8M events/s. Slots stay below the
 * EP1-IN ring depth (NumBufs 16). */
#define SDD_TX_SLOTS            14U    /* < EP1 IN NumBufs */
#define SDD_TX_BUDGET_PER_POLL  14U    /* max messages queued per poll  */

#define SDD_ADDR_STATUS         0x1000U
#define SDD_ADDR_EVENTS         0x1004U
#define SDD_ADDR_WATCHDOG       0x0008U
#define SDD_ADDR_NSC_TIME_0     0x0014U
#define SDD_ADDR_NSC_TIME_1     0x0018U

#define SDD_STATUS_ENABLE_BIT   (1U << 0)
#define SDD_STATUS_OVERFLOW_BIT (1U << 1)


/* ------------------------------------------------------------------ */
/* State                                                                */
/* ------------------------------------------------------------------ */

static XUsbPs_Local SddLocalData;

/* Slot stride padded to the cache line so every slot stays 32-byte
 * aligned (flush granularity). */
#define SDD_TX_SLOT_BYTES ((SDD_MSG_MAX_BYTES + 31U) & ~31U)
static u8 SddTxBuf[SDD_TX_SLOTS][SDD_TX_SLOT_BYTES]
    __attribute__((aligned(32)));

/* Monotonic counters: submitted only advances in poll context, completed
 * only in IRQ context; in-flight = submitted - completed needs no lock. */
static volatile u32 SddTxSubmitted;
static volatile u32 SddTxCompleted;

static volatile u8 SddActive;         /* personality switched + attached */
static volatile u8 SddHostConfigured; /* SET_CONFIGURATION seen          */
static volatile u8 SddForwarding;     /* host enabled bus events         */
static volatile u8 SddOverflowLatched;
static volatile u8 SddStatusPending;  /* need to send a 0x1000 message   */
static u8 SddStreamStarted;           /* PL ring initialized at least once */

static u32 SddConsumerPtr;            /* byte offset within the ring     */

/* host->card message reassembly (messages can split across USB packets).
 * Appended by the EP2-OUT event handler in IRQ context; parsed/compacted
 * by the poll under an IRQ mask. */
static u8 SddRxBuf[1024];
static volatile u32 SddRxLen;

/* stats */
static u32 SddEventsSent;
static u32 SddMessagesSent;
static u32 SddOverflowCount;
static u32 SddHostEnables;
static u32 SddWatchdogWrites;
static u32 SddUnknownWrites;

/* ------------------------------------------------------------------ */
/* PL tap control                                                       */
/* ------------------------------------------------------------------ */

static void sdd_pl_stream_stop(void)
{
    REG_WRITE(SDD_REG_CFG_ENABLE, 0U);
}

static void sdd_pl_stream_start(void)
{
    volatile uint32_t *producer =
        (volatile uint32_t *)(uintptr_t)SDD_PRODUCER_PTR_ADDR;

    REG_WRITE(SDD_REG_CFG_ENABLE,         0U);
    REG_WRITE(SDD_REG_CFG_RING_BASE,      SDD_RING_BASE);
    REG_WRITE(SDD_REG_CFG_RING_SIZE_LOG2, SDD_RING_SIZE_LOG2);
    REG_WRITE(SDD_REG_CFG_PRODUCER_ADDR,  SDD_PRODUCER_PTR_ADDR);
    REG_WRITE(SDD_REG_CFG_CONSUMER_PTR,   0U);
    *producer = 0U;
    __asm__ volatile("dsb" ::: "memory");
    REG_WRITE(SDD_REG_CFG_RESET_PULSE, 1U);
    SddConsumerPtr = 0U;
    SddStreamStarted = 1U;
    REG_WRITE(SDD_REG_CFG_ENABLE, 1U);
}

/* ------------------------------------------------------------------ */
/* TX path (card -> host, EP2 IN)                                       */
/* ------------------------------------------------------------------ */

/* The XUsbPs dTD rings are manipulated by BOTH the poll context (sends,
 * receives) and the USB ISR (completion reclaim). They are not safe
 * against each other: an unguarded EpBufferSend racing the ISR's dTD
 * walk corrupts the ring into a cycle and the ISR never exits (observed
 * live via JTAG: CPU0 pinned inside XUsbPs_IntrHandler's invalidate
 * loop, whole card frozen). Mirror the storage class's discipline and
 * mask IRQs around every ring operation. */
static u32 sdd_irq_save(void)
{
    u32 cpsr;

    __asm__ volatile ("mrs %0, cpsr" : "=r"(cpsr));
    __asm__ volatile ("cpsid i" ::: "memory");
    return cpsr;
}

static void sdd_irq_restore(u32 cpsr)
{
    if ((cpsr & 0x80U) == 0U) {
        __asm__ volatile ("cpsie i" ::: "memory");
    }
}

static u32 sdd_tx_in_flight(void)
{
    u32 sub = SddTxSubmitted;
    u32 comp = SddTxCompleted;

    /* Self-heal: spurious completions (endpoint flush during a host
     * reset) can push completed past submitted; treat that as an empty
     * pipe instead of an underflowed-huge one. */
    if ((s32)(sub - comp) < 0) {
        SddTxCompleted = sub;
        return 0U;
    }
    return sub - comp;
}

static int sdd_tx_send_words(const u32 *words, u32 word_count)
{
    XUsbPs *inst = usb_storage_service_usb_instance();
    u8 *slot;
    u32 bytes = word_count * 4U;
    u32 cpsr;
    int status;

    if (sdd_tx_in_flight() >= SDD_TX_SLOTS) {
        return XST_FAILURE;
    }
    slot = SddTxBuf[SddTxSubmitted % SDD_TX_SLOTS];
    memcpy(slot, words, bytes);
    Xil_DCacheFlushRange((unsigned int)slot, bytes);
    cpsr = sdd_irq_save();
    status = XUsbPs_EpBufferSend(inst, SDDV_EP_DATA, slot, bytes);
    sdd_irq_restore(cpsr);
    if (status == XST_SUCCESS) {
        SddTxSubmitted++;
    }
    return status;
}

static int sdd_tx_send_status(void)
{
    u32 msg[3];

    msg[0] = 1U;               /* count=1, incr=0 */
    msg[1] = SDD_ADDR_STATUS;
    msg[2] = (SddForwarding ? SDD_STATUS_ENABLE_BIT : 0U) |
             (SddOverflowLatched ? SDD_STATUS_OVERFLOW_BIT : 0U);
    return sdd_tx_send_words(msg, 3U);
}

/* ------------------------------------------------------------------ */
/* Ring drain                                                           */
/* ------------------------------------------------------------------ */

static void sdd_drain_ring(void)
{
    static u32 msg[SDD_MSG_MAX_WORDS];
    volatile uint32_t *producer_cell =
        (volatile uint32_t *)(uintptr_t)SDD_PRODUCER_PTR_ADDR;
    u32 budget = SDD_TX_BUDGET_PER_POLL;
    u32 producer;

    if (!SddForwarding || SddOverflowLatched) {
        /* Not forwarding: discard whatever the PL wrote so the ring
         * can't wrap while the host is idle. Only meaningful once the
         * ring/producer cell have been initialized by a stream start;
         * before that the DDR cell holds garbage. */
        if (SddStreamStarted) {
            producer = *producer_cell;
            if (producer != SddConsumerPtr) {
                SddConsumerPtr = producer;
                REG_WRITE(SDD_REG_CFG_CONSUMER_PTR, SddConsumerPtr);
            }
        }
        return;
    }

    /* The consumer can never be more than one ring behind the producer. An
     * impossible distance indicates pointer corruption, so restart the stream
     * before entering the TX-slot gate. */
    producer = *producer_cell;
    if ((producer != SddConsumerPtr) &&
        (((producer - SddConsumerPtr) & (SDD_RING_SIZE_BYTES - 1U)) >
         (SDD_RING_SIZE_BYTES - 64U))) {
        SddOverflowCount++;
        sdd_pl_stream_start();
        return;
    }

    while (budget != 0U && sdd_tx_in_flight() < SDD_TX_SLOTS) {
        u32 batch_start;
        u32 count = 0U;
        u8 overflow_hit = 0U;

        producer = *producer_cell;
        if (producer == SddConsumerPtr) {
            break;
        }

        batch_start = SddConsumerPtr;
        while (count < SDD_EVENTS_PER_MSG && SddConsumerPtr != producer) {
            const volatile u32 *rec = (const volatile u32 *)(uintptr_t)
                (SDD_RING_BASE + SddConsumerPtr);
            u32 lo = rec[0];
            u32 hi = rec[1];

            SddConsumerPtr = (SddConsumerPtr + SDD_RECORD_BYTES) &
                             (SDD_RING_SIZE_BYTES - 1U);

            if (lo == 0U && hi == 0U) {
                /* Egress gap marker: the tap FIFO overflowed. */
                overflow_hit = 1U;
                break;
            }
            msg[2U + count] = lo;
            count++;
        }

        if (overflow_hit) {
            /* Run the overflow protocol even if the send fails. Events are
             * already lost, so both pointers must consume the marker together;
             * host re-enable performs a full stream restart. */
            if (count != 0U) {
                msg[0] = count;        /* incr=0 */
                msg[1] = SDD_ADDR_EVENTS;
                if (sdd_tx_send_words(msg, 2U + count) == XST_SUCCESS) {
                    SddEventsSent += count;
                    SddMessagesSent++;
                }
            }
            SddOverflowLatched = 1U;
            SddForwarding = 0U;
            SddOverflowCount++;
            SddStatusPending = 1U;
            sdd_pl_stream_stop();
            break;
        }

        if (count != 0U) {
            msg[0] = count;            /* incr=0 */
            msg[1] = SDD_ADDR_EVENTS;
            if (sdd_tx_send_words(msg, 2U + count) != XST_SUCCESS) {
                /* No slot: restore the exact pre-batch consumer and
                 * retry next poll. Never arithmetic-rewind -- a batch
                 * that swallowed a gap marker advanced one extra
                 * record and count-based math strands the consumer
                 * ahead of the producer. */
                SddConsumerPtr = batch_start;
                break;
            }
            SddEventsSent += count;
            SddMessagesSent++;
        }
        budget--;
    }

    REG_WRITE(SDD_REG_CFG_CONSUMER_PTR, SddConsumerPtr);
}

/* ------------------------------------------------------------------ */
/* RX path (host -> card, EP2 OUT register messages)                    */
/* ------------------------------------------------------------------ */

static void sdd_apply_nsc_time(u32 w0, u32 w1)
{
    rtc_pcf8563_time_t t;

    memset(&t, 0, sizeof(t));
    /* w0: [7:4]=centisec hi/lo (ignored), [15:8]=sec BCD, [23:16]=min BCD,
     * [31:24]=hour BCD. w1: [7:0]=dow BCD (1-based), [15:8]=day BCD,
     * [23:16]=month BCD, [31:24]=year BCD (two digits). */
    t.sec   = (uint8_t)((((w0 >> 12) & 0xFU) * 10U) + ((w0 >> 8)  & 0xFU));
    t.min   = (uint8_t)((((w0 >> 20) & 0xFU) * 10U) + ((w0 >> 16) & 0xFU));
    t.hour  = (uint8_t)((((w0 >> 28) & 0xFU) * 10U) + ((w0 >> 24) & 0xFU));
    t.wday  = (uint8_t)(((((w1 >> 4) & 0xFU) * 10U) + (w1 & 0xFU)));
    if (t.wday != 0U) {
        t.wday--;               /* SDD sends tm_wday+1 */
    }
    t.day   = (uint8_t)((((w1 >> 12) & 0xFU) * 10U) + ((w1 >> 8)  & 0xFU));
    t.month = (uint8_t)((((w1 >> 20) & 0xFU) * 10U) + ((w1 >> 16) & 0xFU));
    t.year  = (uint16_t)(2000U + (((w1 >> 28) & 0xFU) * 10U) +
                         ((w1 >> 24) & 0xFU));
    t.valid = 1U;
    no_slot_clock_control_publish_rtc(&t);
}

static void sdd_handle_reg_write(u32 addr, u32 value)
{
    static u32 nsc_w0;

    switch (addr) {
        case SDD_ADDR_STATUS:
            if ((value & SDD_STATUS_ENABLE_BIT) != 0U) {
                SddHostEnables++;
                SddOverflowLatched = 0U;
                SddForwarding = 1U;
                sdd_pl_stream_start();
            } else {
                SddForwarding = 0U;
                sdd_pl_stream_stop();
            }
            break;
        case SDD_ADDR_NSC_TIME_0:
            nsc_w0 = value;
            break;
        case SDD_ADDR_NSC_TIME_1:
            sdd_apply_nsc_time(nsc_w0, value);
            break;
        case SDD_ADDR_WATCHDOG:
            SddWatchdogWrites++;
            break;
        default:
            SddUnknownWrites++;
            break;
    }
}

/* Consume complete [hdr][addr][data...] messages from SddRxBuf. */
static void sdd_parse_rx_buffer(void)
{
    u32 off = 0U;

    for (;;) {
        u32 avail = SddRxLen - off;
        u32 hdr, addr, count, incr, need, i;

        if (avail < 8U) {
            break;
        }
        memcpy(&hdr,  &SddRxBuf[off], 4U);
        memcpy(&addr, &SddRxBuf[off + 4U], 4U);
        count = hdr & 0xFFU;
        incr  = (hdr >> 31) & 1U;
        need  = 8U + count * 4U;
        if (avail < need) {
            break;
        }
        for (i = 0U; i < count; i++) {
            u32 value;
            memcpy(&value, &SddRxBuf[off + 8U + i * 4U], 4U);
            sdd_handle_reg_write(addr, value);
            if (incr) {
                addr += 4U;
            }
        }
        off += need;
    }

    if (off != 0U) {
        if (off < SddRxLen) {
            memmove(SddRxBuf, &SddRxBuf[off], SddRxLen - off);
        }
        SddRxLen -= off;
    }
}

static void sdd_pump_rx(void)
{
    u32 cpsr;

    /* All OUT-endpoint consumption happens in the event handlers (see
     * SddEpDataEventHandler); the poll only parses what they appended.
     * The single-word peek is safe unmasked; the parse + compaction runs
     * masked so it cannot race the ISR's append. */
    if (SddRxLen == 0U) {
        return;
    }
    cpsr = sdd_irq_save();
    sdd_parse_rx_buffer();
    sdd_irq_restore(cpsr);
}

/* ------------------------------------------------------------------ */
/* Endpoint / controller event handlers (IRQ context)                   */
/* ------------------------------------------------------------------ */

static void SddEp0EventHandler(void *CallBackRef, u8 EpNum,
                               u8 EventType, void *Data)
{
    XUsbPs *InstancePtr = (XUsbPs *)CallBackRef;
    XUsbPs_SetupData SetupData;
    u8 *BufferPtr;
    u32 BufferLen;
    u32 Handle;

    (void)Data;

    switch (EventType) {
        case XUSBPS_EP_EVENT_SETUP_DATA_RECEIVED:
            if (XUsbPs_EpGetSetupData(InstancePtr, EpNum, &SetupData) ==
                XST_SUCCESS) {
                (void)XUsbPs_Ch9HandleSetupPacket(InstancePtr, &SetupData);
            }
            break;
        case XUSBPS_EP_EVENT_DATA_RX:
            if (XUsbPs_EpBufferReceive(InstancePtr, EpNum, &BufferPtr,
                                       &BufferLen, &Handle) == XST_SUCCESS) {
                XUsbPs_EpBufferRelease(Handle);
                /* Complete a vendor OUT whose data stage just landed:
                 * the payload is ignored (our chip config is fixed) and
                 * the host gets its status-stage ZLP. The vendor handler
                 * must not wait for this in the ISR (see
                 * usb_ft60x_vendor_req). */
                if (g_sddv_vendor_out_ack_owed) {
                    g_sddv_vendor_out_ack_owed = 0U;
                    (void)XUsbPs_EpBufferSend(InstancePtr, 0, NULL, 0);
                }
            }
            break;
        default:
            break;
    }
}

/* OUT-endpoint buffers MUST be consumed and released inside the event
 * handler: the XUsbPs ISR's RX walk only terminates at an ACTIVE dTD, so
 * if every buffer in the circular ring completes before the app re-arms
 * any (deferred main-loop draining), the walk cycles the all-inactive
 * ring forever and never returns (JTAG-captured: 4-dTD EP1-OUT cycle,
 * every token completed). Same contract the storage class follows. */
static void SddEpDataEventHandler(void *CallBackRef, u8 EpNum,
                                  u8 EventType, void *Data)
{
    XUsbPs *InstancePtr = (XUsbPs *)CallBackRef;
    u8 *BufferPtr;
    u32 BufferLen;
    u32 Handle;

    (void)Data;

    if (EventType == XUSBPS_EP_EVENT_DATA_TX) {
        SddTxCompleted++;
        return;
    }
    if (EventType != XUSBPS_EP_EVENT_DATA_RX) {
        return;
    }
    while (XUsbPs_EpBufferReceive(InstancePtr, EpNum, &BufferPtr,
                                  &BufferLen, &Handle) == XST_SUCCESS) {
        if (BufferLen > 0U &&
            (SddRxLen + BufferLen) <= sizeof(SddRxBuf)) {
            memcpy(&SddRxBuf[SddRxLen], BufferPtr, BufferLen);
            SddRxLen += BufferLen;
        } else if (BufferLen > 0U) {
            /* Host flooded us with garbage: resync the parser. */
            SddRxLen = 0U;
        }
        XUsbPs_EpBufferRelease(Handle);
    }
}

static void SddIntrHandler(void *CallBackRef, u32 Mask)
{
    XUsbPs *InstancePtr = (XUsbPs *)CallBackRef;

    if (Mask & XUSBPS_IXR_UR_MASK) {
        /* USB bus reset: host restarted enumeration. */
        SddHostConfigured = 0U;
        SddForwarding = 0U;
        SddTxCompleted = SddTxSubmitted;
        (void)InstancePtr;
    }
}

/* ------------------------------------------------------------------ */
/* Device configuration                                                 */
/* ------------------------------------------------------------------ */

static void sdd_build_device_config(XUsbPs_DeviceConfig *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    cfg->EpCfg[0].Out.Type          = XUSBPS_EP_TYPE_CONTROL;
    cfg->EpCfg[0].Out.NumBufs       = 2;
    cfg->EpCfg[0].Out.BufSize       = 64;
    cfg->EpCfg[0].Out.MaxPacketSize = 64;
    cfg->EpCfg[0].In.Type           = XUSBPS_EP_TYPE_CONTROL;
    cfg->EpCfg[0].In.NumBufs        = 2;
    cfg->EpCfg[0].In.MaxPacketSize  = 64;

    /* EP1: single data channel, byte-for-byte the storage layout the
     * hardware has proven under sustained load. */
    cfg->EpCfg[1].Out.Type          = XUSBPS_EP_TYPE_BULK;
    cfg->EpCfg[1].Out.NumBufs       = 16;
    cfg->EpCfg[1].Out.BufSize       = 512;
    cfg->EpCfg[1].Out.MaxPacketSize = 512;
    cfg->EpCfg[1].In.Type           = XUSBPS_EP_TYPE_BULK;
    cfg->EpCfg[1].In.NumBufs        = 16;
    cfg->EpCfg[1].In.MaxPacketSize  = 512;

    cfg->NumEndpoints = 2;
}

static int sdd_configure_device(void)
{
    XUsbPs *inst = usb_storage_service_usb_instance();
    XUsbPs_DeviceConfig cfg;
    int status;

    XUsbPs_IntrDisable(inst, XUSBPS_IXR_ALL);
    /* Ack anything already latched: an IRQ pending in the GIC at this
     * point would otherwise run the ISR against the half-rebuilt dTD
     * rings below (observed live: the ISR walks a re-linked ring in a
     * cycle and never exits). The caller additionally masks CPU IRQs
     * around the whole switch. */
    XUsbPs_WriteReg(inst->Config.BaseAddress, XUSBPS_ISR_OFFSET,
                    XUSBPS_IXR_ALL);

    sdd_build_device_config(&cfg);
    /* Uncached arena (see SDD_DMA_ARENA_ADDR): no flush needed, and the
     * controller and CPU can never diverge on descriptor state. */
    memset((void *)(uintptr_t)SDD_DMA_ARENA_ADDR, 0, SDD_DMA_ARENA_BYTES);
    cfg.DMAMemPhys = SDD_DMA_ARENA_ADDR;

    memset(&SddLocalData, 0, sizeof(SddLocalData));
    inst->UserDataPtr = &SddLocalData;
    inst->CurrentAltSetting = XUSBPS_DEFAULT_ALT_SETTING;
    inst->IsConfigDone = 0U;

    status = XUsbPs_ConfigureDevice(inst, &cfg);
    if (status != XST_SUCCESS) {
        return status;
    }

    status = XUsbPs_IntrSetHandler(inst, SddIntrHandler, inst,
                                   SDD_IRQ_MASK);
    if (status != XST_SUCCESS) {
        return status;
    }
    status = XUsbPs_EpSetHandler(inst, 0, XUSBPS_EP_DIRECTION_OUT,
                                 SddEp0EventHandler, inst);
    if (status != XST_SUCCESS) {
        return status;
    }
    status = XUsbPs_EpSetHandler(inst, SDDV_EP_DATA,
                                 XUSBPS_EP_DIRECTION_OUT,
                                 SddEpDataEventHandler, inst);
    if (status != XST_SUCCESS) {
        return status;
    }
    return XUsbPs_EpSetHandler(inst, SDDV_EP_DATA,
                               XUSBPS_EP_DIRECTION_IN,
                               SddEpDataEventHandler, inst);
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

int usb_sdd_service_init(void)
{
    SddActive = 0U;
    SddForwarding = 0U;
    return 0;
}

int usb_sdd_service_active(void)
{
    return SddActive != 0U;
}

void usb_sdd_service_note_configured(void)
{
    /* SET_CONFIGURATION flushes + reinits EP1-IN, and the ISR reports
     * completions for the flushed ring descriptors. Reset the TX
     * accounting or a mid-stream re-enumeration leaves completed >
     * submitted, the unsigned in-flight count underflows huge, and the
     * drain gate never opens again (seen live: sub=213 comp=229
     * inflight=4294967280, stream frozen). */
    SddTxSubmitted = 0U;
    SddTxCompleted = 0U;
    SddHostConfigured = 1U;
}

int usb_sdd_service_start(void)
{
    XUsbPs *inst;
    u32 cpsr;
    int status;

    if (SddActive) {
        return 0;
    }

    /* Detach the storage personality from the bus first. */
    (void)usb_storage_service_disconnect();
    usb0_personality_set(USB0_PERSONALITY_SDD);

    SddTxSubmitted = 0U;
    SddTxCompleted = 0U;
    SddHostConfigured = 0U;
    SddForwarding = 0U;
    SddOverflowLatched = 0U;
    SddStatusPending = 0U;
    SddStreamStarted = 0U;
    SddRxLen = 0U;
    SddConsumerPtr = 0U;
    sdd_pl_stream_stop();

    /* Reconfiguration is atomic against the USB ISR. A latched GIC IRQ must
     * not observe half-initialized dTD rings. */
    cpsr = sdd_irq_save();
    status = sdd_configure_device();
    if (status != XST_SUCCESS) {
        sdd_irq_restore(cpsr);
        usb0_personality_set(USB0_PERSONALITY_STORAGE);
        xil_printf("SDD: USB0 configure failed: %d\r\n", status);
        return status;
    }

    inst = usb_storage_service_usb_instance();
    XUsbPs_IntrEnable(inst, SDD_IRQ_MASK);
    XUsbPs_Start(inst);
    /* Present the device to the host (pullup). */
    XUsbPs_SetBits(inst, XUSBPS_OTGCSR_OFFSET, XUSBPS_OTGSC_OT_MASK);
    (void)XUsbPs_EpPrime(inst, 0, XUSBPS_EP_DIRECTION_OUT);
    sdd_irq_restore(cpsr);

    SddActive = 1U;
    xil_printf("SDD: USB0 now streaming for SuperDuperDisplay\r\n");
    return 0;
}

void usb_sdd_service_stop(void)
{
    XUsbPs *inst;
    u32 cpsr;

    if (!SddActive) {
        return;
    }
    inst = usb_storage_service_usb_instance();

    SddForwarding = 0U;
    sdd_pl_stream_stop();

    /* Soft-disconnect and hand-back are atomic against the USB ISR so a
     * latched IRQ cannot walk descriptor rings during reconstruction. */
    cpsr = sdd_irq_save();
    XUsbPs_IntrDisable(inst, XUSBPS_IXR_ALL);
    XUsbPs_WriteReg(inst->Config.BaseAddress, XUSBPS_CMD_OFFSET, 0);
    XUsbPs_WriteReg(inst->Config.BaseAddress, XUSBPS_OTGCSR_OFFSET,
                    XUsbPs_ReadReg(inst->Config.BaseAddress,
                                   XUSBPS_OTGCSR_OFFSET) &
                    ~(XUSBPS_OTGSC_OT_MASK | XUSBPS_OTGSC_DP_MASK));
    XUsbPs_WriteReg(inst->Config.BaseAddress, XUSBPS_ISR_OFFSET,
                    XUSBPS_IXR_ALL);

    SddActive = 0U;
    SddHostConfigured = 0U;

    /* Hand USB0 back to the storage personality, but leave it detached until
     * the user explicitly starts the SD-card bridge from the USB tab. */
    usb0_personality_set(USB0_PERSONALITY_STORAGE);
    sdd_irq_restore(cpsr);
    xil_printf("SDD: USB0 detached\r\n");
}

void usb_sdd_service_poll(void)
{
    if (!SddActive) {
        return;
    }

    sdd_pump_rx();

    if (!SddHostConfigured) {
        return;
    }

    if (SddStatusPending) {
        if (sdd_tx_send_status() == XST_SUCCESS) {
            SddStatusPending = 0U;
        }
    }

    sdd_drain_ring();
}

void usb_sdd_service_print_status(uint32_t uart_base)
{
    char line[96];

    (void)snprintf(line, sizeof(line),
                   "sdd: %s cfg=%u fwd=%u ovf=%u(x%lu)\r\n",
                   SddActive ? "ACTIVE" : "off",
                   (unsigned)SddHostConfigured,
                   (unsigned)SddForwarding,
                   (unsigned)SddOverflowLatched,
                   (unsigned long)SddOverflowCount);
    uart_puts(uart_base, line);
    (void)snprintf(line, sizeof(line),
                   "sdd: events=%lu msgs=%lu enables=%lu wdog=%lu unk=%lu\r\n",
                   (unsigned long)SddEventsSent,
                   (unsigned long)SddMessagesSent,
                   (unsigned long)SddHostEnables,
                   (unsigned long)SddWatchdogWrites,
                   (unsigned long)SddUnknownWrites);
    uart_puts(uart_base, line);
    (void)snprintf(line, sizeof(line),
                   "sdd: tx sub=%lu comp=%lu inflight=%lu\r\n",
                   (unsigned long)SddTxSubmitted,
                   (unsigned long)SddTxCompleted,
                   (unsigned long)(SddTxSubmitted - SddTxCompleted));
    uart_puts(uart_base, line);
    (void)snprintf(line, sizeof(line),
                   "sdd: pl prod=%08lx cons=%08lx recs=%lu gaps=%lu\r\n",
                   (unsigned long)REG_READ(SDD_REG_STAT_PRODUCER_PTR),
                   (unsigned long)SddConsumerPtr,
                   (unsigned long)REG_READ(SDD_REG_STAT_RECORDS),
                   (unsigned long)REG_READ(SDD_REG_STAT_GAP_MARKERS));
    uart_puts(uart_base, line);
    (void)snprintf(line, sizeof(line),
                   "sdd: vendor in=%lu out=%lu msos=%lu\r\n",
                   (unsigned long)g_sddv_vendor_in_count,
                   (unsigned long)g_sddv_vendor_out_count,
                   (unsigned long)g_sddv_msos_reads);
    uart_puts(uart_base, line);
}
