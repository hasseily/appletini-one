/* SPDX-License-Identifier: GPL-3.0-only */
/* Wired Xbox 360/XInput transport adapted from hasseily/multitini-one:
 * firmware/platform/xilinx/usb_hid_service.c and firmware/common/c64_xinput.c.
 * CherryUSB's Apache-2.0 Xbox class owns matching and endpoint discovery.
 * This adapter publishes full analog reports through Appletini's input API.
 */
#include <stdio.h>
#include <string.h>

#include "usbh_core.h"
#include "usbh_xbox.h"
#include "usb_gamepad_input.h"
#include "usb_controller_identity.h"
#include "usb_xbox_360.h"
#include "../lib/uart.h"

#define XBOX360_PACKET_BYTES 64U
#define XBOX360_REPORT_BYTES 20U
#define XBOX360_PREVIEW_BYTES 10U
#define XBOX360_RETRY_MS 10U
#define XBOX360_OUTPUT_ATTEMPTS 3U
#define XBOX360_LED_BYTES 3U

#if CONFIG_USBHOST_MAX_XBOX_CLASS > ONEE_INPUT_DEVICE_SLOT_COUNT
#error Xbox 360 class instances exceed the shared input slots
#endif

typedef struct {
    struct usbh_xbox *xbox;
    uint32_t report_retry_at;
    uint32_t output_retry_at;
    uint32_t reports;
    uint32_t invalid_reports;
    uint32_t submit_errors;
    uint32_t transfer_errors;
    int last_error;
    int output_last_error;
    uint8_t slot;
    uint8_t active;
    uint8_t report_pending;
    uint8_t report_retry;
    uint8_t output_pending;
    uint8_t output_retry;
    uint8_t output_attempts;
    uint8_t output_sent;
    uint8_t last_length;
    uint8_t last_packet[XBOX360_PREVIEW_BYTES];
} xbox360_t;

static xbox360_t g_xbox360[CONFIG_USBHOST_MAX_XBOX_CLASS];
/* DMA must never share a cache line with live driver state or another pad. */
static uint8_t g_xbox360_in[CONFIG_USBHOST_MAX_XBOX_CLASS]
    [USB_ALIGN_UP(XBOX360_PACKET_BYTES, CONFIG_USB_ALIGN_SIZE)] USB_MEM_ALIGNX;
static uint8_t g_xbox360_out[CONFIG_USBHOST_MAX_XBOX_CLASS]
    [USB_ALIGN_UP(XBOX360_PACKET_BYTES, CONFIG_USB_ALIGN_SIZE)] USB_MEM_ALIGNX;
static uint8_t g_xbox360_control[CONFIG_USBHOST_MAX_XBOX_CLASS]
    [USB_ALIGN_UP(XBOX360_PACKET_BYTES, CONFIG_USB_ALIGN_SIZE)] USB_MEM_ALIGNX;

static xbox360_t *xbox360_for_class(struct usbh_xbox *xbox)
{
    if (xbox == NULL || xbox->minor >= CONFIG_USBHOST_MAX_XBOX_CLASS) {
        return NULL;
    }
    return &g_xbox360[xbox->minor];
}

static uint8_t xbox360_index(const xbox360_t *pad)
{
    return (uint8_t)(pad - g_xbox360);
}

static int xbox360_connected(const xbox360_t *pad)
{
    return pad->active && pad->xbox != NULL && pad->xbox->hport != NULL &&
           pad->xbox->hport->connected;
}

static int32_t xbox360_s16(const uint8_t *data)
{
    const uint16_t value = (uint16_t)((uint16_t)data[0] |
                                     ((uint16_t)data[1] << 8U));
    return value < 0x8000U ? (int32_t)value : (int32_t)value - 65536;
}

static uint8_t xbox360_hat(uint8_t directions)
{
    static const uint8_t hats[16] = {
        8U, 0U, 4U, 8U, 6U, 7U, 5U, 8U,
        2U, 1U, 3U, 8U, 8U, 8U, 8U, 8U
    };
    return hats[directions & 15U];
}

static int xbox360_decode(xbox360_t *pad, const uint8_t *data, size_t length)
{
    onee_input_joystick_report_t report = {0};
    static const uint8_t offsets[6] = {6U, 8U, 4U, 10U, 12U, 5U};

    if (length < XBOX360_REPORT_BYTES || data[0] != 0U ||
        data[1] != XBOX360_REPORT_BYTES) {
        return -USB_ERR_INVAL;
    }
    report.axis_valid_mask = 0x3fU;
    report.buttons_valid = 1U;
    report.buttons = (uint8_t)((data[3] >> 4U) | ((data[3] & 3U) << 4U) |
                              ((data[2] & 0x20U) << 1U) |
                              ((data[2] & 0x10U) << 3U));
    if (data[2] & 0x40U) report.buttons |= UINT32_C(1) << USB_GAMEPAD_BUTTON_LEFT_STICK;
    if (data[2] & 0x80U) report.buttons |= UINT32_C(1) << USB_GAMEPAD_BUTTON_RIGHT_STICK;
    if (data[3] & 4U) report.buttons |= UINT32_C(1) << USB_GAMEPAD_BUTTON_HOME;
    for (unsigned axis = 0U; axis < 6U; ++axis) {
        const unsigned offset = offsets[axis];
        if (axis == ONEE_INPUT_AXIS_Z || axis == ONEE_INPUT_AXIS_RZ) {
            report.axis[axis] = data[offset];
            report.logical_max[axis] = 255;
        } else {
            int32_t value = xbox360_s16(data + offset);
            if (axis == ONEE_INPUT_AXIS_Y || axis == ONEE_INPUT_AXIS_RY) {
                value = -1 - value;
            }
            report.axis[axis] = value;
            report.logical_min[axis] = -32768;
            report.logical_max[axis] = 32767;
        }
    }
    usb_gamepad_input_report(pad->slot, &report, xbox360_hat(data[2]), 0U);
    return 0;
}

static void xbox360_submit_report(xbox360_t *pad);

static void xbox360_report_complete(void *argument, int bytes)
{
    xbox360_t *pad = argument;
    if (pad == NULL) return;
    pad->report_pending = 0U;
    if (!xbox360_connected(pad) || bytes == -USB_ERR_SHUTDOWN) return;
    if (bytes > 0) {
        const uint8_t *data = g_xbox360_in[xbox360_index(pad)];
        pad->last_length = (uint8_t)((unsigned)bytes > XBOX360_PREVIEW_BYTES ?
                                   XBOX360_PREVIEW_BYTES : (unsigned)bytes);
        memset(pad->last_packet, 0, sizeof(pad->last_packet));
        memcpy(pad->last_packet, data, pad->last_length);
        pad->last_error = xbox360_decode(pad, data, (size_t)bytes);
        if (pad->last_error == 0) ++pad->reports;
        else ++pad->invalid_reports;
    } else if (bytes < 0) {
        ++pad->transfer_errors;
        pad->last_error = bytes;
        pad->report_retry_at = cherryusb_baremetal_ms();
        pad->report_retry = 1U;
        return;
    }
    xbox360_submit_report(pad);
}

static void xbox360_submit_report(xbox360_t *pad)
{
    uint32_t length;
    int result;
    if (!xbox360_connected(pad) || pad->report_pending || pad->xbox->intin == NULL) return;
    length = USB_GET_MAXPACKETSIZE(pad->xbox->intin->wMaxPacketSize);
    if (length > XBOX360_PACKET_BYTES) length = XBOX360_PACKET_BYTES;
    if (length < XBOX360_REPORT_BYTES) {
        pad->last_error = -USB_ERR_RANGE;
        return;
    }
    memset(g_xbox360_in[xbox360_index(pad)], 0, XBOX360_PACKET_BYTES);
    usbh_int_urb_fill(&pad->xbox->intin_urb, pad->xbox->hport, pad->xbox->intin,
                      g_xbox360_in[xbox360_index(pad)], length, 0U,
                      xbox360_report_complete, pad);
    result = usbh_submit_urb(&pad->xbox->intin_urb);
    if (result == 0) {
        pad->report_pending = 1U;
        pad->report_retry = 0U;
        return;
    }
    if (result == -USB_ERR_NOMEM && pad->xbox->intin_urb.hcpriv == NULL) {
        pad->xbox->intin_urb.errorcode = 0;
    }
    ++pad->submit_errors;
    pad->last_error = result;
    pad->report_retry_at = cherryusb_baremetal_ms();
    pad->report_retry = 1U;
}

static void xbox360_output_retry(xbox360_t *pad, int error)
{
    pad->output_last_error = error;
    pad->last_error = error;
    if (pad->output_attempts < XBOX360_OUTPUT_ATTEMPTS &&
        error != -USB_ERR_STALL && error != -USB_ERR_SHUTDOWN &&
        error != -USB_ERR_NODEV && error != -USB_ERR_NOTCONN) {
        pad->output_retry_at = cherryusb_baremetal_ms();
        pad->output_retry = 1U;
    }
}

static void xbox360_output_complete(void *argument, int bytes)
{
    xbox360_t *pad = argument;
    if (pad == NULL) return;
    pad->output_pending = 0U;
    if (!xbox360_connected(pad) || bytes == -USB_ERR_SHUTDOWN) return;
    if (bytes == XBOX360_LED_BYTES) {
        pad->output_sent = 1U;
        pad->output_retry = 0U;
        pad->output_last_error = 0;
    } else {
        ++pad->transfer_errors;
        xbox360_output_retry(pad, bytes < 0 ? bytes : -USB_ERR_IO);
    }
}

static void xbox360_submit_output(xbox360_t *pad)
{
    struct usb_endpoint_descriptor *endpoint;
    uint8_t *data;
    int result;
    if (!xbox360_connected(pad) || pad->output_pending || pad->output_sent ||
        pad->output_attempts >= XBOX360_OUTPUT_ATTEMPTS) return;
    endpoint = pad->xbox->intout;
    if (endpoint == NULL || (endpoint->bEndpointAddress & USB_ENDPOINT_DIRECTION_MASK) ||
        (endpoint->bmAttributes & USB_ENDPOINT_TYPE_MASK) != USB_ENDPOINT_TYPE_INTERRUPT ||
        USB_GET_MAXPACKETSIZE(endpoint->wMaxPacketSize) < XBOX360_LED_BYTES) {
        pad->output_last_error = -USB_ERR_NOTSUPP;
        return;
    }
    data = g_xbox360_out[xbox360_index(pad)];
    memset(data, 0, XBOX360_PACKET_BYTES);
    data[0] = 0x01U;
    data[1] = XBOX360_LED_BYTES;
    data[2] = 0x02U; /* Player 1: a steady ring quadrant. */
    pad->output_retry = 0U;
    ++pad->output_attempts;
    usbh_int_urb_fill(&pad->xbox->intout_urb, pad->xbox->hport, endpoint, data,
                      XBOX360_LED_BYTES, 0U, xbox360_output_complete, pad);
    result = usbh_submit_urb(&pad->xbox->intout_urb);
    if (result == 0) {
        pad->output_pending = 1U;
        return;
    }
    if ((result == -USB_ERR_NOMEM || result == -USB_ERR_BUSY) &&
        pad->xbox->intout_urb.hcpriv == NULL) {
        pad->xbox->intout_urb.errorcode = 0;
    }
    ++pad->submit_errors;
    xbox360_output_retry(pad, result);
}

static void xbox360_initialize(xbox360_t *pad)
{
    struct usb_setup_packet *setup = pad->xbox->hport->setup;
    int result;
    if (setup == NULL) return;
    memset(setup, 0, sizeof(*setup));
    memset(g_xbox360_control[xbox360_index(pad)], 0, XBOX360_PACKET_BYTES);
    setup->bmRequestType = USB_REQUEST_DIR_IN | USB_REQUEST_VENDOR | USB_REQUEST_RECIPIENT_INTERFACE;
    setup->bRequest = 0x01U;
    setup->wValue = 0x0100U;
    setup->wIndex = pad->xbox->intf;
    setup->wLength = XBOX360_REPORT_BYTES;
    result = usbh_control_transfer(pad->xbox->hport, setup,
                                   g_xbox360_control[xbox360_index(pad)]);
    if (result < 0 && result != -USB_ERR_STALL) {
        cherryusb_printf("[usb1] Xbox 360 initialization warning: %d\r\n", result);
    }
}

void usbh_xbox_run(struct usbh_xbox *xbox)
{
    xbox360_t *pad = xbox360_for_class(xbox);
    int slot;
    if (pad == NULL || pad->active || xbox->hport == NULL || !xbox->hport->connected ||
        xbox->intin == NULL ||
        !(xbox->intin->bEndpointAddress & USB_ENDPOINT_DIRECTION_MASK) ||
        (xbox->intin->bmAttributes & USB_ENDPOINT_TYPE_MASK) != USB_ENDPOINT_TYPE_INTERRUPT ||
        USB_GET_MAXPACKETSIZE(xbox->intin->wMaxPacketSize) < XBOX360_REPORT_BYTES) {
        return;
    }
    slot = usb_gamepad_input_connect();
    if (slot < 0) return;
    memset(pad, 0, sizeof(*pad));
    pad->slot = (uint8_t)slot;
    pad->xbox = xbox;
    pad->active = 1U;
    usb_gamepad_input_identify(pad->slot, xbox->hport, xbox->intf,
                               USB_CONTROLLER_TRANSPORT_XBOX360);
    cherryusb_printf("[usb1] Xbox 360 connected slot=%u class=%u vid=%04x pid=%04x intf=%u\r\n",
                     pad->slot, xbox->minor, xbox->hport->device_desc.idVendor,
                     xbox->hport->device_desc.idProduct, xbox->intf);
    xbox360_submit_report(pad);
    xbox360_initialize(pad);
    xbox360_submit_output(pad);
}

void usbh_xbox_stop(struct usbh_xbox *xbox)
{
    xbox360_t *pad = xbox360_for_class(xbox);
    if (pad == NULL || pad->xbox != xbox) return;
    /* Cancellation can call the completion callbacks before returning. */
    pad->active = 0U;
    if (pad->report_pending) (void)usbh_kill_urb(&xbox->intin_urb);
    if (pad->output_pending) (void)usbh_kill_urb(&xbox->intout_urb);
    usb_gamepad_input_disconnect(pad->slot);
    memset(pad, 0, sizeof(*pad));
}

void usb_xbox_360_poll(void)
{
    const uint32_t now = cherryusb_baremetal_ms();
    for (unsigned index = 0U; index < CONFIG_USBHOST_MAX_XBOX_CLASS; ++index) {
        xbox360_t *pad = &g_xbox360[index];
        if (!xbox360_connected(pad)) continue;
        if (pad->report_retry && !pad->report_pending &&
            (uint32_t)(now - pad->report_retry_at) >= XBOX360_RETRY_MS) {
            pad->report_retry = 0U;
            xbox360_submit_report(pad);
        }
        if (pad->output_retry && !pad->output_pending &&
            (uint32_t)(now - pad->output_retry_at) >= XBOX360_RETRY_MS) {
            pad->output_retry = 0U;
            xbox360_submit_output(pad);
        }
    }
}

void usb_xbox_360_stop(void)
{
    for (unsigned index = 0U; index < CONFIG_USBHOST_MAX_XBOX_CLASS; ++index) {
        if (g_xbox360[index].xbox != NULL) usbh_xbox_stop(g_xbox360[index].xbox);
    }
}

void usb_xbox_360_get_activity(uint32_t *reports, uint32_t *errors)
{
    *reports = 0U;
    *errors = 0U;
    for (unsigned index = 0U; index < CONFIG_USBHOST_MAX_XBOX_CLASS; ++index) {
        const xbox360_t *pad = &g_xbox360[index];
        *reports += pad->reports;
        *errors += pad->invalid_reports + pad->submit_errors + pad->transfer_errors;
    }
}

void usb_xbox_360_dump_status(uint32_t uart_base)
{
    char line[256];
    for (unsigned index = 0U; index < CONFIG_USBHOST_MAX_XBOX_CLASS; ++index) {
        const xbox360_t *pad = &g_xbox360[index];
        if (!pad->active) continue;
        int used = snprintf(line, sizeof(line),
                            "  Xbox 360 slot=%u reports=%lu invalid=%lu submit=%lu xfer=%lu "
                            "err=%d led=%u out_try=%u out_err=%d len=%u data=",
                            pad->slot, (unsigned long)pad->reports,
                            (unsigned long)pad->invalid_reports, (unsigned long)pad->submit_errors,
                            (unsigned long)pad->transfer_errors, pad->last_error,
                            pad->output_sent, pad->output_attempts, pad->output_last_error,
                            pad->last_length);
        for (unsigned byte = 0U; byte < pad->last_length && used >= 0 &&
             (size_t)used < sizeof(line); ++byte) {
            const int added = snprintf(line + used, sizeof(line) - (size_t)used,
                                       "%02x", pad->last_packet[byte]);
            if (added < 0) break;
            used += added;
        }
        if (used >= 0 && (size_t)used < sizeof(line)) {
            (void)snprintf(line + used, sizeof(line) - (size_t)used, "\r\n");
        }
        uart_puts(uart_base, line);
    }
}
