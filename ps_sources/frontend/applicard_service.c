/* PCPI Applicard service -- see applicard_service.h. */

#include "applicard_service.h"

#include <stdio.h>
#include <string.h>

#include "xiltimer.h"

#include "applicard_regs.h"
#include "applicard_rom.h"
#include "applicard_z80.h"
#include "../lib/common.h"
#include "../lib/uart.h"

/* A core blocked on a handshake gets an occasional slice so software timeout
 * loops still advance. Catch-up slices use a short unchanged-poll limit
 * because no handshake state changed while the core slept. */
#define APPLICARD_IDLE_CATCHUP_MS     250U
#define APPLICARD_IDLE_RECHECK_POLLS  256U

static applicard_z80_ctx_t g_ctx;
static uint32_t g_uart_base;
static uint8_t g_enabled;
static uint32_t g_slice_budget = APPLICARD_DEFAULT_SLICE_TSTATES;
static uint32_t g_wall_cap_us = APPLICARD_DEFAULT_WALL_CAP_US;
static void (*g_checkpoint)(void);

static uint32_t g_blocked_flags;
static XTime g_blocked_since;

static uint32_t g_reset_count;
static uint32_t g_nmi_count;
static uint64_t g_cycles_acc;
static uint64_t g_ticks_acc;
static uint32_t g_slice_count;
static uint32_t g_gov_block_count;   /* slices ended by the idle governor */
static uint32_t g_gov_resume_count;  /* wakeups because PL flags changed */
static uint32_t g_gov_catchup_count; /* wakeups from the catch-up timer */

void applicard_service_init(uint32_t uart_base)
{
    g_uart_base = uart_base;

    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.ram = (uint8_t *)APPLICARD_Z80_RAM_BASE;
    memset(g_ctx.ram, 0, APPLICARD_Z80_RAM_SIZE);

    /* Fill the 8 KB mirror page with the embedded 2 KB boot ROM repeated:
     * together with the shared page pointer this realizes the $0000-$7FFF
     * mirror. The ROM is embedded only -- deliberately no SD override, so
     * a stray user file can never change the card's behavior. */
    for (uint32_t off = 0U; off < APPLICARD_Z80_PAGE_SIZE;
         off += APPLICARD_Z80_ROM_SIZE) {
        memcpy(&g_ctx.rom_mirror[off], applicard_rom, APPLICARD_Z80_ROM_SIZE);
    }
    applicard_z80_reset(&g_ctx);

    uart_puts(uart_base, "Applicard: embedded boot ROM ready\r\n");
}

void applicard_service_set_enabled(uint8_t enable)
{
    if (enable != 0U) {
        applicard_z80_reset(&g_ctx);
        g_enabled = 1U;
    } else {
        g_enabled = 0U;
    }
}

uint8_t applicard_service_is_enabled(void)
{
    return g_enabled;
}

void applicard_service_request_reset(void)
{
    applicard_z80_reset(&g_ctx);
    g_reset_count++;
}

void applicard_service_set_budget(uint32_t tstates)
{
    if (tstates < 1000U) {
        tstates = 1000U;
    }
    if (tstates > 2000000U) {
        tstates = 2000000U;
    }
    g_slice_budget = tstates;
}

void applicard_service_set_wall_cap(uint32_t us)
{
    if (us < 200U) {
        us = 200U;
    }
    if (us > 20000U) {
        us = 20000U;
    }
    g_wall_cap_us = us;
}

void applicard_service_set_checkpoint(void (*checkpoint)(void))
{
    g_checkpoint = checkpoint;
}

void applicard_service_poll(void)
{
    uint32_t status;
    XTime t0;
    XTime t1;
    int elapsed;

    if (g_enabled == 0U) {
        return;
    }

    status = REG_READ(APPLICARD_REG_STATUS);

    if ((status & APPLICARD_STATUS_RESET_REQ) != 0U) {
        applicard_z80_reset(&g_ctx);
        REG_WRITE(APPLICARD_REG_CONTROL, APPLICARD_CONTROL_CLR_RESET);
        g_reset_count++;
    }
    if ((status & APPLICARD_STATUS_NMI_REQ) != 0U) {
        (void)Z80NonMaskableInterrupt(&g_ctx.state, &g_ctx);
        REG_WRITE(APPLICARD_REG_CONTROL, APPLICARD_CONTROL_CLR_NMI);
        g_nmi_count++;
        g_ctx.blocked = 0U;
    }

    if (g_ctx.blocked != 0U) {
        const uint32_t flags =
            status & (APPLICARD_STATUS_F_Z80 | APPLICARD_STATUS_F_6502);
        if (flags == g_blocked_flags) {
            XTime now;
            XTime_GetTime(&now);
            if ((uint64_t)(now - g_blocked_since) <
                ((uint64_t)APPLICARD_IDLE_CATCHUP_MS *
                 (COUNTS_PER_SECOND / 1000U))) {
                return;
            }
            /* Catch-up wakeup: nothing changed while asleep, so run with
             * a short leash -- the streak is pre-loaded so the burst
             * re-blocks after IDLE_RECHECK_POLLS unchanged polls. A value
             * change still resets the streak and earns a full run. */
            g_gov_catchup_count++;
            g_ctx.status_streak =
                (uint16_t)(APPLICARD_Z80_IDLE_STREAK -
                           APPLICARD_IDLE_RECHECK_POLLS);
        } else {
            g_gov_resume_count++;
            g_ctx.status_streak = 0U;
            /* Force the first status poll of the new slice to re-sample. */
            g_ctx.last_status_byte = 0xFFU;
        }
        g_ctx.blocked = 0U;
    }

    /* Compute-greedy scheduling: one slice per main-loop pass starves a
     * compute-bound Z80 (a ~230 us slice inside a ~2 ms pass is ~11%% duty).
     * Run slices back-to-back under a wall-clock cap, giving USB0 its
     * priority checkpoint and re-sampling the PL stickies between slices.
     * An idle Z80 exits the loop immediately via the governor. */
    {
        XTime loop_start;
        const uint64_t cap_ticks =
            ((uint64_t)g_wall_cap_us * COUNTS_PER_SECOND) / 1000000ULL;

        XTime_GetTime(&loop_start);
        for (;;) {
            g_ctx.stop_requested = 0U;
            /* Deliberately NOT resetting status_streak here: the idle
             * threshold (4096 polls) is larger than one slice's worth of
             * polling, so it must accumulate across the burst's slices.
             * Resetting per slice made the governor unreachable and left
             * the emulator burning the full wall cap at an idle prompt. */

            XTime_GetTime(&t0);
            elapsed = Z80Emulate(&g_ctx.state, (int)g_slice_budget, &g_ctx);
            XTime_GetTime(&t1);

            g_cycles_acc += (uint64_t)elapsed;
            g_ticks_acc += (uint64_t)(t1 - t0);
            g_slice_count++;

            if (g_ctx.blocked != 0U) {
                /* Snapshot the flags the wait loop saw; the governor
                 * sleeps until the PL view differs (or reset/NMI/
                 * catch-up). */
                g_gov_block_count++;
                g_blocked_since = t1;
                g_blocked_flags = 0U;
                if ((g_ctx.last_status_byte & 0x80U) != 0U) {
                    g_blocked_flags |= APPLICARD_STATUS_F_Z80;
                }
                if ((g_ctx.last_status_byte & 0x01U) != 0U) {
                    g_blocked_flags |= APPLICARD_STATUS_F_6502;
                }
                break;
            }
            if ((uint64_t)(t1 - loop_start) >= cap_ticks) {
                break;
            }
            if (g_checkpoint != NULL) {
                g_checkpoint();
            }
            /* A $C0D5 reset or NMI posted mid-burst is handled at the top
             * of the next poll; don't keep burning the burst on stale
             * work. */
            status = REG_READ(APPLICARD_REG_STATUS);
            if ((status &
                 (APPLICARD_STATUS_RESET_REQ | APPLICARD_STATUS_NMI_REQ))
                != 0U) {
                break;
            }
        }
    }
}

void applicard_service_uart_status(uint32_t uart_base)
{
    static uint64_t s_prev_cycles;
    static uint64_t s_prev_ticks;
    static uint64_t s_prev_wall;
    static uint32_t s_prev_statpolls;
    static uint32_t s_prev_tx;
    static uint32_t s_prev_rx;
    XTime wall_now;
    char line[160];
    const uint32_t status = REG_READ(APPLICARD_REG_STATUS);
    const uint32_t debug = REG_READ(APPLICARD_REG_DEBUG);
    const uint64_t dc = g_cycles_acc - s_prev_cycles;
    const uint64_t dt = g_ticks_acc - s_prev_ticks;
    /* Effective emulated clock while actually executing: T-states per
     * second of host time spent inside Z80Emulate, in kHz to stay in
     * 32-bit-friendly ranges. */
    uint32_t khz = 0U;
    if (dt != 0U) {
        khz = (uint32_t)((dc * (COUNTS_PER_SECOND / 1000U)) / dt);
    }
    s_prev_cycles = g_cycles_acc;
    s_prev_ticks = g_ticks_acc;

    (void)snprintf(line, sizeof(line),
        "z80: en=%u blocked=%u budget=%lu wall=%luus resets=%lu nmis=%lu\r\n",
        (unsigned)g_enabled,
        (unsigned)g_ctx.blocked, (unsigned long)g_slice_budget,
        (unsigned long)g_wall_cap_us,
        (unsigned long)g_reset_count, (unsigned long)g_nmi_count);
    uart_puts(uart_base, line);

    (void)snprintf(line, sizeof(line),
        "z80: PC=%04X SP=%04X AF=%04X BC=%04X DE=%04X HL=%04X "
        "rom_mapped=%u bank=%02X\r\n",
        (unsigned)(g_ctx.state.pc & 0xFFFFU),
        (unsigned)g_ctx.state.registers.word[Z80_SP],
        (unsigned)g_ctx.state.registers.word[Z80_AF],
        (unsigned)g_ctx.state.registers.word[Z80_BC],
        (unsigned)g_ctx.state.registers.word[Z80_DE],
        (unsigned)g_ctx.state.registers.word[Z80_HL],
        (unsigned)g_ctx.rom_mapped, (unsigned)g_ctx.bank_reg);
    uart_puts(uart_base, line);

    (void)snprintf(line, sizeof(line),
        "z80: eff=%lu.%03lu MHz (since last status) slices=%lu "
        "tx=%lu rx=%lu\r\n",
        (unsigned long)(khz / 1000U), (unsigned long)(khz % 1000U),
        (unsigned long)g_slice_count,
        (unsigned long)g_ctx.bytes_to_apple,
        (unsigned long)g_ctx.bytes_from_apple);
    uart_puts(uart_base, line);

    (void)snprintf(line, sizeof(line),
        "z80: gov blocks=%lu resumes=%lu catchups=%lu\r\n",
        (unsigned long)g_gov_block_count,
        (unsigned long)g_gov_resume_count,
        (unsigned long)g_gov_catchup_count);
    uart_puts(uart_base, line);

    /* Duty = share of wall time spent inside Z80Emulate since the last
     * status call; statpolls/trips localize where a slow run went (high
     * statpolls = spinning on 6502 latency, low duty = main-loop bound). */
    XTime_GetTime(&wall_now);
    {
        const uint64_t wall_dt = (uint64_t)wall_now - s_prev_wall;
        uint32_t duty_pct = 0U;
        if (s_prev_wall != 0U && wall_dt != 0U) {
            duty_pct = (uint32_t)((dt * 100U) / wall_dt);
        }
        (void)snprintf(line, sizeof(line),
            "z80: duty=%lu%% statpolls=%lu trips tx=%lu rx=%lu "
            "(since last status)\r\n",
            (unsigned long)duty_pct,
            (unsigned long)(g_ctx.status_reads - s_prev_statpolls),
            (unsigned long)(g_ctx.bytes_to_apple - s_prev_tx),
            (unsigned long)(g_ctx.bytes_from_apple - s_prev_rx));
        uart_puts(uart_base, line);
    }
    s_prev_wall = (uint64_t)wall_now;
    s_prev_statpolls = g_ctx.status_reads;
    s_prev_tx = g_ctx.bytes_to_apple;
    s_prev_rx = g_ctx.bytes_from_apple;

    (void)snprintf(line, sizeof(line),
        "z80: pl status=%08lX debug=%08lX (f_z80=%lu f_6502=%lu "
        "rst=%lu nmi=%lu seq=%02lX)\r\n",
        (unsigned long)status, (unsigned long)debug,
        (unsigned long)(status & APPLICARD_STATUS_F_Z80),
        (unsigned long)((status & APPLICARD_STATUS_F_6502) >> 1),
        (unsigned long)((status & APPLICARD_STATUS_RESET_REQ) >> 2),
        (unsigned long)((status & APPLICARD_STATUS_NMI_REQ) >> 3),
        (unsigned long)APPLICARD_STATUS_SEQ(status));
    uart_puts(uart_base, line);
}

void applicard_service_uart_dump(uint32_t uart_base, uint32_t addr,
                                 uint32_t len)
{
    char line[96];

    if (len == 0U || len > 256U) {
        len = 64U;
    }
    for (uint32_t row = 0U; row < len; row += 16U) {
        char *p = line;
        p += snprintf(p, sizeof(line),
                      "%04lX:", (unsigned long)((addr + row) & 0xFFFFU));
        for (uint32_t i = 0U; i < 16U && (row + i) < len; ++i) {
            const uint32_t a = (addr + row + i) & 0xFFFFU;
            const uint8_t b =
                g_ctx.rd_page[a >> APPLICARD_Z80_PAGE_SHIFT]
                             [a & (APPLICARD_Z80_PAGE_SIZE - 1U)];
            p += snprintf(p, (size_t)(line + sizeof(line) - p), " %02X", b);
        }
        uart_puts(uart_base, line);
        uart_puts(uart_base, "\r\n");
    }
}
