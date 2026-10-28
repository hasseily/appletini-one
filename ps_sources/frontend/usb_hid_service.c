/******************************************************************************
 * Appletini USB1 HID service
 *
 * CherryUSB host adapter for USB1. CherryUSB owns USB enumeration, hubs, split
 * transactions, and HID class binding; this file bridges HID input reports into
 * the slot-2 MouseCard register block and config-menu events.
 ******************************************************************************/

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "xiltimer.h"
#include "xparameters.h"
#include "xusbps_hw.h"

#include "usb_hid_service.h"
#include "https_probe.h"
#include "usb_gamepad_input.h"
#include "usb_ps4_report.h"
#include "usb_xbox_one.h"
#include "usb_xbox_360.h"
#include "usb_controller_identity.h"
/* Collection, relative-input and identity handling adapted from Multitini One
 * 6f2a90286956bf8e4956dfc9b94c94dfc3335b05 (GPL-3.0-only). */
#include "onee_input_service.h"
#include "slot2_gamepad_service.h"

#include "cherryusb_platform.h"
#include "usbh_core.h"
#include "usbh_hid.h"
#include "usb_gamepad_descriptor.h"
#include "usb_def.h"
#include "usb_errno.h"
#include "usb_hid.h"
#include "onee_usb_controls.h"

#include "../lib/uart.h"

#ifdef XPAR_XUSBPS_1_BASEADDR
#define USB1_BASE XPAR_XUSBPS_1_BASEADDR
#else
#define USB1_BASE 0xE0003000U
#endif

#define REG_READ(addr) (*(volatile uint32_t *)(uintptr_t)(addr))
#define REG_WRITE(addr, value) (*(volatile uint32_t *)(uintptr_t)(addr) = (uint32_t)(value))

#define MOUSE_SENSITIVITY_MIN 3U
#define MOUSE_SENSITIVITY_MAX 150U
#define MOUSE_SENSITIVITY_BASE 100U
#define MOUSE_MENU_EVENT_DEPTH 16U
#define MOUSE_MENU_HOLD_TICKS ((XTime)COUNTS_PER_SECOND)

#define MOUSE_BUTTON_LEFT 0x01U
#define MOUSE_BUTTON_RIGHT 0x02U
#define MOUSE_BUTTON_MIDDLE 0x04U
#define MOUSE_BUTTON_4 0x08U
#define MOUSE_BUTTON_5 0x10U
#define MOUSE_BUTTON_APPLE_MASK (MOUSE_BUTTON_LEFT | MOUSE_BUTTON_RIGHT)
#define MOUSE_BOOT_WHEEL_INDEX 3U

#define USB_HID_SLOT_COUNT ONEE_INPUT_DEVICE_SLOT_COUNT
#define HID_REPORT_BYTES 64U
#define HID_REPORT_BUFFER_BYTES USB_ALIGN_UP(HID_REPORT_BYTES, CONFIG_USB_ALIGN_SIZE)
#define HID_REPORT_DESC_BYTES CONFIG_USBHOST_REQUEST_BUFFER_LEN
#define HID_KEY_TRACK_COUNT 64U
#define HID_SOURCE_TRACK_COUNT (HID_KEY_TRACK_COUNT + 8U)
#define HID_HAT_NEUTRAL 8U
#define HID_REPORT_RETRY_MS 10U
#define HID_RELATIVE_PULSE_TICKS ((XTime)((COUNTS_PER_SECOND / 1000U) * 20U))
#define HID_REPORT_RETRY_TICKS \
    ((XTime)((COUNTS_PER_SECOND / 1000U) * HID_REPORT_RETRY_MS))

#define MOUSE_MMIO_BASE 0x40050000U
#define MOUSE_REG(idx) (MOUSE_MMIO_BASE + ((idx) * 4U))
#define MOUSE_REG_STATUS MOUSE_REG(0)
#define MOUSE_REG_X MOUSE_REG(1)
#define MOUSE_REG_Y MOUSE_REG(2)
#define MOUSE_REG_BUTTONS MOUSE_REG(3)
#define MOUSE_REG_COMMIT MOUSE_REG(4)
#define MOUSE_REG_MODE MOUSE_REG(5)
#define MOUSE_REG_X_MIN MOUSE_REG(6)
#define MOUSE_REG_X_MAX MOUSE_REG(7)
#define MOUSE_REG_Y_MIN MOUSE_REG(8)
#define MOUSE_REG_Y_MAX MOUSE_REG(9)

typedef struct {
    uint8_t index;
    uint8_t active;
    uint8_t vendor_gamepad;
    uint8_t ps4;
    uint8_t boot_mouse;
    uint8_t boot_keyboard;
    uint8_t keyboard_keys_down;
    uint8_t raw_buttons_down;
    uint16_t raw_axis_active_mask;
    uint16_t raw_axis_rest_valid_mask;
    uint8_t raw_hat_active;
    uint8_t mouse_capable;
    uint8_t onee_joystick;
    uint8_t generic_axes;
    uint8_t native_mouse_buttons;
    uint8_t mouse_card;
    uint8_t report_info_valid;
    uint8_t report_pending;
    uint8_t report_retry_armed;
    uint8_t prev_buttons;
    uint32_t gamepad_buttons;
    uint32_t prev_gamepad_buttons;
    uint8_t gamepad_tab;
    uint32_t fixed_buttons;
    uint8_t fixed_tab;
    uint8_t fixed_hat;
    uint8_t gamepad_dpad;
    uint8_t apple_buttons;
    usb_hid_menu_source_t prev_sources[HID_SOURCE_TRACK_COUNT];
    uint8_t prev_hat;
    int8_t prev_x_dir;
    int8_t prev_y_dir;
    uint8_t open_close_down;
    uint8_t open_close_hold_fired;
    XTime open_close_down_tick;
    usb_hid_menu_source_t open_close_down_source;
    uint8_t ok_down;
    uint8_t ok_hold_fired;
    XTime ok_down_tick;
    usb_hid_menu_source_t ok_down_source;
    usb_hid_menu_action_t ok_down_action;
    int32_t raw_axis_rest[ONEE_INPUT_AXIS_COUNT];
    struct usbh_hid *hid;
    struct usbh_hid_report_info report_info;
    appletini_usb_hid_descriptor_meta_t descriptor_meta;
    uint8_t keyboard_state[APPLETINI_USB_HID_MAX_KEYBOARD_ITEMS][32];
    uint16_t relative_pending;
    uint16_t relative_active;
    XTime relative_started[ONEE_INPUT_AXIS_COUNT];
    int8_t relative_queue[ONEE_INPUT_AXIS_COUNT][32];
    uint8_t relative_head[ONEE_INPUT_AXIS_COUNT];
    uint8_t relative_count[ONEE_INPUT_AXIS_COUNT];
    uint32_t report_count;
    uint32_t submit_error_count;
    uint32_t transfer_error_count;
    int last_error;
    uint8_t error_log_suppressed;
    XTime report_retry_started;
    uint8_t interface_subclass;
    uint8_t interface_protocol;
} usb_hid_slot_t;

static uint8_t g_started;
/* A failed PHY shutdown must be retried before the next host start. */
static uint8_t g_power_shutdown_required;
static uint8_t g_ready;
static uint8_t g_seq;
static uint8_t g_sensitivity = MOUSE_SENSITIVITY_BASE;
static uint8_t g_menu_capture;
static uint8_t g_joystick_preview;
static uint8_t g_binding_capture;
static uint32_t g_gamepad_binding_mask;
static uint8_t g_onee_fixed_mode;
static uint8_t g_onee_standby_mode;
static uint8_t g_onee_input_blocked;
static usb_hid_menu_event_t g_menu_events[MOUSE_MENU_EVENT_DEPTH];
static uint8_t g_menu_event_rd;
static uint8_t g_menu_event_wr;
static uint8_t g_menu_event_count;
static usb_hid_menu_source_t g_menu_ok_source = USB_HID_MENU_ACTION_SELECT;
static usb_hid_menu_source_t g_menu_open_close_source = USB_HID_MENU_ACTION_SELECT;
static usb_hid_menu_source_t g_screenshot_a2_source = USB_HID_MENU_SOURCE_NONE;
static usb_hid_menu_source_t g_screenshot_1080p_source = USB_HID_MENU_SOURCE_NONE;
static usb_hid_menu_source_t g_vtw_sources[USB_HID_VTW_SOURCE_COUNT];
static int32_t g_x;
static int32_t g_y;
static int32_t g_x_residue;
static int32_t g_y_residue;

static usb_hid_slot_t g_hid_slots[USB_HID_SLOT_COUNT];
static uint8_t g_reports[USB_HID_SLOT_COUNT][HID_REPORT_BUFFER_BYTES]
    __attribute__((aligned(CONFIG_USB_ALIGN_SIZE)));
static uint8_t g_report_descs[USB_HID_SLOT_COUNT][HID_REPORT_DESC_BYTES]
    __attribute__((aligned(CONFIG_USB_ALIGN_SIZE)));

static uint32_t g_report_count;
static uint32_t g_submit_error_count;
static uint32_t g_transfer_error_count;
static int g_last_error;

static void hid_resubmit_report(usb_hid_slot_t *slot);
static void hid_slots_retry_reports(void);
static uint8_t hid_source_in_list(usb_hid_menu_source_t source,
                                  const usb_hid_menu_source_t *sources,
                                  uint32_t count);

static const char *usb_speed_name(uint8_t speed)
{
    switch (speed) {
    case USB_SPEED_LOW:
        return "low";
    case USB_SPEED_FULL:
        return "full";
    case USB_SPEED_HIGH:
        return "high";
    case USB_SPEED_SUPER:
        return "super";
    default:
        return "unknown";
    }
}

usb_hid_menu_source_t usb_hid_menu_source_from_keyboard_usage(uint8_t usage)
{
    if (usage <= HID_KBD_USAGE_ERRUNDEF || usage > HID_KBD_USAGE_MAX) {
        return USB_HID_MENU_SOURCE_NONE;
    }
    return (usb_hid_menu_source_t)(USB_HID_MENU_SOURCE_KEY_BASE | usage);
}

uint8_t usb_hid_menu_source_is_keyboard(usb_hid_menu_source_t source)
{
    const uint8_t usage = (uint8_t)(source & USB_HID_MENU_SOURCE_KEY_MASK);

    return ((source & (usb_hid_menu_source_t)~USB_HID_MENU_SOURCE_KEY_MASK) ==
                USB_HID_MENU_SOURCE_KEY_BASE &&
            usb_hid_menu_source_from_keyboard_usage(usage) == source) ? 1U : 0U;
}

usb_hid_menu_source_t usb_hid_menu_source_from_gamepad_button(uint8_t button)
{
    return button < USB_HID_MENU_SOURCE_GAMEPAD_COUNT ?
        (usb_hid_menu_source_t)(USB_HID_MENU_SOURCE_GAMEPAD_BASE + button) :
        USB_HID_MENU_SOURCE_NONE;
}

uint8_t usb_hid_menu_source_is_gamepad(usb_hid_menu_source_t source)
{
    return source >= USB_HID_MENU_SOURCE_GAMEPAD_BASE &&
           source < USB_HID_MENU_SOURCE_GAMEPAD_BASE + USB_HID_MENU_SOURCE_GAMEPAD_COUNT;
}

static const char *hid_keyboard_usage_label(uint8_t usage)
{
    static const char * const letters[] = {
        "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
        "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z"
    };
    static const char * const numbers[] = {
        "1", "2", "3", "4", "5", "6", "7", "8", "9", "0"
    };
    static const char * const fkeys_1_12[] = {
        "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10",
        "F11", "F12"
    };
    static const char * const fkeys_13_24[] = {
        "F13", "F14", "F15", "F16", "F17", "F18", "F19", "F20", "F21",
        "F22", "F23", "F24"
    };
    static const char * const keypad_numbers[] = {
        "KP 1", "KP 2", "KP 3", "KP 4", "KP 5", "KP 6", "KP 7", "KP 8",
        "KP 9", "KP 0"
    };

    if (usage >= HID_KBD_USAGE_A && usage < (HID_KBD_USAGE_A + 26U)) {
        return letters[usage - HID_KBD_USAGE_A];
    }
    if (usage >= HID_KBD_USAGE_1 && usage <= HID_KBD_USAGE_0) {
        return numbers[usage - HID_KBD_USAGE_1];
    }
    if (usage >= HID_KBD_USAGE_F1 && usage <= HID_KBD_USAGE_F12) {
        return fkeys_1_12[usage - HID_KBD_USAGE_F1];
    }
    if (usage >= HID_KBD_USAGE_F13 && usage <= HID_KBD_USAGE_F24) {
        return fkeys_13_24[usage - HID_KBD_USAGE_F13];
    }
    if (usage >= HID_KBD_USAGE_KPD1 && usage <= HID_KBD_USAGE_KPD0) {
        return keypad_numbers[usage - HID_KBD_USAGE_KPD1];
    }

    switch (usage) {
    case HID_KBD_USAGE_ENTER:
        return "Enter";
    case HID_KBD_USAGE_ESCAPE:
        return "Esc";
    case HID_KBD_USAGE_DELETE:
        return "Backspace";
    case HID_KBD_USAGE_TAB:
        return "Tab";
    case HID_KBD_USAGE_SPACE:
        return "Space";
    case HID_KBD_USAGE_HYPHEN:
        return "-";
    case HID_KBD_USAGE_EQUAL:
        return "=";
    case HID_KBD_USAGE_LBRACKET:
        return "[";
    case HID_KBD_USAGE_RBRACKET:
        return "]";
    case HID_KBD_USAGE_BSLASH:
        return "\\";
    case HID_KBD_USAGE_NONUSPOUND:
        return "Non-US #";
    case HID_KBD_USAGE_SEMICOLON:
        return ";";
    case HID_KBD_USAGE_SQUOTE:
        return "'";
    case HID_KBD_USAGE_GACCENT:
        return "`";
    case HID_KBD_USAGE_COMMON:
        return ",";
    case HID_KBD_USAGE_PERIOD:
        return ".";
    case HID_KBD_USAGE_DIV:
        return "/";
    case HID_KBD_USAGE_CAPSLOCK:
        return "Caps";
    case HID_KBD_USAGE_PRINTSCN:
        return "Print";
    case HID_KBD_USAGE_SCROLLLOCK:
        return "Scroll";
    case HID_KBD_USAGE_PAUSE:
        return "Pause";
    case HID_KBD_USAGE_INSERT:
        return "Insert";
    case HID_KBD_USAGE_HOME:
        return "Home";
    case HID_KBD_USAGE_PAGEUP:
        return "PgUp";
    case HID_KBD_USAGE_DELFWD:
        return "Delete";
    case HID_KBD_USAGE_END:
        return "End";
    case HID_KBD_USAGE_PAGEDOWN:
        return "PgDn";
    case HID_KBD_USAGE_RIGHT:
        return "Right";
    case HID_KBD_USAGE_LEFT:
        return "Left";
    case HID_KBD_USAGE_DOWN:
        return "Down";
    case HID_KBD_USAGE_UP:
        return "Up";
    case HID_KBD_USAGE_KPDNUMLOCK:
        return "KP Num";
    case HID_KBD_USAGE_KPDDIV:
        return "KP /";
    case HID_KBD_USAGE_KPDMUL:
        return "KP *";
    case HID_KBD_USAGE_KPDHMINUS:
        return "KP -";
    case HID_KBD_USAGE_KPDPLUS:
        return "KP +";
    case HID_KBD_USAGE_KPDEMTER:
        return "KP Enter";
    case HID_KBD_USAGE_KPDDECIMALPT:
        return "KP .";
    case HID_KBD_USAGE_NONSLASH:
        return "Non-US \\";
    case HID_KBD_USAGE_APPLICATION:
        return "App";
    case HID_KBD_USAGE_POWER:
        return "Power";
    case HID_KBD_USAGE_KPDEQUAL:
        return "KP =";
    case HID_KBD_USAGE_MENU:
        return "Menu";
    case HID_KBD_USAGE_LCTRL:
        return "L Ctrl";
    case HID_KBD_USAGE_LSHIFT:
        return "L Shift";
    case HID_KBD_USAGE_LALT:
        return "L Alt";
    case HID_KBD_USAGE_LGUI:
        return "L GUI";
    case HID_KBD_USAGE_RCTRL:
        return "R Ctrl";
    case HID_KBD_USAGE_RSHIFT:
        return "R Shift";
    case HID_KBD_USAGE_RALT:
        return "R Alt";
    case HID_KBD_USAGE_RGUI:
        return "R GUI";
    default:
        return NULL;
    }
}

const char *usb_hid_menu_source_text(usb_hid_menu_source_t source)
{
    static char text[24];
    const uint8_t usage = (uint8_t)(source & USB_HID_MENU_SOURCE_KEY_MASK);
    const char *label;

    if (usb_hid_menu_source_is_gamepad(source)) {
        (void)snprintf(text, sizeof(text), "Gamepad %u",
            (unsigned)(source - USB_HID_MENU_SOURCE_GAMEPAD_BASE + 1U));
        return text;
    }
    if (usb_hid_menu_source_is_keyboard(source) == 0U) {
        return "None";
    }

    label = hid_keyboard_usage_label(usage);
    if (label != NULL) {
        (void)snprintf(text, sizeof(text), "USB %s", label);
    } else {
        (void)snprintf(text, sizeof(text), "USB Key %02X", (unsigned)usage);
    }
    return text;
}

static int32_t mouse_clamp_16bit(int32_t value)
{
    if (value < 0) {
        return 0;
    }
    if (value > 0xFFFF) {
        return 0xFFFF;
    }
    return value;
}

/* The card's clamp windows are full 16-bit (AppleWin-normalized, so
 * min <= max after every firmware commit; keep the guard as a safety). */
static int32_t mouse_clamp_range(int32_t value, uint32_t min_value, uint32_t max_value)
{
    min_value &= 0xFFFFU;
    max_value &= 0xFFFFU;
    if (max_value < min_value) {
        max_value = min_value;
    }
    if (value < (int32_t)min_value) {
        return (int32_t)min_value;
    }
    if (value > (int32_t)max_value) {
        return (int32_t)max_value;
    }
    return value;
}

static int32_t mouse_scale_delta(int32_t delta, int32_t *residue)
{
    int32_t scaled;
    int32_t whole;

    if (residue == NULL) {
        return delta;
    }

    scaled = *residue + (delta * (int32_t)g_sensitivity);
    whole = scaled / (int32_t)MOUSE_SENSITIVITY_BASE;
    *residue = scaled - (whole * (int32_t)MOUSE_SENSITIVITY_BASE);
    return whole;
}

static void mouse_menu_push_event(usb_hid_menu_action_t action,
                                  usb_hid_menu_source_t source)
{
    if ((action == USB_HID_MENU_ACTION_NONE &&
         source == USB_HID_MENU_SOURCE_NONE) ||
        g_menu_event_count >= MOUSE_MENU_EVENT_DEPTH) {
        return;
    }

    g_menu_events[g_menu_event_wr].action = action;
    g_menu_events[g_menu_event_wr].source = source;
    g_menu_events[g_menu_event_wr].fixed_action = USB_HID_MENU_ACTION_NONE;
    g_menu_event_wr = (uint8_t)((g_menu_event_wr + 1U) % MOUSE_MENU_EVENT_DEPTH);
    g_menu_event_count++;
}

static void mouse_menu_push_action(usb_hid_menu_action_t action)
{
    mouse_menu_push_event(action, USB_HID_MENU_SOURCE_NONE);
}

static void mouse_menu_push_bindable_action(usb_hid_menu_action_t action)
{
    mouse_menu_push_event(action, (usb_hid_menu_source_t)action);
}

static usb_hid_menu_action_t mouse_menu_action_from_source(usb_hid_menu_source_t source)
{
    return (source >= USB_HID_MENU_ACTION_LEFT &&
            source <= USB_HID_MENU_ACTION_PREV_TAB) ?
        (usb_hid_menu_action_t)source : USB_HID_MENU_ACTION_NONE;
}

static void keyboard_menu_push_source(usb_hid_menu_source_t source)
{
    mouse_menu_push_event(USB_HID_MENU_ACTION_NONE, source);
}

static uint8_t screenshot_source_valid(usb_hid_menu_source_t source)
{
    return (source == USB_HID_MENU_SOURCE_NONE ||
            usb_hid_menu_source_is_keyboard(source) != 0U ||
            usb_hid_menu_source_is_gamepad(source) != 0U) ? 1U : 0U;
}

static uint8_t screenshot_push_source(usb_hid_menu_source_t source)
{
    if (source == USB_HID_MENU_SOURCE_NONE) {
        return 0U;
    }
    if (source == g_screenshot_a2_source) {
        mouse_menu_push_event(USB_HID_MENU_ACTION_SCREENSHOT_A2, source);
        return 1U;
    }
    if (source == g_screenshot_1080p_source) {
        mouse_menu_push_event(USB_HID_MENU_ACTION_SCREENSHOT_1080P, source);
        return 1U;
    }
    return 0U;
}

/* Virtual-TransWarp speed actions: global keyboard bindings that fire
 * whether or not the menu is open, same class as the screenshots. */
static uint8_t vtw_push_source(usb_hid_menu_source_t source)
{
    static const usb_hid_menu_action_t k_vtw_actions[USB_HID_VTW_SOURCE_COUNT] = {
        USB_HID_MENU_ACTION_VTW_SPEED_TOGGLE,
        USB_HID_MENU_ACTION_VTW_SPEED_UP,
        USB_HID_MENU_ACTION_VTW_SPEED_DOWN,
        USB_HID_MENU_ACTION_VTW_SLUG_TOGGLE
    };

    if (source == USB_HID_MENU_SOURCE_NONE) {
        return 0U;
    }
    for (uint32_t i = 0U; i < USB_HID_VTW_SOURCE_COUNT; ++i) {
        if (source == g_vtw_sources[i]) {
            mouse_menu_push_event(k_vtw_actions[i], source);
            return 1U;
        }
    }
    return 0U;
}

static uint8_t mouse_menu_source_button_mask(usb_hid_menu_source_t source)
{
    switch (source) {
    case USB_HID_MENU_ACTION_LEFT:
        return MOUSE_BUTTON_LEFT;
    case USB_HID_MENU_ACTION_RIGHT:
        return MOUSE_BUTTON_RIGHT;
    case USB_HID_MENU_ACTION_SELECT:
        return MOUSE_BUTTON_MIDDLE;
    case USB_HID_MENU_ACTION_ITEM_DOWN:
        return MOUSE_BUTTON_4;
    case USB_HID_MENU_ACTION_ITEM_UP:
        return MOUSE_BUTTON_5;
    default:
        return 0U;
    }
}

static uint8_t menu_hold_source_valid(usb_hid_menu_source_t source)
{
    return (mouse_menu_source_button_mask(source) != 0U ||
            usb_hid_menu_source_is_keyboard(source) != 0U ||
            usb_hid_menu_source_is_gamepad(source) != 0U) ? 1U : 0U;
}

static void hid_slot_reset_menu_state(usb_hid_slot_t *slot)
{
    if (slot == NULL) {
        return;
    }
    slot->prev_buttons = 0U;
    slot->prev_gamepad_buttons = slot->gamepad_buttons;
    slot->gamepad_tab = 0U;
    /* Keep keyboard edges across menu and policy changes. A held Pause or
     * Windows key must not fire again as soon as capture changes. Full slot
     * reset clears these sources when a device connects or disconnects. */
    slot->prev_hat = HID_HAT_NEUTRAL;
    slot->prev_x_dir = 0;
    slot->prev_y_dir = 0;
    slot->open_close_down = 0U;
    slot->open_close_hold_fired = 0U;
    slot->open_close_down_tick = 0U;
    slot->open_close_down_source = USB_HID_MENU_SOURCE_NONE;
    slot->ok_down = 0U;
    slot->ok_hold_fired = 0U;
    slot->ok_down_tick = 0U;
    slot->ok_down_source = USB_HID_MENU_SOURCE_NONE;
    slot->ok_down_action = USB_HID_MENU_ACTION_NONE;
}

static void hid_slot_reset(usb_hid_slot_t *slot)
{
    uint8_t index;

    if (slot == NULL) {
        return;
    }
    index = slot->index;
    memset(slot, 0, sizeof(*slot));
    slot->index = index;
    slot->fixed_hat = HID_HAT_NEUTRAL;
    hid_slot_reset_menu_state(slot);
}

static void hid_slots_reset_all(void)
{
    slot2_gamepad_service_release_all();
    for (uint32_t i = 0U; i < USB_HID_SLOT_COUNT; ++i) {
        usb_controller_identity_disconnect((uint8_t)i);
        g_hid_slots[i].index = (uint8_t)i;
        hid_slot_reset(&g_hid_slots[i]);
    }
}

static void hid_slots_reset_menu_state(void)
{
    for (uint32_t i = 0U; i < USB_HID_SLOT_COUNT; ++i) {
        hid_slot_reset_menu_state(&g_hid_slots[i]);
    }
}

static usb_hid_slot_t *hid_slot_from_hid(struct usbh_hid *hid)
{
    if (hid == NULL) {
        return NULL;
    }
    for (uint32_t i = 0U; i < USB_HID_SLOT_COUNT; ++i) {
        usb_hid_slot_t *slot = &g_hid_slots[i];

        if (slot->active != 0U && slot->vendor_gamepad == 0U &&
            slot->hid == hid && hid->user_data == slot) {
            return slot;
        }
    }
    for (uint32_t i = 0U; i < USB_HID_SLOT_COUNT; ++i) {
        usb_hid_slot_t *slot = &g_hid_slots[i];

        if (slot->active != 0U && slot->vendor_gamepad == 0U &&
            slot->hid == hid) {
            return slot;
        }
    }
    return NULL;
}

static usb_hid_slot_t *hid_slot_find_free(uint8_t preferred)
{
    if (preferred < USB_HID_SLOT_COUNT &&
        g_hid_slots[preferred].active == 0U) {
        return &g_hid_slots[preferred];
    }
    for (uint32_t i = 0U; i < USB_HID_SLOT_COUNT; ++i) {
        if (g_hid_slots[i].active == 0U) {
            return &g_hid_slots[i];
        }
    }
    return NULL;
}

static uint8_t g_controller_string[256] __attribute__((aligned(CONFIG_USB_ALIGN_SIZE)));
static uint8_t g_controller_string_busy;

static size_t controller_string_descriptor(struct usbh_hubport *port,
    uint8_t index, uint16_t language)
{
    struct usb_setup_packet *setup = port->setup;
    struct usbh_urb *control = &port->ep0_urb;
    if (!port->connected || !setup || control->errorcode == -USB_ERR_BUSY) return 0U;
    usb_osal_mutex_take(port->mutex);
    setup->bmRequestType = USB_REQUEST_DIR_IN | USB_REQUEST_STANDARD | USB_REQUEST_RECIPIENT_DEVICE;
    setup->bRequest = USB_REQUEST_GET_DESCRIPTOR;
    setup->wValue = (uint16_t)((USB_DESCRIPTOR_TYPE_STRING << 8U) | index);
    setup->wIndex = language;
    setup->wLength = 255U;
    memset(g_controller_string, 0, sizeof(g_controller_string));
    usbh_control_urb_fill(control, port, setup, g_controller_string, 255U, 50U, NULL, NULL);
    int result = usbh_submit_urb(control);
    if (result == 0) result = (int)control->actual_length;
    if (result == -USB_ERR_NOMEM && control->hcpriv == NULL) control->errorcode = 0;
    control->timeout = 0U;
    usb_osal_mutex_give(port->mutex);
    if (result < 2 || g_controller_string[1] != USB_DESCRIPTOR_TYPE_STRING ||
        g_controller_string[0] < 2U || (g_controller_string[0] & 1U) || g_controller_string[0] > result) return 0U;
    return g_controller_string[0];
}

static void controller_optional_strings(usb_controller_device_t *device, struct usbh_hubport *port)
{
    const uint8_t product = !device->name[0] && port->device_desc.iProduct;
    const uint8_t serial = !device->identity.serial_hash && port->device_desc.iSerialNumber;
    if ((!product && !serial) || g_controller_string_busy) return;
    g_controller_string_busy = 1U;
    uint16_t language = 0x0409U;
    if (controller_string_descriptor(port, 0U, 0U) >= 4U)
        language = (uint16_t)((uint16_t)g_controller_string[2] | ((uint16_t)g_controller_string[3] << 8U));
    if (product) {
        const size_t length = controller_string_descriptor(port, port->device_desc.iProduct, language);
        size_t out = 0U;
        for (size_t at = 2U; at + 1U < length && out + 1U < sizeof(device->name); at += 2U) {
            const uint8_t ascii = g_controller_string[at];
            device->name[out++] = !g_controller_string[at + 1U] && ascii >= 32U && ascii < 127U ? (char)ascii : '?';
        }
        device->name[out] = 0;
    }
    if (serial) {
        const size_t length = controller_string_descriptor(port, port->device_desc.iSerialNumber, language);
        if (length > 2U) device->identity.serial_hash = usb_controller_identity_hash(
            UINT32_C(2166136261), g_controller_string + 2U, length - 2U);
    }
    g_controller_string_busy = 0U;
}

static void identify_controller(uint8_t slot, struct usbh_hubport *port,
    uint8_t interface_id, uint8_t transport, uint32_t descriptor_hash,
    uint32_t buttons, uint16_t axes, uint8_t generic_axes)
{
    if (slot >= USB_HID_SLOT_COUNT || !port) return;
    usb_controller_device_t device = {0};
    device.connected = 1U;
    device.generic_axes = generic_axes;
    device.buttons_present = buttons;
    device.axis_mask = axes;
    device.identity.vendor_id = port->device_desc.idVendor;
    device.identity.product_id = port->device_desc.idProduct;
    device.identity.interface_id = interface_id;
    device.identity.transport = transport;
    device.identity.descriptor_hash = descriptor_hash;
    uint32_t path = UINT32_C(2166136261);
    const struct usbh_hubport *ancestor = port;
    for (unsigned depth = 0U; ancestor && depth < 16U; ++depth) {
        path = usb_controller_identity_hash(path, &ancestor->port, 1U);
        ancestor = ancestor->parent ? ancestor->parent->parent : NULL;
    }
    device.identity.port_path = path;
    /* Read serial bytes in one representation on every connection. The
     * CherryUSB optional cached string can differ in encoding or truncation. */
    if (port->iProduct && port->iProduct[0])
        (void)snprintf(device.name, sizeof(device.name), "%s", port->iProduct);
    controller_optional_strings(&device, port);
    if (!device.name[0]) {
        const char *name = transport == USB_CONTROLLER_TRANSPORT_XBOX360 ? "Xbox 360" :
            transport == USB_CONTROLLER_TRANSPORT_XBOXONE ? "Xbox One" :
            transport == USB_CONTROLLER_TRANSPORT_PS4 ? "DualShock 4" : "USB input";
        (void)snprintf(device.name, sizeof(device.name), "%s %04X:%04X", name,
            (unsigned)device.identity.vendor_id, (unsigned)device.identity.product_id);
    }
    usb_controller_identity_set(slot, &device);
}

void usb_gamepad_input_identify(uint8_t slot, struct usbh_hubport *port,
    uint8_t interface_id, uint8_t transport)
{
    identify_controller(slot, port, interface_id, transport, UINT32_C(0x80000000) | transport,
        transport == USB_CONTROLLER_TRANSPORT_PS4 ? 0x3fffU : 0x7ffU, 0x3fU, 0U);
}

int usb_gamepad_input_connect(void)
{
    usb_hid_slot_t *slot = hid_slot_find_free(USB_HID_SLOT_COUNT);

    if (slot == NULL) {
        return -1;
    }
    onee_input_service_disconnect(slot->index);
    slot2_gamepad_service_disconnect(slot->index);
    hid_slot_reset(slot);
    slot->active = 1U;
    slot->vendor_gamepad = 1U;
    slot->onee_joystick = 1U;
    return slot->index;
}

void usb_gamepad_input_disconnect(uint8_t index)
{
    usb_hid_slot_t *slot;

    if (index >= USB_HID_SLOT_COUNT) {
        return;
    }
    slot = &g_hid_slots[index];
    if (slot->active == 0U || slot->vendor_gamepad == 0U) {
        return;
    }
    onee_input_service_disconnect(slot->index);
    slot2_gamepad_service_disconnect(slot->index);
    usb_controller_identity_disconnect(slot->index);
    hid_slot_reset(slot);
}

static uint32_t hid_active_count(void)
{
    uint32_t count = 0U;

    for (uint32_t i = 0U; i < USB_HID_SLOT_COUNT; ++i) {
        if (g_hid_slots[i].active != 0U &&
            g_hid_slots[i].vendor_gamepad == 0U) {
            count++;
        }
    }
    return count;
}

static uint32_t hid_pending_count(void)
{
    uint32_t count = 0U;

    for (uint32_t i = 0U; i < USB_HID_SLOT_COUNT; ++i) {
        if (g_hid_slots[i].active != 0U &&
            g_hid_slots[i].report_pending != 0U) {
            count++;
        }
    }
    return count;
}

static uint8_t menu_source_is_down(const usb_hid_slot_t *slot,
                                   usb_hid_menu_source_t source)
{
    const uint8_t mouse_mask = mouse_menu_source_button_mask(source);

    if (slot == NULL || source == USB_HID_MENU_SOURCE_NONE) {
        return 0U;
    }
    if (usb_hid_menu_source_is_gamepad(source)) {
        return (slot->gamepad_buttons &
            (UINT32_C(1) << (source - USB_HID_MENU_SOURCE_GAMEPAD_BASE))) != 0U;
    }
    if (mouse_mask != 0U) {
        return ((slot->prev_buttons & mouse_mask) != 0U) ? 1U : 0U;
    }
    if (usb_hid_menu_source_is_keyboard(source) != 0U) {
        return hid_source_in_list(source,
                                  slot->prev_sources,
                                  HID_SOURCE_TRACK_COUNT);
    }
    return 0U;
}

static void menu_start_open_close_hold(usb_hid_slot_t *slot,
                                       usb_hid_menu_source_t source)
{
    if (slot == NULL || source != g_menu_open_close_source) {
        return;
    }
    slot->open_close_down = 1U;
    slot->open_close_hold_fired = 0U;
    slot->open_close_down_source = source;
    XTime_GetTime(&slot->open_close_down_tick);
}

static void menu_finish_open_close_hold(usb_hid_slot_t *slot,
                                        usb_hid_menu_source_t source)
{
    if (slot == NULL ||
        slot->open_close_down == 0U ||
        slot->open_close_down_source != source) {
        return;
    }
    slot->open_close_down = 0U;
    slot->open_close_hold_fired = 0U;
    slot->open_close_down_tick = 0U;
    slot->open_close_down_source = USB_HID_MENU_SOURCE_NONE;
}

static void menu_poll_open_close_hold(usb_hid_slot_t *slot)
{
    XTime now_tick = 0U;

    if (slot == NULL ||
        slot->open_close_down == 0U ||
        slot->open_close_hold_fired != 0U ||
        slot->open_close_down_source != g_menu_open_close_source ||
        menu_source_is_down(slot, g_menu_open_close_source) == 0U) {
        return;
    }

    XTime_GetTime(&now_tick);
    if ((now_tick - slot->open_close_down_tick) >= MOUSE_MENU_HOLD_TICKS) {
        mouse_menu_push_action((g_menu_capture == 0U) ?
            USB_HID_MENU_ACTION_OPEN : USB_HID_MENU_ACTION_CLOSE);
        slot->open_close_hold_fired = 1U;
        if (slot->ok_down != 0U &&
            slot->ok_down_source == slot->open_close_down_source) {
            slot->ok_hold_fired = 1U;
        }
    }
}

static void hid_slots_poll_holds(void)
{
    for (uint32_t i = 0U; i < USB_HID_SLOT_COUNT; ++i) {
        if (g_hid_slots[i].active != 0U) {
            menu_poll_open_close_hold(&g_hid_slots[i]);
        }
    }
}

static void mouse_menu_start_ok_hold(usb_hid_slot_t *slot)
{
    if (slot == NULL) {
        return;
    }
    slot->ok_down = 1U;
    slot->ok_hold_fired = 0U;
    slot->ok_down_source = g_menu_ok_source;
    slot->ok_down_action = USB_HID_MENU_ACTION_NONE;
    XTime_GetTime(&slot->ok_down_tick);
}

static void mouse_menu_finish_ok_hold(usb_hid_slot_t *slot)
{
    if (slot == NULL) {
        return;
    }
    if (slot->ok_down != 0U &&
        slot->ok_hold_fired == 0U &&
        g_menu_capture != 0U) {
        mouse_menu_push_event(mouse_menu_action_from_source(slot->ok_down_source),
                              slot->ok_down_source);
    }
    slot->ok_down = 0U;
    slot->ok_hold_fired = 0U;
    slot->ok_down_tick = 0U;
    slot->ok_down_source = USB_HID_MENU_SOURCE_NONE;
    slot->ok_down_action = USB_HID_MENU_ACTION_NONE;
}

static void mouse_menu_push_button_edge(usb_hid_slot_t *slot,
                                        uint8_t edge_down,
                                        usb_hid_menu_action_t action)
{
    const usb_hid_menu_source_t source = (usb_hid_menu_source_t)action;
    const uint8_t mask = mouse_menu_source_button_mask((usb_hid_menu_source_t)action);

    if (slot == NULL || mask == 0U || (edge_down & mask) == 0U) {
        return;
    }
    if (source == g_menu_open_close_source) {
        menu_start_open_close_hold(slot, source);
        if (g_menu_capture != 0U && source == g_menu_ok_source) {
            mouse_menu_start_ok_hold(slot);
        }
        return;
    }
    if (g_menu_capture != 0U && source == g_menu_ok_source) {
        mouse_menu_start_ok_hold(slot);
        return;
    }
    if (g_menu_capture != 0U) {
        mouse_menu_push_bindable_action(action);
    }
}

static void mouse_menu_process_buttons(usb_hid_slot_t *slot,
                                       uint8_t buttons,
                                       int8_t wheel)
{
    uint8_t edge_down;
    uint8_t edge_up;

    if (slot == NULL) {
        return;
    }

    buttons &= (MOUSE_BUTTON_LEFT |
                MOUSE_BUTTON_RIGHT |
                MOUSE_BUTTON_MIDDLE |
                MOUSE_BUTTON_4 |
                MOUSE_BUTTON_5);
    edge_down = (uint8_t)(buttons & (uint8_t)~slot->prev_buttons);
    edge_up = (uint8_t)((uint8_t)~buttons & slot->prev_buttons);

    if (g_menu_capture != 0U) {
        if (wheel > 0) {
            mouse_menu_push_bindable_action(USB_HID_MENU_ACTION_PREV_TAB);
        } else if (wheel < 0) {
            mouse_menu_push_bindable_action(USB_HID_MENU_ACTION_NEXT_TAB);
        }
    }

    mouse_menu_push_button_edge(slot, edge_down, USB_HID_MENU_ACTION_LEFT);
    mouse_menu_push_button_edge(slot, edge_down, USB_HID_MENU_ACTION_RIGHT);
    mouse_menu_push_button_edge(slot, edge_down, USB_HID_MENU_ACTION_SELECT);
    mouse_menu_push_button_edge(slot, edge_down, USB_HID_MENU_ACTION_ITEM_DOWN);
    mouse_menu_push_button_edge(slot, edge_down, USB_HID_MENU_ACTION_ITEM_UP);

    if ((edge_up & mouse_menu_source_button_mask(g_menu_ok_source)) != 0U) {
        mouse_menu_finish_ok_hold(slot);
    }
    if ((edge_up & mouse_menu_source_button_mask(g_menu_open_close_source)) != 0U) {
        menu_finish_open_close_hold(slot, g_menu_open_close_source);
    }

    slot->prev_buttons = buttons;
    menu_poll_open_close_hold(slot);
}

static uint8_t hid_legacy_menu_buttons(const usb_hid_slot_t *slot)
{
    uint8_t legacy = slot->generic_axes ? 0U :
        (uint8_t)(slot->gamepad_buttons & ~g_gamepad_binding_mask & 0x0cU);
    if (g_menu_capture && !g_binding_capture && !g_joystick_preview &&
        !(g_gamepad_binding_mask & UINT32_C(0x30))) legacy &= (uint8_t)~0x10U;
    return (uint8_t)(slot->native_mouse_buttons | legacy);
}

/* Fixed controller controls and explicit bindings share one physical event.
 * The consumer runs identical actions once and keeps different custom actions.
 * Mouse/keypad axis devices retain their own controls and opt-in bindings. */
static usb_hid_menu_action_t gamepad_menu_fixed_button_action(uint8_t button)
{
    switch (button) {
    case USB_GAMEPAD_BUTTON_A: return USB_HID_MENU_ACTION_SELECT;
    case USB_GAMEPAD_BUTTON_B: return USB_HID_MENU_ACTION_BACK;
    case USB_GAMEPAD_BUTTON_LEFT_SHOULDER: return USB_HID_MENU_ACTION_PREV_TAB;
    case USB_GAMEPAD_BUTTON_RIGHT_SHOULDER: return USB_HID_MENU_ACTION_NEXT_TAB;
    case USB_GAMEPAD_BUTTON_HOME: return USB_HID_MENU_ACTION_TOGGLE;
    default: return USB_HID_MENU_ACTION_NONE;
    }
}

static void gamepad_menu_push_fixed(usb_hid_menu_action_t fixed_action,
    usb_hid_menu_source_t source)
{
    if (g_menu_event_count >= MOUSE_MENU_EVENT_DEPTH) return;
    usb_hid_menu_action_t action = USB_HID_MENU_ACTION_NONE;
    if (source != USB_HID_MENU_SOURCE_NONE) {
        if (source == g_screenshot_a2_source) action = USB_HID_MENU_ACTION_SCREENSHOT_A2;
        else if (source == g_screenshot_1080p_source) action = USB_HID_MENU_ACTION_SCREENSHOT_1080P;
        else {
            static const usb_hid_menu_action_t actions[USB_HID_VTW_SOURCE_COUNT] = {
                USB_HID_MENU_ACTION_VTW_SPEED_TOGGLE, USB_HID_MENU_ACTION_VTW_SPEED_UP,
                USB_HID_MENU_ACTION_VTW_SPEED_DOWN, USB_HID_MENU_ACTION_VTW_SLUG_TOGGLE
            };
            for (unsigned i = 0U; i < USB_HID_VTW_SOURCE_COUNT; ++i)
                if (source == g_vtw_sources[i]) { action = actions[i]; break; }
        }
    }
    g_menu_events[g_menu_event_wr].action = action;
    g_menu_events[g_menu_event_wr].source = source;
    g_menu_events[g_menu_event_wr].fixed_action = fixed_action;
    g_menu_event_wr = (uint8_t)((g_menu_event_wr + 1U) % MOUSE_MENU_EVENT_DEPTH);
    ++g_menu_event_count;
}

static void gamepad_menu_process_buttons(usb_hid_slot_t *slot)
{
    const uint32_t buttons = slot->gamepad_buttons;
    const uint32_t pressed = buttons & ~slot->prev_gamepad_buttons;
    const uint32_t released = ~buttons & slot->prev_gamepad_buttons;
    const uint32_t fixed_pressed = buttons & ~slot->fixed_buttons;
    const uint32_t shoulders = buttons & UINT32_C(0x30);
    const uint8_t tab = shoulders == 0x10U ? 1U : shoulders == 0x20U ? 2U : 0U;
    const uint8_t previous_tab = slot->fixed_tab;
    const uint32_t home = UINT32_C(1) << USB_GAMEPAD_BUTTON_HOME;
    const uint8_t fixed = !slot->generic_axes;
    slot->prev_gamepad_buttons = buttons;
    slot->fixed_buttons = buttons;
    slot->fixed_tab = tab;

    if (fixed && (fixed_pressed & home)) {
        /* Home takes this report before a released custom OK can select a
         * row. Cancel pending holds without dispatching their actions. */
        slot->ok_down = 0U;
        slot->ok_hold_fired = 0U;
        slot->ok_down_tick = 0U;
        slot->ok_down_source = USB_HID_MENU_SOURCE_NONE;
        slot->ok_down_action = USB_HID_MENU_ACTION_NONE;
        menu_finish_open_close_hold(slot, slot->open_close_down_source);
        gamepad_menu_push_fixed(USB_HID_MENU_ACTION_TOGGLE,
            usb_hid_menu_source_from_gamepad_button(USB_GAMEPAD_BUTTON_HOME));
        return;
    }
    for (uint8_t button = 0U; button < USB_HID_MENU_SOURCE_GAMEPAD_COUNT; ++button) {
        const uint32_t bit = UINT32_C(1) << button;
        const usb_hid_menu_source_t source = usb_hid_menu_source_from_gamepad_button(button);
        if (released & bit) {
            if (slot->ok_down && slot->ok_down_source == source) mouse_menu_finish_ok_hold(slot);
            menu_finish_open_close_hold(slot, source);
        }
    }
    if (!g_binding_capture && !slot->generic_axes)
        mouse_menu_process_buttons(slot, hid_legacy_menu_buttons(slot), 0);

    for (uint8_t button = 0U; button < USB_HID_MENU_SOURCE_GAMEPAD_COUNT; ++button) {
        const uint32_t bit = UINT32_C(1) << button;
        const usb_hid_menu_source_t source = usb_hid_menu_source_from_gamepad_button(button);
        const usb_hid_menu_action_t fixed_action = fixed ? gamepad_menu_fixed_button_action(button) : USB_HID_MENU_ACTION_NONE;
        const uint8_t physical_press = (fixed_pressed & bit) != 0U;
        if (!(pressed & bit) && !physical_press) continue;
        if (fixed_action != USB_HID_MENU_ACTION_NONE) {
            if (g_binding_capture) {
                /* The consumer learns this source before any fixed action. */
                gamepad_menu_push_fixed(fixed_action, source);
                continue;
            }
            if (buttons & home) continue;
            if (button == USB_GAMEPAD_BUTTON_HOME) continue;
            if (g_menu_capture && physical_press &&
                (button == USB_GAMEPAD_BUTTON_A || button == USB_GAMEPAD_BUTTON_B)) {
                gamepad_menu_push_fixed(fixed_action, source);
            } else if ((g_gamepad_binding_mask & bit) &&
                !(g_menu_capture && (bit & 0x30U) && tab != 0U && tab != previous_tab)) {
                /* Custom global actions and navigation still work when a
                 * simultaneous shoulder pair cancels its fixed tab action. */
                if (!screenshot_push_source(source) && !vtw_push_source(source) && g_menu_capture)
                    mouse_menu_push_event(USB_HID_MENU_ACTION_NONE, source);
            }
            if (source == g_menu_open_close_source)
                menu_start_open_close_hold(slot, source);
            continue;
        }
        if (g_binding_capture) {
            mouse_menu_push_event(USB_HID_MENU_ACTION_NONE, source);
            continue;
        }
        if (slot->generic_axes && !(g_gamepad_binding_mask & bit)) continue;
        if (!(g_gamepad_binding_mask & bit) && button < 5U) continue;
        if (source == g_menu_open_close_source) {
            menu_start_open_close_hold(slot, source);
            if (g_menu_capture && source == g_menu_ok_source) mouse_menu_start_ok_hold(slot);
        } else if (g_menu_capture && source == g_menu_ok_source) mouse_menu_start_ok_hold(slot);
        else if (!screenshot_push_source(source) && !vtw_push_source(source) && g_menu_capture)
            mouse_menu_push_event(USB_HID_MENU_ACTION_NONE, source);
    }
    if (fixed && g_menu_capture && !g_binding_capture && !(buttons & home) && tab != 0U && tab != previous_tab) {
        const uint8_t button = tab == 1U ? USB_GAMEPAD_BUTTON_LEFT_SHOULDER : USB_GAMEPAD_BUTTON_RIGHT_SHOULDER;
        gamepad_menu_push_fixed(tab == 1U ? USB_HID_MENU_ACTION_PREV_TAB : USB_HID_MENU_ACTION_NEXT_TAB,
            (fixed_pressed & (UINT32_C(1) << button)) ?
                usb_hid_menu_source_from_gamepad_button(button) : USB_HID_MENU_SOURCE_NONE);
    }
}

static void mouse_publish_state(uint8_t connected, int32_t x, int32_t y, uint8_t buttons)
{
    REG_WRITE(MOUSE_REG_STATUS, connected ? 1U : 0U);
    REG_WRITE(MOUSE_REG_X, (uint32_t)mouse_clamp_16bit(x));
    REG_WRITE(MOUSE_REG_Y, (uint32_t)mouse_clamp_16bit(y));
    REG_WRITE(MOUSE_REG_BUTTONS, (uint32_t)(buttons & MOUSE_BUTTON_APPLE_MASK));
    REG_WRITE(MOUSE_REG_COMMIT, (uint32_t)g_seq++);
}

static uint8_t mouse_slot_contributes(const usb_hid_slot_t *slot)
{
    return (slot != NULL &&
            slot->active != 0U &&
            slot->mouse_card != 0U) ? 1U : 0U;
}

static uint32_t mouse_card_active_count(void)
{
    uint32_t count = 0U;

    for (uint32_t i = 0U; i < USB_HID_SLOT_COUNT; ++i) {
        if (mouse_slot_contributes(&g_hid_slots[i]) != 0U) {
            count++;
        }
    }
    return count;
}

static uint8_t mouse_card_aggregate_buttons(void)
{
    uint8_t buttons = 0U;

    for (uint32_t i = 0U; i < USB_HID_SLOT_COUNT; ++i) {
        if (mouse_slot_contributes(&g_hid_slots[i]) != 0U) {
            buttons |= g_hid_slots[i].apple_buttons;
        }
    }
    return (uint8_t)(buttons & MOUSE_BUTTON_APPLE_MASK);
}

static uint8_t mouse_card_publish_buttons(void)
{
    if (g_menu_capture != 0U) {
        return 0U;
    }
    return mouse_card_aggregate_buttons();
}

static void mouse_mark_connected(usb_hid_slot_t *slot)
{
    struct usbh_hid *hid;

    if (slot == NULL || slot->hid == NULL || slot->hid->hport == NULL) {
        return;
    }

    hid = slot->hid;
    slot->mouse_card = 1U;
    slot->apple_buttons = 0U;
    if (g_ready == 0U) {
        g_x = (int32_t)(REG_READ(MOUSE_REG_X) & 0xFFFFU);
        g_y = (int32_t)(REG_READ(MOUSE_REG_Y) & 0xFFFFU);
    }
    g_ready = 1U;
    g_last_error = 0;
    mouse_publish_state(1U, g_x, g_y, mouse_card_publish_buttons());

    uart_puts(UART0_BASE, "[usb1] CherryUSB mousecard HID dev=");
    uart_putdec(UART0_BASE, hid->hport->dev_addr);
    uart_puts(UART0_BASE, " speed=");
    uart_puts(UART0_BASE, usb_speed_name(hid->hport->speed));
    uart_puts(UART0_BASE, " hub=");
    uart_putdec(UART0_BASE, hid->hport->parent ? hid->hport->parent->index : 0U);
    uart_puts(UART0_BASE, " port=");
    uart_putdec(UART0_BASE, hid->hport->port);
    uart_puts(UART0_BASE, " if=");
    uart_putdec(UART0_BASE, hid->intf);
    uart_puts(UART0_BASE, " ep=0x");
    uart_puthex(UART0_BASE, hid->intin ? hid->intin->bEndpointAddress : 0U);
    uart_puts(UART0_BASE, "\r\n");
}

static void mouse_mark_disconnected(void)
{
    if (g_ready != 0U) {
        uart_puts(UART0_BASE, "[usb1] CherryUSB mousecard HID disconnected\r\n");
    }
    g_ready = 0U;
    g_last_error = 0;
    mouse_publish_state(0U, g_x, g_y, 0U);
}

static void mouse_release_slot(usb_hid_slot_t *slot)
{
    if (slot == NULL || slot->mouse_card == 0U) {
        return;
    }

    slot->mouse_card = 0U;
    slot->apple_buttons = 0U;
    if (mouse_card_active_count() == 0U) {
        mouse_mark_disconnected();
        return;
    }

    mouse_publish_state(1U, g_x, g_y, mouse_card_publish_buttons());
}

static void mouse_apply_motion(usb_hid_slot_t *slot,
                               int32_t raw_dx,
                               int32_t raw_dy,
                               uint8_t buttons)
{
    int32_t dx;
    int32_t dy;
    uint32_t x_min;
    uint32_t x_max;
    uint32_t y_min;
    uint32_t y_max;

    if (slot == NULL ||
        mouse_slot_contributes(slot) == 0U ||
        g_ready == 0U) {
        return;
    }

    slot->apple_buttons = (uint8_t)(buttons & MOUSE_BUTTON_APPLE_MASK);
    if (g_menu_capture != 0U) {
        return;
    }

    dx = mouse_scale_delta(raw_dx, &g_x_residue);
    dy = mouse_scale_delta(raw_dy, &g_y_residue);
    x_min = REG_READ(MOUSE_REG_X_MIN);
    x_max = REG_READ(MOUSE_REG_X_MAX);
    y_min = REG_READ(MOUSE_REG_Y_MIN);
    y_max = REG_READ(MOUSE_REG_Y_MAX);

    g_x = (int32_t)(REG_READ(MOUSE_REG_X) & 0xFFFFU);
    g_y = (int32_t)(REG_READ(MOUSE_REG_Y) & 0xFFFFU);
    g_x = mouse_clamp_range(g_x + dx, x_min, x_max);
    g_y = mouse_clamp_range(g_y + dy, y_min, y_max);
    mouse_publish_state(1U, g_x, g_y, mouse_card_aggregate_buttons());
}

static void hid_process_gamepad_report(usb_hid_slot_t *slot,
    const onee_input_joystick_report_t *report, uint8_t hat, uint8_t extra_active);
static void hid_prepare_relative_report(usb_hid_slot_t *slot,
    onee_input_joystick_report_t *report, uint16_t relative_mask);

static void hid_process_boot_mouse_report(usb_hid_slot_t *slot,
                                          const uint8_t *report,
                                          uint32_t len)
{
    uint8_t buttons;
    int8_t raw_dx;
    int8_t raw_dy;
    int8_t wheel;

    if (slot == NULL || report == NULL || len < 3U) {
        return;
    }

    buttons = report[0];
    slot->raw_buttons_down = (buttons != 0U) ? 1U : 0U;
    raw_dx = (int8_t)report[1];
    raw_dy = (int8_t)report[2];
    wheel = (len > MOUSE_BOOT_WHEEL_INDEX) ? (int8_t)report[MOUSE_BOOT_WHEEL_INDEX] : 0;

    mouse_menu_process_buttons(slot, buttons, wheel);
    mouse_apply_motion(slot, raw_dx, raw_dy, buttons);
    if (slot->onee_joystick) {
        onee_input_joystick_report_t axes = {0};
        axes.generic_axes = 1U;
        axes.axis_valid_mask = (1U << ONEE_INPUT_AXIS_X) | (1U << ONEE_INPUT_AXIS_Y);
        axes.axis[ONEE_INPUT_AXIS_X] = raw_dx;
        axes.axis[ONEE_INPUT_AXIS_Y] = raw_dy;
        if (len > MOUSE_BOOT_WHEEL_INDEX) {
            axes.axis_valid_mask |= (1U << ONEE_INPUT_AXIS_WHEEL);
            axes.axis[ONEE_INPUT_AXIS_WHEEL] = wheel;
        }
        axes.buttons_valid = 1U;
        axes.buttons_mask = 0x1fU;
        axes.buttons = buttons & 0x1fU;
        hid_prepare_relative_report(slot, &axes, axes.axis_valid_mask);
        if (axes.axis_valid_mask || axes.buttons_valid)
            hid_process_gamepad_report(slot, &axes, SLOT2_GAMEPAD_HAT_UNCHANGED, 0U);
    }

}

static uint8_t hid_source_in_list(usb_hid_menu_source_t source,
                                  const usb_hid_menu_source_t *sources,
                                  uint32_t count)
{
    if (source == USB_HID_MENU_SOURCE_NONE || sources == NULL) {
        return 0U;
    }

    for (uint32_t i = 0U; i < count; ++i) {
        if (sources[i] == source) {
            return 1U;
        }
    }
    return 0U;
}

static void hid_source_list_add(usb_hid_menu_source_t *sources,
                                uint32_t *count,
                                usb_hid_menu_source_t source)
{
    if (sources == NULL ||
        count == NULL ||
        *count >= HID_SOURCE_TRACK_COUNT ||
        source == USB_HID_MENU_SOURCE_NONE ||
        hid_source_in_list(source, sources, *count) != 0U) {
        return;
    }
    sources[*count] = source;
    (*count)++;
}

static void keyboard_menu_start_ok_hold(usb_hid_slot_t *slot,
                                        usb_hid_menu_source_t source,
                                        usb_hid_menu_action_t action)
{
    if (slot == NULL) {
        return;
    }
    slot->ok_down = 1U;
    slot->ok_hold_fired = 0U;
    slot->ok_down_source = source;
    slot->ok_down_action = action;
    XTime_GetTime(&slot->ok_down_tick);
}

static void hid_process_keyboard_usages(usb_hid_slot_t *slot,
                                        uint8_t modifier,
                                        const uint8_t *keys,
                                        uint32_t count)
{
    usb_hid_menu_source_t next_sources[HID_SOURCE_TRACK_COUNT];
    uint8_t onee_keys[HID_KEY_TRACK_COUNT];
    uint32_t next_count = 0U;
    uint32_t onee_key_count = 0U;
    uint8_t onee_modifier = modifier;
    uint8_t onee_reset_chord;
    const uint8_t fixed_keyboard_mode =
        (g_onee_fixed_mode != 0U || g_onee_standby_mode != 0U) ? 1U : 0U;

    if (slot == NULL || keys == NULL) {
        return;
    }

    slot->keyboard_keys_down = (modifier != 0U) ? 1U : 0U;
    memset(onee_keys, 0, sizeof(onee_keys));
    for (uint32_t i = 0U; i < count; ++i) {
        const uint8_t key = keys[i];

        if (key > HID_KBD_USAGE_ERRUNDEF) {
            slot->keyboard_keys_down = 1U;
        }
        if (onee_key_count >= HID_KEY_TRACK_COUNT ||
            key <= HID_KBD_USAGE_ERRUNDEF ||
            onee_usb_apple_usage_allowed(key, fixed_keyboard_mode) == 0U) {
            continue;
        }
        onee_keys[onee_key_count++] = key;
    }
    if (fixed_keyboard_mode != 0U) {
        onee_modifier = onee_usb_fixed_apple_modifier(onee_modifier);
    }
    if (g_onee_input_blocked != 0U) {
        onee_modifier = 0U;
        onee_key_count = 0U;
    }
    onee_reset_chord = onee_input_service_keyboard_report(slot->index,
                                                           onee_modifier,
                                                           onee_keys,
                                                           onee_key_count);

    memset(next_sources, 0, sizeof(next_sources));
    for (uint8_t bit = 0U; bit < 8U; ++bit) {
        if ((modifier & (uint8_t)(1U << bit)) != 0U) {
            hid_source_list_add(
                next_sources,
                &next_count,
                usb_hid_menu_source_from_keyboard_usage(
                    (uint8_t)(HID_KBD_USAGE_LCTRL + bit)));
        }
    }

    for (uint32_t i = 0U;
         i < count && next_count < HID_SOURCE_TRACK_COUNT;
         ++i) {
        uint8_t key = keys[i];
        usb_hid_menu_source_t source;

        source = usb_hid_menu_source_from_keyboard_usage(key);
        if (source == USB_HID_MENU_SOURCE_NONE) {
            continue;
        }

        hid_source_list_add(next_sources, &next_count, source);
    }

    for (uint32_t i = 0U; i < next_count; ++i) {
        usb_hid_menu_action_t fixed_action;
        uint8_t fixed_route;

        if (onee_reset_chord != 0U &&
            next_sources[i] ==
                usb_hid_menu_source_from_keyboard_usage(
                    HID_KBD_USAGE_DELFWD)) {
            continue;
        }
        if (hid_source_in_list(next_sources[i],
                               slot->prev_sources,
                               HID_SOURCE_TRACK_COUNT) != 0U) {
            continue;
        }
        if (fixed_keyboard_mode != 0U) {
            const uint8_t usage =
                (uint8_t)(next_sources[i] & USB_HID_MENU_SOURCE_KEY_MASK);
            fixed_action = onee_usb_keyboard_action(
                usage, modifier, g_menu_capture, fixed_keyboard_mode);
            fixed_route = onee_usb_fixed_route(usage, fixed_action);
            if ((fixed_route & ONEE_USB_ROUTE_PUSH_NOW) != 0U) {
                mouse_menu_push_event(fixed_action, next_sources[i]);
            }
            if ((fixed_route & ONEE_USB_ROUTE_CONSUME_SOURCE) != 0U) {
                continue;
            }
        }
        if (screenshot_push_source(next_sources[i]) != 0U) {
            continue;
        }
        if (vtw_push_source(next_sources[i]) != 0U) {
            continue;
        }
        if (fixed_keyboard_mode == 0U &&
            next_sources[i] == g_menu_open_close_source) {
            menu_start_open_close_hold(slot, next_sources[i]);
            if (g_menu_capture != 0U && next_sources[i] == g_menu_ok_source) {
                keyboard_menu_start_ok_hold(slot,
                                            next_sources[i],
                                            USB_HID_MENU_ACTION_NONE);
            }
        } else if (g_menu_capture != 0U && next_sources[i] == g_menu_ok_source) {
            keyboard_menu_start_ok_hold(slot,
                                        next_sources[i],
                                        USB_HID_MENU_ACTION_NONE);
        } else if (g_menu_capture != 0U) {
            keyboard_menu_push_source(next_sources[i]);
        }
    }

    if (slot->ok_down != 0U &&
        usb_hid_menu_source_is_keyboard(slot->ok_down_source) != 0U &&
        slot->ok_hold_fired == 0U &&
        hid_source_in_list(slot->ok_down_source, next_sources, next_count) == 0U) {
        if (slot->ok_down_action != USB_HID_MENU_ACTION_NONE) {
            mouse_menu_push_event(slot->ok_down_action,
                                  slot->ok_down_source);
        } else {
            keyboard_menu_push_source(slot->ok_down_source);
        }
    }
    if (slot->ok_down != 0U &&
        usb_hid_menu_source_is_keyboard(slot->ok_down_source) != 0U &&
        hid_source_in_list(slot->ok_down_source, next_sources, next_count) == 0U) {
        slot->ok_down = 0U;
        slot->ok_hold_fired = 0U;
        slot->ok_down_tick = 0U;
        slot->ok_down_source = USB_HID_MENU_SOURCE_NONE;
        slot->ok_down_action = USB_HID_MENU_ACTION_NONE;
    }
    if (slot->open_close_down != 0U &&
        usb_hid_menu_source_is_keyboard(slot->open_close_down_source) != 0U &&
        hid_source_in_list(slot->open_close_down_source, next_sources, next_count) == 0U) {
        menu_finish_open_close_hold(slot, slot->open_close_down_source);
    }

    memcpy(slot->prev_sources, next_sources, sizeof(slot->prev_sources));
    menu_poll_open_close_hold(slot);
}

static void hid_process_boot_keyboard_report(usb_hid_slot_t *slot,
                                             const uint8_t *report,
                                             uint32_t len)
{
    const struct usb_hid_kbd_report *keyboard;

    if (slot == NULL || report == NULL || len < sizeof(*keyboard)) {
        return;
    }

    keyboard = (const struct usb_hid_kbd_report *)report;
    for (unsigned i = 0U; i < sizeof(keyboard->key); ++i)
        if (keyboard->key[i] >= 1U && keyboard->key[i] <= 3U) return;
    hid_process_keyboard_usages(slot,
                                keyboard->modifier,
                                keyboard->key,
                                (uint32_t)sizeof(keyboard->key));
}

static uint8_t hid_report_id_matches(const struct usbh_hid_report_item *item,
                                     const uint8_t *report,
                                     uint32_t len)
{
    if (item->attribute.report_id == 0U) {
        return 1U;
    }
    return (report != NULL &&
            len > 0U &&
            report[0] == item->attribute.report_id) ? 1U : 0U;
}

static uint8_t hid_extract_bits(const struct usbh_hid_report_item *item,
                                const uint8_t *report,
                                uint32_t len,
                                uint32_t field,
                                uint32_t *value)
{
    uint32_t bit_offset;
    uint32_t bit_count;
    uint32_t result = 0U;

    if (item == NULL ||
        report == NULL ||
        value == NULL ||
        item->attribute.report_size == 0U ||
        item->attribute.report_size > 32U ||
        field >= item->attribute.report_count ||
        hid_report_id_matches(item, report, len) == 0U) {
        return 0U;
    }

    bit_count = item->attribute.report_size;
    bit_offset = item->report_bit_offset + (field * bit_count);
    if (item->attribute.report_id != 0U) {
        bit_offset += 8U;
    }
    if ((bit_offset + bit_count) > (len * 8U)) {
        return 0U;
    }

    for (uint32_t i = 0U; i < bit_count; ++i) {
        const uint32_t src_bit = bit_offset + i;
        const uint32_t bit = (uint32_t)((report[src_bit / 8U] >> (src_bit % 8U)) & 0x01U);
        result |= bit << i;
    }

    *value = result;
    return 1U;
}

static int32_t hid_sign_extend(uint32_t value, uint8_t bits)
{
    uint32_t sign_bit;

    if (bits == 0U || bits >= 32U) {
        return (int32_t)value;
    }

    sign_bit = 1UL << (bits - 1U);
    if ((value & sign_bit) != 0U) {
        value |= (~0UL << bits);
    }
    return (int32_t)value;
}

static int32_t hid_item_signed_value(const struct usbh_hid_report_item *item,
                                     uint32_t raw)
{
    if (item->attribute.logical_min < 0 ||
        item->attribute.logical_min > item->attribute.logical_max ||
        (item->report_flags & HID_MAINITEM_RELATIVE) != 0U) {
        return hid_sign_extend(raw, item->attribute.report_size);
    }
    return (int32_t)raw;
}

static void hid_process_keyboard_chunks(usb_hid_slot_t *slot,
    const uint8_t *report, uint32_t length)
{
    const appletini_usb_hid_descriptor_meta_t *meta = &slot->descriptor_meta;
    uint8_t pending[APPLETINI_USB_HID_MAX_KEYBOARD_ITEMS][32];
    uint8_t changed = 0U;
    memcpy(pending, slot->keyboard_state, sizeof(pending));
    for (unsigned i = 0U; i < meta->keyboard_item_count; ++i) {
        const appletini_usb_hid_keyboard_item_t *keyboard = &meta->keyboard_items[i];
        const struct usbh_hid_report_item *item = &keyboard->item;
        uint8_t next[32] = {0};
        if (!hid_report_id_matches(item, report, length)) continue;
        for (uint32_t field = 0U; field < item->attribute.report_count; ++field) {
            uint32_t raw;
            uint16_t usage;
            if (!hid_extract_bits(item, report, length, field, &raw)) return;
            if (item->report_flags & HID_MAINITEM_VARIABLE) {
                if (raw == 0U) continue;
                usage = appletini_usb_hid_keyboard_usage(meta, keyboard, field);
            } else {
                const int64_t selector = (int64_t)hid_item_signed_value(item, raw) - item->attribute.logical_min;
                if (selector < 0 || selector > UINT32_MAX) continue;
                usage = appletini_usb_hid_keyboard_usage(meta, keyboard, (uint32_t)selector);
                if (usage >= 1U && usage <= 3U) return; /* Rollover preserves the last good state. */
            }
            if (usage > 3U && usage < 256U) next[usage / 8U] |= (uint8_t)(1U << (usage % 8U));
        }
        memcpy(pending[i], next, sizeof(next));
        changed = 1U;
    }
    if (changed) {
        uint8_t aggregate[32] = {0}, keys[HID_KEY_TRACK_COUNT], modifier = 0U;
        uint32_t count = 0U;
        memcpy(slot->keyboard_state, pending, sizeof(pending));
        for (unsigned i = 0U; i < meta->keyboard_item_count; ++i)
            for (unsigned byte = 0U; byte < sizeof(aggregate); ++byte)
                aggregate[byte] |= pending[i][byte];
        for (unsigned usage = 4U; usage < 256U; ++usage) {
            if (!(aggregate[usage / 8U] & (1U << (usage % 8U)))) continue;
            if (usage >= HID_KBD_USAGE_LCTRL && usage <= HID_KBD_USAGE_RGUI)
                modifier |= (uint8_t)(1U << (usage - HID_KBD_USAGE_LCTRL));
            else if (count < sizeof(keys)) keys[count++] = (uint8_t)usage;
        }
        hid_process_keyboard_usages(slot, modifier, keys, count);
    }
}

static uint8_t hid_packet_complete(const usb_hid_slot_t *slot,
    const uint8_t *report, uint32_t length)
{
    uint32_t raw;
    if (length == 0U) return 0U;
    const uint8_t id = slot->report_info.using_report_id ? report[0] : 0U;
    const uint32_t bits = slot->descriptor_meta.input_bits[id];
    const uint32_t prefix = slot->report_info.using_report_id ? 1U : 0U;
    if (bits != 0U && length < prefix + (bits + 7U) / 8U) return 0U;
    for (unsigned i = 0U; i < slot->report_info.report_item_count; ++i) {
        const struct usbh_hid_report_item *item = &slot->report_info.report_items[i];
        if (hid_report_id_matches(item, report, length) && item->attribute.report_count &&
            !hid_extract_bits(item, report, length, item->attribute.report_count - 1U, &raw)) return 0U;
    }
    for (unsigned i = 0U; i < slot->descriptor_meta.keyboard_item_count; ++i) {
        const struct usbh_hid_report_item *item = &slot->descriptor_meta.keyboard_items[i].item;
        if (hid_report_id_matches(item, report, length) && item->attribute.report_count &&
            !hid_extract_bits(item, report, length, item->attribute.report_count - 1U, &raw)) return 0U;
    }
    return 1U;
}

static void hid_collect_button_item(usb_hid_slot_t *slot,
                                    const struct usbh_hid_report_item *item,
                                    const uint8_t *report, uint32_t len,
                                    uint32_t *buttons, uint32_t *button_mask,
                                    uint8_t *button_seen)
{
    if (slot == NULL || item->attribute.usage_page != HID_USAGE_PAGE_BUTTON ||
        item->report_type != HID_REPORT_INPUT) return;
    const uint32_t limit = slot->onee_joystick ? 32U : 5U;
    const uint8_t variable = (item->report_flags & HID_MAINITEM_VARIABLE) != 0U;
    for (uint32_t field = 0U; field < item->attribute.report_count; ++field) {
        uint32_t raw;
        if (!hid_extract_bits(item, report, len, field, &raw)) continue;
        if (variable) {
            const uint32_t usage = item->attribute.usage_min + field;
            if (usage < 1U || usage > limit) continue;
            const uint32_t mask = UINT32_C(1) << (usage - 1U);
            *button_mask |= mask;
            if (raw) *buttons |= mask;
        } else {
            /* Array reports describe the complete set within their range. */
            for (uint32_t usage = 1U; usage <= limit; ++usage)
                if (usage >= item->attribute.usage_min && usage <= item->attribute.usage_max)
                    *button_mask |= UINT32_C(1) << (usage - 1U);
            const int64_t usage = (int64_t)hid_item_signed_value(item, raw) -
                item->attribute.logical_min + item->attribute.usage_min;
            if (usage >= 1 && usage <= limit && usage <= item->attribute.usage_max)
                *buttons |= UINT32_C(1) << (usage - 1U);
        }
        *button_seen = 1U;
    }
}

static uint16_t hid_desktop_usage_for_field(const struct usbh_hid_report_item *item,
                                            uint32_t field)
{
    uint16_t usage_min = item->attribute.usage_min;
    uint16_t usage_max = item->attribute.usage_max;

    if (usage_min == 0xFFFFU) {
        return HID_DESKTOP_USAGE_UNDEFINED;
    }

    if (usage_min <= HID_DESKTOP_USAGE_X &&
        usage_max >= HID_DESKTOP_USAGE_WHEEL &&
        item->attribute.report_count >= 3U &&
        field < 3U) {
        if (field == 0U) {
            return HID_DESKTOP_USAGE_X;
        }
        if (field == 1U) {
            return HID_DESKTOP_USAGE_Y;
        }
        return HID_DESKTOP_USAGE_WHEEL;
    }

    if (usage_min <= HID_DESKTOP_USAGE_X &&
        usage_max >= HID_DESKTOP_USAGE_Y &&
        item->attribute.report_count >= 2U &&
        field < 2U) {
        return (field == 0U) ? HID_DESKTOP_USAGE_X : HID_DESKTOP_USAGE_Y;
    }

    if (usage_min <= HID_DESKTOP_USAGE_HATSWITCH &&
        usage_max >= HID_DESKTOP_USAGE_HATSWITCH) {
        return HID_DESKTOP_USAGE_HATSWITCH;
    }

    if ((uint32_t)usage_min + field <= usage_max) {
        return (uint16_t)(usage_min + field);
    }
    return usage_min;
}

static uint8_t hid_onee_axis_from_usage(uint16_t usage, onee_input_axis_t *axis)
{
    if (axis == NULL) return 0U;
    if (usage >= HID_DESKTOP_USAGE_X && usage <= HID_DESKTOP_USAGE_WHEEL) {
        *axis = (onee_input_axis_t)(usage - HID_DESKTOP_USAGE_X);
        return 1U;
    }
    return 0U;
}

static uint8_t hid_axis_active_from_rest(int32_t value,
                                         int32_t rest,
                                         int32_t logical_min,
                                         int32_t logical_max)
{
    const int64_t span = (int64_t)logical_max - (int64_t)logical_min;
    int64_t delta = (int64_t)value - (int64_t)rest;

    if (delta < 0) {
        delta = -delta;
    }
    if (span <= 0) {
        return (value != rest) ? 1U : 0U;
    }
    return (delta > (span / 8)) ? 1U : 0U;
}

static void hid_menu_push_hat(usb_hid_slot_t *slot, uint8_t hat)
{
    if (slot == NULL || hat == slot->fixed_hat) return;
    slot->fixed_hat = hat;
    if (!g_menu_capture || slot->generic_axes || hat > 7U ||
        (slot->gamepad_buttons & (UINT32_C(1) << USB_GAMEPAD_BUTTON_HOME))) return;
    if (hat == 7U || hat == 0U || hat == 1U)
        gamepad_menu_push_fixed(USB_HID_MENU_ACTION_ITEM_UP, USB_HID_MENU_SOURCE_NONE);
    if (hat == 3U || hat == 4U || hat == 5U)
        gamepad_menu_push_fixed(USB_HID_MENU_ACTION_ITEM_DOWN, USB_HID_MENU_SOURCE_NONE);
    if (hat == 5U || hat == 6U || hat == 7U)
        gamepad_menu_push_fixed(USB_HID_MENU_ACTION_LEFT, USB_HID_MENU_SOURCE_NONE);
    if (hat == 1U || hat == 2U || hat == 3U)
        gamepad_menu_push_fixed(USB_HID_MENU_ACTION_RIGHT, USB_HID_MENU_SOURCE_NONE);
}

static void hid_menu_push_axis(usb_hid_slot_t *slot,
                               int8_t *previous,
                               int8_t direction,
                               usb_hid_menu_action_t negative_action,
                               usb_hid_menu_action_t positive_action)
{
    if (slot == NULL || previous == NULL || g_joystick_preview != 0U ||
        *previous == direction) {
        return;
    }

    *previous = direction;
    if (direction < 0) {
        mouse_menu_push_bindable_action(negative_action);
    } else if (direction > 0) {
        mouse_menu_push_bindable_action(positive_action);
    }
}

static void hid_process_gamepad_report(
    usb_hid_slot_t *slot,
    const onee_input_joystick_report_t *report,
    uint8_t hat,
    uint8_t extra_active)
{
    if (slot == NULL || report == NULL) {
        return;
    }

    if (report->buttons_valid) {
        const uint32_t mask = report->buttons_mask ? report->buttons_mask : UINT32_MAX;
        slot->gamepad_buttons = (slot->gamepad_buttons & ~mask) | (report->buttons & mask);
    }
    slot->raw_buttons_down = (slot->native_mouse_buttons != 0U || slot->gamepad_buttons != 0U || extra_active != 0U);
    if (hat != SLOT2_GAMEPAD_HAT_UNCHANGED)
        slot->raw_hat_active = onee_usb_hat_active(hat);
    for (uint32_t axis = 0U; axis < ONEE_INPUT_AXIS_COUNT; ++axis) {
        const uint16_t axis_bit = (uint16_t)(1U << axis);

        if ((report->axis_valid_mask & axis_bit) == 0U) {
            continue;
        }
        if ((slot->raw_axis_rest_valid_mask & axis_bit) == 0U) {
            slot->raw_axis_rest[axis] = report->axis[axis];
            slot->raw_axis_rest_valid_mask |= axis_bit;
        }
        if (hid_axis_active_from_rest(report->axis[axis],
                                      slot->raw_axis_rest[axis],
                                      report->logical_min[axis],
                                      report->logical_max[axis]) == 0U) {
            slot->raw_axis_active_mask &= (uint16_t)~axis_bit;
        } else {
            slot->raw_axis_active_mask |= axis_bit;
        }
    }

    if (report->buttons_valid != 0U) gamepad_menu_process_buttons(slot);
    if (hat != SLOT2_GAMEPAD_HAT_UNCHANGED) hid_menu_push_hat(slot, hat);
    if (g_menu_capture != 0U && !report->generic_axes &&
        !(slot->gamepad_buttons & (UINT32_C(1) << USB_GAMEPAD_BUTTON_HOME))) {
        if ((report->axis_valid_mask & (1U << ONEE_INPUT_AXIS_X)) != 0U) {
            hid_menu_push_axis(slot,
                               &slot->prev_x_dir,
                               onee_usb_axis_direction(
                                   report->axis[ONEE_INPUT_AXIS_X],
                                   report->logical_min[ONEE_INPUT_AXIS_X],
                                   report->logical_max[ONEE_INPUT_AXIS_X]),
                               USB_HID_MENU_ACTION_LEFT,
                               USB_HID_MENU_ACTION_RIGHT);
        }
        if ((report->axis_valid_mask & (1U << ONEE_INPUT_AXIS_Y)) != 0U) {
            hid_menu_push_axis(slot,
                               &slot->prev_y_dir,
                               onee_usb_axis_direction(
                                   report->axis[ONEE_INPUT_AXIS_Y],
                                   report->logical_min[ONEE_INPUT_AXIS_Y],
                                   report->logical_max[ONEE_INPUT_AXIS_Y]),
                               USB_HID_MENU_ACTION_ITEM_UP,
                               USB_HID_MENU_ACTION_ITEM_DOWN);
        }
    }
    /* Gamepad buttons reach paddles, slot-2 gamepads and menus, never MouseCard. */
    if (report->axis_valid_mask || report->buttons_valid)
        onee_input_service_joystick_report(slot->index, report);
    if (!report->generic_axes) slot2_gamepad_service_report(slot->index, report, hat);
}

void usb_gamepad_input_report(
    uint8_t index,
    const onee_input_joystick_report_t *report,
    uint8_t hat,
    uint8_t extra_active)
{
    usb_hid_slot_t *slot;

    if (index >= USB_HID_SLOT_COUNT) {
        return;
    }
    slot = &g_hid_slots[index];
    if (slot->active == 0U || slot->vendor_gamepad == 0U) {
        return;
    }
    hid_process_gamepad_report(slot, report, hat, extra_active);
}

static void hid_collect_desktop_item(usb_hid_slot_t *slot,
                                     const struct usbh_hid_report_item *item,
                                     const uint8_t *report,
                                     uint32_t len,
                                     int64_t *dx,
                                     int64_t *dy,
                                     uint8_t *mouse_delta_seen,
                                     int8_t *wheel,
                                     uint8_t *wheel_seen,
                                     onee_input_joystick_report_t *onee_joystick,
                                     uint8_t *gamepad_hat,
                                     uint8_t native_mouse, uint8_t controller,
                                     uint16_t *relative_mask)
{
    const uint8_t relative = (uint8_t)((item->report_flags & HID_MAINITEM_RELATIVE) ? 1U : 0U);

    if (slot == NULL ||
        (item->attribute.usage_page != HID_USAGE_PAGE_GENERIC_DESKTOP_CONTROLS &&
         item->attribute.usage_page != 2U && item->attribute.usage_page != 0x0cU) ||
        item->report_type != HID_REPORT_INPUT ||
        (item->report_flags & HID_MAINITEM_VARIABLE) == 0U) {
        return;
    }

    for (uint32_t field = 0U; field < item->attribute.report_count; ++field) {
        uint32_t raw = 0U;
        int32_t value;
        uint16_t usage;

        if (hid_extract_bits(item, report, len, field, &raw) == 0U) {
            continue;
        }

        value = hid_item_signed_value(item, raw);
        usage = hid_desktop_usage_for_field(item, field);
        if (item->attribute.usage_page == 2U) {
            if (usage == 0xC4U) usage = HID_DESKTOP_USAGE_Z;
            else if (usage == 0xC5U) usage = HID_DESKTOP_USAGE_RZ;
            else continue;
        }
        if (controller && onee_joystick != NULL) {
            onee_input_axis_t axis;
            if ((item->attribute.usage_page == 0x0cU && usage == 0x0238U ?
                    (axis = ONEE_INPUT_AXIS_PAN, 1U) : hid_onee_axis_from_usage(usage, &axis)) != 0U) {
                const uint16_t axis_bit = (uint16_t)(1U << axis);

                onee_joystick->axis_valid_mask |=
                    axis_bit;
                onee_joystick->axis[axis] = value;
                onee_joystick->logical_min[axis] =
                    item->attribute.logical_min;
                onee_joystick->logical_max[axis] =
                    item->attribute.logical_max;
                if (relative) *relative_mask |= axis_bit;
                if (!relative && (slot->raw_axis_rest_valid_mask & axis_bit) == 0U) {
                    slot->raw_axis_rest[axis] = value;
                    slot->raw_axis_rest_valid_mask |= axis_bit;
                    slot->raw_axis_active_mask &= (uint16_t)~axis_bit;
                }
                if (!relative && slot->onee_joystick != 0U) {
                    if (hid_axis_active_from_rest(
                            value,
                            slot->raw_axis_rest[axis],
                            item->attribute.logical_min,
                            item->attribute.logical_max) == 0U) {
                        slot->raw_axis_active_mask &= (uint16_t)~axis_bit;
                    } else {
                        slot->raw_axis_active_mask |= axis_bit;
                    }
                }
            }
        }
        switch (usage) {
        case HID_DESKTOP_USAGE_X:
            if (relative != 0U && native_mouse) {
                *dx += value;
                *mouse_delta_seen = 1U;
            } else if (!relative && controller && !slot->generic_axes && g_menu_capture != 0U) {
                hid_menu_push_axis(slot,
                                   &slot->prev_x_dir,
                                   onee_usb_axis_direction(
                                       value,
                                       item->attribute.logical_min,
                                       item->attribute.logical_max),
                                   USB_HID_MENU_ACTION_LEFT,
                                   USB_HID_MENU_ACTION_RIGHT);
            }
            break;
        case HID_DESKTOP_USAGE_Y:
            if (relative != 0U && native_mouse) {
                *dy += value;
                *mouse_delta_seen = 1U;
            } else if (!relative && controller && !slot->generic_axes && g_menu_capture != 0U) {
                hid_menu_push_axis(slot,
                                   &slot->prev_y_dir,
                                   onee_usb_axis_direction(
                                       value,
                                       item->attribute.logical_min,
                                       item->attribute.logical_max),
                                   USB_HID_MENU_ACTION_ITEM_UP,
                                   USB_HID_MENU_ACTION_ITEM_DOWN);
            }
            break;
        case HID_DESKTOP_USAGE_WHEEL:
            if (native_mouse) {
                *wheel = value > 0 ? 1 : value < 0 ? -1 : 0;
                *wheel_seen = 1U;
            }
            break;
        case HID_DESKTOP_USAGE_HATSWITCH:
            if (relative == 0U && controller && slot->onee_joystick != 0U) {
                const int64_t position = (int64_t)value - item->attribute.logical_min;
                const int64_t count = (int64_t)item->attribute.logical_max -
                                      item->attribute.logical_min + 1;
                uint8_t hat = HID_HAT_NEUTRAL;
                if (position >= 0 && position < count) {
                    if (count == 4) hat = (uint8_t)(position * 2);
                    else if (position < 8) hat = (uint8_t)position;
                }
                *gamepad_hat = hat;
                slot->raw_hat_active = onee_usb_hat_active(hat);
            }
            break;
        case HID_DESKTOP_USAGE_DPAD_UP:
        case HID_DESKTOP_USAGE_DPAD_DOWN:
        case HID_DESKTOP_USAGE_DPAD_RIGHT:
        case HID_DESKTOP_USAGE_DPAD_LEFT:
            if (relative == 0U && controller && slot->onee_joystick) {
                const uint8_t bit = (uint8_t)(1U << (usage - HID_DESKTOP_USAGE_DPAD_UP));
                if (value) slot->gamepad_dpad |= bit;
                else slot->gamepad_dpad &= (uint8_t)~bit;
                const int x = !!(slot->gamepad_dpad & 4U) - !!(slot->gamepad_dpad & 8U);
                const int y = !!(slot->gamepad_dpad & 2U) - !!(slot->gamepad_dpad & 1U);
                *gamepad_hat = y < 0 ? (x < 0 ? 7U : x > 0 ? 1U : 0U) :
                    y > 0 ? (x < 0 ? 5U : x > 0 ? 3U : 4U) :
                    x < 0 ? 6U : x > 0 ? 2U : HID_HAT_NEUTRAL;
            }
            break;
        default:
            break;
        }
    }
}

static uint8_t hid_report_info_has_absolute_joystick(
    const usb_hid_slot_t *slot)
{
    uint16_t axis_mask = 0U;
    uint8_t has_hat = 0U;

    if (slot == NULL || slot->report_info_valid == 0U ||
        slot->interface_protocol == HID_PROTOCOL_MOUSE) {
        return 0U;
    }
    for (uint32_t i = 0U; i < slot->report_info.report_item_count; ++i) {
        const struct usbh_hid_report_item *item =
            &slot->report_info.report_items[i];

        if (item->report_type != HID_REPORT_INPUT ||
            item->attribute.usage_page !=
                HID_USAGE_PAGE_GENERIC_DESKTOP_CONTROLS ||
            (item->report_flags & HID_MAINITEM_VARIABLE) == 0U ||
            (item->report_flags & HID_MAINITEM_RELATIVE) != 0U) {
            continue;
        }
        for (uint32_t field = 0U; field < item->attribute.report_count;
             ++field) {
            onee_input_axis_t axis;
            const uint16_t usage = hid_desktop_usage_for_field(item, field);
            if (usage == HID_DESKTOP_USAGE_HATSWITCH) {
                has_hat = 1U;
            }
            if (hid_onee_axis_from_usage(usage, &axis) != 0U) {
                axis_mask |= (uint16_t)(1U << axis);
            }
        }
    }
    /* A D-pad hat also identifies gamepads without analog sticks. */
    return (has_hat != 0U || (axis_mask & (uint16_t)(axis_mask - 1U)) != 0U) ? 1U : 0U;
}

static uint8_t hid_report_info_has_relative_mouse(const usb_hid_slot_t *slot)
{
    uint8_t has_x = 0U;
    uint8_t has_y = 0U;

    if (slot == NULL || slot->report_info_valid == 0U) {
        return 0U;
    }

    for (uint32_t i = 0U; i < slot->report_info.report_item_count; ++i) {
        const struct usbh_hid_report_item *item = &slot->report_info.report_items[i];

        if (item->report_type != HID_REPORT_INPUT ||
            item->attribute.usage_page != HID_USAGE_PAGE_GENERIC_DESKTOP_CONTROLS ||
            (item->report_flags & HID_MAINITEM_VARIABLE) == 0U ||
            (item->report_flags & HID_MAINITEM_RELATIVE) == 0U) {
            continue;
        }

        for (uint32_t field = 0U; field < item->attribute.report_count; ++field) {
            const uint16_t usage = hid_desktop_usage_for_field(item, field);

            if (usage == HID_DESKTOP_USAGE_X) {
                has_x = 1U;
            } else if (usage == HID_DESKTOP_USAGE_Y) {
                has_y = 1U;
            }
        }
    }

    return (has_x != 0U && has_y != 0U) ? 1U : 0U;
}

static int hid_pop_relative_detent(usb_hid_slot_t *slot, unsigned axis)
{
    const unsigned head = slot->relative_head[axis];
    const int step = slot->relative_queue[axis][head];
    slot->relative_head[axis] = (uint8_t)((head + 1U) % 32U);
    --slot->relative_count[axis];
    return step;
}

static void hid_prepare_relative_report(usb_hid_slot_t *slot,
    onee_input_joystick_report_t *report, uint16_t relative_mask)
{
    XTime now;
    XTime_GetTime(&now);
    for (unsigned axis = 0U; axis < ONEE_INPUT_AXIS_COUNT; ++axis) {
        const uint16_t bit = (uint16_t)(1U << axis);
        if (!(relative_mask & bit)) continue;
        const int32_t delta = report->axis[axis];
        report->logical_min[axis] = -32768;
        report->logical_max[axis] = 32767;
        if (!delta) { report->axis_valid_mask &= (uint16_t)~bit; continue; }
        if (axis >= ONEE_INPUT_AXIS_DIAL) {
            const int step = delta > 0 ? 1 : -1;
            const uint32_t magnitude = delta > 0 ? (uint32_t)delta : (uint32_t)-(int64_t)delta;
            for (uint32_t n = 0U; n < magnitude && slot->relative_count[axis] < 32U; ++n) {
                const unsigned tail = (slot->relative_head[axis] + slot->relative_count[axis]) % 32U;
                slot->relative_queue[axis][tail] = (int8_t)step;
                ++slot->relative_count[axis];
            }
            if (slot->relative_pending & bit) { report->axis_valid_mask &= (uint16_t)~bit; continue; }
            report->axis[axis] = hid_pop_relative_detent(slot, axis) * 4096;
        } else {
            const int64_t velocity = (int64_t)delta * 256;
            report->axis[axis] = velocity > 32767 ? 32767 : velocity < -32768 ? -32768 : (int32_t)velocity;
        }
        slot->raw_axis_rest[axis] = 0;
        slot->raw_axis_rest_valid_mask |= bit;
        slot->relative_pending |= bit;
        slot->relative_active |= bit;
        slot->relative_started[axis] = now;
    }
}

static void hid_poll_relative_inputs(void)
{
    XTime now;
    XTime_GetTime(&now);
    for (unsigned i = 0U; i < USB_HID_SLOT_COUNT; ++i) {
        usb_hid_slot_t *slot = &g_hid_slots[i];
        if (!slot->active || !slot->relative_pending) continue;
        onee_input_joystick_report_t report = {0};
        report.generic_axes = slot->generic_axes;
        for (unsigned axis = 0U; axis < ONEE_INPUT_AXIS_COUNT; ++axis) {
            const uint16_t bit = (uint16_t)(1U << axis);
            if (!(slot->relative_pending & bit) || now - slot->relative_started[axis] < HID_RELATIVE_PULSE_TICKS) continue;
            report.axis_valid_mask |= bit;
            report.logical_min[axis] = -32768;
            report.logical_max[axis] = 32767;
            if (slot->relative_active & bit) {
                slot->relative_active &= (uint16_t)~bit;
                if (!slot->relative_count[axis]) slot->relative_pending &= (uint16_t)~bit;
            } else {
                report.axis[axis] = hid_pop_relative_detent(slot, axis) * 4096;
                slot->relative_active |= bit;
            }
            slot->relative_started[axis] = now;
        }
        if (report.axis_valid_mask) hid_process_gamepad_report(slot, &report, SLOT2_GAMEPAD_HAT_UNCHANGED, 0U);
    }
}

static void hid_clear_relative_inputs(void)
{
    for (unsigned i = 0U; i < USB_HID_SLOT_COUNT; ++i) {
        usb_hid_slot_t *slot = &g_hid_slots[i];
        if (!slot->active || !slot->relative_pending) continue;
        onee_input_joystick_report_t report = {0};
        report.generic_axes = slot->generic_axes;
        report.axis_valid_mask = slot->relative_pending;
        for (unsigned axis = 0U; axis < ONEE_INPUT_AXIS_COUNT; ++axis) {
            report.logical_min[axis] = -32768;
            report.logical_max[axis] = 32767;
        }
        slot->relative_pending = slot->relative_active = 0U;
        memset(slot->relative_head, 0, sizeof(slot->relative_head));
        memset(slot->relative_count, 0, sizeof(slot->relative_count));
        hid_process_gamepad_report(slot, &report, SLOT2_GAMEPAD_HAT_UNCHANGED, 0U);
    }
}

static void hid_process_report_protocol_report(usb_hid_slot_t *slot,
    const uint8_t *report, uint32_t len)
{
    uint32_t buttons = 0U, button_mask = 0U, mouse_buttons = 0U, mouse_mask = 0U;
    uint8_t button_seen = 0U, native_button_seen = 0U, wheel_seen = 0U, mouse_delta_seen = 0U;
    int8_t wheel = 0;
    int64_t dx = 0, dy = 0;
    uint16_t relative_mask = 0U;
    onee_input_joystick_report_t joystick = {0};
    uint8_t hat = SLOT2_GAMEPAD_HAT_UNCHANGED;
    if (!slot || !report || !slot->report_info_valid || !hid_packet_complete(slot, report, len)) return;
    joystick.generic_axes = slot->generic_axes;
    hid_process_keyboard_chunks(slot, report, len);
    for (uint32_t i = 0U; i < slot->report_info.report_item_count; ++i) {
        const struct usbh_hid_report_item *item = &slot->report_info.report_items[i];
        if (item->report_type != HID_REPORT_INPUT || (item->report_flags & HID_MAINITEM_CONSTANT) ||
            !hid_report_id_matches(item, report, len)) continue;
        const unsigned application = slot->descriptor_meta.field_application[i];
        const unsigned capabilities = application < slot->descriptor_meta.application_count ?
            slot->descriptor_meta.applications[application].capabilities : 0U;
        const uint8_t native_mouse = (capabilities & APPLETINI_USB_HID_APPLICATION_MOUSE) ||
            (!slot->descriptor_meta.application_count && !slot->onee_joystick);
        const uint8_t controller = slot->onee_joystick &&
            (!slot->descriptor_meta.application_count || slot->generic_axes ||
             (capabilities & APPLETINI_USB_HID_APPLICATION_GAMEPAD));
        if (controller) hid_collect_button_item(slot, item, report, len, &buttons, &button_mask, &button_seen);
        if (native_mouse) hid_collect_button_item(slot, item, report, len, &mouse_buttons, &mouse_mask, &native_button_seen);
        hid_collect_desktop_item(slot, item, report, len, &dx, &dy, &mouse_delta_seen,
            &wheel, &wheel_seen, &joystick, &hat, native_mouse, controller, &relative_mask);
    }
    if (native_button_seen) slot->native_mouse_buttons = (uint8_t)(
        (slot->native_mouse_buttons & ~mouse_mask) | (mouse_buttons & mouse_mask));
    if (native_button_seen || wheel_seen) {
        slot->raw_buttons_down = (slot->native_mouse_buttons != 0U || slot->gamepad_buttons != 0U);
        mouse_menu_process_buttons(slot, hid_legacy_menu_buttons(slot), wheel);
    }
    if (mouse_delta_seen || native_button_seen)
        mouse_apply_motion(slot,
            dx > 32767 ? 32767 : dx < -32768 ? -32768 : (int32_t)dx,
            dy > 32767 ? 32767 : dy < -32768 ? -32768 : (int32_t)dy,
            slot->native_mouse_buttons);
    if (slot->onee_joystick && (joystick.axis_valid_mask || button_seen || hat != SLOT2_GAMEPAD_HAT_UNCHANGED)) {
        joystick.buttons_valid = button_seen;
        joystick.buttons = buttons;
        joystick.buttons_mask = button_mask;
        if (relative_mask) hid_prepare_relative_report(slot, &joystick, relative_mask);
        if (joystick.axis_valid_mask || button_seen || hat != SLOT2_GAMEPAD_HAT_UNCHANGED)
            hid_process_gamepad_report(slot, &joystick, hat, 0U);
    }
}

static void hid_process_report(usb_hid_slot_t *slot,
                               const uint8_t *report,
                               uint32_t len)
{
    if (slot == NULL || report == NULL || len == 0U) {
        return;
    }

    if (slot->ps4 != 0U) {
        onee_input_joystick_report_t gamepad;
        uint8_t hat;
        uint8_t extra_active;

        if (usb_ps4_decode(report, len, &gamepad, &hat, &extra_active) != 0U) {
            hid_process_gamepad_report(slot, &gamepad, hat, extra_active);
        }
        return;
    }

    if (slot->boot_mouse != 0U) {
        hid_process_boot_mouse_report(slot, report, len);
        return;
    }
    if (slot->boot_keyboard != 0U) {
        hid_process_boot_keyboard_report(slot, report, len);
        return;
    }
    if (slot->report_info_valid != 0U) {
        hid_process_report_protocol_report(slot, report, len);
        return;
    }

    if (slot->interface_protocol == HID_PROTOCOL_MOUSE && len >= 3U) {
        hid_process_boot_mouse_report(slot, report, len);
    } else if (slot->interface_protocol == HID_PROTOCOL_KEYBOARD && len >= 8U) {
        hid_process_boot_keyboard_report(slot, report, len);
    }
}

static uint32_t hid_report_len(const usb_hid_slot_t *slot)
{
    uint32_t len = HID_REPORT_BUFFER_BYTES;

    if (slot != NULL && slot->hid != NULL && slot->hid->intin != NULL) {
        uint32_t mps = USB_GET_MAXPACKETSIZE(slot->hid->intin->wMaxPacketSize);
        if (mps != 0U && mps < len) {
            len = mps;
        }
    }
    return len;
}

static void hid_report_complete(void *arg, int nbytes)
{
    usb_hid_slot_t *slot = (usb_hid_slot_t *)arg;

    if (slot == NULL) {
        return;
    }

    slot->report_pending = 0U;
    if (slot->active == 0U || slot->hid == NULL) {
        return;
    }

    if (nbytes > 0) {
        slot->error_log_suppressed = 0U;
        slot->last_error = 0;
        g_last_error = 0;
        slot->report_count++;
        g_report_count++;
        hid_process_report(slot, g_reports[slot->index], (uint32_t)nbytes);
    } else if (nbytes < 0) {
        slot->transfer_error_count++;
        g_transfer_error_count++;
        slot->last_error = nbytes;
        g_last_error = nbytes;
        if (slot->error_log_suppressed == 0U) {
            uart_puts(UART0_BASE, "[usb1] HID int transfer err slot=");
            uart_putdec(UART0_BASE, slot->index);
            uart_puts(UART0_BASE, " err=");
            uart_putdec(UART0_BASE, (uint32_t)(-nbytes));
            uart_puts(UART0_BASE, "\r\n");
            slot->error_log_suppressed = 1U;
        }
        /* Retry transport failures from the poll timer, not this completion.
         * A failing endpoint must not resubmit at bus speed. */
        XTime_GetTime(&slot->report_retry_started);
        slot->report_retry_armed = 1U;
        return;
    }

    hid_resubmit_report(slot);
}

static void hid_resubmit_report(usb_hid_slot_t *slot)
{
    uint32_t len;
    int rc;

    if (slot == NULL ||
        slot->active == 0U ||
        slot->hid == NULL ||
        slot->hid->hport == NULL ||
        slot->hid->intin == NULL) {
        return;
    }

    len = hid_report_len(slot);
    memset(g_reports[slot->index], 0, HID_REPORT_BUFFER_BYTES);
    usbh_int_urb_fill(&slot->hid->intin_urb,
                      slot->hid->hport,
                      slot->hid->intin,
                      g_reports[slot->index],
                      len,
                      0U,
                      hid_report_complete,
                      slot);

    rc = usbh_submit_urb(&slot->hid->intin_urb);
    if (rc == 0) {
        slot->report_pending = 1U;
        slot->report_retry_armed = 0U;
        slot->report_retry_started = 0U;
        return;
    }

    /* EHCI marks an URB busy before allocating its QH. If that allocation
     * fails, the port returns NOMEM with no QH but leaves BUSY in the URB.
     * Clear only that detached failure state so the timed retry can submit
     * the same interrupt URB again. Never clear a live controller-owned URB. */
    if (rc == -USB_ERR_NOMEM && slot->hid->intin_urb.hcpriv == NULL) {
        slot->hid->intin_urb.errorcode = 0;
    }

    XTime_GetTime(&slot->report_retry_started);
    slot->report_retry_armed = 1U;
    slot->submit_error_count++;
    g_submit_error_count++;
    slot->last_error = rc;
    g_last_error = rc;
    if (slot->error_log_suppressed == 0U) {
        uart_puts(UART0_BASE, "[usb1] HID int submit err slot=");
        uart_putdec(UART0_BASE, slot->index);
        uart_puts(UART0_BASE, " err=");
        uart_putdec(UART0_BASE, (uint32_t)(-rc));
        uart_puts(UART0_BASE, "\r\n");
        slot->error_log_suppressed = 1U;
    }
}

static void hid_slots_retry_reports(void)
{
    XTime now;

    XTime_GetTime(&now);
    for (uint32_t i = 0U; i < USB_HID_SLOT_COUNT; ++i) {
        usb_hid_slot_t *slot = &g_hid_slots[i];

        if (slot->active == 0U ||
            slot->vendor_gamepad != 0U ||
            slot->hid == NULL ||
            slot->report_pending != 0U ||
            slot->report_retry_armed == 0U ||
            (now - slot->report_retry_started) < HID_REPORT_RETRY_TICKS) {
            continue;
        }

        /* Clear first: a failed retry arms the next backoff itself. This
         * makes a transient EHCI busy/error at cold enumeration recover
         * without submitting the same interrupt URB on every fast poll. */
        slot->report_retry_armed = 0U;
        hid_resubmit_report(slot);
    }
}

static void hid_parse_report_descriptor(usb_hid_slot_t *slot)
{
    if (!slot || !slot->hid || !slot->hid->report_size || slot->hid->report_size > HID_REPORT_DESC_BYTES) return;
    memset(g_report_descs[slot->index], 0, HID_REPORT_DESC_BYTES);
    int result = usbh_hid_get_report_descriptor(slot->hid, g_report_descs[slot->index], slot->hid->report_size);
    if (result <= 0 || result > slot->hid->report_size) {
        slot->last_error = g_last_error = result < 0 ? result : -USB_ERR_INVAL;
        return;
    }
    const uint32_t length = (uint32_t)result;
    result = appletini_usb_parse_input_descriptor(g_report_descs[slot->index], length,
        &slot->report_info, &slot->descriptor_meta);
    if (result < 0) { slot->last_error = g_last_error = result; return; }
    slot->report_info_valid = 1U;
    slot->generic_axes = 1U;
    for (unsigned i = 0U; i < slot->descriptor_meta.application_count; ++i) {
        const unsigned capabilities = slot->descriptor_meta.applications[i].capabilities;
        if (capabilities & APPLETINI_USB_HID_APPLICATION_GAMEPAD) slot->generic_axes = 0U;
        if (capabilities & APPLETINI_USB_HID_APPLICATION_MOUSE) slot->mouse_capable = 1U;
        if (capabilities & (APPLETINI_USB_HID_APPLICATION_GAMEPAD | APPLETINI_USB_HID_APPLICATION_AXIS |
                            APPLETINI_USB_HID_APPLICATION_HAT)) slot->onee_joystick = 1U;
    }
    /* No collection may claim another application's fields. The fallback is
     * only for old descriptors which advertise joystick axes without a kind. */
    if (hid_report_info_has_absolute_joystick(slot) != 0U) {
        slot->onee_joystick = 1U;
    }
    if (hid_report_info_has_relative_mouse(slot)) slot->mouse_capable = 1U;
    if (slot->onee_joystick) {
        uint32_t buttons = 0U;
        uint16_t axes = 0U;
        for (unsigned i = 0U; i < slot->report_info.report_item_count; ++i) {
            const struct usbh_hid_report_item *item = &slot->report_info.report_items[i];
            const unsigned app = slot->descriptor_meta.field_application[i];
            if (!slot->generic_axes && app < slot->descriptor_meta.application_count &&
                !(slot->descriptor_meta.applications[app].capabilities & APPLETINI_USB_HID_APPLICATION_GAMEPAD)) continue;
            if (item->attribute.usage_page == HID_USAGE_PAGE_BUTTON) {
                for (unsigned usage = item->attribute.usage_min; usage <= item->attribute.usage_max && usage <= 32U; ++usage)
                    if (usage) buttons |= UINT32_C(1) << (usage - 1U);
            } else {
                uint16_t usage = item->attribute.usage_min;
                onee_input_axis_t axis;
                if (item->attribute.usage_page == 2U) usage = usage == 0xc4U ? HID_DESKTOP_USAGE_Z : HID_DESKTOP_USAGE_RZ;
                if (item->attribute.usage_page == 0x0cU) axes |= (uint16_t)(1U << ONEE_INPUT_AXIS_PAN);
                else if (hid_onee_axis_from_usage(usage, &axis)) axes |= (uint16_t)(1U << axis);
            }
        }
        identify_controller(slot->index, slot->hid->hport, slot->hid->intf, USB_CONTROLLER_TRANSPORT_HID,
            usb_controller_identity_hash(UINT32_C(2166136261), g_report_descs[slot->index], length),
            buttons, axes, slot->generic_axes);
    }
}

static void hid_log_connected(const usb_hid_slot_t *slot)
{
    const struct usbh_hid *hid;

    if (slot == NULL || slot->hid == NULL || slot->hid->hport == NULL) {
        return;
    }

    hid = slot->hid;
    uart_puts(UART0_BASE, "[usb1] CherryUSB HID connected slot=");
    uart_putdec(UART0_BASE, slot->index);
    uart_puts(UART0_BASE, " dev=");
    uart_putdec(UART0_BASE, hid->hport->dev_addr);
    uart_puts(UART0_BASE, " proto=");
    uart_putdec(UART0_BASE, slot->interface_protocol);
    uart_puts(UART0_BASE, " subclass=");
    uart_putdec(UART0_BASE, slot->interface_subclass);
    uart_puts(UART0_BASE, " parsed=");
    uart_putdec(UART0_BASE, slot->report_info_valid);
    uart_puts(UART0_BASE, " mouse=");
    uart_putdec(UART0_BASE, slot->mouse_capable);
    uart_puts(UART0_BASE, " joystick=");
    uart_putdec(UART0_BASE, slot->onee_joystick);
    uart_puts(UART0_BASE, " ps4=");
    uart_putdec(UART0_BASE, slot->ps4);
    uart_puts(UART0_BASE, "\r\n");
}

static volatile uint32_t g_usb1_activity_count = 0U;

/* Monotonic count of USB1 topology and enumeration events. Consumers use it
 * to detect quiescence while boot holds Apple release. Enumeration polling
 * must continue servicing on-demand Disk II track staging. */
uint32_t usb_hid_service_activity_count(void)
{
    return g_usb1_activity_count;
}

static void cherry_event_handler(uint8_t busid,
                                 uint8_t hub_index,
                                 uint8_t hub_port,
                                 uint8_t intf,
                                 uint8_t event)
{
    g_usb1_activity_count++;
    uart_puts(UART0_BASE, "[usb1] event bus=");
    uart_putdec(UART0_BASE, busid);
    uart_puts(UART0_BASE, " hub=");
    uart_putdec(UART0_BASE, hub_index);
    uart_puts(UART0_BASE, " port=");
    uart_putdec(UART0_BASE, hub_port);
    uart_puts(UART0_BASE, " intf=");
    uart_putdec(UART0_BASE, intf);
    uart_puts(UART0_BASE, " ev=");
    uart_putdec(UART0_BASE, event);
    uart_puts(UART0_BASE, "\r\n");
}

void usbh_hid_run(struct usbh_hid *hid_class)
{
    usb_hid_slot_t *slot;
    struct usb_interface_descriptor *intf_desc;

    if (hid_class == NULL || hid_class->hport == NULL) {
        return;
    }

    slot = hid_slot_from_hid(hid_class);
    if (slot != NULL) {
        return;
    }
    slot = hid_slot_find_free(hid_class->minor);
    if (slot == NULL) {
        uart_puts(UART0_BASE, "[usb1] no free input slot for HID\r\n");
        return;
    }

    onee_input_service_disconnect(slot->index);
    slot2_gamepad_service_disconnect(slot->index);
    hid_slot_reset(slot);
    intf_desc = &hid_class->hport->config.intf[hid_class->intf]
                     .altsetting[0]
                     .intf_desc;

    slot->hid = hid_class;
    slot->active = 1U;
    slot->interface_subclass = intf_desc->bInterfaceSubClass;
    slot->interface_protocol = hid_class->protocol;
    slot->ps4 = (uint8_t)(hid_class->protocol == 0U &&
                         slot->interface_subclass == 0U &&
                         usb_ps4_supported(hid_class->hport->device_desc.idVendor,
                                           hid_class->hport->device_desc.idProduct));
    slot->boot_mouse = (uint8_t)((hid_class->protocol == HID_PROTOCOL_MOUSE &&
                                  intf_desc->bInterfaceSubClass == HID_SUBCLASS_BOOTIF) ? 1U : 0U);
    slot->boot_keyboard = (uint8_t)((hid_class->protocol == HID_PROTOCOL_KEYBOARD &&
                                     intf_desc->bInterfaceSubClass == HID_SUBCLASS_BOOTIF) ? 1U : 0U);
    slot->mouse_capable = (uint8_t)((hid_class->protocol == HID_PROTOCOL_MOUSE) ? 1U : 0U);
    hid_class->user_data = slot;

    (void)usbh_hid_set_idle(hid_class, 0U, 0U);
    if (slot->ps4 != 0U) {
        /* DS4 USB report 1 needs no startup command. Decode it directly:
         * its explicit axis usages are not consecutive, and its right-stick
         * and trigger usage names differ from our shared gamepad mapping. */
        slot->onee_joystick = 1U;
        usb_gamepad_input_identify(slot->index, hid_class->hport, hid_class->intf, USB_CONTROLLER_TRANSPORT_PS4);
    } else {
        const uint8_t boot_keyboard = slot->boot_keyboard;
        const uint8_t boot_mouse = slot->boot_mouse;
        hid_parse_report_descriptor(slot);
        if ((boot_keyboard || boot_mouse) &&
            (!slot->report_info_valid || (boot_keyboard && !slot->onee_joystick) ||
             usbh_hid_set_protocol(hid_class, HID_PROTOCOL_REPORT) != 0)) {
            slot->report_info_valid = 0U;
            slot->onee_joystick = 0U;
            usb_controller_identity_disconnect(slot->index);
            slot->boot_keyboard = boot_keyboard;
            slot->boot_mouse = boot_mouse;
            if (usbh_hid_set_protocol(hid_class, HID_PROTOCOL_BOOT) != 0) {
                slot->boot_keyboard = slot->boot_mouse = slot->mouse_capable = 0U;
                slot->interface_protocol = 0U;
                slot->last_error = g_last_error = -USB_ERR_IO;
            } else if (boot_mouse) {
                slot->generic_axes = slot->onee_joystick = 1U;
                identify_controller(slot->index, hid_class->hport, hid_class->intf,
                    USB_CONTROLLER_TRANSPORT_HID, UINT32_C(0x424f4f54), 0x1fU, 3U, 1U);
            }
        } else {
            slot->boot_keyboard = slot->boot_mouse = 0U;
        }
    }

    hid_log_connected(slot);
    if (slot->mouse_capable != 0U) {
        mouse_mark_connected(slot);
    }

    hid_resubmit_report(slot);
}

void usbh_hid_stop(struct usbh_hid *hid_class)
{
    usb_hid_slot_t *slot;

    if (hid_class == NULL) {
        return;
    }

    slot = hid_slot_from_hid(hid_class);
    if (slot == NULL) {
        return;
    }

    uart_puts(UART0_BASE, "[usb1] CherryUSB HID disconnected slot=");
    uart_putdec(UART0_BASE, slot->index);
    uart_puts(UART0_BASE, "\r\n");

    mouse_release_slot(slot);
    onee_input_service_disconnect(slot->index);
    slot2_gamepad_service_disconnect(slot->index);
    usb_controller_identity_disconnect(slot->index);
    hid_class->user_data = NULL;
    hid_slot_reset(slot);
}

int usb_hid_service_init(void)
{
    onee_input_service_init();
    usb_controller_identity_init();
    g_started = 0U;
    g_ready = 0U;
    g_seq = 0U;
    g_sensitivity = MOUSE_SENSITIVITY_BASE;
    g_menu_capture = 0U;
    g_binding_capture = 0U;
    g_gamepad_binding_mask = 0U;
    g_joystick_preview = 0U;
    g_onee_fixed_mode = 0U;
    g_onee_standby_mode = 0U;
    g_onee_input_blocked = 0U;
    g_menu_ok_source = USB_HID_MENU_ACTION_SELECT;
    g_menu_open_close_source = USB_HID_MENU_ACTION_SELECT;
    g_screenshot_a2_source = USB_HID_MENU_SOURCE_NONE;
    g_screenshot_1080p_source = USB_HID_MENU_SOURCE_NONE;
    for (uint32_t i = 0U; i < USB_HID_VTW_SOURCE_COUNT; ++i) {
        g_vtw_sources[i] = USB_HID_MENU_SOURCE_NONE;
    }
    g_menu_event_rd = 0U;
    g_menu_event_wr = 0U;
    g_menu_event_count = 0U;
    g_x = 512;
    g_y = 384;
    g_x_residue = 0;
    g_y_residue = 0;
    g_report_count = 0U;
    g_submit_error_count = 0U;
    g_transfer_error_count = 0U;
    g_last_error = 0;
    hid_slots_reset_all();
    slot2_gamepad_service_set_blocked(0U);
    onee_input_service_release_all();
    mouse_publish_state(0U, g_x, g_y, 0U);

    uart_puts(UART0_BASE, "[usb1] CherryUSB HID service init (stopped)\r\n");
    return 0;
}

int usb_hid_service_start(void)
{
    struct usbh_bus *bus = &g_usbhost_bus[CHERRYUSB_USB1_BUSID];
    int rc;
    int cleanup_rc;

    if (g_started != 0U) {
        return 0;
    }
    if (g_power_shutdown_required != 0U) {
        rc = cherryusb_usb1_host_power_stop();
        if (rc != 0) {
            g_last_error = rc;
            return rc;
        }
        g_power_shutdown_required = 0U;
    }

    g_ready = 0U;
    g_report_count = 0U;
    g_submit_error_count = 0U;
    g_transfer_error_count = 0U;
    g_last_error = 0;
    hid_slots_reset_all();
    mouse_publish_state(0U, g_x, g_y, 0U);

    uart_puts(UART0_BASE, "[usb1] CherryUSB host start\r\n");
    rc = usbh_initialize(CHERRYUSB_USB1_BUSID, USB1_BASE, cherry_event_handler);
    g_power_shutdown_required = 1U;
    if (rc == 0) {
        g_started = 1U;
        /* This CherryUSB revision drops usbh_hub_initialize()'s error.
         * Its queue/semaphore pair exists only after the HC starts. */
        if (bus->hub_mq == NULL || bus->hub_sem == NULL) {
            rc = cherryusb_usb1_power_result();
            if (rc == 0) {
                rc = -USB_ERR_IO;
            }
        } else {
            rc = cherryusb_usb1_host_power_start();
        }
    }
    if (rc != 0) {
        usb_hid_service_stop();
        cleanup_rc = cherryusb_usb1_power_stop_result();
        if (cleanup_rc != 0) {
            rc = cleanup_rc;
        }
        g_last_error = rc;
        cherryusb_printf("[usb1] host start failed: %d\r\n", rc);
    }
    return rc;
}

void usb_hid_service_stop(void)
{
    int power_rc = 0;

    /* Remove VBUS independently: CherryUSB can return before its low-level
     * deinit hook if the controller fails to halt. */
    if (g_started != 0U || g_power_shutdown_required != 0U) {
        g_power_shutdown_required = 1U;
        power_rc = cherryusb_usb1_host_power_stop();
        if (power_rc == 0) {
            g_power_shutdown_required = 0U;
        }
    }
    if (g_started != 0U) {
        uart_puts(UART0_BASE, "[usb1] CherryUSB host stop\r\n");
        (void)usbh_deinitialize(CHERRYUSB_USB1_BUSID);
        power_rc = cherryusb_usb1_power_stop_result();
        if (power_rc != 0) {
            power_rc = cherryusb_usb1_host_power_stop();
        }
        g_power_shutdown_required = (power_rc != 0) ? 1U : 0U;
        g_started = 0U;
    }
    usb_xbox_one_stop();
    usb_xbox_360_stop();
    mouse_mark_disconnected();
    onee_input_service_release_all();
    hid_slots_reset_all();
    if (power_rc != 0) {
        g_power_shutdown_required = 1U;
        g_last_error = power_rc;
        cherryusb_printf("[usb1] host VBUS shutdown pending: %d\r\n", power_rc);
    }
}

void usb_hid_service_set_sensitivity(uint8_t sensitivity)
{
    if (sensitivity < MOUSE_SENSITIVITY_MIN) {
        sensitivity = MOUSE_SENSITIVITY_MIN;
    } else if (sensitivity > MOUSE_SENSITIVITY_MAX) {
        sensitivity = MOUSE_SENSITIVITY_MAX;
    }

    g_sensitivity = sensitivity;
    g_x_residue = 0;
    g_y_residue = 0;
}

void usb_hid_service_set_menu_capture(uint8_t capture)
{
    capture = (capture != 0U) ? 1U : 0U;
    if (g_menu_capture == capture) {
        return;
    }

    hid_clear_relative_inputs();
    g_menu_capture = capture;
    onee_input_service_set_blocked(
        (uint8_t)(g_menu_capture != 0U || g_onee_input_blocked != 0U));
    slot2_gamepad_service_set_blocked(
        (uint8_t)(g_menu_capture != 0U || g_onee_input_blocked != 0U));
    g_x_residue = 0;
    g_y_residue = 0;
    hid_slots_reset_menu_state();
    if (capture != 0U && g_ready != 0U) {
        g_x = (int32_t)(REG_READ(MOUSE_REG_X) & 0xFFFFU);
        g_y = (int32_t)(REG_READ(MOUSE_REG_Y) & 0xFFFFU);
        mouse_publish_state(1U, g_x, g_y, 0U);
    }
}

void usb_hid_service_set_joystick_preview(uint8_t active)
{
    active = (active != 0U) ? 1U : 0U;
    if (g_joystick_preview == active) {
        return;
    }
    g_joystick_preview = active;
    /* Changing this page only resets axis/hat navigation edges. Keep held
     * keyboard, mouse, and button actions, and the raw joystick reports. */
    for (uint32_t i = 0U; i < USB_HID_SLOT_COUNT; ++i) {
        g_hid_slots[i].prev_hat = HID_HAT_NEUTRAL;
        g_hid_slots[i].prev_x_dir = 0;
        g_hid_slots[i].prev_y_dir = 0;
    }
}

void usb_hid_service_set_gamepad_binding_mask(uint32_t mask)
{
    g_gamepad_binding_mask = mask;
}

void usb_hid_service_set_binding_capture(uint8_t active)
{
    active = active != 0U;
    if (g_binding_capture == active) return;
    g_binding_capture = active;
    for (uint32_t i = 0U; i < USB_HID_SLOT_COUNT; ++i) {
        usb_hid_slot_t *slot = &g_hid_slots[i];

        /* Pad learning must not reset another device's mouse edges or holds. */
        if (slot->vendor_gamepad == 0U && slot->onee_joystick == 0U) {
            continue;
        }
        slot->prev_gamepad_buttons = slot->gamepad_buttons;
        slot->prev_buttons = (uint8_t)(slot->gamepad_buttons & 0x1FU);
        slot->ok_down = slot->open_close_down = 0U;
    }
}

void usb_hid_service_set_onee_fixed_mode(uint8_t enable)
{
    enable = (enable != 0U) ? 1U : 0U;
    if (g_onee_fixed_mode == enable) {
        return;
    }

    g_onee_fixed_mode = enable;
    g_menu_event_rd = 0U;
    g_menu_event_wr = 0U;
    g_menu_event_count = 0U;
    hid_slots_reset_menu_state();
    if (enable != 0U) {
        for (uint32_t i = 0U; i < USB_HID_SLOT_COUNT; ++i) {
            g_hid_slots[i].raw_axis_active_mask = 0U;
            g_hid_slots[i].raw_axis_rest_valid_mask = 0U;
        }
    }
    /* Do not carry a key latched under one routing policy into the other. */
    onee_input_service_release_keyboard();
}

void usb_hid_service_set_onee_standby_mode(uint8_t enable)
{
    enable = (enable != 0U) ? 1U : 0U;
    if (g_onee_standby_mode == enable) {
        return;
    }
    g_onee_standby_mode = enable;
    for (uint32_t i = 0U; i < USB_HID_SLOT_COUNT; ++i) {
        usb_hid_slot_t *slot = &g_hid_slots[i];

        /* Cancel old keyboard holds without replaying held keys or disturbing
         * the mouse/gamepad paths on a composite device. */
        if (usb_hid_menu_source_is_keyboard(slot->open_close_down_source) != 0U) {
            menu_finish_open_close_hold(slot, slot->open_close_down_source);
        }
        if (usb_hid_menu_source_is_keyboard(slot->ok_down_source) != 0U) {
            slot->ok_down = slot->ok_hold_fired = 0U;
            slot->ok_down_tick = 0U;
            slot->ok_down_source = USB_HID_MENU_SOURCE_NONE;
            slot->ok_down_action = USB_HID_MENU_ACTION_NONE;
        }
    }
}

void usb_hid_service_set_onee_input_blocked(uint8_t blocked)
{
    blocked = (blocked != 0U) ? 1U : 0U;
    if (g_onee_input_blocked == blocked) {
        return;
    }

    g_onee_input_blocked = blocked;
    onee_input_service_set_blocked(
        (uint8_t)(g_menu_capture != 0U || g_onee_input_blocked != 0U));
    slot2_gamepad_service_set_blocked(
        (uint8_t)(g_menu_capture != 0U || g_onee_input_blocked != 0U));
}

uint8_t usb_hid_service_all_input_released(void)
{
    for (uint32_t i = 0U; i < USB_HID_SLOT_COUNT; ++i) {
        if (g_hid_slots[i].keyboard_keys_down != 0U ||
            g_hid_slots[i].raw_buttons_down != 0U ||
            g_hid_slots[i].raw_axis_active_mask != 0U ||
            g_hid_slots[i].raw_hat_active != 0U) {
            return 0U;
        }
    }
    return 1U;
}

void usb_hid_service_set_menu_ok_source(usb_hid_menu_source_t source)
{
    if (menu_hold_source_valid(source) == 0U) {
        source = USB_HID_MENU_ACTION_SELECT;
    }
    if (g_menu_ok_source == source) {
        return;
    }

    g_menu_ok_source = source;
    for (uint32_t i = 0U; i < USB_HID_SLOT_COUNT; ++i) {
        g_hid_slots[i].ok_down = 0U;
        g_hid_slots[i].ok_hold_fired = 0U;
        g_hid_slots[i].ok_down_tick = 0U;
        g_hid_slots[i].ok_down_source = USB_HID_MENU_SOURCE_NONE;
        g_hid_slots[i].ok_down_action = USB_HID_MENU_ACTION_NONE;
    }
}

void usb_hid_service_set_menu_open_close_source(usb_hid_menu_source_t source)
{
    if (menu_hold_source_valid(source) == 0U) {
        source = USB_HID_MENU_ACTION_SELECT;
    }
    if (g_menu_open_close_source == source) {
        return;
    }

    g_menu_open_close_source = source;
    for (uint32_t i = 0U; i < USB_HID_SLOT_COUNT; ++i) {
        g_hid_slots[i].open_close_down = 0U;
        g_hid_slots[i].open_close_hold_fired = 0U;
        g_hid_slots[i].open_close_down_tick = 0U;
        g_hid_slots[i].open_close_down_source = USB_HID_MENU_SOURCE_NONE;
    }
}

void usb_hid_service_set_screenshot_sources(usb_hid_menu_source_t a2_source,
                                            usb_hid_menu_source_t full_source)
{
    if (screenshot_source_valid(a2_source) == 0U) {
        a2_source = USB_HID_MENU_SOURCE_NONE;
    }
    if (screenshot_source_valid(full_source) == 0U) {
        full_source = USB_HID_MENU_SOURCE_NONE;
    }
    if (full_source == a2_source) {
        full_source = USB_HID_MENU_SOURCE_NONE;
    }

    g_screenshot_a2_source = a2_source;
    g_screenshot_1080p_source = full_source;
}

void usb_hid_service_set_vtw_sources(
    const usb_hid_menu_source_t sources[USB_HID_VTW_SOURCE_COUNT])
{
    for (uint32_t i = 0U; i < USB_HID_VTW_SOURCE_COUNT; ++i) {
        usb_hid_menu_source_t s =
            (sources != NULL) ? sources[i] : USB_HID_MENU_SOURCE_NONE;
        if (screenshot_source_valid(s) == 0U ||
            s == g_screenshot_a2_source ||
            s == g_screenshot_1080p_source) {
            s = USB_HID_MENU_SOURCE_NONE;
        }
        for (uint32_t j = 0U; j < i; ++j) {
            if (s != USB_HID_MENU_SOURCE_NONE && s == g_vtw_sources[j]) {
                s = USB_HID_MENU_SOURCE_NONE;
            }
        }
        g_vtw_sources[i] = s;
    }
}

int usb_hid_service_pop_menu_event(usb_hid_menu_event_t *event)
{
    if (event == NULL || g_menu_event_count == 0U) {
        return 0;
    }

    *event = g_menu_events[g_menu_event_rd];
    g_menu_event_rd = (uint8_t)((g_menu_event_rd + 1U) % MOUSE_MENU_EVENT_DEPTH);
    g_menu_event_count--;
    return 1;
}

void usb_hid_service_get_status(usb_hid_service_status_t *status)
{
    uint8_t active_count = 0U;
    uint8_t keyboard_count = 0U;
    uint8_t mouse_count = 0U;

    if (status == NULL) {
        return;
    }

    for (uint8_t i = 0U; i < USB_HID_SLOT_COUNT; ++i) {
        const usb_hid_slot_t *slot = &g_hid_slots[i];

        if (slot->active == 0U) {
            continue;
        }
        active_count++;
        if (slot->boot_keyboard != 0U ||
            slot->interface_protocol == HID_PROTOCOL_KEYBOARD ||
            slot->descriptor_meta.keyboard_item_count) {
            keyboard_count++;
        }
        if (slot->mouse_capable != 0U) {
            mouse_count++;
        }
    }

    status->started = g_started;
    status->ready = g_ready;
    status->menu_capture = g_menu_capture;
    status->onee_fixed_mode = g_onee_fixed_mode;
    status->onee_input_blocked = g_onee_input_blocked;
    status->active_count = active_count;
    status->keyboard_count = keyboard_count;
    status->mouse_count = mouse_count;
    status->report_count = g_report_count;
    status->submit_error_count = g_submit_error_count;
    status->transfer_error_count = g_transfer_error_count;
    status->last_error = g_last_error;
}

static void usb_hid_service_dump_slot(uint32_t uart_base,
                                        const usb_hid_slot_t *slot)
{
    const struct usbh_hid *hid;

    if (slot == NULL || slot->active == 0U || slot->hid == NULL) {
        return;
    }

    hid = slot->hid;
    uart_puts(uart_base, "hid");
    uart_putdec(uart_base, slot->index);
    uart_puts(uart_base, ": dev=");
    uart_putdec(uart_base, hid->hport ? hid->hport->dev_addr : 0U);
    uart_puts(uart_base, " if=");
    uart_putdec(uart_base, hid->intf);
    uart_puts(uart_base, " protocol=");
    uart_putdec(uart_base, slot->interface_protocol);
    uart_puts(uart_base, " subclass=");
    uart_putdec(uart_base, slot->interface_subclass);
    uart_puts(uart_base, " mousecard=");
    uart_putdec(uart_base, slot->mouse_card);
    uart_puts(uart_base, " parsed=");
    uart_putdec(uart_base, slot->report_info_valid);
    uart_puts(uart_base, " pending=");
    uart_putdec(uart_base, slot->report_pending);
    uart_puts(uart_base, " reports=");
    uart_putdec(uart_base, slot->report_count);
    uart_puts(uart_base, " submit_errs=");
    uart_putdec(uart_base, slot->submit_error_count);
    uart_puts(uart_base, " xfer_errs=");
    uart_putdec(uart_base, slot->transfer_error_count);
    uart_puts(uart_base, " last_err=");
    if (slot->last_error < 0) {
        uart_puts(uart_base, "-");
        uart_putdec(uart_base, (uint32_t)(-slot->last_error));
    } else {
        uart_putdec(uart_base, (uint32_t)slot->last_error);
    }
    uart_puts(uart_base, "\r\n");

    if (hid->intin != NULL) {
        uart_puts(uart_base, "  intin: ep=0x");
        uart_puthex(uart_base, hid->intin->bEndpointAddress);
        uart_puts(uart_base, " mps=");
        uart_putdec(uart_base, USB_GET_MAXPACKETSIZE(hid->intin->wMaxPacketSize));
        uart_puts(uart_base, " interval=");
        uart_putdec(uart_base, hid->intin->bInterval);
        if (hid->hport != NULL) {
            uart_puts(uart_base, " speed=");
            uart_putdec(uart_base, hid->hport->speed);
            uart_puts(uart_base, " hub=");
            uart_putdec(uart_base, hid->hport->parent ? hid->hport->parent->index : 0U);
            uart_puts(uart_base, " port=");
            uart_putdec(uart_base, hid->hport->port);
        }
        uart_puts(uart_base, "\r\n");
    }
    char detail[160];
    usb_controller_device_t device;
    if (usb_controller_identity_get(slot->index, &device)) {
        (void)snprintf(detail, sizeof(detail), "  input=%s vid=%04x pid=%04x generic=%u axes=%03x serial=%08lx path=%08lx\r\n",
            device.name, device.identity.vendor_id, device.identity.product_id,
            device.generic_axes, device.axis_mask, (unsigned long)device.identity.serial_hash,
            (unsigned long)device.identity.port_path);
        uart_puts(uart_base, detail);
    }
    for (unsigned i = 0U; i < slot->descriptor_meta.application_count; ++i) {
        const appletini_usb_hid_application_t *app = &slot->descriptor_meta.applications[i];
        (void)snprintf(detail, sizeof(detail), "  app%u page=%04x usage=%04x capabilities=%02x\r\n",
            i, app->usage_page, app->usage, app->capabilities);
        uart_puts(uart_base, detail);
    }
    for (unsigned i = 0U; i < slot->report_info.report_item_count; ++i) {
        const struct usbh_hid_report_item *item = &slot->report_info.report_items[i];
        (void)snprintf(detail, sizeof(detail), "  field%u app=%u id=%u page=%04x usage=%04x flags=%02x offset=%lu size=%u count=%lu\r\n",
            i, slot->descriptor_meta.field_application[i], item->attribute.report_id,
            item->attribute.usage_page, item->attribute.usage_min, item->report_flags,
            (unsigned long)item->report_bit_offset, item->attribute.report_size,
            (unsigned long)item->attribute.report_count);
        uart_puts(uart_base, detail);
    }

}

void usb_hid_service_dump_status(uint32_t uart_base)
{
    cherryusb_usb1_phy_status_t phy;
    cherryusb_host_debug_t host;
    char phy_line[256];
    uint32_t portsc;
    uint32_t hid_status;
    uint32_t mouse_x;
    uint32_t mouse_y;
    uint32_t mouse_buttons;
    uint32_t mouse_commit;
    uint32_t mouse_mode;
    uint32_t mouse_x_min;
    uint32_t mouse_x_max;
    uint32_t mouse_y_min;
    uint32_t mouse_y_max;

    portsc = cherryusb_usb1_portsc();
    cherryusb_usb1_phy_status(&phy);
    cherryusb_host_debug_snapshot(CHERRYUSB_USB1_BUSID, &host);
    hid_status = REG_READ(MOUSE_REG_STATUS);
    mouse_x = REG_READ(MOUSE_REG_X) & 0xFFFFU;
    mouse_y = REG_READ(MOUSE_REG_Y) & 0xFFFFU;
    mouse_buttons = REG_READ(MOUSE_REG_BUTTONS);
    mouse_commit = REG_READ(MOUSE_REG_COMMIT);
    mouse_mode = REG_READ(MOUSE_REG_MODE);
    mouse_x_min = REG_READ(MOUSE_REG_X_MIN) & 0xFFFFU;
    mouse_x_max = REG_READ(MOUSE_REG_X_MAX) & 0xFFFFU;
    mouse_y_min = REG_READ(MOUSE_REG_Y_MIN) & 0xFFFFU;
    mouse_y_max = REG_READ(MOUSE_REG_Y_MAX) & 0xFFFFU;

    uart_puts(uart_base, "---- usb1 CherryUSB HID ----\r\n");
    uart_puts(uart_base, "service: started=");
    uart_putdec(uart_base, g_started);
    uart_puts(uart_base, " ready=");
    uart_putdec(uart_base, g_ready);
    uart_puts(uart_base, " active_hids=");
    uart_putdec(uart_base, hid_active_count());
    uart_puts(uart_base, " report_pending=");
    uart_putdec(uart_base, hid_pending_count());
    uart_puts(uart_base, " reports=");
    uart_putdec(uart_base, g_report_count);
    uart_puts(uart_base, " submit_errs=");
    uart_putdec(uart_base, g_submit_error_count);
    uart_puts(uart_base, " xfer_errs=");
    uart_putdec(uart_base, g_transfer_error_count);
    uart_puts(uart_base, " last_err=");
    if (g_last_error < 0) {
        uart_puts(uart_base, "-");
        uart_putdec(uart_base, (uint32_t)(-g_last_error));
    } else {
        uart_putdec(uart_base, (uint32_t)g_last_error);
    }
    uart_puts(uart_base, "\r\n");

    (void)snprintf(phy_line, sizeof(phy_line),
                   "ehci: errors=%lu token=%08lx qtd=%u pid=%u length=%u remaining=%u actual=%u\r\n",
                   (unsigned long)host.ehci_error_count,
                   (unsigned long)host.last_ehci_qtd_token,
                   host.last_ehci_qtd_index, host.last_ehci_qtd_pid,
                   host.last_ehci_qtd_length, host.last_ehci_qtd_remaining,
                   host.last_ehci_qtd_actual);
    uart_puts(uart_base, phy_line);

    uart_puts(uart_base, "mouse_regs: status=0x");
    uart_puthex(uart_base, hid_status);
    uart_puts(uart_base, " x=");
    uart_putdec(uart_base, mouse_x);
    uart_puts(uart_base, " y=");
    uart_putdec(uart_base, mouse_y);
    uart_puts(uart_base, " buttons=0x");
    uart_puthex(uart_base, mouse_buttons);
    uart_puts(uart_base, " commit=0x");
    uart_puthex(uart_base, mouse_commit);
    uart_puts(uart_base, " mode=0x");
    uart_puthex(uart_base, mouse_mode);
    uart_puts(uart_base, " clamp_x=");
    uart_putdec(uart_base, mouse_x_min);
    uart_puts(uart_base, "..");
    uart_putdec(uart_base, mouse_x_max);
    uart_puts(uart_base, " clamp_y=");
    uart_putdec(uart_base, mouse_y_min);
    uart_puts(uart_base, "..");
    uart_putdec(uart_base, mouse_y_max);
    uart_puts(uart_base, "\r\n");

    uart_puts(uart_base, "root: PORTSC=0x");
    uart_puthex(uart_base, portsc);
    uart_puts(uart_base, " ccs=");
    uart_putdec(uart_base, (portsc & XUSBPS_PORTSCR_CCS_MASK) ? 1U : 0U);
    uart_puts(uart_base, " pe=");
    uart_putdec(uart_base, (portsc & XUSBPS_PORTSCR_PE_MASK) ? 1U : 0U);
    uart_puts(uart_base, " pp=");
    uart_putdec(uart_base, (portsc & XUSBPS_PORTSCR_PP_MASK) ? 1U : 0U);
    uart_puts(uart_base, " phcd=");
    uart_putdec(uart_base, (portsc & XUSBPS_PORTSCR_PHCD_MASK) ? 1U : 0U);
    uart_puts(uart_base, " pspd=");
    uart_putdec(uart_base, (portsc & XUSBPS_PORTSCR_PSPD_MASK) >> 26);
    uart_puts(uart_base, "\r\n");

    uart_puts(uart_base, "hid: aggregate_mice=");
    uart_putdec(uart_base, mouse_card_active_count());
    uart_puts(uart_base, " aggregate_buttons=0x");
    uart_puthex(uart_base, mouse_card_aggregate_buttons());
    uart_puts(uart_base, "\r\n");

    (void)snprintf(phy_line, sizeof(phy_line),
                   "phy (cached): setup_rc=%d off_rc=%d drive_known=%u drive=%u "
                   "vbus_known=%u vbus_valid=%u id=%04x:%04x "
                   "func=%02x iface=%02x otg=%02x ulpi_err=%d timeouts=%u\r\n",
                   phy.power_result, phy.disable_result,
                   (unsigned int)phy.vbus_drive_known,
                   (unsigned int)phy.vbus_drive_enabled,
                   (unsigned int)phy.vbus_valid_known,
                   (unsigned int)phy.vbus_valid,
                   (unsigned int)phy.vendor_id, (unsigned int)phy.product_id,
                   (unsigned int)phy.function_control,
                   (unsigned int)phy.interface_control,
                   (unsigned int)phy.otg_control,
                   phy.last_error, (unsigned int)phy.timeouts);
    uart_puts(uart_base, phy_line);

    for (uint32_t i = 0U; i < USB_HID_SLOT_COUNT; ++i) {
        usb_hid_service_dump_slot(uart_base, &g_hid_slots[i]);
    }
    usb_xbox_one_dump_status(uart_base);
    usb_xbox_360_dump_status(uart_base);
    slot2_gamepad_snapshot_t slot2;
    slot2_gamepad_service_get_snapshot(&slot2);
    (void)snprintf(phy_line, sizeof(phy_line),
                   "slot2: card=%u fpga=%u blocked=%u present=%x\r\n",
                   slot2.card, slot2.available, slot2.blocked, slot2.present);
    uart_puts(uart_base, phy_line);
    for (uint8_t player = 0U; player < SLOT2_GAMEPAD_PLAYER_COUNT; ++player) {
        (void)snprintf(phy_line, sizeof(phy_line),
                       "  player%u: usb=%u buttons=%03x\r\n", player + 1U,
                       slot2.slots[player] < ONEE_INPUT_DEVICE_SLOT_COUNT ?
                           slot2.slots[player] + 1U : 0U,
                       slot2.buttons[player]);
        uart_puts(uart_base, phy_line);
    }
}

void usb_hid_service_poll(void)
{
    https_probe_service_tick(HTTPS_PROBE_USB1);
    /* Arm a newly running session before this poll can deliver its first HID
     * report, then flush report changes without waiting for the next loop. */
    onee_input_service_poll();
    if (g_started != 0U) {
        cherryusb_host_poll(CHERRYUSB_USB1_BUSID);
        usb_xbox_one_poll();
        usb_xbox_360_poll();
        hid_poll_relative_inputs();
        hid_slots_retry_reports();
        hid_slots_poll_holds();
        onee_input_service_poll();
    }
    slot2_gamepad_service_poll();
}
