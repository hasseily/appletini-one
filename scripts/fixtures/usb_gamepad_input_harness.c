/* Test the production bridge with USB transport, time, and register I/O stubbed. */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#define __PACKED __attribute__((packed))
#include "usb_gamepad_input.h"
#include "onee_usb_controls.h"
#include "usb_ps4_report.h"
#include "slot2_gamepad_service.h"

#define USB_HID_SLOT_COUNT ONEE_INPUT_DEVICE_SLOT_COUNT
#define HID_SOURCE_TRACK_COUNT 16U
#define HID_KEY_TRACK_COUNT 8U
#define CONFIG_USB_HID_MAX_REPORT_ITEMS 48U
#define HID_HAT_NEUTRAL 8U
#define MOUSE_MENU_EVENT_DEPTH 16U
#define MOUSE_MENU_HOLD_TICKS 1000U
#define HID_REPORT_RETRY_TICKS 10U
#define MOUSE_BUTTON_LEFT 1U
#define MOUSE_BUTTON_RIGHT 2U
#define MOUSE_BUTTON_MIDDLE 4U
#define MOUSE_BUTTON_4 8U
#define MOUSE_BUTTON_5 16U
#define UART0_BASE 0U
#define MOUSE_REG_X 0U
#define MOUSE_REG_Y 1U
#define REG_READ(address) ((void)(address), 0U)
typedef uint64_t XTime;
/* PRODUCTION_REPORT_TYPES */
struct usb_interface_descriptor { uint8_t bInterfaceSubClass; };
struct mock_altsetting { struct usb_interface_descriptor intf_desc; };
struct mock_interface { struct mock_altsetting altsetting[1]; };
struct mock_hport {
    struct { struct mock_interface intf[2]; } config;
    struct { uint16_t idVendor, idProduct; } device_desc;
};
struct usbh_hid {
    uint8_t minor, intf, protocol;
    void *user_data;
    struct mock_hport *hport;
};

/* PRODUCTION_SLOT */

static usb_hid_slot_t g_hid_slots[USB_HID_SLOT_COUNT];
static uint8_t g_reports[USB_HID_SLOT_COUNT][64];
static uint32_t g_report_count, g_transfer_error_count;
static int g_last_error;
static uint8_t g_menu_capture, g_joystick_preview, g_onee_fixed_mode;
static uint8_t g_onee_input_blocked, g_ready, g_binding_capture;
static uint32_t g_gamepad_binding_mask;
static usb_hid_menu_source_t g_screenshot_a2_source, g_screenshot_1080p_source;
static usb_hid_menu_source_t g_vtw_sources[USB_HID_VTW_SOURCE_COUNT];
static usb_hid_menu_source_t g_menu_ok_source = USB_HID_MENU_ACTION_SELECT;
static usb_hid_menu_source_t g_menu_open_close_source = USB_HID_MENU_ACTION_SELECT;
static usb_hid_menu_event_t g_menu_events[MOUSE_MENU_EVENT_DEPTH];
static uint8_t g_menu_event_rd, g_menu_event_wr, g_menu_event_count;
static int32_t g_x, g_y, g_x_residue, g_y_residue;
static XTime mock_time;
static uint8_t mock_blocked;
static uint32_t mock_report_calls, mock_mouse_writes, mock_release_keys;
static uint32_t mock_other_report_calls;
static uint32_t mock_submit_calls[USB_HID_SLOT_COUNT];
static uint32_t mock_disconnect_calls[USB_HID_SLOT_COUNT];
static onee_input_joystick_report_t mock_reports[USB_HID_SLOT_COUNT];
static onee_input_joystick_report_t mock_slot2_reports[USB_HID_SLOT_COUNT];
static uint8_t mock_slot2_hats[USB_HID_SLOT_COUNT], mock_slot2_blocked;
static uint32_t mock_slot2_report_calls, mock_slot2_release_calls;
static uint32_t mock_slot2_disconnect_calls[USB_HID_SLOT_COUNT];

/* PRODUCTION_PROTOTYPES */

static void XTime_GetTime(XTime *value) { *value = mock_time; }
static void uart_puts(uint32_t base, const char *text)
{ (void)base; (void)text; }
static void uart_putdec(uint32_t base, uint32_t value)
{ (void)base; (void)value; }
static int usbh_hid_set_idle(struct usbh_hid *hid, uint8_t id, uint8_t duration)
{ (void)hid; (void)id; (void)duration; return 0; }
static int usbh_hid_set_protocol(struct usbh_hid *hid, uint8_t protocol)
{ (void)hid; (void)protocol; return 0; }
static void hid_parse_report_descriptor(usb_hid_slot_t *slot) { (void)slot; }
static void hid_log_connected(const usb_hid_slot_t *slot) { (void)slot; }
static void hid_process_boot_mouse_report(usb_hid_slot_t *slot,
                                          const uint8_t *report, uint32_t len)
{ (void)slot; (void)report; (void)len; ++mock_other_report_calls; }
static void hid_process_boot_keyboard_report(usb_hid_slot_t *slot,
                                             const uint8_t *report, uint32_t len)
{ (void)slot; (void)report; (void)len; ++mock_other_report_calls; }
static void hid_collect_keyboard_item(const struct usbh_hid_report_item *item,
    const uint8_t *report, uint32_t len, uint8_t *modifier, uint8_t *keys,
    uint32_t *count, uint8_t *seen)
{ (void)item; (void)report; (void)len; (void)modifier; (void)keys; (void)count; (void)seen; }
static void mouse_apply_motion(usb_hid_slot_t *slot, int32_t dx,int32_t dy,uint8_t buttons)
{ (void)dx; (void)dy; (void)buttons; if (slot->mouse_card) ++mock_mouse_writes; }
static void hid_resubmit_report(usb_hid_slot_t *slot)
{
    ++mock_submit_calls[slot->index];
    slot->report_pending = 1U;
    slot->report_retry_armed = 0U;
    slot->report_retry_started = 0U;
}
static void mouse_mark_connected(usb_hid_slot_t *slot) { slot->mouse_card = 1U; }
static void mouse_release_slot(usb_hid_slot_t *slot) { slot->mouse_card = 0U; }
static void mouse_publish_state(uint8_t connected, int32_t x, int32_t y,
                                uint8_t buttons)
{ (void)connected; (void)x; (void)y; (void)buttons; ++mock_mouse_writes; }
void onee_input_service_disconnect(uint8_t slot)
{
    ++mock_disconnect_calls[slot];
    memset(&mock_reports[slot], 0, sizeof(mock_reports[slot]));
}
void onee_input_service_joystick_report(
    uint8_t slot, const onee_input_joystick_report_t *report)
{ ++mock_report_calls; mock_reports[slot] = *report; }
uint8_t onee_input_service_keyboard_report(uint8_t slot, uint8_t modifier,
    const uint8_t *keys, uint32_t count)
{ (void)slot; (void)modifier; (void)keys; (void)count; return 0U; }
void onee_input_service_set_blocked(uint8_t value) { mock_blocked = value; }
void onee_input_service_release_keyboard(void) { ++mock_release_keys; }
void slot2_gamepad_service_disconnect(uint8_t slot)
{
    ++mock_slot2_disconnect_calls[slot];
    memset(&mock_slot2_reports[slot], 0, sizeof(mock_slot2_reports[slot]));
    mock_slot2_hats[slot] = HID_HAT_NEUTRAL;
}
void slot2_gamepad_service_release_all(void)
{
    ++mock_slot2_release_calls;
    memset(mock_slot2_reports, 0, sizeof(mock_slot2_reports));
    memset(mock_slot2_hats, HID_HAT_NEUTRAL, sizeof(mock_slot2_hats));
}
void slot2_gamepad_service_report(uint8_t slot,
    const onee_input_joystick_report_t *report, uint8_t hat)
{
    ++mock_slot2_report_calls;
    mock_slot2_reports[slot] = *report;
    mock_slot2_hats[slot] = hat;
}
void slot2_gamepad_service_set_blocked(uint8_t blocked) { mock_slot2_blocked = blocked; }

/* PRODUCTION_FUNCTIONS */

#define CHECK(condition, message) do { if (!(condition)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, message); return 1; \
} } while (0)

static int take_event(usb_hid_menu_action_t action)
{
    usb_hid_menu_event_t event;
    return usb_hid_service_pop_menu_event(&event) && event.action == action;
}

static int test_hid_completion_backoff(void)
{
    struct mock_hport port = {0};
    struct usbh_hid first_hid = {0}, second_hid = {0};
    usb_hid_slot_t *first = &g_hid_slots[0];
    usb_hid_slot_t *second = &g_hid_slots[1];
    uint32_t calls;
    uint32_t releases = mock_release_keys;

    hid_slots_reset_all();
    memset(mock_submit_calls, 0, sizeof(mock_submit_calls));
    mock_other_report_calls = 0U;
    g_report_count = 0U;
    g_transfer_error_count = 0U;
    g_last_error = 0;
    port.config.intf[0].altsetting[0].intf_desc.bInterfaceSubClass =
        HID_SUBCLASS_BOOTIF;
    port.config.intf[1].altsetting[0].intf_desc.bInterfaceSubClass =
        HID_SUBCLASS_BOOTIF;
    first_hid.hport = &port;
    first_hid.protocol = HID_PROTOCOL_KEYBOARD;
    second_hid.hport = &port;
    second_hid.protocol = HID_PROTOCOL_KEYBOARD;
    second_hid.minor = 1U;
    second_hid.intf = 1U;
    usbh_hid_run(&first_hid);
    usbh_hid_run(&second_hid);
    CHECK(first_hid.user_data == first && second_hid.user_data == second,
          "two keyboard interfaces own separate report slots");

    first->keyboard_keys_down = 1U;
    mock_time = 100U;
    hid_report_complete(first, -12);
    mock_time = 103U;
    hid_report_complete(second, -12);
    CHECK(mock_submit_calls[0] == 1U && mock_submit_calls[1] == 1U &&
          !first->report_pending && !second->report_pending &&
          first->report_retry_armed && second->report_retry_armed,
          "failed completions defer both interfaces without immediate resubmit");
    CHECK(first->transfer_error_count == 1U && second->transfer_error_count == 1U &&
          g_transfer_error_count == 2U && g_last_error == -12 &&
          first->last_error == -12 && second->last_error == -12 &&
          !g_report_count && !mock_other_report_calls,
          "transport failures remain errors and never become input reports");
    CHECK(first->keyboard_keys_down && mock_release_keys == releases,
          "transient errors do not invent a keyboard release");

    mock_time = 109U;
    hid_slots_retry_reports();
    CHECK(mock_submit_calls[0] == 1U && mock_submit_calls[1] == 1U,
          "fast polls cannot retry before ten milliseconds");
    mock_time = 110U;
    hid_slots_retry_reports();
    CHECK(mock_submit_calls[0] == 2U && mock_submit_calls[1] == 1U &&
          first->report_pending && !first->report_retry_armed,
          "first interface retries once at its own deadline");
    mock_time = 113U;
    hid_slots_retry_reports();
    hid_slots_retry_reports();
    CHECK(mock_submit_calls[0] == 2U && mock_submit_calls[1] == 2U &&
          second->report_pending && !second->report_retry_armed,
          "second interface retries independently without duplicate submissions");

    mock_time = 114U;
    hid_report_complete(first, -12);
    hid_report_complete(second, 8);
    CHECK(mock_submit_calls[0] == 2U && mock_submit_calls[1] == 3U &&
          second->report_count == 1U && g_report_count == 1U &&
          mock_other_report_calls == 1U && !second->last_error &&
          !second->error_log_suppressed && !g_last_error,
          "healthy input is processed and resubmitted immediately during another failure");
    hid_report_complete(second, 0);
    CHECK(mock_submit_calls[1] == 4U && second->report_count == 1U,
          "empty successful completions retain immediate resubmission");

    for (uint32_t attempt = 0U; attempt < 100U; ++attempt) {
        calls = mock_submit_calls[0];
        mock_time = first->report_retry_started + HID_REPORT_RETRY_TICKS - 1U;
        hid_slots_retry_reports();
        CHECK(mock_submit_calls[0] == calls,
              "persistent completion errors cannot retry early");
        ++mock_time;
        hid_slots_retry_reports();
        CHECK(mock_submit_calls[0] == calls + 1U && first->report_pending &&
              first->active && mock_submit_calls[1] == 4U,
              "persistent errors retain capped automatic recovery without delaying peers");
        hid_report_complete(first, -12);
    }
    CHECK(first->transfer_error_count == 102U,
          "each persistent transport failure remains visible in counters");
    calls = mock_submit_calls[0];
    usbh_hid_stop(&first_hid);
    mock_time += HID_REPORT_RETRY_TICKS;
    hid_slots_retry_reports();
    hid_report_complete(first, -12);
    hid_report_complete(first, 8);
    CHECK(mock_submit_calls[0] == calls && !first->active &&
          !first->report_retry_armed && !first->keyboard_keys_down &&
          mock_other_report_calls == 1U,
          "disconnect cancels delayed retries and ignores late completions");
    usbh_hid_stop(&second_hid);
    puts("PASS native HID completion backoff, independent interfaces, recovery, and disconnect");
    return 0;
}

static void setup_item(struct usbh_hid_report_item *item, uint16_t page,
                       uint16_t first, uint16_t last, uint8_t id,
                       uint8_t size, uint32_t count, int32_t min, int32_t max)
{
    memset(item, 0, sizeof(*item));
    item->report_type = HID_REPORT_INPUT;
    item->report_flags = HID_MAINITEM_VARIABLE;
    item->attribute.usage_page = page;
    item->attribute.usage_min = first;
    item->attribute.usage_max = last;
    item->attribute.report_id = id;
    item->attribute.report_size = size;
    item->attribute.report_count = count;
    item->attribute.logical_min = min;
    item->attribute.logical_max = max;
}

static int test_generic_hid_gamepad(void)
{
    struct mock_hport port = {0};
    struct usbh_hid hid = {0};
    usb_hid_slot_t *slot;
    struct usbh_hid_report_item *hat, *buttons, *axes;
    uint8_t packet[3] = {1U, 0U, 0U};
    uint32_t before, paddle_before, released = mock_slot2_release_calls;

    hid_slots_reset_all();
    CHECK(mock_slot2_release_calls == released + 1U,
          "host reset releases every slot-2 player");
    usb_hid_service_set_menu_capture(0U);
    usb_hid_service_set_onee_input_blocked(0U);
    hid.hport = &port;
    usbh_hid_run(&hid);
    slot = &g_hid_slots[0];
    slot->report_info_valid = 1U;
    slot->report_info.report_item_count = 1U;
    slot->report_info.using_report_id = true;
    hat = &slot->report_info.report_items[0];
    buttons = &slot->report_info.report_items[1];
    axes = &slot->report_info.report_items[2];
    setup_item(hat, HID_USAGE_PAGE_GENERIC_DESKTOP_CONTROLS,
        HID_DESKTOP_USAGE_HATSWITCH, HID_DESKTOP_USAGE_HATSWITCH, 1U, 8U, 1U, 1, 8);
    CHECK(hid_report_info_has_absolute_joystick(slot) &&
          !hid_report_info_has_relative_mouse(slot),
          "a hat-only controller is recognized without analog axes");
    slot->interface_protocol = HID_PROTOCOL_MOUSE;
    CHECK(!hid_report_info_has_absolute_joystick(slot),
          "mouse protocol does not become a gamepad because it has a hat");
    slot->interface_protocol = 0U;
    hat->report_flags |= HID_MAINITEM_RELATIVE;
    CHECK(!hid_report_info_has_absolute_joystick(slot),
          "relative fields do not identify a gamepad");
    hat->report_flags = HID_MAINITEM_VARIABLE;
    slot->onee_joystick = hid_report_info_has_absolute_joystick(slot);

    paddle_before = mock_report_calls;
    for (uint8_t position = 1U; position <= 8U; ++position) {
        packet[1] = position;
        before = mock_slot2_report_calls;
        hid_process_report(slot, packet, 2U);
        CHECK(mock_slot2_report_calls == before + 1U &&
              mock_slot2_hats[0] == position - 1U && slot->raw_hat_active,
              "one-based eight-way hats normalize clockwise from north");
        CHECK(!mock_slot2_reports[0].axis_valid_mask &&
              !mock_slot2_reports[0].buttons_valid,
              "hat-only packets preserve omitted axes and buttons");
    }
    CHECK(mock_report_calls == paddle_before,
          "hat-only input goes to slot-2 without inventing paddle axes");
    for (uint8_t value = 0U; value <= 9U; value += 9U) {
        packet[1] = value;
        hid_process_report(slot, packet, 2U);
        CHECK(mock_slot2_hats[0] == HID_HAT_NEUTRAL && !slot->raw_hat_active,
              "one-based null/out-of-range hat values release directions");
    }
    for (uint8_t base = 0U; base <= 1U; ++base) {
        hat->attribute.logical_min = base;
        hat->attribute.logical_max = base + 3U;
        for (uint8_t position = 0U; position < 4U; ++position) {
            packet[1] = base + position;
            hid_process_report(slot, packet, 2U);
            CHECK(mock_slot2_hats[0] == position * 2U,
                  "zero/one-based four-way hats map to N/E/S/W");
        }
        packet[1] = base + 4U;
        hid_process_report(slot, packet, 2U);
        CHECK(mock_slot2_hats[0] == HID_HAT_NEUTRAL,
              "four-way null value releases the hat");
    }
    hat->attribute.logical_min = 1;
    hat->attribute.logical_max = 8;
    slot->report_info.report_item_count = 3U;
    setup_item(buttons, HID_USAGE_PAGE_BUTTON, 1U, 8U, 2U, 1U, 8U, 0, 1);
    setup_item(axes, HID_USAGE_PAGE_GENERIC_DESKTOP_CONTROLS,
        HID_DESKTOP_USAGE_X, HID_DESKTOP_USAGE_Y, 3U, 8U, 2U, 0, 255);
    packet[0] = 2U;
    for (uint8_t button = 0U; button < 8U; ++button) {
        packet[1] = (uint8_t)(1U << button);
        hid_process_report(slot, packet, 2U);
        CHECK(mock_slot2_reports[0].buttons_valid &&
              mock_slot2_reports[0].buttons == packet[1] &&
              mock_reports[0].buttons == packet[1],
              "every generic HID button including six through eight reaches both services");
        CHECK(mock_slot2_hats[0] == SLOT2_GAMEPAD_HAT_UNCHANGED &&
              !mock_slot2_reports[0].axis_valid_mask,
              "button-only report preserves the stored hat and axes");
    }
    packet[1] = 0xFFU;
    hid_process_report(slot, packet, 2U);
    CHECK(mock_slot2_reports[0].buttons == 0xFFU,
          "simultaneous generic buttons keep all eight bits");
    packet[0] = 3U; packet[1] = 64U; packet[2] = 192U;
    hid_process_report(slot, packet, sizeof(packet));
    CHECK(mock_slot2_reports[0].axis_valid_mask == 3U &&
          mock_slot2_reports[0].axis[0] == 64 && mock_slot2_reports[0].axis[1] == 192 &&
          !mock_slot2_reports[0].buttons_valid &&
          mock_slot2_hats[0] == SLOT2_GAMEPAD_HAT_UNCHANGED && slot->raw_buttons_down,
          "axis-only report preserves buttons and hat without clearing held-input state");
    packet[0] = 1U; packet[1] = 3U;
    hid_process_report(slot, packet, 2U);
    CHECK(mock_slot2_hats[0] == 2U && !mock_slot2_reports[0].buttons_valid &&
          !mock_slot2_reports[0].axis_valid_mask && slot->raw_buttons_down,
          "hat-only report in a composite gamepad preserves its other controls");
    before = mock_slot2_report_calls;
    packet[0] = 4U;
    hid_process_report(slot, packet, sizeof(packet));
    packet[0] = 1U; hid_process_report(slot, packet, 1U);
    hid_process_report(slot, packet, 0U);
    CHECK(mock_slot2_report_calls == before,
          "unknown IDs and truncated hats do not fabricate a report or release");

    /* A mouse still exposes only five buttons to the shared menu collector. */
    slot->onee_joystick = 0U;
    packet[0] = 2U; packet[1] = 0xFFU;
    {
        uint32_t value = 0U, mask = 0U;
        uint8_t seen = 0U;
        hid_collect_button_item(slot, buttons, packet, 2U, &value, &mask, &seen);
        CHECK(seen && value == 0x1FU, "mouse button limit stays at five");
    }
    slot->onee_joystick = 1U;
    before = mock_slot2_disconnect_calls[0];
    usbh_hid_stop(&hid);
    CHECK(mock_slot2_disconnect_calls[0] == before + 1U &&
          !mock_slot2_reports[0].axis_valid_mask && mock_slot2_hats[0] == HID_HAT_NEUTRAL,
          "generic HID disconnect releases the matching slot-2 controller");
    puts("PASS native generic HID hat-only detection, one-based and four/eight-way hats, all eight buttons, partial report IDs, mouse limit, disconnect");
    return 0;
}

static int test_gamepad_descriptor_and_masks(void)
{
    /* Explicit nonconsecutive axes; output/feature fields must not consume
     * input offsets, and switching report IDs must retain each input offset. */
    static const uint8_t descriptor[] = {
        0x05,1,0x09,5,0xa1,1,0x85,1,0x15,0,0x26,0xff,0,
        0x75,8,0x95,4,0x09,0x30,0x09,0x31,0x09,0x33,0x09,0x35,0x81,2,
        0x95,2,0x91,2,0xb1,2,
        0x85,2,0x05,9,0x19,1,0x29,16,0x15,0,0x25,1,0x75,1,0x95,16,0x81,2,
        0x85,3,0x19,17,0x29,32,0x81,2,
        0x85,1,0x05,2,0x09,0xc4,0x09,0xc5,0x15,0,0x26,0xff,0,
        0x75,8,0x95,2,0x81,2,0xc0
    };
    static const uint8_t array_descriptor[] = {
        0x05,1,0x09,5,0xa1,1,0x05,9,0x19,1,0x29,32,
        0x15,1,0x25,32,0x75,8,0x95,2,0x81,0,0xc0
    };
    usb_hid_slot_t *slot = &g_hid_slots[0];
    hid_slots_reset_all();
    g_menu_capture = g_binding_capture = g_joystick_preview = 0U;
    g_gamepad_binding_mask = 0U;
    slot->active = slot->onee_joystick = slot->report_info_valid = 1U;
    CHECK(appletini_usb_parse_gamepad_descriptor(descriptor, sizeof(descriptor),
        &slot->report_info) == 1, "gamepad application collection identified");
    CHECK(slot->report_info.report_item_count == 38U &&
        slot->report_info.report_items[2].attribute.usage_min == HID_DESKTOP_USAGE_RX &&
        slot->report_info.report_items[3].attribute.usage_min == HID_DESKTOP_USAGE_RZ &&
        slot->report_info.report_items[36].report_bit_offset == 32U,
        "nonconsecutive usages and independent report-type offsets retained");
    const uint8_t axes[] = {1,64,192,99,12,0,255};
    hid_process_report_protocol_report(slot, axes, sizeof(axes));
    CHECK(mock_reports[0].axis_valid_mask == 0x2FU &&
        mock_reports[0].axis[ONEE_INPUT_AXIS_RX] == 99 &&
        mock_reports[0].axis[ONEE_INPUT_AXIS_Z] == 0 &&
        mock_reports[0].axis[ONEE_INPUT_AXIS_RZ] == 255,
        "explicit axes and simulation trigger fields decode independently");
    uint8_t buttons[] = {3,0,0x80};
    hid_process_report_protocol_report(slot, buttons, sizeof(buttons));
    CHECK(slot->gamepad_buttons == UINT32_C(0x80000000) &&
        mock_reports[0].buttons_mask == UINT32_C(0xFFFF0000), "button32 kept independently");
    buttons[0]=2;buttons[1]=1;buttons[2]=0;
    hid_process_report_protocol_report(slot, buttons, sizeof(buttons));
    CHECK(slot->gamepad_buttons == UINT32_C(0x80000001), "different report ID preserves held button32");
    hid_process_report_protocol_report(slot, axes, sizeof(axes));
    CHECK(slot->gamepad_buttons == UINT32_C(0x80000001) && slot->raw_buttons_down,
        "axes-only packet cannot release any buttons");
    buttons[0]=3;buttons[1]=buttons[2]=0;
    hid_process_report_protocol_report(slot, buttons, sizeof(buttons));
    CHECK(slot->gamepad_buttons == 1U, "high button release preserves low report state");
    CHECK(appletini_usb_parse_gamepad_descriptor(array_descriptor, sizeof(array_descriptor),
        &slot->report_info) == 1, "button arrays parsed");
    const uint8_t array[] = {1,32}, released[] = {0,0};
    hid_process_report_protocol_report(slot,array,sizeof(array));
    CHECK(slot->gamepad_buttons == UINT32_C(0x80000001), "array selectors keep both low and high buttons");
    hid_process_report_protocol_report(slot,released,sizeof(released));
    CHECK(slot->gamepad_buttons == 0U, "array null selectors release the declared range");
    const uint8_t bad_short[] = {0x26,1}, bad_pop[] = {0xb4}, bad_long[] = {0xfe,4,0,1};
    CHECK(appletini_usb_parse_gamepad_descriptor(bad_short,sizeof(bad_short),&slot->report_info)<0 &&
        appletini_usb_parse_gamepad_descriptor(bad_pop,sizeof(bad_pop),&slot->report_info)<0 &&
        appletini_usb_parse_gamepad_descriptor(bad_long,sizeof(bad_long),&slot->report_info)<0,
        "truncation and global stack underflow rejected");
    puts("PASS descriptor usages, input/output/feature offsets, trigger fields, all32 buttons, partial IDs, arrays and malformed bounds");
    return 0;
}

static int test_gamepad_shortcuts_and_capture(void)
{
    onee_input_joystick_report_t report = {0};
    usb_hid_menu_event_t event;
    hid_slots_reset_all();
    g_menu_event_rd=g_menu_event_wr=g_menu_event_count=0;
    g_menu_capture=g_binding_capture=g_joystick_preview=0;
    g_gamepad_binding_mask=0;
    g_screenshot_a2_source=g_screenshot_1080p_source=0;
    memset(g_vtw_sources,0,sizeof(g_vtw_sources));
    g_menu_ok_source=g_menu_open_close_source=USB_HID_MENU_ACTION_SELECT;
    CHECK(usb_gamepad_input_connect()==0,"shortcut controller allocated");
    report.buttons_valid=1;
    report.buttons=0x10U;usb_gamepad_input_report(0,&report,8,0);
    CHECK(!g_menu_event_count,"shoulders never navigate during gameplay");
    report.buttons=0;usb_gamepad_input_report(0,&report,8,0);
    usb_hid_service_set_menu_capture(1);
    report.buttons=0x10U;usb_gamepad_input_report(0,&report,8,0);
    CHECK(take_event(USB_HID_MENU_ACTION_PREV_TAB) && !g_menu_event_count,"LB navigates once");
    usb_gamepad_input_report(0,&report,8,0);CHECK(!g_menu_event_count,"held LB does not repeat");
    report.buttons=0x20U;usb_gamepad_input_report(0,&report,8,0);
    CHECK(take_event(USB_HID_MENU_ACTION_NEXT_TAB) && !g_menu_event_count,"direct LB to RB changes direction");
    report.buttons=0x30U;usb_gamepad_input_report(0,&report,8,0);
    CHECK(!g_menu_event_count,"both shoulders cancel");
    report.buttons=0x10U;usb_gamepad_input_report(0,&report,8,0);
    CHECK(take_event(USB_HID_MENU_ACTION_PREV_TAB),"release one shoulder resumes the held direction");
    report.buttons=0;usb_gamepad_input_report(0,&report,8,0);
    usb_hid_service_set_binding_capture(1);
    for(uint8_t button=0;button<32;++button) {
        report.buttons=UINT32_C(1)<<button;usb_gamepad_input_report(0,&report,8,0);
        CHECK(usb_hid_service_pop_menu_event(&event) && event.action==USB_HID_MENU_ACTION_NONE &&
            event.source==usb_hid_menu_source_from_gamepad_button(button) && !g_menu_event_count,
            "learning keeps every button source separate and suppresses shortcuts");
        report.buttons=0;usb_gamepad_input_report(0,&report,8,0);
    }
    usb_hid_service_set_binding_capture(0);
    usb_hid_service_set_gamepad_binding_mask(UINT32_C(1)<<4);
    report.buttons=0x10U;usb_gamepad_input_report(0,&report,8,0);
    CHECK(usb_hid_service_pop_menu_event(&event) && event.action==USB_HID_MENU_ACTION_NONE &&
        event.source==USB_HID_MENU_SOURCE_GAMEPAD_BASE+4 && !g_menu_event_count,
        "explicit LB binding replaces its default tab shortcut");
    report.buttons=0;usb_gamepad_input_report(0,&report,8,0);
    usb_hid_service_set_gamepad_binding_mask(UINT32_C(1)<<31);
    usb_hid_service_set_screenshot_sources(USB_HID_MENU_SOURCE_GAMEPAD_BASE+31,0);
    usb_hid_service_set_menu_capture(0);
    report.buttons=UINT32_C(1)<<31;usb_gamepad_input_report(0,&report,8,0);
    CHECK(take_event(USB_HID_MENU_ACTION_SCREENSHOT_A2),"button32 global screenshot binding fires outside menu");
    usb_gamepad_input_report(0,&report,8,0);CHECK(!g_menu_event_count,"held screenshot button does not repeat");
    report.buttons=0;usb_gamepad_input_report(0,&report,8,0);
    usb_hid_service_set_screenshot_sources(0,0);
    usb_hid_menu_source_t speed[USB_HID_VTW_SOURCE_COUNT]={USB_HID_MENU_SOURCE_GAMEPAD_BASE+31,0,0,0};
    usb_hid_service_set_vtw_sources(speed);
    report.buttons=UINT32_C(1)<<31;usb_gamepad_input_report(0,&report,8,0);
    CHECK(take_event(USB_HID_MENU_ACTION_VTW_SPEED_TOGGLE),"button32 global speed binding fires");
    report.buttons=0;usb_gamepad_input_report(0,&report,8,0);
    memset(speed,0,sizeof(speed));usb_hid_service_set_vtw_sources(speed);
    usb_hid_service_set_menu_ok_source(USB_HID_MENU_SOURCE_GAMEPAD_BASE+31);
    usb_hid_service_set_menu_open_close_source(USB_HID_MENU_SOURCE_GAMEPAD_BASE+31);
    report.buttons=UINT32_C(1)<<31;usb_gamepad_input_report(0,&report,8,0);
    mock_time+=MOUSE_MENU_HOLD_TICKS;hid_slots_poll_holds();
    CHECK(take_event(USB_HID_MENU_ACTION_OPEN),"high button holds open menu");
    report.buttons=0;usb_gamepad_input_report(0,&report,8,0);
    usb_hid_service_set_menu_capture(1);
    report.buttons=UINT32_C(1)<<31;usb_gamepad_input_report(0,&report,8,0);
    report.buttons=0;usb_gamepad_input_report(0,&report,8,0);
    CHECK(usb_hid_service_pop_menu_event(&event) && event.source==USB_HID_MENU_SOURCE_GAMEPAD_BASE+31,
        "high button short OK emits its own source on release");
    CHECK(!g_menu_event_count,"no legacy event duplicates a high button binding");
    puts("PASS shoulder direction/cancellation/gameplay,32-source learning, explicit override, screenshot/speed and high-button holds");
    return 0;
}

static int test_input_kind_state_boundaries(void)
{
    onee_input_joystick_report_t report = {0};
    usb_hid_menu_event_t event;
    const uint8_t no_keys[] = {0U};
    const uint8_t held_key[] = {HID_KBD_USAGE_A};
    usb_hid_slot_t *slot = &g_hid_slots[0];

    hid_slots_reset_all();
    g_menu_event_rd = g_menu_event_wr = g_menu_event_count = 0U;
    g_menu_capture = 1U;
    g_binding_capture = g_joystick_preview = 0U;
    g_gamepad_binding_mask = 0U;
    g_screenshot_a2_source = g_screenshot_1080p_source = 0U;
    memset(g_vtw_sources, 0, sizeof(g_vtw_sources));
    g_menu_ok_source = g_menu_open_close_source = USB_HID_MENU_ACTION_SELECT;
    slot->active = 1U;

    mouse_menu_process_buttons(slot, MOUSE_BUTTON_LEFT, 0);
    CHECK(take_event(USB_HID_MENU_ACTION_LEFT), "initial physical mouse edge");
    usb_hid_service_set_binding_capture(1U);
    mouse_menu_process_buttons(slot, MOUSE_BUTTON_LEFT, 0);
    CHECK(!g_menu_event_count, "capture entry cannot retrigger a held physical mouse");
    usb_hid_service_set_binding_capture(0U);
    mouse_menu_process_buttons(slot, MOUSE_BUTTON_LEFT, 0);
    CHECK(!g_menu_event_count, "capture exit cannot retrigger a held physical mouse");
    mouse_menu_process_buttons(slot, 0U, 0);
    mouse_menu_process_buttons(slot, MOUSE_BUTTON_MIDDLE, 0);
    CHECK(slot->ok_down && slot->open_close_down, "mouse hold begins");
    usb_hid_service_set_binding_capture(1U);
    usb_hid_service_set_binding_capture(0U);
    CHECK(slot->ok_down && slot->open_close_down, "pad learning preserves mouse holds");
    mouse_menu_process_buttons(slot, 0U, 0);
    CHECK(take_event(USB_HID_MENU_ACTION_SELECT), "preserved mouse hold releases once");

    g_menu_ok_source = g_menu_open_close_source =
        usb_hid_menu_source_from_keyboard_usage(HID_KBD_USAGE_A);
    hid_process_keyboard_usages(slot, 0U, held_key, sizeof(held_key));
    usb_hid_service_set_binding_capture(1U);
    usb_hid_service_set_binding_capture(0U);
    CHECK(slot->ok_down && slot->open_close_down, "pad learning preserves keyboard holds");
    hid_process_keyboard_usages(slot, 0U, no_keys, sizeof(no_keys));
    CHECK(!slot->ok_down && !slot->open_close_down &&
        usb_hid_service_pop_menu_event(&event) && event.source == g_menu_ok_source,
        "real keyboard release still finishes its own hold");
    CHECK(!g_menu_event_count, "keyboard release emits one event");

    slot->onee_joystick = 1U;
    g_menu_capture = 0U;
    g_menu_ok_source = g_menu_open_close_source = USB_HID_MENU_SOURCE_GAMEPAD_BASE + 31U;
    g_gamepad_binding_mask = UINT32_C(1) << 31U;
    report.buttons_valid = 1U;
    report.buttons = UINT32_C(1) << 31U;
    hid_process_gamepad_report(slot, &report, HID_HAT_NEUTRAL, 0U);
    hid_process_keyboard_usages(slot, 0U, no_keys, sizeof(no_keys));
    CHECK(slot->open_close_down, "composite keyboard packet preserves gamepad open hold");
    mock_time += MOUSE_MENU_HOLD_TICKS;
    hid_slots_poll_holds();
    CHECK(take_event(USB_HID_MENU_ACTION_OPEN), "preserved composite gamepad hold opens menu");
    report.buttons = 0U;
    hid_process_gamepad_report(slot, &report, HID_HAT_NEUTRAL, 0U);
    g_menu_capture = 1U;
    report.buttons = UINT32_C(1) << 31U;
    hid_process_gamepad_report(slot, &report, HID_HAT_NEUTRAL, 0U);
    hid_process_keyboard_usages(slot, 0U, no_keys, sizeof(no_keys));
    CHECK(slot->ok_down && slot->open_close_down && !g_menu_event_count,
        "composite keyboard packet cannot finish a held gamepad OK");
    report.buttons = 0U;
    hid_process_gamepad_report(slot, &report, HID_HAT_NEUTRAL, 0U);
    CHECK(!slot->ok_down && !slot->open_close_down &&
        usb_hid_service_pop_menu_event(&event) && event.source == g_menu_ok_source &&
        !g_menu_event_count, "gamepad release finishes its own composite hold once");
    puts("PASS mouse/keyboard state across pad learning and composite gamepad hold isolation");
    return 0;
}

int main(void)
{
    struct mock_hport port = {0};
    struct usbh_hid first_hid = {0}, second_hid = {0};
    onee_input_joystick_report_t report = {0};
    uint8_t ps4_report[64] = {1U, 128U, 128U, 128U, 128U, 8U};
    int gamepad;
    uint32_t calls;

    hid_slots_reset_all();
    gamepad = usb_gamepad_input_connect();
    CHECK(gamepad == 0, "first Xbox takes slot zero");
    report.axis_valid_mask = 0x3FU;
    report.buttons_valid = 1U;
    for (uint32_t axis = 0U; axis < ONEE_INPUT_AXIS_COUNT; ++axis) {
        report.logical_min[axis] = -32768;
        report.logical_max[axis] = 32767;
    }
    usb_gamepad_input_report((uint8_t)gamepad, &report, 8U, 0U);
    CHECK(usb_hid_service_all_input_released(), "rest report releases input");

    port.config.intf[0].altsetting[0].intf_desc.bInterfaceSubClass =
        HID_SUBCLASS_BOOTIF;
    first_hid.hport = &port;
    first_hid.protocol = HID_PROTOCOL_KEYBOARD;
    usbh_hid_run(&first_hid);
    CHECK(first_hid.user_data == &g_hid_slots[1], "HID minor zero moves to free slot");
    CHECK(g_hid_slots[0].vendor_gamepad && mock_reports[0].axis_valid_mask == 0x3FU,
          "HID connect preserves Xbox input");
    calls = mock_disconnect_calls[0];
    usbh_hid_run(&first_hid);
    CHECK(mock_submit_calls[1] == 1U, "duplicate HID run cannot resubmit or steal slot");
    first_hid.user_data = &g_hid_slots[0];
    usbh_hid_stop(&first_hid);
    CHECK(!g_hid_slots[1].active && g_hid_slots[0].active &&
          mock_disconnect_calls[0] == calls, "HID disconnect finds owner by HID pointer");
    usbh_hid_stop(&first_hid);
    CHECK(mock_disconnect_calls[0] == calls, "duplicate HID stop leaves Xbox intact");

    second_hid.hport = &port;
    second_hid.minor = 5U;
    second_hid.protocol = HID_PROTOCOL_KEYBOARD;
    usbh_hid_run(&second_hid);
    CHECK(second_hid.user_data == &g_hid_slots[5], "free HID minor stays preferred");
    usb_gamepad_input_disconnect(5U);
    CHECK(g_hid_slots[5].active, "vendor disconnect cannot release a HID slot");
    calls = mock_report_calls;
    usb_gamepad_input_report(5U, &report, 0U, 1U);
    usb_gamepad_input_report(255U, &report, 0U, 1U);
    usb_gamepad_input_report(0U, NULL, 0U, 1U);
    CHECK(mock_report_calls == calls, "invalid or HID targets reject vendor reports");

    g_hid_slots[0].report_retry_armed = 1U;
    g_hid_slots[5].report_retry_armed = 1U;
    g_hid_slots[5].report_pending = 0U;
    mock_time = 10U;
    hid_slots_retry_reports();
    CHECK(mock_submit_calls[0] == 0U && mock_submit_calls[5] == 2U,
          "HID retry ignores vendor slots without a HID pointer");

    report.buttons = 0x80U;
    usb_gamepad_input_report(0U, &report, 8U, 0U);
    CHECK(!usb_hid_service_all_input_released(), "Menu button participates in release guard");
    CHECK(mock_reports[0].buttons == 0x80U, "all eight buttons reach the input service");
    CHECK(mock_slot2_reports[0].buttons == 0x80U && mock_slot2_hats[0] == 8U,
          "all vendor buttons and the hat reach the slot-2 card service");
    report.buttons = 0xFFU;
    usb_gamepad_input_report(0U, &report, 8U, 0U);
    CHECK(mock_slot2_reports[0].buttons == 0xFFU,
          "simultaneous vendor buttons preserve all eight bits");
    report.buttons = 0U;
    usb_gamepad_input_report(0U, &report, 8U, 1U);
    CHECK(!usb_hid_service_all_input_released(), "Guide or stick click holds release guard");
    usb_gamepad_input_report(0U, &report, 0U, 0U);
    CHECK(!usb_hid_service_all_input_released(), "D-pad holds release guard");
    CHECK(mock_slot2_hats[0] == 0U, "vendor D-pad reaches slot-2 independently of axes");
    report.axis[ONEE_INPUT_AXIS_RX] = 32767;
    usb_gamepad_input_report(0U, &report, 8U, 0U);
    CHECK(!usb_hid_service_all_input_released(), "right stick holds release guard");
    report.axis[ONEE_INPUT_AXIS_RX] = 0;
    usb_gamepad_input_report(0U, &report, 8U, 0U);
    CHECK(usb_hid_service_all_input_released(), "all physical releases clear guard");

    usb_hid_service_set_menu_capture(1U);
    CHECK(mock_blocked && mock_slot2_blocked, "menu capture blocks both guest input paths");
    report.axis[ONEE_INPUT_AXIS_X] = 32767;
    usb_gamepad_input_report(0U, &report, 0U, 0U);
    CHECK(take_event(USB_HID_MENU_ACTION_RIGHT) &&
          take_event(USB_HID_MENU_ACTION_ITEM_UP) && !g_menu_event_count,
          "left stick and D-pad navigate the menu");
    usb_gamepad_input_report(0U, &report, 0U, 0U);
    CHECK(!g_menu_event_count, "unchanged axis and hat do not repeat menu edges");
    usb_hid_service_set_joystick_preview(1U);
    report.axis[ONEE_INPUT_AXIS_X] = -32768;
    usb_gamepad_input_report(0U, &report, 4U, 0U);
    CHECK(!g_menu_event_count && mock_reports[0].axis[ONEE_INPUT_AXIS_X] == -32768,
          "preview keeps raw report current while suppressing axis and hat navigation");
    CHECK(mock_blocked && !usb_hid_service_all_input_released(),
          "preview retains guest block and release guard");
    report.buttons = 1U;
    usb_gamepad_input_report(0U, &report, 4U, 0U);
    CHECK(take_event(USB_HID_MENU_ACTION_LEFT), "preview keeps button navigation");
    usb_hid_service_set_joystick_preview(0U);
    report.buttons = 0U;
    report.axis[ONEE_INPUT_AXIS_X] = 0;
    usb_gamepad_input_report(0U, &report, 8U, 0U);

    usb_hid_service_set_menu_ok_source(USB_HID_MENU_ACTION_SELECT);
    usb_hid_service_set_menu_open_close_source(USB_HID_MENU_ACTION_SELECT);
    report.buttons = 4U;
    usb_gamepad_input_report(0U, &report, 8U, 0U);
    CHECK(!g_menu_event_count, "OK waits for release or hold");
    report.buttons = 0U;
    usb_gamepad_input_report(0U, &report, 8U, 0U);
    CHECK(take_event(USB_HID_MENU_ACTION_SELECT), "short OK press selects on release");
    report.buttons = 4U;
    usb_gamepad_input_report(0U, &report, 8U, 0U);
    mock_time += MOUSE_MENU_HOLD_TICKS;
    hid_slots_poll_holds();
    CHECK(take_event(USB_HID_MENU_ACTION_CLOSE), "held Xbox button closes menu without new report");
    hid_slots_poll_holds();
    report.buttons = 0U;
    usb_gamepad_input_report(0U, &report, 8U, 0U);
    CHECK(!g_menu_event_count, "long hold fires once and suppresses trailing select");

    usb_hid_service_set_onee_input_blocked(1U);
    usb_hid_service_set_menu_capture(0U);
    CHECK(mock_blocked && mock_slot2_blocked, "release wait stays blocked after menu capture ends");
    calls = mock_report_calls;
    usb_gamepad_input_report(0U, &report, 8U, 0U);
    CHECK(mock_report_calls == calls + 1U, "blocked input still updates saved joystick state");
    usb_hid_service_set_onee_input_blocked(0U);
    CHECK(!mock_blocked && !mock_slot2_blocked, "both paths unblock after menu and release wait end");
    usb_hid_service_set_onee_fixed_mode(1U);
    CHECK(mock_release_keys == 1U, "routing transition releases stale keyboard state");
    report.buttons = 4U;
    usb_gamepad_input_report(0U, &report, 8U, 0U);
    mock_time += MOUSE_MENU_HOLD_TICKS;
    hid_slots_poll_holds();
    CHECK(take_event(USB_HID_MENU_ACTION_OPEN), "Xbox hold opens menu in ONEe fixed mode");
    CHECK(mock_mouse_writes == 0U, "vendor reports never write MouseCard registers");

    usb_gamepad_input_disconnect(0U);
    CHECK(!g_hid_slots[0].active && !mock_reports[0].axis_valid_mask &&
          !mock_slot2_reports[0].axis_valid_mask && mock_slot2_hats[0] == HID_HAT_NEUTRAL &&
          usb_hid_service_all_input_released(), "disconnect releases report and held input");
    CHECK(g_hid_slots[5].active, "Xbox disconnect leaves HID intact");
    usbh_hid_stop(&second_hid);
    for (uint32_t index = 0U; index < USB_HID_SLOT_COUNT; ++index) {
        CHECK(usb_gamepad_input_connect() == (int)index, "allocation uses free slots in order");
    }
    CHECK(usb_gamepad_input_connect() == -1, "ninth device fails without replacing input");
    first_hid.user_data = NULL;
    usbh_hid_run(&first_hid);
    CHECK(first_hid.user_data == NULL && g_hid_slots[0].vendor_gamepad,
          "HID connection with a full table cannot overwrite Xbox");
    usb_gamepad_input_disconnect(1U);
    port.config.intf[0].altsetting[0].intf_desc.bInterfaceSubClass = 0U;
    port.device_desc.idVendor = 0x054CU;
    port.device_desc.idProduct = 0x09CCU;
    first_hid.protocol = 0U;
    usbh_hid_run(&first_hid);
    CHECK(first_hid.user_data == &g_hid_slots[1] && g_hid_slots[1].ps4 &&
          g_hid_slots[1].onee_joystick && !g_hid_slots[1].vendor_gamepad,
          "DS4 receives a shared HID joystick slot alongside Xbox");
    hid_process_report(&g_hid_slots[1], ps4_report, sizeof(ps4_report));
    CHECK(mock_reports[1].axis_valid_mask == 0x3FU &&
          mock_slot2_reports[1].axis_valid_mask == 0x3FU && mock_slot2_hats[1] == 8U &&
          usb_hid_service_all_input_released(), "DS4 rest report reaches shared input path");
    usb_hid_service_set_menu_capture(1U);
    ps4_report[5] = 0x28U;
    hid_process_report(&g_hid_slots[1], ps4_report, sizeof(ps4_report));
    CHECK(mock_reports[1].buttons == 1U && !usb_hid_service_all_input_released() &&
          mock_slot2_reports[1].buttons == 1U &&
          take_event(USB_HID_MENU_ACTION_LEFT),
          "DS4 Cross follows normalized gamepad button and menu routing");
    calls = mock_report_calls;
    hid_process_report(&g_hid_slots[1], ps4_report, 4U);
    CHECK(mock_report_calls == calls && mock_other_report_calls == 0U,
          "truncated DS4 reports cannot fall through to generic HID or boot paths");
    usbh_hid_stop(&first_hid);
    CHECK(!g_hid_slots[1].active && g_hid_slots[0].vendor_gamepad &&
          usb_hid_service_all_input_released(),
          "DS4 disconnect releases only its shared slot");
    puts("PASS native USB gamepad slot ownership, menu, preview, hold, and release guards");
    CHECK(test_hid_completion_backoff() == 0, "HID failed-completion retry coverage");
    CHECK(test_generic_hid_gamepad() == 0, "generic HID gamepad bridge coverage");
    CHECK(test_gamepad_descriptor_and_masks() == 0, "bounded descriptor parsing and partial masks");
    CHECK(test_gamepad_shortcuts_and_capture() == 0, "32button binding and shoulder coverage");
    CHECK(test_input_kind_state_boundaries() == 0, "input-kind hold and capture boundaries");
    return 0;
}
