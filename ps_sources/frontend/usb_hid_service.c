/******************************************************************************
 * Appletini USB1 HID service
 *
 * CherryUSB host adapter for USB1. CherryUSB owns USB enumeration, hubs, split
 * transactions, and HID class binding; this file bridges HID input reports into
 * the slot-4 MouseCard register block and config-menu events.
 ******************************************************************************/

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "xiltimer.h"
#include "xparameters.h"
#include "xusbps_hw.h"

#include "usb_hid_service.h"

#include "cherryusb_platform.h"
#include "usbh_core.h"
#include "usbh_hid.h"
#include "usb_def.h"
#include "usb_errno.h"
#include "usb_hid.h"

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

#define USB_HID_SLOT_COUNT CONFIG_USBHOST_MAX_HID_CLASS
#define HID_REPORT_BYTES 64U
#define HID_REPORT_BUFFER_BYTES USB_ALIGN_UP(HID_REPORT_BYTES, CONFIG_USB_ALIGN_SIZE)
#define HID_REPORT_DESC_BYTES CONFIG_USBHOST_REQUEST_BUFFER_LEN
#define HID_KEY_TRACK_COUNT 8U
#define HID_SOURCE_TRACK_COUNT (HID_KEY_TRACK_COUNT + 8U)
#define HID_HAT_NEUTRAL 8U

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
    uint8_t boot_mouse;
    uint8_t boot_keyboard;
    uint8_t mouse_capable;
    uint8_t mouse_card;
    uint8_t report_info_valid;
    uint8_t report_pending;
    uint8_t prev_buttons;
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
    struct usbh_hid *hid;
    struct usbh_hid_report_info report_info;
    uint32_t report_count;
    uint32_t submit_error_count;
    uint32_t transfer_error_count;
    int last_error;
    uint8_t error_log_suppressed;
    uint8_t interface_subclass;
    uint8_t interface_protocol;
} usb_hid_slot_t;

static uint8_t g_started;
static uint8_t g_ready;
static uint8_t g_seq;
static uint8_t g_sensitivity = MOUSE_SENSITIVITY_BASE;
static uint8_t g_menu_capture;
static usb_hid_menu_event_t g_menu_events[MOUSE_MENU_EVENT_DEPTH];
static uint8_t g_menu_event_rd;
static uint8_t g_menu_event_wr;
static uint8_t g_menu_event_count;
static usb_hid_menu_source_t g_menu_ok_source = USB_HID_MENU_ACTION_SELECT;
static usb_hid_menu_source_t g_menu_open_close_source = USB_HID_MENU_ACTION_SELECT;
static usb_hid_menu_source_t g_screenshot_a2_source = USB_HID_MENU_SOURCE_NONE;
static usb_hid_menu_source_t g_screenshot_1080p_source = USB_HID_MENU_SOURCE_NONE;
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

static int32_t mouse_clamp_10bit(int32_t value)
{
    if (value < 0) {
        return 0;
    }
    if (value > 1023) {
        return 1023;
    }
    return value;
}

static int32_t mouse_clamp_range(int32_t value, uint32_t min_value, uint32_t max_value)
{
    min_value &= 0x3FFU;
    max_value &= 0x3FFU;
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
            usb_hid_menu_source_is_keyboard(source) != 0U) ? 1U : 0U;
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
            usb_hid_menu_source_is_keyboard(source) != 0U) ? 1U : 0U;
}

static void hid_slot_reset_menu_state(usb_hid_slot_t *slot)
{
    if (slot == NULL) {
        return;
    }
    slot->prev_buttons = 0U;
    memset(slot->prev_sources, 0, sizeof(slot->prev_sources));
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
    hid_slot_reset_menu_state(slot);
}

static void hid_slots_reset_all(void)
{
    for (uint32_t i = 0U; i < USB_HID_SLOT_COUNT; ++i) {
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
    if (hid == NULL || hid->minor >= USB_HID_SLOT_COUNT) {
        return NULL;
    }
    return &g_hid_slots[hid->minor];
}

static uint32_t hid_active_count(void)
{
    uint32_t count = 0U;

    for (uint32_t i = 0U; i < USB_HID_SLOT_COUNT; ++i) {
        if (g_hid_slots[i].active != 0U) {
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

static void mouse_publish_state(uint8_t connected, int32_t x, int32_t y, uint8_t buttons)
{
    REG_WRITE(MOUSE_REG_STATUS, connected ? 1U : 0U);
    REG_WRITE(MOUSE_REG_X, (uint32_t)mouse_clamp_10bit(x));
    REG_WRITE(MOUSE_REG_Y, (uint32_t)mouse_clamp_10bit(y));
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
        g_x = (int32_t)(REG_READ(MOUSE_REG_X) & 0x3FFU);
        g_y = (int32_t)(REG_READ(MOUSE_REG_Y) & 0x3FFU);
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

    g_x = (int32_t)(REG_READ(MOUSE_REG_X) & 0x3FFU);
    g_y = (int32_t)(REG_READ(MOUSE_REG_Y) & 0x3FFU);
    g_x = mouse_clamp_range(g_x + dx, x_min, x_max);
    g_y = mouse_clamp_range(g_y + dy, y_min, y_max);
    mouse_publish_state(1U, g_x, g_y, mouse_card_aggregate_buttons());
}

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
    raw_dx = (int8_t)report[1];
    raw_dy = (int8_t)report[2];
    wheel = (len > MOUSE_BOOT_WHEEL_INDEX) ? (int8_t)report[MOUSE_BOOT_WHEEL_INDEX] : 0;

    mouse_menu_process_buttons(slot, buttons, wheel);
    mouse_apply_motion(slot, raw_dx, raw_dy, buttons);
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

static uint8_t hid_key_in_list(uint8_t key, const uint8_t *keys, uint32_t count)
{
    if (key == HID_KBD_USAGE_NONE || keys == NULL) {
        return 0U;
    }

    for (uint32_t i = 0U; i < count; ++i) {
        if (keys[i] == key) {
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
                                        usb_hid_menu_source_t source)
{
    if (slot == NULL) {
        return;
    }
    slot->ok_down = 1U;
    slot->ok_hold_fired = 0U;
    slot->ok_down_source = source;
    XTime_GetTime(&slot->ok_down_tick);
}

static void hid_process_keyboard_usages(usb_hid_slot_t *slot,
                                        uint8_t modifier,
                                        const uint8_t *keys,
                                        uint32_t count)
{
    usb_hid_menu_source_t next_sources[HID_SOURCE_TRACK_COUNT];
    uint32_t next_count = 0U;

    if (slot == NULL || keys == NULL) {
        return;
    }

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
        if (hid_source_in_list(next_sources[i],
                               slot->prev_sources,
                               HID_SOURCE_TRACK_COUNT) != 0U) {
            continue;
        }
        if (screenshot_push_source(next_sources[i]) != 0U) {
            continue;
        }
        if (next_sources[i] == g_menu_open_close_source) {
            menu_start_open_close_hold(slot, next_sources[i]);
            if (g_menu_capture != 0U && next_sources[i] == g_menu_ok_source) {
                keyboard_menu_start_ok_hold(slot, next_sources[i]);
            }
        } else if (g_menu_capture != 0U && next_sources[i] == g_menu_ok_source) {
            keyboard_menu_start_ok_hold(slot, next_sources[i]);
        } else if (g_menu_capture != 0U) {
            keyboard_menu_push_source(next_sources[i]);
        }
    }

    if (slot->ok_down != 0U &&
        slot->ok_hold_fired == 0U &&
        hid_source_in_list(slot->ok_down_source, next_sources, next_count) == 0U) {
        keyboard_menu_push_source(slot->ok_down_source);
    }
    if (slot->ok_down != 0U &&
        hid_source_in_list(slot->ok_down_source, next_sources, next_count) == 0U) {
        slot->ok_down = 0U;
        slot->ok_hold_fired = 0U;
        slot->ok_down_tick = 0U;
        slot->ok_down_source = USB_HID_MENU_SOURCE_NONE;
    }
    if (slot->open_close_down != 0U &&
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

static void hid_key_list_add(uint8_t *keys, uint32_t *count, uint8_t key)
{
    if (keys == NULL ||
        count == NULL ||
        *count >= HID_KEY_TRACK_COUNT ||
        key <= HID_KBD_USAGE_ERRUNDEF ||
        key > HID_KBD_USAGE_MAX ||
        hid_key_in_list(key, keys, *count) != 0U) {
        return;
    }

    keys[*count] = key;
    (*count)++;
}

static void hid_collect_keyboard_item(const struct usbh_hid_report_item *item,
                                      const uint8_t *report,
                                      uint32_t len,
                                      uint8_t *modifier,
                                      uint8_t *keys,
                                      uint32_t *key_count,
                                      uint8_t *keyboard_seen)
{
    const uint8_t variable = (uint8_t)((item->report_flags & HID_MAINITEM_VARIABLE) ? 1U : 0U);

    if (item->attribute.usage_page != HID_USAGE_PAGE_KEYBOARD_KEYPAD ||
        item->report_type != HID_REPORT_INPUT) {
        return;
    }

    if (keyboard_seen != NULL) {
        *keyboard_seen = 1U;
    }

    for (uint32_t field = 0U; field < item->attribute.report_count; ++field) {
        uint32_t raw = 0U;
        uint16_t usage;

        if (hid_extract_bits(item, report, len, field, &raw) == 0U) {
            continue;
        }

        if (variable != 0U) {
            usage = (uint16_t)(item->attribute.usage_min + field);
            if (raw == 0U) {
                continue;
            }
        } else {
            usage = (uint16_t)raw;
        }

        if (usage >= HID_KBD_USAGE_LCTRL && usage <= HID_KBD_USAGE_RGUI) {
            if (modifier != NULL) {
                *modifier |= (uint8_t)(1U << (usage - HID_KBD_USAGE_LCTRL));
            }
            continue;
        }

        hid_key_list_add(keys, key_count, (uint8_t)usage);
    }
}

static void hid_collect_button_item(usb_hid_slot_t *slot,
                                    const struct usbh_hid_report_item *item,
                                    const uint8_t *report,
                                    uint32_t len,
                                    uint8_t *buttons,
                                    uint8_t *button_seen)
{
    if (slot == NULL ||
        item->attribute.usage_page != HID_USAGE_PAGE_BUTTON ||
        item->report_type != HID_REPORT_INPUT ||
        (item->report_flags & HID_MAINITEM_VARIABLE) == 0U) {
        return;
    }

    for (uint32_t field = 0U; field < item->attribute.report_count; ++field) {
        uint32_t raw = 0U;
        uint16_t usage = (uint16_t)(item->attribute.usage_min + field);

        if (usage < 1U || usage > 5U) {
            continue;
        }
        if (hid_extract_bits(item, report, len, field, &raw) == 0U) {
            continue;
        }

        *button_seen = 1U;
        if (raw != 0U) {
            *buttons |= (uint8_t)(1U << (usage - 1U));
        }
    }
}

static int8_t hid_axis_direction(int32_t value,
                                 int32_t logical_min,
                                 int32_t logical_max)
{
    int32_t span;
    int32_t low;
    int32_t high;

    if (logical_max <= logical_min) {
        return 0;
    }

    span = logical_max - logical_min;
    low = logical_min + (span / 3);
    high = logical_min + ((span * 2) / 3);
    if (value < low) {
        return -1;
    }
    if (value > high) {
        return 1;
    }
    return 0;
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

static void hid_menu_push_hat(usb_hid_slot_t *slot, uint8_t hat)
{
    if (slot == NULL || hat == slot->prev_hat) {
        return;
    }

    slot->prev_hat = hat;
    if (hat > 7U) {
        return;
    }

    if (hat == 7U || hat == 0U || hat == 1U) {
        mouse_menu_push_bindable_action(USB_HID_MENU_ACTION_ITEM_UP);
    }
    if (hat == 3U || hat == 4U || hat == 5U) {
        mouse_menu_push_bindable_action(USB_HID_MENU_ACTION_ITEM_DOWN);
    }
    if (hat == 5U || hat == 6U || hat == 7U) {
        mouse_menu_push_bindable_action(USB_HID_MENU_ACTION_LEFT);
    }
    if (hat == 1U || hat == 2U || hat == 3U) {
        mouse_menu_push_bindable_action(USB_HID_MENU_ACTION_RIGHT);
    }
}

static void hid_menu_push_axis(usb_hid_slot_t *slot,
                               int8_t *previous,
                               int8_t direction,
                               usb_hid_menu_action_t negative_action,
                               usb_hid_menu_action_t positive_action)
{
    if (slot == NULL || previous == NULL || *previous == direction) {
        return;
    }

    *previous = direction;
    if (direction < 0) {
        mouse_menu_push_bindable_action(negative_action);
    } else if (direction > 0) {
        mouse_menu_push_bindable_action(positive_action);
    }
}

static void hid_collect_desktop_item(usb_hid_slot_t *slot,
                                     const struct usbh_hid_report_item *item,
                                     const uint8_t *report,
                                     uint32_t len,
                                     int32_t *dx,
                                     int32_t *dy,
                                     uint8_t *mouse_delta_seen,
                                     int8_t *wheel,
                                     uint8_t *wheel_seen)
{
    const uint8_t relative = (uint8_t)((item->report_flags & HID_MAINITEM_RELATIVE) ? 1U : 0U);

    if (slot == NULL ||
        item->attribute.usage_page != HID_USAGE_PAGE_GENERIC_DESKTOP_CONTROLS ||
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
        switch (usage) {
        case HID_DESKTOP_USAGE_X:
            if (relative != 0U) {
                *dx += value;
                *mouse_delta_seen = 1U;
            } else if (g_menu_capture != 0U) {
                hid_menu_push_axis(slot,
                                   &slot->prev_x_dir,
                                   hid_axis_direction(value,
                                                      item->attribute.logical_min,
                                                      item->attribute.logical_max),
                                   USB_HID_MENU_ACTION_LEFT,
                                   USB_HID_MENU_ACTION_RIGHT);
            }
            break;
        case HID_DESKTOP_USAGE_Y:
            if (relative != 0U) {
                *dy += value;
                *mouse_delta_seen = 1U;
            } else if (g_menu_capture != 0U) {
                hid_menu_push_axis(slot,
                                   &slot->prev_y_dir,
                                   hid_axis_direction(value,
                                                      item->attribute.logical_min,
                                                      item->attribute.logical_max),
                                   USB_HID_MENU_ACTION_ITEM_UP,
                                   USB_HID_MENU_ACTION_ITEM_DOWN);
            }
            break;
        case HID_DESKTOP_USAGE_WHEEL:
            *wheel = (int8_t)value;
            *wheel_seen = 1U;
            break;
        case HID_DESKTOP_USAGE_HATSWITCH:
            if (g_menu_capture != 0U) {
                hid_menu_push_hat(slot, (uint8_t)value);
            }
            break;
        default:
            break;
        }
    }
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

static void hid_process_report_protocol_report(usb_hid_slot_t *slot,
                                               const uint8_t *report,
                                               uint32_t len)
{
    uint8_t keys[HID_KEY_TRACK_COUNT];
    uint32_t key_count = 0U;
    uint8_t modifier = 0U;
    uint8_t keyboard_seen = 0U;
    uint8_t buttons = 0U;
    uint8_t button_seen = 0U;
    int8_t wheel = 0;
    uint8_t wheel_seen = 0U;
    int32_t dx = 0;
    int32_t dy = 0;
    uint8_t mouse_delta_seen = 0U;

    if (slot == NULL || report == NULL || slot->report_info_valid == 0U) {
        return;
    }

    memset(keys, 0, sizeof(keys));
    for (uint32_t i = 0U; i < slot->report_info.report_item_count; ++i) {
        struct usbh_hid_report_item *item = &slot->report_info.report_items[i];

        if (item->report_type != HID_REPORT_INPUT ||
            (item->report_flags & HID_MAINITEM_CONSTANT) != 0U ||
            hid_report_id_matches(item, report, len) == 0U) {
            continue;
        }

        hid_collect_keyboard_item(item,
                                  report,
                                  len,
                                  &modifier,
                                  keys,
                                  &key_count,
                                  &keyboard_seen);
        hid_collect_button_item(slot,
                                item,
                                report,
                                len,
                                &buttons,
                                &button_seen);
        hid_collect_desktop_item(slot,
                                 item,
                                 report,
                                 len,
                                 &dx,
                                 &dy,
                                 &mouse_delta_seen,
                                 &wheel,
                                 &wheel_seen);
    }

    if (keyboard_seen != 0U) {
        hid_process_keyboard_usages(slot, modifier, keys, key_count);
    }

    if (button_seen != 0U || wheel_seen != 0U) {
        mouse_menu_process_buttons(slot,
                                   (button_seen != 0U) ? buttons : slot->prev_buttons,
                                   wheel);
    }

    if (mouse_delta_seen != 0U || button_seen != 0U) {
        mouse_apply_motion(slot, dx, dy, (button_seen != 0U) ? buttons : slot->prev_buttons);
    }
}

static void hid_process_report(usb_hid_slot_t *slot,
                               const uint8_t *report,
                               uint32_t len)
{
    if (slot == NULL || report == NULL || len == 0U) {
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
        return;
    }

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

static void hid_parse_report_descriptor(usb_hid_slot_t *slot)
{
    int rc;

    if (slot == NULL ||
        slot->hid == NULL ||
        slot->hid->report_size == 0U ||
        slot->hid->report_size > HID_REPORT_DESC_BYTES) {
        return;
    }

    memset(g_report_descs[slot->index], 0, HID_REPORT_DESC_BYTES);
    rc = usbh_hid_get_report_descriptor(slot->hid,
                                        g_report_descs[slot->index],
                                        slot->hid->report_size);
    if (rc < 0) {
        slot->last_error = rc;
        g_last_error = rc;
        return;
    }

    rc = usbh_hid_parse_report_descriptor(g_report_descs[slot->index],
                                          slot->hid->report_size,
                                          &slot->report_info);
    if (rc == 0) {
        slot->report_info_valid = 1U;
        if (hid_report_info_has_relative_mouse(slot) != 0U) {
            slot->mouse_capable = 1U;
        }
    } else {
        slot->last_error = rc;
        g_last_error = rc;
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
    if (slot == NULL) {
        return;
    }

    hid_slot_reset(slot);
    intf_desc = &hid_class->hport->config.intf[hid_class->intf]
                     .altsetting[0]
                     .intf_desc;

    slot->hid = hid_class;
    slot->active = 1U;
    slot->interface_subclass = intf_desc->bInterfaceSubClass;
    slot->interface_protocol = hid_class->protocol;
    slot->boot_mouse = (uint8_t)((hid_class->protocol == HID_PROTOCOL_MOUSE &&
                                  intf_desc->bInterfaceSubClass == HID_SUBCLASS_BOOTIF) ? 1U : 0U);
    slot->boot_keyboard = (uint8_t)((hid_class->protocol == HID_PROTOCOL_KEYBOARD &&
                                     intf_desc->bInterfaceSubClass == HID_SUBCLASS_BOOTIF) ? 1U : 0U);
    slot->mouse_capable = (uint8_t)((hid_class->protocol == HID_PROTOCOL_MOUSE) ? 1U : 0U);
    hid_class->user_data = slot;

    (void)usbh_hid_set_idle(hid_class, 0U, 0U);
    if (slot->boot_keyboard != 0U) {
        /* Keyboards: boot protocol. Fixed 8-byte layout, exactly what the
         * accel-keys / menu key tracking parse. */
        (void)usbh_hid_set_protocol(hid_class, HID_PROTOCOL_BOOT);
    } else {
        /* Mice (and everything else): stay in REPORT protocol (the HID
         * default after reset) and parse the report descriptor. Forcing
         * boot protocol on mice loses the scroll wheel -- the boot report
         * is 3 bytes (buttons/X/Y) and a spec-conforming mouse stops
         * sending the wheel byte. Fall back to boot protocol only when
         * the descriptor parse fails on a boot-capable mouse. */
        hid_parse_report_descriptor(slot);
        if (slot->report_info_valid == 0U && slot->boot_mouse != 0U) {
            (void)usbh_hid_set_protocol(hid_class, HID_PROTOCOL_BOOT);
        } else {
            slot->boot_mouse = 0U;  /* parsed path owns the reports */
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

    slot = (usb_hid_slot_t *)hid_class->user_data;
    if (slot == NULL) {
        slot = hid_slot_from_hid(hid_class);
    }
    if (slot == NULL || slot->hid != hid_class) {
        return;
    }

    uart_puts(UART0_BASE, "[usb1] CherryUSB HID disconnected slot=");
    uart_putdec(UART0_BASE, slot->index);
    uart_puts(UART0_BASE, "\r\n");

    mouse_release_slot(slot);
    hid_class->user_data = NULL;
    hid_slot_reset(slot);
}

int usb_hid_service_init(void)
{
    g_started = 0U;
    g_ready = 0U;
    g_seq = 0U;
    g_sensitivity = MOUSE_SENSITIVITY_BASE;
    g_menu_capture = 0U;
    g_menu_ok_source = USB_HID_MENU_ACTION_SELECT;
    g_menu_open_close_source = USB_HID_MENU_ACTION_SELECT;
    g_screenshot_a2_source = USB_HID_MENU_SOURCE_NONE;
    g_screenshot_1080p_source = USB_HID_MENU_SOURCE_NONE;
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
    mouse_publish_state(0U, g_x, g_y, 0U);

    uart_puts(UART0_BASE, "[usb1] CherryUSB HID service init (stopped)\r\n");
    return 0;
}

int usb_hid_service_start(void)
{
    int rc;

    if (g_started != 0U) {
        return 0;
    }

    g_ready = 0U;
    g_report_count = 0U;
    g_submit_error_count = 0U;
    g_transfer_error_count = 0U;
    g_last_error = 0;
    hid_slots_reset_all();
    mouse_publish_state(0U, g_x, g_y, 0U);

    uart_puts(UART0_BASE, "[usb1] CherryUSB host start\r\n");
    g_started = 1U;
    rc = usbh_initialize(CHERRYUSB_USB1_BUSID, USB1_BASE, cherry_event_handler);
    if (rc != 0) {
        g_started = 0U;
        g_last_error = rc;
    }
    return rc;
}

void usb_hid_service_stop(void)
{
    if (g_started == 0U) {
        mouse_mark_disconnected();
        hid_slots_reset_all();
        return;
    }

    uart_puts(UART0_BASE, "[usb1] CherryUSB host stop\r\n");
    (void)usbh_deinitialize(CHERRYUSB_USB1_BUSID);
    g_started = 0U;
    mouse_mark_disconnected();
    hid_slots_reset_all();
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

    g_menu_capture = capture;
    g_x_residue = 0;
    g_y_residue = 0;
    hid_slots_reset_menu_state();
    if (capture != 0U && g_ready != 0U) {
        g_x = (int32_t)(REG_READ(MOUSE_REG_X) & 0x3FFU);
        g_y = (int32_t)(REG_READ(MOUSE_REG_Y) & 0x3FFU);
        mouse_publish_state(1U, g_x, g_y, 0U);
    }
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
            slot->interface_protocol == HID_PROTOCOL_KEYBOARD) {
            keyboard_count++;
        }
        if (slot->mouse_capable != 0U) {
            mouse_count++;
        }
    }

    status->started = g_started;
    status->ready = g_ready;
    status->menu_capture = g_menu_capture;
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
        uart_puts(uart_base, "\r\n");
    }
}

void usb_hid_service_dump_status(uint32_t uart_base)
{
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
    hid_status = REG_READ(MOUSE_REG_STATUS);
    mouse_x = REG_READ(MOUSE_REG_X) & 0x3FFU;
    mouse_y = REG_READ(MOUSE_REG_Y) & 0x3FFU;
    mouse_buttons = REG_READ(MOUSE_REG_BUTTONS);
    mouse_commit = REG_READ(MOUSE_REG_COMMIT);
    mouse_mode = REG_READ(MOUSE_REG_MODE);
    mouse_x_min = REG_READ(MOUSE_REG_X_MIN) & 0x3FFU;
    mouse_x_max = REG_READ(MOUSE_REG_X_MAX) & 0x3FFU;
    mouse_y_min = REG_READ(MOUSE_REG_Y_MIN) & 0x3FFU;
    mouse_y_max = REG_READ(MOUSE_REG_Y_MAX) & 0x3FFU;

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

    for (uint32_t i = 0U; i < USB_HID_SLOT_COUNT; ++i) {
        usb_hid_service_dump_slot(uart_base, &g_hid_slots[i]);
    }
}

void usb_hid_service_poll(void)
{
    if (g_started == 0U) {
        return;
    }

    cherryusb_host_poll(CHERRYUSB_USB1_BUSID);
    hid_slots_poll_holds();
}
