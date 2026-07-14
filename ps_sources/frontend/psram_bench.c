#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "xil_mmu.h"
#include "xiltimer.h"

#include "../lib/framebuffer.h"
#include "../lib/common.h"
#include "psram_bench.h"

#define REG_PSRAM_CTRL    (FB_CONTROL_BASE + 0x38U)
#define REG_PSRAM_ADDR    (FB_CONTROL_BASE + 0x3CU)
#define REG_PSRAM_WDATA   (FB_CONTROL_BASE + 0x40U)
#define REG_PSRAM_RDATA   (FB_CONTROL_BASE + 0x44U)
#define REG_PSRAM_STATUS  (FB_CONTROL_BASE + 0x48U)

#define PSRAM_CTRL_START      (1U << 0)
#define PSRAM_CTRL_OP_SHIFT   1U
#define PSRAM_CTRL_DIV_SHIFT  8U

#define PSRAM_OP_READ      1U
#define PSRAM_OP_WRITE     2U
#define PSRAM_OP_CMD       3U
#define PSRAM_TIMEOUT_CYCLES      5000000U
#define PSRAM_DEFAULT_BENCH_DIV   1U
#define PSRAM_SAFE_XFER_DIV      64U

static uint8_t g_psram_tx_buf[PSRAM_COPY_BYTES] __attribute__((aligned(32)));
static uint8_t g_psram_rx_buf[PSRAM_COPY_BYTES] __attribute__((aligned(32)));

static void psram_set_window_clk_div(uint8_t clk_div)
{
    uint32_t ctrl = REG_READ(REG_PSRAM_CTRL);
    ctrl &= ~(0xFFU << PSRAM_CTRL_DIV_SHIFT);
    ctrl |= ((uint32_t)clk_div << PSRAM_CTRL_DIV_SHIFT);
    ctrl &= ~PSRAM_CTRL_START;
    REG_WRITE(REG_PSRAM_CTRL, ctrl);
}

static uint32_t psram_mibps_x100(uint32_t bytes, XTime ticks)
{
    uint64_t bps;
    if (ticks == 0U || COUNTS_PER_SECOND == 0U) {
        return 0U;
    }
    bps = ((uint64_t)bytes * (uint64_t)COUNTS_PER_SECOND) / (uint64_t)ticks;
    return (uint32_t)((bps * 100ULL) / (1024ULL * 1024ULL));
}

static void psram_map_window(uintptr_t base, uint32_t size)
{
#if defined(DEVICE_MEMORY)
    const uint32_t attr = DEVICE_MEMORY;
#elif defined(STRONG_ORDERED)
    const uint32_t attr = STRONG_ORDERED;
#else
    const uint32_t attr = NORM_NONCACHE;
#endif
    const uint32_t section = 0x00100000U;
    const uint32_t bytes = (size + section - 1U) & ~(section - 1U);
    uint32_t off;

    for (off = 0U; off < bytes; off += section) {
        Xil_SetTlbAttributes(base + off, attr);
    }
}

static void psram_map_mmio(uintptr_t base, uint32_t size)
{
#if defined(DEVICE_MEMORY)
    const uint32_t attr = DEVICE_MEMORY;
#elif defined(STRONG_ORDERED)
    const uint32_t attr = STRONG_ORDERED;
#else
    const uint32_t attr = NORM_NONCACHE;
#endif
    const uint32_t section = 0x00100000U;
    const uint32_t bytes = (size + section - 1U) & ~(section - 1U);
    uint32_t off;

    for (off = 0U; off < bytes; off += section) {
        Xil_SetTlbAttributes(base + off, attr);
    }
}

static int psram_xfer(uint32_t op, uint32_t addr, uint32_t wdata,
                      uint8_t clk_div, uint32_t *rdata, uint32_t *status)
{
    return -1;
    const uint32_t ctrl = ((uint32_t)clk_div << PSRAM_CTRL_DIV_SHIFT) |
                          ((op & 0x3U) << PSRAM_CTRL_OP_SHIFT);
    uint32_t t = 0U;

    REG_WRITE(REG_PSRAM_ADDR, addr);
    REG_WRITE(REG_PSRAM_WDATA, wdata);
    REG_WRITE(REG_PSRAM_CTRL, ctrl);
    REG_WRITE(REG_PSRAM_CTRL, ctrl | PSRAM_CTRL_START);
    REG_WRITE(REG_PSRAM_CTRL, ctrl);

    {
        uint32_t done_low_seen = 0U;
        while (t++ < PSRAM_TIMEOUT_CYCLES) {
            uint32_t st = REG_READ(REG_PSRAM_STATUS);
            if ((st & 0x2U) == 0U) {
                done_low_seen = 1U;
            }
            if (done_low_seen && ((st & 0x2U) != 0U)) {
                if (rdata != NULL) {
                    *rdata = REG_READ(REG_PSRAM_RDATA);
                }
                if (status != NULL) {
                    *status = st;
                }
                return ((st & 0x4U) == 0U) ? 0 : -2;
            }
        }
    }

    if (status != NULL) {
        *status = REG_READ(REG_PSRAM_STATUS);
    }
    return -1;
}

static void psram_mode_recover(void)
{
    return;
    uint32_t st_dummy = 0U;
    (void)psram_xfer(PSRAM_OP_CMD, 0x00000066U, 0U, PSRAM_SAFE_XFER_DIV, NULL, &st_dummy);
    (void)psram_xfer(PSRAM_OP_CMD, 0x00000099U, 0U, PSRAM_SAFE_XFER_DIV, NULL, &st_dummy);
}

static int psram_selftest(psram_ui_state_t *p, uint8_t clk_div)
{
    return -1;
    uint32_t r = 0U;
    const uint32_t w = 0x5AA5C33CU;
    if (psram_xfer(PSRAM_OP_WRITE, 0x00000020U, w, clk_div, NULL, &p->last_status) != 0) {
        return -1;
    }
    if (psram_xfer(PSRAM_OP_READ, 0x00000020U, 0U, clk_div, &r, &p->last_status) != 0) {
        return -1;
    }
    if (r != w) {
        snprintf(p->msg, sizeof(p->msg), "selftest mismatch 0x%08lX", (unsigned long)r);
        return -1;
    }
    return 0;
}

static void psram_tune_bench_div(psram_ui_state_t *p)
{
    return;
    static const uint8_t candidates[] = {0U, 1U, 2U, 4U, 8U};
    uint32_t i;

    p->clk_div = PSRAM_DEFAULT_BENCH_DIV;
    p->clk_div_tuned = 0U;
    for (i = 0U; i < (uint32_t)(sizeof(candidates) / sizeof(candidates[0])); i++) {
        if (psram_selftest(p, candidates[i]) == 0) {
            p->clk_div = candidates[i];
            p->clk_div_tuned = 1U;
            break;
        }
    }
    psram_set_window_clk_div(p->clk_div);
}

static int psram_test_window_copy_memcpy(uint32_t win_off, uint32_t seed, uint32_t *mismatch_count,
                                         XTime *write_ticks, XTime *read_ticks, XTime *cmp_ticks,
                                         XTime *total_ticks)
{
    return -1;
    volatile uint8_t *win = (volatile uint8_t *)(uintptr_t)(PSRAM_WIN_BASE + win_off);
    uint32_t i;
    int cmp_rc;
    XTime t0 = 0U, t1 = 0U, t2 = 0U, t3 = 0U;

    for (i = 0U; i < PSRAM_COPY_BYTES; i++) {
        g_psram_tx_buf[i] = (uint8_t)((seed + i * 13U) ^ (i >> 2U));
    }
    memset(g_psram_rx_buf, 0, PSRAM_COPY_BYTES);

    XTime_GetTime(&t0);
    memcpy((void *)(uintptr_t)win, g_psram_tx_buf, PSRAM_COPY_BYTES);
    XTime_GetTime(&t1);
    memcpy(g_psram_rx_buf, (const void *)(uintptr_t)win, PSRAM_COPY_BYTES);
    XTime_GetTime(&t2);

    cmp_rc = memcmp(g_psram_rx_buf, g_psram_tx_buf, PSRAM_COPY_BYTES);
    *mismatch_count = 0U;
    if (cmp_rc != 0) {
        for (i = 0U; i < PSRAM_COPY_BYTES; i++) {
            if (g_psram_rx_buf[i] != g_psram_tx_buf[i]) {
                (*mismatch_count)++;
            }
        }
    }
    XTime_GetTime(&t3);

    if (write_ticks != NULL) *write_ticks = t1 - t0;
    if (read_ticks  != NULL) *read_ticks  = t2 - t1;
    if (cmp_ticks   != NULL) *cmp_ticks   = t3 - t2;
    if (total_ticks != NULL) *total_ticks = t3 - t0;

    return (*mismatch_count == 0U) ? 0 : -1;
}

void psram_bench_runtime_map(void)
{
    return;
    psram_map_window(PSRAM_WIN_BASE, PSRAM_WIN_SIZE);
    psram_map_mmio(FB_CONTROL_BASE, FB_CONTROL_SIZE);
}

void psram_bench_init_defaults(psram_ui_state_t *p)
{
    return;
    if (p == NULL) {
        return;
    }
    memset(p, 0, sizeof(*p));
    p->running = 1U;
    p->stress_mode = 0U;
    p->use_memcpy = 1U;
    p->clk_div = PSRAM_DEFAULT_BENCH_DIV;
    p->clk_div_tuned = 0U;
    psram_set_window_clk_div(p->clk_div);
}

void psram_bench_reset_counters(psram_ui_state_t *p)
{
    return;
    if (p == NULL) {
        return;
    }
    p->loop = 0U;
    p->ok_count = 0U;
    p->fail_count = 0U;
    p->mismatch_count = 0U;
    p->stress_off = 0U;
    p->stress_passes = 0U;
    p->last_copy_ticks = 0U;
    p->last_write_ticks = 0U;
    p->last_read_ticks = 0U;
    p->last_cmp_ticks = 0U;
    p->last_rw_mibps_x100 = 0U;
    p->last_loop_mibps_x100 = 0U;
    p->last_rw_sec_per_mib_x100 = 0U;
    p->last_loop_sec_per_mib_x100 = 0U;
    p->avg_rw_mibps_x100 = 0U;
    p->avg_rw_sec_per_mib_x100 = 0U;
    p->avg_loop_sec_per_mib_x100 = 0U;
    p->bench_total_ticks = 0U;
    p->bench_total_rw_ticks = 0U;
    p->bench_total_rw_bytes = 0U;
    p->bench_total_loop_bytes = 0U;
}

int psram_bench_startup(psram_ui_state_t *p)
{
    return 0;
    if (p == NULL) {
        return -1;
    }

    psram_mode_recover();
    if (psram_selftest(p, PSRAM_SAFE_XFER_DIV) == 0) {
        psram_tune_bench_div(p);
        p->mapped = 1U;
        snprintf(p->msg, sizeof(p->msg), "virtual x8 selftest pass (div=%u)",
                 (unsigned)p->clk_div);
        return 0;
    }

    p->mapped = 0U;
    if (p->msg[0] == '\0') {
        snprintf(p->msg, sizeof(p->msg), "virtual x8 selftest failed");
    }
    return -1;
}

int psram_bench_step(psram_ui_state_t *p)
{
    return -1;
    int rc;
    uint32_t mismatch = 0U;
    const uint32_t off = p->stress_mode ? p->stress_off : ((p->loop & 0x3FFU) * 4U);
    XTime wr_ticks = 0U, rd_ticks = 0U, cmp_ticks = 0U, total_ticks = 0U;
    const uint32_t rw_bytes = (uint32_t)(PSRAM_COPY_BYTES * 2U);

    if (p == NULL) {
        return -1;
    }

    rc = psram_test_window_copy_memcpy(off, 0x12340000U ^ p->loop ^ off, &mismatch,
                                       &wr_ticks, &rd_ticks, &cmp_ticks, &total_ticks);
    p->mismatch_count = mismatch;
    p->last_status = REG_READ(REG_PSRAM_STATUS);
    p->last_write_ticks = wr_ticks;
    p->last_read_ticks = rd_ticks;
    p->last_cmp_ticks = cmp_ticks;
    p->last_copy_ticks = total_ticks;
    p->last_rw_mibps_x100 = psram_mibps_x100(rw_bytes, (XTime)(wr_ticks + rd_ticks));
    p->last_loop_mibps_x100 = psram_mibps_x100(PSRAM_COPY_BYTES, total_ticks);
    if (((uint64_t)wr_ticks + (uint64_t)rd_ticks) != 0U && COUNTS_PER_SECOND != 0U) {
        p->last_rw_sec_per_mib_x100 =
            (uint32_t)((((uint64_t)wr_ticks + (uint64_t)rd_ticks) * 100ULL * 1024ULL * 1024ULL) /
                       ((uint64_t)COUNTS_PER_SECOND * (uint64_t)rw_bytes));
    } else {
        p->last_rw_sec_per_mib_x100 = 0U;
    }
    if (total_ticks != 0U && COUNTS_PER_SECOND != 0U) {
        p->last_loop_sec_per_mib_x100 =
            (uint32_t)(((uint64_t)total_ticks * 100ULL * 1024ULL * 1024ULL) /
                       ((uint64_t)COUNTS_PER_SECOND * (uint64_t)PSRAM_COPY_BYTES));
    } else {
        p->last_loop_sec_per_mib_x100 = 0U;
    }

    p->bench_total_ticks += (uint64_t)total_ticks;
    p->bench_total_rw_ticks += (uint64_t)wr_ticks + (uint64_t)rd_ticks;
    p->bench_total_rw_bytes += (uint64_t)rw_bytes;
    p->bench_total_loop_bytes += (uint64_t)PSRAM_COPY_BYTES;
    if (p->bench_total_ticks != 0U && COUNTS_PER_SECOND != 0U) {
        uint64_t bps_rw = 0U;
        if (p->bench_total_rw_ticks != 0U) {
            bps_rw = (p->bench_total_rw_bytes * (uint64_t)COUNTS_PER_SECOND) / p->bench_total_rw_ticks;
        }
        p->avg_rw_mibps_x100 = (uint32_t)((bps_rw * 100ULL) / (1024ULL * 1024ULL));
        if (p->bench_total_rw_bytes != 0U) {
            p->avg_rw_sec_per_mib_x100 =
                (uint32_t)((p->bench_total_rw_ticks * 100ULL * 1024ULL * 1024ULL) /
                           ((uint64_t)COUNTS_PER_SECOND * p->bench_total_rw_bytes));
        } else {
            p->avg_rw_sec_per_mib_x100 = 0U;
        }
        p->avg_loop_sec_per_mib_x100 =
            (uint32_t)((p->bench_total_ticks * 100ULL * 1024ULL * 1024ULL) /
                       ((uint64_t)COUNTS_PER_SECOND * p->bench_total_loop_bytes));
    }

    p->loop++;
    if (p->stress_mode) {
        p->stress_off += PSRAM_COPY_BYTES;
        if (p->stress_off >= (PSRAM_WIN_SIZE - PSRAM_COPY_BYTES)) {
            p->stress_off = 0U;
            p->stress_passes++;
        }
    }

    if (rc == 0) {
        p->ok_count++;
        snprintf(p->msg, sizeof(p->msg), "%s pass", p->stress_mode ? "stress" : "quick");
        return 0;
    }

    p->fail_count++;
    snprintf(p->msg, sizeof(p->msg), "fail mismatch=%lu", (unsigned long)mismatch);
    return -1;
}

/* ------------------------------------------------------------------ */
/* PSRAM read-capture delay (dcount) calibration                       */
/*                                                                     */
/* Boot-time eye scan: write a stress pattern to scratch PSRAM, sweep  */
/* every capture setting (edge x 32 delay taps) through PSDMA, and     */
/* program the center of the widest passing window for board and       */
/* temperature margin.                                                 */
/* ------------------------------------------------------------------ */

#include "xil_cache.h"
#include "../lib/uart.h"

#define CAL_PSDMA_MC_ADDR    0x40030000U
#define CAL_PSDMA_DDR_ADDR   0x40030004U
#define CAL_PSDMA_LEN_RW     0x40030008U
#define CAL_PSDMA_STATUS     0x4003000CU
#define CAL_PSDMA_RW_TO_MC   (1UL << 31)
#define CAL_DCOUNT_REG       0x4000018CU   /* CARD_CTRL word 0x63 */
#define CAL_SCRATCH_MC       0x00000040U   /* bank-0 region (aliased by
                                             * RamWorks bank 128); cal runs
                                             * at boot before the Apple, so
                                             * the 64-byte scribble lands in
                                             * power-on-undefined content */
#define CAL_BYTES            64U

static uint8_t g_cal_buf[CAL_BYTES] __attribute__((aligned(64)));
static uint8_t g_cal_ref[CAL_BYTES] __attribute__((aligned(64)));

static int cal_psdma(uint32_t mc, uint32_t ddr, uint32_t len, uint32_t to_mc)
{
    uint32_t i;
    REG_WRITE(CAL_PSDMA_MC_ADDR, mc);
    REG_WRITE(CAL_PSDMA_DDR_ADDR, ddr);
    REG_WRITE(CAL_PSDMA_LEN_RW, (to_mc ? CAL_PSDMA_RW_TO_MC : 0U) | len);
    for (i = 0U; i < 5000000U; ++i) {
        if ((REG_READ(CAL_PSDMA_STATUS) & 1U) != 0U) {
            return 0;
        }
    }
    return -1;
}

static uint32_t cal_setting_passes(uint32_t setting)
{
    uint32_t rep;
    uint32_t i;

    REG_WRITE(CAL_DCOUNT_REG, setting);
    for (rep = 0U; rep < 4U; ++rep) {
        Xil_DCacheFlushRange((UINTPTR)g_cal_buf, sizeof(g_cal_buf));
        if (cal_psdma(CAL_SCRATCH_MC, (uint32_t)(uintptr_t)g_cal_buf,
                      CAL_BYTES, 0U) != 0) {
            return 0U;
        }
        Xil_DCacheInvalidateRange((UINTPTR)g_cal_buf, sizeof(g_cal_buf));
        for (i = 0U; i < CAL_BYTES; ++i) {
            if (g_cal_buf[i] != g_cal_ref[i]) {
                return 0U;
            }
        }
    }
    return 1U;
}

int psram_calibrate_dcount(uint32_t uart_base)
{
    uint32_t i;
    uint64_t pass_map = 0U;
    uint32_t best_start = 0U;
    uint32_t best_len = 0U;
    char line[112];

    for (i = 0U; i < CAL_BYTES; ++i) {
        static const uint8_t seed[8] =
            { 0x00U, 0xFFU, 0x55U, 0xAAU, 0xA5U, 0x5AU, 0x0FU, 0xF0U };
        g_cal_ref[i] = (uint8_t)(seed[i & 7U] ^ (uint8_t)(i << 3));
    }
    Xil_DCacheFlushRange((UINTPTR)g_cal_ref, sizeof(g_cal_ref));
    if (cal_psdma(CAL_SCRATCH_MC, (uint32_t)(uintptr_t)g_cal_ref,
                  CAL_BYTES, 1U) != 0) {
        uart_puts(uart_base, "psram cal: pattern write FAILED");
        uart_puts(uart_base, "\r\n");
        return -1;
    }

    for (i = 0U; i < 64U; ++i) {
        if (cal_setting_passes(i) != 0U) {
            pass_map |= (1ULL << i);
        }
    }

    /* Longest consecutive passing run within each edge half
     * (settings 0-31 = pos edge, 32-63 = neg edge; a physical eye
     * cannot straddle the edge boundary). */
    {
        uint32_t half;
        for (half = 0U; half < 2U; ++half) {
            uint32_t run_start = 0U;
            uint32_t run_len = 0U;
            const uint32_t base = half * 32U;
            for (i = 0U; i < 32U; ++i) {
                if (((pass_map >> (base + i)) & 1ULL) != 0ULL) {
                    if (run_len == 0U) {
                        run_start = base + i;
                    }
                    run_len++;
                    if (run_len > best_len) {
                        best_len = run_len;
                        best_start = run_start;
                    }
                } else {
                    run_len = 0U;
                }
            }
        }
    }

    (void)snprintf(line, sizeof(line),
        "psram cal: eye map neg=%08lX pos=%08lX",
        (unsigned long)(pass_map >> 32), (unsigned long)(pass_map & 0xFFFFFFFFUL));
    uart_puts(uart_base, line);
    uart_puts(uart_base, "\r\n");

    if (best_len == 0U) {
        REG_WRITE(CAL_DCOUNT_REG, 0U);
        uart_puts(uart_base,
                  "psram cal: NO passing window, default kept");
        uart_puts(uart_base, "\r\n");
        return -1;
    }

    i = best_start + (best_len / 2U);
    REG_WRITE(CAL_DCOUNT_REG, i);
    (void)snprintf(line, sizeof(line),
        "psram cal: window len=%lu -> setting %lu (edge=%lu delay=%lu)",
        (unsigned long)best_len, (unsigned long)i,
        (unsigned long)((i >> 5) & 1U), (unsigned long)(i & 31U));
    uart_puts(uart_base, line);
    uart_puts(uart_base, "\r\n");
    return (int)i;
}

/* ------------------------------------------------------------------ */
/* PSRAM bank probe: writes a distinct signature line into each of a   */
/* set of 64K-bank bases across the 24-bit space, then reads them all  */
/* back. Aliasing (two banks, one storage) or capacity truncation      */
/* (high address bits ignored) shows immediately -- independent of     */
/* every Apple-side mechanism. RamWorks banks 1..128 live at           */
/* 0x010000..0x800000; disk2 staging starts at 0xE00000.               */
/* ------------------------------------------------------------------ */

int psram_bank_probe(uint32_t uart_base)
{
    static const uint32_t bases[] = {
        0x010000U, 0x020000U, 0x030000U, 0x100000U, 0x200000U,
        0x400000U, 0x7E0000U, 0x7F0000U, 0x800000U, 0x810000U,
        0xC00000U, 0xD00000U
    };
    static uint8_t pbuf[64] __attribute__((aligned(64)));
    uint32_t i, j, bad = 0U;
    char line[96];

    for (i = 0U; i < sizeof(bases)/sizeof(bases[0]); ++i) {
        for (j = 0U; j < 64U; ++j) {
            pbuf[j] = (uint8_t)(0xB0U + i) ^ (uint8_t)j;
        }
        Xil_DCacheFlushRange((UINTPTR)pbuf, sizeof(pbuf));
        if (cal_psdma(bases[i] + 0x40U, (uint32_t)(uintptr_t)pbuf,
                      64U, 1U) != 0) {
            uart_puts(uart_base, "bankprobe: write TIMEOUT\r\n");
            return -1;
        }
    }
    for (i = 0U; i < sizeof(bases)/sizeof(bases[0]); ++i) {
        uint32_t errs = 0U;
        Xil_DCacheFlushRange((UINTPTR)pbuf, sizeof(pbuf));
        if (cal_psdma(bases[i] + 0x40U, (uint32_t)(uintptr_t)pbuf,
                      64U, 0U) != 0) {
            uart_puts(uart_base, "bankprobe: read TIMEOUT\r\n");
            return -1;
        }
        Xil_DCacheInvalidateRange((UINTPTR)pbuf, sizeof(pbuf));
        for (j = 0U; j < 64U; ++j) {
            if (pbuf[j] != ((uint8_t)(0xB0U + i) ^ (uint8_t)j)) {
                errs++;
            }
        }
        (void)snprintf(line, sizeof(line),
            "bank 0x%06lX: %s (sig %02X, got %02X%02X%02X%02X)\r\n",
            (unsigned long)bases[i], errs ? "FAIL" : "ok",
            (unsigned)(0xB0U + i),
            pbuf[0], pbuf[1], pbuf[2], pbuf[3]);
        uart_puts(uart_base, line);
        bad += errs;
    }
    (void)snprintf(line, sizeof(line),
        "bankprobe: %lu byte errors\r\n", (unsigned long)bad);
    uart_puts(uart_base, line);
    return (int)bad;
}
