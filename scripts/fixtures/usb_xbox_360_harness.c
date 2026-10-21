/* Exercise the production transport and pinned CherryUSB Xbox class. Only
 * controller scheduling, time, UART, and shared input consumers are replaced. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "usbh_core.h"
#include "usbh_xbox.h"
#include "usb_xbox_360.h"
#include "usb_gamepad_input.h"

extern const struct usbh_class_info xbox_custom_class_info;

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, message); \
        exit(1); \
    } \
} while (0)

#define PORTS 5U
static struct usbh_hubport ports[PORTS];
static struct usb_setup_packet setups[PORTS];
static struct usbh_urb *pending[PORTS][2];
static unsigned submits[PORTS][2], reject_count[PORTS][2];
static int reject_result[PORTS][2];
static unsigned class_connected[PORTS], identified[PORTS];
static uint8_t port_slot[PORTS];
static onee_input_joystick_report_t last_report[ONEE_INPUT_DEVICE_SLOT_COUNT];
static uint8_t last_hat[ONEE_INPUT_DEVICE_SLOT_COUNT];
static unsigned report_count[ONEE_INPUT_DEVICE_SLOT_COUNT];
static unsigned disconnect_count[ONEE_INPUT_DEVICE_SLOT_COUNT];
static uint32_t now_ms, allocated;
static unsigned connects, controls, kills, in_callback;
static int control_result;
static char diagnostics[32768];
static size_t diagnostics_length;

static unsigned port_index(const struct usbh_hubport *port)
{
    CHECK(port >= ports && port < ports + PORTS, "unknown USB device");
    return (unsigned)(port - ports);
}

uint32_t cherryusb_baremetal_ms(void) { return now_ms; }
void usb_osal_thread_schedule_other(void) {}

int cherryusb_printf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    const int result = vsnprintf(diagnostics + diagnostics_length,
                                 sizeof(diagnostics) - diagnostics_length, format, args);
    va_end(args);
    CHECK(result >= 0 && (size_t)result < sizeof(diagnostics) - diagnostics_length,
          "fixture diagnostic log overflow");
    diagnostics_length += (size_t)result;
    return result;
}

void uart_puts(uint32_t base, const char *text) { (void)base; cherryusb_printf("%s", text); }

int usb_gamepad_input_connect(void)
{
    for (unsigned slot = 0U; slot < ONEE_INPUT_DEVICE_SLOT_COUNT; ++slot) {
        if (!(allocated & (UINT32_C(1) << slot))) {
            allocated |= UINT32_C(1) << slot;
            ++connects;
            return (int)slot;
        }
    }
    return -1;
}

void usb_gamepad_input_identify(uint8_t slot, struct usbh_hubport *port,
                                uint8_t interface, uint8_t transport)
{
    const unsigned device = port_index(port);
    CHECK(interface == 0U && transport == 1U, "incorrect Xbox 360 identity");
    CHECK(allocated & (UINT32_C(1) << slot), "identified unallocated input slot");
    port_slot[device] = slot;
    ++identified[device];
}

void usb_gamepad_input_disconnect(uint8_t slot)
{
    CHECK(allocated & (UINT32_C(1) << slot), "released another or already free input slot");
    allocated &= ~(UINT32_C(1) << slot);
    ++disconnect_count[slot];
    memset(&last_report[slot], 0, sizeof(last_report[slot]));
}

void usb_gamepad_input_report(uint8_t slot, const onee_input_joystick_report_t *report,
                              uint8_t hat, uint8_t extra_active)
{
    CHECK(allocated & (UINT32_C(1) << slot), "reported input on a free slot");
    CHECK(extra_active == 0U, "transport hid bindable controls in extra_active");
    last_report[slot] = *report;
    last_hat[slot] = hat;
    ++report_count[slot];
}

int usbh_control_transfer(struct usbh_hubport *port, struct usb_setup_packet *setup, uint8_t *data)
{
    const unsigned device = port_index(port);
    CHECK(!in_callback, "initialization blocked an interrupt callback");
    CHECK(setup == port->setup && setup->bmRequestType == 0xc1U && setup->bRequest == 1U &&
          setup->wValue == 0x0100U && setup->wIndex == 0U && setup->wLength == 20U,
          "incorrect Xbox 360 vendor initialization");
    CHECK(submits[device][1] != 0U,
          "initialization preceded first input submission");
    CHECK(((uintptr_t)data % CONFIG_USB_ALIGN_SIZE) == 0U, "unaligned control DMA buffer");
    ++controls;
    return control_result;
}

int usbh_submit_urb(struct usbh_urb *urb)
{
    const unsigned device = port_index(urb->hport);
    const unsigned direction = (urb->ep->bEndpointAddress & 0x80U) != 0U;
    CHECK(urb->timeout == 0U && urb->complete != NULL, "interrupt transfer blocked");
    CHECK(pending[device][direction] == NULL, "endpoint submitted twice");
    CHECK(((uintptr_t)urb->transfer_buffer % CONFIG_USB_ALIGN_SIZE) == 0U,
          "unaligned interrupt DMA buffer");
    CHECK(urb->transfer_buffer_length <= 64U, "transfer exceeds owned DMA storage");
    ++submits[device][direction];
    if (reject_count[device][direction]) {
        --reject_count[device][direction];
        return reject_result[device][direction];
    }
    pending[device][direction] = urb;
    urb->hcpriv = urb;
    if (!direction) {
        const uint8_t command[3] = {1U, 3U, 2U};
        CHECK(urb->transfer_buffer_length == 3U &&
              memcmp(urb->transfer_buffer, command, sizeof(command)) == 0,
              "incorrect steady player-1 LED command");
    }
    return 0;
}

static void complete(unsigned device, unsigned direction, const uint8_t *data, int length)
{
    struct usbh_urb *urb = pending[device][direction];
    CHECK(urb != NULL, "no pending transfer");
    if (direction && length > 0) {
        CHECK((uint32_t)length <= urb->transfer_buffer_length, "fixture receive exceeds transfer");
        memcpy(urb->transfer_buffer, data, (size_t)length);
    }
    pending[device][direction] = NULL;
    urb->hcpriv = NULL;
    urb->actual_length = length > 0 ? (uint32_t)length : 0U;
    ++in_callback;
    urb->complete(urb->arg, length);
    --in_callback;
}

int usbh_kill_urb(struct usbh_urb *urb)
{
    for (unsigned device = 0U; device < PORTS; ++device) {
        for (unsigned direction = 0U; direction < 2U; ++direction) {
            if (pending[device][direction] == urb) {
                ++kills;
                complete(device, direction, NULL, -USB_ERR_SHUTDOWN);
            }
        }
    }
    return 0;
}

static void disconnect_device(unsigned device)
{
    if (class_connected[device]) {
        ports[device].connected = false;
        CHECK(xbox_custom_class_info.class_driver->disconnect(&ports[device], 0U) == 0,
              "CherryUSB class disconnect failed");
        class_connected[device] = 0U;
    }
}

static void prepare(void)
{
    for (unsigned device = 0U; device < PORTS; ++device) disconnect_device(device);
    usb_xbox_360_stop();
    memset(ports, 0, sizeof(ports));
    memset(setups, 0, sizeof(setups));
    memset(pending, 0, sizeof(pending));
    memset(submits, 0, sizeof(submits));
    memset(reject_count, 0, sizeof(reject_count));
    memset(reject_result, 0, sizeof(reject_result));
    memset(identified, 0, sizeof(identified));
    memset(last_report, 0, sizeof(last_report));
    memset(report_count, 0, sizeof(report_count));
    memset(disconnect_count, 0, sizeof(disconnect_count));
    memset(port_slot, 0, sizeof(port_slot));
    diagnostics[0] = '\0'; diagnostics_length = 0U;
    connects = controls = kills = in_callback = 0U;
    now_ms = 100U;
    allocated = 0U;
    control_result = 20;
    for (unsigned device = 0U; device < PORTS; ++device) {
        struct usbh_hubport *port = &ports[device];
        port->connected = true;
        port->speed = USB_SPEED_FULL;
        port->setup = &setups[device];
        port->device_desc.idVendor = 0x045eU;
        port->device_desc.idProduct = 0x028eU;
        struct usbh_interface_altsetting *alt = &port->config.intf[0].altsetting[0];
        alt->intf_desc.bNumEndpoints = 2U;
        alt->ep[0].ep_desc.bEndpointAddress = 0x81U;
        alt->ep[1].ep_desc.bEndpointAddress = 0x02U;
        for (unsigned endpoint = 0U; endpoint < 2U; ++endpoint) {
            alt->ep[endpoint].ep_desc.bmAttributes = USB_ENDPOINT_TYPE_INTERRUPT;
            alt->ep[endpoint].ep_desc.wMaxPacketSize = 32U;
            alt->ep[endpoint].ep_desc.bInterval = 4U;
        }
    }
}

static void connect_device(unsigned device)
{
    CHECK(xbox_custom_class_info.class_driver->connect(&ports[device], 0U) == 0,
          "CherryUSB class connect failed");
    class_connected[device] = 1U;
}

static void tick(uint32_t milliseconds) { now_ms += milliseconds; usb_xbox_360_poll(); }

static void set_axis(uint8_t *packet, unsigned offset, int32_t value)
{
    packet[offset] = (uint8_t)value;
    packet[offset + 1U] = (uint8_t)((uint32_t)value >> 8U);
}

static void test_decode(void)
{
    prepare();
    connect_device(0U);
    CHECK(connects == 1U && controls == 1U && identified[0] == 1U,
          "Xbox class failed to allocate, identify, or initialize input");
    CHECK(pending[0][1]->transfer_buffer_length == 32U, "receive length ignored endpoint maximum");
    complete(0U, 0U, NULL, 3);
    uint8_t packet[32] = {0U, 20U, 0xf9U, 0xf7U, 0U, 255U};
    set_axis(packet, 6U, -32768); set_axis(packet, 8U, 32767);
    set_axis(packet, 10U, 32767); set_axis(packet, 12U, -32768);
    complete(0U, 1U, packet, 20);
    const unsigned slot = port_slot[0];
    const onee_input_joystick_report_t *report = &last_report[slot];
    CHECK(report_count[slot] == 1U && report->axis_valid_mask == 0x3fU && report->buttons_valid,
          "valid report omitted controls or exposed absent axes");
    CHECK(report->buttons == 0x7ffU && last_hat[slot] == 1U, "button/diagonal mapping failed");
    CHECK(report->axis[0] == -32768 && report->axis[1] == -32768 && report->axis[2] == 0 &&
          report->axis[3] == 32767 && report->axis[4] == 32767 && report->axis[5] == 255,
          "stick signs, signed bounds, or trigger offsets failed");
    CHECK(report->logical_min[0] == -32768 && report->logical_max[0] == 32767 &&
          report->logical_min[2] == 0 && report->logical_max[2] == 255,
          "stick/trigger ranges failed");
    for (unsigned axis = 6U; axis < ONEE_INPUT_AXIS_COUNT; ++axis)
        CHECK(report->axis[axis] == 0 && report->logical_max[axis] == 0, "unreported generic axis was initialized");
    static const uint8_t directions[9] = {1U, 9U, 8U, 10U, 2U, 6U, 4U, 5U, 0U};
    packet[3] = 0U;
    for (uint8_t hat = 0U; hat < 9U; ++hat) {
        packet[2] = directions[hat];
        complete(0U, 1U, packet, 32); /* Valid padded report. */
        CHECK(last_hat[slot] == hat && last_report[slot].buttons == 0U, "hat or button release failed");
    }
    packet[2] = 7U; complete(0U, 1U, packet, 20);
    CHECK(last_hat[slot] == 8U, "opposing D-pad directions produced a false hat");
    const unsigned before = report_count[slot];
    complete(0U, 1U, packet, 19);
    packet[0] = 1U; complete(0U, 1U, packet, 20);
    packet[0] = 0U; packet[1] = 19U; complete(0U, 1U, packet, 20);
    complete(0U, 1U, NULL, 0);
    CHECK(report_count[slot] == before && pending[0][1] != NULL, "malformed/empty packet leaked input or stopped polling");
    usb_xbox_360_dump_status(0U);
    CHECK(strstr(diagnostics, "invalid=3") && strstr(diagnostics, "led=1 out_try=1"),
          "Xbox status omitted receive/LED diagnostics");
    puts("PASS Xbox 360 class initialization, full analog/buttons/hats and malformed packets");
}

static void test_retries(void)
{
    prepare();
    reject_count[0][1] = 1U; reject_result[0][1] = -USB_ERR_NOMEM;
    reject_count[0][0] = 1U; reject_result[0][0] = -USB_ERR_BUSY;
    control_result = -USB_ERR_STALL;
    connect_device(0U);
    CHECK(connects == 1U && !pending[0][1] && !pending[0][0], "initial submit failure lost the input slot");
    tick(9U); CHECK(submits[0][1] == 1U && submits[0][0] == 1U, "retry ran before backoff");
    tick(1U); CHECK(submits[0][1] == 2U && submits[0][0] == 2U, "submission retry failed");
    complete(0U, 1U, NULL, -USB_ERR_IO);
    complete(0U, 0U, NULL, 2); /* Short OUT is a failed command. */
    tick(9U); CHECK(!pending[0][1] && !pending[0][0], "completion error retried without backoff");
    tick(1U); CHECK(pending[0][1] && pending[0][0], "completion retry failed");
    complete(0U, 0U, NULL, -USB_ERR_IO);
    tick(100U); CHECK(submits[0][0] == 3U && !pending[0][0], "LED retries exceeded three attempts");
    now_ms = UINT32_MAX - 5U;
    complete(0U, 1U, NULL, -USB_ERR_IO);
    const unsigned before = submits[0][1];
    tick(9U); CHECK(submits[0][1] == before, "retry wrap skipped delay");
    tick(1U); CHECK(submits[0][1] == before + 1U, "retry failed across clock wrap");
    static const int terminal[] = {-USB_ERR_STALL, -USB_ERR_SHUTDOWN, -USB_ERR_NODEV, -USB_ERR_NOTCONN};
    for (unsigned error = 0U; error < sizeof(terminal) / sizeof(terminal[0]); ++error) {
        prepare(); connect_device(0U);
        complete(0U, 0U, NULL, terminal[error]);
        tick(100U); CHECK(submits[0][0] == 1U, "terminal OUT error retried");
        CHECK(pending[0][1] != NULL, "LED failure stopped input");
    }
    prepare(); control_result = -USB_ERR_IO; connect_device(0U);
    CHECK(pending[0][1] && pending[0][0] && strstr(diagnostics, "initialization warning"),
          "vendor warning stopped input or lacked a diagnostic");
    puts("PASS Xbox 360 submit/completion backoff, bounded LED attempts and clock wrap");
}

static void test_slots_and_disconnect(void)
{
    prepare();
    allocated = (1U << 0U) | (1U << 3U); /* Existing HID and Xbox One slots. */
    for (unsigned device = 0U; device < 4U; ++device) connect_device(device);
    CHECK(connects == 4U && port_slot[0] == 1U && port_slot[1] == 2U &&
          port_slot[2] == 4U && port_slot[3] == 5U, "mixed transport slots collided");
    CHECK(xbox_custom_class_info.class_driver->connect(&ports[4], 0U) == -USB_ERR_NOMEM,
          "Xbox class capacity was ignored");
    uint8_t packet[20] = {0U, 20U, 1U, 0x14U};
    complete(1U, 1U, packet, sizeof(packet));
    CHECK(report_count[2] == 1U && last_report[2].buttons == (1U | (1U << 10U)) &&
          report_count[1] == 0U, "one Xbox changed another pad's state");
    disconnect_device(1U);
    CHECK(disconnect_count[2] == 1U && !last_report[2].buttons && !pending[1][0] &&
          !pending[1][1] && (allocated & 3U) == 3U && (allocated & (1U << 3U)),
          "disconnect failed to release held input or touched another transport");
    tick(100U); CHECK(!pending[1][0] && !pending[1][1], "disconnect callbacks rearmed a transfer");
    ports[1].connected = true; connect_device(1U);
    CHECK(port_slot[1] == 2U && identified[1] == 2U, "reconnect did not reuse free slots");
    usb_xbox_360_stop();
    CHECK(allocated == ((1U << 0U) | (1U << 3U)), "global stop released non-Xbox slots");
    const unsigned cancelled = kills;
    usb_xbox_360_stop(); tick(100U);
    CHECK(kills == cancelled, "repeated stop cancelled stale transfers");
    for (unsigned device = 0U; device < 4U; ++device)
        CHECK(!pending[device][0] && !pending[device][1], "global stop left a live DMA transfer");
    puts("PASS Xbox 360 multiple mixed slots, disconnect while held, reconnect and stop cancellation");
}

static void test_endpoints_and_matching(void)
{
    const struct usbh_class_info *info = &xbox_custom_class_info;
    CHECK(info->bInterfaceClass == 0xffU && info->bInterfaceSubClass == 0x5dU &&
          info->bInterfaceProtocol == 1U && (info->match_flags & USB_CLASS_MATCH_VID_PID),
          "wrong wired Xbox class match");
    unsigned known = 0U;
    for (unsigned index = 0U; info->id_table[index][0] != 0U; ++index)
        if (info->id_table[index][0] == 0x045eU && info->id_table[index][1] == 0x028eU) known = 1U;
    CHECK(known, "Microsoft Xbox 360 ID absent from pinned class");
    prepare();
    ports[0].config.intf[0].altsetting[0].intf_desc.bNumEndpoints = 1U;
    connect_device(0U);
    CHECK(pending[0][1] && !pending[0][0], "missing optional OUT endpoint disabled input");
    prepare();
    ports[0].config.intf[0].altsetting[0].ep[0].ep_desc.wMaxPacketSize = 512U;
    ports[0].config.intf[0].altsetting[0].ep[1].ep_desc.bmAttributes = USB_ENDPOINT_TYPE_BULK;
    connect_device(0U);
    CHECK(pending[0][1] && pending[0][1]->transfer_buffer_length == 64U && !pending[0][0],
          "large IN or unsupported OUT endpoint exceeded buffers or disabled input");
    prepare();
    ports[0].config.intf[0].altsetting[0].ep[0].ep_desc.wMaxPacketSize = 19U;
    connect_device(0U); CHECK(!connects && !controls, "short input endpoint allocated a slot");
    prepare();
    ports[0].config.intf[0].altsetting[0].ep[0].ep_desc.bmAttributes = USB_ENDPOINT_TYPE_BULK;
    connect_device(0U); CHECK(!connects, "non-interrupt endpoint allocated a slot");
    prepare(); allocated = (1U << ONEE_INPUT_DEVICE_SLOT_COUNT) - 1U;
    connect_device(0U); CHECK(!connects && !controls && !pending[0][1], "full shared allocator still started transfers");
    puts("PASS Xbox 360 pinned class matching, endpoint validation and full shared allocator");
}

int main(void)
{
    test_decode();
    test_retries();
    test_slots_and_disconnect();
    test_endpoints_and_matching();
    prepare();
    puts("PASS all Xbox 360 transport checks");
    return 0;
}
