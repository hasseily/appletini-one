#include "boot_menu_service.h"

#include <stddef.h>

#include "boot_menu_rom_patch.h"
#include "card_control_regs.h"

#include "../lib/common.h"
#include "../lib/uart.h"

#define BOOT_MENU_MMIO_BASE             0x40040000U
#define BOOT_MENU_REG_STATUS           (BOOT_MENU_MMIO_BASE + 0x00U)
#define BOOT_MENU_REG_CONTROL          (BOOT_MENU_MMIO_BASE + 0x04U)
#define BOOT_MENU_REG_TIMEOUT_TICKS    (BOOT_MENU_MMIO_BASE + 0x08U)
#define BOOT_MENU_REG_KEYDATA          (BOOT_MENU_MMIO_BASE + 0x0CU)
#define BOOT_MENU_REG_HANDOFF_MODE     (BOOT_MENU_MMIO_BASE + 0x10U)
#define BOOT_MENU_REG_APPLE_STATUS     (BOOT_MENU_MMIO_BASE + 0x14U)
#define BOOT_MENU_REG_APPLE_TIMING     (BOOT_MENU_MMIO_BASE + 0x18U)
#define BOOT_MENU_REG_C8_PATCH         (BOOT_MENU_MMIO_BASE + 0x1CU)

#define BOOT_MENU_STATUS_MENU_REQUESTED    (1U << 2)
#define BOOT_MENU_STATUS_CLOSE_REQUESTED   (1U << 3)
#define BOOT_MENU_STATUS_MENU_ACTIVE_ACK   (1U << 5)
#define BOOT_MENU_STATUS_KEY_VALID         (1U << 6)

#define BOOT_MENU_CONTROL_SET_ACTIVE       (1U << 0)
#define BOOT_MENU_CONTROL_CLEAR_ACTIVE     (1U << 1)
#define BOOT_MENU_CONTROL_CLEAR_REQUEST    (1U << 2)
#define BOOT_MENU_CONTROL_CLEAR_CLOSE      (1U << 3)
#define BOOT_MENU_CONTROL_POP_KEY          (1U << 4)
#define BOOT_MENU_CONTROL_CLEAR_OVERFLOW   (1U << 5)
#define BOOT_MENU_CONTROL_REQUEST_ROM_CLOSE (1U << 7)

#define BOOT_MENU_APPLE_TIMING_VALID       (1U << 0)
#define BOOT_MENU_APPLE_TIMING_50HZ        (1U << 1)

#define BOOT_MENU_DEFAULT_TIMEOUT_TICKS    399000000U
#define BOOT_MENU_POP_BUDGET               8U

static uint8_t g_boot_menu_open_reported = 0U;

static void boot_menu_service_patch_c8_byte(uint8_t offset, uint8_t value)
{
    REG_WRITE(BOOT_MENU_REG_C8_PATCH,
              ((uint32_t)offset << 8) | (uint32_t)value);
}

static void boot_menu_service_set_vbl_delay(uint8_t x_count, uint8_t y_count)
{
    boot_menu_service_patch_c8_byte((uint8_t)BOOT_MENU_C8_PATCH_DELAY_X_OFFSET,
                                    x_count);
    boot_menu_service_patch_c8_byte((uint8_t)BOOT_MENU_C8_PATCH_DELAY_Y_OFFSET,
                                    y_count);
}

static void boot_menu_service_set_iiplus_vapor_delay(uint8_t x_count,
                                                     uint8_t y_count)
{
    boot_menu_service_patch_c8_byte(
        (uint8_t)BOOT_MENU_C8_PATCH_IIPLUS_VAPOR_X_OFFSET,
        x_count);
    boot_menu_service_patch_c8_byte(
        (uint8_t)BOOT_MENU_C8_PATCH_IIPLUS_VAPOR_Y_OFFSET,
        y_count);
}

void boot_menu_service_apply_video_rom_patch(void)
{
    const uint32_t timing = REG_READ(BOOT_MENU_REG_APPLE_TIMING);

    boot_menu_service_set_iiplus_vapor_delay(
        (uint8_t)BOOT_MENU_C8_IIPLUS_VAPOR_X,
        (uint8_t)BOOT_MENU_C8_IIPLUS_VAPOR_Y);

    if ((timing & (BOOT_MENU_APPLE_TIMING_VALID |
                   BOOT_MENU_APPLE_TIMING_50HZ)) ==
        (BOOT_MENU_APPLE_TIMING_VALID | BOOT_MENU_APPLE_TIMING_50HZ)) {
        boot_menu_service_set_vbl_delay((uint8_t)BOOT_MENU_C8_DELAY_PAL_X,
                                        (uint8_t)BOOT_MENU_C8_DELAY_PAL_Y);
        uart_puts(UART0_BASE, "boot menu VBL ROM patch: PAL\r\n");
    } else {
        boot_menu_service_set_vbl_delay((uint8_t)BOOT_MENU_C8_DELAY_NTSC_X,
                                        (uint8_t)BOOT_MENU_C8_DELAY_NTSC_Y);
        if ((timing & BOOT_MENU_APPLE_TIMING_VALID) != 0U) {
            uart_puts(UART0_BASE, "boot menu VBL ROM patch: NTSC\r\n");
        } else {
            uart_puts(UART0_BASE, "boot menu VBL ROM patch: NTSC default\r\n");
        }
    }
}

uint8_t boot_menu_service_is_apple_video_50hz(void)
{
    const uint32_t timing = REG_READ(BOOT_MENU_REG_APPLE_TIMING);

    return ((timing & (BOOT_MENU_APPLE_TIMING_VALID |
                       BOOT_MENU_APPLE_TIMING_50HZ)) ==
            (BOOT_MENU_APPLE_TIMING_VALID |
             BOOT_MENU_APPLE_TIMING_50HZ)) ? 1U : 0U;
}

static boot_menu_event_t boot_menu_no_event(void)
{
    boot_menu_event_t event;

    event.type = BOOT_MENU_EVENT_NONE;
    event.input.key = UI_KEY_NONE;
    event.input.pressed = 0U;
    event.input.ascii = 0U;
    event.raw_key = 0U;
    return event;
}

static uint8_t boot_menu_map_key(uint8_t raw_key, ui_input_t *input)
{
    const uint8_t ascii = raw_key & 0x7FU;

    if (input == NULL) {
        return 0U;
    }

    input->pressed = 1U;
    input->key = UI_KEY_NONE;
    input->ascii = 0U;

    switch (ascii) {
    case 0x1B:
        input->key = UI_KEY_BACK;
        break;
    case 0x09:
        input->key = UI_KEY_TAB;
        break;
    case 0x0D:
        input->key = UI_KEY_ENTER;
        break;
    case 0x0B:
        input->key = UI_KEY_PAGE_UP;
        break;
    case 0x0A:
        input->key = UI_KEY_PAGE_DOWN;
        break;
    case 0x08:
        input->key = UI_KEY_LEFT;
        break;
    case 0x15:
        input->key = UI_KEY_RIGHT;
        break;
    case 0x7F:
        input->key = UI_KEY_SHIFT_TAB;
        break;
    default:
        if (ascii >= 0x20U && ascii <= 0x7EU) {
            input->ascii = ascii;
            break;
        }
        input->pressed = 0U;
        return 0U;
    }

    return 1U;
}

void boot_menu_service_init(void)
{
    g_boot_menu_open_reported = 0U;
    boot_menu_service_apply_video_rom_patch();
    boot_menu_service_set_handoff(BOOT_MENU_HANDOFF_SMARTPORT);
    REG_WRITE(BOOT_MENU_REG_TIMEOUT_TICKS, BOOT_MENU_DEFAULT_TIMEOUT_TICKS);
    REG_WRITE(BOOT_MENU_REG_CONTROL,
              BOOT_MENU_CONTROL_CLEAR_ACTIVE |
              BOOT_MENU_CONTROL_CLEAR_REQUEST |
              BOOT_MENU_CONTROL_CLEAR_CLOSE |
              BOOT_MENU_CONTROL_CLEAR_OVERFLOW);
}

void boot_menu_service_set_timeout_ticks(uint32_t ticks)
{
    REG_WRITE(BOOT_MENU_REG_TIMEOUT_TICKS, ticks);
}

void boot_menu_service_set_handoff(boot_menu_handoff_t handoff)
{
    if (handoff == BOOT_MENU_HANDOFF_SMARTPORT) {
        uart_puts(UART0_BASE, "boot_menu_service_set_handoff: handoff set to SMARTPORT\r\n");
    } else if (handoff == BOOT_MENU_HANDOFF_DISK2) {
        uart_puts(UART0_BASE, "boot_menu_service_set_handoff: handoff set to DISK2\r\n");
    }
    if (handoff != BOOT_MENU_HANDOFF_SMARTPORT &&
        handoff != BOOT_MENU_HANDOFF_DISK2) {
        handoff = BOOT_MENU_HANDOFF_SMARTPORT;
        uart_puts(UART0_BASE, "boot_menu_service_set_handoff: invalid handoff mode\r\n");
    }
    REG_WRITE(BOOT_MENU_REG_HANDOFF_MODE, (uint32_t)handoff);
}

void boot_menu_service_request_rom_close(void)
{
    REG_WRITE(BOOT_MENU_REG_CONTROL, BOOT_MENU_CONTROL_REQUEST_ROM_CLOSE);
}

void boot_menu_service_debug_snapshot(boot_menu_debug_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }

    snapshot->status_word = REG_READ(BOOT_MENU_REG_STATUS);
    snapshot->timeout_ticks = REG_READ(BOOT_MENU_REG_TIMEOUT_TICKS);
    snapshot->handoff_mode_word = REG_READ(BOOT_MENU_REG_HANDOFF_MODE);
    snapshot->apple_status_byte = REG_READ(BOOT_MENU_REG_APPLE_STATUS) & 0xFFU;
}

/* ---- Machine identification --------------------------------------- */

static uint8_t g_machine_id_raw;       /* status_word[15:12]; 0 = none */
static uint8_t g_aux_card_present;     /* boot ROM probe: physical aux card */
static uint8_t g_aux_probe_seen;
static uint8_t g_machine_mode_applied = 0xFFU;  /* force first apply   */
static uint8_t g_machine_forced;

static const char *machine_mode_name(uint8_t mode)
{
    switch (mode) {
    case CARD_MACHINE_MODE_IIPLUS: return "II/II+";
    case CARD_MACHINE_MODE_IIE:    return "IIe";
    case CARD_MACHINE_MODE_IIGS:   return "IIgs";
    default:                       return "unknown";
    }
}

static uint8_t machine_mode_from_id(uint8_t id)
{
    switch (id) {
    case 1U: return CARD_MACHINE_MODE_IIPLUS;
    case 2U:
    case 3U: return CARD_MACHINE_MODE_IIE;   /* IIe / enhanced IIe */
    case 4U: return CARD_MACHINE_MODE_IIGS;
    default: return CARD_MACHINE_MODE_UNKNOWN; /* incl. 5=IIc: no slots,
                                                * a report of it means
                                                * something is off --
                                                * stay safe. */
    }
}

/* From main.c: the config menu's RAM tab enable. Both settings are
 * valid working configurations: enabled = the card provides 64K aux
 * (+RamWorks) via INH; disabled = a physical aux-slot card provides
 * it and the Appletini leaves those cycles alone. */
extern uint8_t appletini_config_ram_enabled(void);
extern uint8_t appletini_config_ramworks_enabled(void);

static uint8_t g_aux_provide_applied = 0xFFU;
static uint8_t g_ramworks_applied = 0xFFU;

#define CARD_CTRL_RAMWORKS_EN_REG   CARD_CTRL_REG_ADDR(0x62U)
#define CARD_CTRL_AUX_PROBE_REG     CARD_CTRL_REG_ADDR(0x6AU)

static void machine_refresh_aux_policy(void)
{
    const uint8_t want =
        (g_machine_mode_applied == CARD_MACHINE_MODE_IIE &&
         g_aux_card_present == 0U &&
         appletini_config_ram_enabled() != 0U) ? 1U : 0U;
    /* RamWorks banking requires the Appletini to own base aux: with a
     * physical card providing bank 0 we must never serve banks >0
     * (the real card answers every aux cycle regardless of bank). */
    const uint8_t rw_want =
        (want != 0U &&
         appletini_config_ramworks_enabled() != 0U) ? 1U : 0U;
    if (want != g_aux_provide_applied) {
        g_aux_provide_applied = want;
        REG_WRITE(CARD_CTRL_AUX_PROVIDE_REG, (uint32_t)want);
        uart_puts(UART0_BASE, want ? "machine: aux provide ON\r\n"
                                   : "machine: aux provide OFF\r\n");
    }
    if (rw_want != g_ramworks_applied) {
        g_ramworks_applied = rw_want;
        REG_WRITE(CARD_CTRL_RAMWORKS_EN_REG, (uint32_t)rw_want);
        uart_puts(UART0_BASE, rw_want ? "machine: ramworks 8MB ON\r\n"
                                      : "machine: ramworks OFF\r\n");
    }
}

static void machine_apply_mode(uint8_t mode)
{
    if (mode == g_machine_mode_applied) {
        machine_refresh_aux_policy();
        return;
    }
    g_machine_mode_applied = mode;
    REG_WRITE(CARD_CTRL_MACHINE_MODE_REG, (uint32_t)mode);
    machine_refresh_aux_policy();
    uart_puts(UART0_BASE, "machine: mode -> ");
    uart_puts(UART0_BASE, machine_mode_name(mode));
    uart_puts(UART0_BASE, "\r\n");
}

static void machine_observe_status(uint32_t status)
{
    const uint8_t id = (uint8_t)((status >> 12) & 0xFU);

    if (id != 0U && id != g_machine_id_raw) {
        g_machine_id_raw = id;
        uart_puts(UART0_BASE, "machine: boot ROM reports id ");
        uart_putdec(UART0_BASE, id);
        uart_puts(UART0_BASE, "\r\n");
    }
    if (id != 0U) {
        const uint32_t probe = REG_READ(CARD_CTRL_AUX_PROBE_REG);
        if ((probe & 2U) != 0U) {   /* fresh report, once per boot */
            g_aux_card_present = (uint8_t)(probe & 1U);
            g_aux_probe_seen = 1U;
            uart_puts(UART0_BASE, g_aux_card_present
                ? "machine: physical aux card detected\r\n"
                : "machine: no aux card - Appletini may provide\r\n");
            /* The ROM's probe force-dropped aux_provide in the PL;
             * invalidate the dedup so policy rewrites it, and consume
             * the report (write-one-to-clear) so this runs once. */
            g_aux_provide_applied = 0xFFU;
            REG_WRITE(CARD_CTRL_AUX_PROBE_REG, 1U);
        }
    }
    if (g_machine_forced == 0U) {
        machine_apply_mode(machine_mode_from_id(g_machine_id_raw));
    }
}

uint8_t boot_menu_service_aux_card_present(void)
{
    return g_aux_card_present;
}

uint8_t boot_menu_service_machine_id(void)
{
    return g_machine_id_raw;
}

uint8_t boot_menu_service_machine_mode(void)
{
    return (g_machine_mode_applied == 0xFFU) ? CARD_MACHINE_MODE_UNKNOWN
                                             : g_machine_mode_applied;
}

const char *boot_menu_service_machine_name(void)
{
    return machine_mode_name(boot_menu_service_machine_mode());
}

void boot_menu_service_force_machine_mode(int mode)
{
    if (mode < 0) {
        g_machine_forced = 0U;
        machine_apply_mode(machine_mode_from_id(g_machine_id_raw));
        return;
    }
    g_machine_forced = 1U;
    machine_apply_mode((uint8_t)mode);
}

uint8_t boot_menu_service_machine_forced(void)
{
    return g_machine_forced;
}

void boot_menu_service_refresh_machine_policy(void)
{
    machine_observe_status(REG_READ(BOOT_MENU_REG_STATUS));
}

boot_menu_event_t boot_menu_service_poll(void)
{
    uint32_t budget;

    boot_menu_service_refresh_machine_policy();

    for (budget = 0U; budget < BOOT_MENU_POP_BUDGET; ++budget) {
        const uint32_t status = REG_READ(BOOT_MENU_REG_STATUS);
        boot_menu_event_t event = boot_menu_no_event();

        if ((status & BOOT_MENU_STATUS_MENU_REQUESTED) != 0U &&
            g_boot_menu_open_reported == 0U) {
            g_boot_menu_open_reported = 1U;
            REG_WRITE(BOOT_MENU_REG_CONTROL,
                      BOOT_MENU_CONTROL_SET_ACTIVE |
                      BOOT_MENU_CONTROL_CLEAR_OVERFLOW);
            event.type = BOOT_MENU_EVENT_OPEN;
            return event;
        }

        if ((status & BOOT_MENU_STATUS_CLOSE_REQUESTED) != 0U) {
            g_boot_menu_open_reported = 0U;
            REG_WRITE(BOOT_MENU_REG_CONTROL,
                      BOOT_MENU_CONTROL_CLEAR_ACTIVE |
                      BOOT_MENU_CONTROL_CLEAR_REQUEST |
                      BOOT_MENU_CONTROL_CLEAR_CLOSE |
                      BOOT_MENU_CONTROL_CLEAR_OVERFLOW);
            event.type = BOOT_MENU_EVENT_CLOSE;
            return event;
        }

        if ((status & BOOT_MENU_STATUS_KEY_VALID) != 0U) {
            const uint32_t keydata = REG_READ(BOOT_MENU_REG_KEYDATA);
            const uint8_t raw_key = (uint8_t)(keydata & 0xFFU);

            REG_WRITE(BOOT_MENU_REG_CONTROL, BOOT_MENU_CONTROL_POP_KEY);
            if (g_boot_menu_open_reported != 0U &&
                boot_menu_map_key(raw_key, &event.input) != 0U) {
                event.type = BOOT_MENU_EVENT_INPUT;
                event.raw_key = raw_key;
                return event;
            }
            continue;
        }

        if ((status & (BOOT_MENU_STATUS_MENU_REQUESTED |
                       BOOT_MENU_STATUS_MENU_ACTIVE_ACK)) == 0U) {
            g_boot_menu_open_reported = 0U;
        }
        return event;
    }

    return boot_menu_no_event();
}
