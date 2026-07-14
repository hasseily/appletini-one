/*
 * apple_cycle_egress.h -- C-side mirror of hdl/apple/apple_cycle_capture_pkg.sv
 *                        AppleCycleRecord, plus the polling drain loop that
 *                        consumes records produced by the FPGA module
 *                        apple_cycle_egress (HP0 -> DDR ring buffer).
 *
 * Bit positions, addresses, and the gap-marker convention must stay
 * bit-compatible with hdl/apple/apple_cycle_capture_pkg.sv and
 * hdl/apple/apple_cycle_egress.sv. Do not edit one side without the other.
 *
 * Lifecycle:
 *   CPU0 calls apple_cycle_egress_init() before releasing CPU1.
 *   CPU1 calls apple_cycle_egress_amp_secondary_init(), then drains records
 *   through apple_cycle_egress_poll() in its dedicated loop.
 *
 * g_main_bank[] and g_aux_bank[] mirror Apple video memory for the renderer.
 * addr_decode bit 16 selects the bank and the low 16 bits select the byte.
 */

#ifndef APPLE_CYCLE_EGRESS_H
#define APPLE_CYCLE_EGRESS_H

#include <stdint.h>

/* --- DDR allocations (must match init()) ------------------------------- */
#define ACE_PRODUCER_PTR_ADDR   0x3F000000U
#define ACE_RING_BASE           0x3F010000U
#define ACE_RING_SIZE_LOG2      16U
#define ACE_RING_SIZE_BYTES     (1U << ACE_RING_SIZE_LOG2)
#define ACE_RING_MASK           (ACE_RING_SIZE_BYTES - 1U)
#define ACE_RECORD_BYTES        8U

#define ACE_MMU_RING_SECTION    0x3F000000U   /* 1 MB section, NORM_NONCACHE */
#define ACE_MMU_SHADOW_SECTION  0x3F100000U   /* 1 MB section, cacheable     */

#define ACE_MAIN_BANK_ADDR      0x3F100000U
#define ACE_AUX_BANK_ADDR       0x3F110000U
#define ACE_BANK_BYTES          0x10000U      /* 64 KB per bank */

/* --- AxiSimple register map (mirror of apple_top.sv 8'h20..8'h2A) -------
 * Slave base 0x40000000. Word index N -> byte addr 0x40000000 + N*4. */
#define ACE_AS_BASE                 0x40000000U
#define ACE_AS_REG(n)               (ACE_AS_BASE + ((n) * 4U))

#define ACE_REG_CFG_ENABLE          ACE_AS_REG(0x20U)  /* 0x40000080 */
#define ACE_REG_CFG_RING_BASE       ACE_AS_REG(0x21U)  /* 0x40000084 */
#define ACE_REG_CFG_RING_SIZE_LOG2  ACE_AS_REG(0x22U)  /* 0x40000088 */
#define ACE_REG_CFG_PRODUCER_ADDR   ACE_AS_REG(0x23U)  /* 0x4000008C */
#define ACE_REG_CFG_CONSUMER_PTR    ACE_AS_REG(0x24U)  /* 0x40000090 */
#define ACE_REG_CFG_RESET_PULSE     ACE_AS_REG(0x25U)  /* 0x40000094 */
#define ACE_REG_STAT_PRODUCER_PTR   ACE_AS_REG(0x26U)  /* 0x40000098 */
#define ACE_REG_STAT_RECORDS        ACE_AS_REG(0x27U)  /* 0x4000009C */
#define ACE_REG_STAT_GAPS           ACE_AS_REG(0x28U)  /* 0x400000A0 */
#define ACE_REG_STAT_BURSTS         ACE_AS_REG(0x29U)  /* 0x400000A4 */
#define ACE_REG_STAT_FULL_STALL     ACE_AS_REG(0x2AU)  /* 0x400000A8 */

/* --- Drain loop tunables ----------------------------------------------- */
/* Cap records processed per poll() to bound main-loop latency. Apple bus
 * runs at ~1 MHz with frame_en=1 capture, so each main-loop iteration
 * (~30 Hz) sees ~33K new records. The 8K-entry ring fills in ~8 ms if
 * we only drain 1024 per poll, which is faster than the main-loop
 * period -- causing constant gap-marker resync churn that prevents
 * the renderer from completing any frame. Sized to cover one full
 * ring (8192) plus headroom so a single poll can always catch up. */
#define ACE_POLL_RECORD_CAP   16384U

/* --- AppleCycleRecord bit layout --------------------------------------- *
 * Mirrors apple_cycle_capture_pkg::AppleCycleRecord exactly.
 *
 *   [63:61] record_kind
 *   [60]    frame_en
 *   [59:51] line_in_frame   (9b)
 *   [50:44] cycle_in_line   (7b)
 *   [43]    sw_80store
 *   [42]    sw_ramrd
 *   [41]    sw_ramwrt
 *   [40]    sw_altzp
 *   [39]    sw_text
 *   [38]    sw_mixed
 *   [37]    sw_page2
 *   [36]    sw_hires
 *   [35]    sw_altcharset
 *   [34]    sw_80col
 *   [33]    sw_dhires
 *   [32:9]  addr_decode     (24b)
 *   [8]     addr_decode_en
 *   [7:0]   data            (8b)
 *
 * record_kind 0 is the combined frame/bus-write record above.
 * record_kind 1 is an ordered C0xx I/O write record:
 *
 *   [63:61] record_kind     (1)
 *   [60:45] io_addr         (16b)
 *   [44:37] io_data         (8b)
 *   [36:28] line_in_frame   (9b)
 *   [27:21] cycle_in_line   (7b)
 *   [20:0]  reserved
 *
 * record_kind 2 is an ordered soft-switch access record:
 *
 *   Uses the combined-record field positions for line/cycle/soft-switch bits.
 *   The low 16 bits of addr_decode carry the C0xx address touched.
 *   addr_decode_en remains 0 so it is never treated as a memory write.
 */
#define ACE_RECORD_KIND_LEGACY          0U
#define ACE_RECORD_KIND_IO_WRITE        1U
#define ACE_RECORD_KIND_SOFTSW_ACCESS   2U

#define ACE_BIT_RECORD_KIND_LO    61
#define ACE_BIT_IO_ADDR_LO        45
#define ACE_BIT_IO_DATA_LO        37
#define ACE_BIT_IO_LINE_LO        28
#define ACE_BIT_IO_CYCLE_LO       21

#define ACE_BIT_FRAME_EN          60
#define ACE_BIT_LINE_IN_FRAME_LO  51
#define ACE_BIT_CYCLE_IN_LINE_LO  44
#define ACE_BIT_SW_80STORE        43
#define ACE_BIT_SW_RAMRD          42
#define ACE_BIT_SW_RAMWRT         41
#define ACE_BIT_SW_ALTZP          40
#define ACE_BIT_SW_TEXT           39
#define ACE_BIT_SW_MIXED          38
#define ACE_BIT_SW_PAGE2          37
#define ACE_BIT_SW_HIRES          36
#define ACE_BIT_SW_ALTCHARSET     35
#define ACE_BIT_SW_80COL          34
#define ACE_BIT_SW_DHIRES         33
#define ACE_BIT_ADDR_DECODE_LO    9
#define ACE_BIT_ADDR_DECODE_EN    8
#define ACE_BIT_DATA_LO           0

/* --- Inline accessors -------------------------------------------------- */
static inline uint32_t ace_record_kind(uint64_t r) {
    return (uint32_t)((r >> ACE_BIT_RECORD_KIND_LO) & 0x7U);
}

static inline uint16_t ace_io_addr(uint64_t r) {
    return (uint16_t)((r >> ACE_BIT_IO_ADDR_LO) & 0xFFFFU);
}

static inline uint8_t ace_io_data(uint64_t r) {
    return (uint8_t)((r >> ACE_BIT_IO_DATA_LO) & 0xFFU);
}

static inline uint32_t ace_io_line_in_frame(uint64_t r) {
    return (uint32_t)((r >> ACE_BIT_IO_LINE_LO) & 0x1FFU);
}

static inline uint32_t ace_io_cycle_in_line(uint64_t r) {
    return (uint32_t)((r >> ACE_BIT_IO_CYCLE_LO) & 0x7FU);
}

/* Bus-write half. */
static inline int ace_addr_decode_en(uint64_t r) {
    return (int)((r >> ACE_BIT_ADDR_DECODE_EN) & 1U);
}
static inline uint32_t ace_addr_decode(uint64_t r) {
    return (uint32_t)((r >> ACE_BIT_ADDR_DECODE_LO) & 0xFFFFFFU);
}
static inline uint16_t ace_softswitch_access_addr(uint64_t r) {
    return (uint16_t)(ace_addr_decode(r) & 0xFFFFU);
}
static inline uint8_t ace_data(uint64_t r) {
    return (uint8_t)(r & 0xFFU);
}

/* Frame half. */
static inline int ace_frame_en(uint64_t r) {
    return (int)((r >> ACE_BIT_FRAME_EN) & 1U);
}
static inline uint32_t ace_line_in_frame(uint64_t r) {
    return (uint32_t)((r >> ACE_BIT_LINE_IN_FRAME_LO) & 0x1FFU);
}
static inline uint32_t ace_cycle_in_line(uint64_t r) {
    return (uint32_t)((r >> ACE_BIT_CYCLE_IN_LINE_LO) & 0x7FU);
}

/* All soft-switch bits as a single 11-bit value. Bit 0 = sw_dhires (lowest
 * SW bit position in the record), bit 10 = sw_80store (highest). */
static inline uint32_t ace_softswitch_bits(uint64_t r) {
    return (uint32_t)((r >> ACE_BIT_SW_DHIRES) & 0x7FFU);
}

/* Per-bit accessors (raw record). */
static inline int ace_sw_80store   (uint64_t r) { return (int)((r >> ACE_BIT_SW_80STORE   ) & 1U); }
static inline int ace_sw_ramrd     (uint64_t r) { return (int)((r >> ACE_BIT_SW_RAMRD     ) & 1U); }
static inline int ace_sw_ramwrt    (uint64_t r) { return (int)((r >> ACE_BIT_SW_RAMWRT    ) & 1U); }
static inline int ace_sw_altzp     (uint64_t r) { return (int)((r >> ACE_BIT_SW_ALTZP     ) & 1U); }
static inline int ace_sw_text      (uint64_t r) { return (int)((r >> ACE_BIT_SW_TEXT      ) & 1U); }
static inline int ace_sw_mixed     (uint64_t r) { return (int)((r >> ACE_BIT_SW_MIXED     ) & 1U); }
static inline int ace_sw_page2     (uint64_t r) { return (int)((r >> ACE_BIT_SW_PAGE2     ) & 1U); }
static inline int ace_sw_hires     (uint64_t r) { return (int)((r >> ACE_BIT_SW_HIRES     ) & 1U); }
static inline int ace_sw_altcharset(uint64_t r) { return (int)((r >> ACE_BIT_SW_ALTCHARSET) & 1U); }
static inline int ace_sw_80col     (uint64_t r) { return (int)((r >> ACE_BIT_SW_80COL     ) & 1U); }
static inline int ace_sw_dhires    (uint64_t r) { return (int)((r >> ACE_BIT_SW_DHIRES    ) & 1U); }

/* Per-bit accessors on the packed 11-bit softswitch_bits word. Bit
 * positions inside that word match the full-record positions shifted
 * by ACE_BIT_SW_DHIRES (=33). So sw_dhires is bit 0, sw_80store bit 10. */
#define ACE_SWB_DHIRES_BIT      0U
#define ACE_SWB_80COL_BIT       1U
#define ACE_SWB_ALTCHARSET_BIT  2U
#define ACE_SWB_HIRES_BIT       3U
#define ACE_SWB_PAGE2_BIT       4U
#define ACE_SWB_MIXED_BIT       5U
#define ACE_SWB_TEXT_BIT        6U
#define ACE_SWB_ALTZP_BIT       7U
#define ACE_SWB_RAMWRT_BIT      8U
#define ACE_SWB_RAMRD_BIT       9U
#define ACE_SWB_80STORE_BIT     10U

/* --- Public API -------------------------------------------------------- */
int  apple_cycle_egress_init(void);
void apple_cycle_egress_poll(void);

/* AMP secondary-core MMU setup. Marks the ring + producer-pointer
 * region non-cacheable on the calling core's MMU table so reads of
 * the producer pointer hit DDR rather than a stale L1 line. CPU1
 * must call this once before its first apple_cycle_egress_poll();
 * CPU0's apple_cycle_egress_init() already does the equivalent on
 * its own MMU. Does NOT touch any PL registers, so safe to call
 * after CPU0's init has finished. */
void apple_cycle_egress_amp_secondary_init(void);

/* --- Renderer hook ----------------------------------------------------- *
 * Called once per consumed record, in order. The weak declaration lets
 * egress-only diagnostic builds omit the renderer. */
__attribute__((weak)) void apple_cycle_renderer_on_record(uint64_t rec);

/* --- Public state ------------------------------------------------------ */
extern volatile uint8_t  *const g_main_bank;       /* 64 KB at 0x3F100000 */
extern volatile uint8_t  *const g_aux_bank;        /* 64 KB at 0x3F110000 */
extern volatile uint32_t g_resync_pending;

/* --- Diagnostic counters ----------------------------------------------- */
extern volatile uint32_t g_records_processed;
extern volatile uint32_t g_gap_markers_seen;
extern volatile uint32_t g_bus_writes_seen;
extern volatile uint32_t g_frame_records_seen;
extern volatile uint32_t g_poll_calls;
extern volatile uint32_t g_oversized_drains;

#endif /* APPLE_CYCLE_EGRESS_H */
