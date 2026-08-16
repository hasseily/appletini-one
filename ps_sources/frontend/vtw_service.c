/* Virtual TransWarp service -- see vtw_service.h. */

#include "vtw_service.h"

#include <stdio.h>
#include <string.h>

#include "xiltimer.h"

#include "card_control_regs.h"
#include "boot_menu_service.h"
#include "disk2_service.h"
#include "../lib/common.h"
#include "../lib/uart.h"

/* The fixed Enhanced //e main ROM (16 KB, $C000-$FFFF), embedded and
 * loaded into the shadow ROM region on every accelerated session so every
 * host -- //e or II+ -- runs an Enhanced //e when accelerated. Offsets
 * $0000-$00FF map to the $C000-$C0FF I/O window (zero-filled, never
 * ROM-routed); the firmware/monitor image is $0100-$3FFF. Destination:
 * shadow phys $20000 = $C000, matching vtw_shadow_pkg. Data is in the
 * generated apple2e_cpu_rom_data.c. */
extern const uint8_t apple2e_cpu_rom[16384];
#define VTW_ROM_BYTES          0x4000U
#define VTW_SHADOW_ROM_PHYS    0x20000UL
/* Bus-takeover confirmation: the engine needs a few Apple cycles (DMA
 * assert + grace); retry the check across polls rather than spinning. */
#define VTW_TAKE_POLL_LIMIT    64U
/* Takeover machine reset: hold Apple RES# like a CTRL-RESET, then let the
 * PS-side reset handlers (boot menu re-arm, machine re-report plumbing)
 * settle before the ROM copy starts. */
#define VTW_RES_HOLD_MS        100U
#define VTW_RES_SETTLE_MS      50U
#define VTW_ONEE_RUN_POLL_LIMIT 64U

typedef struct {
    uint16_t off;
    uint8_t val;
} vtw_rom_patch_t;

/* Enhanced //e reset-path Apple-key reads. Only physical II/II+ host
 * sessions use these patches; ONE//e is a native Enhanced //e. */
static const vtw_rom_patch_t k_iiplus_rom_patches[] = {
    { 0x02BBU, 0xA9U }, { 0x02BCU, 0x00U }, { 0x02BDU, 0xEAU },
    { 0x02C3U, 0xA9U }, { 0x02C4U, 0x00U }, { 0x02C5U, 0xEAU },
};

typedef enum {
    VTW_ST_IDLE = 0,     /* disabled, or waiting for machine identification */
    VTW_ST_TAKE_BUS,     /* enable written; waiting for bus ownership */
    VTW_ST_RES_HOLD,     /* Apple RES# held: whole machine resets */
    VTW_ST_RES_SETTLE,   /* RES# released: PS reset handlers settle */
    VTW_ST_LOAD_ROM,     /* load the fixed Enhanced //e ROM into the shadow */
    VTW_ST_RUN           /* core released, session live */
} vtw_state_t;

static uint32_t g_uart_base;
static uint8_t g_intent_enabled;
static uint8_t g_speed_mode = CARD_CTRL_VTW_SPEED_FULL;
static uint8_t g_pace_divider = 37U;   /* ~3.6 MHz-equivalent preset */

/* Runtime speed override (USB keymap actions): $C074-style, never
 * persisted. The configured menu speed above is the baseline the toggles
 * return to; a menu speed change clears the override. */
static uint8_t  g_ovr_active;
static uint8_t  g_ovr_mode;
static uint16_t g_ovr_div;
/* Slug key binding gate (TransWarp tab, default off): an accidental
 * 0.05 MHz toggle mid-game looks like a crash, so the key must be
 * armed deliberately. */
static uint8_t  g_slug_enabled;
/* Persisted compatibility override: discard all core writes to $C074.
 * Menu and USB speed controls still rewrite VTW_CTRL normally. */
static uint8_t  g_ignore_c074;
/* Persisted compatibility option: keep Disk II on the original physical
 * 1 MHz path instead of using vTW's private read shortcut. */
static uint8_t  g_disable_disk2_accel;
/* Result of the last speed action, for the on-screen overlay. */
static char     g_last_action_text[40];

/* 133.333 MHz / 50 kHz: the 0.05 MHz slug debug speed. */
#define VTW_SLUG_DIVIDER 2667U

/* Per-region slowdown config (mirrored into the PL 0x6B register). */
static uint16_t g_slowdown_mask;    /* [8:0] region enables */
static uint16_t g_slowdown_cycles;  /* 1 MHz window per region access */

/* Speed ladder for the up/down actions. Slug is deliberately NOT on it. */
static const struct {
    uint8_t     mode;
    uint16_t    divider;
    const char *name;
} k_vtw_ladder[] = {
    { CARD_CTRL_VTW_SPEED_1MHZ,    0U,  "1 MHz default" },
    { CARD_CTRL_VTW_SPEED_DIVIDED, 51U, "2.6 MHz" },
    { CARD_CTRL_VTW_SPEED_DIVIDED, 37U, "3.6 MHz (TransWarp)" },
    { CARD_CTRL_VTW_SPEED_DIVIDED, 19U, "7 MHz" },
    { CARD_CTRL_VTW_SPEED_DIVIDED, 10U, "13 MHz (UltraWarp)" },
    { CARD_CTRL_VTW_SPEED_DIVIDED,  5U, "26 MHz" },
    { CARD_CTRL_VTW_SPEED_FULL,    0U,  "MAX Speed" },
};
#define VTW_LADDER_COUNT \
    (sizeof(k_vtw_ladder) / sizeof(k_vtw_ladder[0]))

static uint8_t vtw_eff_mode(void)
{
    return g_ovr_active != 0U ? g_ovr_mode : g_speed_mode;
}

static uint16_t vtw_eff_divider(void)
{
    return g_ovr_active != 0U ? g_ovr_div : (uint16_t)g_pace_divider;
}

static uint8_t vtw_eff_is_slug(void)
{
    return (vtw_eff_mode() == CARD_CTRL_VTW_SPEED_DIVIDED &&
            vtw_eff_divider() >= VTW_SLUG_DIVIDER) ? 1U : 0U;
}

static void vtw_override_clear(void)
{
    g_ovr_active = 0U;
    g_ovr_mode = 0U;
    g_ovr_div = 0U;
}

static vtw_state_t g_state = VTW_ST_IDLE;
static uint32_t g_take_polls;
static uint32_t g_sessions_started;
static uint8_t g_announced_wait;
static uint8_t g_announced_handoff_wait;
static uint8_t g_onee_running;
static uint8_t g_onee_disk2_override_active;
static uint8_t g_disk2_config_enabled;
static XTime g_res_phase_start;

static const char *vtw_state_name(vtw_state_t st)
{
    switch (st) {
    case VTW_ST_IDLE:       return "idle";
    case VTW_ST_TAKE_BUS:   return "taking bus";
    case VTW_ST_RES_HOLD:   return "machine reset";
    case VTW_ST_RES_SETTLE: return "reset settle";
    case VTW_ST_LOAD_ROM:   return "loading ROM";
    case VTW_ST_RUN:        return "running";
    default:                return "?";
    }
}

static uint32_t vtw_ctrl_value(uint8_t enable, uint8_t core_run,
                               uint8_t apple_res)
{
    uint32_t v = 0U;

    if (enable != 0U) {
        v |= CARD_CTRL_VTW_CTRL_ENABLE_BIT;
    }
    if (core_run != 0U) {
        v |= CARD_CTRL_VTW_CTRL_CORE_RUN_BIT;
    }
    if (apple_res != 0U) {
        v |= CARD_CTRL_VTW_CTRL_APPLE_RES_BIT;
    }
    v |= ((uint32_t)(vtw_eff_mode() & CARD_CTRL_VTW_CTRL_SPEED_MASK))
         << CARD_CTRL_VTW_CTRL_SPEED_SHIFT;
    v |= ((uint32_t)vtw_eff_divider()) << CARD_CTRL_VTW_CTRL_DIVIDER_SHIFT;
    if (boot_menu_service_machine_mode() == CARD_MACHINE_MODE_IIPLUS) {
        /* Controller-less II/II+ game-connector buttons float "pressed";
         * serve the //e Apple-key reads ($C061-$C063) as not-pressed
         * inside the core. Clear via a ctrl-reg write for hosts with a
         * real controller attached. */
        v |= CARD_CTRL_VTW_CTRL_IIPLUS_BTNS_BIT;
    }
    if (g_ignore_c074 != 0U) {
        v |= CARD_CTRL_VTW_CTRL_IGNORE_C074_BIT;
    }
    if (g_disable_disk2_accel != 0U) {
        v |= CARD_CTRL_VTW_CTRL_DISABLE_D2_ACCEL_BIT;
    }
    return v;
}

static uint32_t vtw_onee_ctrl_value(uint8_t core_run, uint8_t apple_res)
{
    uint32_t v = CARD_CTRL_VTW_CTRL_ENABLE_BIT |
                 CARD_CTRL_VTW_CTRL_DISABLE_D2_ACCEL_BIT;

    if (core_run != 0U) {
        v |= CARD_CTRL_VTW_CTRL_CORE_RUN_BIT;
    }
    if (apple_res != 0U) {
        v |= CARD_CTRL_VTW_CTRL_APPLE_RES_BIT;
    }
    v |= ((uint32_t)(vtw_eff_mode() & CARD_CTRL_VTW_CTRL_SPEED_MASK))
         << CARD_CTRL_VTW_CTRL_SPEED_SHIFT;
    v |= ((uint32_t)vtw_eff_divider()) << CARD_CTRL_VTW_CTRL_DIVIDER_SHIFT;
    if (g_ignore_c074 != 0U) {
        v |= CARD_CTRL_VTW_CTRL_IGNORE_C074_BIT;
    }
    return v;
}

typedef enum {
    VTW_CTRL_LIVE_NONE = 0,
    VTW_CTRL_LIVE_APPLIED,
    VTW_CTRL_LIVE_FAILED
} vtw_ctrl_live_result_t;

/* Apply the current speed and option state without changing the phase of the
 * active host or ONE//e session. All menu, option, and USB-key live updates
 * pass through this writer so they cannot disagree about which control word
 * owns the running core. */
static vtw_ctrl_live_result_t vtw_apply_ctrl_live(void)
{
    uint32_t desired;

    if (g_onee_running != 0U) {
        desired = vtw_onee_ctrl_value(1U, 0U);
    } else if (g_state == VTW_ST_RUN) {
        desired = vtw_ctrl_value(1U, 1U, 0U);
    } else if (g_state == VTW_ST_RES_HOLD) {
        desired = vtw_ctrl_value(1U, 0U, 1U);
    } else if (g_state != VTW_ST_IDLE) {
        desired = vtw_ctrl_value(1U, 0U, 0U);
    } else {
        return VTW_CTRL_LIVE_NONE;
    }

    REG_WRITE(CARD_CTRL_VTW_CTRL_REG, desired);
    if (REG_READ(CARD_CTRL_VTW_CTRL_REG) != desired) {
        uart_puts(g_uart_base, "vtw: CTRL write readback failed\r\n");
        return VTW_CTRL_LIVE_FAILED;
    }
    return VTW_CTRL_LIVE_APPLIED;
}

static uint8_t vtw_onee_isolation_confirmed(void)
{
    const uint32_t status = REG_READ(CARD_CTRL_ONEE_MODE_REG);
    const uint32_t signature =
        (status >> CARD_CTRL_ONEE_STATUS_SIGNATURE_SHIFT) &
        CARD_CTRL_ONEE_STATUS_SIGNATURE_MASK;
    const uint32_t inhibit =
        (status >> CARD_CTRL_ONEE_STATUS_INHIBIT_SHIFT) &
        CARD_CTRL_ONEE_STATUS_INHIBIT_MASK;
    const uint32_t required =
        CARD_CTRL_ONEE_STATUS_REQUEST_BIT |
        CARD_CTRL_ONEE_STATUS_EFFECTIVE_BIT |
        CARD_CTRL_ONEE_STATUS_ISOLATED_BIT |
        CARD_CTRL_ONEE_STATUS_SELECTED_BIT |
        CARD_CTRL_ONEE_STATUS_HDL_PRESENT_BIT;
    const uint32_t blocked =
        CARD_CTRL_ONEE_STATUS_OUTPUTS_OFF_BIT |
        CARD_CTRL_ONEE_STATUS_ACTIVITY_BIT |
        CARD_CTRL_ONEE_STATUS_LOCKOUT_BIT |
        CARD_CTRL_ONEE_STATUS_APPLE_POWER_BIT;

    return ((status & required) == required &&
            (status & blocked) == 0U &&
            signature == CARD_CTRL_ONEE_STATUS_SIGNATURE &&
            inhibit == CARD_CTRL_ONEE_INHIBIT_NONE) ? 1U : 0U;
}

static uint8_t vtw_onee_control_active(void)
{
    const uint32_t status = REG_READ(CARD_CTRL_ONEE_MODE_REG);
    const uint32_t active =
        CARD_CTRL_ONEE_STATUS_REQUEST_BIT |
        CARD_CTRL_ONEE_STATUS_EFFECTIVE_BIT |
        CARD_CTRL_ONEE_STATUS_ISOLATED_BIT |
        CARD_CTRL_ONEE_STATUS_ISOLATION_HOLD_BIT |
        CARD_CTRL_ONEE_STATUS_SELECTED_BIT;

    return ((status & active) != 0U) ? 1U : 0U;
}

static uint8_t vtw_iiplus_rom_patch_enabled(void)
{
    if (boot_menu_service_machine_mode() == CARD_MACHINE_MODE_IIPLUS) {
        /* Continue with the guarded patch below. */
    } else {
        return 0U;
    }

    /* Guard the exact LDA $C062 / LDA $C061 bytes. A future ROM image swap
     * must be re-audited rather than patched at stale offsets. */
    if ((apple2e_cpu_rom[0x02BBU] != 0xADU) ||
        (apple2e_cpu_rom[0x02BCU] != 0x62U) ||
        (apple2e_cpu_rom[0x02BDU] != 0xC0U) ||
        (apple2e_cpu_rom[0x02C3U] != 0xADU) ||
        (apple2e_cpu_rom[0x02C4U] != 0x61U) ||
        (apple2e_cpu_rom[0x02C5U] != 0xC0U)) {
        uart_puts(g_uart_base,
                  "vtw: ROM image changed, II+ button patch SKIPPED\r\n");
        return 0U;
    }

    uart_puts(g_uart_base,
              "vtw: II+ host, patching //e ROM reset button checks\r\n");
    return 1U;
}

static uint8_t vtw_shadow_force_cold_start(uint8_t require_onee_isolation)
{
    if (require_onee_isolation != 0U &&
        vtw_onee_isolation_confirmed() == 0U) {
        return 0U;
    }

    /* Invalidate the autostart warm signature. In-session resets remain warm. */
    REG_WRITE(CARD_CTRL_VTW_SHADOW_ADDR_REG, 0x003F3U);
    REG_WRITE(CARD_CTRL_VTW_SHADOW_DATA_REG, 0x00U);  /* $03F3 */
    REG_WRITE(CARD_CTRL_VTW_SHADOW_DATA_REG, 0x00U);  /* $03F4 */

    return (require_onee_isolation == 0U ||
            vtw_onee_isolation_confirmed() != 0U) ? 1U : 0U;
}

static uint8_t vtw_shadow_load_fixed_rom(uint8_t iiplus_patch,
                                         uint8_t require_onee_isolation)
{
    uint32_t i;

    REG_WRITE(CARD_CTRL_VTW_SHADOW_ADDR_REG, VTW_SHADOW_ROM_PHYS);
    for (i = 0U; i < VTW_ROM_BYTES; ++i) {
        uint8_t b = apple2e_cpu_rom[i];

        /* Check every byte. If the connector state changes during this
         * blocking copy, the caller clears VTW CTRL at the first failed
         * check instead of waiting for the next main-loop poll. */
        if (require_onee_isolation != 0U &&
            vtw_onee_isolation_confirmed() == 0U) {
            return 0U;
        }
        if (iiplus_patch != 0U) {
            uint32_t p;

            for (p = 0U;
                 p < (sizeof(k_iiplus_rom_patches) /
                      sizeof(k_iiplus_rom_patches[0]));
                 ++p) {
                if ((uint32_t)k_iiplus_rom_patches[p].off == i) {
                    b = k_iiplus_rom_patches[p].val;
                }
            }
        }
        REG_WRITE(CARD_CTRL_VTW_SHADOW_DATA_REG, (uint32_t)b);
    }

    return (require_onee_isolation == 0U ||
            vtw_onee_isolation_confirmed() != 0U) ? 1U : 0U;
}

static void vtw_apply_ctrl_options_live(void)
{
    (void)vtw_apply_ctrl_live();
}

static uint8_t vtw_ms_elapsed(XTime since, uint32_t ms)
{
    XTime now;

    XTime_GetTime(&now);
    return ((now - since) >= ((XTime)ms * (COUNTS_PER_SECOND / 1000U)))
               ? 1U : 0U;
}

static void vtw_session_stop(const char *reason)
{
    REG_WRITE(CARD_CTRL_VTW_CTRL_REG, vtw_ctrl_value(0U, 0U, 0U));
    if (g_state != VTW_ST_IDLE) {
        uart_puts(g_uart_base, "vtw: session off (");
        uart_puts(g_uart_base, reason);
        uart_puts(g_uart_base,
                  "); CTRL-RESET the Apple to resume the motherboard CPU\r\n");
    }
    g_state = VTW_ST_IDLE;
    g_announced_wait = 0U;
    g_announced_handoff_wait = 0U;
    vtw_override_clear();
}

void vtw_service_init(uint32_t uart_base)
{
    g_uart_base = uart_base;
    g_state = VTW_ST_IDLE;
    g_onee_running = 0U;
    g_onee_disk2_override_active = 0U;
    g_disk2_config_enabled = 0U;
    vtw_override_clear();
    REG_WRITE(CARD_CTRL_VTW_CTRL_REG, 0U);
    uart_puts(uart_base, "vtw: virtual TransWarp service ready\r\n");
}

void vtw_service_set_disk2_config_enabled(uint8_t enable)
{
    uint8_t effective_enable;

    /* Keep the saved host setting separate from the ONE//e session override.
     * Reset-time config reapply and live menu changes both pass here, so the
     * virtual track service cannot be turned off under a running ONE//e. The
     * latest saved value takes effect as soon as the override ends. */
    g_disk2_config_enabled = (enable != 0U) ? 1U : 0U;
    effective_enable = (g_onee_disk2_override_active != 0U) ?
        1U : g_disk2_config_enabled;
    disk2_service_set_enabled(effective_enable);
}

void vtw_service_set_enabled(uint8_t enable)
{
    g_intent_enabled = (enable != 0U) ? 1U : 0U;
    if (g_intent_enabled == 0U && g_state != VTW_ST_IDLE) {
        vtw_session_stop("disabled");
    }
}

uint8_t vtw_service_is_enabled(void)
{
    return g_intent_enabled;
}

void vtw_service_set_speed(uint8_t speed_mode, uint8_t pace_divider)
{
    g_speed_mode = (uint8_t)(speed_mode & CARD_CTRL_VTW_CTRL_SPEED_MASK);
    if (pace_divider < 2U) {
        pace_divider = 2U;
    }
    g_pace_divider = pace_divider;
    /* A configured (menu) speed change wins over any runtime override. */
    vtw_override_clear();
    /* Live update: rewrite CTRL with the current session bits intact. */
    (void)vtw_apply_ctrl_live();
}

void vtw_service_set_ignore_c074(uint8_t ignore)
{
    g_ignore_c074 = (ignore != 0U) ? 1U : 0U;

    /* Apply it live. The PL clears any old $C074 latch as soon as the bit
     * rises; turning it off starts from fast state until software writes a
     * new value. */
    vtw_apply_ctrl_options_live();
}

void vtw_service_set_disk2_accel_disabled(uint8_t disable)
{
    g_disable_disk2_accel = (disable != 0U) ? 1U : 0U;
    vtw_apply_ctrl_options_live();
}

/* ---- Runtime speed overrides (USB keymap actions) ------------------- */

static const char *vtw_eff_speed_name(void)
{
    const uint8_t mode = vtw_eff_mode();
    const uint16_t div = vtw_eff_divider();

    if (vtw_eff_is_slug() != 0U) {
        return "0.05 MHz slug";
    }
    for (uint32_t i = 0U; i < VTW_LADDER_COUNT; ++i) {
        if (k_vtw_ladder[i].mode == mode &&
            (mode != CARD_CTRL_VTW_SPEED_DIVIDED ||
             k_vtw_ladder[i].divider == div)) {
            return k_vtw_ladder[i].name;
        }
    }
    return "custom divider";
}

static uint8_t vtw_override_apply(const char *tag)
{
    const vtw_ctrl_live_result_t applied = vtw_apply_ctrl_live();
    const uint8_t queued = (applied == VTW_CTRL_LIVE_NONE) ? 1U : 0U;

    if (applied == VTW_CTRL_LIVE_FAILED) {
        uart_puts(g_uart_base, "vtw: ");
        uart_puts(g_uart_base, tag);
        uart_puts(g_uart_base, " failed; speed unchanged\r\n");
        (void)snprintf(g_last_action_text, sizeof(g_last_action_text),
                       "TW: CONTROL WRITE FAILED");
        return 0U;
    }
    uart_puts(g_uart_base, "vtw: ");
    uart_puts(g_uart_base, tag);
    uart_puts(g_uart_base,
              queued == 0U ? " -> " : " queued -> ");
    uart_puts(g_uart_base, vtw_eff_speed_name());
    uart_puts(g_uart_base, g_ovr_active != 0U ? " (override)\r\n"
                                              : " (configured)\r\n");
    (void)snprintf(g_last_action_text, sizeof(g_last_action_text),
                   queued == 0U ? "TW: %s" : "TW NEXT: %s",
                   vtw_eff_speed_name());
    return 1U;
}

/* Nearest ladder index for the current effective speed; slug maps below
 * the bottom so a step up leaves it at 1 MHz and a step down stays. */
static int vtw_eff_ladder_index(void)
{
    const uint8_t mode = vtw_eff_mode();
    const uint16_t div = vtw_eff_divider();
    int fastest_divided = 0;

    if (mode == CARD_CTRL_VTW_SPEED_FULL) {
        return (int)VTW_LADDER_COUNT - 1;
    }
    if (mode == CARD_CTRL_VTW_SPEED_1MHZ) {
        return 0;
    }
    if (div >= VTW_SLUG_DIVIDER) {
        return -1;                      /* slug: below the ladder */
    }
    /* The ladder is ordered slowest to fastest and divided-mode dividers
     * therefore decrease. Derive the index from the table itself so adding a
     * preset cannot leave a stale hard-coded top rung. */
    for (uint32_t i = 0U; i < VTW_LADDER_COUNT; ++i) {
        if (k_vtw_ladder[i].mode != CARD_CTRL_VTW_SPEED_DIVIDED) {
            continue;
        }
        fastest_divided = (int)i;
        if (div >= k_vtw_ladder[i].divider) {
            return (int)i;
        }
    }
    return fastest_divided;
}

static uint8_t vtw_speed_request_allowed(void)
{
    return (g_intent_enabled != 0U || g_state != VTW_ST_IDLE ||
            g_onee_running != 0U || vtw_onee_control_active() != 0U) ? 1U : 0U;
}

void vtw_service_set_slug_enabled(uint8_t enable)
{
    g_slug_enabled = enable ? 1U : 0U;
    /* Disarming the key also drops an active slug override: the setting
     * must leave no way to be stuck at 0.05 MHz unknowingly. */
    if (g_slug_enabled == 0U && vtw_eff_is_slug() != 0U) {
        const uint8_t old_ovr_active = g_ovr_active;
        const uint8_t old_ovr_mode = g_ovr_mode;
        const uint16_t old_ovr_div = g_ovr_div;
        vtw_ctrl_live_result_t applied;

        g_ovr_active = 0U;
        applied = vtw_apply_ctrl_live();
        if (applied == VTW_CTRL_LIVE_FAILED) {
            g_ovr_active = old_ovr_active;
            g_ovr_mode = old_ovr_mode;
            g_ovr_div = old_ovr_div;
            uart_puts(g_uart_base,
                      "vtw: slug disable CTRL write failed; speed unchanged\r\n");
            return;
        }
        if (applied == VTW_CTRL_LIVE_APPLIED) {
            uart_puts(g_uart_base, "vtw: slug disabled, speed restored\r\n");
        } else {
            uart_puts(g_uart_base,
                      "vtw: slug disabled for the next session\r\n");
        }
    }
}

const char *vtw_service_last_action_text(void)
{
    return g_last_action_text;
}

void vtw_service_speed_toggle(void)
{
    const uint8_t old_ovr_active = g_ovr_active;
    const uint8_t old_ovr_mode = g_ovr_mode;
    const uint16_t old_ovr_div = g_ovr_div;

    if (vtw_speed_request_allowed() == 0U) {
        uart_puts(g_uart_base, "vtw: speed toggle ignored (vtw off)\r\n");
        (void)snprintf(g_last_action_text, sizeof(g_last_action_text),
                       "TW: OFF");
        return;
    }
    if (g_ovr_active != 0U &&
        vtw_eff_mode() == CARD_CTRL_VTW_SPEED_1MHZ) {
        g_ovr_active = 0U;
    } else if (g_ovr_active == 0U &&
               g_speed_mode == CARD_CTRL_VTW_SPEED_1MHZ) {
        /* Configured speed IS 1 MHz: nothing to toggle to. */
    } else {
        g_ovr_active = 1U;
        g_ovr_mode = CARD_CTRL_VTW_SPEED_1MHZ;
        g_ovr_div = 0U;
    }
    if (vtw_override_apply("speed toggle") == 0U) {
        g_ovr_active = old_ovr_active;
        g_ovr_mode = old_ovr_mode;
        g_ovr_div = old_ovr_div;
    }
}

void vtw_service_speed_step(int8_t dir)
{
    int idx;
    const uint8_t old_ovr_active = g_ovr_active;
    const uint8_t old_ovr_mode = g_ovr_mode;
    const uint16_t old_ovr_div = g_ovr_div;

    if (vtw_speed_request_allowed() == 0U) {
        uart_puts(g_uart_base, "vtw: speed step ignored (vtw off)\r\n");
        (void)snprintf(g_last_action_text, sizeof(g_last_action_text),
                       "TW: OFF");
        return;
    }
    idx = vtw_eff_ladder_index() + (int)dir;
    if (idx < 0) {
        idx = 0;
    }
    if (idx >= (int)VTW_LADDER_COUNT) {
        idx = (int)VTW_LADDER_COUNT - 1;
    }
    g_ovr_active = 1U;
    g_ovr_mode = k_vtw_ladder[idx].mode;
    g_ovr_div = (k_vtw_ladder[idx].divider != 0U)
                    ? k_vtw_ladder[idx].divider : (uint16_t)g_pace_divider;
    if (vtw_override_apply(dir > 0 ? "speed +" : "speed -") == 0U) {
        g_ovr_active = old_ovr_active;
        g_ovr_mode = old_ovr_mode;
        g_ovr_div = old_ovr_div;
    }
}

void vtw_service_slug_toggle(void)
{
    const uint8_t old_ovr_active = g_ovr_active;
    const uint8_t old_ovr_mode = g_ovr_mode;
    const uint16_t old_ovr_div = g_ovr_div;

    if (vtw_speed_request_allowed() == 0U) {
        uart_puts(g_uart_base, "vtw: slug toggle ignored (vtw off)\r\n");
        (void)snprintf(g_last_action_text, sizeof(g_last_action_text),
                       "TW: OFF");
        return;
    }
    if (g_slug_enabled == 0U) {
        uart_puts(g_uart_base,
                  "vtw: slug key disabled (TransWarp tab)\r\n");
        (void)snprintf(g_last_action_text, sizeof(g_last_action_text),
                       "TW: SLUG KEY DISABLED");
        return;
    }
    if (vtw_eff_is_slug() != 0U) {
        g_ovr_active = 0U;
    } else {
        g_ovr_active = 1U;
        g_ovr_mode = CARD_CTRL_VTW_SPEED_DIVIDED;
        g_ovr_div = VTW_SLUG_DIVIDER;
    }
    if (vtw_override_apply("slug toggle") == 0U) {
        g_ovr_active = old_ovr_active;
        g_ovr_mode = old_ovr_mode;
        g_ovr_div = old_ovr_div;
    }
}

void vtw_service_set_slowdown(uint16_t region_mask, uint16_t cycles)
{
    uint32_t v;

    g_slowdown_mask   = (uint16_t)(region_mask & CARD_CTRL_VTW_SLOWDOWN_MASK_MASK);
    g_slowdown_cycles = cycles;

    /* The PL register is independent of the session: written directly and
     * held live by vtw_core_top. Region enables in [8:0], duration in
     * [31:16]; duration 0 disables the feature. */
    v = ((uint32_t)g_slowdown_cycles << CARD_CTRL_VTW_SLOWDOWN_DUR_SHIFT) |
        ((uint32_t)g_slowdown_mask   << CARD_CTRL_VTW_SLOWDOWN_MASK_SHIFT);
    REG_WRITE(CARD_CTRL_VTW_SLOWDOWN_REG, v);
}

uint8_t vtw_service_speed_mode(void)
{
    return g_speed_mode;
}

uint8_t vtw_service_pace_divider(void)
{
    return g_pace_divider;
}

uint8_t vtw_service_session_active(void)
{
    return (g_state == VTW_ST_RUN || g_onee_running != 0U) ? 1U : 0U;
}

uint8_t vtw_service_onee_start(uint8_t disk2_config_enabled)
{
    uint32_t poll;

    if (g_onee_running != 0U) {
        return vtw_service_onee_running();
    }
    if (g_state != VTW_ST_IDLE || vtw_onee_isolation_confirmed() == 0U) {
        return 0U;
    }

    /* ONE//e forces the virtual Disk II card into slot 6 even when the saved
     * slot mask leaves it off. Match that session-only PL override in the PS
     * track service, then apply the latest saved bit-6 state on every exit. */
    g_disk2_config_enabled =
        (disk2_config_enabled != 0U) ? 1U : 0U;
    g_onee_disk2_override_active = 1U;
    disk2_service_set_enabled(1U);

    /* Only this confirmed-isolation branch asserts the virtual reset. The
     * physical wrapper is already cut off, while ab_write_arb still carries
     * assert_res into apple_virtual_bus. Keep the soft core held too. */
    REG_WRITE(CARD_CTRL_VTW_CTRL_REG, vtw_onee_ctrl_value(0U, 1U));
    if (vtw_onee_isolation_confirmed() == 0U ||
        vtw_shadow_force_cold_start(1U) == 0U ||
        vtw_shadow_load_fixed_rom(0U, 1U) == 0U) {
        vtw_service_onee_stop();
        return 0U;
    }

    /* Release virtual RESET while the core remains held, then release the
     * core. Disk II private acceleration stays forced off in both writes. */
    REG_WRITE(CARD_CTRL_VTW_CTRL_REG, vtw_onee_ctrl_value(0U, 0U));
    if (vtw_onee_isolation_confirmed() == 0U) {
        vtw_service_onee_stop();
        return 0U;
    }
    REG_WRITE(CARD_CTRL_VTW_CTRL_REG, vtw_onee_ctrl_value(1U, 0U));

    for (poll = 0U; poll < VTW_ONEE_RUN_POLL_LIMIT; ++poll) {
        const uint32_t status = REG_READ(CARD_CTRL_VTW_STATUS_REG);

        if (vtw_onee_isolation_confirmed() == 0U) {
            break;
        }
        if ((status & (CARD_CTRL_VTW_STATUS_ENABLE_EFF |
                       CARD_CTRL_VTW_STATUS_CORE_RUN)) ==
                      (CARD_CTRL_VTW_STATUS_ENABLE_EFF |
                       CARD_CTRL_VTW_STATUS_CORE_RUN)) {
            g_onee_running = 1U;
            ++g_sessions_started;
            uart_puts(g_uart_base,
                      "vtw: ONE//e cold ROM loaded, core released\r\n");
            return 1U;
        }
    }

    vtw_service_onee_stop();
    uart_puts(g_uart_base, "vtw: ONE//e core release failed\r\n");
    return 0U;
}

void vtw_service_onee_stop(void)
{
    /* Literal zero: do not leave a host option bit or core-run request set. */
    REG_WRITE(CARD_CTRL_VTW_CTRL_REG, 0U);
    if (g_onee_running != 0U) {
        uart_puts(g_uart_base, "vtw: ONE//e core stopped\r\n");
    }
    g_onee_running = 0U;
    vtw_override_clear();
    if (g_onee_disk2_override_active != 0U) {
        g_onee_disk2_override_active = 0U;
        disk2_service_set_enabled(g_disk2_config_enabled);
    }
}

uint8_t vtw_service_onee_running(void)
{
    const uint32_t status = REG_READ(CARD_CTRL_VTW_STATUS_REG);
    const uint32_t required = CARD_CTRL_VTW_STATUS_ENABLE_EFF |
                              CARD_CTRL_VTW_STATUS_CORE_RUN;

    return (g_onee_running != 0U &&
            vtw_onee_isolation_confirmed() != 0U &&
            (status & required) == required) ? 1U : 0U;
}

void vtw_service_poll(void)
{
    if (g_onee_running != 0U) {
        if (vtw_service_onee_running() == 0U) {
            vtw_service_onee_stop();
        }
        return;
    }

    /* A manual ONE//e request owns the vTW control plane from request until
     * isolation fully clears. The saved host intent resumes only later. */
    if (vtw_onee_control_active() != 0U) {
        return;
    }

    switch (g_state) {
    case VTW_ST_IDLE:
        if (g_intent_enabled == 0U) {
            g_announced_handoff_wait = 0U;
            break;
        }
        if (boot_menu_service_machine_mode() != CARD_MACHINE_MODE_IIE &&
            boot_menu_service_machine_mode() != CARD_MACHINE_MODE_IIPLUS) {
            /* The PL gate would refuse anyway; wait for the boot ROM's
             * machine report (or a forced mode). Announce once. Both //e
             * and II/II+ accelerate as an Enhanced //e: the fixed //e ROM
             * and the full MMU model apply regardless of host. */
            if (g_announced_wait == 0U) {
                uart_puts(g_uart_base,
                          "vtw: waiting for machine identification\r\n");
                g_announced_wait = 1U;
            }
            break;
        }
        /* Take the bus only after the boot menu has handed slot 7 off to a
         * boot target. If we take over while slot 7 is still the boot menu,
         * the takeover RES# re-arms it and the accelerated //e ROM re-runs
         * the boot menu on the vTW core -- which misbehaves on a II+ (the 'A'
         * key is missed, the speaker screams) because the core re-executes
         * the menu + manufactured SmartPort handoff. Waiting for the handoff
         * lets the boot menu run ONCE, natively (un-accelerated), and the vTW
         * core then boots the target directly with slot 7 already in SmartPort
         * mode -- the same clean path as enabling acceleration on an
         * already-booted machine. No timeout: while the user is in the boot
         * menu slot 7 stays BOOTMENU, so we simply keep waiting until they
         * finish and it hands off. A machine with no boot target never hands
         * off and stays un-accelerated by design (reliability over the flaky
         * re-run). */
        if (boot_menu_service_slot7_handed_off() == 0U) {
            if (g_announced_handoff_wait == 0U) {
                uart_puts(g_uart_base,
                          "vtw: waiting for boot menu handoff\r\n");
                g_announced_handoff_wait = 1U;
            }
            break;
        }
        REG_WRITE(CARD_CTRL_VTW_CTRL_REG, vtw_ctrl_value(1U, 0U, 0U));
        g_take_polls = 0U;
        g_state = VTW_ST_TAKE_BUS;
        break;

    case VTW_ST_TAKE_BUS: {
        uint32_t st = REG_READ(CARD_CTRL_VTW_STATUS_REG);
        if ((st & CARD_CTRL_VTW_STATUS_BUS_OWNED) != 0U) {
            /* Reset the whole machine before the copy: the takeover can
             * land mid-protocol (boot menu open, command in flight), and
             * without a RES# edge the cards and PS would keep that state
             * while the vTW boots from scratch. This is our CTRL-RESET.
             * A //e engine releases /DMA for the duration of RES# low so its
             * MMU can process a stock reset, then re-takes automatically.
             * A II/II+ keeps /DMA asserted while releasing address/data so
             * the physical CPU can never escape. BUS_OWNED dips in either
             * case, which is why later states are timer-driven rather than
             * owned-polled. */
            uart_puts(g_uart_base, "vtw: bus taken, resetting machine\r\n");
            REG_WRITE(CARD_CTRL_VTW_CTRL_REG, vtw_ctrl_value(1U, 0U, 1U));
            XTime_GetTime(&g_res_phase_start);
            g_state = VTW_ST_RES_HOLD;
            break;
        }
        if (++g_take_polls > VTW_TAKE_POLL_LIMIT) {
            /* No PHI0, or the PL machine gate disagrees. Back off. */
            vtw_session_stop("bus takeover failed");
        }
        break;
    }

    case VTW_ST_RES_HOLD:
        if (vtw_ms_elapsed(g_res_phase_start, VTW_RES_HOLD_MS) != 0U) {
            REG_WRITE(CARD_CTRL_VTW_CTRL_REG, vtw_ctrl_value(1U, 0U, 0U));
            XTime_GetTime(&g_res_phase_start);
            g_state = VTW_ST_RES_SETTLE;
        }
        break;

    case VTW_ST_RES_SETTLE:
        if (vtw_ms_elapsed(g_res_phase_start, VTW_RES_SETTLE_MS) != 0U) {
            uart_puts(g_uart_base, "vtw: loading //e ROM\r\n");
            g_state = VTW_ST_LOAD_ROM;
        }
        break;

    case VTW_ST_LOAD_ROM: {
        /* Enhanced //e reset-path Apple-key checks, patched out on II/II+
         * hosts. The //e RESET routine reads Closed-Apple ($C062: pressed
         * -> JMP $C600 self-test) and Open-Apple ($C061: pressed -> forced
         * cold start) at ROM $C2BB/$C2C3. On a II/II+ those addresses are
         * the game-connector pushbuttons, which FLOAT HIGH with no
         * controller attached, so every accelerated boot entered the ROM
         * self-test (lo-res patterns + beeps) and every reset went cold;
         * the stray $C061 read also re-armed the boot menu's post-reset
         * Open-Apple snoop window. Replace both loads with LDA #$00 / NOP
         * so the checks read "not pressed". Gameplay button reads
         * elsewhere still hit the real bus, so attached controllers keep
         * working; //e hosts get the unmodified ROM. */
        const uint8_t iiplus_patch = vtw_iiplus_rom_patch_enabled();

        /* Force the autostart COLD path: the shadow persists across
         * sessions, so a re-takeover would otherwise warm-vector through
         * a stale ($03F2) into OS state that no longer matches the
         * re-booted cards. A real TW powers up with fresh RAM; this is
         * the equivalent. In-session CTRL-RESETs stay warm, as on a real
         * machine. */
        (void)vtw_shadow_force_cold_start(0U);

        /* Load the fixed Enhanced //e ROM into the shadow ROM region. The
         * image is embedded, so the takeover no longer reads the
         * motherboard ROM over the bus -- and every machine, //e or II+,
         * runs this same Enhanced //e ROM when accelerated. The shadow
         * data pointer auto-increments on each write. */
        (void)vtw_shadow_load_fixed_rom(iiplus_patch, 0U);

        REG_WRITE(CARD_CTRL_VTW_CTRL_REG, vtw_ctrl_value(1U, 1U, 0U));
        g_sessions_started++;
        uart_puts(g_uart_base, "vtw: core released, accelerating\r\n");
        g_state = VTW_ST_RUN;
        break;
    }

    case VTW_ST_RUN:
        /* Nothing hot: the PL runs the machine. Watch for the enable
         * gate dropping (machine mode override mid-session). */
        if ((REG_READ(CARD_CTRL_VTW_STATUS_REG) &
             CARD_CTRL_VTW_STATUS_ENABLE_EFF) == 0U) {
            vtw_session_stop("PL gate dropped");
        }
        break;

    default:
        g_state = VTW_ST_IDLE;
        break;
    }
}

void vtw_service_uart_status(uint32_t uart_base)
{
    char line[96];
    uint32_t st = REG_READ(CARD_CTRL_VTW_STATUS_REG);
    uint32_t post = REG_READ(CARD_CTRL_VTW_POST_STATS_REG);
    uint32_t wr_check = REG_READ(CARD_CTRL_VTW_WR_CHECK_REG);
    uint32_t wr_addr = REG_READ(CARD_CTRL_VTW_WR_ADDR_REG);
    uint32_t c000_ctx = REG_READ(CARD_CTRL_VTW_C000_CTX_REG);
    uint32_t c000_cnt = REG_READ(CARD_CTRL_VTW_C000_CNT_REG);
    uint32_t trace = REG_READ(CARD_CTRL_VTW_TRACE_STATUS_REG);
    uint32_t faults = REG_READ(CARD_CTRL_VTW_BUS_FAULTS_REG);
    uint32_t resetf = REG_READ(CARD_CTRL_RESET_FORENSICS_REG);
    static const char *const cycle_kind[4] = {
        "park", "post", "syncR", "syncW"
    };
    static const char *const trace_reason[4] = {
        "none", "reserved", "C000-bit7", "internal-C600"
    };

    snprintf(line, sizeof(line),
             "vtw: %s, session=%s, host-intent=%s, machine=%s\r\n",
             vtw_state_name(g_state),
             g_onee_running != 0U ? "ONE//e" :
             (g_state == VTW_ST_IDLE ? "off" : "host"),
             g_intent_enabled != 0U ? "on" : "off",
             boot_menu_service_machine_name());
    uart_puts(uart_base, line);
    snprintf(line, sizeof(line),
             "vtw: trace=%s reason=%s faults addr=%lu selfdata=%lu dmahigh=%lu\r\n",
             (trace & CARD_CTRL_VTW_TRACE_FROZEN_BIT) ? "frozen" : "armed",
             trace_reason[(trace >> CARD_CTRL_VTW_TRACE_REASON_SHIFT) &
                          CARD_CTRL_VTW_TRACE_REASON_MASK],
             (unsigned long)(faults >> 16),
             (unsigned long)((faults >> 8) & 0xFFU),
             (unsigned long)(faults & 0xFFU));
    uart_puts(uart_base, line);
    snprintf(line, sizeof(line),
             "vtw: reset seen=%lu source=%s%s rstn=$%lX\r\n",
             (unsigned long)((resetf &
                 CARD_CTRL_RESET_FORENSICS_RES_SEEN_BIT) ? 1U : 0U),
             (resetf & CARD_CTRL_RESET_FORENSICS_INTERNAL_BIT)
                 ? "Appletini" : "",
             (resetf & CARD_CTRL_RESET_FORENSICS_EXTERNAL_BIT)
                 ? ((resetf & CARD_CTRL_RESET_FORENSICS_INTERNAL_BIT)
                        ? "+external" : "external")
                 : ((resetf & CARD_CTRL_RESET_FORENSICS_INTERNAL_BIT)
                        ? "" : "none"),
             (unsigned long)(resetf &
                 CARD_CTRL_RESET_FORENSICS_RSTN_MASK));
    uart_puts(uart_base, line);

    if ((trace & CARD_CTRL_VTW_TRACE_FROZEN_BIT) != 0U) {
        for (uint32_t row = 0U; row < 2U; ++row) {
            int pos = snprintf(line, sizeof(line),
                               row == 0U ? "vtw: pc trail" :
                                           "vtw: pc trail+");
            for (uint32_t n = row * 4U; n < row * 4U + 4U; ++n) {
                uint32_t r = REG_READ(CARD_CTRL_VTW_PC_TRACE_REG(n));
                pos += snprintf(line + pos, sizeof(line) - (size_t)pos,
                                " %04lX %04lX",
                                (unsigned long)(r & 0xFFFFU),
                                (unsigned long)(r >> 16));
            }
            uart_puts(uart_base, line);
            uart_puts(uart_base, row == 1U ? " (new->old)\r\n" : "\r\n");
        }

        for (uint32_t row = 0U; row < 4U; ++row) {
            int pos = snprintf(line, sizeof(line),
                               row == 0U ? "vtw: io trail" :
                                           "vtw: io trail+");
            for (uint32_t n = row * 4U; n < row * 4U + 4U; ++n) {
                uint32_t e = REG_READ(CARD_CTRL_VTW_IO_TRACE_REG(n));
                pos += snprintf(line + pos, sizeof(line) - (size_t)pos,
                                " %c%04lX:%02lX/%c%c%c%c",
                                (e & 0x80000000UL) ? 'R' : 'W',
                                (unsigned long)((e >> 8) & 0xFFFFU),
                                (unsigned long)(e & 0xFFU),
                                (e & 0x40000000UL) ? 'S' : '-',
                                (e & 0x20000000UL) ? 'D' : '-',
                                (e & 0x10000000UL) ? 'A' : '-',
                                (e & 0x08000000UL) ? 'V' : '-');
            }
            uart_puts(uart_base, line);
            uart_puts(uart_base, row == 3U ? " (new->old)\r\n" : "\r\n");
        }
    }

    snprintf(line, sizeof(line),
             "vtw: slowdown mask=$%03X cycles=%u%s\r\n",
             (unsigned)g_slowdown_mask,
             (unsigned)g_slowdown_cycles,
             (g_slowdown_mask == 0U || g_slowdown_cycles == 0U)
                 ? " [inactive]" : "");
    uart_puts(uart_base, line);
    snprintf(line, sizeof(line),
             "vtw: speed=%s%s div=%u c074=%lu ignore=%u d2accel=%s owned=%lu run=%lu pc=%04lX\r\n",
             vtw_eff_speed_name(),
             g_ovr_active != 0U ? " (override)" : "",
             (unsigned)vtw_eff_divider(),
             (unsigned long)(st & CARD_CTRL_VTW_STATUS_C074_MASK),
             (unsigned)g_ignore_c074,
             g_disable_disk2_accel != 0U ? "off" : "on",
             (unsigned long)((st & CARD_CTRL_VTW_STATUS_BUS_OWNED) ? 1U : 0U),
             (unsigned long)((st & CARD_CTRL_VTW_STATUS_CORE_RUN) ? 1U : 0U),
             (unsigned long)(st >> CARD_CTRL_VTW_STATUS_PC_SHIFT));
    uart_puts(uart_base, line);
    snprintf(line, sizeof(line),
             "vtw: core=%lu bus=%lu posted=%lu invalid=%lu\r\n",
             (unsigned long)REG_READ(CARD_CTRL_VTW_CNT_CORE_REG),
             (unsigned long)REG_READ(CARD_CTRL_VTW_CNT_BUS_REG),
             (unsigned long)REG_READ(CARD_CTRL_VTW_CNT_POSTED_REG),
             (unsigned long)REG_READ(CARD_CTRL_VTW_CNT_INVALID_REG));
    uart_puts(uart_base, line);
    snprintf(line, sizeof(line),
             "vtw: qfill=%lu qhw=%lu qdrop=%lu sessions=%lu\r\n",
             (unsigned long)(post & 0x3FFU),
             (unsigned long)((post >> 16) & 0x3FFU),
             (unsigned long)(post >> 26),
             (unsigned long)g_sessions_started);
    uart_puts(uart_base, line);
    snprintf(line, sizeof(line),
             "vtw: wrchk mismatch=%lu last $%04lX expected=$%02lX observed=$%02lX\r\n",
             (unsigned long)(wr_check >> 16),
             (unsigned long)(wr_addr & 0xFFFFU),
             (unsigned long)((wr_check >> 8) & 0xFFU),
             (unsigned long)(wr_check & 0xFFU));
    uart_puts(uart_base, line);
    snprintf(line, sizeof(line),
             "vtw: c000 reads=%lu bit7=%lu last=$%02lX prev=%s $%04lX %c drive=%lu\r\n",
             (unsigned long)(c000_cnt & 0xFFFFU),
             (unsigned long)(c000_cnt >> 16),
             (unsigned long)(c000_ctx & 0xFFU),
             cycle_kind[(c000_ctx >> 14) & 0x3U],
             (unsigned long)(c000_ctx >> 16),
             ((c000_ctx & (1UL << 13)) != 0U) ? 'R' : 'W',
             (unsigned long)((c000_ctx >> 12) & 1U));
    uart_puts(uart_base, line);

    {
        uint32_t vsss = (st >> CARD_CTRL_VTW_STATUS_VSSS_SHIFT) &
                        CARD_CTRL_VTW_STATUS_VSSS_MASK;
        uint32_t last = REG_READ(CARD_CTRL_VTW_LAST_SYNC_REG);

        snprintf(line, sizeof(line),
                 "vtw: sw intcx=%lu slotc3=%lu intc8=%lu lc r/w/b2=%lu%lu%lu"
                 " altzp=%lu rd/wt=%lu%lu 80st=%lu text=%lu\r\n",
                 (unsigned long)((vsss >> 10) & 1U),
                 (unsigned long)((vsss >> 9) & 1U),
                 (unsigned long)((vsss >> 8) & 1U),
                 (unsigned long)((vsss >> 7) & 1U),
                 (unsigned long)((vsss >> 6) & 1U),
                 (unsigned long)((vsss >> 5) & 1U),
                 (unsigned long)((vsss >> 4) & 1U),
                 (unsigned long)((vsss >> 3) & 1U),
                 (unsigned long)((vsss >> 2) & 1U),
                 (unsigned long)((vsss >> 1) & 1U),
                 (unsigned long)(vsss & 1U));
        uart_puts(uart_base, line);
        {
            int pos = snprintf(line, sizeof(line), "vtw: sw trail");
            for (uint32_t n = 0U; n < 4U; ++n) {
                uint32_t r = REG_READ(CARD_CTRL_VTW_C0_RING_REG(n));
                for (uint32_t half = 0U; half < 2U; ++half) {
                    uint32_t e = (half == 0U) ? (r & 0xFFFFU) : (r >> 16);
                    pos += snprintf(line + pos, sizeof(line) - (size_t)pos,
                                    " %c%02lX:%02lX",
                                    (e & 0x2000U) ? 'r' : 'w',
                                    (unsigned long)((e >> 8) & 0x1FU),
                                    (unsigned long)(e & 0xFFU));
                }
            }
            uart_puts(uart_base, line);
            uart_puts(uart_base, " (new->old)\r\n");
        }
        {
            uint32_t r0 = REG_READ(CARD_CTRL_VTW_CXXX_RING_REG(0));
            uint32_t r1 = REG_READ(CARD_CTRL_VTW_CXXX_RING_REG(1));
            uint32_t r2 = REG_READ(CARD_CTRL_VTW_CXXX_RING_REG(2));
            uint32_t r3 = REG_READ(CARD_CTRL_VTW_CXXX_RING_REG(3));

            snprintf(line, sizeof(line),
                     "vtw: cxxx trail %04lX %04lX %04lX %04lX"
                     " %04lX %04lX %04lX %04lX (new->old)\r\n",
                     (unsigned long)(r0 & 0xFFFFU),
                     (unsigned long)(r0 >> 16),
                     (unsigned long)(r1 & 0xFFFFU),
                     (unsigned long)(r1 >> 16),
                     (unsigned long)(r2 & 0xFFFFU),
                     (unsigned long)(r2 >> 16),
                     (unsigned long)(r3 & 0xFFFFU),
                     (unsigned long)(r3 >> 16));
            uart_puts(uart_base, line);
        }
        snprintf(line, sizeof(line),
                 "vtw: last bus %s $%04lX = $%02lX, irqs seen=%lu%s\r\n",
                 (last & CARD_CTRL_VTW_LAST_SYNC_RW_BIT) ? "read" : "write",
                 (unsigned long)(last & 0xFFFFU),
                 (unsigned long)((last >> CARD_CTRL_VTW_LAST_SYNC_DATA_SHIFT)
                                 & 0xFFU),
                 (unsigned long)((last >> CARD_CTRL_VTW_LAST_SYNC_IRQ_SHIFT)
                                 & CARD_CTRL_VTW_LAST_SYNC_IRQ_MASK),
                 (((last >> CARD_CTRL_VTW_LAST_SYNC_IRQ_SHIFT)
                   & CARD_CTRL_VTW_LAST_SYNC_IRQ_MASK) ==
                  CARD_CTRL_VTW_LAST_SYNC_IRQ_MASK) ? " (sat)" : "");
        uart_puts(uart_base, line);
    }
}

void vtw_service_uart_dump(uint32_t uart_base, uint32_t phys, uint32_t len)
{
    char line[80];

    if (len > 0x1000U) {
        len = 0x1000U;
    }
    for (uint32_t row = 0U; row < len; row += 16U) {
        int pos = snprintf(line, sizeof(line), "%05lX:",
                           (unsigned long)((phys + row) & 0x3FFFFU));
        for (uint32_t i = 0U; i < 16U && (row + i) < len; ++i) {
            /* Writing the pointer register fetches that byte for the
             * data register (the shadow port-B sequencer finishes in a
             * few fabric clocks, far inside one AXI round trip). */
            REG_WRITE(CARD_CTRL_VTW_SHADOW_ADDR_REG,
                      (phys + row + i) & 0x3FFFFU);
            pos += snprintf(line + pos, sizeof(line) - (size_t)pos, " %02lX",
                            (unsigned long)(REG_READ(CARD_CTRL_VTW_SHADOW_DATA_REG)
                                            & 0xFFU));
        }
        uart_puts(uart_base, line);
        uart_puts(uart_base, "\r\n");
    }
}
