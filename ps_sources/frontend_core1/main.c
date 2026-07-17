/*
 * frontend_core1/main.c -- CPU1 firmware (AMP).
 *
 * Runs the apple_cycle_egress poll + apple_cycle_renderer
 * dispatch loop. CPU0 (frontend) handles UI, compositor, smartport,
 * UART input, etc. -- everything that is latency-sensitive to host
 * input. CPU1 owns the Apple-bus stream end-to-end:
 *
 *   1. Drains records from the PL cycle ring (DDR @ 0x3F010000) via
 *      apple_cycle_egress_poll().
 *   2. Dispatches per-cycle records into apple_cycle_renderer_on_record
 *      which walks the NTSC core's per-cycle scan state.
 *   3. On each clean Apple frame edge, publishes the just-rendered
 *      560x192 BGRA32 slot via apple_fb_writer_publish() (atomic
 *      handoff in apple_fb_handoff.c). CPU0's compositor claims
 *      the latest published slot at its own rate.
 *
 * Sharing rules:
 *   - CPU0 writes the egress config registers (cfg_enable, ring
 *     base, etc.) once during boot via apple_cycle_egress_init().
 *     Then ownership transfers: CPU1 polls. CPU1 must NOT call
 *     apple_cycle_egress_init() again -- it would zero CPU0's
 *     work (the producer pointer slot).
 *   - The shadow banks (g_main_bank/g_aux_bank) and the FB slots
 *     are written by CPU1, read by CPU0 (compositor). Coherency:
 *     CPU1 dcache-flushes the slot before the atomic publish
 *     (already done in apple_cycle_renderer.c on_frame_end).
 *     CPU0's MMU has the same slot regions as cacheable so its
 *     read pulls from L1; we rely on the SCU + dsb-after-flush
 *     to make CPU1's writes visible. If we see staleness, we'll
 *     either invalidate on CPU0's side or mark the slot region
 *     non-cacheable on both.
 */

#include <stdint.h>

#include "xiltimer.h"     /* COUNTS_PER_SECOND */

#include "../lib/common.h"
#include "../lib/uart.h"

#include "../frontend/apple_cycle_egress.h"
#include "../frontend/apple_cycle_renderer.h"
#include "../frontend/apple_fb_handoff.h"
#include "../frontend/apple_pal_video_timing.h"

#define RESET_RELEASE_REG            0x4000000CU
#define RESET_RELEASE_CPU1_READY_BIT (1UL << 1)
#define APPLE_RESET_STATUS_REG       0x40000024U
#define APPLE_RESET_SEQ_MASK         0x000000FFUL

/* ---- Live CPU1 health line (UART0, TX shared with CPU0) ----------------
 * Emits one line per ~second carrying per-interval deltas, so the PAL
 * renderer's effect on the capture path is visible while testing. On a
 * renderer that keeps up, steady state is:
 *
 *   gap=0  ovr=0  resync=0   frames ~= 50 (PAL) / 60 (NTSC)
 *
 * The PAL "render too slow on CPU1" failure mode shows up as gap>0 and/or
 * ovr>0 (the on-chip capture FIFO overflowed and the egress emitted gap
 * markers), resync climbing (the renderer dropping to the next frame edge),
 * and frames collapsing well below the line rate. Fields:
 *
 *   dt_ms   measured interval length (sanity)
 *   frames  g_acr_frames_complete  delta  -- completed frames / interval
 *   gap     g_gap_markers_seen     delta  -- FIFO/ring overflow markers
 *   ovr     g_oversized_drains     delta  -- drain hit ACE_POLL_RECORD_CAP
 *   resync  g_acr_resyncs_cleared  delta  -- renderer resync events
 *   lastrec g_acr_last_frame_records       -- records in last completed frame
 *   raw     g_acr_frame_edges_seen delta   -- incoming Apple frame edges seen
 *   recpf   recs/raw delta average         -- records per incoming frame
 *   recs    g_records_processed    delta  -- Apple-bus record throughput
 *   polls   g_poll_calls           delta  -- egress poll iterations
 *   palok   g_pal_frames_published delta  -- PAL frames completed/published
 *   paldrop g_pal_frames_dropped   delta  -- PAL frames skipped/dropped whole
 *   paltry  palok+paldrop delta            -- PAL accepted+skipped frame attempts
 *   palln   g_pal_lines_rendered   delta  -- PAL lines rendered
 *   palbad  g_pal_lines_incomplete delta  -- incomplete captured PAL lines
 *   palovr  g_pal_queue_overflows  delta  -- PAL render queue overflows
 *   palqmax g_pal_queue_max               -- high-water queue depth
 *   palfast g_pal_fast_lines delta         -- constant-soft-switch fast lines
 *   palslow g_pal_slow_lines delta         -- delayed-transition fallback lines
 *   palus   avg render time per PAL line   -- from ARM global timer
 *   palmax  worst render time per PAL line -- lifetime max, usec
 *   palend  avg queue depth at end-frame   -- before bounded frame flush
 *   palendm worst queue depth at end-frame -- lifetime max
 *   palfl   lines drained at end-frame     -- bounded frame flush work
 *
 * Set to 1 for profiling builds. Leave 0 for normal firmware; this compiles
 * out the timer reads, counter snapshots, decimal formatting, and UART writes. */
#define CPU1_STATUS_UART 0

/* DIAGNOSTIC BUILD OVERRIDE (diag/transwarp-video-freeze): force the health
 * line on, plus a bw= field (video-shadow write records per interval) to
 * timestamp when TransWarp write-through cycles reach the shadow banks.
 * Delete this block when the investigation ends. */
#undef CPU1_STATUS_UART
#define CPU1_STATUS_UART 1

static uint8_t apple_reset_seq_read(void)
{
    return (uint8_t)(REG_READ(APPLE_RESET_STATUS_REG) & APPLE_RESET_SEQ_MASK);
}

/* ---- Diagnostic: text-page shadow dump (diag/transwarp-video-freeze) ----
 * Prints the 40x24 text page 1 ($0400-$07FF) from the requested shadow
 * bank, decoded to readable ASCII, plus the capture counters at dump
 * time. Runs on CPU1 because the shadow banks are cacheable and written
 * through THIS core's L1 -- only CPU1's view is guaranteed current.
 * Triggered from CPU0's UART console (":shadow [main|aux]") via the
 * strongly-ordered request word in apple_fb_handoff.
 *
 * Emitted one line per main-loop pass: a full-dump blocking print
 * (~13 ms at 921600 baud) would overflow the 8 ms cycle ring and inject
 * the very gap markers this build exists to observe. One line (~0.5 ms)
 * keeps the ring backlog under ~600 records between egress polls. */
static uint32_t s_dump_req  = APPLE_FB_DUMP_NONE;
static uint32_t s_dump_row  = 0u;

static void cpu1_dump_text_shadow_step(void)
{
    const volatile uint8_t *bank;
    uint32_t base;
    uint32_t col;

    if (s_dump_req == APPLE_FB_DUMP_NONE) {
        s_dump_req = apple_fb_debug_dump_take();
        if (s_dump_req == APPLE_FB_DUMP_NONE) {
            return;
        }
        /* First pass: header with the counters at request time. */
        s_dump_row = 0u;
        uart_puts(UART0_BASE, "\r\n[cpu1] shadow dump: ");
        uart_puts(UART0_BASE,
                  (s_dump_req == APPLE_FB_DUMP_TEXT_AUX) ? "AUX" : "MAIN");
        uart_puts(UART0_BASE, " $0400-$07FF  bus_writes=");
        uart_putdec(UART0_BASE, g_bus_writes_seen);
        uart_puts(UART0_BASE, " recs=");
        uart_putdec(UART0_BASE, g_records_processed);
        uart_puts(UART0_BASE, " gaps=");
        uart_putdec(UART0_BASE, g_gap_markers_seen);
        uart_puts(UART0_BASE, " frames=");
        uart_putdec(UART0_BASE, g_acr_frames_complete);
        uart_puts(UART0_BASE, "\r\n");
        return;
    }

    if (s_dump_row >= 24u) {
        uart_puts(UART0_BASE, "[cpu1] shadow dump end\r\n");
        s_dump_req = APPLE_FB_DUMP_NONE;
        return;
    }

    bank = (s_dump_req == APPLE_FB_DUMP_TEXT_AUX) ? g_aux_bank : g_main_bank;
    /* Standard interleaved text page 1 row base. */
    base = 0x0400u + ((s_dump_row & 7u) << 7) + ((s_dump_row >> 3) * 0x28u);

    uart_putdec(UART0_BASE, s_dump_row);
    uart_puts(UART0_BASE, (s_dump_row < 10u) ? "  |" : " |");
    for (col = 0u; col < 40u; col++) {
        /* Screen code -> printable: strip the high bit, remap the
         * inverse/flash control rows into the letter range. */
        uint8_t v = (uint8_t)(bank[base + col] & 0x7Fu);

        if (v < 0x20u) {
            v = (uint8_t)(v + 0x40u);
        }
        uart_putc(UART0_BASE, (char)v);
    }
    uart_puts(UART0_BASE, "|\r\n");
    s_dump_row++;
}

#if CPU1_STATUS_UART
/* Zynq-7000 ARM global timer (SCU @ 0xF8F00200): a free-running 64-bit counter
 * ticking at CPU_freq/2 == COUNTS_PER_SECOND. It is shared by both cores and
 * started at boot; we only ever READ it, so CPU0's timebase is untouched.
 * Reading the registers directly (rather than XTime_GetTime) keeps this
 * secondary-core diagnostic free of any XilTimer runtime-init assumption on
 * CPU1. */
#define GLOBAL_TMR_COUNT_LO 0xF8F00200U
#define GLOBAL_TMR_COUNT_HI 0xF8F00204U

static uint64_t global_timer_read(void)
{
    uint32_t hi;
    uint32_t lo;
    uint32_t hi_again;

    do {
        hi       = REG_READ(GLOBAL_TMR_COUNT_HI);
        lo       = REG_READ(GLOBAL_TMR_COUNT_LO);
        hi_again = REG_READ(GLOBAL_TMR_COUNT_HI);
    } while (hi != hi_again);

    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static uint64_t cpu1_counts_per_second(void)
{
    /* COUNTS_PER_SECOND expands from the BSP as XPAR_CPU_CORE_CLOCK_FREQ_HZ/2,
     * without protective parentheses. Store it once before using it as a
     * denominator, otherwise `/ (uint64_t)COUNTS_PER_SECOND` is parsed as
     * `/ XPAR_CPU_CORE_CLOCK_FREQ_HZ / 2` and reports one quarter of the real
     * elapsed time. */
    return (uint64_t)COUNTS_PER_SECOND;
}

static void cpu1_emit_status(uint32_t dt_ms)
{
    static uint32_t p_frames, p_gap, p_ovr, p_resync, p_raw, p_recs, p_polls;
    static uint32_t p_bus_writes;
    static uint32_t p_pal_ok, p_pal_drop, p_pal_lines, p_pal_bad, p_pal_ovr;
    static uint32_t p_pal_fast, p_pal_slow, p_pal_ticks;
    static uint32_t p_pal_end_count, p_pal_end_queue, p_pal_end_drained;
    const uint32_t frames = g_acr_frames_complete;
    const uint32_t gap    = g_gap_markers_seen;
    const uint32_t ovr    = g_oversized_drains;
    const uint32_t resync = g_acr_resyncs_cleared;
    const uint32_t raw    = g_acr_frame_edges_seen;
    const uint32_t recs   = g_records_processed;
    const uint32_t polls  = g_poll_calls;
    const uint32_t bus_writes = g_bus_writes_seen;
    const uint32_t pal_ok = g_pal_frames_published;
    const uint32_t pal_drop = g_pal_frames_dropped;
    const uint32_t pal_lines = g_pal_lines_rendered;
    const uint32_t pal_bad = g_pal_lines_incomplete;
    const uint32_t pal_ovr = g_pal_queue_overflows;
    const uint32_t pal_fast = g_pal_fast_lines;
    const uint32_t pal_slow = g_pal_slow_lines;
    const uint32_t pal_ticks = g_pal_render_ticks_total;
    const uint32_t pal_end_count = g_pal_end_frame_count;
    const uint32_t pal_end_queue = g_pal_end_queue_total;
    const uint32_t pal_end_drained = g_pal_end_lines_drained;
    const uint32_t raw_delta = raw - p_raw;
    const uint32_t recs_delta = recs - p_recs;
    const uint32_t pal_ok_delta = pal_ok - p_pal_ok;
    const uint32_t pal_drop_delta = pal_drop - p_pal_drop;
    const uint32_t pal_lines_delta = pal_lines - p_pal_lines;
    const uint32_t pal_ticks_delta = pal_ticks - p_pal_ticks;
    const uint32_t pal_end_count_delta = pal_end_count - p_pal_end_count;
    const uint32_t pal_end_queue_delta = pal_end_queue - p_pal_end_queue;
    const uint64_t ticks_per_second = cpu1_counts_per_second();
    const uint32_t recs_per_frame =
        (raw_delta != 0u) ?
        ((recs_delta + (raw_delta / 2u)) / raw_delta) : 0u;
    const uint32_t pal_avg_us =
        (pal_lines_delta != 0u && ticks_per_second != 0u) ?
        (uint32_t)(((uint64_t)pal_ticks_delta * 1000000ULL) /
                   ticks_per_second /
                   (uint64_t)pal_lines_delta) : 0u;
    const uint32_t pal_max_us =
        (ticks_per_second != 0u) ?
        (uint32_t)(((uint64_t)g_pal_render_ticks_max * 1000000ULL) /
                   ticks_per_second) : 0u;
    const uint32_t pal_end_avg =
        (pal_end_count_delta != 0u) ?
        ((pal_end_queue_delta + (pal_end_count_delta / 2u)) /
         pal_end_count_delta) : 0u;

    uart_puts(UART0_BASE, "[cpu1] dt_ms=");  uart_putdec(UART0_BASE, dt_ms);
    uart_puts(UART0_BASE, " frames=");       uart_putdec(UART0_BASE, frames - p_frames);
    uart_puts(UART0_BASE, " gap=");          uart_putdec(UART0_BASE, gap - p_gap);
    uart_puts(UART0_BASE, " ovr=");          uart_putdec(UART0_BASE, ovr - p_ovr);
    uart_puts(UART0_BASE, " resync=");       uart_putdec(UART0_BASE, resync - p_resync);
    uart_puts(UART0_BASE, " lastrec=");      uart_putdec(UART0_BASE, g_acr_last_frame_records);
    uart_puts(UART0_BASE, " raw=");          uart_putdec(UART0_BASE, raw_delta);
    uart_puts(UART0_BASE, " recpf=");        uart_putdec(UART0_BASE, recs_per_frame);
    uart_puts(UART0_BASE, " recs=");         uart_putdec(UART0_BASE, recs_delta);
    uart_puts(UART0_BASE, " bw=");           uart_putdec(UART0_BASE, bus_writes - p_bus_writes);
    uart_puts(UART0_BASE, " polls=");        uart_putdec(UART0_BASE, polls - p_polls);
    uart_puts(UART0_BASE, " palok=");        uart_putdec(UART0_BASE, pal_ok_delta);
    uart_puts(UART0_BASE, " paldrop=");      uart_putdec(UART0_BASE, pal_drop_delta);
    uart_puts(UART0_BASE, " paltry=");       uart_putdec(UART0_BASE, pal_ok_delta + pal_drop_delta);
    uart_puts(UART0_BASE, " palln=");        uart_putdec(UART0_BASE, pal_lines_delta);
    uart_puts(UART0_BASE, " palbad=");       uart_putdec(UART0_BASE, pal_bad - p_pal_bad);
    uart_puts(UART0_BASE, " palovr=");       uart_putdec(UART0_BASE, pal_ovr - p_pal_ovr);
    uart_puts(UART0_BASE, " palqmax=");      uart_putdec(UART0_BASE, g_pal_queue_max);
    uart_puts(UART0_BASE, " palfast=");      uart_putdec(UART0_BASE, pal_fast - p_pal_fast);
    uart_puts(UART0_BASE, " palslow=");      uart_putdec(UART0_BASE, pal_slow - p_pal_slow);
    uart_puts(UART0_BASE, " palus=");        uart_putdec(UART0_BASE, pal_avg_us);
    uart_puts(UART0_BASE, " palmax=");       uart_putdec(UART0_BASE, pal_max_us);
    uart_puts(UART0_BASE, " palend=");       uart_putdec(UART0_BASE, pal_end_avg);
    uart_puts(UART0_BASE, " palendm=");      uart_putdec(UART0_BASE, g_pal_end_queue_max);
    uart_puts(UART0_BASE, " palfl=");        uart_putdec(UART0_BASE, pal_end_drained - p_pal_end_drained);
    uart_puts(UART0_BASE, "\r\n");

    p_frames = frames; p_gap = gap; p_ovr = ovr;
    p_resync = resync; p_raw = raw; p_recs = recs; p_polls = polls;
    p_bus_writes = bus_writes;
    p_pal_ok = pal_ok; p_pal_drop = pal_drop; p_pal_lines = pal_lines;
    p_pal_bad = pal_bad; p_pal_ovr = pal_ovr;
    p_pal_fast = pal_fast; p_pal_slow = pal_slow; p_pal_ticks = pal_ticks;
    p_pal_end_count = pal_end_count;
    p_pal_end_queue = pal_end_queue;
    p_pal_end_drained = pal_end_drained;
}
#endif

int main(void)
{
    uint8_t reset_seq_last;

    /* CPU0 owns UART0 init (921600 baud). We just write to the TX
     * FIFO; do NOT re-run uart_init_baud or we will glitch CPU0's
     * in-flight output. */

    /* Mark the cycle ring + producer-pointer region non-cacheable
     * on CPU1's MMU table. CPU0's apple_cycle_egress_init already
     * did this on its own MMU; CPU1 needs to do it independently
     * or its reads of the producer pointer slot will hit a stale
     * L1 line and never see the PL's writes. */
    apple_cycle_egress_amp_secondary_init();

    /* Initialise the renderer on this core. The egress was already
     * initialised by CPU0 (the PL config registers are write-once
     * at boot), so we do not call apple_cycle_egress_init() here. */
    if (apple_cycle_renderer_init() != 0) {
        uart_puts(UART0_BASE, "core 1: apple_cycle_renderer_init failed\r\n");
        for (;;) { __asm__ volatile ("wfe"); }
    }

    REG_WRITE(RESET_RELEASE_REG,
              RESET_RELEASE_CPU1_READY_BIT);
    reset_seq_last = apple_reset_seq_read();

    /* Tight poll loop. No CPU0 work fights us for the cycles, so
     * we burn the whole core on egress drain + renderer dispatch.
     * The renderer's cost scales with Apple bus rate (~1 MHz);
     * drain cost is dominated by the per-record dispatch cycle
     * count, well within a 666 MHz A9's budget. */
#if CPU1_STATUS_UART
    /* The once-per-loop timer read is cheap relative to a drain pass, and the
     * whole block compiles out with CPU1_STATUS_UART=0. */
    uint64_t status_last = global_timer_read();
    uart_puts(UART0_BASE,
        "[cpu1] health line armed: per-1s deltas; gap/ovr/resync should stay 0,"
        " frames ~= line rate\r\n");
#endif
    for (;;) {
        uint8_t reset_seq = apple_reset_seq_read();
        if (reset_seq != reset_seq_last) {
            reset_seq_last = reset_seq;
            apple_cycle_renderer_reset_local_video_state();
        }
        apple_cycle_egress_poll();

        /* Diagnostic shadow-dump request from CPU0's UART console.
         * Costs one strongly-ordered OCM read per pass when idle;
         * emits at most one dump line per pass when active. */
        cpu1_dump_text_shadow_step();

        /* Render queued PAL lines off the drain path. The drain (above) only
         * buffers; this does the heavy per-line work, bounded per call so the
         * loop returns to drain the capture ring before it can back up. No-op
         * in every non-PAL-accurate mode. */
        apple_pal_video_pump();

#if CPU1_STATUS_UART
        const uint64_t ticks_per_second = cpu1_counts_per_second();
        if (ticks_per_second != 0U) {
            const uint64_t status_now = global_timer_read();
            if ((status_now - status_last) >= ticks_per_second) {
                const uint32_t dt_ms = (uint32_t)(((status_now - status_last) *
                                                   1000ULL) /
                                                  ticks_per_second);
                cpu1_emit_status(dt_ms);
                status_last = status_now;
            }
        }
#endif
    }
    return 0;
}
