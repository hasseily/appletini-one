/* Test the production bridge with USB transport, time, and register I/O stubbed. */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#define __PACKED __attribute__((packed))
#include "usb_def.h"
#include "usb_errno.h"
#include "usb_gamepad_input.h"
#include "onee_usb_controls.h"
#include "usb_keyboard_ascii.h"
#include "usb_ps4_report.h"
#include "slot2_gamepad_service.h"

#define USB_HID_SLOT_COUNT ONEE_INPUT_DEVICE_SLOT_COUNT
#define HID_SOURCE_TRACK_COUNT 72U
#define HID_KEY_TRACK_COUNT 64U
#define CONFIG_USB_HID_MAX_REPORT_ITEMS 48U
#define HID_HAT_NEUTRAL 8U
#define HID_REPORT_DESC_BYTES 512U
#define HID_RELATIVE_PULSE_TICKS 20U
#define MOUSE_MENU_EVENT_DEPTH 16U
#define MOUSE_MENU_HOLD_TICKS 1000U
#define HID_REPORT_RETRY_TICKS 10U
#define MOUSE_BUTTON_LEFT 1U
#define MOUSE_BUTTON_RIGHT 2U
#define MOUSE_BUTTON_MIDDLE 4U
#define MOUSE_BUTTON_4 8U
#define MOUSE_BUTTON_5 16U
#define MOUSE_BOOT_WHEEL_INDEX 3U
#define UART0_BASE 0U
#define MOUSE_REG_X 0U
#define MOUSE_REG_Y 1U
#define REG_READ(address) ((void)(address), 0U)
typedef uint64_t XTime;
/* PRODUCTION_REPORT_TYPES */
struct mock_altsetting { struct usb_interface_descriptor intf_desc; };
struct mock_interface { struct mock_altsetting altsetting[1]; };
struct usbh_urb {
    int errorcode;
    void *hcpriv;
    uint32_t actual_length, timeout;
    struct usb_setup_packet *setup;
    uint8_t *transfer_buffer;
};
struct mock_hub { struct usbh_hubport *parent; };
struct usbh_hubport {
    struct { struct mock_interface intf[8]; } config;
    struct { uint16_t idVendor, idProduct; uint8_t iProduct, iSerialNumber; } device_desc;
    uint8_t connected, port;
    struct mock_hub *parent;
    struct usb_setup_packet *setup;
    struct usbh_urb ep0_urb;
    void *mutex;
    char *iProduct;
};
#define mock_hport usbh_hubport
struct usbh_hid {
    uint8_t minor, intf, protocol;
    void *user_data;
    struct mock_hport *hport;
    uint16_t report_size;
};

/* PRODUCTION_SLOT */

static usb_hid_slot_t g_hid_slots[USB_HID_SLOT_COUNT];
static uint8_t g_reports[USB_HID_SLOT_COUNT][64];
static uint8_t g_report_descs[USB_HID_SLOT_COUNT][HID_REPORT_DESC_BYTES];
static uint32_t g_report_count, g_transfer_error_count;
static int g_last_error;
static uint8_t g_menu_capture, g_joystick_preview, g_onee_fixed_mode;
static uint8_t g_menu_text_capture;
static uint8_t g_onee_standby_mode;
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
static const uint8_t *mock_descriptor;
static uint32_t mock_descriptor_size;
static int mock_descriptor_result;
static unsigned mock_protocol_calls[2];
static int mock_protocol_result[2];
static uint8_t mock_keys[256], mock_modifiers;
static uint32_t mock_keyboard_calls, mock_key_count;
static int32_t mock_mouse_dx, mock_mouse_dy;
static uint8_t mock_mouse_buttons;
static uint32_t mock_mouse_calls;
static uint8_t g_controller_string[256], g_controller_string_busy;
static unsigned mock_string_calls, mock_mutex_take, mock_mutex_give;
static uint32_t mock_string_timeout;
static uint16_t mock_string_language;
static unsigned mock_string_bad;
static int mock_string_error;

/* PRODUCTION_PROTOTYPES */

static void XTime_GetTime(XTime *value) { *value = mock_time; }
static void uart_puts(uint32_t base, const char *text)
{ (void)base; (void)text; }
static void uart_putdec(uint32_t base, uint32_t value)
{ (void)base; (void)value; }
static int usbh_hid_set_idle(struct usbh_hid *hid, uint8_t id, uint8_t duration)
{ (void)hid; (void)id; (void)duration; return 0; }
static int usbh_hid_set_protocol(struct usbh_hid *hid, uint8_t protocol)
{ (void)hid; ++mock_protocol_calls[protocol]; return mock_protocol_result[protocol]; }
static int usbh_hid_get_report_descriptor(struct usbh_hid *hid, uint8_t *data, uint32_t length)
{
    (void)hid;
    if (mock_descriptor_result != 0) return mock_descriptor_result;
    if (mock_descriptor_size < length) length = mock_descriptor_size;
    if (mock_descriptor != NULL) memcpy(data, mock_descriptor, length);
    return (int)length;
}
static void usb_osal_mutex_take(void *mutex) { (void)mutex; ++mock_mutex_take; }
static void usb_osal_mutex_give(void *mutex) { (void)mutex; ++mock_mutex_give; }
static void usbh_control_urb_fill(struct usbh_urb *urb, struct usbh_hubport *port,
    struct usb_setup_packet *setup, uint8_t *buffer, uint32_t length,
    uint32_t timeout, void *complete, void *argument)
{
    (void)port; (void)length; (void)complete; (void)argument;
    urb->setup = setup; urb->transfer_buffer = buffer; urb->timeout = timeout;
}
static int usbh_submit_urb(struct usbh_urb *urb)
{
    const uint8_t index = (uint8_t)urb->setup->wValue;
    uint8_t *data = urb->transfer_buffer;
    ++mock_string_calls; mock_string_timeout = urb->timeout;
    if (mock_string_error) {
        urb->errorcode = mock_string_error == -USB_ERR_NOMEM ? -USB_ERR_BUSY : mock_string_error;
        return mock_string_error;
    }
    data[1] = USB_DESCRIPTOR_TYPE_STRING;
    if (index == 0U) { data[0] = 4U; data[2] = 0x11U; data[3] = 0x04U; }
    else {
        const char *text = index == 2U ? "Test Controller" : "Serial 1234";
        unsigned count = (unsigned)strlen(text);
        data[0] = (uint8_t)(2U + count * 2U);
        for (unsigned i = 0U; i < count; ++i) { data[2U + 2U*i] = (uint8_t)text[i]; data[3U + 2U*i] = 0U; }
        mock_string_language = urb->setup->wIndex;
    }
    urb->actual_length = data[0];
    if (mock_string_bad == 1U) data[0] |= 1U;
    if (mock_string_bad == 2U) data[1] = USB_DESCRIPTOR_TYPE_DEVICE;
    if (mock_string_bad == 3U) urb->actual_length = 2U;
    return 0;
}
static void hid_log_connected(const usb_hid_slot_t *slot) { (void)slot; }
static void hid_process_boot_keyboard_report(usb_hid_slot_t *slot,
                                             const uint8_t *report, uint32_t len)
{ (void)slot; (void)report; (void)len; ++mock_other_report_calls; }
static void mouse_apply_motion(usb_hid_slot_t *slot, int32_t dx,int32_t dy,uint8_t buttons)
{ ++mock_mouse_calls; mock_mouse_dx = dx; mock_mouse_dy = dy; mock_mouse_buttons = buttons; if (slot->mouse_card) ++mock_mouse_writes; }
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
{
    (void)slot; ++mock_keyboard_calls; mock_modifiers = modifier;
    mock_key_count = count; memset(mock_keys, 0, sizeof(mock_keys));
    for (uint32_t i = 0U; i < count; ++i) mock_keys[keys[i]] = 1U;
    return 0U;
}
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

#include "usb_hid_live_descriptors.h"
static int take_event(usb_hid_menu_action_t action);

static usb_hid_slot_t *connect_descriptor(const uint8_t *descriptor, uint32_t length,
    uint8_t subclass, uint8_t protocol)
{
    static struct mock_hport port;
    static struct usbh_hid hid;
    static struct usb_setup_packet setup;
    hid_slots_reset_all();
    memset(&port, 0, sizeof(port));
    memset(&hid, 0, sizeof(hid));
    g_menu_capture = g_binding_capture = g_joystick_preview = 0U;
    g_onee_fixed_mode = g_onee_input_blocked = 0U;
    g_gamepad_binding_mask = 0U;
    mock_descriptor = descriptor; mock_descriptor_size = length;
    mock_descriptor_result = 0;
    memset(mock_protocol_calls, 0, sizeof(mock_protocol_calls));
    memset(mock_protocol_result, 0, sizeof(mock_protocol_result));
    hid.hport = &port; hid.protocol = protocol; hid.report_size = (uint16_t)length;
    port.connected = port.port = 1U; port.setup = &setup;
    port.device_desc.idVendor = 0x045eU; port.device_desc.idProduct = 0x028eU;
    port.config.intf[0].altsetting[0].intf_desc.bInterfaceSubClass = subclass;
    usbh_hid_run(&hid);
    return &g_hid_slots[0];
}

static int test_captured_descriptors(void)
{
    struct usbh_hid_report_info info;
    appletini_usb_hid_descriptor_meta_t meta;
    for (unsigned i = 0U; i < sizeof(hid_live_fixtures) / sizeof(hid_live_fixtures[0]); ++i) {
        const usb_hid_live_fixture_t *fixture = &hid_live_fixtures[i];
        CHECK(appletini_usb_parse_input_descriptor(fixture->descriptor, fixture->length, &info, &meta) == 0,
            "captured report descriptor parses");
        CHECK(info.report_item_count == fixture->input_items && meta.keyboard_item_count == fixture->keyboard_items,
            "captured descriptors preserve bindable fields and compact key chunks");
        usb_hid_slot_t *slot = connect_descriptor(fixture->descriptor, fixture->length,
            fixture->subclass, fixture->protocol);
        if (i == 0U) CHECK(slot->boot_keyboard && !slot->onee_joystick, "pure captured keyboard retains boot protocol");
        if (i == 2U) CHECK(slot->onee_joystick && !slot->generic_axes && !slot->mouse_capable,
            "button-only gamepad interface retains controller identity");
        if (i == 3U) {
            const uint8_t packet[] = {0U,255U,0U,0U,0U,0x10U,0U,0U};
            hid_process_report(slot, packet, sizeof(packet));
            CHECK(mock_reports[0].axis_valid_mask == 3U && mock_reports[0].axis[0] == 0 &&
                mock_reports[0].axis[1] == 255 && mock_reports[0].buttons == 1U,
                "SNES-style captured absolute XY Dpad and packed button decode");
        }
        if (i == 5U) {
            CHECK(meta.application_count == 4U && slot->generic_axes && slot->mouse_capable,
                "captured composite keyboard/mouse keeps four application owners");
            const uint8_t keys1[] = {1U,0U,0U,4U,0U,0U,0U,0U,0U};
            const uint8_t keys4[] = {4U,0U,0U,5U,0U,0U,0U,0U,0U,0U,0U,0U};
            const uint8_t mouse[] = {2U,0U,0U,0U,1U};
            hid_process_report(slot, keys1, sizeof(keys1));
            CHECK(mock_keys[4U], "first keyboard report ID presses A");
            hid_process_report(slot, keys4, sizeof(keys4));
            CHECK(mock_keys[4U] && mock_keys[5U], "second key ID preserves first ID keys");
            unsigned keyboard_before = mock_keyboard_calls;
            hid_process_report(slot, mouse, sizeof(mouse));
            CHECK(mock_keys[4U] && mock_keys[5U] && mock_keyboard_calls == keyboard_before,
                "captured wheel ID cannot release held keyboard reports");
        }
        if (i == 6U) {
            CHECK(info.report_items[5].attribute.report_size == 12U && info.report_items[6].report_bit_offset == 20U,
                "captured packed 12-bit mouse offsets remain exact");
            const uint8_t packet[] = {17U,0x34U,0xF2U,0xFFU,1U};
            hid_process_report(slot, packet, sizeof(packet));
            CHECK(mock_mouse_dx == 564 && mock_mouse_dy == -1 && mock_mouse_buttons == 17U,
                "packed 12-bit native mouse keeps signs and five buttons");
            CHECK(mock_reports[0].axis[ONEE_INPUT_AXIS_X] == 32767 &&
                mock_reports[0].axis[ONEE_INPUT_AXIS_Y] == -256 &&
                mock_reports[0].axis[ONEE_INPUT_AXIS_WHEEL] == 4096,
                "captured mouse feeds scaled relative controller pulses");
        }
    }
    puts("PASS seven captured descriptors, composite held keys, SNES XY and packed 12-bit mouse runtime");
    return 0;
}

static int test_keyboard_chunks_and_atomicity(void)
{
    /* A 200-key bitmap uses one compact chunk, with separate ID2 array state.
     * The first report also owns axes and a trailing constant byte. */
    static const uint8_t descriptor[] = {
        0x05,1,0x09,6,0xa1,1,0x85,1,0x05,7,0x19,4,0x29,203,
        0x15,0,0x25,1,0x75,1,0x96,200,0,0x81,2,
        0x85,2,0x19,0,0x29,255,0x15,0,0x26,255,0,0x75,8,0x95,2,0x81,0,0xc0,
        0x05,1,0x09,5,0xa1,1,0x85,1,0x05,9,0x19,1,0x29,8,
        0x15,0,0x25,1,0x75,1,0x95,8,0x81,2,
        0x05,1,0x09,0x30,0x09,0x31,0x15,0,0x26,255,0,0x75,8,0x95,2,0x81,2,
        0x95,1,0x81,1,0xc0
    };
    usb_hid_slot_t *slot = connect_descriptor(descriptor, sizeof(descriptor), 0U, 0U);
    CHECK(slot->report_info_valid && slot->descriptor_meta.keyboard_item_count == 2U &&
        slot->report_info.report_item_count == 10U, "200 bitmap keys do not exhaust controller field capacity");
    uint8_t packet[30] = {1U};
    packet[1] = 1U; packet[25] = 0x80U; /* usages4 and203 */
    packet[26] = 1U; packet[27] = 64U; packet[28] = 192U;
    hid_process_report(slot, packet, sizeof(packet));
    CHECK(mock_keys[4U] && mock_keys[203U] && mock_reports[0].buttons == 1U &&
        mock_reports[0].axis[0] == 64 && mock_reports[0].axis[1] == 192,
        "complete bitmap/button/axis packet commits all owned fields");
    uint8_t keys2[] = {2U,5U,0U};
    hid_process_report(slot, keys2, sizeof(keys2));
    CHECK(mock_keys[4U] && mock_keys[5U] && mock_keys[203U], "separate ID array adds held B without losing bitmap keys");
    keys2[1] = 1U;
    unsigned before = mock_keyboard_calls;
    hid_process_report(slot, keys2, sizeof(keys2));
    CHECK(mock_keyboard_calls == before && mock_keys[5U], "array rollover retains last good keyboard state");
    unsigned pads_before = mock_report_calls;
    memset(packet + 1U, 0, sizeof(packet) - 1U);
    for (unsigned length = 1U; length < sizeof(packet); ++length) {
        hid_process_report(slot, packet, length);
        CHECK(mock_keyboard_calls == before && mock_report_calls == pads_before &&
            slot->gamepad_buttons == 1U && mock_keys[4U] && mock_keys[203U],
            "every short prefix including omitted trailing constant commits no keys/buttons/axes");
    }
    hid_process_report(slot, packet, sizeof(packet));
    CHECK(!mock_keys[4U] && !mock_keys[203U] && mock_keys[5U] && slot->gamepad_buttons == 0U,
        "full bitmap release preserves the other ID's held key");
    keys2[1] = 0U;
    hid_process_report(slot, keys2, sizeof(keys2));
    CHECK(!mock_key_count, "independent array release clears final held key");
    puts("PASS compact 200-key bitmap, split report IDs, rollover and atomic keys/buttons/axes with trailing padding");
    return 0;
}

static int test_collection_isolation_and_relative_axes(void)
{
    static const uint8_t composite[] = {
        0x05,1,0x09,5,0xa1,1,0x85,1,0x05,9,0x09,1,0x15,0,0x25,1,
        0x75,8,0x95,1,0x81,2,0xc0,
        0x05,1,0x09,2,0xa1,1,0x85,2,0x05,9,0x09,2,0x81,2,
        0x05,1,0x09,0x30,0x09,0x31,0x15,0x81,0x25,0x7f,0x95,2,0x81,6,0xc0,
        0x05,1,0x09,6,0xa1,1,0x85,3,0x05,7,0x19,0,0x29,255,
        0x15,0,0x26,255,0,0x95,1,0x81,0,0xc0
    };
    usb_hid_slot_t *slot = connect_descriptor(composite, sizeof(composite), 0U, 0U);
    CHECK(slot->onee_joystick && !slot->generic_axes && slot->mouse_capable,
        "composite keeps gamepad and native mouse as separate owners");
    const uint8_t pad[] = {1U,1U}, mouse[] = {2U,1U,3U,254U}, key[] = {3U,4U};
    hid_process_report(slot, pad, sizeof(pad));
    unsigned before = mock_report_calls;
    hid_process_report(slot, mouse, sizeof(mouse));
    CHECK(mock_report_calls == before && slot->gamepad_buttons == 1U &&
        mock_mouse_buttons == 2U && mock_mouse_dx == 3 && mock_mouse_dy == -2,
        "mouse buttons and movement cannot become gamepad fields");
    hid_process_report(slot, key, sizeof(key));
    CHECK(mock_report_calls == before && slot->gamepad_buttons == 1U && mock_keys[4U],
        "keyboard reports keep native keys and leave controller buttons held");

    static const uint8_t ten_axes[] = {
        0x05,1,0x09,8,0xa1,1,0x85,1,0x15,0,0x26,255,0,0x75,8,0x95,9,
        0x19,0x30,0x29,0x38,0x81,2,0x05,0x0c,0x0a,0x38,2,0x95,1,0x81,2,
        0x85,2,0x05,1,0x09,0x30,0x09,0x37,0x09,0x38,0x15,0x81,0x25,0x7f,0x95,3,0x81,6,0xc0
    };
    slot = connect_descriptor(ten_axes, sizeof(ten_axes), 0U, 0U);
    const uint8_t absolute[] = {1U,1U,2U,3U,4U,5U,6U,7U,8U,9U,10U};
    hid_process_report(slot, absolute, sizeof(absolute));
    CHECK(mock_reports[0].axis_valid_mask == 0x3ffU &&
        mock_reports[0].axis[ONEE_INPUT_AXIS_SLIDER] == 7 && mock_reports[0].axis[ONEE_INPUT_AXIS_PAN] == 10,
        "all ten absolute axis usages retain distinct values");
    const uint8_t relative[] = {2U,2U,254U,3U};
    mock_time = 100U;
    hid_process_report(slot, relative, sizeof(relative));
    CHECK(mock_reports[0].axis[ONEE_INPUT_AXIS_X] == 512 &&
        mock_reports[0].axis[ONEE_INPUT_AXIS_DIAL] == -4096 &&
        mock_reports[0].axis[ONEE_INPUT_AXIS_WHEEL] == 4096,
        "relative motion scales by count while each wheel/dial detent has its own pulse");
    CHECK(slot->relative_count[ONEE_INPUT_AXIS_DIAL] == 1U && slot->relative_count[ONEE_INPUT_AXIS_WHEEL] == 2U,
        "detent bursts queue remaining events");
    before = mock_report_calls;
    mock_time = 119U; hid_poll_relative_inputs();
    CHECK(mock_report_calls == before, "relative pulse remains active until expiry");
    mock_time = 120U; hid_poll_relative_inputs();
    CHECK(mock_reports[0].axis[0] == 0 && mock_reports[0].axis[ONEE_INPUT_AXIS_DIAL] == 0 &&
        mock_reports[0].axis[ONEE_INPUT_AXIS_WHEEL] == 0, "pulse expiry releases axes without a USB report");
    mock_time = 140U; hid_poll_relative_inputs();
    CHECK(mock_reports[0].axis_valid_mask == ((1U << ONEE_INPUT_AXIS_DIAL) | (1U << ONEE_INPUT_AXIS_WHEEL)) &&
        mock_reports[0].axis[ONEE_INPUT_AXIS_DIAL] == -4096 && mock_reports[0].axis[ONEE_INPUT_AXIS_WHEEL] == 4096,
        "queued detents follow a distinct released phase");
    for (mock_time = 160U; mock_time <= 200U; mock_time += 20U) hid_poll_relative_inputs();
    CHECK(!slot->relative_pending && !slot->relative_active && mock_reports[0].axis[ONEE_INPUT_AXIS_WHEEL] == 0,
        "final queued detent releases and stops polling");
    mock_time = 300U;
    hid_process_report(slot, relative, sizeof(relative));
    CHECK(slot->relative_pending && slot->relative_count[ONEE_INPUT_AXIS_WHEEL], "fresh detents arm a new burst");
    usb_hid_service_set_menu_capture(1U);
    CHECK(!slot->relative_pending && !slot->relative_active && !slot->relative_count[ONEE_INPUT_AXIS_WHEEL],
        "menu transition clears active pulses and queued detents");
    before = mock_report_calls;
    mock_time = 1000U; hid_poll_relative_inputs();
    CHECK(mock_report_calls == before, "cleared detents cannot reappear in the menu");
    puts("PASS collection isolation, ten axes, scaled relative expiry and queued detent release phases");
    return 0;
}

static int test_descriptor_fetch_and_protocol_fallback(void)
{
    const usb_hid_live_fixture_t *mouse = &hid_live_fixtures[6];
    usb_hid_slot_t *slot = connect_descriptor(mouse->descriptor, mouse->length,
        HID_SUBCLASS_BOOTIF, HID_PROTOCOL_MOUSE);
    CHECK(slot->report_info_valid && !slot->boot_mouse && mock_protocol_calls[HID_PROTOCOL_REPORT] == 1U,
        "parsed boot-capable mouse selects report protocol");
    slot->hid->report_size = (uint16_t)(mouse->length + 9U);
    slot->report_info_valid = 0U;
    hid_parse_report_descriptor(slot);
    CHECK(slot->report_info_valid, "short successful fetch parses actual length without fabricated trailing bytes");
    slot->report_info_valid = 0U;
    mock_descriptor_result = -USB_ERR_IO;
    hid_parse_report_descriptor(slot);
    CHECK(!slot->report_info_valid && slot->last_error == -USB_ERR_IO,
        "failed descriptor fetch cannot enable report decoding");
    mock_descriptor_result = 0; mock_descriptor_size = 0U;
    hid_parse_report_descriptor(slot);
    CHECK(!slot->report_info_valid && slot->last_error == -USB_ERR_INVAL,
        "zero-length descriptor fetch fails");

    const uint8_t malformed[] = {0x05U};
    slot = connect_descriptor(malformed, sizeof(malformed), HID_SUBCLASS_BOOTIF, HID_PROTOCOL_MOUSE);
    CHECK(slot->boot_mouse && !slot->report_info_valid && mock_protocol_calls[HID_PROTOCOL_BOOT] == 1U,
        "bad descriptor falls back to proven boot mouse format");
    CHECK(slot->onee_joystick && slot->generic_axes, "fallback boot mouse remains explicitly bindable");
    const uint8_t boot_packet[] = {3U,2U,254U};
    hid_process_report(slot, boot_packet, sizeof(boot_packet));
    CHECK(mock_mouse_dx == 2 && mock_mouse_dy == -2 && mock_mouse_buttons == 3U &&
        mock_reports[0].axis[0] == 512 && mock_reports[0].axis[1] == -512 && mock_reports[0].buttons == 3U,
        "fallback boot mouse retains native movement and generic controller pulses");

    slot = connect_descriptor(mouse->descriptor, mouse->length, HID_SUBCLASS_BOOTIF, HID_PROTOCOL_MOUSE);
    struct usbh_hid *hid = slot->hid;
    usbh_hid_stop(hid);
    mock_protocol_result[HID_PROTOCOL_REPORT] = -USB_ERR_IO;
    usbh_hid_run(hid);
    CHECK(slot->boot_mouse && !slot->report_info_valid, "failed report selection falls back to boot");
    usbh_hid_stop(hid);
    mock_protocol_result[HID_PROTOCOL_BOOT] = -USB_ERR_IO;
    usbh_hid_run(hid);
    unsigned before = mock_mouse_calls, keys_before = mock_keyboard_calls;
    const uint8_t packet[] = {1U,2U,3U,4U,5U,6U,7U,8U};
    hid_process_report(slot, packet, sizeof(packet));
    CHECK(!slot->boot_mouse && !slot->report_info_valid && mock_mouse_calls == before && mock_keyboard_calls == keys_before,
        "failed report and boot selection disables unknown-format decoding");
    memset(mock_protocol_result, 0, sizeof(mock_protocol_result));
    mock_descriptor = NULL; mock_descriptor_size = 0U;
    puts("PASS actual descriptor length, zero/failed fetch and report/boot fallback failure handling");
    return 0;
}

static int test_generic_button_bindings_and_composite_guards(void)
{
    static const uint8_t generic[] = {
        0x05,1,0x09,7,0xa1,1,0x09,0x37,0x15,0x81,0x25,0x7f,0x75,8,0x95,1,0x81,6,
        0x05,9,0x19,1,0x29,32,0x15,0,0x25,1,0x75,1,0x95,32,0x81,2,0xc0
    };
    usb_hid_slot_t *slot = connect_descriptor(generic, sizeof(generic), 0U, 0U);
    usb_controller_device_t device;
    usb_controller_identity_t legacy = {0};
    CHECK(usb_controller_identity_get(0U, &device) && device.generic_axes && device.axis_mask == (1U << ONEE_INPUT_AXIS_DIAL),
        "generic controller metadata exists before its first input report");
    CHECK(usb_controller_identity_resolve(&legacy, 0U, 1U) == USB_CONTROLLER_SLOT_COUNT,
        "generic axis device stays unassigned under Auto");
    uint8_t packet[] = {0U,0x10U,0U,0U,0U};
    usb_hid_service_set_menu_capture(1U);
    unsigned before = mock_slot2_report_calls;
    hid_process_report(slot, packet, sizeof(packet));
    CHECK(mock_reports[0].generic_axes && mock_slot2_report_calls == before && !g_menu_event_count,
        "unbound generic shoulder has no default gamepad navigation or slot2 gameplay");
    packet[1] = 0U; hid_process_report(slot, packet, sizeof(packet));
    usb_hid_service_set_binding_capture(1U);
    packet[1] = 0x10U; hid_process_report(slot, packet, sizeof(packet));
    usb_hid_menu_event_t event;
    const usb_hid_menu_source_t source = usb_hid_menu_source_from_gamepad_button(4U);
    CHECK(usb_hid_service_pop_menu_event(&event) && event.source == source && event.action == USB_HID_MENU_ACTION_NONE,
        "generic button learning exposes an independent gamepad source");
    usb_hid_service_set_binding_capture(0U);
    packet[1] = 0U; hid_process_report(slot, packet, sizeof(packet));
    usb_hid_service_set_gamepad_binding_mask(UINT32_C(1) << 4U);
    usb_hid_service_set_screenshot_sources(source, USB_HID_MENU_SOURCE_NONE);
    usb_hid_service_set_menu_capture(0U);
    packet[1] = 0x10U; hid_process_report(slot, packet, sizeof(packet));
    CHECK(take_event(USB_HID_MENU_ACTION_SCREENSHOT_A2) && !g_menu_event_count,
        "explicit generic button screenshot binding works outside the menu");
    usb_hid_service_set_screenshot_sources(USB_HID_MENU_SOURCE_NONE, USB_HID_MENU_SOURCE_NONE);

    static const uint8_t composite[] = {
        0x05,1,0x09,5,0xa1,1,0x85,1,0x05,9,0x09,3,0x15,0,0x25,1,
        0x75,8,0x95,1,0x81,2,0xc0,
        0x05,1,0x09,2,0xa1,1,0x85,2,0x05,9,0x09,3,0x81,2,
        0x05,1,0x09,0x30,0x09,0x31,0x15,0x81,0x25,0x7f,0x95,2,0x81,6,0xc0
    };
    slot = connect_descriptor(composite, sizeof(composite), 0U, 0U);
    usb_hid_service_set_menu_ok_source(USB_HID_MENU_ACTION_SELECT);
    usb_hid_service_set_menu_open_close_source(USB_HID_MENU_ACTION_SELECT);
    usb_hid_service_set_menu_capture(1U);
    uint8_t pad[] = {1U,0U}, mouse[] = {2U,1U,0U,0U};
    mock_time = 500U;
    hid_process_report(slot, mouse, sizeof(mouse));
    const XTime started = slot->ok_down_tick;
    CHECK(slot->ok_down && !usb_hid_service_all_input_released(), "native mouse starts one physical hold");
    mock_time += 5U;
    hid_process_report(slot, pad, sizeof(pad));
    hid_process_report(slot, mouse, sizeof(mouse));
    CHECK(slot->ok_down && slot->ok_down_tick == started && !g_menu_event_count && !usb_hid_service_all_input_released(),
        "neutral gamepad reports cannot release or restart a held native mouse action");
    mouse[1] = 0U; hid_process_report(slot, mouse, sizeof(mouse));
    CHECK(take_event(USB_HID_MENU_ACTION_SELECT) && !g_menu_event_count && usb_hid_service_all_input_released(),
        "native release finishes its hold exactly once");
    pad[1] = 1U; hid_process_report(slot, pad, sizeof(pad));
    const XTime pad_started = slot->ok_down_tick;
    hid_process_report(slot, mouse, sizeof(mouse));
    CHECK(slot->ok_down && slot->ok_down_tick == pad_started && !g_menu_event_count && !usb_hid_service_all_input_released(),
        "neutral mouse reports cannot release a held legacy gamepad action");
    mouse[1] = 1U; hid_process_report(slot, mouse, sizeof(mouse));
    pad[1] = 0U; hid_process_report(slot, pad, sizeof(pad));
    CHECK(slot->ok_down && !g_menu_event_count && !usb_hid_service_all_input_released(),
        "shared logical button stays held until both physical collections release");
    mouse[1] = 0U; hid_process_report(slot, mouse, sizeof(mouse));
    CHECK(take_event(USB_HID_MENU_ACTION_SELECT) && !g_menu_event_count && usb_hid_service_all_input_released(),
        "final composite release completes one hold with no duplicate press or release");
    puts("PASS generic button learning/explicit screenshot without default gameplay; composite release and menu holds");
    return 0;
}

static int test_optional_identity_metadata(void)
{
    struct mock_hport port = {0}, upstream = {0};
    struct mock_hub hub = {&upstream};
    struct usb_setup_packet setup = {0};
    usb_controller_device_t device, moved;
    usb_controller_identity_init();
    port.connected = 1U; port.setup = &setup; port.port = 3U;
    upstream.port = 2U; port.parent = &hub;
    port.device_desc.idVendor = 0x045eU; port.device_desc.idProduct = 0x028eU;
    port.device_desc.iProduct = 2U; port.device_desc.iSerialNumber = 3U;
    mock_string_calls = mock_mutex_take = mock_mutex_give = 0U;
    mock_string_error = 0; mock_string_bad = 0U;
    identify_controller(0U, &port, 4U, USB_CONTROLLER_TRANSPORT_XBOX360, 123U, 0x7ffU, 0x3fU, 0U);
    CHECK(usb_controller_identity_get(0U, &device) && usb_controller_identity_valid(&device.identity) &&
        strcmp(device.name, "Test Controller") == 0 && device.identity.serial_hash &&
        device.identity.interface_id == 4U && device.identity.descriptor_hash == 123U,
        "real identity integration registers model, interface, product and serial before reports");
    CHECK(mock_string_calls == 3U && mock_string_timeout == 50U && mock_string_language == 0x0411U &&
        mock_mutex_take == 3U && mock_mutex_give == 3U && port.ep0_urb.timeout == 0U,
        "optional strings use discovered language and one bounded request per field");
    port.port = 7U; port.iProduct = "Cached Controller";
    mock_string_calls = 0U;
    identify_controller(1U, &port, 4U, USB_CONTROLLER_TRANSPORT_XBOX360, 123U, 0x7ffU, 0x3fU, 0U);
    CHECK(usb_controller_identity_get(1U, &moved) && mock_string_calls == 2U &&
        strcmp(moved.name, "Cached Controller") == 0 && usb_controller_identity_equal(&device.identity, &moved.identity),
        "cached product skips its read while stable serial identity survives a moved port");
    for (mock_string_bad = 1U; mock_string_bad <= 3U; ++mock_string_bad) {
        CHECK(controller_string_descriptor(&port, 2U, 0x0409U) == 0U,
            "odd, wrong-type and truncated string descriptors fail validation");
    }
    mock_string_bad = 0U;
    mock_string_error = -USB_ERR_NOMEM;
    port.ep0_urb.errorcode = 0; port.ep0_urb.hcpriv = NULL;
    unsigned before = mock_string_calls;
    CHECK(!controller_string_descriptor(&port, 2U, 0x0409U) && mock_string_calls == before + 1U &&
        port.ep0_urb.errorcode == 0 && !port.ep0_urb.timeout && mock_mutex_take == mock_mutex_give,
        "allocation failure clears detached BUSY state, releases the mutex and never retries");
    mock_string_error = -USB_ERR_IO;
    port.iProduct = NULL; before = mock_string_calls;
    identify_controller(2U, &port, 4U, USB_CONTROLLER_TRANSPORT_XBOX360, 123U, 0x7ffU, 0x3fU, 0U);
    CHECK(usb_controller_identity_get(2U, &moved) && !moved.identity.serial_hash && moved.identity.port_path &&
        strstr(moved.name, "Xbox 360") != NULL && mock_string_calls == before + 3U,
        "optional I/O failure still registers a port-bound named controller with one attempt per field");
    before = mock_string_calls;
    port.ep0_urb.errorcode = -USB_ERR_BUSY;
    CHECK(!controller_string_descriptor(&port, 2U, 0U) && mock_string_calls == before,
        "already busy control pipe does not start another optional request");
    port.ep0_urb.errorcode = 0; port.connected = 0U;
    CHECK(!controller_string_descriptor(&port, 2U, 0U) && mock_string_calls == before,
        "disconnected device does not start an optional request");
    mock_string_error = 0; mock_string_bad = 0U;
    puts("PASS production optional USB identity reads: language, serial stability, bounds, timeout and failures");
    return 0;
}

static int take_event(usb_hid_menu_action_t action)
{
    usb_hid_menu_event_t event;
    return usb_hid_service_pop_menu_event(&event) &&
        (event.fixed_action != USB_HID_MENU_ACTION_NONE ? event.fixed_action : event.action) == action;
}

static int take_fixed_event(usb_hid_menu_action_t fixed, usb_hid_menu_source_t source,
    usb_hid_menu_action_t additional)
{
    usb_hid_menu_event_t event;
    return usb_hid_service_pop_menu_event(&event) && event.fixed_action == fixed &&
        event.source == source && event.action == additional && !g_menu_event_count;
}

static void send_fixed_report(usb_hid_slot_t *slot, const onee_input_joystick_report_t *report,
    uint8_t hat)
{
    if (slot->vendor_gamepad) usb_gamepad_input_report(slot->index, report, hat, 0U);
    else hid_process_gamepad_report(slot, report, hat, 0U);
}

static int test_fixed_controller_navigation(void)
{
    static const uint8_t generic_pad[] = {
        0x05,1,0x09,5,0xa1,1,0x85,1,0x05,9,0x19,1,0x29,16,
        0x15,0,0x25,1,0x75,1,0x95,16,0x81,2,
        0x85,2,0x05,1,0x09,0x30,0x09,0x31,0x15,0,0x26,255,0,0x75,8,0x95,2,0x81,2,
        0x85,3,0x09,0x39,0x15,0,0x25,7,0x95,1,0x81,2,0xc0
    };
    static const uint8_t buttons[] = {0U,1U,4U,5U};
    static const usb_hid_menu_action_t actions[] = {
        USB_HID_MENU_ACTION_SELECT, USB_HID_MENU_ACTION_BACK,
        USB_HID_MENU_ACTION_PREV_TAB, USB_HID_MENU_ACTION_NEXT_TAB
    };
    static const uint8_t transports[] = {USB_CONTROLLER_TRANSPORT_XBOX360,
        USB_CONTROLLER_TRANSPORT_XBOXONE, USB_CONTROLLER_TRANSPORT_PS4};
    for (unsigned transport = 0U; transport < 4U; ++transport) {
        usb_hid_slot_t *slot;
        onee_input_joystick_report_t report = {0};
        if (transport == 3U) slot = connect_descriptor(generic_pad, sizeof(generic_pad), 0U, 0U);
        else {
            struct mock_hport port = {0};
            hid_slots_reset_all();
            CHECK(usb_gamepad_input_connect() == 0, "fixed-control vendor slot allocated");
            slot = &g_hid_slots[0];
            port.device_desc.idVendor = 0x045eU; port.device_desc.idProduct = (uint16_t)(0x028eU + transport);
            port.port = 1U;
            usb_gamepad_input_identify(0U, &port, 0U, transports[transport]);
        }
        g_menu_event_rd = g_menu_event_wr = g_menu_event_count = 0U;
        g_menu_capture = g_binding_capture = g_joystick_preview = g_onee_fixed_mode = g_onee_input_blocked = 0U;
        g_gamepad_binding_mask = UINT32_MAX;
        g_menu_ok_source = g_menu_open_close_source = usb_hid_menu_source_from_keyboard_usage(HID_KBD_USAGE_A + 25U);
        g_screenshot_a2_source = g_screenshot_1080p_source = USB_HID_MENU_SOURCE_NONE;
        memset(g_vtw_sources, 0, sizeof(g_vtw_sources));
        report.buttons_valid = 1U;
        send_fixed_report(slot, &report, HID_HAT_NEUTRAL);
        usb_hid_service_set_menu_capture(1U);
        if (transport == 3U) {
            const uint8_t hid_a[] = {1U,1U,0U}, hid_axes[] = {2U,128U,128U};
            const uint8_t hid_home[] = {1U,0U,4U}, hid_release[] = {1U,0U,0U};
            hid_process_report(slot, hid_a, sizeof(hid_a));
            CHECK(take_fixed_event(USB_HID_MENU_ACTION_SELECT, USB_HID_MENU_SOURCE_GAMEPAD_BASE,
                USB_HID_MENU_ACTION_NONE), "real generic HID button report reaches fixed A");
            hid_process_report(slot, hid_axes, sizeof(hid_axes));
            CHECK(!g_menu_event_count && slot->gamepad_buttons == 1U,
                "real generic HID axis report preserves held fixed A without replay");
            hid_process_report(slot, hid_release, sizeof(hid_release));
            hid_process_report(slot, hid_home, sizeof(hid_home));
            CHECK(take_fixed_event(USB_HID_MENU_ACTION_TOGGLE,
                USB_HID_MENU_SOURCE_GAMEPAD_BASE + USB_GAMEPAD_BUTTON_HOME, USB_HID_MENU_ACTION_NONE),
                "real generic HID Home bit reaches fixed toggle");
            hid_process_report(slot, hid_axes, sizeof(hid_axes));
            CHECK(!g_menu_event_count, "real HID partial report cannot retrigger Home");
            hid_process_report(slot, hid_release, sizeof(hid_release));
        }
        for (unsigned mode = 0U; mode < 4U; ++mode) {
            usb_hid_service_set_joystick_preview((uint8_t)(mode & 1U));
            usb_hid_service_set_onee_fixed_mode((uint8_t)((mode >> 1U) & 1U));
            for (unsigned button = 0U; button < 4U; ++button) {
                report.buttons = 0U;
                send_fixed_report(slot, &report, HID_HAT_NEUTRAL);
                report.buttons = UINT32_C(1) << buttons[button];
                send_fixed_report(slot, &report, HID_HAT_NEUTRAL);
                CHECK(take_fixed_event(actions[button], usb_hid_menu_source_from_gamepad_button(buttons[button]),
                    USB_HID_MENU_ACTION_NONE), "fixed A/B/LB/RB retain their action and physical source in every mode");
                CHECK(mock_reports[0].buttons == report.buttons && mock_slot2_reports[0].buttons == report.buttons,
                    "fixed menu buttons still reach both controller gameplay paths");
                send_fixed_report(slot, &report, HID_HAT_NEUTRAL);
                CHECK(!g_menu_event_count, "held fixed button never repeats");
                onee_input_joystick_report_t partial = {0};
                partial.axis_valid_mask = 1U; partial.axis[0] = 128;
                partial.logical_max[0] = 255;
                send_fixed_report(slot, &partial, SLOT2_GAMEPAD_HAT_UNCHANGED);
                CHECK(!g_menu_event_count && slot->gamepad_buttons == report.buttons,
                    "partial axis report preserves held fixed buttons without replay");
            }
        }
        report.buttons = 0U;
        send_fixed_report(slot, &report, HID_HAT_NEUTRAL);
        g_gamepad_binding_mask = UINT32_C(1) << USB_GAMEPAD_BUTTON_LEFT_SHOULDER;
        g_screenshot_a2_source = usb_hid_menu_source_from_gamepad_button(USB_GAMEPAD_BUTTON_LEFT_SHOULDER);
        report.buttons = 0x10U; send_fixed_report(slot, &report, HID_HAT_NEUTRAL);
        CHECK(take_fixed_event(USB_HID_MENU_ACTION_PREV_TAB, g_screenshot_a2_source, USB_HID_MENU_ACTION_SCREENSHOT_A2),
            "initial LB emits fixed tab and explicit global action once");
        report.buttons = 0x30U; send_fixed_report(slot, &report, HID_HAT_NEUTRAL);
        CHECK(!g_menu_event_count, "both shoulders cancel fixed tab direction");
        report.buttons = 0x10U; send_fixed_report(slot, &report, HID_HAT_NEUTRAL);
        CHECK(take_fixed_event(USB_HID_MENU_ACTION_PREV_TAB, USB_HID_MENU_SOURCE_NONE, USB_HID_MENU_ACTION_NONE),
            "release-one shoulder resumes fixed tab without replaying the custom global binding");
        report.buttons = 0U; send_fixed_report(slot, &report, HID_HAT_NEUTRAL);
        g_gamepad_binding_mask = UINT32_MAX;
        g_screenshot_a2_source = USB_HID_MENU_SOURCE_NONE;
        static const uint32_t hats[] = {
            1U << USB_HID_MENU_ACTION_ITEM_UP,
            (1U << USB_HID_MENU_ACTION_ITEM_UP) | (1U << USB_HID_MENU_ACTION_RIGHT),
            1U << USB_HID_MENU_ACTION_RIGHT,
            (1U << USB_HID_MENU_ACTION_ITEM_DOWN) | (1U << USB_HID_MENU_ACTION_RIGHT),
            1U << USB_HID_MENU_ACTION_ITEM_DOWN,
            (1U << USB_HID_MENU_ACTION_ITEM_DOWN) | (1U << USB_HID_MENU_ACTION_LEFT),
            1U << USB_HID_MENU_ACTION_LEFT,
            (1U << USB_HID_MENU_ACTION_ITEM_UP) | (1U << USB_HID_MENU_ACTION_LEFT)
        };
        for (uint8_t hat = 0U; hat < 8U; ++hat) {
            send_fixed_report(slot, &report, HID_HAT_NEUTRAL);
            send_fixed_report(slot, &report, hat);
            uint32_t observed = 0U;
            usb_hid_menu_event_t event;
            while (usb_hid_service_pop_menu_event(&event)) {
                CHECK(event.action == USB_HID_MENU_ACTION_NONE && event.source == USB_HID_MENU_SOURCE_NONE,
                    "fixed Dpad carries no remappable mouse alias");
                observed |= 1U << event.fixed_action;
            }
            CHECK(observed == hats[hat], "all fixed Dpad directions work during preview and fixed ONEe controls");
            send_fixed_report(slot, &report, hat);
            CHECK(!g_menu_event_count, "held fixed Dpad never repeats");
        }
        send_fixed_report(slot, &report, HID_HAT_NEUTRAL);
        report.buttons = 1U;
        send_fixed_report(slot, &report, HID_HAT_NEUTRAL);
        CHECK(take_fixed_event(USB_HID_MENU_ACTION_SELECT, USB_HID_MENU_SOURCE_GAMEPAD_BASE, USB_HID_MENU_ACTION_NONE),
            "fresh A presses after preview changes remain fixed OK");
        usb_hid_service_set_menu_capture(0U);
        usb_hid_service_set_menu_capture(1U);
        send_fixed_report(slot, &report, HID_HAT_NEUTRAL);
        CHECK(!g_menu_event_count, "capture transitions do not replay a held A");
        report.buttons = 0U; send_fixed_report(slot, &report, HID_HAT_NEUTRAL);
        usb_hid_service_set_menu_capture(0U);
        for (unsigned button = 0U; button < 4U; ++button) {
            report.buttons = UINT32_C(1) << buttons[button];
            send_fixed_report(slot, &report, HID_HAT_NEUTRAL);
            CHECK(!g_menu_event_count && mock_reports[0].buttons == report.buttons,
                "fixed navigation remains inactive in normal gameplay while raw controls continue");
            report.buttons = 0U; send_fixed_report(slot, &report, HID_HAT_NEUTRAL);
        }
        report.buttons = UINT32_C(1) << USB_GAMEPAD_BUTTON_HOME;
        send_fixed_report(slot, &report, HID_HAT_NEUTRAL);
        CHECK(take_fixed_event(USB_HID_MENU_ACTION_TOGGLE,
            USB_HID_MENU_SOURCE_GAMEPAD_BASE + USB_GAMEPAD_BUTTON_HOME, USB_HID_MENU_ACTION_NONE),
            "Home requests one global config-menu toggle");
        usb_hid_service_set_menu_capture(1U);
        usb_hid_service_set_joystick_preview(0U);
        send_fixed_report(slot, &report, HID_HAT_NEUTRAL);
        CHECK(!g_menu_event_count, "held Home cannot toggle back after opening capture");
        onee_input_joystick_report_t partial_home = {0};
        partial_home.axis_valid_mask = 1U; partial_home.axis[0] = 128;
        partial_home.logical_max[0] = 255;
        send_fixed_report(slot, &partial_home, SLOT2_GAMEPAD_HAT_UNCHANGED);
        CHECK(!g_menu_event_count && slot->gamepad_buttons == report.buttons,
            "separate axis packet cannot release or repeat the held Home packet");
        report.buttons = 0U; send_fixed_report(slot, &report, HID_HAT_NEUTRAL);
        report.buttons = (UINT32_C(1) << USB_GAMEPAD_BUTTON_HOME) | 1U;
        send_fixed_report(slot, &report, HID_HAT_NEUTRAL);
        CHECK(take_fixed_event(USB_HID_MENU_ACTION_TOGGLE,
            USB_HID_MENU_SOURCE_GAMEPAD_BASE + USB_GAMEPAD_BUTTON_HOME, USB_HID_MENU_ACTION_NONE),
            "Home wins over simultaneous A in its report");
        usb_hid_service_set_menu_capture(0U);
        usb_hid_service_set_onee_input_blocked(1U);
        send_fixed_report(slot, &report, HID_HAT_NEUTRAL);
        CHECK(!g_menu_event_count && mock_blocked && mock_slot2_blocked,
            "held Home across menu close cannot reopen while guest input waits for release");
        report.buttons = 0U; send_fixed_report(slot, &report, HID_HAT_NEUTRAL);
        usb_hid_service_set_onee_input_blocked(0U);
        usb_hid_service_set_menu_capture(1U);
        const usb_hid_menu_source_t a_source = USB_HID_MENU_SOURCE_GAMEPAD_BASE;
        g_screenshot_a2_source = a_source;
        report.buttons = 1U; send_fixed_report(slot, &report, HID_HAT_NEUTRAL);
        CHECK(take_fixed_event(USB_HID_MENU_ACTION_SELECT, a_source, USB_HID_MENU_ACTION_SCREENSHOT_A2),
            "fixed OK and a conflicting explicit global mapping share one physical event");
        report.buttons = 0U; send_fixed_report(slot, &report, HID_HAT_NEUTRAL);
        usb_hid_service_set_binding_capture(1U);
        report.buttons = 1U; send_fixed_report(slot, &report, HID_HAT_NEUTRAL);
        usb_hid_menu_event_t learned;
        CHECK(usb_hid_service_pop_menu_event(&learned) && learned.source == a_source &&
            learned.fixed_action == USB_HID_MENU_ACTION_SELECT &&
            learned.action == USB_HID_MENU_ACTION_SCREENSHOT_A2 && !g_menu_event_count,
            "learning receives one physical event with both actions for capture before consumer dispatch");
        usb_hid_service_set_binding_capture(0U);
        g_screenshot_a2_source = USB_HID_MENU_SOURCE_NONE;
        report.buttons = 0U; send_fixed_report(slot, &report, HID_HAT_NEUTRAL);
        const usb_hid_menu_source_t home_source =
            usb_hid_menu_source_from_gamepad_button(USB_GAMEPAD_BUTTON_HOME);
        g_screenshot_a2_source = home_source;
        for (uint8_t custom_button = 2U; custom_button <= 3U; ++custom_button) {
            const usb_hid_menu_source_t custom_source = usb_hid_menu_source_from_gamepad_button(custom_button);
            usb_hid_service_set_menu_ok_source(custom_source);
            usb_hid_service_set_menu_open_close_source(custom_source);
            report.buttons = UINT32_C(1) << custom_button;
            send_fixed_report(slot, &report, HID_HAT_NEUTRAL);
            CHECK(slot->ok_down && slot->open_close_down && !g_menu_event_count,
                "custom X/Y OK arms its short-press and open-close holds");
            mock_time += 5U;
            report.buttons = UINT32_C(1) << USB_GAMEPAD_BUTTON_HOME;
            send_fixed_report(slot, &report, HID_HAT_NEUTRAL);
            CHECK(take_fixed_event(USB_HID_MENU_ACTION_TOGGLE, home_source, USB_HID_MENU_ACTION_SCREENSHOT_A2),
                "Home press plus custom OK release emits only one combined toggle and screenshot event");
            CHECK(!slot->ok_down && !slot->open_close_down && !slot->ok_down_tick &&
                slot->ok_down_source == USB_HID_MENU_SOURCE_NONE &&
                slot->ok_down_action == USB_HID_MENU_ACTION_NONE,
                "Home cancels pending custom OK and open-close holds without selecting a row");
            send_fixed_report(slot, &report, HID_HAT_NEUTRAL);
            mock_time += MOUSE_MENU_HOLD_TICKS;
            hid_slots_poll_holds();
            CHECK(!g_menu_event_count, "held Home and expired canceled holds cannot repeat toggle or screenshot");
            report.buttons = 0U; send_fixed_report(slot, &report, HID_HAT_NEUTRAL);
            CHECK(!g_menu_event_count, "Home release cannot dispatch the canceled custom OK action");
        }
        g_screenshot_a2_source = USB_HID_MENU_SOURCE_NONE;
    }
    puts("PASS fixed A/B/shoulders/Dpad/Home for Xbox360/XboxOne/PS4/HID, preview, remaps, held transitions and gameplay");
    return 0;
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
            "learning keeps every button source separate in one event for capture before dispatch");
        report.buttons=0;usb_gamepad_input_report(0,&report,8,0);
    }
    usb_hid_service_set_binding_capture(0);
    usb_hid_service_set_gamepad_binding_mask(UINT32_C(1)<<4);
    report.buttons=0x10U;usb_gamepad_input_report(0,&report,8,0);
    CHECK(usb_hid_service_pop_menu_event(&event) && event.action==USB_HID_MENU_ACTION_NONE &&
        event.source==USB_HID_MENU_SOURCE_GAMEPAD_BASE+4 &&
        event.fixed_action==USB_HID_MENU_ACTION_PREV_TAB && !g_menu_event_count,
        "explicit LB binding accompanies its fixed tab action in one event");
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
    g_onee_fixed_mode = 0U;
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

static int test_fixed_onee_keyboard_menu(uint8_t standby)
{
    static const struct {
        uint8_t usage;
        usb_hid_menu_action_t action;
    } navigation[] = {
        {HID_KBD_USAGE_UP, USB_HID_MENU_ACTION_ITEM_UP},
        {HID_KBD_USAGE_DOWN, USB_HID_MENU_ACTION_ITEM_DOWN},
        {HID_KBD_USAGE_LEFT, USB_HID_MENU_ACTION_LEFT},
        {HID_KBD_USAGE_RIGHT, USB_HID_MENU_ACTION_RIGHT},
        {HID_KBD_USAGE_ENTER, USB_HID_MENU_ACTION_SELECT},
        {HID_KBD_USAGE_KPDEMTER, USB_HID_MENU_ACTION_SELECT},
        {HID_KBD_USAGE_TAB, USB_HID_MENU_ACTION_NEXT_TAB},
        {HID_KBD_USAGE_ESCAPE, USB_HID_MENU_ACTION_BACK},
    };
    const uint8_t no_keys[] = {0U};
    usb_hid_menu_event_t event;
    usb_hid_slot_t *slot = &g_hid_slots[0];

    hid_slots_reset_all();
    g_menu_event_rd = g_menu_event_wr = g_menu_event_count = 0U;
    g_menu_capture = g_binding_capture = g_joystick_preview = 0U;
    g_onee_fixed_mode = standby == 0U;
    usb_hid_service_set_onee_standby_mode(standby);
    CHECK(g_onee_fixed_mode == (standby == 0U), "standby must not select the virtual machine");
    g_onee_input_blocked = g_ready = 0U;
    memset(g_vtw_sources, 0, sizeof(g_vtw_sources));
    slot->active = 1U;

    for (unsigned i = 0U; i < sizeof(navigation) / sizeof(navigation[0]); ++i) {
        const uint8_t keys[] = {navigation[i].usage};
        const usb_hid_menu_source_t source =
            usb_hid_menu_source_from_keyboard_usage(keys[0]);
        /* Conflicting saved bindings must not run in either menu state. */
        g_screenshot_a2_source = g_screenshot_1080p_source = source;
        g_vtw_sources[0] = source;
        g_menu_ok_source = g_menu_open_close_source = source;
        usb_hid_service_set_menu_capture(0U);
        usb_hid_service_set_onee_input_blocked(0U);
        hid_process_keyboard_usages(slot, 0U, keys, sizeof(keys));
        CHECK(mock_keys[keys[0]] && !g_menu_event_count && !slot->open_close_down,
            "closed ONEe menu leaves navigation in the guest without saved shortcuts or holds");
        mock_time += MOUSE_MENU_HOLD_TICKS;
        hid_slots_poll_holds();
        hid_process_keyboard_usages(slot, 0U, keys, sizeof(keys));
        CHECK(!g_menu_event_count, "holding a gameplay navigation key cannot open the menu");
        usb_hid_service_set_menu_capture(1U);
        usb_hid_service_set_onee_input_blocked(1U);
        hid_process_keyboard_usages(slot, 0U, keys, sizeof(keys));
        CHECK(!g_menu_event_count && !mock_key_count,
            "opening the menu cannot turn an already-held guest key into menu input");
        hid_process_keyboard_usages(slot, 0U, no_keys, sizeof(no_keys));
        hid_process_keyboard_usages(slot, 0U, keys, sizeof(keys));
        CHECK(!mock_key_count && !mock_modifiers &&
            usb_hid_service_pop_menu_event(&event) && event.action == navigation[i].action &&
            event.source == source && event.fixed_action == USB_HID_MENU_ACTION_NONE &&
            !g_menu_event_count && !slot->open_close_down && !slot->ok_down,
            "menu navigation emits one immediate fixed action and no guest input or saved hold");
        hid_process_keyboard_usages(slot, 0U, keys, sizeof(keys));
        CHECK(!g_menu_event_count, "a held menu key cannot repeat its action on identical reports");
        usb_hid_service_set_menu_capture(0U);
        hid_process_keyboard_usages(slot, 0U, keys, sizeof(keys));
        CHECK(!g_menu_event_count && !mock_key_count && !usb_hid_service_all_input_released(),
            "closing the menu retains guest blocking until held navigation is released");
        hid_process_keyboard_usages(slot, 0U, no_keys, sizeof(no_keys));
        CHECK(!g_menu_event_count && usb_hid_service_all_input_released(),
            "key release cannot select, reopen or dispatch a saved shortcut");
    }

    g_screenshot_a2_source = g_screenshot_1080p_source = 0U;
    memset(g_vtw_sources, 0, sizeof(g_vtw_sources));
    for (unsigned side = 0U; side < 2U; ++side) {
        const uint8_t modifier = (uint8_t)(1U << (side ? 5U : 1U));
        const uint8_t tab[] = {HID_KBD_USAGE_TAB};
        hid_process_keyboard_usages(slot, modifier, no_keys, sizeof(no_keys));
        usb_hid_service_set_menu_capture(1U);
        hid_process_keyboard_usages(slot, modifier, tab, sizeof(tab));
        CHECK(take_event(USB_HID_MENU_ACTION_PREV_TAB) && !g_menu_event_count,
            "both Shift modifiers reach fixed previous-tab routing");
        hid_process_keyboard_usages(slot, 0U, no_keys, sizeof(no_keys));
        usb_hid_service_set_menu_capture(0U);
    }
    usb_hid_service_set_onee_input_blocked(0U);
    for (unsigned side = 0U; side < 2U; ++side) {
        const uint8_t modifier = (uint8_t)(1U << (side ? 7U : 3U));
        hid_process_keyboard_usages(slot, modifier, no_keys, sizeof(no_keys));
        CHECK(take_event(USB_HID_MENU_ACTION_OPEN) && !g_menu_event_count && !mock_modifiers,
            "either Windows modifier opens the menu without asserting an Apple key");
        usb_hid_service_set_menu_capture(1U);
        hid_process_keyboard_usages(slot, modifier, no_keys, sizeof(no_keys));
        hid_process_keyboard_usages(slot, 0U, no_keys, sizeof(no_keys));
        hid_process_keyboard_usages(slot, modifier, no_keys, sizeof(no_keys));
        CHECK(!g_menu_event_count, "Windows is open-only, including a fresh press inside the menu");
        hid_process_keyboard_usages(slot, 0U, no_keys, sizeof(no_keys));
        const uint8_t pause[] = {HID_KBD_USAGE_PAUSE};
        hid_process_keyboard_usages(slot, 0U, pause, sizeof(pause));
        CHECK(take_event(USB_HID_MENU_ACTION_CLOSE) && !g_menu_event_count,
            "Pause retains menu close");
        usb_hid_service_set_menu_capture(0U);
        hid_process_keyboard_usages(slot, 0U, pause, sizeof(pause));
        CHECK(!g_menu_event_count, "held Pause cannot reopen after closing");
        hid_process_keyboard_usages(slot, 0U, no_keys, sizeof(no_keys));
    }
    const uint8_t custom_ok[] = {HID_KBD_USAGE_A};
    g_menu_ok_source = g_menu_open_close_source =
        usb_hid_menu_source_from_keyboard_usage(custom_ok[0]);
    hid_process_keyboard_usages(slot, 0U, custom_ok, sizeof(custom_ok));
    mock_time += MOUSE_MENU_HOLD_TICKS;
    hid_slots_poll_holds();
    CHECK(mock_keys[custom_ok[0]] && !g_menu_event_count && !slot->open_close_down,
        "a saved keyboard OK hold cannot add another ONEe menu-opening key");
    hid_process_keyboard_usages(slot, 0U, no_keys, sizeof(no_keys));
    usb_hid_service_set_onee_fixed_mode(0U);
    usb_hid_service_set_onee_standby_mode(0U);
    hid_process_keyboard_usages(slot, 0U, custom_ok, sizeof(custom_ok));
    mock_time += MOUSE_MENU_HOLD_TICKS;
    hid_slots_poll_holds();
    CHECK(take_event(USB_HID_MENU_ACTION_OPEN), "ordinary Apple-host saved keyboard OK holds still work");
    printf("PASS %s ONEe keyboard navigation, Windows/Pause, Shift+Tab, conflicts and held transitions\n",
           standby != 0U ? "OFF" : "running");
    return 0;
}

static int test_onee_standby_transition(void)
{
    usb_hid_slot_t *slot = &g_hid_slots[0];
    const uint8_t keys[] = {HID_KBD_USAGE_A};
    const uint8_t released[] = {0U};

    hid_slots_reset_all();
    g_onee_fixed_mode = g_onee_standby_mode = g_menu_capture = 0U;
    g_menu_event_rd = g_menu_event_wr = g_menu_event_count = 0U;
    g_menu_ok_source = g_menu_open_close_source =
        usb_hid_menu_source_from_keyboard_usage(keys[0]);
    slot->active = 1U;
    hid_process_keyboard_usages(slot, 0U, keys, sizeof(keys));
    CHECK(slot->open_close_down, "ordinary keyboard press arms its saved menu hold");
    usb_hid_service_set_onee_standby_mode(1U);
    mock_time += MOUSE_MENU_HOLD_TICKS;
    hid_slots_poll_holds();
    hid_process_keyboard_usages(slot, 0U, keys, sizeof(keys));
    CHECK(!slot->open_close_down && !g_menu_event_count && !g_onee_fixed_mode,
        "entering standby cancels an old hold without replaying its key or selecting ONEe");
    hid_process_keyboard_usages(slot, 0U, released, sizeof(released));
    CHECK(!g_menu_event_count, "release after standby transition cannot open or select");
    usb_hid_service_set_onee_standby_mode(0U);
    puts("PASS OFF policy transition cancels saved keyboard holds without key replay");
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
    CHECK(take_event(USB_HID_MENU_ACTION_ITEM_UP) &&
          take_event(USB_HID_MENU_ACTION_RIGHT) && !g_menu_event_count,
          "left stick and D-pad navigate the menu");
    usb_gamepad_input_report(0U, &report, 0U, 0U);
    CHECK(!g_menu_event_count, "unchanged axis and hat do not repeat menu edges");
    usb_hid_service_set_joystick_preview(1U);
    report.axis[ONEE_INPUT_AXIS_X] = -32768;
    usb_gamepad_input_report(0U, &report, 4U, 0U);
    CHECK(take_event(USB_HID_MENU_ACTION_ITEM_DOWN) && !g_menu_event_count &&
          mock_reports[0].axis[ONEE_INPUT_AXIS_X] == -32768,
          "preview keeps fixed Dpad navigation while suppressing stick navigation");
    CHECK(mock_blocked && !usb_hid_service_all_input_released(),
          "preview retains guest block and release guard");
    report.buttons = 1U;
    usb_gamepad_input_report(0U, &report, 4U, 0U);
    CHECK(take_event(USB_HID_MENU_ACTION_SELECT), "preview keeps fixed A navigation");
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
          take_event(USB_HID_MENU_ACTION_SELECT),
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
    CHECK(test_captured_descriptors() == 0, "captured descriptors and runtime");
    CHECK(test_keyboard_chunks_and_atomicity() == 0, "keyboard chunks and packet atomicity");
    CHECK(test_collection_isolation_and_relative_axes() == 0, "collection and relative axis parity");
    CHECK(test_descriptor_fetch_and_protocol_fallback() == 0, "descriptor fetch and protocol fallback");
    CHECK(test_generic_button_bindings_and_composite_guards() == 0, "generic bindings and composite holds");
    CHECK(test_optional_identity_metadata() == 0, "optional USB identity metadata");
    CHECK(test_fixed_controller_navigation() == 0, "fixed controller navigation alongside saved bindings");
    CHECK(test_fixed_onee_keyboard_menu(0U) == 0, "running ONEe keyboard menu routing");
    CHECK(test_fixed_onee_keyboard_menu(1U) == 0, "OFF ONEe keyboard menu routing");
    CHECK(test_onee_standby_transition() == 0, "ONEe standby routing transition");
    return 0;
}
