#include "uart_control.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../lib/common.h"
#include "../lib/framebuffer.h"
#include "../lib/uart.h"
#include "xil_cache.h"
#include "ff.h"
#include "psram_bench.h"
#include "card_control_regs.h"
#include "boot_menu_service.h"
#include "compositor.h"
#include "supersprite_vdp.h"
#include "disk2_service.h"
#include "smartport_service.h"
#include "usb_hid_service.h"
#include "usb_storage_backend.h"
#include "usb_storage_service.h"
#include "usb_sdd_service.h"
#include "applicard_service.h"
#include "config_menu.h"
#include "xusbps_class_storage.h"
#include "xusbps_hw.h"
#include "xiltimer.h"

#define UART_SR_OFFSET    0x2CU
#define UART_FIFO_OFFSET  0x30U
#define UART_SR_RXEMPTY   0x00000002U
#define UART_ESC_BYTE_WAIT_LOOPS 20000U

#define MRD_OCM_END          0x0003FFFFU
#define MRD_DDR_START        0x00100000U
#define MRD_DDR_END          0x3FFFFFFFU
#define MRD_PL_START         0x40000000U
#define MRD_PL_END           0x400FFFFFU
#define MRD_PS_PERIPH_START  0xE0000000U
#define MRD_PS_PERIPH_END    0xE02FFFFFU
#define MRD_SLCR_START       0xF8000000U
#define MRD_SLCR_END         0xF8000FFFU
#define MRD_QSPI_START       0xFC000000U
#define MRD_QSPI_END         0xFDFFFFFFU

typedef struct {
    uint32_t dma_ctrl;
    uint32_t fifo_cnt;
    uint32_t exec_cnt;
    uint32_t dma_snap;
} uart_control_dma_dbg_t;
/* Bound by main.c so the 'sdd' command routes through the config menu
 * (personality switch + persisted setting stay in lockstep). */
static config_menu_t *g_sdd_config_menu = NULL;

void uart_control_bind_config_menu(void *menu)
{
    g_sdd_config_menu = (config_menu_t *)menu;
}

static int uart_try_getc(uint32_t base, char *out)
{
    if ((REG_READ(base + UART_SR_OFFSET) & UART_SR_RXEMPTY) != 0U) {
        return 0;
    }
    *out = (char)(REG_READ(base + UART_FIFO_OFFSET) & 0xFFU);
    return 1;
}

static uint8_t mrd_addr_allowed(uint32_t addr, uint32_t count)
{
    uint32_t last;

    if (count == 0U || addr > UINT32_MAX - ((count - 1U) * 4U)) {
        return 0U;
    }
    last = addr + ((count - 1U) * 4U);

    if (last <= MRD_OCM_END) {
        return 1U;
    }
    if (addr >= MRD_DDR_START && last <= MRD_DDR_END) {
        return 1U;
    }
    if (addr >= MRD_PL_START && last <= MRD_PL_END) {
        return 1U;
    }
    if (addr >= MRD_PS_PERIPH_START && last <= MRD_PS_PERIPH_END) {
        return 1U;
    }
    if (addr >= MRD_SLCR_START && last <= MRD_SLCR_END) {
        return 1U;
    }
    if (addr >= MRD_QSPI_START && last <= MRD_QSPI_END) {
        return 1U;
    }
    return 0U;
}

/* When bustail is armed, the main-loop poll advances the SDD egress consumer
 * behind the producer so the ring free-runs. Without this, flow control fills
 * the 512 KB ring after roughly 125 ms and every dump shows the same window. */
/* ------------------------------------------------------------------ */
/* sswatch: live reference-MMU divergence checker.                     */
/*                                                                     */
/* Replays the SDD ring's real bus stream through a reference //e MMU  */
/* soft-switch model (UTAIIe / AppleWin semantics) and compares it     */
/* against the PL tracker's live state (CARD_CTRL reg 2). On stable    */
/* divergence it reports which switches differ -- mechanically finding */
/* "the switch we handle wrong" on real traffic.                       */
/* Requires the bustail tap armed (same ring).                         */
/* ------------------------------------------------------------------ */

static uint8_t  g_ssw_on = 0U;
static uint32_t g_ssw_cons = 0U;      /* our private ring cursor */
static uint32_t g_ssw_model = 0U;     /* reference state, live-reg packing */
static uint8_t  g_ssw_lc_prewrite = 0U;
/* C8 arbitration trio, modeled per AppleWin Memory.cpp:
 * INTC8ROM sets on C3xx access with SLOTC3ROM off in both INTCXROM
 * states; clears only on $CFFF access or reset. INTCXROM/SLOTC3ROM
 * are write-only $C006/7 and $C00A/B. */
static uint8_t g_ssw_intcxrom = 0U;
static uint8_t g_ssw_slotc3rom = 0U;
static uint8_t g_ssw_intc8rom = 0U;
static uint8_t g_ssw_c8_slot = 0U;

static uint32_t g_ssw_mismatch_polls = 0U;
static uint32_t g_ssw_reports = 0U;
static uint32_t g_ssw_last_c0xx[8];
static uint32_t g_ssw_last_idx = 0U;

/* live-reg bit positions (see apple_top current_softswitch_state) */
#define SSW_80STORE  (1UL<<0)
#define SSW_RAMRD    (1UL<<1)
#define SSW_RAMWRT   (1UL<<2)
#define SSW_ALTZP    (1UL<<3)
#define SSW_TEXT     (1UL<<4)
#define SSW_MIXED    (1UL<<5)
#define SSW_PAGE2    (1UL<<6)
#define SSW_HIRES    (1UL<<7)
#define SSW_ALTCHAR  (1UL<<8)
#define SSW_80COL    (1UL<<9)
#define SSW_DHIRES   (1UL<<10)
#define SSW_LCBANK2  (1UL<<11)
#define SSW_LCRD     (1UL<<12)
#define SSW_LCWRT    (1UL<<13)

static void ssw_bit(uint32_t bit, uint32_t on)
{
    if (on) { g_ssw_model |= bit; } else { g_ssw_model &= ~bit; }
}

/* Reference //e MMU update for one bus event. */
static void ssw_apply(uint32_t addr, uint32_t is_read, uint32_t data)
{
    (void)data;
    if ((addr >> 12) != 0xCU) {
        return;
    }
    if ((addr & 0xFF00U) != 0xC000U) {
        /* Cxxx (slot/C8 space): feed the C8 model; log only the
         * arbitration-relevant ones to the history. */
        if (addr == 0xCFFFU || (addr >= 0xC300U && addr <= 0xC3FFU)) {
            g_ssw_last_c0xx[g_ssw_last_idx & 7U] =
                addr | (is_read ? 0x10000U : 0U);
            g_ssw_last_idx++;
        }
    } else {
        g_ssw_last_c0xx[g_ssw_last_idx & 7U] =
            addr | (is_read ? 0x10000U : 0U);
        g_ssw_last_idx++;
    }

    if (addr <= 0xC00FU) {
        /* Write-only memory/video switches. */
        if (!is_read) {
            const uint32_t on = addr & 1U;
            switch ((addr >> 1) & 7U) {
            case 0: ssw_bit(SSW_80STORE, on); break;
            case 1: ssw_bit(SSW_RAMRD,   on); break;
            case 2: ssw_bit(SSW_RAMWRT,  on); break;
            case 3: g_ssw_intcxrom = (uint8_t)on; break;
            case 4: ssw_bit(SSW_ALTZP,   on); break;
            case 5: g_ssw_slotc3rom = (uint8_t)on; break;
            case 6: ssw_bit(SSW_80COL,   on); break;
            case 7: ssw_bit(SSW_ALTCHAR, on); break;
            default: break;
            }
        }
        return;
    }

    if (addr >= 0xC050U && addr <= 0xC057U) {
        /* R/W-actuated video switches. */
        const uint32_t on = addr & 1U;
        switch ((addr >> 1) & 3U) {
        case 0: ssw_bit(SSW_TEXT,  on); break;
        case 1: ssw_bit(SSW_MIXED, on); break;
        case 2: ssw_bit(SSW_PAGE2, on); break;
        case 3: ssw_bit(SSW_HIRES, on); break;
        default: break;
        }
        return;
    }
    if (addr == 0xC05EU || addr == 0xC05FU) {
        ssw_bit(SSW_DHIRES, (addr & 1U) == 0U);   /* C05E on, C05F off */
        return;
    }

    if (addr >= 0xC100U && addr <= 0xCFFFU) {
        if (addr == 0xCFFFU) {
            g_ssw_intc8rom = 0U;
            g_ssw_c8_slot = 0U;
        } else if (addr <= 0xC7FFU) {
            const uint32_t slot = (addr >> 8) & 7U;
            if (!g_ssw_intcxrom && slot != 0U) {
                g_ssw_c8_slot = (uint8_t)slot;
            }
            if (slot == 3U && !g_ssw_slotc3rom) {
                g_ssw_intc8rom = 1U;   /* both INTCXROM states */
                g_ssw_c8_slot = 0U;
            }
        }
        return;
    }

    if (addr >= 0xC080U && addr <= 0xC08FU) {
        /* Language card (UTAIIe 5-23): A0/A1 select read/write, A3
         * selects bank. Pre-write latch: set by odd READ, cleared by
         * even access or any write. Write-enable: odd READ with
         * pre-write already set; cleared by even access. */
        const uint32_t a0 = addr & 1U;
        const uint32_t a1 = (addr >> 1) & 1U;
        ssw_bit(SSW_LCBANK2, ((addr >> 3) & 1U) == 0U);
        ssw_bit(SSW_LCRD, a0 == a1);
        if (a0 == 0U) {
            g_ssw_lc_prewrite = 0U;
            ssw_bit(SSW_LCWRT, 0U);
        } else {
            if (is_read) {
                if (g_ssw_lc_prewrite) {
                    ssw_bit(SSW_LCWRT, 1U);
                }
                g_ssw_lc_prewrite = 1U;
            } else {
                g_ssw_lc_prewrite = 0U;
                /* odd WRITE: write-enable unchanged */
            }
        }
        return;
    }
}

static uint32_t g_ssw_laps = 0U;
static uint32_t g_ssw_since_c08x = 0U;
static uint32_t g_ssw_warmup = 0U;static uint32_t g_ssw_map_bad = 0U;
static uint32_t g_ssw_map_reports = 0U;

/* Reference //e memory map: should this access be card-served (aux
 * read serve / aux write capture), given the model switch state?
 * Mirrors the serve predicate exactly: route==CACHE && bank!=0.
 * C-page ($Cxxx) is excluded (slot/C8 arbitration audited separately). */
static uint32_t ssw_expect_serve(uint32_t addr, uint32_t is_read)
{
    if (addr < 0x0200U) {
        return (g_ssw_model & SSW_ALTZP) != 0U;
    }
    if (addr < 0xC000U) {
        const uint32_t in_text_win = (addr >= 0x0400U) && (addr <= 0x07FFU);
        const uint32_t in_hgr_win  = (addr >= 0x2000U) && (addr <= 0x3FFFU) &&
                                     ((g_ssw_model & SSW_HIRES) != 0U);
        if (((g_ssw_model & SSW_80STORE) != 0U) && (in_text_win || in_hgr_win)) {
            return (g_ssw_model & SSW_PAGE2) != 0U;
        }
        return is_read ? ((g_ssw_model & SSW_RAMRD) != 0U)
                       : ((g_ssw_model & SSW_RAMWRT) != 0U);
    }
    /* $D000-$FFFF language card */
    if (is_read ? ((g_ssw_model & SSW_LCRD) == 0U)
                : ((g_ssw_model & SSW_LCWRT) == 0U)) {
        return 0U;   /* ROM read / discarded write: never served */
    }
    return (g_ssw_model & SSW_ALTZP) != 0U;
}

static void sswatch_poll(uint32_t uart_base)
{
    uint32_t producer;
    uint32_t pending;
    uint32_t steps;

    if (g_ssw_on == 0U) {
        return;
    }

    producer = *(volatile uint32_t *)(uintptr_t)0x3F030000U;
    pending = (producer - g_ssw_cons) & ((1U << 19) - 1U);

    /* Lap detection: the PL ring is 512KB; if more than ~7/8 of it is
     * pending, assume we were lapped and the stream under our cursor
     * was overwritten. Resync and invalidate the model until the next
     * quiet window (comparison suppressed until re-seeded). */
    if (pending > ((7U << 19) / 8U)) {
        g_ssw_cons = producer;
        g_ssw_model = REG_READ(CARD_CTRL_REG_ADDR(0x02U)) & 0x3FFFU;
        g_ssw_lc_prewrite = 0U;
        g_ssw_mismatch_polls = 0U;
        g_ssw_laps++;
        return;
    }

    /* Drain to fully caught up (bounded; 64K events ~ a few ms). */
    steps = 0U;
    while (g_ssw_cons != producer && steps < 70000U) {
        const uint32_t lo =
            *(volatile uint32_t *)(uintptr_t)(0x3F040000U + g_ssw_cons);
        g_ssw_cons = (g_ssw_cons + 8U) & ((1U << 19) - 1U);
        steps++;
        g_ssw_since_c08x++;
        if (lo == 0U) {
            continue;
        }
        if ((lo & 0x20000U) == 0U) {
            g_ssw_model = SSW_TEXT | SSW_LCBANK2 | SSW_LCWRT;
            g_ssw_lc_prewrite = 0U;
            g_ssw_intcxrom = 0U;
            g_ssw_slotc3rom = 0U;
            g_ssw_intc8rom = 0U;
            g_ssw_c8_slot = 0U;
            continue;
        }
        {
            const uint32_t ca = lo & 0xFFFFU;
            const uint32_t cw = ((lo >> 16) & 1U) == 0U;
            if (((ca & 0xFFF0U) == 0xC080U) ||
                (ca <= 0xC00FU && ca >= 0xC000U && cw) ||
                (ca >= 0xC050U && ca <= 0xC05FU)) {
                g_ssw_since_c08x = 0U;
            }
        }
        /* Routing audit: compare the tracker's recorded serve verdict
         * ([31] route_cache && [30] bank_nz) against the reference
         * map, for every non-C-page access. Warmup skips the seed
         * race right after arming. */
        if (g_ssw_warmup >= 4096U) {
            const uint32_t a = lo & 0xFFFFU;
            if ((a >> 12) != 0xCU) {
                const uint32_t is_rd = (lo >> 16) & 1U;
                const uint32_t rec =
                    ((lo >> 31) & 1U) & ((lo >> 30) & 1U);
                if (rec != ssw_expect_serve(a, is_rd)) {
                    g_ssw_map_bad++;
                    if (g_ssw_map_reports < 8U) {
                        char mline[112];
                        g_ssw_map_reports++;
                        (void)snprintf(mline, sizeof(mline),
                            "sswatch: MAP %c %04lX served=%lu expected=%lu state=%04lX\r\n",
                            is_rd ? 'R' : 'W',
                            (unsigned long)a,
                            (unsigned long)rec,
                            (unsigned long)ssw_expect_serve(a, is_rd),
                            (unsigned long)(g_ssw_model & 0x3FFFU));
                        uart_puts(uart_base, mline);
                    }
                }
            }
        } else {
            g_ssw_warmup++;
        }
        ssw_apply(lo & 0xFFFFU, (lo >> 16) & 1U, (lo >> 20) & 0xFFU);
        /* refresh producer occasionally so we truly catch up */
        if ((steps & 1023U) == 0U) {
            producer = *(volatile uint32_t *)(uintptr_t)0x3F030000U;
        }
    }

    /* Compare ONLY when: fully caught up, and the last C08x access is
     * at least 256 events in the past (LC bursts are sparse; outside a
     * burst, model/live skew is impossible). */
    if (g_ssw_cons != producer || g_ssw_since_c08x < 256U) {
        return;
    }
    {
        /* C8-state compare: live reg 0x65 = {c8_select[2:0],
         * intc8rom, slotc3rom, intcxrom}. c8_select is export-gated
         * (0 while intc8rom) -- mirror that for the model. */
        const uint32_t c8live =
            REG_READ(CARD_CTRL_REG_ADDR(0x65U)) & 0x3FU;
        const uint32_t c8model =
            ((g_ssw_intc8rom ? 0U : (uint32_t)g_ssw_c8_slot) << 3) |
            ((uint32_t)g_ssw_intc8rom << 2) |
            ((uint32_t)g_ssw_slotc3rom << 1) |
            (uint32_t)g_ssw_intcxrom;
        static uint32_t c8_bad_polls = 0U;
        static uint32_t c8_reports = 0U;
        if (c8model != c8live) {
            c8_bad_polls++;
        } else {
            c8_bad_polls = 0U;
        }
        if (c8_bad_polls == 3U && c8_reports < 16U) {
            char cline[112];
            uint32_t k;
            c8_reports++;
            (void)snprintf(cline, sizeof(cline),
                "sswatch: C8 DIVERGENCE model=%02lX live=%02lX "
                "[slot|c8|c3|cx] laps=%lu\r\n",
                (unsigned long)c8model, (unsigned long)c8live,
                (unsigned long)g_ssw_laps);
            uart_puts(uart_base, cline);
            for (k = 0U; k < 8U; ++k) {
                const uint32_t e =
                    g_ssw_last_c0xx[(g_ssw_last_idx + k) & 7U];
                (void)snprintf(cline, sizeof(cline),
                    "  c0xx[%lu]: %c %04lX\r\n",
                    (unsigned long)k,
                    (e & 0x10000U) ? 'R' : 'W',
                    (unsigned long)(e & 0xFFFFU));
                uart_puts(uart_base, cline);
            }
        }
    }
    {
        const uint32_t live =
            REG_READ(CARD_CTRL_REG_ADDR(0x02U)) & 0x3FFFU;
        if ((g_ssw_model & 0x3FFFU) != live) {
            g_ssw_mismatch_polls++;
        } else {
            g_ssw_mismatch_polls = 0U;
        }
        if (g_ssw_mismatch_polls == 3U && g_ssw_reports < 16U) {
            char line[120];
            uint32_t k;
            g_ssw_reports++;
            (void)snprintf(line, sizeof(line),
                "sswatch: DIVERGENCE model=%04lX live=%04lX diff=%04lX laps=%lu\r\n",
                (unsigned long)(g_ssw_model & 0x3FFFU),
                (unsigned long)live,
                (unsigned long)((g_ssw_model ^ live) & 0x3FFFU),
                (unsigned long)g_ssw_laps);
            uart_puts(uart_base, line);
            for (k = 0U; k < 8U; ++k) {
                const uint32_t e =
                    g_ssw_last_c0xx[(g_ssw_last_idx + k) & 7U];
                (void)snprintf(line, sizeof(line),
                    "  c0xx[%lu]: %c %04lX\r\n",
                    (unsigned long)k,
                    (e & 0x10000U) ? 'R' : 'W',
                    (unsigned long)(e & 0xFFFFU));
                uart_puts(uart_base, line);
            }
        }
    }
}

static uint8_t g_bustail_armed = 0U;
static uint8_t g_bustail_trapped = 0U;

/* Chase the SDD egress consumer so the forensics ring free-runs; in
 * trap mode, stop chasing the moment the newest events contain IRQ/BRK
 * vector fetches ($FFFE reads) so the ring preserves the ~125 ms leading
 * into the storm, including the triggering entry. */
static void bustail_chase_consumer(void)
{
    if (g_bustail_armed == 0U || g_bustail_trapped != 0U) {
        return;
    }
    {
        const uint32_t producer =
            *(volatile uint32_t *)(uintptr_t)0x3F030000U;
        uint32_t vec_hits = 0U;
        uint32_t k;
        for (k = 1U; k <= 16U; ++k) {
            const uint32_t off =
                (producer - (k * 8U)) & ((1U << 19) - 1U);
            const uint32_t lo =
                *(volatile uint32_t *)(uintptr_t)(0x3F040000U + off);
            if ((lo & 0x1FFFFU) == 0x1FFFEU) {   /* read of $FFFE */
                vec_hits++;
            }
        }
        if (vec_hits >= 2U) {
            REG_WRITE(CARD_CTRL_REG_ADDR(0x50U), 0U);  /* tap OFF */
            g_bustail_trapped = 1U;
            return;
        }
        REG_WRITE(CARD_CTRL_REG_ADDR(0x54U), producer);
    }
}

static void uart_control_set_input(ui_input_t *input, ui_key_t key)
{
    if (input == NULL) {
        return;
    }
    input->key = key;
    input->pressed = (key != UI_KEY_NONE) ? 1U : 0U;
    input->ascii = 0U;
}

static ui_key_t uart_control_escape_final_key(char final)
{
    switch (final) {
    case 'A': return UI_KEY_UP;
    case 'B': return UI_KEY_DOWN;
    case 'C': return UI_KEY_RIGHT;
    case 'D': return UI_KEY_LEFT;
    case 'Z': return UI_KEY_SHIFT_TAB;
    default:  return UI_KEY_NONE;
    }
}

static int uart_try_getc_wait(uint32_t base, char *out)
{
    uint32_t i;

    for (i = 0U; i < UART_ESC_BYTE_WAIT_LOOPS; ++i) {
        if (uart_try_getc(base, out)) {
            return 1;
        }
    }
    return 0;
}

static ui_key_t uart_control_decode_escape(uint32_t base)
{
    char c;
    uint32_t code;

    if (!uart_try_getc_wait(base, &c)) {
        return UI_KEY_ESC;
    }

    if (c == '[' || c == 'O') {
        if (!uart_try_getc_wait(base, &c)) {
            return UI_KEY_ESC;
        }
        if (c >= '0' && c <= '9') {
            code = 0U;
            do {
                if (c >= '0' && c <= '9') {
                    code = (code * 10U) + (uint32_t)(c - '0');
                }
                if (!uart_try_getc_wait(base, &c)) {
                    return UI_KEY_ESC;
                }
            } while ((c >= '0' && c <= '9') || c == ';');
            if (c == '~') {
                if (code == 5U) {
                    return UI_KEY_PAGE_UP;
                }
                if (code == 6U) {
                    return UI_KEY_PAGE_DOWN;
                }
            }
            return uart_control_escape_final_key(c);
        }
        return uart_control_escape_final_key(c);
    }

    return UI_KEY_ESC;
}

int uart_control_has_pending_input(const uart_control_t *control)
{
    if (control == NULL) {
        return 0;
    }

    return ((REG_READ(control->control_uart_base + UART_SR_OFFSET) & UART_SR_RXEMPTY) == 0U) ? 1 : 0;
}

const char *uart_control_key_name(ui_key_t key)
{
    switch (key) {
    case UI_KEY_UP: return "UP";
    case UI_KEY_DOWN: return "DOWN";
    case UI_KEY_LEFT: return "LEFT";
    case UI_KEY_RIGHT: return "RIGHT";
    case UI_KEY_ENTER: return "E";
    case UI_KEY_BACK: return "BACK";
    case UI_KEY_TOGGLE: return "TOGGLE";
    case UI_KEY_SCANLINES: return "SCANLINES";
    case UI_KEY_TAB: return "TAB";
    case UI_KEY_SHIFT_TAB: return "SHIFT-TAB";
    case UI_KEY_PAGE_UP: return "PAGE-UP";
    case UI_KEY_PAGE_DOWN: return "PAGE-DOWN";
    case UI_KEY_SPACE: return "SPACE";
    case UI_KEY_ESC: return "ESC";
    case UI_KEY_MENU: return "MENU";
    default: return "NONE";
    }
}

static int str_ieq(const char *a, const char *b)
{
    unsigned char ca;
    unsigned char cb;

    if (a == NULL || b == NULL) {
        return 0;
    }

    while (*a != '\0' && *b != '\0') {
        ca = (unsigned char)*a++;
        cb = (unsigned char)*b++;
        if (tolower(ca) != tolower(cb)) {
            return 0;
        }
    }
    return (*a == '\0' && *b == '\0');
}

static int split_tokens(char *line, char **argv, int max_args)
{
    int argc = 0;
    char *p = line;

    while (*p != '\0' && argc < max_args) {
        while (*p != '\0' && isspace((unsigned char)*p)) {
            p++;
        }
        if (*p == '\0') {
            break;
        }

        argv[argc++] = p;
        while (*p != '\0' && !isspace((unsigned char)*p)) {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        *p++ = '\0';
    }

    return argc;
}

static int parse_u32(const char *text, uint32_t *out)
{
    char *end = NULL;
    unsigned long val;

    if (text == NULL || out == NULL) {
        return -1;
    }

    val = strtoul(text, &end, 0);
    if (end == text || *end != '\0') {
        return -1;
    }
    *out = (uint32_t)val;
    return 0;
}

static int parse_disk2_drive(const char *text, uint8_t *drive)
{
    if (text == NULL || drive == NULL) {
        return -1;
    }
    if (str_ieq(text, "d1") || str_ieq(text, "drive1") || str_ieq(text, "1")) {
        *drive = 0U;
        return 0;
    }
    if (str_ieq(text, "d2") || str_ieq(text, "drive2") || str_ieq(text, "2")) {
        *drive = 1U;
        return 0;
    }
    return -1;
}

static int parse_disk2_scan_args(int argc,
                                 char **argv,
                                 uint8_t *drive,
                                 int *all_tracks)
{
    int index;
    int drive_seen = 0;

    if (drive == NULL || all_tracks == NULL) {
        return -1;
    }

    *drive = 0U;
    *all_tracks = 0;
    for (index = 2; index < argc; ++index) {
        uint8_t parsed_drive;

        if (str_ieq(argv[index], "all")) {
            if (*all_tracks != 0) {
                return -1;
            }
            *all_tracks = 1;
        } else if (parse_disk2_drive(argv[index], &parsed_drive) == 0) {
            if (drive_seen != 0) {
                return -1;
            }
            *drive = parsed_drive;
            drive_seen = 1;
        } else {
            return -1;
        }
    }
    return 0;
}

static void uart_control_dump_bytes(uint32_t uart_base,
                                    uint32_t start_offset,
                                    const uint8_t *buf,
                                    uint32_t count)
{
    uint32_t row;

    for (row = 0U; row < count; row += 16U) {
        uint32_t col;
        char line[96];
        char *p = line;
        size_t remaining = sizeof(line);
        int n;

        n = snprintf(p, remaining, "%03lX:", (unsigned long)(start_offset + row));
        if (n < 0 || (size_t)n >= remaining) {
            return;
        }
        p += n;
        remaining -= (size_t)n;

        for (col = 0U; col < 16U && (row + col) < count; ++col) {
            n = snprintf(p, remaining, " %02X", (unsigned int)buf[row + col]);
            if (n < 0 || (size_t)n >= remaining) {
                return;
            }
            p += n;
            remaining -= (size_t)n;
        }
        uart_puts(uart_base, line);
        uart_puts(uart_base, "\r\n");
    }
}

static void dma_hold_set(uint8_t enable)
{
    uint32_t ctrl = REG_READ(AT_DMA_CTRL_REG);

    if (enable != 0U) {
        ctrl |= AT_DMA_CTRL_HOLD_REQ_BIT;
    } else {
        ctrl &= ~AT_DMA_CTRL_HOLD_REQ_BIT;
    }
    REG_WRITE(AT_DMA_CTRL_REG, ctrl);
}

static int dma_dbg_read(uart_control_dma_dbg_t *out)
{
    if (out == NULL) {
        return -1;
    }

    out->dma_ctrl = REG_READ(AT_DMA_CTRL_REG);
    out->fifo_cnt = REG_READ(SP_DBG_FIFO_CNT_REG);
    out->exec_cnt = REG_READ(SP_DBG_EXEC_CNT_REG);
    out->dma_snap = REG_READ(SP_DBG_DMA_SNAP_REG);
    return 0;
}

static void dma_dbg_reset(void)
{
    REG_WRITE(SP_DBG_RESET_REG, 0U);
    REG_WRITE(AT_TAIL_CTRL_REG, AT_TAIL_CTRL_CLEAR_BIT);
}

static int wait_for_probe_complete(uint32_t requested_count, uint32_t *ctrl_out)
{
    uint32_t ctrl = 0U;
    uint32_t poll;
    uint32_t saw_active = 0U;
    uint32_t budget = requested_count * 1000U + 200000U;

    if (budget > 10000000U) {
        budget = 10000000U;
    }

    for (poll = 0U; poll < budget; ++poll) {
        ctrl = REG_READ(AT_PROBE_CTRL_REG);
        if ((ctrl & AT_PROBE_CTRL_ACTIVE_BIT) != 0U) {
            saw_active = 1U;
        } else if (saw_active != 0U) {
            break;
        }
    }

    ctrl = REG_READ(AT_PROBE_CTRL_REG);
    if (ctrl_out != NULL) {
        *ctrl_out = ctrl;
    }
    return (saw_active != 0U && (ctrl & AT_PROBE_CTRL_ACTIVE_BIT) == 0U) ? 0 : -1;
}

static int dma_peek_raw_read(uint16_t addr, uint16_t count, uint8_t *out_buf, uint16_t *done_out)
{
    uint32_t params = AT_PROBE_PARAMS_READ_BIT | (uint32_t)addr;
    uint32_t ctrl = 0U;
    uint16_t done = 0U;
    uint16_t i;
    int rc;

    REG_WRITE(AT_PROBE_PARAMS_REG, params);
    REG_WRITE(AT_PROBE_COUNT_REG, (uint32_t)count);
    REG_WRITE(AT_PROBE_CTRL_REG, AT_PROBE_CTRL_GO_BIT);

    rc = wait_for_probe_complete((uint32_t)count, &ctrl);
    done = (uint16_t)(ctrl & 0xFFFFU);
    if (done > count) {
        done = count;
    }

    if (out_buf != NULL) {
        for (i = 0U; i < done; ++i) {
            REG_WRITE(AT_PEEK_INDEX_REG, i);
            (void)REG_READ(AT_PEEK_DATA_REG);
            out_buf[i] = (uint8_t)(REG_READ(AT_PEEK_DATA_REG) & 0xFFU);
        }
    }

    if (done_out != NULL) {
        *done_out = done;
    }
    return rc;
}

static int dma_probe_write(uint16_t addr, uint8_t pattern, uint16_t count, uint16_t *done_count_out)
{
    uint32_t params = ((uint32_t)pattern << 16) | (uint32_t)addr;
    uint32_t ctrl = 0U;
    int rc;

    REG_WRITE(AT_PROBE_PARAMS_REG, params);
    REG_WRITE(AT_PROBE_COUNT_REG, (uint32_t)count);
    REG_WRITE(AT_PROBE_CTRL_REG, AT_PROBE_CTRL_GO_BIT);

    rc = wait_for_probe_complete((uint32_t)count, &ctrl);
    if (done_count_out != NULL) {
        *done_count_out = (uint16_t)(ctrl & 0xFFFFU);
    }
    return rc;
}

static int parse_rtc_datetime(const char *date_str, const char *time_str, rtc_pcf8563_time_t *t)
{
    unsigned int y;
    unsigned int mo;
    unsigned int d;
    unsigned int hh;
    unsigned int mm;
    unsigned int ss;

    if (date_str == NULL || time_str == NULL || t == NULL) {
        return -1;
    }

    if (sscanf(date_str, "%u-%u-%u", &y, &mo, &d) != 3) {
        return -1;
    }
    if (sscanf(time_str, "%u:%u:%u", &hh, &mm, &ss) != 3) {
        return -1;
    }

    if (y < 2000U || y > 2099U || mo < 1U || mo > 12U || d < 1U || d > 31U ||
        hh > 23U || mm > 59U || ss > 59U) {
        return -1;
    }

    t->year = (uint16_t)y;
    t->month = (uint8_t)mo;
    t->day = (uint8_t)d;
    t->hour = (uint8_t)hh;
    t->min = (uint8_t)mm;
    t->sec = (uint8_t)ss;
    t->wday = rtc_pcf8563_weekday_from_ymd(t->year, t->month, t->day);
    t->valid = 1U;
    t->status = 0U;

    return 0;
}

static int parse_nav_key(const char *text, ui_key_t *key)
{
    if (text == NULL || key == NULL) {
        return -1;
    }

    if (str_ieq(text, "up")) {
        *key = UI_KEY_UP;
        return 0;
    }
    if (str_ieq(text, "down")) {
        *key = UI_KEY_DOWN;
        return 0;
    }
    if (str_ieq(text, "left")) {
        *key = UI_KEY_LEFT;
        return 0;
    }
    if (str_ieq(text, "right")) {
        *key = UI_KEY_RIGHT;
        return 0;
    }
    if (str_ieq(text, "e") || str_ieq(text, "select") || str_ieq(text, "action")) {
        *key = UI_KEY_ENTER;
        return 0;
    }
    if (str_ieq(text, "back") || str_ieq(text, "home")) {
        *key = UI_KEY_BACK;
        return 0;
    }
    if (str_ieq(text, "toggle")) {
        *key = UI_KEY_TOGGLE;
        return 0;
    }
    if (str_ieq(text, "scanlines") || str_ieq(text, "scanline")) {
        *key = UI_KEY_SCANLINES;
        return 0;
    }
    if (str_ieq(text, "tab")) {
        *key = UI_KEY_TAB;
        return 0;
    }
    if (str_ieq(text, "shift-tab") ||
        str_ieq(text, "shift_tab") ||
        str_ieq(text, "backtab")) {
        *key = UI_KEY_SHIFT_TAB;
        return 0;
    }
    if (str_ieq(text, "page-up") ||
        str_ieq(text, "page_up") ||
        str_ieq(text, "pgup")) {
        *key = UI_KEY_PAGE_UP;
        return 0;
    }
    if (str_ieq(text, "page-down") ||
        str_ieq(text, "page_down") ||
        str_ieq(text, "pgdn")) {
        *key = UI_KEY_PAGE_DOWN;
        return 0;
    }
    if (str_ieq(text, "space")) {
        *key = UI_KEY_SPACE;
        return 0;
    }
    if (str_ieq(text, "esc") || str_ieq(text, "escape")) {
        *key = UI_KEY_ESC;
        return 0;
    }
    if (str_ieq(text, "menu")) {
        *key = UI_KEY_MENU;
        return 0;
    }

    return -1;
}

static int parse_bool_mode(const char *text, int *mode)
{
    if (text == NULL || mode == NULL) {
        return -1;
    }

    if (str_ieq(text, "on") || str_ieq(text, "enable") || str_ieq(text, "enabled")) {
        *mode = 1;
        return 0;
    }
    if (str_ieq(text, "off") || str_ieq(text, "disable") || str_ieq(text, "disabled")) {
        *mode = 0;
        return 0;
    }
    if (str_ieq(text, "toggle")) {
        *mode = 2;
        return 0;
    }

    return -1;
}

static int parse_scanlines_mode(const char *text, int *mode)
{
    uint32_t numeric;

    if (text == NULL || mode == NULL) {
        return -1;
    }
    if (str_ieq(text, "toggle")) {
        *mode = APPLETINI_SCANLINES_COUNT;
        return 0;
    }
    if (str_ieq(text, "off") ||
        str_ieq(text, "disable") ||
        str_ieq(text, "disabled")) {
        *mode = APPLETINI_SCANLINES_OFF;
        return 0;
    }
    if (str_ieq(text, "on") ||
        str_ieq(text, "enable") ||
        str_ieq(text, "enabled") ||
        str_ieq(text, "light")) {
        *mode = APPLETINI_SCANLINES_LIGHT;
        return 0;
    }
    if (str_ieq(text, "medium") || str_ieq(text, "med")) {
        *mode = APPLETINI_SCANLINES_MEDIUM;
        return 0;
    }
    if (str_ieq(text, "strong") || str_ieq(text, "heavy")) {
        *mode = APPLETINI_SCANLINES_STRONG;
        return 0;
    }
    if (parse_u32(text, &numeric) == 0 && numeric < APPLETINI_SCANLINES_COUNT) {
        *mode = (int)numeric;
        return 0;
    }

    return -1;
}

static const char *mono_color_name(uint8_t color)
{
    switch (color & 0x3U) {
    case 0U: return "black";
    case 1U: return "white";
    case 2U: return "amber";
    default: return "green";
    }
}

static const char *boot_device_name(uint8_t device)
{
    return (device == 1U) ? "Disk II" : "SmartPort";
}

static const char *boot_timeout_name(uint8_t mode)
{
    switch (mode) {
    case 1U: return "5 seconds";
    case 2U: return "Always show";
    case 0U:
    default: return "3 seconds";
    }
}

static int parse_mono_color(const char *text, uint8_t *color)
{
    if (text == NULL || color == NULL) {
        return -1;
    }

    if (str_ieq(text, "black")) {
        *color = 0U;
        return 0;
    }
    if (str_ieq(text, "white")) {
        *color = 1U;
        return 0;
    }
    if (str_ieq(text, "amber")) {
        *color = 2U;
        return 0;
    }
    if (str_ieq(text, "green")) {
        *color = 3U;
        return 0;
    }

    return -1;
}

static int load_snapshot(const uart_control_ops_t *ops, uart_control_snapshot_t *snapshot)
{
    if (ops == NULL || ops->get_snapshot == NULL || snapshot == NULL) {
        return -1;
    }

    if (ops->get_snapshot(ops->ctx, snapshot) != 0) {
        return -1;
    }

    return 0;
}

static void print_snapshot(const uart_control_t *control, const uart_control_ops_t *ops)
{
    char line[256];
    uart_control_snapshot_t snap;
    int32_t centi;

    if (load_snapshot(ops, &snap) != 0) {
        uart_puts(control->control_uart_base, "status: unavailable\r\n");
        return;
    }

    uart_puts(control->control_uart_base, "---- card status ----\r\n");
    snprintf(line, sizeof(line), "ui: frames=%lu keys=%lu",
             (unsigned long)snap.frame_count,
             (unsigned long)snap.key_count);
    uart_puts(control->control_uart_base, line);
    uart_puts(control->control_uart_base, "\r\n");

    snprintf(line, sizeof(line),
             "boot cfg: device=%s timeout=%s disk2_slot6=%u loaded=%u session=%u",
             boot_device_name(snap.config_boot_device),
             boot_timeout_name(snap.config_boot_timeout_mode),
             (unsigned)snap.config_disk2_slot6_enabled,
             (unsigned)snap.config_settings_loaded,
             (unsigned)snap.config_session_only);
    uart_puts(control->control_uart_base, line);
    uart_puts(control->control_uart_base, "\r\n");

    snprintf(line, sizeof(line), "video: vsync=%s scanlines=%s",
             snap.vsync_enable ? "on" : "off",
             appletini_scanlines_name(snap.scanlines_mode));
    uart_puts(control->control_uart_base, line);
    uart_puts(control->control_uart_base, "\r\n");

    snprintf(line, sizeof(line), "text mono: fg=%s bg=%s",
             mono_color_name(snap.text_mono_fg_color),
             mono_color_name(snap.text_mono_bg_color));
    uart_puts(control->control_uart_base, line);
    uart_puts(control->control_uart_base, "\r\n");

    if (snap.display_mono_enable) {
        snprintf(line, sizeof(line), "display mono: on (%s)",
                 mono_color_name(snap.display_mono_color));
    } else {
        snprintf(line, sizeof(line), "display mono: off");
    }
    uart_puts(control->control_uart_base, line);
    uart_puts(control->control_uart_base, "\r\n");

    snprintf(line, sizeof(line),
             "apple fb: slot=%lu mode=%s target=%uHz apple_fps=%lu.%02lu hdmi_fps=%lu.%02lu blits=%lu comp_pub=%lu comp_skip=%lu pl_vblanks=%lu latched=0x%08lX",
             (unsigned long)snap.apple_fb_slot,
             (snap.apple_fb_mode != 0U) ? "SHR" : "legacy",
             (unsigned)(snap.apple_video_50hz != 0U ? 50U : 60U),
             (unsigned long)(snap.apple_fps_x100 / 100U),
             (unsigned long)(snap.apple_fps_x100 % 100U),
             (unsigned long)(snap.fps_x100 / 100U),
             (unsigned long)(snap.fps_x100 % 100U),
             (unsigned long)snap.apple_fb_blits,
             (unsigned long)snap.compositor_frames_published,
             (unsigned long)snap.compositor_frames_skipped,
             (unsigned long)snap.fb_frame_count,
             (unsigned long)snap.fb_last_latched);
    uart_puts(control->control_uart_base, line);
    uart_puts(control->control_uart_base, "\r\n");

    if (snap.updater_meta_valid) {
        snprintf(line, sizeof(line), "updater meta: golden='%s' local_fw='%s'",
                 snap.updater_golden_version, snap.updater_firmware_version);
    } else {
        snprintf(line, sizeof(line), "updater meta: unavailable");
    }
    uart_puts(control->control_uart_base, line);
    uart_puts(control->control_uart_base, "\r\n");

    snprintf(line, sizeof(line), "audio: en=%s mute=%s tone=%luHz amp=0x%08lX",
             snap.audio_enable ? "on" : "off",
             snap.audio_mute ? "on" : "off",
             (unsigned long)snap.audio_tone_hz,
             (unsigned long)snap.audio_amp);
    uart_puts(control->control_uart_base, line);
    uart_puts(control->control_uart_base, "\r\n");

    snprintf(line, sizeof(line), "spdif dbg: clkc=%lu q3lvl=%lu q3tog=%lu",
             (unsigned long)snap.audio_clkcnt,
             (unsigned long)snap.audio_q3lvl,
             (unsigned long)snap.audio_q3tog);
    uart_puts(control->control_uart_base, line);
    uart_puts(control->control_uart_base, "\r\n");

    if (snap.rtc.valid) {
        snprintf(line, sizeof(line), "rtc: %04u-%02u-%02u %02u:%02u:%02u",
                 (unsigned)snap.rtc.year, (unsigned)snap.rtc.month, (unsigned)snap.rtc.day,
                 (unsigned)snap.rtc.hour, (unsigned)snap.rtc.min, (unsigned)snap.rtc.sec);
    } else {
        snprintf(line, sizeof(line), "rtc: N/A status=0x%02X", (unsigned)snap.rtc.status);
    }
    uart_puts(control->control_uart_base, line);
    uart_puts(control->control_uart_base, "\r\n");

    if (snap.temp.valid) {
        char sign = '+';
        centi = snap.temp.centi_c;
        if (centi < 0) {
            sign = '-';
            centi = -centi;
        }
        snprintf(line, sizeof(line), "temp: %c%ld.%02ld C",
                 sign, (long)(centi / 100), (long)(centi % 100));
    } else {
        snprintf(line, sizeof(line), "temp: N/A");
    }
    uart_puts(control->control_uart_base, line);
    uart_puts(control->control_uart_base, "\r\n");

    if (!snap.i2c_ready) {
        uart_puts(control->control_uart_base, "i2c: not initialized\r\n");
    } else if (!snap.tmp102_ready) {
        uart_puts(control->control_uart_base, "i2c: tmp102 unavailable\r\n");
    }
}

static uint32_t ticks_to_us_sat(u64 ticks)
{
    u64 us;

    if (ticks == 0U || COUNTS_PER_SECOND == 0U) {
        return 0U;
    }
    us = (ticks * 1000000ULL) / (u64)COUNTS_PER_SECOND;
    return (us > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : (uint32_t)us;
}

static uint32_t mibps_x100(u64 bytes, u64 ticks)
{
    u64 rate;

    if (bytes == 0U || ticks == 0U || COUNTS_PER_SECOND == 0U) {
        return 0U;
    }
    rate = (bytes * (u64)COUNTS_PER_SECOND * 100ULL) /
           (ticks * 1048576ULL);
    return (rate > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : (uint32_t)rate;
}

static void print_usb_rate(uint32_t uart_base,
                           const char *label,
                           u64 bytes,
                           u64 ticks,
                           u64 last_ticks,
                           u64 max_ticks)
{
    char line[160];
    uint32_t rate = mibps_x100(bytes, ticks);

    snprintf(line, sizeof(line),
             "%s: %lu.%02lu MiB/s total=%llu MiB last=%lu us max=%lu us\r\n",
             label,
             (unsigned long)(rate / 100U),
             (unsigned long)(rate % 100U),
             (unsigned long long)(bytes / 1048576ULL),
             (unsigned long)ticks_to_us_sat(last_ticks),
             (unsigned long)ticks_to_us_sat(max_ticks));
    uart_puts(uart_base, line);
}

static void print_usb_storage_perf(const uart_control_t *control)
{
    usb_storage_service_stats_t service;
    usb_storage_class_stats_t scsi;
    usb_storage_backend_stats_t sd;
    uint32_t uart_base = control->control_uart_base;
    char line[256];

    usb_storage_service_get_stats(&service);
    XUsbPs_StorageGetStats(&scsi);
    usb_storage_get_backend_stats(&sd);

    uart_puts(uart_base, "---- usb storage perf ----\r\n");
    snprintf(line, sizeof(line),
             "limits: max_cbw=%lu KiB block=%lu queue_capacity~=%lu packets\r\n",
             (unsigned long)(USB_STORAGE_MAX_TRANSFER_BYTES / 1024U),
             (unsigned long)USB_STORAGE_BLOCK_SIZE,
             (unsigned long)((2U * USB_STORAGE_MAX_TRANSFER_BLOCKS) + 16U));
    uart_puts(uart_base, line);

    snprintf(line, sizeof(line),
             "medium: ready=%lu reinit=%lu/%lu guard_rej=%lu sense=%02lX/%02lX "
             "idle_to=%lu wd_recover=%lu ring_full=%lu csw_fail=%lu "
             "csw_retry=%lu\r\n",
             (unsigned long)(usb_storage_medium_ready() ? 1U : 0U),
             (unsigned long)sd.reinit_successes,
             (unsigned long)sd.reinit_attempts,
             (unsigned long)sd.guard_rejects,
             (unsigned long)scsi.last_sense_key,
             (unsigned long)scsi.last_sense_asc,
             (unsigned long)scsi.ep1_idle_wait_timeouts,
             (unsigned long)scsi.prime_watchdog_recoveries,
             (unsigned long)scsi.ring_full_recoveries,
             (unsigned long)scsi.csw_send_failures,
             (unsigned long)scsi.csw_retries);
    uart_puts(uart_base, line);
    snprintf(line, sizeof(line),
             "bot: proto_stalls=%lu (data-IN failed -> stall+CSW per BOT)\r\n",
             (unsigned long)scsi.ep1_protocol_stalls);
    uart_puts(uart_base, line);

    snprintf(line, sizeof(line),
             "usb0: starts=%llu irq=%llu rst=%llu ui=%llu ue=%llu pc=%llu mask=0x%08lX cfg=%lu\r\n",
             (unsigned long long)service.starts,
             (unsigned long long)service.irq_count,
             (unsigned long long)service.reset_irqs,
             (unsigned long long)service.ui_irqs,
             (unsigned long long)service.ue_irqs,
             (unsigned long long)service.pc_irqs,
             (unsigned long)service.last_irq_mask,
             (unsigned long)service.current_config);
    uart_puts(uart_base, line);
    snprintf(line, sizeof(line),
             "attach: disc=%llu conn=%llu need_ep0_prime=%lu\r\n",
             (unsigned long long)service.soft_disconnects,
             (unsigned long long)service.soft_connects,
             (unsigned long)service.need_ep0_prime);
    uart_puts(uart_base, line);

    snprintf(line, sizeof(line),
             "ep0: setup=%llu data=%llu rx_fail=%llu other=%llu stall=%llu send_fail=%llu\r\n",
             (unsigned long long)service.ep0_setup_events,
             (unsigned long long)service.ep0_data_rx_events,
             (unsigned long long)service.ep0_rx_failures,
             (unsigned long long)service.ep0_other_events,
             (unsigned long long)service.ep0_stalls,
             (unsigned long long)service.ep0_send_failures);
    uart_puts(uart_base, line);

    snprintf(line, sizeof(line),
             "setup: bm=0x%02lX req=0x%02lX wValue=0x%04lX wIndex=0x%04lX wLen=%lu errors=%llu class=%llu\r\n",
             (unsigned long)service.last_setup_bm_request_type,
             (unsigned long)service.last_setup_b_request,
             (unsigned long)service.last_setup_w_value,
             (unsigned long)service.last_setup_w_index,
             (unsigned long)service.last_setup_w_length,
             (unsigned long long)service.setup_errors,
             (unsigned long long)service.class_requests);
    uart_puts(uart_base, line);

    snprintf(line, sizeof(line),
             "recovery: msc_reset=%llu clr_halt=%llu (BOT error-recovery path)\r\n",
             (unsigned long long)service.msc_resets,
             (unsigned long long)service.clear_feature_halt);
    uart_puts(uart_base, line);

    snprintf(line, sizeof(line),
             "desc: dev=%llu cfg=%llu str=%llu qual=%llu other=%llu last=0x%02lX req=%lu reply=%lu setaddr=%llu setcfg=%llu\r\n",
             (unsigned long long)service.get_descriptor_device,
             (unsigned long long)service.get_descriptor_config,
             (unsigned long long)service.get_descriptor_string,
             (unsigned long long)service.get_descriptor_qualifier,
             (unsigned long long)service.get_descriptor_other,
             (unsigned long)service.last_desc_type,
             (unsigned long)service.last_desc_request_len,
             (unsigned long)service.last_desc_reply_len,
             (unsigned long long)service.set_address_requests,
             (unsigned long long)service.set_configuration_requests);
    uart_puts(uart_base, line);

    snprintf(line, sizeof(line),
             "ep1: rx=%llu tx=%llu fail=%llu other=%llu ep0prime=%llu/%llu ep1prime=%llu/%llu lastprime ep=%lu dir=%lu st=%ld\r\n",
             (unsigned long long)service.ep1_rx_events,
             (unsigned long long)service.ep1_tx_events,
             (unsigned long long)service.ep1_rx_failures,
             (unsigned long long)service.ep1_other_events,
             (unsigned long long)service.ep0_prime_count,
             (unsigned long long)service.ep0_prime_failures,
             (unsigned long long)service.ep1_prime_count,
             (unsigned long long)service.ep1_prime_failures,
             (unsigned long)service.last_prime_ep,
             (unsigned long)service.last_prime_dir,
             (long)service.last_prime_status);
    uart_puts(uart_base, line);

    snprintf(line, sizeof(line),
             "regs: cmd=0x%08lX sts=0x%08lX intr=0x%08lX addr=0x%08lX eplist=0x%08lX mode=0x%08lX portsc=0x%08lX otgsc=0x%08lX\r\n",
             (unsigned long)service.last_usb_cmd,
             (unsigned long)service.last_usb_sts,
             (unsigned long)service.last_usb_intr,
             (unsigned long)service.last_deviceaddr,
             (unsigned long)service.last_eplistaddr,
             (unsigned long)service.last_usb_mode,
             (unsigned long)service.last_portsc,
             (unsigned long)service.last_otgsc);
    uart_puts(uart_base, line);

    snprintf(line, sizeof(line),
             "port: ccs=%lu csc=%lu pe=%lu pec=%lu susp=%lu pr=%lu hsp=%lu ls=%lu pp=%lu phcd=%lu spd=%lu\r\n",
             (unsigned long)((service.last_portsc & XUSBPS_PORTSCR_CCS_MASK) ? 1U : 0U),
             (unsigned long)((service.last_portsc & XUSBPS_PORTSCR_CSC_MASK) ? 1U : 0U),
             (unsigned long)((service.last_portsc & XUSBPS_PORTSCR_PE_MASK) ? 1U : 0U),
             (unsigned long)((service.last_portsc & XUSBPS_PORTSCR_PEC_MASK) ? 1U : 0U),
             (unsigned long)((service.last_portsc & XUSBPS_PORTSCR_SUSP_MASK) ? 1U : 0U),
             (unsigned long)((service.last_portsc & XUSBPS_PORTSCR_PR_MASK) ? 1U : 0U),
             (unsigned long)((service.last_portsc & XUSBPS_PORTSCR_HSP_MASK) ? 1U : 0U),
             (unsigned long)((service.last_portsc & XUSBPS_PORTSCR_LS_MASK) >> 10),
             (unsigned long)((service.last_portsc & XUSBPS_PORTSCR_PP_MASK) ? 1U : 0U),
             (unsigned long)((service.last_portsc & XUSBPS_PORTSCR_PHCD_MASK) ? 1U : 0U),
             (unsigned long)((service.last_portsc & XUSBPS_PORTSCR_PSPD_MASK) >> 26));
    uart_puts(uart_base, line);

    snprintf(line, sizeof(line),
             "otg: id=%lu avv=%lu asv=%lu bsv=%lu bse=%lu ot=%lu dp=%lu idpu=%lu 1mst=%lu\r\n",
             (unsigned long)((service.last_otgsc & XUSBPS_OTGSC_ID_MASK) ? 1U : 0U),
             (unsigned long)((service.last_otgsc & XUSBPS_OTGSC_AVV_MASK) ? 1U : 0U),
             (unsigned long)((service.last_otgsc & XUSBPS_OTGSC_ASV_MASK) ? 1U : 0U),
             (unsigned long)((service.last_otgsc & XUSBPS_OTGSC_BSV_MASK) ? 1U : 0U),
             (unsigned long)((service.last_otgsc & XUSBPS_OTGSC_BSE_MASK) ? 1U : 0U),
             (unsigned long)((service.last_otgsc & XUSBPS_OTGSC_OT_MASK) ? 1U : 0U),
             (unsigned long)((service.last_otgsc & XUSBPS_OTGSC_DP_MASK) ? 1U : 0U),
             (unsigned long)((service.last_otgsc & XUSBPS_OTGSC_IDPU_MASK) ? 1U : 0U),
             (unsigned long)((service.last_otgsc & XUSBPS_OTGSC_1MST_MASK) ? 1U : 0U));
    uart_puts(uart_base, line);

    snprintf(line, sizeof(line),
             "phy: usb0clk=0x%08lX usb1clk=0x%08lX rst=0x%08lX\r\n",
             (unsigned long)service.last_slcr_usb0_clk_ctrl,
             (unsigned long)service.last_slcr_usb1_clk_ctrl,
             (unsigned long)service.last_slcr_usb_rst_ctrl);
    uart_puts(uart_base, line);

    snprintf(line, sizeof(line),
             "mio28-31: 0x%08lX 0x%08lX 0x%08lX 0x%08lX\r\n",
             (unsigned long)service.last_usb0_mio[0],
             (unsigned long)service.last_usb0_mio[1],
             (unsigned long)service.last_usb0_mio[2],
             (unsigned long)service.last_usb0_mio[3]);
    uart_puts(uart_base, line);

    snprintf(line, sizeof(line),
             "mio32-39: 0x%08lX 0x%08lX 0x%08lX 0x%08lX 0x%08lX 0x%08lX 0x%08lX 0x%08lX\r\n",
             (unsigned long)service.last_usb0_mio[4],
             (unsigned long)service.last_usb0_mio[5],
             (unsigned long)service.last_usb0_mio[6],
             (unsigned long)service.last_usb0_mio[7],
             (unsigned long)service.last_usb0_mio[8],
             (unsigned long)service.last_usb0_mio[9],
             (unsigned long)service.last_usb0_mio[10],
             (unsigned long)service.last_usb0_mio[11]);
    uart_puts(uart_base, line);

    snprintf(line, sizeof(line),
             "ulpi: view=0x%08lX\r\n",
             (unsigned long)service.last_ulpi_view);
    uart_puts(uart_base, line);

    snprintf(line, sizeof(line),
             "epregs: stat=0x%08lX prime=0x%08lX ready=0x%08lX comp=0x%08lX epcr0=0x%08lX epcr1=0x%08lX\r\n",
             (unsigned long)service.last_epstat,
             (unsigned long)service.last_epprime,
             (unsigned long)service.last_eprdy,
             (unsigned long)service.last_epcomplete,
             (unsigned long)service.last_epcr0,
             (unsigned long)service.last_epcr1);
    uart_puts(uart_base, line);

    snprintf(line, sizeof(line),
             "ep1inq: dqh=0x%08lX cfg=0x%08lX cptr=0x%08lX qnext=0x%08lX qtok=0x%08lX dtds=0x%08lX head=0x%08lX tail=0x%08lX\r\n",
             (unsigned long)service.ep1in_dqh,
             (unsigned long)service.ep1in_dqh_cfg,
             (unsigned long)service.ep1in_dqh_cptr,
             (unsigned long)service.ep1in_dqh_next,
             (unsigned long)service.ep1in_dqh_token,
             (unsigned long)service.ep1in_dtds,
             (unsigned long)service.ep1in_head,
             (unsigned long)service.ep1in_tail);
    uart_puts(uart_base, line);

    snprintf(line, sizeof(line),
             "ep1intd: hnext=0x%08lX htok=0x%08lX hbuf=0x%08lX tnext=0x%08lX ttok=0x%08lX tbuf=0x%08lX req=%lu txed=%lu buf=0x%08lX tact=%lu thalt=%lu terr=%lu tlen=%lu\r\n",
             (unsigned long)service.ep1in_head_next,
             (unsigned long)service.ep1in_head_token,
             (unsigned long)service.ep1in_head_buf,
             (unsigned long)service.ep1in_tail_next,
             (unsigned long)service.ep1in_tail_token,
             (unsigned long)service.ep1in_tail_buf,
             (unsigned long)service.ep1in_requested_bytes,
             (unsigned long)service.ep1in_bytes_txed,
             (unsigned long)service.ep1in_buffer_ptr,
             (unsigned long)((service.ep1in_tail_token & 0x00000080U) ? 1U : 0U),
             (unsigned long)((service.ep1in_tail_token & 0x00000040U) ? 1U : 0U),
             (unsigned long)((service.ep1in_tail_token & 0x00000028U) ? 1U : 0U),
             (unsigned long)((service.ep1in_tail_token & 0x7FFF0000U) >> 16));
    uart_puts(uart_base, line);

    snprintf(line, sizeof(line),
             "out: pkts=%llu fast=%llu queued=%llu proc=%llu drops=%llu q_hi=%lu q_now=%lu\r\n",
             (unsigned long long)service.out_packets,
             (unsigned long long)service.fast_packets,
             (unsigned long long)service.queued_packets,
             (unsigned long long)service.processed_packets,
             (unsigned long long)service.queue_drops,
             (unsigned long)service.queue_high_water,
             (unsigned long)service.last_queue_depth);
    uart_puts(uart_base, line);

    snprintf(line, sizeof(line),
             "out bytes: rx=%llu KiB fast=%llu KiB queued=%llu KiB proc=%llu KiB\r\n",
             (unsigned long long)(service.out_bytes / 1024ULL),
             (unsigned long long)(service.fast_bytes / 1024ULL),
             (unsigned long long)(service.queued_bytes / 1024ULL),
             (unsigned long long)(service.processed_bytes / 1024ULL));
    uart_puts(uart_base, line);

    snprintf(line, sizeof(line),
             "cbw: pkts=%llu phase=%lu rxleft=%lu len=%lu sig=0x%08lX tag=0x%08lX xfer=%lu op=0x%02lX\r\n",
             (unsigned long long)scsi.cbw_packets,
             (unsigned long)scsi.phase,
             (unsigned long)scsi.rx_bytes_left,
             (unsigned long)scsi.last_cbw_len,
             (unsigned long)scsi.last_cbw_signature,
             (unsigned long)scsi.last_cbw_tag,
             (unsigned long)scsi.last_cbw_transfer_len,
             (unsigned long)scsi.last_scsi_opcode);
    uart_puts(uart_base, line);

    snprintf(line, sizeof(line),
             "cbw2: flags=0x%02lX lun=%lu cblen=%lu cmds=%llu tur=%llu inq=%llu cap=%llu mode=%llu sense=%llu r10=%llu w10=%llu unhandled=%llu\r\n",
             (unsigned long)scsi.last_cbw_flags,
             (unsigned long)scsi.last_cbw_lun,
             (unsigned long)scsi.last_cbw_cb_len,
             (unsigned long long)scsi.scsi_cmds,
             (unsigned long long)scsi.cmd_test_unit_ready,
             (unsigned long long)scsi.cmd_inquiry,
             (unsigned long long)scsi.cmd_read_capacity,
             (unsigned long long)scsi.cmd_mode_sense,
             (unsigned long long)scsi.cmd_request_sense,
             (unsigned long long)scsi.cmd_read10,
             (unsigned long long)scsi.cmd_write10,
             (unsigned long long)scsi.cmd_unhandled);
    uart_puts(uart_base, line);

    snprintf(line, sizeof(line),
             "cmd2: getcap=%llu medrem=%llu verify=%llu sync=%llu start=%llu format=%llu modesel=%llu\r\n",
             (unsigned long long)scsi.cmd_get_cap_list,
             (unsigned long long)scsi.cmd_medium_removal,
             (unsigned long long)scsi.cmd_verify,
             (unsigned long long)scsi.cmd_sync_cache,
             (unsigned long long)scsi.cmd_start_stop,
             (unsigned long long)scsi.cmd_format,
             (unsigned long long)scsi.cmd_mode_select);
    uart_puts(uart_base, line);

    snprintf(line, sizeof(line),
             "ep1in: sends=%llu fail=%llu data=%llu datafail=%llu csw=%llu lastlen=%lu lastst=%ld datalen=%lu datast=%ld residue=%lu status=%lu\r\n",
             (unsigned long long)scsi.ep1_in_sends,
             (unsigned long long)scsi.ep1_in_failures,
             (unsigned long long)scsi.data_in_sends,
             (unsigned long long)scsi.data_in_failures,
             (unsigned long long)scsi.csw_sends,
             (unsigned long)scsi.last_ep1_send_len,
             (long)scsi.last_ep1_send_status,
             (unsigned long)scsi.last_data_in_len,
             (long)scsi.last_data_in_status,
             (unsigned long)scsi.last_status_residue,
             (unsigned long)scsi.last_status_code);
    uart_puts(uart_base, line);

    snprintf(line, sizeof(line),
             "scsi write: cmds=%lu fail=%lu async=%lu ovf=%lu pend=%lu/%lu maxpend=%lu req=%llu MiB recv=%llu MiB ok=%llu MiB max=%lu blk\r\n",
             (unsigned long)scsi.write_cmds,
             (unsigned long)scsi.write_failures,
             (unsigned long)scsi.write_async_failures,
             (unsigned long)scsi.write_overflows,
             (unsigned long)scsi.write_pending_slots,
             (unsigned long)scsi.write_pipe_depth,
             (unsigned long)scsi.max_write_pending_slots,
             (unsigned long long)(scsi.write_requested_bytes / 1048576ULL),
             (unsigned long long)(scsi.write_received_bytes / 1048576ULL),
             (unsigned long long)(scsi.write_committed_bytes / 1048576ULL),
             (unsigned long)scsi.max_write_blocks);
    uart_puts(uart_base, line);
    print_usb_rate(uart_base, "write usb-rx",
                   scsi.write_committed_bytes,
                   scsi.write_rx_ticks,
                   scsi.last_write_rx_ticks,
                   scsi.max_write_rx_ticks);
    print_usb_rate(uart_base, "write total",
                   scsi.write_committed_bytes,
                   scsi.write_total_ticks,
                   scsi.last_write_total_ticks,
                   scsi.max_write_total_ticks);

    snprintf(line, sizeof(line),
             "scsi read: cmds=%lu fail=%lu bytes=%llu MiB max=%lu blk\r\n",
             (unsigned long)scsi.read_cmds,
             (unsigned long)scsi.read_failures,
             (unsigned long long)(scsi.read_bytes / 1048576ULL),
             (unsigned long)scsi.max_read_blocks);
    uart_puts(uart_base, line);
    print_usb_rate(uart_base, "read cmd",
                   scsi.read_bytes,
                   scsi.read_ticks,
                   scsi.last_read_ticks,
                   scsi.max_read_ticks);

    snprintf(line, sizeof(line),
             "sd write: ops=%lu fail=%lu bytes=%llu MiB max=%lu blk\r\n",
             (unsigned long)sd.write_ops,
             (unsigned long)sd.write_failures,
             (unsigned long long)(sd.write_bytes / 1048576ULL),
             (unsigned long)sd.max_write_blocks);
    uart_puts(uart_base, line);
    print_usb_rate(uart_base, "sd write",
                   sd.write_bytes,
                   sd.write_ticks,
                   sd.last_write_ticks,
                   sd.max_write_ticks);

    snprintf(line, sizeof(line),
             "sd read: ops=%lu fail=%lu bytes=%llu MiB max=%lu blk\r\n",
             (unsigned long)sd.read_ops,
             (unsigned long)sd.read_failures,
             (unsigned long long)(sd.read_bytes / 1048576ULL),
             (unsigned long)sd.max_read_blocks);
    uart_puts(uart_base, line);
    print_usb_rate(uart_base, "sd read",
                   sd.read_bytes,
                   sd.read_ticks,
                   sd.last_read_ticks,
                   sd.max_read_ticks);
}

static void reset_usb_storage_perf(void)
{
    usb_storage_service_reset_stats();
    XUsbPs_StorageResetStats();
    usb_storage_reset_backend_stats();
}

void uart_control_init(uart_control_t *control, uint32_t control_uart_base, uint32_t debug_uart_base)
{
    if (control == NULL) {
        return;
    }

    memset(control, 0, sizeof(*control));
    control->control_uart_base = control_uart_base;
    control->debug_uart_base = debug_uart_base;
}

void uart_control_print_help(const uart_control_t *control, const uart_control_ops_t *ops)
{
    (void)ops;

    uart_puts(control->control_uart_base, "\r\n[text_ui_test] Control UART\r\n");
    uart_puts(control->control_uart_base, "  W/S : move menu\r\n");
    uart_puts(control->control_uart_base, "  A/D : page-specific adjust\r\n");
    uart_puts(control->control_uart_base, "  E   : page action\r\n");
    uart_puts(control->control_uart_base, "  L   : cycle scanlines\r\n");
    uart_puts(control->control_uart_base, "  M   : toggle config menu\r\n");
    uart_puts(control->control_uart_base, "  T   : page toggle\r\n");
    uart_puts(control->control_uart_base, "  Tab/Shift-Tab/PgUp/PgDn/Space/Esc and arrows work in config menu\r\n");
    uart_puts(control->control_uart_base, "  :<cmd> then ENTER for command mode\r\n");
    uart_puts(control->control_uart_base, "  ?   : print this help\r\n");
    uart_puts(control->control_uart_base, "Commands:\r\n");
    uart_puts(control->control_uart_base, "  help | status\r\n");
    uart_puts(control->control_uart_base, "  reboot | reset\r\n");
    uart_puts(control->control_uart_base, "  mrd <addr> [count]\r\n");
    uart_puts(control->control_uart_base, "  nav <up|down|left|right|e|toggle|scanlines|tab|shift-tab|pgup|pgdn|space|esc|menu>\r\n");
    uart_puts(control->control_uart_base, "  vsync <on|off|toggle>\r\n");
    uart_puts(control->control_uart_base, "  scanlines <off|light|medium|strong|toggle|status>\r\n");
    uart_puts(control->control_uart_base, "  textmono <fg> <bg> | textmono status\r\n");
    uart_puts(control->control_uart_base, "  mono <off|white|amber|green|status>\r\n");
    uart_puts(control->control_uart_base, "  audio status\r\n");
    uart_puts(control->control_uart_base, "  audio <on|off>\r\n");
    uart_puts(control->control_uart_base, "  audio mute <on|off|toggle>\r\n");
    uart_puts(control->control_uart_base, "  audio tone <hz>\r\n");
    uart_puts(control->control_uart_base, "  audio amp <0xHEX|DEC>\r\n");
    uart_puts(control->control_uart_base, "  sd reset | sd dumpblk <block> [count]\r\n");
    uart_puts(control->control_uart_base, "  disk2 status | disk2 scan [d1|d2] [all] | disk2 wozscan [d1|d2] [all] | disk2 underruns\r\n");
    uart_puts(control->control_uart_base, "  disk2 wozwrite d1|d2 <on|off|status>\r\n");
    uart_puts(control->control_uart_base, "  rtc get\r\n");
    uart_puts(control->control_uart_base, "  rtc set YYYY-MM-DD HH:MM:SS\r\n");
    uart_puts(control->control_uart_base, "  dma hold <on|off>\r\n");
    uart_puts(control->control_uart_base, "  dma status\r\n");
    uart_puts(control->control_uart_base, "  dma blocks\r\n");
    uart_puts(control->control_uart_base, "  dma tail\r\n");
    uart_puts(control->control_uart_base, "  dma peek <addr> [count]\r\n");
    uart_puts(control->control_uart_base, "  dma reset\r\n");
    uart_puts(control->control_uart_base, "  dma probe [addr] [pattern] [count]\r\n");
    uart_puts(control->control_uart_base, "  dma trace <status|arm [addr|release [addr]]|off|clear|dump [start] [count]>\r\n");
    uart_puts(control->control_uart_base, "  comp uncap <on|off> (bypass vblank pacing to measure raw comp fps)\r\n");
    uart_puts(control->control_uart_base, "  usb status | usb resetstats\r\n");
    uart_puts(control->control_uart_base, "  usb1 [status|start|stop]\r\n");
    uart_puts(control->control_uart_base, "  sdd [status|on|off] (USB0 bus-event stream for SuperDuperDisplay)\r\n");
    uart_puts(control->control_uart_base, "  z80 [status|on|off|reset|budget <tstates>|wall <us>|dump <hex-addr> [len]] (Applicard slot 5)\r\n");
}

static uart_control_event_t process_smartport_command(
    uart_control_t *control,
    const uart_control_ops_t *ops,
    int argc,
    char **argv
)
{
    uart_control_event_t event;
    static uint8_t disk_buf[512];

    memset(&event, 0, sizeof(event));

    if (argc < 2) {
        uart_puts(control->control_uart_base,
                  "usage: sd <reset|dumpblk>\r\n");
        return event;
    }

    if (str_ieq(argv[1], "reset")) {
        int rc;
        if (ops->smartport_reset_media == NULL) {
            uart_puts(control->control_uart_base, "sd reset: unavailable\r\n");
            return event;
        }
        rc = ops->smartport_reset_media(ops->ctx);
        if (rc == 0) {
            uart_puts(control->control_uart_base, "sd reset: mounted current SmartPort image\r\n");
        } else {
            char line[64];
            snprintf(line, sizeof(line), "sd reset: failed rc=%d\r\n", rc);
            uart_puts(control->control_uart_base, line);
        }
        return event;
    }

    if (str_ieq(argv[1], "dumpblk")) {
        uint32_t block_num;
        uint32_t count = 64U;
        uint32_t actual = 0U;
        int rc;

        if (argc < 3 ||
            parse_u32(argv[2], &block_num) != 0 ||
            (argc >= 4 && parse_u32(argv[3], &count) != 0)) {
            uart_puts(control->control_uart_base, "usage: sd dumpblk <block> [count]\r\n");
            return event;
        }
        if (count == 0U || count > sizeof(disk_buf)) {
            count = sizeof(disk_buf);
        }
        if (ops->smartport_read_block == NULL) {
            uart_puts(control->control_uart_base, "sd dumpblk: unavailable\r\n");
            return event;
        }

        rc = ops->smartport_read_block(ops->ctx, block_num, disk_buf, count, &actual);
        if (rc != 0) {
            char line[64];
            snprintf(line, sizeof(line), "sd dumpblk: read failed rc=%d\r\n", rc);
            uart_puts(control->control_uart_base, line);
            return event;
        }
        uart_control_dump_bytes(control->control_uart_base, 0U, disk_buf, actual);
        return event;
    }

    uart_puts(control->control_uart_base,
              "usage: sd <reset|dumpblk>\r\n");
    return event;
}

static uart_control_event_t process_disk2_command(
    uart_control_t *control,
    const uart_control_ops_t *ops,
    int argc,
    char **argv
)
{
    uart_control_event_t event;
    char line[320];
    uint32_t pl_status;
    uint32_t track_info;
    uint32_t write_info;

    (void)ops;
    memset(&event, 0, sizeof(event));

    if (argc < 2 ||
        (!str_ieq(argv[1], "status") &&
         !str_ieq(argv[1], "scan") &&
         !str_ieq(argv[1], "wozscan") &&
         !str_ieq(argv[1], "verify") &&
         !str_ieq(argv[1], "wozwrite") &&
         !str_ieq(argv[1], "underruns"))) {
        uart_puts(control->control_uart_base,
                  "usage: disk2 status | disk2 scan [d1|d2] [all] | disk2 wozscan [d1|d2] [all] | disk2 underruns | disk2 wozwrite d1|d2 <on|off|status>\r\n");
        return event;
    }

    if (str_ieq(argv[1], "verify")) {
        uint32_t first = 0U;
        uint32_t bad = 0U;
        const int rc = disk2_service_verify_staged(&first, &bad);
        if (rc < 0) {
            snprintf(line, sizeof(line),
                     "disk2 verify: unavailable (rc=%d)%s", rc, "\r\n");
        } else if (rc == 0) {
            snprintf(line, sizeof(line),
                     "disk2 verify: staged track MATCHES DDR source%s",
                     "\r\n");
        } else {
            snprintf(line, sizeof(line),
                     "disk2 verify: %lu MISMATCHED bytes, first at +0x%04lX%s",
                     (unsigned long)bad, (unsigned long)first, "\r\n");
        }
        uart_puts(control->control_uart_base, line);
        return event;
    }

    if (str_ieq(argv[1], "underruns")) {
        snprintf(line, sizeof(line),
                 "disk2 underruns: %lu\r\n",
                 (unsigned long)REG_READ(0x4006000CU));
        uart_puts(control->control_uart_base, line);
        return event;
    }

    if (str_ieq(argv[1], "wozwrite")) {
        disk2_image_info_t info;
        uint8_t drive;
        uint8_t enable;
        int rc = 0;

        if (argc < 4 || parse_disk2_drive(argv[2], &drive) != 0) {
            uart_puts(control->control_uart_base,
                      "usage: disk2 wozwrite d1|d2 <on|off|status>\r\n");
            return event;
        }

        if (str_ieq(argv[3], "status")) {
            rc = disk2_service_get_image_info(drive, &info);
            if (rc != 0 || info.present == 0U || info.format != DISK2_IMAGE_WOZ) {
                snprintf(line, sizeof(line),
                         "disk2 wozwrite d%u: unavailable\r\n",
                         (unsigned)drive + 1U);
            } else {
                snprintf(line, sizeof(line),
                         "disk2 wozwrite d%u: %s effective=%s\r\n",
                         (unsigned)drive + 1U,
                         disk2_service_get_woz_write_enable(drive) ? "on" : "off",
                         info.read_only ? "ro" : "rw");
            }
            uart_puts(control->control_uart_base, line);
            return event;
        }

        if (str_ieq(argv[3], "on")) {
            enable = 1U;
        } else if (str_ieq(argv[3], "off")) {
            enable = 0U;
        } else {
            uart_puts(control->control_uart_base,
                      "usage: disk2 wozwrite d1|d2 <on|off|status>\r\n");
            return event;
        }

        rc = disk2_service_set_woz_write_enable(drive, enable);
        if (rc == 0) {
            snprintf(line, sizeof(line),
                     "disk2 wozwrite d%u: %s\r\n",
                     (unsigned)drive + 1U,
                     enable ? "on" : "off");
        } else {
            snprintf(line, sizeof(line),
                     "disk2 wozwrite d%u: failed rc=%d\r\n",
                     (unsigned)drive + 1U,
                     rc);
        }
        uart_puts(control->control_uart_base, line);
        return event;
    }

    if (str_ieq(argv[1], "scan")) {
        disk2_track_scan_t scan;
        uint8_t drive;
        int all_tracks;

        if (parse_disk2_scan_args(argc, argv, &drive, &all_tracks) != 0) {
            uart_puts(control->control_uart_base, "usage: disk2 scan [d1|d2] [all]\r\n");
            return event;
        }

        if (all_tracks != 0) {
            disk2_image_info_t info;

            if (disk2_service_get_image_info(drive, &info) != 0 ||
                info.present == 0U) {
                snprintf(line,
                         sizeof(line),
                         "disk2 scan all: D%u is empty\r\n",
                         (unsigned)drive + 1U);
                uart_puts(control->control_uart_base, line);
                return event;
            }
            for (uint32_t track = 0U; track < info.track_count; ++track) {
                if (disk2_service_scan_file_track(drive, (uint8_t)track, &scan) != 0) {
                    snprintf(line, sizeof(line),
                             "disk2 scan d%u t%02lu: read failed\r\n",
                             (unsigned)drive + 1U,
                             (unsigned long)track);
                } else {
                    snprintf(line,
                             sizeof(line),
                             "disk2 scan d%u t%02lu: addr16=%lu first=0x%03lX sec=%u hdrtrk=%u ck=%u addr13=%lu data=%lu\r\n",
                             (unsigned)drive + 1U,
                             (unsigned long)track,
                             (unsigned long)scan.addr16_count,
                             (unsigned long)scan.first_addr16,
                             scan.first_addr16_valid ? (unsigned)scan.first_addr16_sector : 255U,
                             scan.first_addr16_valid ? (unsigned)scan.first_addr16_track : 255U,
                             (unsigned)scan.first_addr16_checksum_ok,
                             (unsigned long)scan.addr13_count,
                             (unsigned long)scan.data_count);
                }
                uart_puts(control->control_uart_base, line);
            }
            return event;
        }
        if (disk2_service_scan_loaded_track(&scan) != 0) {
            uart_puts(control->control_uart_base, "disk2 scan: no loaded NIB track\r\n");
            return event;
        }
        snprintf(line,
                 sizeof(line),
                 "disk2 scan: d%u t%u q%u len=%lu addr16(D5 AA 96)=%lu first=0x%03lX sec=%u hdrtrk=%u ck=%u addr13(D5 AA B5)=%lu first=0x%03lX data(D5 AA AD)=%lu first=0x%03lX\r\n",
                 (unsigned)scan.drive,
                 (unsigned)scan.track,
                 (unsigned)scan.qtrack,
                 (unsigned long)scan.length,
                 (unsigned long)scan.addr16_count,
                 (unsigned long)scan.first_addr16,
                 scan.first_addr16_valid ? (unsigned)scan.first_addr16_sector : 255U,
                 scan.first_addr16_valid ? (unsigned)scan.first_addr16_track : 255U,
                 (unsigned)scan.first_addr16_checksum_ok,
                 (unsigned long)scan.addr13_count,
                 (unsigned long)scan.first_addr13,
                 (unsigned long)scan.data_count,
                 (unsigned long)scan.first_data);
        uart_puts(control->control_uart_base, line);
        return event;
    }

    if (str_ieq(argv[1], "wozscan")) {
        disk2_track_scan_t scan;
        uint8_t drive;
        int all_tracks;

        if (parse_disk2_scan_args(argc, argv, &drive, &all_tracks) != 0) {
            uart_puts(control->control_uart_base, "usage: disk2 wozscan [d1|d2] [all]\r\n");
            return event;
        }

        if (all_tracks != 0) {
            disk2_image_info_t info;

            if (disk2_service_get_image_info(drive, &info) != 0 ||
                info.present == 0U ||
                info.format != DISK2_IMAGE_WOZ) {
                snprintf(line,
                         sizeof(line),
                         "disk2 wozscan all: D%u is not WOZ\r\n",
                         (unsigned)drive + 1U);
                uart_puts(control->control_uart_base, line);
                return event;
            }
            for (uint32_t track = 0U; track < info.track_count; ++track) {
                if (disk2_service_wozscan_file_track(drive, (uint8_t)track, &scan) != 0) {
                    snprintf(line, sizeof(line),
                             "disk2 wozscan d%u t%02lu: read failed\r\n",
                             (unsigned)drive + 1U,
                             (unsigned long)track);
                } else {
                    snprintf(line,
                             sizeof(line),
                             "disk2 wozscan d%u t%02lu: decoded=%lu addr16=%lu first=0x%03lX sec=%u hdrtrk=%u ck=%u addr13=%lu data=%lu\r\n",
                             (unsigned)drive + 1U,
                             (unsigned long)track,
                             (unsigned long)scan.length,
                             (unsigned long)scan.addr16_count,
                             (unsigned long)scan.first_addr16,
                             scan.first_addr16_valid ? (unsigned)scan.first_addr16_sector : 255U,
                             scan.first_addr16_valid ? (unsigned)scan.first_addr16_track : 255U,
                             (unsigned)scan.first_addr16_checksum_ok,
                             (unsigned long)scan.addr13_count,
                             (unsigned long)scan.data_count);
                }
                uart_puts(control->control_uart_base, line);
            }
            return event;
        }
        if (disk2_service_wozscan_loaded_track(&scan) != 0) {
            uart_puts(control->control_uart_base, "disk2 wozscan: no loaded WOZ track\r\n");
            return event;
        }
        snprintf(line,
                 sizeof(line),
                 "disk2 wozscan: d%u t%u q%u decoded=%lu addr16=%lu first=0x%03lX sec=%u hdrtrk=%u ck=%u addr13=%lu first=0x%03lX data=%lu first=0x%03lX\r\n",
                 (unsigned)scan.drive,
                 (unsigned)scan.track,
                 (unsigned)scan.qtrack,
                 (unsigned long)scan.length,
                 (unsigned long)scan.addr16_count,
                 (unsigned long)scan.first_addr16,
                 scan.first_addr16_valid ? (unsigned)scan.first_addr16_sector : 255U,
                 scan.first_addr16_valid ? (unsigned)scan.first_addr16_track : 255U,
                 (unsigned)scan.first_addr16_checksum_ok,
                 (unsigned long)scan.addr13_count,
                 (unsigned long)scan.first_addr13,
                 (unsigned long)scan.data_count,
                 (unsigned long)scan.first_data);
        uart_puts(control->control_uart_base, line);
        return event;
    }

    for (uint8_t drive = 0U; drive < DISK2_DRIVE_COUNT; ++drive) {
        disk2_image_info_t info;
        const char *path = disk2_service_get_image_path(drive);

        if (disk2_service_get_image_info(drive, &info) != 0) {
            continue;
        }
        snprintf(line,
                 sizeof(line),
                 "disk2 d%u: %s %s %s wozwrite=%s size=%lu tracks=%lu blocks=%lu path=%s\r\n",
                 (unsigned)drive + 1U,
                 info.present ? "present" : "empty",
                 disk2_service_format_name(info.format),
                 info.read_only ? "ro" : "rw",
                 (info.format == DISK2_IMAGE_WOZ && disk2_service_get_woz_write_enable(drive)) ?
                    "on" : "off",
                 (unsigned long)info.file_size,
                 (unsigned long)info.track_count,
                 (unsigned long)info.logical_blocks,
                 (path[0] != '\0') ? path : "-");
        uart_puts(control->control_uart_base, line);
    }

    pl_status = REG_READ(0x40060000U);
    snprintf(line,
             sizeof(line),
             "disk2 pl: status=0x%08lX psram=0x%08lX lastio=0x%02lX iocount=%lu underruns=%lu\r\n",
             (unsigned long)pl_status,
             (unsigned long)REG_READ(0x40060008U),
             (unsigned long)(REG_READ(0x40060010U) & 0xFFU),
             (unsigned long)REG_READ(0x40060014U),
             (unsigned long)REG_READ(0x4006000CU));
    uart_puts(control->control_uart_base, line);

    track_info = REG_READ(0x40060018U);
    snprintf(line,
             sizeof(line),
             "disk2 track: loaded=%u match=%u cur=d%lu q%lu load=d%lu q%lu len=%lu pos=%lu reads=%lu\r\n",
             (unsigned)(track_info & 1U),
             (unsigned)((track_info >> 1) & 1U),
             (unsigned long)(((track_info >> 16) & 1U) + 1U),
             (unsigned long)((track_info >> 8) & 0xFFU),
             (unsigned long)(((track_info >> 20) & 1U) + 1U),
             (unsigned long)((track_info >> 24) & 0xFFU),
             (unsigned long)REG_READ(0x4006001CU),
             (unsigned long)REG_READ(0x40060028U),
             (unsigned long)REG_READ(0x4006002CU));
    uart_puts(control->control_uart_base, line);

    write_info = REG_READ(0x40060030U);
    snprintf(line,
             sizeof(line),
             "disk2 write: dirty=%u d%lu q%lu writes=%lu latch=0x%02lX\r\n",
             (unsigned)(write_info & 1U),
             (unsigned long)(((write_info >> 16) & 1U) + 1U),
             (unsigned long)((write_info >> 8) & 0xFFU),
             (unsigned long)REG_READ(0x40060034U),
             (unsigned long)((write_info >> 24) & 0xFFU));
    uart_puts(control->control_uart_base, line);

    snprintf(line,
             sizeof(line),
             "disk2 state: en=%u motor=%u spin=%u drive=%u phase=0x%lX q6=%u q7=%u\r\n",
             (unsigned)((pl_status >> 2) & 1U),
             (unsigned)((pl_status >> 3) & 1U),
             (unsigned)((pl_status >> 11) & 1U),
             (unsigned)((pl_status >> 4) & 1U) + 1U,
             (unsigned long)((pl_status >> 5) & 0xFU),
             (unsigned)((pl_status >> 9) & 1U),
             (unsigned)((pl_status >> 10) & 1U));
    uart_puts(control->control_uart_base, line);
    return event;
}



static uart_control_event_t process_command(
    uart_control_t *control,
    const uart_control_ops_t *ops,
    char *cmd_line
)
{
    uart_control_event_t event;
    char *argv[10];
    int argc;
    uart_control_snapshot_t snap;

    memset(&event, 0, sizeof(event));

    argc = split_tokens(cmd_line, argv, (int)(sizeof(argv) / sizeof(argv[0])));
    if (argc == 0) {
        uart_puts(control->control_uart_base, "cmd: empty\r\n");
        return event;
    }

    if (str_ieq(argv[0], "help")) {
        uart_control_print_help(control, ops);
        return event;
    }

    if (str_ieq(argv[0], "status")) {
        print_snapshot(control, ops);
        return event;
    }

    if (str_ieq(argv[0], "usb")) {
        if (argc < 2 || str_ieq(argv[1], "status")) {
            print_usb_storage_perf(control);
            return event;
        }
        if (str_ieq(argv[1], "resetstats")) {
            reset_usb_storage_perf();
            uart_puts(control->control_uart_base, "usb storage perf: reset\r\n");
            return event;
        }
        uart_puts(control->control_uart_base, "usage: usb status | usb resetstats\r\n");
        return event;
    }

    if (str_ieq(argv[0], "sdd")) {
        if (argc < 2 || str_ieq(argv[1], "status")) {
            usb_sdd_service_print_status(control->control_uart_base);
            if (g_sdd_config_menu != NULL) {
                uart_puts(control->control_uart_base,
                          g_sdd_config_menu->sdd_stream_enabled ?
                              "sdd: persisted setting = ON (boots as SDD stream)\r\n" :
                              "sdd: persisted setting = OFF (USB0 detached by default)\r\n");
            }
            return event;
        }
        if (str_ieq(argv[1], "on") || str_ieq(argv[1], "off")) {
            uint8_t enable = str_ieq(argv[1], "on") ? 1U : 0U;

            /* Route through the config menu so the persisted setting can
             * never disagree with the live personality -- a stale saved
             * "on" would silently bring USB0 up as an FT601 on the next
             * boot and USB0 would unexpectedly enumerate on the host. */
            if (g_sdd_config_menu != NULL) {
                config_menu_set_sdd_stream(g_sdd_config_menu, enable);
            } else if (enable) {
                (void)usb_sdd_service_start();
            } else {
                usb_sdd_service_stop();
            }
            uart_puts(control->control_uart_base,
                      enable ? "sdd: on (USB0 -> SDD stream, saved)\r\n"
                             : "sdd: off (USB0 detached, saved)\r\n");
            return event;
        }
        uart_puts(control->control_uart_base, "usage: sdd [status|on|off]\r\n");
        return event;
    }

    if (str_ieq(argv[0], "z80")) {
        if (argc < 2 || str_ieq(argv[1], "status")) {
            applicard_service_uart_status(control->control_uart_base);
            if (g_sdd_config_menu != NULL) {
                uart_puts(control->control_uart_base,
                          g_sdd_config_menu->applicard_slot5_enabled != 0U ?
                              "z80: persisted setting = ON\r\n" :
                              "z80: persisted setting = OFF\r\n");
            }
            return event;
        }
        if (str_ieq(argv[1], "on") || str_ieq(argv[1], "off")) {
            uint8_t enable = str_ieq(argv[1], "on") ? 1U : 0U;

            /* Route through the config menu so the persisted setting and
             * the live slot-5 enable stay in lockstep (same rationale as
             * the sdd command). */
            if (g_sdd_config_menu != NULL) {
                config_menu_set_applicard_enabled(g_sdd_config_menu, enable);
            } else {
                applicard_service_set_enabled(enable);
            }
            uart_puts(control->control_uart_base,
                      applicard_service_is_enabled() != 0U ?
                          "z80: on (Applicard in slot 5, saved)\r\n" :
                          "z80: off (slot 5 empty, saved)\r\n");
            return event;
        }
        if (str_ieq(argv[1], "reset")) {
            applicard_service_request_reset();
            uart_puts(control->control_uart_base, "z80: reset\r\n");
            return event;
        }
        if (str_ieq(argv[1], "budget") && argc >= 3) {
            applicard_service_set_budget(
                (uint32_t)strtoul(argv[2], NULL, 10));
            uart_puts(control->control_uart_base, "z80: budget set\r\n");
            return event;
        }
        if (str_ieq(argv[1], "wall") && argc >= 3) {
            applicard_service_set_wall_cap(
                (uint32_t)strtoul(argv[2], NULL, 10));
            uart_puts(control->control_uart_base, "z80: wall cap set\r\n");
            return event;
        }
        if (str_ieq(argv[1], "dump") && argc >= 3) {
            uint32_t addr = (uint32_t)strtoul(argv[2], NULL, 16);
            uint32_t len = (argc >= 4) ?
                (uint32_t)strtoul(argv[3], NULL, 16) : 64U;
            applicard_service_uart_dump(control->control_uart_base,
                                        addr, len);
            return event;
        }
        uart_puts(control->control_uart_base,
                  "usage: z80 [status|on|off|reset|budget <tstates>|"
                  "wall <us>|dump <hex-addr> [len]]\r\n");
        return event;
    }

    if (str_ieq(argv[0], "usb1")) {
        if (argc < 2 || str_ieq(argv[1], "status")) {
            usb_hid_service_dump_status(control->control_uart_base);
            return event;
        }
        if (str_ieq(argv[1], "start")) {
            int rc = usb_hid_service_start();
            if (rc == 0) {
                uart_puts(control->control_uart_base, "usb1 start: ok\r\n");
            } else {
                uart_puts(control->control_uart_base, "usb1 start: failed rc=");
                uart_putdec(control->control_uart_base, (uint32_t)(-rc));
                uart_puts(control->control_uart_base, "\r\n");
            }
            return event;
        }
        if (str_ieq(argv[1], "stop")) {
            usb_hid_service_stop();
            uart_puts(control->control_uart_base, "usb1 stop: ok\r\n");
            return event;
        }
        uart_puts(control->control_uart_base, "usage: usb1 [status|start|stop]\r\n");
        return event;
    }

    if (str_ieq(argv[0], "sd")) {
        return process_smartport_command(control, ops, argc, argv);
    }

    if (str_ieq(argv[0], "disk2")) {
        return process_disk2_command(control, ops, argc, argv);
    }

    if (str_ieq(argv[0], "bustail") && argc >= 2 &&
        str_ieq(argv[1], "save")) {
        /* Dump the whole (ideally trapped) ring to SD, oldest event
         * first, for exact-trace replay in simulation. */
        FIL sf;
        UINT bw = 0U;
        uint32_t total = 0U;
        const uint32_t producer =
            *(volatile uint32_t *)(uintptr_t)0x3F030000U;
        char line[64];
        if (f_open(&sf, "0:/bustail.bin",
                   FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
            uart_puts(control->control_uart_base,
                      "bustail save: open failed\r\n");
            return event;
        }
        {
            /* oldest event = producer (the next overwrite target) */
            uint32_t off = producer & ((1U << 19) - 1U);
            uint32_t i;
            for (i = 0U; i < 65536U; ++i) {
                if (f_write(&sf,
                            (const void *)(uintptr_t)(0x3F040000U + off),
                            8U, &bw) != FR_OK || bw != 8U) {
                    break;
                }
                total += 8U;
                off = (off + 8U) & ((1U << 19) - 1U);
            }
        }
        f_close(&sf);
        (void)snprintf(line, sizeof(line),
            "bustail save: %lu bytes -> 0:/bustail.bin\r\n",
            (unsigned long)total);
        uart_puts(control->control_uart_base, line);
        return event;
    }

    if (str_ieq(argv[0], "bustail") && argc >= 3 &&
        str_ieq(argv[1], "find")) {
        /* Scan the (trapped/frozen) ring backward for every access to
         * one address; print back-offsets usable with `bustail N back`
         * for context dumps. */
        const uint32_t producer =
            *(volatile uint32_t *)(uintptr_t)0x3F030000U;
        uint32_t target = 0U;
        uint32_t back;
        uint32_t hits = 0U;
        char line[64];
        if (parse_u32(argv[2], &target) != 0 || target > 0xFFFFU) {
            uart_puts(control->control_uart_base,
                      "usage: bustail find <addr16>\r\n");
            return event;
        }
        for (back = 1U; back < 60000U; ++back) {
            const uint32_t off =
                (producer - (back * 8U)) & ((1U << 19) - 1U);
            const uint32_t lo =
                *(volatile uint32_t *)(uintptr_t)(0x3F040000U + off);
            if (lo == 0U || (lo & 0xFFFFU) != target) {
                continue;
            }
            hits++;
            if (hits <= 24U) {
                (void)snprintf(line, sizeof(line),
                    "back=%lu %c %04lX %02lX %s\r\n",
                    (unsigned long)back,
                    ((lo >> 16) & 1U) ? 'R' : 'W',
                    (unsigned long)(lo & 0xFFFFU),
                    (unsigned long)((lo >> 20) & 0xFFU),
                    ((lo >> 31) & 1U)
                        ? (((lo >> 30) & 1U) ? "AUX" : "cache0")
                        : (((lo >> 29) & 1U) ? "rom" : "main"));
                uart_puts(control->control_uart_base, line);
            }
        }
        (void)snprintf(line, sizeof(line),
            "bustail find: %lu hits\r\n",
            (unsigned long)hits);
        uart_puts(control->control_uart_base, line);
        return event;
    }

    if (str_ieq(argv[0], "bustail") && argc >= 2 &&
        str_ieq(argv[1], "entry")) {
        /* Walk the (ideally trapped/frozen) ring backward from the
         * newest event, find the OLDEST $FFFE vector fetch of the
         * final storm cluster (gaps < 512 events keep a cluster
         * together), and print the events leading into it. */
        const uint32_t producer =
            *(volatile uint32_t *)(uintptr_t)0x3F030000U;
        uint32_t back;
        uint32_t entry_back = 0U;
        uint32_t last_hit = 0U;
        char line[64];

        for (back = 1U; back < 60000U; ++back) {
            const uint32_t off =
                (producer - (back * 8U)) & ((1U << 19) - 1U);
            const uint32_t lo =
                *(volatile uint32_t *)(uintptr_t)(0x3F040000U + off);
            if (lo != 0U && (lo & 0x1FFFFU) == 0x1FFFEU) {
                if (entry_back != 0U && (back - last_hit) > 512U) {
                    break;   /* older FFFE beyond a quiet gap: not the storm */
                }
                entry_back = back;
                last_hit = back;
            }
        }
        if (entry_back == 0U) {
            uart_puts(control->control_uart_base,
                      "bustail entry: no vector fetches found\r\n");
            return event;
        }
        (void)snprintf(line, sizeof(line),
            "storm entry at back=%lu; pre-storm window:\r\n",
            (unsigned long)entry_back);
        uart_puts(control->control_uart_base, line);
        {
            uint32_t i;
            const uint32_t start = entry_back + 96U;
            for (i = start; i + 1U >= entry_back && i >= 1U; --i) {
                const uint32_t off =
                    (producer - (i * 8U)) & ((1U << 19) - 1U);
                const uint32_t lo =
                    *(volatile uint32_t *)(uintptr_t)(0x3F040000U + off);
                if (lo == 0U) {
                    continue;
                }
                (void)snprintf(line, sizeof(line),
                    "%c %04lX %02lX%s\r\n",
                    ((lo >> 16) & 1U) ? 'R' : 'W',
                    (unsigned long)(lo & 0xFFFFU),
                    (unsigned long)((lo >> 20) & 0xFFU),
                    ((lo & 0x20000U) == 0U) ? " !RES" : "");
                uart_puts(control->control_uart_base, line);
                if (i == entry_back) {
                    uart_puts(control->control_uart_base,
                              "--- first storm vector fetch above ---\r\n");
                    break;
                }
            }
        }
        return event;
    }

    if (str_ieq(argv[0], "bustail")) {
        /* Bus-cycle forensics. "bustail on" arms the SDD bus tap in
         * ring-only mode (no USB personality change): the PL then
         * keeps the last ~125 ms of EVERY bus cycle (reads included)
         * in the SDD DDR ring. "bustail [n]" decodes the most recent
         * n events -- point-and-shoot freeze diagnosis: reproduce the
         * hang, then read the loop the 6502 is stuck in. */
        if (argc >= 2 && str_ieq(argv[1], "on")) {
            volatile uint32_t *producer =
                (volatile uint32_t *)(uintptr_t)0x3F030000U;
            REG_WRITE(CARD_CTRL_REG_ADDR(0x50U), 0U);
            REG_WRITE(CARD_CTRL_REG_ADDR(0x51U), 0x3F040000U);
            REG_WRITE(CARD_CTRL_REG_ADDR(0x52U), 19U);
            REG_WRITE(CARD_CTRL_REG_ADDR(0x53U), 0x3F030000U);
            REG_WRITE(CARD_CTRL_REG_ADDR(0x54U), 0U);
            *producer = 0U;
            __asm__ volatile("dsb" ::: "memory");
            REG_WRITE(CARD_CTRL_REG_ADDR(0x55U), 1U);
            REG_WRITE(CARD_CTRL_REG_ADDR(0x50U), 1U);
            g_bustail_armed = 1U;
            g_bustail_trapped = 0U;
            uart_puts(control->control_uart_base,
                      "bustail: tap armed (free-running, traps on IRQ/BRK vectors)\r\n");
            return event;
        }
        {
            const uint32_t producer =
                *(volatile uint32_t *)(uintptr_t)0x3F030000U;
            uint32_t n = 32U;
            uint32_t back = 0U;
            if (argc >= 3) {
                (void)parse_u32(argv[2], &back);
                if (back > 60000U) {
                    back = 60000U;
                }
            }
            if (g_bustail_trapped) {
                uart_puts(control->control_uart_base,
                          "bustail: TRAPPED on vector storm; ring holds pre-storm history\r\n");
            }
            uint32_t i;
            char line[64];
            if (argc >= 2) {
                (void)parse_u32(argv[1], &n);
                if (n == 0U || n > 512U) {
                    n = 32U;
                }
            }
            if (producer == 0U) {
                uart_puts(control->control_uart_base,
                          "bustail: ring empty (run: bustail on)\r\n");
                return event;
            }
            for (i = n; i >= 1U; --i) {
                const uint32_t off =
                    (producer - ((back + i) * 8U)) & ((1U << 19) - 1U);
                const uint32_t lo =
                    *(volatile uint32_t *)(uintptr_t)(0x3F040000U + off);
                const uint32_t hi =
                    *(volatile uint32_t *)(uintptr_t)(0x3F040000U + off + 4U);
                if (lo == 0U && hi == 0U) {
                    continue;    /* gap marker */
                }
                (void)snprintf(line, sizeof(line),
                    "%c %04lX %02lX%s%s",
                    (lo & 0x10000U) ? 'R' : 'W',
                    (unsigned long)(lo & 0xFFFFU),
                    (unsigned long)((lo >> 20) & 0xFFU),
                    (lo & 0x20000U) ? "" : " !RES",
                    "\r\n");
                uart_puts(control->control_uart_base, line);
            }
        }
        return event;
    }

    if (str_ieq(argv[0], "mwr")) {
        uint32_t waddr = 0U;
        uint32_t wval = 0U;
        if (argc < 3 || parse_u32(argv[1], &waddr) != 0 ||
            parse_u32(argv[2], &wval) != 0 || (waddr & 3U) != 0U) {
            uart_puts(control->control_uart_base,
                      "usage: mwr <addr> <value>\r\n");
            return event;
        }
        *(volatile uint32_t *)(uintptr_t)waddr = wval;
        __asm__ __volatile__("dsb" ::: "memory");
        uart_puts(control->control_uart_base, "ok\r\n");
        return event;
    }

    if (str_ieq(argv[0], "bankprobe")) {
        (void)psram_bank_probe(control->control_uart_base);
        return event;
    }

    if (str_ieq(argv[0], "spverify")) {
        (void)smartport_service_verify_crclog(control->control_uart_base);
        return event;
    }

    if (str_ieq(argv[0], "sswatch")) {
        if (argc >= 2 && str_ieq(argv[1], "on")) {
            g_ssw_cons = *(volatile uint32_t *)(uintptr_t)0x3F030000U;
            /* Seed the model from the LIVE tracker so pre-existing
             * state does not count as divergence. */
            g_ssw_model = REG_READ(CARD_CTRL_REG_ADDR(0x02U)) & 0x3FFFU;
            {
                const uint32_t c8 =
                    REG_READ(CARD_CTRL_REG_ADDR(0x65U)) & 0x3FU;
                g_ssw_intcxrom  = (uint8_t)(c8 & 1U);
                g_ssw_slotc3rom = (uint8_t)((c8 >> 1) & 1U);
                g_ssw_intc8rom  = (uint8_t)((c8 >> 2) & 1U);
                g_ssw_c8_slot   = (uint8_t)((c8 >> 3) & 7U);
            }
            g_ssw_lc_prewrite = 0U;
            g_ssw_mismatch_polls = 0U;
            g_ssw_reports = 0U;
            g_ssw_warmup = 0U;
            g_ssw_map_bad = 0U;
            g_ssw_map_reports = 0U;
            g_ssw_on = 1U;
            uart_puts(control->control_uart_base,
                      "sswatch: reference MMU checker ON (needs bustail on)\r\n");
            return event;
        }
        if (argc >= 2 && str_ieq(argv[1], "off")) {
            g_ssw_on = 0U;
            uart_puts(control->control_uart_base, "sswatch: off\r\n");
            return event;
        }
        {
            char line[96];
            (void)snprintf(line, sizeof(line),
                "sswatch: %s model=%04lX live=%04lX reports=%lu mapbad=%lu\r\n",
                g_ssw_on ? "ON" : "off",
                (unsigned long)(g_ssw_model & 0x3FFFU),
                (unsigned long)(REG_READ(CARD_CTRL_REG_ADDR(0x02U)) & 0x3FFFU),
                (unsigned long)g_ssw_reports,
                (unsigned long)g_ssw_map_bad);
            uart_puts(control->control_uart_base, line);
        }
        return event;
    }

    if (str_ieq(argv[0], "machine")) {
        if (argc >= 3 && str_ieq(argv[1], "force")) {
            int mode = -2;
            if (str_ieq(argv[2], "unknown")) {
                mode = (int)CARD_MACHINE_MODE_UNKNOWN;
            } else if (str_ieq(argv[2], "iiplus") || str_ieq(argv[2], "ii")) {
                mode = (int)CARD_MACHINE_MODE_IIPLUS;
            } else if (str_ieq(argv[2], "iie")) {
                mode = (int)CARD_MACHINE_MODE_IIE;
            } else if (str_ieq(argv[2], "iigs") || str_ieq(argv[2], "gs")) {
                mode = (int)CARD_MACHINE_MODE_IIGS;
            }
            if (mode == -2) {
                uart_puts(control->control_uart_base,
                          "usage: machine force <unknown|iiplus|iie|iigs>\r\n");
                return event;
            }
            boot_menu_service_force_machine_mode(mode);
            uart_puts(control->control_uart_base, "machine: forced\r\n");
            return event;
        }
        if (argc >= 2 && str_ieq(argv[1], "auto")) {
            boot_menu_service_force_machine_mode(-1);
            uart_puts(control->control_uart_base,
                      "machine: automatic (boot ROM report)\r\n");
            return event;
        }
        {
            char line[120];
            (void)snprintf(line, sizeof(line),
                "machine: mode=%s (reported id=%u%s) pl_reg=%lu\r\n",
                boot_menu_service_machine_name(),
                (unsigned)boot_menu_service_machine_id(),
                boot_menu_service_machine_forced() ? ", FORCED" : "",
                (unsigned long)REG_READ(CARD_CTRL_MACHINE_MODE_REG));
            uart_puts(control->control_uart_base, line);
        }
        return event;
    }

    if (str_ieq(argv[0], "comp")) {
        if (argc >= 2 && str_ieq(argv[1], "uncap")) {
            if (argc >= 3 && str_ieq(argv[2], "on")) {
                compositor_set_uncapped(1U);
            } else if (argc >= 3 && str_ieq(argv[2], "off")) {
                compositor_set_uncapped(0U);
            } else if (argc >= 3) {
                uart_puts(control->control_uart_base,
                          "usage: comp uncap [on|off]\r\n");
                return event;
            }
            uart_puts(control->control_uart_base,
                      compositor_uncapped() ?
                          "comp uncap: on (bypassing vblank pacing)\r\n" :
                          "comp uncap: off\r\n");
            return event;
        }
        if (argc >= 2 && str_ieq(argv[1], "stats")) {
            char line[160];
            (void)snprintf(line, sizeof(line),
                "comp: total=%luus ui=%luus apple=%luus sync=%luus\r\n",
                (unsigned long)g_compositor_last_total_us,
                (unsigned long)g_compositor_last_ui_us,
                (unsigned long)g_compositor_last_apple_us,
                (unsigned long)g_compositor_last_sync_us);
            uart_puts(control->control_uart_base, line);
            (void)snprintf(line, sizeof(line),
                "comp: published=%lu skipped=%lu apple_drawn=%lu "
                "mode=%s uncap=%u\r\n",
                (unsigned long)g_compositor_frames_published,
                (unsigned long)g_compositor_frames_skipped,
                (unsigned long)g_compositor_apple_frames_drawn,
                (g_compositor_last_apple_mode == 1U) ? "SHR" : "legacy",
                (unsigned)compositor_uncapped());
            uart_puts(control->control_uart_base, line);
            {
                const uint32_t dbg2 = REG_READ(FB_DEBUG2_REG);
                (void)snprintf(line, sizeof(line),
                    "comp: scanout underruns=%lu axi_rd_errs=%lu\r\n",
                    (unsigned long)(dbg2 >> 16),
                    (unsigned long)(dbg2 & 0xFFFFU));
                uart_puts(control->control_uart_base, line);
            }
            return event;
        }
        uart_puts(control->control_uart_base,
                  "usage: comp uncap [on|off] | comp stats\r\n");
        return event;
    }

    if (str_ieq(argv[0], "ss")) {
        uint32_t feat = REG_READ(CARD_CTRL_FEATURE_ENABLE_REG);
        if (argc >= 2 && str_ieq(argv[1], "on")) {
            feat |= CARD_CTRL_FEATURE_SUPERSPRITE_ENABLE_BIT;
            REG_WRITE(CARD_CTRL_FEATURE_ENABLE_REG, feat);
        } else if (argc >= 2 && str_ieq(argv[1], "off")) {
            feat &= ~CARD_CTRL_FEATURE_SUPERSPRITE_ENABLE_BIT;
            REG_WRITE(CARD_CTRL_FEATURE_ENABLE_REG, feat);
        } else if (argc >= 2 && str_ieq(argv[1], "force")) {
            uint8_t on = (uint8_t)(argc >= 3 && str_ieq(argv[2], "on"));
            supersprite_vdp_set_force_active(on);
            uart_puts(control->control_uart_base, on ?
                "ss force: on (overlay forced active + live render)\r\n" :
                "ss force: off\r\n");
            return event;
        } else if (argc >= 2 && str_ieq(argv[1], "dump")) {
            char line[160];
            uint32_t status = REG_READ(CARD_CTRL_SS_STATUS_REG);
            uint32_t lo = REG_READ(CARD_CTRL_SS_REGS_LO_REG);
            uint32_t hi = REG_READ(CARD_CTRL_SS_REGS_HI_REG);
            uint8_t r0 = (uint8_t)lo, r1 = (uint8_t)(lo >> 8);
            uint32_t name_base = (uint32_t)((lo >> 16) & 0x0FU) << 10; /* R2 */
            const char *mode = (r1 & 0x10U) ? "text" :
                               (r1 & 0x08U) ? "multicolor" :
                               (r0 & 0x02U) ? "graphics2" : "graphics1";
            char *p;
            int i;
            (void)snprintf(line, sizeof(line),
                "ss: en=%lu overlay=%lu apple=%lu frame=%lu blank=%lu mode=%s force=%u\r\n",
                (unsigned long)((feat & CARD_CTRL_FEATURE_SUPERSPRITE_ENABLE_BIT) ? 1U : 0U),
                (unsigned long)((status >> 25) & 1U),
                (unsigned long)((status >> 24) & 1U),
                (unsigned long)((status >> 8) & 0xFFFFU),
                (unsigned long)((r1 & 0x40U) ? 0U : 1U),
                mode, (unsigned)supersprite_vdp_get_force_active());
            uart_puts(control->control_uart_base, line);
            (void)snprintf(line, sizeof(line),
                "ss: R0=%02lX R1=%02lX R2=%02lX R3=%02lX R4=%02lX R5=%02lX R6=%02lX R7=%02lX\r\n",
                (unsigned long)(lo & 0xFFU), (unsigned long)((lo >> 8) & 0xFFU),
                (unsigned long)((lo >> 16) & 0xFFU), (unsigned long)((lo >> 24) & 0xFFU),
                (unsigned long)(hi & 0xFFU), (unsigned long)((hi >> 8) & 0xFFU),
                (unsigned long)((hi >> 16) & 0xFFU), (unsigned long)((hi >> 24) & 0xFFU));
            uart_puts(control->control_uart_base, line);
            p = line + snprintf(line, sizeof(line), "ss: name@%04lX:",
                                (unsigned long)name_base);
            for (i = 0; i < 16; ++i) {
                uint8_t b;
                REG_WRITE(CARD_CTRL_SS_VRAM_ADDR_REG,
                          (name_base + (uint32_t)i) & 0x3FFFU);
                b = (uint8_t)REG_READ(CARD_CTRL_SS_VRAM_DATA_REG);
                p += snprintf(p, (size_t)(line + sizeof(line) - p), " %02X", b);
            }
            uart_puts(control->control_uart_base, line);
            uart_puts(control->control_uart_base, "\r\n");
            return event;
        } else if (argc >= 2) {
            uart_puts(control->control_uart_base,
                      "usage: ss [on|off|force on|off|dump]\r\n");
            return event;
        }
        feat = REG_READ(CARD_CTRL_FEATURE_ENABLE_REG);
        uart_puts(control->control_uart_base,
                  (feat & CARD_CTRL_FEATURE_SUPERSPRITE_ENABLE_BIT) ?
                      "supersprite: on (slot 7; SmartPort disabled)\r\n" :
                      "supersprite: off\r\n");
        return event;
    }

    if (str_ieq(argv[0], "mrd")) {
        uint32_t addr;
        uint32_t count = 1U;
        uint32_t i;

        if (argc < 2 || parse_u32(argv[1], &addr) != 0 || (addr & 0x3U) != 0U) {
            uart_puts(control->control_uart_base, "usage: mrd <aligned addr> [count]\r\n");
            return event;
        }
        if (argc >= 3 && parse_u32(argv[2], &count) != 0) {
            uart_puts(control->control_uart_base, "usage: mrd <aligned addr> [count]\r\n");
            return event;
        }
        if (count == 0U || count > 16U) {
            uart_puts(control->control_uart_base, "mrd: count must be 1..16\r\n");
            return event;
        }
        if (!mrd_addr_allowed(addr, count)) {
            uart_puts(control->control_uart_base,
                      "mrd: address range is not safe to read\r\n");
            return event;
        }

        for (i = 0U; i < count; ++i) {
            char line[64];
            const uint32_t cur_addr = addr + (i * 4U);
            const uint32_t value = REG_READ(cur_addr);
            snprintf(line, sizeof(line), "0x%08lX: 0x%08lX\r\n",
                     (unsigned long)cur_addr, (unsigned long)value);
            uart_puts(control->control_uart_base, line);
        }
        return event;
    }

    if (str_ieq(argv[0], "reboot") || str_ieq(argv[0], "reset")) {
        uart_puts(control->control_uart_base, "rebooting...\r\n");
        uart_puts(control->debug_uart_base, "[uartctl] reboot requested\r\n");
        if (ops->reboot_system != NULL) {
            ops->reboot_system(ops->ctx);
        } else {
            uart_puts(control->control_uart_base, "reboot unavailable\r\n");
        }
        return event;
    }

    if (str_ieq(argv[0], "temp")) {
        if (load_snapshot(ops, &snap) != 0 || !snap.temp.valid) {
            uart_puts(control->control_uart_base, "temp: N/A\r\n");
        } else {
            char line[64];
            int32_t centi = snap.temp.centi_c;
            char sign = '+';
            if (centi < 0) {
                sign = '-';
                centi = -centi;
            }
            snprintf(line, sizeof(line), "temp: %c%ld.%02ld C\r\n",
                     sign, (long)(centi / 100), (long)(centi % 100));
            uart_puts(control->control_uart_base, line);
        }
        return event;
    }

    if (str_ieq(argv[0], "nav")) {
        if (argc < 2 || parse_nav_key(argv[1], &event.input.key) != 0) {
            uart_puts(control->control_uart_base, "usage: nav <up|down|left|right|e|toggle|scanlines|tab|shift-tab|pgup|pgdn|space|esc|menu>\r\n");
            return event;
        }
        event.input.pressed = 1U;
        event.request_redraw = 1U;
        return event;
    }

    if (str_ieq(argv[0], "vsync")) {
        int mode;
        if (argc < 2 || parse_bool_mode(argv[1], &mode) != 0) {
            uart_puts(control->control_uart_base, "usage: vsync <on|off|toggle>\r\n");
            return event;
        }
        if (ops->set_vsync == NULL) {
            return event;
        }
        if (mode == 2) {
            if (load_snapshot(ops, &snap) != 0) {
                uart_puts(control->control_uart_base, "vsync toggle failed: status unavailable\r\n");
                return event;
            }
            mode = snap.vsync_enable ? 0 : 1;
        }
        ops->set_vsync(ops->ctx, (uint8_t)mode);
        event.request_redraw = 1U;
        uart_puts(control->control_uart_base, (mode != 0) ? "vsync on\r\n" : "vsync off\r\n");
        return event;
    }

    if (str_ieq(argv[0], "scanlines")) {
        int mode;
        if (argc < 2 || str_ieq(argv[1], "status")) {
            if (load_snapshot(ops, &snap) != 0) {
                uart_puts(control->control_uart_base, "scanlines status unavailable\r\n");
                return event;
            }
            {
                char line[48];
                snprintf(line,
                         sizeof(line),
                         "scanlines %s\r\n",
                         appletini_scanlines_name(snap.scanlines_mode));
                uart_puts(control->control_uart_base, line);
            }
            return event;
        }
        if (parse_scanlines_mode(argv[1], &mode) != 0) {
            uart_puts(control->control_uart_base,
                      "usage: scanlines <off|light|medium|strong|toggle|status>\r\n");
            return event;
        }
        if (ops->set_scanlines == NULL) {
            uart_puts(control->control_uart_base, "scanlines unavailable\r\n");
            return event;
        }
        if (mode == (int)APPLETINI_SCANLINES_COUNT) {
            if (load_snapshot(ops, &snap) != 0) {
                uart_puts(control->control_uart_base, "scanlines toggle failed: status unavailable\r\n");
                return event;
            }
            mode = (int)((snap.scanlines_mode + 1U) % APPLETINI_SCANLINES_COUNT);
        }
        ops->set_scanlines(ops->ctx, (uint8_t)mode);
        event.request_redraw = 1U;
        {
            char line[48];
            snprintf(line,
                     sizeof(line),
                     "scanlines %s\r\n",
                     appletini_scanlines_name((uint8_t)mode));
            uart_puts(control->control_uart_base, line);
        }
        return event;
    }

    if (str_ieq(argv[0], "pid")) {
        ops->psram_read_id(ops->ctx);
        return event;
    }

    if (str_ieq(argv[0], "pqpi")) {
        ops->psram_qpi(ops->ctx);
        return event;
    }
    
    if (str_ieq(argv[0], "pqpix")) {
        ops->psram_qpi_exit(ops->ctx);
        return event;
    }

    if (str_ieq(argv[0], "pres")) {
        ops->psram_reset(ops->ctx);
        return event;
    }

    if (str_ieq(argv[0], "ptr")) {
        ops->psram_toggle_wrap(ops->ctx);
        return event;
    }

    if (str_ieq(argv[0], "pqr")) {
        if (argc > 1) {
            ops->psram_qspi_read(ops->ctx,argv[1]);
        }
        return event;
    }

    if (str_ieq(argv[0], "pqw")) {
        if (argc > 2) {
            ops->psram_qspi_write(ops->ctx,argv[1],argv[2],argv[3]);
        }
        return event;
    }

    if (str_ieq(argv[0], "psr")) {
        if (argc > 1) {
            ops->psram_spi_read(ops->ctx,argv[1]);
        }
        return event;
    }

    if (str_ieq(argv[0], "psw")) {
        if (argc > 2) {
            ops->psram_spi_write(ops->ctx,argv[1],argv[2],argv[3]);
        }
        return event;
    }

    if (str_ieq(argv[0], "pd")) {
        if (argc > 2) {
            ops->psram_set_delay(ops->ctx, argv[1]);
        }
        return event;
    }

    if (str_ieq(argv[0], "ps")) {
        if (argc > 1) {
            ops->psram_scan_delay(ops->ctx, argv[1]);
        }
        return event;
    }  

    if (str_ieq(argv[0], "textmono")) {
        uint8_t fg_color;
        uint8_t bg_color;

        if (argc < 2 || str_ieq(argv[1], "status")) {
            if (load_snapshot(ops, &snap) != 0) {
                uart_puts(control->control_uart_base, "textmono status unavailable\r\n");
                return event;
            }
            {
                char line[64];
                snprintf(line, sizeof(line), "text mono: fg=%s bg=%s\r\n",
                         mono_color_name(snap.text_mono_fg_color),
                         mono_color_name(snap.text_mono_bg_color));
                uart_puts(control->control_uart_base, line);
            }
            return event;
        }

        if (argc < 3 ||
            parse_mono_color(argv[1], &fg_color) != 0 ||
            parse_mono_color(argv[2], &bg_color) != 0 ||
            ops->set_text_mono_colors == NULL) {
            uart_puts(control->control_uart_base, "usage: textmono <black|white|amber|green> <black|white|amber|green>\r\n");
            return event;
        }

        ops->set_text_mono_colors(ops->ctx, fg_color, bg_color);
        event.request_redraw = 1U;
        {
            char line[96];
            snprintf(line, sizeof(line), "text mono: fg=%s bg=%s\r\n",
                     mono_color_name(fg_color), mono_color_name(bg_color));
            uart_puts(control->control_uart_base, line);
        }
        return event;
    }

    if (str_ieq(argv[0], "mono")) {
        uint8_t mono_color = 1U;

        if (argc < 2 || str_ieq(argv[1], "status")) {
            if (load_snapshot(ops, &snap) != 0) {
                uart_puts(control->control_uart_base, "mono status unavailable\r\n");
                return event;
            }
            if (snap.display_mono_enable) {
                char line[64];
                snprintf(line, sizeof(line), "display mono: on (%s)\r\n",
                         mono_color_name(snap.display_mono_color));
                uart_puts(control->control_uart_base, line);
            } else {
                uart_puts(control->control_uart_base, "display mono: off\r\n");
            }
            return event;
        }

        if (str_ieq(argv[1], "off")) {
            if (ops->set_display_mono != NULL) {
                ops->set_display_mono(ops->ctx, 0U, mono_color);
                event.request_redraw = 1U;
            }
            uart_puts(control->control_uart_base, "display mono: off\r\n");
            return event;
        }

        if (parse_mono_color(argv[1], &mono_color) != 0 || ops->set_display_mono == NULL) {
            uart_puts(control->control_uart_base, "usage: mono <off|white|amber|green|status>\r\n");
            return event;
        }

        ops->set_display_mono(ops->ctx, 1U, mono_color);
        event.request_redraw = 1U;
        {
            char line[64];
            snprintf(line, sizeof(line), "display mono: on (%s)\r\n",
                     mono_color_name(mono_color));
            uart_puts(control->control_uart_base, line);
        }
        return event;
    }

    if (str_ieq(argv[0], "audio")) {
        if (argc < 2) {
            uart_puts(control->control_uart_base, "usage: audio <status|on|off|mute|tone|amp>\r\n");
            return event;
        }

        if (str_ieq(argv[1], "status")) {
            print_snapshot(control, ops);
            return event;
        }

        if (str_ieq(argv[1], "on") || str_ieq(argv[1], "off")) {
            const uint8_t enable = str_ieq(argv[1], "on") ? 1U : 0U;
            if (ops->set_audio_enable != NULL) {
                ops->set_audio_enable(ops->ctx, enable);
                event.request_redraw = 1U;
            }
            uart_puts(control->control_uart_base, enable ? "audio enabled\r\n" : "audio disabled\r\n");
            return event;
        }

        if (str_ieq(argv[1], "mute")) {
            int mode;
            if (argc < 3) {
                mode = 2;
            } else if (parse_bool_mode(argv[2], &mode) != 0) {
                uart_puts(control->control_uart_base, "usage: audio mute <on|off|toggle>\r\n");
                return event;
            }
            if (mode == 2) {
                if (load_snapshot(ops, &snap) != 0) {
                    uart_puts(control->control_uart_base, "audio mute toggle failed: status unavailable\r\n");
                    return event;
                }
                mode = snap.audio_mute ? 0 : 1;
            }
            if (ops->set_audio_mute != NULL) {
                ops->set_audio_mute(ops->ctx, (uint8_t)mode);
                event.request_redraw = 1U;
            }
            uart_puts(control->control_uart_base, mode ? "audio mute on\r\n" : "audio mute off\r\n");
            return event;
        }

        if (str_ieq(argv[1], "tone")) {
            uint32_t hz;
            if (argc < 3 || parse_u32(argv[2], &hz) != 0 || hz == 0U || hz > 20000U) {
                uart_puts(control->control_uart_base, "usage: audio tone <1..20000>\r\n");
                return event;
            }
            if (ops->set_audio_tone_hz != NULL) {
                char line[64];
                ops->set_audio_tone_hz(ops->ctx, hz);
                event.request_redraw = 1U;
                snprintf(line, sizeof(line), "audio tone: %lu Hz\r\n", (unsigned long)hz);
                uart_puts(control->control_uart_base, line);
            }
            return event;
        }

        if (str_ieq(argv[1], "amp")) {
            uint32_t amp;
            if (argc < 3 || parse_u32(argv[2], &amp) != 0) {
                uart_puts(control->control_uart_base, "usage: audio amp <0xHEX|DEC>\r\n");
                return event;
            }
            if (ops->set_audio_amp != NULL) {
                char line[64];
                ops->set_audio_amp(ops->ctx, amp);
                event.request_redraw = 1U;
                snprintf(line, sizeof(line), "audio amp: 0x%08lX\r\n", (unsigned long)amp);
                uart_puts(control->control_uart_base, line);
            }
            return event;
        }

        uart_puts(control->control_uart_base, "usage: audio <status|on|off|mute|tone|amp>\r\n");
        return event;
    }

    if (str_ieq(argv[0], "dma")) {
        if (argc < 2) {
            uart_puts(control->control_uart_base,
                      "usage: dma <hold on|hold off|status|blocks|tail|peek|reset|probe|trace>\r\n");
            return event;
        }

        if (str_ieq(argv[1], "hold")) {
            int mode;

            if (argc < 3 || parse_bool_mode(argv[2], &mode) != 0 || mode == 2) {
                uart_puts(control->control_uart_base, "usage: dma hold <on|off>\r\n");
                return event;
            }
            dma_hold_set((uint8_t)mode);
            uart_puts(control->control_uart_base, mode ? "dma hold: on\r\n" : "dma hold: off\r\n");
            return event;
        }

        if (str_ieq(argv[1], "status")) {
            uart_control_dma_dbg_t dbg;
            char line[128];

            if (dma_dbg_read(&dbg) != 0) {
                uart_puts(control->control_uart_base, "dma status: unavailable\r\n");
                return event;
            }
            snprintf(line, sizeof(line),
                     "dma ctrl: 0x%08lX hold_req=%lu hold_act=%lu sp_act=%lu eng_dma=%lu eng_rdy=%lu\r\n",
                     (unsigned long)dbg.dma_ctrl,
                     (unsigned long)((dbg.dma_ctrl >> 0) & 1U),
                     (unsigned long)((dbg.dma_ctrl >> 4) & 1U),
                     (unsigned long)((dbg.dma_ctrl >> 8) & 1U),
                     (unsigned long)((dbg.dma_ctrl >> 9) & 1U),
                     (unsigned long)((dbg.dma_ctrl >> 10) & 1U));
            uart_puts(control->control_uart_base, line);
            snprintf(line, sizeof(line),
                     "sp dbg : fifo_writes=%lu exec_reads=%lu launches=%lu\r\n",
                     (unsigned long)dbg.fifo_cnt,
                     (unsigned long)dbg.exec_cnt,
                     (unsigned long)((dbg.dma_snap >> 24) & 0xFU));
            uart_puts(control->control_uart_base, line);
            snprintf(line, sizeof(line),
                     "sp snap: mem_ptr=0x%04lX cmd=0x%02lX (raw=0x%08lX)\r\n",
                     (unsigned long)(dbg.dma_snap & 0xFFFFU),
                     (unsigned long)((dbg.dma_snap >> 16) & 0xFFU),
                     (unsigned long)dbg.dma_snap);
            uart_puts(control->control_uart_base, line);
            return event;
        }

        if (str_ieq(argv[1], "blocks")) {
            uint32_t ctrl = REG_READ(SP_DBG_EVT_CTRL_REG);
            uint32_t count = (ctrl >> 10) & 0xFU;
            uint32_t wr_ptr = (ctrl >> 6) & 0x7U;
            uint32_t i;
            char line[192];

            snprintf(line, sizeof(line), "dma blocks: count=%lu wr_ptr=%lu\r\n",
                     (unsigned long)count,
                     (unsigned long)wr_ptr);
            uart_puts(control->control_uart_base, line);

            if (count == 0U) {
                uart_puts(control->control_uart_base, "dma blocks: no events\r\n");
                return event;
            }

            for (i = 0U; i < count; ++i) {
                uint32_t idx = (wr_ptr + SP_DBG_EVT_DEPTH - count + i) % SP_DBG_EVT_DEPTH;
                uint32_t word0;
                uint32_t word1;
                uint32_t evt_type;
                uint32_t detail;
                uint32_t mem_ptr;
                uint32_t unit;
                uint32_t block;
                const char *evt_name;

                REG_WRITE(SP_DBG_EVT_CTRL_REG, idx);
                word0 = REG_READ(SP_DBG_EVT_WORD0_REG);
                word1 = REG_READ(SP_DBG_EVT_WORD1_REG);
                evt_type = (word0 >> 24) & 0xFFU;
                detail = (word0 >> 16) & 0xFFU;
                mem_ptr = word0 & 0xFFFFU;
                unit = (word1 >> 24) & 0xFFU;
                block = word1 & 0x00FFFFFFU;

                switch (evt_type) {
                case 0x01U: evt_name = "REQ_PS"; break;
                case 0x02U: evt_name = "DMA_START"; break;
                case 0x03U: evt_name = "RESP_ERR"; break;
                case 0x04U: evt_name = "DMA_DONE"; break;
                case 0x05U: evt_name = "RESUME_ROM"; break;
                case 0x06U: evt_name = "RESUME_IO"; break;
                default: evt_name = "UNKNOWN"; break;
                }

                if (evt_type == 0x05U) {
                    uint32_t host_state = unit & 0x7U;
                    uint32_t command = (block >> 16) & 0xFFU;
                    uint32_t dma_mem = block & 0xFFFFU;

                    snprintf(line, sizeof(line),
                             "%lu: %s data=0x%02lX addr=0x%04lX state=%lu cmd=0x%02lX dma_mem=0x%04lX\r\n",
                             (unsigned long)idx,
                             evt_name,
                             (unsigned long)detail,
                             (unsigned long)mem_ptr,
                             (unsigned long)host_state,
                             (unsigned long)command,
                             (unsigned long)dma_mem);
                } else if (evt_type == 0x06U) {
                    uint32_t hold = (unit >> 7) & 0x1U;
                    uint32_t host_state = (unit >> 4) & 0x7U;
                    uint32_t io_reg = unit & 0xFU;
                    uint32_t command = (block >> 16) & 0xFFU;
                    uint32_t dma_mem = block & 0xFFFFU;

                    snprintf(line, sizeof(line),
                             "%lu: %s data=0x%02lX addr=0x%04lX reg=0x%01lX state=%lu hold=%lu cmd=0x%02lX dma_mem=0x%04lX\r\n",
                             (unsigned long)idx,
                             evt_name,
                             (unsigned long)detail,
                             (unsigned long)mem_ptr,
                             (unsigned long)io_reg,
                             (unsigned long)host_state,
                             (unsigned long)hold,
                             (unsigned long)command,
                             (unsigned long)dma_mem);
                } else {
                    snprintf(line, sizeof(line),
                             "%lu: %s detail=0x%02lX unit=0x%02lX block=%lu mem=0x%04lX\r\n",
                             (unsigned long)idx,
                             evt_name,
                             (unsigned long)detail,
                             (unsigned long)unit,
                             (unsigned long)block,
                             (unsigned long)mem_ptr);
                }
                uart_puts(control->control_uart_base, line);
            }
            return event;
        }

        if (str_ieq(argv[1], "tail")) {
            uint32_t ctrl = REG_READ(AT_TAIL_CTRL_REG);
            uint32_t count = (ctrl >> 8) & 0x1FU;
            uint32_t wr_ptr = (ctrl >> 4) & 0xFU;
            uint32_t i;
            char line[192];

            snprintf(line, sizeof(line), "dma tail: count=%lu wr_ptr=%lu\r\n",
                     (unsigned long)count,
                     (unsigned long)wr_ptr);
            uart_puts(control->control_uart_base, line);

            if (count == 0U) {
                uart_puts(control->control_uart_base, "dma tail: no events\r\n");
                return event;
            }

            for (i = 0U; i < count; ++i) {
                uint32_t idx = (wr_ptr + AT_TAIL_DEPTH - count + i) % AT_TAIL_DEPTH;
                uint32_t word0;
                uint32_t word1;
                uint32_t evt_type;
                const char *evt_name;

                REG_WRITE(AT_TAIL_CTRL_REG, idx);
                word0 = REG_READ(AT_TAIL_WORD0_REG);
                word1 = REG_READ(AT_TAIL_WORD1_REG);
                evt_type = (word0 >> 24) & 0xFFU;

                switch (evt_type) {
                case 0x01U: evt_name = "LAST_PUSH"; break;
                case 0x02U: evt_name = "SP_IDLE"; break;
                case 0x03U: evt_name = "STAGE_POP"; break;
                case 0x04U: evt_name = "STOP"; break;
                case 0x05U: evt_name = "BYTE_DONE"; break;
                case 0x06U: evt_name = "ENG_IDLE"; break;
                default: evt_name = "UNKNOWN"; break;
                }

                snprintf(line, sizeof(line),
                         "%lu: #%lu %s tail=0x%04lX eng=0x%04lX sp=%lu stg=%lu comb=%lu busy=%lu edma=%lu ready=%lu done=%lu last=%lu push=%lu pop=%lu stop=%lu valid=%lu rw=%lu\r\n",
                         (unsigned long)idx,
                         (unsigned long)((word0 >> 16) & 0xFFU),
                         evt_name,
                         (unsigned long)((word1 >> 16) & 0xFFFFU),
                         (unsigned long)(word1 & 0xFFFFU),
                         (unsigned long)((word0 >> 15) & 1U),
                         (unsigned long)((word0 >> 14) & 1U),
                         (unsigned long)((word0 >> 13) & 1U),
                         (unsigned long)((word0 >> 12) & 1U),
                         (unsigned long)((word0 >> 11) & 1U),
                         (unsigned long)((word0 >> 10) & 1U),
                         (unsigned long)((word0 >> 9) & 1U),
                         (unsigned long)((word0 >> 8) & 1U),
                         (unsigned long)((word0 >> 7) & 1U),
                         (unsigned long)((word0 >> 6) & 1U),
                         (unsigned long)((word0 >> 5) & 1U),
                         (unsigned long)((word0 >> 4) & 1U),
                         (unsigned long)((word0 >> 3) & 1U));
                uart_puts(control->control_uart_base, line);
            }
            return event;
        }

        if (str_ieq(argv[1], "trace")) {
            uint32_t status;
            uint32_t trace_cfg;
            uint32_t trigger_addr;
            uint32_t release_mode;
            char line[160];

            if (argc < 3 || str_ieq(argv[2], "status")) {
                status = REG_READ(AT_TRACE_CTRL_REG);
                trace_cfg = REG_READ(AT_TRACE_CFG_REG);
                trigger_addr = trace_cfg & 0xFFFFU;
                release_mode = (trace_cfg & AT_TRACE_CFG_RELEASE_BIT) ? 1U : 0U;
                snprintf(line, sizeof(line),
                         "dma trace: arm=%lu capturing=%lu done=%lu mode=%s trigger_addr=0x%04lX next_byte=%lu samples=%lu\r\n",
                         (unsigned long)((status & AT_TRACE_CTRL_ARM_BIT) ? 1U : 0U),
                         (unsigned long)((status & AT_TRACE_CTRL_CAPTURING_BIT) ? 1U : 0U),
                         (unsigned long)((status & AT_TRACE_CTRL_DONE_BIT) ? 1U : 0U),
                         release_mode ? "release" : "write",
                         (unsigned long)trigger_addr,
                         (unsigned long)(status >> 16),
                         (unsigned long)((status >> 8) & 0xFFU));
                uart_puts(control->control_uart_base, line);
                return event;
            }

            if (str_ieq(argv[2], "arm")) {
                uint32_t trace_addr = 0x0800U;
                uint32_t cfg;
                uint32_t release = 0U;

                if (argc >= 4 && (str_ieq(argv[3], "release") || str_ieq(argv[3], "resume"))) {
                    release = 1U;
                    trace_addr = 0U;
                    if (argc >= 5 && parse_u32(argv[4], &trace_addr) != 0) {
                        uart_puts(control->control_uart_base, "usage: dma trace arm [addr|release [addr]]\r\n");
                        return event;
                    }
                } else if (argc >= 4 && parse_u32(argv[3], &trace_addr) != 0) {
                    uart_puts(control->control_uart_base, "usage: dma trace arm [addr|release [addr]]\r\n");
                    return event;
                }
                if (trace_addr > 0xFFFFU) {
                    uart_puts(control->control_uart_base, "dma trace arm: addr must be 0x0000..0xFFFF\r\n");
                    return event;
                }

                cfg = trace_addr & 0xFFFFU;
                if (release != 0U) {
                    cfg |= AT_TRACE_CFG_RELEASE_BIT;
                }
                REG_WRITE(AT_TRACE_CFG_REG, cfg);
                REG_WRITE(AT_TRACE_CTRL_REG, AT_TRACE_CTRL_CLEAR_BIT);
                REG_WRITE(AT_TRACE_CTRL_REG, AT_TRACE_CTRL_ARM_BIT);

                if (release != 0U) {
                    if (trace_addr == 0U) {
                        uart_puts(control->control_uart_base, "dma trace: armed for post-DMA release (any session)\r\n");
                    } else {
                        snprintf(line, sizeof(line),
                                 "dma trace: armed for post-DMA release at 0x%04lX\r\n",
                                 (unsigned long)trace_addr);
                        uart_puts(control->control_uart_base, line);
                    }
                } else {
                    snprintf(line, sizeof(line),
                             "dma trace: armed for write addr 0x%04lX\r\n",
                             (unsigned long)trace_addr);
                    uart_puts(control->control_uart_base, line);
                }
                return event;
            }

            if (str_ieq(argv[2], "off")) {
                REG_WRITE(AT_TRACE_CTRL_REG, 0U);
                uart_puts(control->control_uart_base, "dma trace: disabled\r\n");
                return event;
            }

            if (str_ieq(argv[2], "clear")) {
                status = REG_READ(AT_TRACE_CTRL_REG) & AT_TRACE_CTRL_ARM_BIT;
                REG_WRITE(AT_TRACE_CTRL_REG, status | AT_TRACE_CTRL_CLEAR_BIT);
                uart_puts(control->control_uart_base, "dma trace: cleared\r\n");
                return event;
            }

            if (str_ieq(argv[2], "dump")) {
                uint32_t start = 0U;
                uint32_t count = 16U;
                uint32_t i;

                if (argc >= 4 && parse_u32(argv[3], &start) != 0) {
                    uart_puts(control->control_uart_base, "usage: dma trace dump [start] [count]\r\n");
                    return event;
                }
                if (argc >= 5 && parse_u32(argv[4], &count) != 0) {
                    uart_puts(control->control_uart_base, "usage: dma trace dump [start] [count]\r\n");
                    return event;
                }
                if (count == 0U || count > 32U || start >= AT_TRACE_DEPTH || (start + count) > AT_TRACE_DEPTH) {
                    uart_puts(control->control_uart_base, "dma trace dump: start/count out of range\r\n");
                    return event;
                }

                status = REG_READ(AT_TRACE_CTRL_REG);
                trace_cfg = REG_READ(AT_TRACE_CFG_REG);
                trigger_addr = trace_cfg & 0xFFFFU;
                release_mode = (trace_cfg & AT_TRACE_CFG_RELEASE_BIT) ? 1U : 0U;
                snprintf(line, sizeof(line),
                         "dma trace dump: mode=%s trigger_addr=0x%04lX next_byte=%lu samples=%lu arm=%lu cap=%lu done=%lu\r\n",
                         release_mode ? "release" : "write",
                         (unsigned long)trigger_addr,
                         (unsigned long)(status >> 16),
                         (unsigned long)((status >> 8) & 0xFFU),
                         (unsigned long)((status & AT_TRACE_CTRL_ARM_BIT) ? 1U : 0U),
                         (unsigned long)((status & AT_TRACE_CTRL_CAPTURING_BIT) ? 1U : 0U),
                         (unsigned long)((status & AT_TRACE_CTRL_DONE_BIT) ? 1U : 0U));
                uart_puts(control->control_uart_base, line);

                for (i = 0U; i < count; ++i) {
                    uint32_t idx = start + i;
                    uint32_t word0;
                    uint32_t word1;

                    REG_WRITE(AT_TRACE_INDEX_REG, idx);
                    word0 = REG_READ(AT_TRACE_WORD0_REG);
                    word1 = REG_READ(AT_TRACE_WORD1_REG);
                    snprintf(line, sizeof(line),
                             "%02lu: b=%03lu phi0=%lu dma=%lu rdy=%lu rw=%lu dir=%lu dout=%lu adr=%lu do=0x%02lX di=0x%02lX ao=0x%04lX ai=0x%04lX\r\n",
                             (unsigned long)idx,
                             (unsigned long)(word0 & 0xFFU),
                             (unsigned long)((word0 >> 30) & 1U),
                             (unsigned long)((word0 >> 29) & 1U),
                             (unsigned long)((word0 >> 28) & 1U),
                             (unsigned long)((word0 >> 27) & 1U),
                             (unsigned long)((word0 >> 26) & 1U),
                             (unsigned long)((word0 >> 25) & 1U),
                             (unsigned long)((word0 >> 24) & 1U),
                             (unsigned long)((word0 >> 8) & 0xFFU),
                             (unsigned long)((word0 >> 16) & 0xFFU),
                             (unsigned long)((word1 >> 16) & 0xFFFFU),
                             (unsigned long)(word1 & 0xFFFFU));
                    uart_puts(control->control_uart_base, line);
                }
                return event;
            }

            uart_puts(control->control_uart_base,
                      "usage: dma trace <status|arm [addr|release [addr]]|off|clear|dump [start] [count]>\r\n");
            return event;
        }

        if (str_ieq(argv[1], "reset")) {
            dma_dbg_reset();
            uart_puts(control->control_uart_base, "dma debug cleared\r\n");
            return event;
        }

        if (str_ieq(argv[1], "dump")) {
            /* Raw PSRAM read via the psdma path (MC -> DDR), for
             * inspecting card-served memory (aux banks, disk2 staging)
             * while the Apple is wedged. 24-bit MC byte address; bank
             * N of aux lives at N*0x10000. Read-only. */
            static uint8_t dump_buf[64] __attribute__((aligned(64)));
            uint32_t addr = 0U;
            uint32_t count = 32U;
            uint32_t base;
            uint32_t span;
            uint32_t i;
            uint32_t t0;
            char line[96];

            if (argc < 3 || parse_u32(argv[2], &addr) != 0 ||
                addr > 0xFFFFFFU) {
                uart_puts(control->control_uart_base,
                          "usage: dma dump <mcaddr24> [count<=56]\r\n");
                return event;
            }
            if (argc >= 4) {
                (void)parse_u32(argv[3], &count);
            }
            if (count == 0U || count > 56U) {
                count = 32U;
            }
            base = addr & ~7U;
            span = ((addr + count + 7U) & ~7U) - base;
            if (span > sizeof(dump_buf)) {
                span = sizeof(dump_buf);
            }

            Xil_DCacheFlushRange((UINTPTR)dump_buf, sizeof(dump_buf));
            REG_WRITE(0x40030000U, base);
            REG_WRITE(0x40030004U, (uint32_t)(uintptr_t)dump_buf);
            REG_WRITE(0x40030008U, span);   /* rw bit31=0: MC -> DDR */
            for (t0 = 0U; t0 < 5000000U; ++t0) {
                if ((REG_READ(0x4003000CU) & 1U) != 0U) {
                    break;
                }
            }
            if (t0 >= 5000000U) {
                uart_puts(control->control_uart_base,
                          "dma dump: TIMEOUT\r\n");
                return event;
            }
            Xil_DCacheInvalidateRange((UINTPTR)dump_buf, sizeof(dump_buf));

            for (i = 0U; i < span; i += 8U) {
                (void)snprintf(line, sizeof(line),
                    "%06lX: %02X %02X %02X %02X %02X %02X %02X %02X\r\n",
                    (unsigned long)(base + i),
                    dump_buf[i], dump_buf[i+1U], dump_buf[i+2U],
                    dump_buf[i+3U], dump_buf[i+4U], dump_buf[i+5U],
                    dump_buf[i+6U], dump_buf[i+7U]);
                uart_puts(control->control_uart_base, line);
            }
            return event;
        }

        if (str_ieq(argv[1], "peek")) {
            uint32_t addr = 0U;
            uint32_t count = 16U;
            uint32_t done = 0U;
            uint32_t saw_active = 0U;
            uint32_t i;
            uint8_t dump_buf[AT_PEEK_DEPTH];
            char line[160];

            if (argc < 3 || parse_u32(argv[2], &addr) != 0) {
                uart_puts(control->control_uart_base, "usage: dma peek <addr> [count]\r\n");
                return event;
            }
            if (argc >= 4 && parse_u32(argv[3], &count) != 0) {
                uart_puts(control->control_uart_base, "usage: dma peek <addr> [count]\r\n");
                return event;
            }
            if (addr > 0xFFFFU || count == 0U || count > AT_PEEK_DEPTH || (addr + count - 1U) > 0xFFFFU) {
                uart_puts(control->control_uart_base, "dma peek: addr/count out of range (count 1..32)\r\n");
                return event;
            }

            while (done < count) {
                uint8_t raw_buf[AT_PEEK_DEPTH];
                uint16_t raw_done = 0U;
                uint32_t chunk = count - done;
                uint32_t actual_done;
                int rc;

                if (chunk > 8U) {
                    chunk = 8U;
                }

                rc = dma_peek_raw_read((uint16_t)(addr + done),
                                       (uint16_t)(chunk + 1U),
                                       raw_buf,
                                       &raw_done);
                saw_active |= (rc == 0) ? 1U : 0U;
                if (raw_done <= 1U) {
                    break;
                }

                actual_done = (uint32_t)raw_done - 1U;
                if (actual_done > chunk) {
                    actual_done = chunk;
                }
                memcpy(&dump_buf[done], &raw_buf[1], actual_done);
                done += actual_done;

                if (rc != 0 || actual_done != chunk) {
                    break;
                }
            }

            snprintf(line, sizeof(line),
                     "dma peek: addr=0x%04lX requested=%lu done=%lu%s\r\n",
                     (unsigned long)addr,
                     (unsigned long)count,
                     (unsigned long)done,
                     (saw_active == 0U || done != count) ? " timeout" : "");
            uart_puts(control->control_uart_base, line);

            for (i = 0U; i < done; i += 8U) {
                uint32_t j;
                uint32_t line_count = ((done - i) > 8U) ? 8U : (done - i);
                int offset = snprintf(line, sizeof(line), "0x%04lX:",
                                      (unsigned long)(addr + i));
                for (j = 0U; j < line_count && offset > 0 && offset < (int)sizeof(line); ++j) {
                    offset += snprintf(line + offset, sizeof(line) - (size_t)offset,
                                       " %02X", dump_buf[i + j]);
                }
                if (offset > 0 && offset < (int)(sizeof(line) - 2U)) {
                    line[offset++] = '\r';
                    line[offset++] = '\n';
                    line[offset] = '\0';
                } else {
                    line[sizeof(line) - 3U] = '\r';
                    line[sizeof(line) - 2U] = '\n';
                    line[sizeof(line) - 1U] = '\0';
                }
                uart_puts(control->control_uart_base, line);
            }
            return event;
        }

        if (str_ieq(argv[1], "probe")) {
            uint32_t addr = 0x0400U;
            uint32_t pattern = 0xC1U;
            uint32_t count = 16U;
            uint16_t done = 0U;
            int rc;
            char line[128];

            if (argc >= 3 && parse_u32(argv[2], &addr) != 0) {
                uart_puts(control->control_uart_base, "usage: dma probe [addr] [pattern] [count]\r\n");
                return event;
            }
            if (argc >= 4 && parse_u32(argv[3], &pattern) != 0) {
                uart_puts(control->control_uart_base, "usage: dma probe [addr] [pattern] [count]\r\n");
                return event;
            }
            if (argc >= 5 && parse_u32(argv[4], &count) != 0) {
                uart_puts(control->control_uart_base, "usage: dma probe [addr] [pattern] [count]\r\n");
                return event;
            }
            if (count == 0U || count > 0xFFFFU) {
                uart_puts(control->control_uart_base, "dma probe: count must be 1..65535\r\n");
                return event;
            }
            if (addr > 0xFFFFU || pattern > 0xFFU) {
                uart_puts(control->control_uart_base, "dma probe: addr<=0xFFFF, pattern<=0xFF\r\n");
                return event;
            }

            snprintf(line, sizeof(line),
                     "dma probe: writing %lu bytes of 0x%02lX to 0x%04lX...\r\n",
                     (unsigned long)count,
                     (unsigned long)pattern,
                     (unsigned long)addr);
            uart_puts(control->control_uart_base, line);

            rc = dma_probe_write((uint16_t)addr, (uint8_t)pattern, (uint16_t)count, &done);
            snprintf(line, sizeof(line),
                     "dma probe: %s, byte_done=%lu of %lu\r\n",
                     (rc == 0) ? "complete" : "TIMEOUT (still busy)",
                     (unsigned long)done,
                     (unsigned long)count);
            uart_puts(control->control_uart_base, line);
            return event;
        }

        uart_puts(control->control_uart_base,
                  "usage: dma <hold on|hold off|status|blocks|tail|peek|reset|probe|trace>\r\n");
        return event;
    }

    if (str_ieq(argv[0], "rtc")) {
        rtc_pcf8563_time_t t;

        if (argc >= 2 && str_ieq(argv[1], "get")) {
            int snap_rc = load_snapshot(ops, &snap);

            if (snap_rc != 0) {
                uart_puts(control->control_uart_base, "rtc: snapshot unavailable\r\n");
            } else if (!snap.rtc.valid) {
                char line[40];

                snprintf(line, sizeof(line), "rtc: N/A status=0x%02X\r\n",
                         (unsigned)snap.rtc.status);
                uart_puts(control->control_uart_base, line);
            } else {
                char line[64];
                snprintf(line, sizeof(line), "rtc: %04u-%02u-%02u %02u:%02u:%02u\r\n",
                         (unsigned)snap.rtc.year, (unsigned)snap.rtc.month, (unsigned)snap.rtc.day,
                         (unsigned)snap.rtc.hour, (unsigned)snap.rtc.min, (unsigned)snap.rtc.sec);
                uart_puts(control->control_uart_base, line);
            }
            return event;
        }
        
        if (argc >= 3 && str_ieq(argv[1], "set")) {
            if (argc < 4 || parse_rtc_datetime(argv[2], argv[3], &t) != 0) {
                uart_puts(control->control_uart_base, "usage: rtc set YYYY-MM-DD HH:MM:SS\r\n");
                return event;
            }
        }

        if (ops->set_rtc == NULL) {
            uart_puts(control->control_uart_base, "rtc set unavailable\r\n");
            return event;
        }

        if (ops->set_rtc(ops->ctx, &t) == 0) {
            uart_puts(control->control_uart_base, "rtc set OK\r\n");
            uart_puts(control->debug_uart_base, "[rtc] set OK\r\n");
            event.request_redraw = 1U;
        } else {
            uart_puts(control->control_uart_base, "rtc set failed\r\n");
            uart_puts(control->debug_uart_base, "[rtc] set failed\r\n");
        }
        return event;
    }

    uart_puts(control->control_uart_base, "Unknown cmd. Use :help\r\n");
    return event;
}

uart_control_event_t uart_control_poll(uart_control_t *control, const uart_control_ops_t *ops)
{
    bustail_chase_consumer();
    sswatch_poll(control->control_uart_base);
    uart_control_event_t event;
    char c;
    ui_key_t esc_key;

    memset(&event, 0, sizeof(event));

    if (control == NULL || ops == NULL) {
        return event;
    }

    if (!uart_try_getc(control->control_uart_base, &c)) {
        return event;
    }

    if (control->cmd_mode != 0U) {
        if (c == '\r' || c == '\n') {
            uart_puts(control->control_uart_base, "\r\n");
            control->cmd_buf[control->cmd_len] = '\0';
            control->cmd_mode = 0U;
            event = process_command(control, ops, control->cmd_buf);
            control->cmd_len = 0U;
            return event;
        }

        if ((c == 8 || c == 127) && control->cmd_len > 0U) {
            control->cmd_len--;
            uart_puts(control->control_uart_base, "\b \b");
            return event;
        }

        if (c >= 32 && c <= 126 && control->cmd_len < (sizeof(control->cmd_buf) - 1U)) {
            control->cmd_buf[control->cmd_len++] = c;
            uart_putc(control->control_uart_base, c);
        }
        return event;
    }

    if (c == ':' || c == ';') {
        control->cmd_mode = 1U;
        control->cmd_len = 0U;
        uart_puts(control->control_uart_base, "\r\ncmd> ");
        return event;
    }

    if (c == '?') {
        uart_control_print_help(control, ops);
        return event;
    }

    if (c == '\r' || c == '\n') {
        return event;
    }
    if (c == '\t') {
        uart_control_set_input(&event.input, UI_KEY_TAB);
        return event;
    }
    if (c == ' ') {
        uart_control_set_input(&event.input, UI_KEY_SPACE);
        return event;
    }
    if (c == 27) {
        esc_key = uart_control_decode_escape(control->control_uart_base);
        uart_control_set_input(&event.input, esc_key);
        return event;
    }

    switch (c) {
    case 'w':
    case 'W':
        event.input.key = UI_KEY_UP;
        event.input.pressed = 1U;
        break;
    case 's':
    case 'S':
        event.input.key = UI_KEY_DOWN;
        event.input.pressed = 1U;
        break;
    case 'a':
    case 'A':
        event.input.key = UI_KEY_LEFT;
        event.input.pressed = 1U;
        break;
    case 'd':
    case 'D':
        event.input.key = UI_KEY_RIGHT;
        event.input.pressed = 1U;
        break;
    case 'e':
    case 'E':
        event.input.key = UI_KEY_ENTER;
        event.input.pressed = 1U;
        break;
    case 't':
    case 'T':
        event.input.key = UI_KEY_TOGGLE;
        event.input.pressed = 1U;
        break;
    case 'l':
    case 'L':
        event.input.key = UI_KEY_SCANLINES;
        event.input.pressed = 1U;
        break;
    case 'm':
    case 'M':
        event.input.key = UI_KEY_MENU;
        event.input.pressed = 1U;
        break;
    default:
        if (c >= 32 && c <= 126) {
            uart_puts(control->control_uart_base, "\r\nUnknown key. Use ? for help\r\n");
        }
        break;
    }

    return event;
}
