/* Runtime checks use the production driver, descriptor types, and URB helpers.
 * Only the host controller, clock, UART, and gamepad consumers are replaced. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "usbh_core.h"
#include "usb_xbox_one.h"
#include "usb_gamepad_input.h"

extern const struct usbh_class_info usb_xbox_one_class_info;

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, message); \
        exit(1); \
    } \
} while (0)

static uint32_t now_ms;
static struct usbh_hubport port;
static struct usbh_bus bus;
static struct usbh_hub hub;
static struct usb_setup_packet setup;
static struct usbh_urb *pending[2];
static unsigned submit_count[2];
static unsigned reject_count[2];
static int reject_result[2];
static unsigned halted[2];
static unsigned clear_calls[2];
static unsigned clear_fail_count[2];
static int clear_fail_result;
static unsigned control_timeout_max;
static unsigned control_notes;
static unsigned complete_out_during_clear;
static unsigned input_during_clear;
static unsigned in_callback;
static unsigned kills;
static unsigned connects;
static unsigned disconnects;
static unsigned reports;
static int allocated_slot = 3;
static uint8_t last_slot;
static uint8_t last_hat;
static uint8_t last_extra;
static onee_input_joystick_report_t last_report;
static uint8_t pending_out_copy[64];
static uint32_t pending_out_length;
static char diagnostic_log[32768];
static size_t diagnostic_log_length;
static const uint8_t hello[32] = {0x02, 0x20, 0x01, 28};
static const uint8_t valid_input[18] = {0x20, 0x00, 0x01, 14, 0x10};
static const uint8_t clone_identify[] = {0x04, 0x20, 0x01, 0x00};
static const uint8_t clone_serial_ack[] = {
    0x01, 0x20, 0x01, 0x09, 0x00, 0x1e, 0x20, 0x10,
    0x00, 0x00, 0x00, 0x00, 0x00
};

static void complete(unsigned direction, const uint8_t *data, int length);

static void clear_diagnostic_log(void)
{
    diagnostic_log[0] = '\0';
    diagnostic_log_length = 0U;
}

static unsigned diagnostic_count(const char *text)
{
    unsigned count = 0U;
    const size_t length = strlen(text);
    const char *cursor = diagnostic_log;
    while ((cursor = strstr(cursor, text)) != NULL) {
        ++count;
        cursor += length;
    }
    return count;
}

static void check_snapshot_data(const char *expected)
{
    const char *data = strstr(diagnostic_log, "data=");
    CHECK(data != NULL, "receive diagnostic omitted packet bytes");
    data += strlen("data=");
    const size_t length = strcspn(data, "\r\n");
    CHECK(length == strlen(expected) && memcmp(data, expected, length) == 0,
          "receive snapshot contained wrong bytes or a stale packet tail");
}

int usbh_set_interface(struct usbh_hubport *hport, uint8_t interface,
                        uint8_t alternate)
{
    CHECK(hport == &port && interface == 1U && alternate == 0U,
          "driver changed an unexpected alternate interface");
    return 0;
}

int usb_osal_mutex_take(usb_osal_mutex_t mutex)
{
    (void)mutex;
    return 0;
}

int usb_osal_mutex_give(usb_osal_mutex_t mutex)
{
    (void)mutex;
    return 0;
}

void cherryusb_host_debug_note_control(const struct usbh_hubport *hport,
                                        const struct usb_setup_packet *request, int result)
{
    (void)result;
    CHECK(hport == &port && request == port.setup, "wrong control diagnostic");
    ++control_notes;
}

static int clear_halt(struct usbh_urb *urb)
{
    struct usbh_hubport *hport = urb->hport;
    struct usb_setup_packet *request = urb->setup;
    unsigned direction = (request->wIndex & 0x80U) != 0U;
    CHECK(in_callback == 0U, "halt clear blocked an interrupt completion callback");
    CHECK(hport == &port && request == port.setup && urb->transfer_buffer == NULL,
          "halt clear did not use the device's control endpoint setup");
    CHECK(urb == &port.ep0_urb && urb->timeout > 0U && urb->timeout <= 100U &&
          urb->complete == NULL, "halt clear exceeded its bounded control wait");
    if (urb->timeout > control_timeout_max) {
        control_timeout_max = urb->timeout;
    }
    CHECK(request->bmRequestType == 0x02U && request->bRequest == 0x01U &&
          request->wValue == 0U && request->wLength == 0U &&
          request->wIndex == port.config.intf[0].altsetting[0].
              ep[direction ? 0U : 1U].ep_desc.bEndpointAddress,
          "wrong CLEAR_FEATURE(ENDPOINT_HALT) request");
    CHECK(halted[direction] != 0U, "halt clear sent to an endpoint without STALL");
    ++clear_calls[direction];
    if (clear_fail_count[direction] != 0U) {
        --clear_fail_count[direction];
        if (clear_fail_result == -USB_ERR_TIMEOUT) {
            now_ms += urb->timeout;
        }
        return clear_fail_result;
    }
    if (complete_out_during_clear != 0U) {
        complete_out_during_clear = 0U;
        now_ms += 25U;
        complete(0U, NULL, (int)pending_out_length);
    }
    if (input_during_clear != 0U) {
        input_during_clear = 0U;
        complete(1U, valid_input, sizeof(valid_input));
    }
    halted[direction] = 0U;
    return 0;
}

uint32_t cherryusb_baremetal_ms(void)
{
    return now_ms;
}

int cherryusb_printf(const char *format, ...)
{
    va_list args;
    const size_t available = sizeof(diagnostic_log) - diagnostic_log_length;
    va_start(args, format);
    const int result = vsnprintf(diagnostic_log + diagnostic_log_length,
                                 available, format, args);
    va_end(args);
    CHECK(result >= 0 && (size_t)result < available,
          "bounded fixture diagnostic log overflowed");
    diagnostic_log_length += (size_t)result;
    return result;
}

void uart_puts(uint32_t base, const char *text)
{
    (void)base;
    (void)cherryusb_printf("%s", text);
}

int usb_gamepad_input_connect(void)
{
    ++connects;
    return allocated_slot;
}

void usb_gamepad_input_disconnect(uint8_t slot)
{
    CHECK(slot == (uint8_t)allocated_slot, "disconnect released another device slot");
    ++disconnects;
}

void usb_gamepad_input_report(uint8_t slot,
                              const onee_input_joystick_report_t *report,
                              uint8_t hat, uint8_t extra_active)
{
    CHECK(slot == (uint8_t)allocated_slot, "report reached another device slot");
    ++reports;
    last_slot = slot;
    last_report = *report;
    last_hat = hat;
    last_extra = extra_active;
}

int usbh_submit_urb(struct usbh_urb *urb)
{
    if (urb->ep == &port.ep0) {
        return clear_halt(urb);
    }
    unsigned direction = (urb->ep->bEndpointAddress & 0x80U) != 0U;
    CHECK(urb->timeout == 0U, "driver used a blocking transfer");
    CHECK(urb->complete != NULL, "async transfer has no callback");
    CHECK(pending[direction] == NULL, "same endpoint submitted twice");
    CHECK(halted[direction] == 0U, "endpoint rearmed before its halt was cleared");
    CHECK(((uintptr_t)urb->transfer_buffer % CONFIG_USB_ALIGN_SIZE) == 0U,
          "DMA transfer buffer is not cache-line aligned");
    ++submit_count[direction];
    if (reject_count[direction] != 0U) {
        --reject_count[direction];
        return reject_result[direction];
    }
    pending[direction] = urb;
    urb->hcpriv = urb;
    if (direction == 0U) {
        CHECK(urb->transfer_buffer_length <= sizeof(pending_out_copy),
              "unexpectedly large output packet");
        pending_out_length = urb->transfer_buffer_length;
        memcpy(pending_out_copy, urb->transfer_buffer, pending_out_length);
    }
    return 0;
}

int usbh_kill_urb(struct usbh_urb *urb)
{
    unsigned direction;
    for (direction = 0U; direction < 2U; ++direction) {
        if (pending[direction] == urb) {
            pending[direction] = NULL;
            urb->hcpriv = NULL;
            ++kills;
            /* A completion during cancellation must not revive the device. */
            ++in_callback;
            urb->complete(urb->arg, -USB_ERR_SHUTDOWN);
            --in_callback;
        }
    }
    return 0;
}

static void tick(uint32_t milliseconds)
{
    now_ms += milliseconds;
    usb_xbox_one_poll();
}

static void complete(unsigned direction, const uint8_t *data, int length)
{
    struct usbh_urb *urb = pending[direction];
    CHECK(urb != NULL, "no pending transfer to complete");
    if (direction == 0U) {
        CHECK(memcmp(urb->transfer_buffer, pending_out_copy,
                     pending_out_length) == 0,
              "pending output buffer changed before completion");
    } else if (length > 0) {
        CHECK((uint32_t)length <= urb->transfer_buffer_length,
              "fixture packet exceeds the receive buffer");
        memcpy(urb->transfer_buffer, data, (size_t)length);
    }
    pending[direction] = NULL;
    urb->hcpriv = NULL;
    urb->actual_length = length > 0 ? (uint32_t)length : 0U;
    if (length == -USB_ERR_STALL) {
        halted[direction] = 1U;
        urb->data_toggle = 1U;
    }
    ++in_callback;
    urb->complete(urb->arg, length);
    --in_callback;
}

static void receive(const uint8_t *data, int length)
{
    if (pending[1] == NULL) {
        tick(20U);
    }
    complete(1U, data, length);
    tick(0U);
}

static void prepare(void)
{
    usb_xbox_one_stop();
    clear_diagnostic_log();
    memset(&port, 0, sizeof(port));
    memset(&bus, 0, sizeof(bus));
    memset(&hub, 0, sizeof(hub));
    memset(&setup, 0, sizeof(setup));
    memset(pending, 0, sizeof(pending));
    memset(submit_count, 0, sizeof(submit_count));
    memset(reject_count, 0, sizeof(reject_count));
    memset(reject_result, 0, sizeof(reject_result));
    memset(halted, 0, sizeof(halted));
    memset(clear_calls, 0, sizeof(clear_calls));
    memset(clear_fail_count, 0, sizeof(clear_fail_count));
    clear_fail_result = -USB_ERR_IO;
    control_timeout_max = control_notes = 0U;
    complete_out_during_clear = input_during_clear = 0U;
    memset(&last_report, 0, sizeof(last_report));
    now_ms = 100U;
    connects = disconnects = reports = kills = 0U;
    allocated_slot = 3;
    port.connected = true;
    port.speed = USB_SPEED_FULL;
    port.port = 3U;
    port.bus = &bus;
    port.parent = &hub;
    port.setup = &setup;
    hub.index = 2U;
    port.device_desc.idVendor = 0x045eU;
    port.device_desc.idProduct = 0x02eaU;
    port.config.config_desc.bNumInterfaces = 3U;
    port.config.intf[0].altsetting_num = 1U;
    struct usbh_interface_altsetting *alt = &port.config.intf[0].altsetting[0];
    alt->intf_desc.bInterfaceNumber = 0U;
    alt->intf_desc.bInterfaceClass = 0xffU;
    alt->intf_desc.bInterfaceSubClass = 0x47U;
    alt->intf_desc.bInterfaceProtocol = 0xd0U;
    alt->intf_desc.bNumEndpoints = 2U;
    alt->ep[0].ep_desc.bEndpointAddress = 0x81U;
    alt->ep[1].ep_desc.bEndpointAddress = 0x01U;
    for (unsigned i = 0; i < 2U; ++i) {
        alt->ep[i].ep_desc.bmAttributes = USB_ENDPOINT_TYPE_INTERRUPT;
        alt->ep[i].ep_desc.wMaxPacketSize = 64U;
        alt->ep[i].ep_desc.bInterval = 4U;
    }
}

static int connect_raw_device(uint8_t interface)
{
    return usb_xbox_one_class_info.class_driver->connect(&port, interface);
}

static int connect_device(uint8_t interface)
{
    const int result = connect_raw_device(interface);
    if (result == 0 && pending[1] != NULL) {
        receive(hello, sizeof(hello));
    }
    return result;
}

static void disconnect_device(void)
{
    port.connected = false;
    CHECK(usb_xbox_one_class_info.class_driver->disconnect(&port, 0U) == 0,
          "disconnect failed");
    CHECK(pending[0] == NULL && pending[1] == NULL,
          "disconnect left a DMA transfer active");
    CHECK(port.config.intf[0].priv == NULL, "disconnect left interface attached");
}

static uint32_t finish_startup(void)
{
    static const uint8_t expected[4][7] = {
        {0x05, 0x20, 0, 0x01, 0x00},
        {0x05, 0x20, 0, 0x0f, 0x06},
        {0x0a, 0x20, 0, 0x03, 0x00, 0x01, 0x14},
        {0x06, 0x20, 0, 0x02, 0x01, 0x00},
    };
    static const uint8_t lengths[] = {5, 5, 7, 6};
    uint8_t sequence = 0;
    for (unsigned i = 0; i < 4U; ++i) {
        tick(20U);
        CHECK(pending[1] != NULL, "startup blocked the receive endpoint");
        CHECK(pending[0] != NULL, "startup did not submit the next output");
        if (i == 0U) {
            sequence = pending_out_copy[2];
            CHECK(sequence == 1U, "startup did not begin with sequence one");
        }
        CHECK(pending_out_length == lengths[i], "wrong startup packet length");
        CHECK(pending_out_copy[2] == (uint8_t)(sequence + i),
              "startup sequence did not advance");
        for (unsigned j = 0; j < lengths[i]; ++j) {
            CHECK(j == 2U || pending_out_copy[j] == expected[i][j],
                  "wrong startup packet or startup order");
        }
        complete(0U, NULL, lengths[i]);
    }
    const uint32_t completed_at = now_ms;
    tick(20U);
    CHECK(pending[0] == NULL, "startup repeats after completion");
    return completed_at;
}

static void check_output(const uint8_t *bytes, size_t size)
{
    CHECK(pending[0] != NULL && pending_out_length == size &&
          memcmp(pending_out_copy, bytes, size) == 0,
          "wrong output packet bytes or fixed sequence");
}

static void finish_clone_fallback(void)
{
    check_output(clone_identify, sizeof(clone_identify));
    complete(0U, NULL, sizeof(clone_identify));
    tick(0U);
    check_output(clone_serial_ack, sizeof(clone_serial_ack));
    complete(0U, NULL, sizeof(clone_serial_ack));
    tick(0U);
    CHECK(pending[0] == NULL, "clone fallback repeated after completion");
}

static void test_scope_and_endpoints(void)
{
    const struct usbh_class_info *info = &usb_xbox_one_class_info;
    unsigned flags = USB_CLASS_MATCH_VID_PID | USB_CLASS_MATCH_INTF_CLASS |
        USB_CLASS_MATCH_INTF_SUBCLASS | USB_CLASS_MATCH_INTF_PROTOCOL |
        USB_CLASS_MATCH_INTF_NUM;
    CHECK((info->match_flags & flags) == flags, "class registry is too broad");
    CHECK(info->bInterfaceClass == 0xffU && info->bInterfaceSubClass == 0x47U &&
          info->bInterfaceProtocol == 0xd0U && info->bInterfaceNumber == 0U,
          "class registry does not isolate the gamepad interface");
    CHECK(info->id_table != NULL && info->id_table[0][0] == 0x045eU &&
          info->id_table[0][1] == 0x02eaU && info->id_table[1][0] == 0U,
          "class registry must only claim the verified controller ID");
    for (unsigned mode = 0U; mode < 8U; ++mode) {
        prepare();
        struct usbh_interface_altsetting *alt = &port.config.intf[0].altsetting[0];
        uint8_t interface = 0U;
        switch (mode) {
        case 0: interface = 1U; break;
        case 1: alt->intf_desc.bNumEndpoints = CONFIG_USBHOST_MAX_ENDPOINTS + 1U; break;
        case 2: alt->ep[1].ep_desc.bEndpointAddress = 0x82U; break;
        case 3: alt->intf_desc.bNumEndpoints = 1U; break;
        case 4: alt->ep[1].ep_desc.bmAttributes = USB_ENDPOINT_TYPE_BULK; break;
        case 5: alt->ep[0].ep_desc.wMaxPacketSize = 0U; break;
        case 6: alt->ep[0].ep_desc.bInterval = 0U; break;
        default: alt->ep[0].ep_desc.wMaxPacketSize = 128U; break;
        }
        CHECK(connect_device(interface) < 0, "invalid device or endpoints accepted");
        CHECK(connects == 0U && pending[0] == NULL && pending[1] == NULL,
              "rejected interface allocated input or transfers");
    }
    prepare();
    allocated_slot = -1;
    CHECK(connect_device(0U) < 0, "full shared gamepad slots were ignored");
    CHECK(pending[0] == NULL && pending[1] == NULL,
          "slot allocation failure started I/O");

    prepare();
    struct usbh_interface_altsetting *alt = &port.config.intf[0].altsetting[0];
    alt->ep[0].ep_desc.bEndpointAddress = 0x82U;
    alt->ep[1].ep_desc.bEndpointAddress = 0x02U;
    CHECK(connect_device(0U) == 0, "observed 0x82/0x02 endpoints were rejected");
    CHECK(pending[1] != NULL && pending[1]->ep->bEndpointAddress == 0x82U &&
          pending[0] != NULL && pending[0]->ep->bEndpointAddress == 0x02U,
          "driver hard-coded endpoint one instead of using descriptors");
    finish_startup();
    complete(1U, NULL, -USB_ERR_STALL);
    tick(10U);
    CHECK(clear_calls[1] == 1U && pending[1] != NULL &&
          pending[1]->ep->bEndpointAddress == 0x82U,
          "halt recovery did not preserve the observed IN endpoint");
    disconnect_device();
    puts("PASS class scope and endpoint validation");
}

static void test_hello_gates_startup(void)
{
    prepare();
    CHECK(connect_raw_device(0U) == 0, "valid device did not connect");
    CHECK(pending[1] != NULL && pending[0] == NULL && submit_count[0] == 0U,
          "driver sent startup before the controller hello");
    tick(10000U);
    CHECK(pending[1] != NULL && pending[0] == NULL && submit_count[0] == 0U &&
          disconnects == 0U && control_notes == 0U,
          "quiet controller timed out or received speculative startup output");

    for (unsigned mode = 0U; mode < 11U; ++mode) {
        uint8_t packet[sizeof(hello)];
        memcpy(packet, hello, sizeof(packet));
        int length = sizeof(packet);
        switch (mode) {
        case 0: length = 3; break;
        case 1: length = 31; break;
        case 2: packet[3] = 27U; break;
        case 3: packet[3] = 29U; break;
        case 4: packet[1] = 0x21U; break;
        case 5: packet[1] = 0U; break;
        case 6: packet[1] = 0x60U; break;
        case 7: packet[1] = 0xa0U; break;
        case 8: packet[0] = 0x03U; break;
        case 9: packet[0] = 0xffU; break;
        default: packet[2] = 0U; break;
        }
        receive(packet, length);
        tick(100U);
        CHECK(pending[1] != NULL && pending[0] == NULL && submit_count[0] == 0U,
              "malformed or unrelated hello started output");
    }
    const uint8_t guide[] = {0x07, 0x30, 0x35, 2, 1, 0};
    const uint8_t input[18] = {0x20, 0x00, 0x01, 14, 0x10};
    receive(guide, sizeof(guide));
    receive(input, sizeof(input));
    CHECK(pending[1] != NULL && pending[0] == NULL && submit_count[0] == 0U &&
          reports == 0U,
          "non-hello packet bypassed the startup gate");
    disconnect_device();

    prepare();
    CHECK(connect_raw_device(0U) == 0, "valid device did not reconnect");
    uint8_t padded_hello[40] = {0};
    memcpy(padded_hello, hello, sizeof(hello));
    receive(padded_hello, sizeof(padded_hello));
    CHECK(pending[0] != NULL && pending_out_copy[2] == 1U,
          "valid hello did not start sequence one");
    for (unsigned step = 0U; step < 4U; ++step) {
        uint8_t saved[sizeof(pending_out_copy)];
        const uint32_t length = pending_out_length;
        const unsigned submissions = submit_count[0];
        struct usbh_urb *active_output = pending[0];
        memcpy(saved, pending_out_copy, length);
        CHECK(saved[2] == step + 1U, "hello advanced startup sequence twice");
        receive(hello, sizeof(hello));
        receive(hello, sizeof(hello));
        CHECK(pending[0] == active_output && pending_out_length == length &&
              submit_count[0] == submissions &&
              memcmp(pending[0]->transfer_buffer, saved, length) == 0,
              "repeated hello changed or duplicated an active startup transfer");
        complete(0U, NULL, (int)length);
        tick(20U);
    }
    CHECK(pending[0] == NULL && submit_count[0] == 4U,
          "repeated hello caused duplicate startup commands");
    receive(hello, sizeof(hello));
    padded_hello[2] = 2U;
    receive(padded_hello, sizeof(padded_hello));
    tick(900U);
    CHECK(pending[0] == NULL && pending[1] != NULL && submit_count[0] == 4U &&
          disconnects == 0U,
          "hello after startup restarted commands or stopped input");
    disconnect_device();
    puts("PASS hello-gated startup, malformed hellos, and active-transfer stability");
}

static void test_clone_fallback_deadline(void)
{
    for (unsigned wrap = 0U; wrap < 2U; ++wrap) {
        prepare();
        if (wrap != 0U) {
            now_ms = UINT32_MAX - 2000U;
        }
        CHECK(connect_device(0U) == 0, "valid device did not connect");
        /* Enumeration/startup can take more than one second. The fallback
         * must wait for the last normal command, including across wrap. */
        for (unsigned step = 0U; step < 4U; ++step) {
            tick(400U);
            CHECK(pending[0] != NULL && pending_out_copy[2] == step + 1U,
                  "fallback interrupted normal startup");
            complete(0U, NULL, (int)pending_out_length);
            tick(0U);
        }
        CHECK(pending[0] == NULL && submit_count[0] == 4U,
              "fallback used connection time instead of completed startup");
        tick(999U);
        receive(hello, sizeof(hello));
        CHECK(pending[0] == NULL && submit_count[0] == 4U,
              "fallback ran before its deadline or a duplicate Hello retriggered it");
        tick(1U);
        check_output(clone_identify, sizeof(clone_identify));
        CHECK(diagnostic_count("clone fallback starting") == 1U,
              "fallback start was not logged once");
        receive(hello, sizeof(hello));
        finish_clone_fallback();
        CHECK(submit_count[0] == 6U && pending[1] != NULL &&
              diagnostic_count("clone fallback sent") == 1U,
              "fallback did not finish exactly once while keeping input live");
        tick(10000U);
        receive(hello, sizeof(hello));
        tick(10000U);
        CHECK(submit_count[0] == 6U && pending[0] == NULL &&
              diagnostic_count("clone fallback starting") == 1U &&
              diagnostic_count("clone fallback sent") == 1U,
              "silence or duplicate Hello repeated the clone fallback");
        clear_diagnostic_log();
        usb_xbox_one_dump_status(0U);
        CHECK(strstr(diagnostic_log, "init=4/4 clone=2/2") != NULL,
              "fallback changed init count or status lost clone progress");
        disconnect_device();
        port.connected = true;
        CHECK(connect_raw_device(0U) == 0 && pending[0] == NULL,
              "reconnect retained the Hello gate or fallback output");
        receive(hello, sizeof(hello));
        const uint32_t completed_at = finish_startup();
        tick(1000U - (uint32_t)(now_ms - completed_at));
        finish_clone_fallback();
        disconnect_device();
    }
    puts("PASS one-shot clone fallback, startup-based deadline, clock wrap, and reconnect");
}

static void test_clone_input_suppression(void)
{
    for (unsigned phase = 0U; phase < 4U; ++phase) {
        prepare();
        CHECK(connect_device(0U) == 0, "valid device did not connect");
        const uint32_t completed_at = finish_startup();
        if (phase == 0U) {
            receive(valid_input, sizeof(valid_input));
        } else {
            tick(1000U - (uint32_t)(now_ms - completed_at));
            check_output(clone_identify, sizeof(clone_identify));
            if (phase == 1U) {
                /* First fallback packet remains owned by the controller. */
                receive(valid_input, sizeof(valid_input));
                check_output(clone_identify, sizeof(clone_identify));
                complete(0U, NULL, sizeof(clone_identify));
            } else {
                complete(0U, NULL, sizeof(clone_identify));
                if (phase == 3U) {
                    /* The second packet is prepared but rejected by HCD. */
                    reject_count[0] = 1U;
                    reject_result[0] = -USB_ERR_NOMEM;
                    tick(0U);
                    CHECK(pending[0] == NULL, "fallback rejection was ignored");
                }
                receive(valid_input, sizeof(valid_input));
            }
        }
        tick(10U);
        const unsigned attempts = submit_count[0];
        tick(10000U);
        receive(hello, sizeof(hello));
        CHECK(reports == 1U && pending[0] == NULL && pending[1] != NULL &&
              submit_count[0] == attempts && kills == 0U && disconnects == 0U,
              "valid input did not suppress unsent fallback without cancellation");
        CHECK(diagnostic_count("clone fallback starting") == (phase == 0U ? 0U : 1U) &&
              diagnostic_count("clone fallback sent") == 0U,
              "suppressed fallback reported completion or restarted");
        disconnect_device();
    }
    puts("PASS input suppresses unsent clone fallback and preserves active DMA");
}

static void test_clone_ack_and_retries(void)
{
    const uint8_t guide[] = {0x07, 0x30, 0x01, 2, 1, 0};
    const uint8_t guide_ack[] = {
        0x01, 0x20, 0x01, 0x09, 0x00, 0x07, 0x20, 0x02,
        0x00, 0x00, 0x00, 0x00, 0x00
    };
    prepare();
    CHECK(connect_device(0U) == 0, "valid device did not connect");
    const uint32_t completed_at = finish_startup();
    reject_count[0] = 1U;
    reject_result[0] = -USB_ERR_NOMEM;
    tick(1000U - (uint32_t)(now_ms - completed_at));
    CHECK(pending[0] == NULL, "clone allocation rejection was ignored");
    const unsigned attempts = submit_count[0];
    tick(9U);
    CHECK(submit_count[0] == attempts, "clone retry bypassed backoff");
    tick(1U);
    check_output(clone_identify, sizeof(clone_identify));
    receive(guide, sizeof(guide));
    check_output(clone_identify, sizeof(clone_identify));
    complete(0U, NULL, sizeof(clone_identify));
    tick(0U);
    check_output(guide_ack, sizeof(guide_ack));
    complete(0U, NULL, sizeof(guide_ack));
    tick(0U);
    check_output(clone_serial_ack, sizeof(clone_serial_ack));
    /* A Guide ACK with the same sequence is distinct from the clone ACK. */
    receive(guide, sizeof(guide));
    complete(0U, NULL, 1);
    tick(10U);
    check_output(clone_serial_ack, sizeof(clone_serial_ack));
    complete(0U, NULL, -USB_ERR_STALL);
    tick(10U);
    CHECK(clear_calls[0] == 1U, "clone STALL did not use bounded halt recovery");
    check_output(clone_serial_ack, sizeof(clone_serial_ack));
    complete(0U, NULL, sizeof(clone_serial_ack));
    tick(0U);
    check_output(guide_ack, sizeof(guide_ack));
    complete(0U, NULL, sizeof(guide_ack));
    tick(10000U);
    CHECK(pending[0] == NULL && pending[1] != NULL &&
          diagnostic_count("clone fallback starting") == 1U &&
          diagnostic_count("clone fallback sent") == 1U,
          "ACK interleaving or retries restarted clone startup");
    clear_diagnostic_log();
    usb_xbox_one_dump_status(0U);
    CHECK(strstr(diagnostic_log, "init=4/4 clone=2/2") != NULL,
          "Guide ACK or retry changed startup counters");
    disconnect_device();
    puts("PASS clone packet retries, Guide ACK interleaving, and distinct progress");
}

static void test_clone_failure_and_unplug(void)
{
    for (unsigned permanent = 0U; permanent < 2U; ++permanent) {
        prepare();
        CHECK(connect_device(0U) == 0, "valid device did not connect");
        const uint32_t completed_at = finish_startup();
        tick(1000U - (uint32_t)(now_ms - completed_at));
        check_output(clone_identify, sizeof(clone_identify));
        if (permanent != 0U) {
            for (unsigned failure = 0U; failure < 8U; ++failure) {
                check_output(clone_identify, sizeof(clone_identify));
                complete(0U, NULL, -USB_ERR_IO);
                tick(10U);
            }
            CHECK(disconnects == 1U && pending[0] == NULL && pending[1] == NULL,
                  "clone fallback escaped the existing output failure budget");
        }
        disconnect_device();
        const unsigned attempts = submit_count[0];
        tick(10000U);
        CHECK(disconnects == 1U && pending[0] == NULL && pending[1] == NULL &&
              submit_count[0] == attempts,
              "unplug or failure revived clone output or released input twice");
    }
    puts("PASS bounded clone transport failure and active fallback unplug");
}

static void test_clone_control_pump(void)
{
    prepare();
    CHECK(connect_device(0U) == 0, "valid device did not connect");
    for (unsigned step = 0U; step < 3U; ++step) {
        complete(0U, NULL, (int)pending_out_length);
        tick(0U);
    }
    complete(1U, NULL, -USB_ERR_STALL);
    complete_out_during_clear = 1U;
    tick(10U);
    CHECK(clear_calls[1] == 1U && submit_count[0] == 4U && pending[0] == NULL,
          "stale poll time triggered fallback during final startup completion");
    tick(999U);
    CHECK(pending[0] == NULL, "pumped completion shortened the fallback deadline");
    tick(1U);
    check_output(clone_identify, sizeof(clone_identify));
    complete(0U, NULL, -USB_ERR_STALL);
    input_during_clear = 1U;
    const unsigned attempts = submit_count[0];
    tick(10U);
    CHECK(clear_calls[0] == 1U && reports == 1U && pending[0] == NULL &&
          submit_count[0] == attempts,
          "input during OUT halt recovery did not suppress unsent fallback");
    tick(10000U);
    CHECK(submit_count[0] == attempts && pending[1] != NULL,
          "suppressed fallback restarted after the control transfer");
    disconnect_device();
    puts("PASS clone deadline and input suppression across control-transfer callbacks");
}

static void test_input_and_ack(void)
{
    /* Menu/View/A/B/X/Y, up-right/LB/RB, LT=512, RT=1023,
     * LX=12345, LY=-32768, RX=-12345, RY=32767. */
    uint8_t data[18] = {0x20, 0x00, 0x17, 14, 0xfc, 0x39,
        0x00, 0x02, 0xff, 0x03, 0x39, 0x30, 0x00, 0x80,
        0xc7, 0xcf, 0xff, 0x7f};
    uint8_t guide[] = {0x07, 0x30, 0x5a, 0x02, 0x01, 0x00};
    static const uint8_t ack[] = {0x01, 0x20, 0x5a, 0x09, 0x00, 0x07,
        0x20, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00};
    prepare();
    CHECK(connect_device(0U) == 0, "valid device did not connect");
    finish_startup();
    receive(data, sizeof(data));
    CHECK(reports >= 1U && last_slot == 3U, "input never reached shared bridge");
    CHECK(last_report.axis_valid_mask == 0x3fU && last_report.buttons_valid != 0U,
          "report does not expose all six axes and buttons");
    CHECK(last_report.buttons == 0xffU && last_hat == 1U && last_extra == 0U,
          "buttons or diagonal D-pad mapped incorrectly");
    CHECK(last_report.axis[ONEE_INPUT_AXIS_X] == 12345 &&
          last_report.axis[ONEE_INPUT_AXIS_Y] == 32767 &&
          last_report.axis[ONEE_INPUT_AXIS_RX] == -12345 &&
          last_report.axis[ONEE_INPUT_AXIS_RY] == -32768 &&
          last_report.axis[ONEE_INPUT_AXIS_Z] == 512 &&
          last_report.axis[ONEE_INPUT_AXIS_RZ] == 1023,
          "stick signs, Y inversion, or triggers mapped incorrectly");
    for (unsigned axis = 0; axis < ONEE_INPUT_AXIS_COUNT; ++axis) {
        unsigned trigger = axis == ONEE_INPUT_AXIS_Z || axis == ONEE_INPUT_AXIS_RZ;
        CHECK(last_report.logical_min[axis] == (trigger ? 0 : -32768) &&
              last_report.logical_max[axis] == (trigger ? 1023 : 32767),
              "axis ranges do not match wire values");
    }
    onee_input_joystick_report_t saved = last_report;
    receive(guide, sizeof(guide));
    CHECK(pending[0] != NULL && pending_out_length == sizeof(ack) &&
          memcmp(pending_out_copy, ack, sizeof(ack)) == 0,
          "guide ACK bytes or echoed sequence are wrong");
    CHECK(memcmp(&saved, &last_report, sizeof(saved)) == 0 && last_extra != 0U,
          "guide press lost the main input state");
    /* Incoming input remains live while an ACK owns the output buffer. */
    data[4] = 0x10U;
    data[5] = 0xc0U;
    receive(data, sizeof(data));
    CHECK(last_report.buttons == 1U && last_hat == 8U && last_extra != 0U,
          "input stalled behind output or stick-click activity was lost");
    complete(0U, NULL, sizeof(ack));
    tick(0U);
    CHECK(pending[0] == NULL, "completed ACK was sent twice");
    guide[1] = 0x20U;
    guide[4] = 0U;
    receive(guide, sizeof(guide));
    CHECK(pending[0] == NULL, "guide message without request bit was ACKed");
    CHECK(last_extra != 0U, "guide release lost held stick-click state");
    data[5] = 0U;
    receive(data, sizeof(data));
    CHECK(last_extra == 0U, "release did not clear extra button activity");
    disconnect_device();
    CHECK(disconnects == 1U, "disconnect did not release shared slot");
    puts("PASS report mapping, guide ACK, and independent IN/OUT");
}

static void test_malformed_reports(void)
{
    const uint8_t good[] = {0x20, 0x00, 0x01, 14, 0x10, 0x00,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    prepare();
    CHECK(connect_device(0U) == 0, "valid device did not connect");
    finish_startup();
    receive(good, sizeof(good));
    unsigned count = reports;
    for (unsigned i = 0; i < 9U; ++i) {
        uint8_t data[18];
        memcpy(data, good, sizeof(data));
        int length = sizeof(data);
        switch (i) {
        case 0: length = 3; break;
        case 1: length = 17; break;
        case 2: data[3] = 13U; break;
        case 3: data[3] = 15U; break;
        case 4: data[1] = 1U; break;
        case 5: data[1] = 0x40U; break;
        case 6: data[1] = 0x80U; break;
        case 7: data[3] = 0x8eU; break;
        default: data[0] = 0xffU; break;
        }
        receive(data, length);
        CHECK(reports == count && last_report.buttons == 1U,
              "malformed report changed input state");
        CHECK(pending[0] == NULL, "malformed report produced output");
    }
    uint8_t guide[] = {0x07, 0x30, 0x7f, 0x02, 0x01};
    receive(guide, sizeof(guide));
    CHECK(reports == count && pending[0] == NULL,
          "truncated guide packet was accepted or ACKed");
    disconnect_device();
    puts("PASS malformed and truncated packet rejection");
}

static void test_retry_and_disconnect(void)
{
    prepare();
    reject_count[1] = 1U;
    reject_result[1] = -USB_ERR_NOMEM;
    CHECK(connect_device(0U) == 0, "temporary IN allocation failure rejected device");
    CHECK(pending[1] == NULL && pending[0] == NULL,
          "startup ran without a receive transfer");
    unsigned attempts = submit_count[1];
    tick(0U);
    CHECK(submit_count[1] == attempts, "NOMEM causes immediate retry spin");
    tick(10U);
    CHECK(pending[1] != NULL, "receive did not retry after NOMEM");
    receive(hello, sizeof(hello));
    finish_startup();
    complete(1U, NULL, 0);
    attempts = submit_count[1];
    tick(0U);
    CHECK(submit_count[1] == attempts, "empty completions cause retry spin");
    tick(10U);
    CHECK(pending[1] != NULL, "empty completion stopped receiving");
    complete(1U, NULL, -USB_ERR_IO);
    tick(10U);
    CHECK(pending[1] != NULL, "transient IN failure stopped receiving");
    uint8_t guide[] = {0x07, 0x30, 0x11, 2, 1, 0};
    reject_count[0] = 1U;
    reject_result[0] = -USB_ERR_NOMEM;
    receive(guide, sizeof(guide));
    CHECK(pending[0] == NULL, "output rejection was ignored");
    attempts = submit_count[0];
    tick(0U);
    CHECK(submit_count[0] == attempts, "OUT NOMEM causes immediate retry spin");
    tick(10U);
    CHECK(pending[0] != NULL && pending_out_copy[2] == 0x11U,
          "ACK lost after NOMEM retry");
    CHECK(pending[1] != NULL, "pending OUT stopped reads");
    disconnect_device();
    CHECK(kills == 2U && disconnects == 1U, "disconnect did not stop both transfers");
    attempts = submit_count[0] + submit_count[1];
    tick(100U);
    CHECK(submit_count[0] + submit_count[1] == attempts,
          "disconnected device restarted a transfer");
    usb_xbox_one_stop();
    CHECK(disconnects == 1U, "stop released the same shared slot twice");
    prepare();
    CHECK(connect_device(0U) == 0, "device could not reconnect");
    finish_startup();
    disconnect_device();
    puts("PASS NOMEM backoff, empty/error completion, and teardown");
}

static void test_output_completion_and_timeout(void)
{
    uint8_t first[64];
    uint32_t length;
    prepare();
    CHECK(connect_device(0U) == 0, "valid device did not connect");
    CHECK(pending[0] != NULL, "initial output was not submitted");
    length = pending_out_length;
    memcpy(first, pending_out_copy, length);
    complete(0U, NULL, (int)length - 1);
    unsigned attempts = submit_count[0];
    tick(0U);
    CHECK(submit_count[0] == attempts, "short OUT completion spins on retry");
    tick(10U);
    CHECK(pending[0] != NULL && pending_out_length == length &&
          memcmp(first, pending_out_copy, length) == 0,
          "short OUT completion advanced startup or changed its sequence");
    complete(0U, NULL, -USB_ERR_STALL);
    tick(10U);
    CHECK(pending[0] != NULL && memcmp(first, pending_out_copy, length) == 0,
          "OUT transfer error lost the pending startup packet");
    CHECK(clear_calls[0] == 1U && pending[0]->data_toggle == 0U,
          "OUT halt was not cleared with its data toggle reset");
    tick(1000U);
    CHECK(kills == 1U && pending[0] == NULL && pending[1] != NULL,
          "timed-out output was not killed independently of input");
    attempts = submit_count[0];
    tick(0U);
    CHECK(submit_count[0] == attempts, "timeout cancellation caused retry spin");
    tick(10U);
    CHECK(pending[0] != NULL && memcmp(first, pending_out_copy, length) == 0,
          "output timeout did not retry the same packet and sequence");
    finish_startup();
    disconnect_device();
    puts("PASS short/error output completion and asynchronous timeout");
}

static void test_guide_ack_queue(void)
{
    uint8_t guide[] = {0x07, 0x30, 1, 2, 1, 0};
    prepare();
    CHECK(connect_device(0U) == 0, "valid device did not connect");
    finish_startup();
    receive(guide, sizeof(guide));
    CHECK(pending[0] != NULL && pending_out_copy[2] == 1U,
          "first guide ACK not submitted");
    receive(guide, sizeof(guide));
    for (uint8_t sequence = 2U; sequence <= 5U; ++sequence) {
        guide[2] = sequence;
        receive(guide, sizeof(guide));
    }
    guide[2] = 3U;
    receive(guide, sizeof(guide));
    for (uint8_t sequence = 1U; sequence <= 5U; ++sequence) {
        CHECK(pending[0] != NULL && pending_out_copy[2] == sequence,
              "guide ACK queue lost order or sent a duplicate");
        complete(0U, NULL, (int)pending_out_length);
        tick(0U);
    }
    CHECK(pending[0] == NULL, "guide retransmission produced redundant ACKs");
    disconnect_device();
    puts("PASS guide ACK queue order and duplicate suppression");
}

static void test_stall_recovery(void)
{
    uint8_t guide[] = {0x07, 0x30, 0x35, 2, 1, 0};
    prepare();
    CHECK(connect_device(0U) == 0, "valid device did not connect");
    finish_startup();
    clear_fail_count[1] = 1U;
    unsigned attempts = submit_count[1];
    complete(1U, NULL, -USB_ERR_STALL);
    tick(10U);
    CHECK(clear_calls[1] == 1U && pending[1] == NULL &&
          submit_count[1] == attempts,
          "failed IN halt clear still submitted an endpoint transfer");
    tick(9U);
    CHECK(clear_calls[1] == 1U, "IN halt clear failure retried without backoff");
    tick(1U);
    CHECK(clear_calls[1] == 2U && pending[1] != NULL &&
          pending[1]->data_toggle == 0U && halted[1] == 0U,
          "IN did not recover after successful halt clear");

    receive(guide, sizeof(guide));
    CHECK(pending[0] != NULL, "guide ACK was not submitted after IN recovery");
    clear_fail_count[0] = 1U;
    attempts = submit_count[0];
    complete(0U, NULL, -USB_ERR_STALL);
    tick(10U);
    CHECK(clear_calls[0] == 1U && pending[0] == NULL &&
          submit_count[0] == attempts && pending[1] != NULL,
          "failed OUT halt clear rearmed output or interrupted input");
    tick(9U);
    CHECK(clear_calls[0] == 1U, "OUT halt clear failure retried without backoff");
    tick(1U);
    CHECK(clear_calls[0] == 2U && pending[0] != NULL &&
          pending[0]->data_toggle == 0U && halted[0] == 0U &&
          pending_out_length == 13U && pending_out_copy[2] == 0x35U,
          "OUT halt recovery lost its pending ACK or data toggle reset");
    complete(0U, NULL, (int)pending_out_length);
    tick(0U);
    disconnect_device();
    puts("PASS IN/OUT halt clear, failure backoff, and data toggle reset");
}

static void test_permanent_halt_failure(void)
{
    const int failures[] = { -USB_ERR_STALL, -USB_ERR_TIMEOUT, -USB_ERR_IO };
    for (unsigned direction=0; direction<2U; ++direction) {
        for (unsigned error=0; error<sizeof(failures)/sizeof(failures[0]); ++error) {
            prepare();
            CHECK(connect_device(0U) == 0, "valid device did not connect");
            clear_fail_count[direction] = 100U;
            clear_fail_result = failures[error];
            complete(direction, NULL, -USB_ERR_STALL);
            for (unsigned i=0; i<100U; ++i) {
                tick(10U);
            }
            CHECK(clear_calls[direction] > 0U && clear_calls[direction] <= 8U,
                  "permanent halt failure caused unbounded control retries");
            CHECK(control_timeout_max <= 100U && control_notes == clear_calls[direction],
                  "recovery lost its time limit or control diagnostics");
            CHECK(disconnects == 1U && pending[0] == NULL && pending[1] == NULL,
                  "failed controller kept guest input or USB transfers active");
            unsigned attempts = submit_count[0] + submit_count[1] + control_notes;
            tick(10000U);
            CHECK(submit_count[0] + submit_count[1] + control_notes == attempts,
                  "stopped controller restarted itself");
            disconnect_device();
            CHECK(disconnects == 1U, "unplug released an already freed input slot");
            port.connected = true;
            halted[direction] = 0U; /* USB reconnect resets endpoint halt. */
            clear_fail_count[direction] = 0U;
            CHECK(connect_device(0U) == 0, "controller could not reconnect after failure");
            finish_startup();
            disconnect_device();
        }
    }
    puts("PASS bounded permanent STALL/timeout/IO recovery, input release, and reconnect");
}

static void test_transport_failures(void)
{
    prepare();
    CHECK(connect_device(0U) == 0, "valid device did not connect");
    for (unsigned i=0; i<8U; ++i) {
        complete(1U, NULL, -USB_ERR_IO);
        tick(10U);
    }
    CHECK(control_notes == 0U, "transport errors caused endpoint-halt requests");
    CHECK(disconnects == 1U && pending[0] == NULL && pending[1] == NULL,
          "permanent transport failure was not contained");
    disconnect_device();

    prepare();
    CHECK(connect_device(0U) == 0, "valid device did not connect");
    finish_startup();
    const uint8_t guide[] = {0x07, 0x20, 0x35, 2, 0, 0};
    for (unsigned cycle=0; cycle<4U; ++cycle) {
        for (unsigned i=0; i<7U; ++i) {
            complete(1U, NULL, -USB_ERR_IO);
            tick(10U);
        }
        receive(guide, sizeof(guide));
        CHECK(disconnects == 0U, "successful input did not reset failure budget");
    }
    disconnect_device();
    puts("PASS transport failure limit and reset after successful input");
}

static void test_output_failure_with_live_input(void)
{
    const uint8_t guide[] = {0x07, 0x20, 0x35, 2, 1, 0};
    prepare();
    CHECK(connect_device(0U) == 0, "valid device did not connect");
    usbh_complete_callback_t late_complete = pending[1]->complete;
    void *late_arg = pending[1]->arg;
    for (unsigned i=0; i<8U; ++i) {
        receive(guide, sizeof(guide));
        CHECK(last_extra == 1U, "Guide-down report did not reach held-input tracking");
        complete(0U, NULL, -USB_ERR_IO);
        tick(10U);
    }
    CHECK(disconnects == 1U && pending[0] == NULL && pending[1] == NULL,
          "valid IN reports hid a permanent OUT failure");
    unsigned reported = reports;
    late_complete(late_arg, sizeof(guide));
    tick(1000U);
    CHECK(reports == reported && pending[1] == NULL,
          "late completion revived held input after controller failure");
    /* Another input may now occupy the released slot; the old class must
     * not disconnect it when the failed USB device is finally unplugged. */
    disconnect_device();
    CHECK(disconnects == 1U, "unplug released the shared input twice");
    puts("PASS independent OUT failure limit, held-input release, and late completion");
}

static void test_waiting_diagnostics(void)
{
    uint8_t announce[32] = {0x02, 0x20, 0x01, 28};
    for (unsigned i = 4U; i < sizeof(announce); ++i) {
        announce[i] = (uint8_t)i;
    }
    const char *snapshot = "02 20 01 1c 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f 10 11";
    prepare();
    const uint32_t connected_at = now_ms;
    CHECK(connect_raw_device(0U) == 0, "valid device did not connect");
    CHECK(diagnostic_count("Xbox One startup sent") == 0U,
          "startup diagnostic appeared before output completed");
    const unsigned input_submits = submit_count[1];
    receive(announce, sizeof(announce));
    CHECK(reports == 0U && pending[1] != NULL && pending[0] != NULL &&
          submit_count[1] == input_submits + 1U &&
          submit_count[0] == 1U && control_notes == 0U,
          "hello diagnostic changed input or blocked normal startup");
    CHECK(diagnostic_count("Xbox One rx") == 1U &&
          strstr(diagnostic_log, "packets=1") != NULL &&
          strstr(diagnostic_log, "empty=0") != NULL,
          "first positive but ignored packet was not diagnosed");
    check_snapshot_data(snapshot);
    finish_startup();
    CHECK(diagnostic_count("Xbox One startup sent") == 1U &&
          strstr(diagnostic_log, "commands=4") != NULL,
          "completed startup did not emit one command-count diagnostic");

    complete(1U, NULL, 0);
    const unsigned after_empty = submit_count[1];
    tick(9U);
    CHECK(pending[1] == NULL && submit_count[1] == after_empty,
          "empty-packet diagnostics bypassed the input retry delay");
    tick(1U);
    CHECK(pending[1] != NULL && submit_count[1] == after_empty + 1U,
          "empty-packet diagnostics stopped normal input retry");
    tick(1999U - (uint32_t)(now_ms - connected_at));
    finish_clone_fallback();
    const unsigned output_submits = submit_count[0];
    CHECK(diagnostic_count("Xbox One waiting for input") == 0U,
          "waiting diagnostic appeared before the two-second deadline");
    tick(1U);
    CHECK(diagnostic_count("Xbox One waiting for input") == 1U &&
          diagnostic_count("Xbox One rx") == 2U &&
          strstr(diagnostic_log, "empty=1") != NULL,
          "silent controller did not emit its waiting state and receive snapshot");
    CHECK(reports == 0U && pending[1] != NULL && pending[0] == NULL &&
          submit_count[0] == output_submits && control_notes == 0U && disconnects == 0U,
          "waiting diagnostic retried startup or disconnected a quiet controller");
    const size_t logged = diagnostic_log_length;
    tick(10000U);
    tick(10000U);
    CHECK(diagnostic_log_length == logged,
          "quiet controller diagnostics repeated during later polls");

    clear_diagnostic_log();
    usb_xbox_one_dump_status(0U);
    CHECK(diagnostic_count("Xbox One rx") == 1U &&
          strstr(diagnostic_log, "init=4/4") != NULL &&
          strstr(diagnostic_log, "hello=1") != NULL &&
          strstr(diagnostic_log, "packets=1") != NULL &&
          strstr(diagnostic_log, "empty=1") != NULL,
          "status omitted completed startup or receive counters");
    check_snapshot_data(snapshot);

    const uint8_t short_packet[] = {0x07, 0x20, 0x77};
    receive(short_packet, sizeof(short_packet));
    clear_diagnostic_log();
    usb_xbox_one_dump_status(0U);
    CHECK(strstr(diagnostic_log, "packets=2") != NULL &&
          strstr(diagnostic_log, "len=3") != NULL && reports == 0U,
          "status lost the latest short packet or parsed it as input");
    check_snapshot_data("07 20 77");
    disconnect_device();
    puts("PASS one-time startup/waiting diagnostics, ignored packets, and fresh status snapshots");
}

static void test_active_input_diagnostics(void)
{
    const uint8_t input[] = {0x20, 0x00, 0x01, 14, 0x10, 0x00,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    prepare();
    CHECK(connect_device(0U) == 0, "valid device did not connect");
    finish_startup();
    receive(input, sizeof(input));
    CHECK(reports == 1U && diagnostic_count("Xbox One rx") == 1U &&
          diagnostic_count("Xbox One input active") == 1U,
          "normal input missed its first-packet diagnostic");
    clear_diagnostic_log();
    tick(5000U);
    receive(input, sizeof(input));
    tick(10000U);
    CHECK(reports == 2U && diagnostic_log_length == 0U,
          "normal reports did not suppress waiting notices or repeated packet logs");
    disconnect_device();
    puts("PASS normal input suppresses waiting notices and repeated packet diagnostics");
}

int main(void)
{
    test_scope_and_endpoints();
    test_hello_gates_startup();
    test_clone_fallback_deadline();
    test_clone_input_suppression();
    test_clone_ack_and_retries();
    test_clone_failure_and_unplug();
    test_clone_control_pump();
    test_input_and_ack();
    test_malformed_reports();
    test_retry_and_disconnect();
    test_output_completion_and_timeout();
    test_guide_ack_queue();
    test_stall_recovery();
    test_permanent_halt_failure();
    test_transport_failures();
    test_output_failure_with_live_input();
    test_waiting_diagnostics();
    test_active_input_diagnostics();
    puts("XBOX ONE RUNTIME PASS");
    return 0;
}
