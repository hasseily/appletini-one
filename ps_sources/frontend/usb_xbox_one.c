/* Wired Xbox One S input for USB1 (including EG-C50700X, 045e:02ea).
 * Protocol references: Linux drivers/input/joystick/xpad.c and
 * github.com/quantus/xbox-one-controller-protocol. Only interface 0 carries
 * gamepad input. Audio/accessory interfaces must not claim input slots.
 */
#include <stdio.h>
#include <string.h>

#include "usbh_core.h"
#include "usb_gamepad_input.h"
#include "usb_xbox_one.h"
#include "../lib/uart.h"

#define XBOX_COUNT ONEE_INPUT_DEVICE_SLOT_COUNT
#define XBOX_PACKET_BYTES 64U
#define XBOX_RETRY_MS 10U
#define XBOX_HALT_TIMEOUT_MS 100U
#define XBOX_MAX_FAILURES 8U
#define XBOX_OUT_TIMEOUT_MS 1000U
#define XBOX_INPUT_NOTICE_MS 2000U
#define XBOX_RX_PREVIEW_BYTES 18U
#define XBOX_ACK_COUNT 4U
#define XBOX_INIT_COUNT 4U
#define XBOX_CLONE_WAIT_MS 1000U
#define XBOX_CLONE_COUNT 2U

enum {
    XBOX_TX_INIT,
    XBOX_TX_ACK,
    XBOX_TX_CLONE
};

typedef struct {
    struct usbh_hubport *hport;
    struct usb_endpoint_descriptor *in_ep;
    struct usb_endpoint_descriptor *out_ep;
    struct usbh_urb in_urb;
    struct usbh_urb out_urb;
    onee_input_joystick_report_t report;
    uint32_t in_retry_at;
    uint32_t out_retry_at;
    uint32_t out_started;
    uint32_t connected_at;
    uint32_t init_completed_at;
    uint32_t packets;
    uint32_t empty_packets;
    uint32_t reports;
    uint32_t errors;
    uint8_t active;
    uint8_t input_connected;
    uint8_t faulted;
    uint8_t failures[2];
    uint8_t slot;
    uint8_t index;
    uint8_t in_pending;
    uint8_t out_pending;
    uint8_t in_retry;
    uint8_t out_retry;
    uint8_t in_halted;
    uint8_t out_halted;
    uint8_t hello_seen;
    uint8_t init_step;
    uint8_t clone_step;
    uint8_t sequence;
    uint8_t tx_len;
    uint8_t tx_type;
    uint8_t ack_sequence[XBOX_ACK_COUNT];
    uint8_t ack_count;
    uint8_t guide;
    uint8_t stick_buttons;
    uint8_t hat;
    uint8_t error_logged;
    uint8_t waiting_logged;
    uint8_t last_rx_len;
    uint8_t last_rx[XBOX_RX_PREVIEW_BYTES];
} xbox_one_t;

static xbox_one_t g_xbox[XBOX_COUNT];
/* Each DMA buffer occupies whole cache lines, separate from driver state. */
static uint8_t g_xbox_in[XBOX_COUNT][USB_ALIGN_UP(XBOX_PACKET_BYTES, CONFIG_USB_ALIGN_SIZE)]
    USB_MEM_ALIGNX;
static uint8_t g_xbox_out[XBOX_COUNT][USB_ALIGN_UP(XBOX_PACKET_BYTES, CONFIG_USB_ALIGN_SIZE)]
    USB_MEM_ALIGNX;

static void xbox_rx_status(const xbox_one_t *pad, char *line, size_t size)
{
    int used = snprintf(line, size,
                        "Xbox One rx slot=%u packets=%lu empty=%lu len=%u data=",
                        pad->slot, (unsigned long)pad->packets,
                        (unsigned long)pad->empty_packets, pad->last_rx_len);
    for (uint8_t i = 0U; i < pad->last_rx_len && i < XBOX_RX_PREVIEW_BYTES; ++i) {
        if (used < 0 || (size_t)used >= size) {
            return;
        }
        int added = snprintf(line + used, size - (size_t)used,
                             "%s%02x", i == 0U ? "" : " ", pad->last_rx[i]);
        if (added < 0) {
            return;
        }
        used += added;
    }
    if (used >= 0 && (size_t)used < size) {
        (void)snprintf(line + used, size - (size_t)used, "\r\n");
    }
}

static void xbox_error(xbox_one_t *pad, int error, uint8_t endpoint)
{
    pad->errors++;
    if (endpoint != 0U) {
        const unsigned direction = (endpoint & 0x80U) != 0U;
        if (pad->failures[direction] < XBOX_MAX_FAILURES) {
            pad->failures[direction]++;
        }
        if (pad->failures[direction] == XBOX_MAX_FAILURES) {
            pad->faulted = 1U;
        }
    }
    if (pad->error_logged == 0U) {
        cherryusb_printf("[usb1] Xbox One transfer error slot=%u ep=%02x err=%d init=%u/%u\r\n",
                         pad->slot, endpoint, error, pad->init_step, XBOX_INIT_COUNT);
        pad->error_logged = 1U;
    }
}

static uint16_t xbox_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static int32_t xbox_s16(const uint8_t *p)
{
    const uint16_t value = xbox_u16(p);
    return (value & 0x8000U) ? (int32_t)value - 65536 : (int32_t)value;
}

static uint8_t xbox_hat(uint8_t buttons)
{
    const int x = ((buttons & 8U) != 0U) - ((buttons & 4U) != 0U);
    const int y = ((buttons & 2U) != 0U) - ((buttons & 1U) != 0U);
    if (y < 0) {
        return (x < 0) ? 7U : ((x > 0) ? 1U : 0U);
    }
    if (y > 0) {
        return (x < 0) ? 5U : ((x > 0) ? 3U : 4U);
    }
    return (x < 0) ? 6U : ((x > 0) ? 2U : 8U);
}

static void xbox_queue_ack(xbox_one_t *pad, uint8_t sequence)
{
    if (pad->tx_len != 0U && pad->tx_type == XBOX_TX_ACK &&
        g_xbox_out[pad->index][2] == sequence) {
        return;
    }
    for (uint8_t i = 0U; i < pad->ack_count; ++i) {
        if (pad->ack_sequence[i] == sequence) {
            return;
        }
    }
    if (pad->ack_count < XBOX_ACK_COUNT) {
        pad->ack_sequence[pad->ack_count++] = sequence;
    } else {
        /* The controller retries an unacknowledged report. */
        xbox_error(pad, -USB_ERR_NOMEM, 0U);
    }
}

static void xbox_input(xbox_one_t *pad, const uint8_t *data, uint32_t len)
{
    onee_input_joystick_report_t *report = &pad->report;

    /* This driver handles unchunked packets for the main controller only.
     * Extended lengths and accessory clients use different header layouts. */
    if (len < 4U || (data[1] & 0xcfU) != 0U ||
        (data[3] & 0x80U) != 0U || (uint32_t)data[3] + 4U > len) {
        return;
    }
    if (data[0] == 0x02U) {
        /* GIP Hello: primary device, system packet, 28-byte identity.
         * Start is a valid Hello response for a non-audio device. Latch it
         * once so queued duplicate Hellos cannot restart an active OUT. */
        if (data[1] == 0x20U && data[2] != 0U && data[3] == 28U &&
            pad->hello_seen == 0U) {
            pad->hello_seen = 1U;
            cherryusb_printf("[usb1] Xbox One Hello received slot=%u; starting input\r\n",
                             pad->slot);
        }
        return;
    }
    if (pad->hello_seen == 0U) {
        return;
    }
    if (data[0] == 0x07U && data[3] == 2U) {
        if ((data[1] & 0x10U) != 0U) {
            xbox_queue_ack(pad, data[2]);
        }
        pad->guide = data[4] & 1U;
    } else if (data[0] == 0x20U && data[3] >= 14U) {
        report->axis_valid_mask = (1U << ONEE_INPUT_AXIS_COUNT) - 1U;
        report->buttons_valid = 1U;
        report->buttons = (uint8_t)((data[4] >> 4) |
                                   (data[5] & 0x30U) |
                                   ((data[4] & 0x08U) << 3) |
                                   ((data[4] & 0x04U) << 5));
        report->axis[ONEE_INPUT_AXIS_X] = xbox_s16(data + 10);
        report->axis[ONEE_INPUT_AXIS_Y] = -1 - xbox_s16(data + 12);
        report->axis[ONEE_INPUT_AXIS_RX] = xbox_s16(data + 14);
        report->axis[ONEE_INPUT_AXIS_RY] = -1 - xbox_s16(data + 16);
        report->axis[ONEE_INPUT_AXIS_Z] = xbox_u16(data + 6);
        report->axis[ONEE_INPUT_AXIS_RZ] = xbox_u16(data + 8);
        for (uint8_t i = 0U; i < ONEE_INPUT_AXIS_COUNT; ++i) {
            const uint8_t trigger = (i == ONEE_INPUT_AXIS_Z || i == ONEE_INPUT_AXIS_RZ);
            report->logical_min[i] = trigger ? 0 : -32768;
            report->logical_max[i] = trigger ? 1023 : 32767;
            if (trigger && report->axis[i] > 1023) {
                report->axis[i] = 1023;
            }
        }
        pad->stick_buttons = data[5] & 0xc0U;
        if (pad->stick_buttons & 0x40U)
            report->buttons |= UINT32_C(1) << USB_GAMEPAD_BUTTON_LEFT_STICK;
        if (pad->stick_buttons & 0x80U)
            report->buttons |= UINT32_C(1) << USB_GAMEPAD_BUTTON_RIGHT_STICK;
        pad->hat = xbox_hat(data[5]);
        pad->reports++;
        if (pad->reports == 1U) {
            cherryusb_printf("[usb1] Xbox One input active slot=%u joystick=1\r\n",
                             pad->slot);
        }
    } else {
        return;
    }
    /* Guide arrives separately; the saved main report and Guide state each
     * survive packets that update only the other controls. */
    report->buttons &= ~(UINT32_C(1) << USB_GAMEPAD_BUTTON_HOME);
    if (pad->guide) report->buttons |= UINT32_C(1) << USB_GAMEPAD_BUTTON_HOME;
    report->buttons_valid = 1U;
    usb_gamepad_input_report(pad->slot, report, pad->hat, 0U);
}

static void xbox_in_complete(void *arg, int nbytes)
{
    xbox_one_t *pad = (xbox_one_t *)arg;
    pad->in_pending = 0U;
    if (pad->active == 0U || pad->faulted != 0U) {
        return;
    }
    if (nbytes > 0 && nbytes <= (int)XBOX_PACKET_BYTES) {
        pad->packets++;
        pad->last_rx_len = (uint8_t)nbytes;
        memcpy(pad->last_rx, g_xbox_in[pad->index],
               nbytes < (int)XBOX_RX_PREVIEW_BYTES ? (size_t)nbytes : XBOX_RX_PREVIEW_BYTES);
        if (pad->packets == 1U) {
            char line[160];
            xbox_rx_status(pad, line, sizeof(line));
            cherryusb_printf("[usb1] %s", line);
        }
        pad->failures[1] = 0U;
        pad->error_logged = 0U;
        xbox_input(pad, g_xbox_in[pad->index], (uint32_t)nbytes);
    } else {
        if (nbytes == -USB_ERR_STALL) {
            pad->in_halted = 1U;
        }
        if (nbytes != 0) {
            xbox_error(pad, nbytes, pad->in_ep->bEndpointAddress);
        } else {
            pad->empty_packets++;
        }
        pad->in_retry = 1U;
        pad->in_retry_at = cherryusb_baremetal_ms();
    }
    /* Submit again in poll, after EHCI releases the completed QH. */
}

static void xbox_out_complete(void *arg, int nbytes)
{
    xbox_one_t *pad = (xbox_one_t *)arg;
    pad->out_pending = 0U;
    if (pad->active == 0U || pad->faulted != 0U) {
        return;
    }
    if (nbytes == (int)pad->tx_len) {
        pad->failures[0] = 0U;
        if (pad->tx_type == XBOX_TX_INIT) {
            pad->init_step++;
            if (pad->init_step == XBOX_INIT_COUNT) {
                pad->init_completed_at = cherryusb_baremetal_ms();
                cherryusb_printf("[usb1] Xbox One startup sent slot=%u commands=%u\r\n",
                                 pad->slot, XBOX_INIT_COUNT);
            }
        } else if (pad->tx_type == XBOX_TX_CLONE) {
            pad->clone_step++;
            if (pad->clone_step == XBOX_CLONE_COUNT) {
                cherryusb_printf("[usb1] Xbox One clone fallback sent slot=%u commands=%u\r\n",
                                 pad->slot, XBOX_CLONE_COUNT);
            }
        }
        pad->tx_len = 0U;
    } else {
        if (nbytes == -USB_ERR_STALL) {
            pad->out_halted = 1U;
        }
        xbox_error(pad, nbytes < 0 ? nbytes : -USB_ERR_IO,
                   pad->out_ep->bEndpointAddress);
        pad->out_retry = 1U;
        pad->out_retry_at = cherryusb_baremetal_ms();
    }
}

static int xbox_clear_halt(xbox_one_t *pad, struct usbh_urb *urb)
{
    struct usb_setup_packet *setup = pad->hport->setup;
    struct usbh_urb *control = &pad->hport->ep0_urb;
    int rc;

    /* One bounded attempt per poll, not the generic one-second, three-attempt
     * control wrapper. The EP0 wait still pumps USB and Apple-facing work. */
    if (control->errorcode == -USB_ERR_BUSY) {
        xbox_error(pad, -USB_ERR_BUSY, urb->ep->bEndpointAddress);
        return -USB_ERR_BUSY;
    }
    usb_osal_mutex_take(pad->hport->mutex);
    setup->bmRequestType = USB_REQUEST_DIR_OUT | USB_REQUEST_STANDARD |
                           USB_REQUEST_RECIPIENT_ENDPOINT;
    setup->bRequest = USB_REQUEST_CLEAR_FEATURE;
    setup->wValue = USB_FEATURE_ENDPOINT_HALT;
    setup->wIndex = urb->ep->bEndpointAddress;
    setup->wLength = 0U;
    usbh_control_urb_fill(control, pad->hport, setup, NULL, 0U,
                          XBOX_HALT_TIMEOUT_MS, NULL, NULL);
    rc = usbh_submit_urb(control);
    if (rc == -USB_ERR_NOMEM && control->hcpriv == NULL) {
        control->errorcode = 0;
    }
    control->timeout = 0U;
    usb_osal_mutex_give(pad->hport->mutex);
    cherryusb_host_debug_note_control(pad->hport, setup, rc);
    if (rc >= 0) {
        urb->data_toggle = 0U;
    } else {
        xbox_error(pad, rc, urb->ep->bEndpointAddress);
    }
    return rc;
}

static void xbox_submit_in(xbox_one_t *pad, uint32_t now)
{
    int rc;
    if (pad->in_pending != 0U ||
        (pad->in_retry != 0U && (uint32_t)(now - pad->in_retry_at) < XBOX_RETRY_MS)) {
        return;
    }
    pad->in_retry = 0U;
    if (pad->in_halted != 0U) {
        if (xbox_clear_halt(pad, &pad->in_urb) < 0) {
            pad->in_retry = 1U;
            pad->in_retry_at = cherryusb_baremetal_ms();
            return;
        }
        pad->in_halted = 0U;
    }
    if (pad->active == 0U || pad->faulted != 0U || !pad->hport->connected) {
        return;
    }
    usbh_int_urb_fill(&pad->in_urb, pad->hport, pad->in_ep,
                      g_xbox_in[pad->index], USB_GET_MAXPACKETSIZE(pad->in_ep->wMaxPacketSize),
                      0U, xbox_in_complete, pad);
    pad->in_pending = 1U;
    rc = usbh_submit_urb(&pad->in_urb);
    if (rc != 0) {
        pad->in_pending = 0U;
        if (rc == -USB_ERR_NOMEM && pad->in_urb.hcpriv == NULL) {
            pad->in_urb.errorcode = 0;
        }
        pad->in_retry = 1U;
        pad->in_retry_at = now;
        xbox_error(pad, rc, pad->in_ep->bEndpointAddress);
    }
}

static void xbox_prepare_out(xbox_one_t *pad, uint32_t now)
{
    uint8_t *data = g_xbox_out[pad->index];
    /* Xbox One S requires the extra power and LED/mode commands, including
     * the five-byte power packet whose length field is deliberately 0x0f. */
    static const uint8_t init[XBOX_INIT_COUNT][7] = {
        { 0x05, 0x20, 0x00, 0x01, 0x00 },
        { 0x05, 0x20, 0x00, 0x0f, 0x06 },
        { 0x0a, 0x20, 0x00, 0x03, 0x00, 0x01, 0x14 },
        { 0x06, 0x20, 0x00, 0x02, 0x01, 0x00 }
    };
    static const uint8_t init_len[XBOX_INIT_COUNT] = { 5U, 5U, 7U, 6U };
    static const uint8_t ack[] = {
        0x01, 0x20, 0x00, 0x09, 0x00, 0x07, 0x20, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    /* Clone-specific Identify/serial ACK workaround reported in
     * github.com/paroj/xpad/issues/161. Preserve its fixed sequence bytes.
     * Try once only when normal startup has completed without input. */
    static const uint8_t clone[XBOX_CLONE_COUNT][13] = {
        { 0x04, 0x20, 0x01, 0x00 },
        { 0x01, 0x20, 0x01, 0x09, 0x00, 0x1e, 0x20, 0x10,
          0x00, 0x00, 0x00, 0x00, 0x00 }
    };
    static const uint8_t clone_len[XBOX_CLONE_COUNT] = { 4U, 13U };

    if (pad->hello_seen == 0U) {
        return;
    }
    if (pad->tx_len != 0U) {
        /* Poll calls here only when no OUT is active. Input makes a queued
         * fallback unnecessary; all other retries keep their exact bytes. */
        if (pad->tx_type == XBOX_TX_CLONE && pad->reports != 0U) {
            pad->tx_len = 0U;
        } else {
            return;
        }
    }
    if (pad->ack_count != 0U) {
        memcpy(data, ack, sizeof(ack));
        data[2] = pad->ack_sequence[0];
        pad->ack_count--;
        memmove(pad->ack_sequence, pad->ack_sequence + 1, pad->ack_count);
        pad->tx_len = sizeof(ack);
        pad->tx_type = XBOX_TX_ACK;
    } else if (pad->init_step < XBOX_INIT_COUNT) {
        pad->tx_len = init_len[pad->init_step];
        memcpy(data, init[pad->init_step], pad->tx_len);
        data[2] = pad->sequence++;
        if (pad->sequence == 0U) {
            pad->sequence = 1U; /* GIP reserves sequence zero. */
        }
        pad->tx_type = XBOX_TX_INIT;
    } else if (pad->reports == 0U && pad->clone_step < XBOX_CLONE_COUNT &&
               (uint32_t)(now - pad->init_completed_at) >= XBOX_CLONE_WAIT_MS) {
        if (pad->clone_step == 0U) {
            cherryusb_printf("[usb1] Xbox One clone fallback starting slot=%u\r\n",
                             pad->slot);
        }
        pad->tx_len = clone_len[pad->clone_step];
        memcpy(data, clone[pad->clone_step], pad->tx_len);
        pad->tx_type = XBOX_TX_CLONE;
    }
}

static void xbox_release_failed_pad(xbox_one_t *pad)
{
    /* Run outside completion callbacks, after EHCI has released its QH. Keep
     * the class instance until unplug, but release this pad's shared input. */
    if (pad->input_connected != 0U) {
        (void)usbh_kill_urb(&pad->in_urb);
        (void)usbh_kill_urb(&pad->out_urb);
        usb_gamepad_input_disconnect(pad->slot);
        pad->input_connected = 0U;
        cherryusb_printf("[usb1] Xbox One stopped after repeated transfer failures slot=%u; unplug and reconnect\r\n",
                         pad->slot);
    }
}

static void xbox_poll_pad(xbox_one_t *pad, uint32_t now)
{
    int rc;
    if (pad->reports == 0U && pad->waiting_logged == 0U &&
        (uint32_t)(now - pad->connected_at) >= XBOX_INPUT_NOTICE_MS) {
        char line[160];
        pad->waiting_logged = 1U;
        cherryusb_printf("[usb1] Xbox One waiting for input slot=%u hello=%u init=%u/%u packets=%lu errors=%lu in=%u out=%u\r\n",
                         pad->slot, pad->hello_seen, pad->init_step, XBOX_INIT_COUNT,
                         (unsigned long)pad->packets, (unsigned long)pad->errors,
                         pad->in_pending, pad->out_pending);
        xbox_rx_status(pad, line, sizeof(line));
        cherryusb_printf("[usb1] %s", line);
    }
    if (pad->faulted != 0U) {
        xbox_release_failed_pad(pad);
        return;
    }
    xbox_submit_in(pad, now);
    if (pad->faulted != 0U) {
        xbox_release_failed_pad(pad);
        return;
    }
    if (pad->active == 0U || !pad->hport->connected) {
        return;
    }
    if (pad->out_pending != 0U) {
        if ((uint32_t)(now - pad->out_started) >= XBOX_OUT_TIMEOUT_MS) {
            /* kill completes synchronously; its callback arms the retry. */
            (void)usbh_kill_urb(&pad->out_urb);
        }
        return;
    }
    /* Receive Hello before waking the controller. All output stays async. */
    if (pad->in_pending == 0U ||
        (pad->out_retry != 0U && (uint32_t)(now - pad->out_retry_at) < XBOX_RETRY_MS)) {
        return;
    }
    /* IN halt recovery can pump the final startup completion above. */
    xbox_prepare_out(pad, cherryusb_baremetal_ms());
    if (pad->tx_len == 0U) {
        return;
    }
    pad->out_retry = 0U;
    if (pad->out_halted != 0U) {
        if (xbox_clear_halt(pad, &pad->out_urb) < 0) {
            pad->out_retry = 1U;
            pad->out_retry_at = cherryusb_baremetal_ms();
            return;
        }
        pad->out_halted = 0U;
    }
    if (pad->active == 0U || pad->faulted != 0U || !pad->hport->connected) {
        return;
    }
    /* OUT halt recovery can deliver input before this packet is submitted. */
    if (pad->tx_type == XBOX_TX_CLONE && pad->reports != 0U) {
        pad->tx_len = 0U;
        return;
    }
    usbh_int_urb_fill(&pad->out_urb, pad->hport, pad->out_ep,
                      g_xbox_out[pad->index], pad->tx_len, 0U, xbox_out_complete, pad);
    pad->out_pending = 1U;
    pad->out_started = now;
    rc = usbh_submit_urb(&pad->out_urb);
    if (rc != 0) {
        pad->out_pending = 0U;
        if (rc == -USB_ERR_NOMEM && pad->out_urb.hcpriv == NULL) {
            pad->out_urb.errorcode = 0;
        }
        pad->out_retry = 1U;
        pad->out_retry_at = now;
        xbox_error(pad, rc, pad->out_ep->bEndpointAddress);
    }
}

static int xbox_connect(struct usbh_hubport *hport, uint8_t intf)
{
    struct usb_endpoint_descriptor *in_ep = NULL;
    struct usb_endpoint_descriptor *out_ep = NULL;
    xbox_one_t *pad = NULL;
    int slot;

    if (intf != 0U || hport == NULL) {
        return -USB_ERR_NOTSUPP;
    }
    const struct usbh_interface_altsetting *alt = &hport->config.intf[intf].altsetting[0];
    if (alt->intf_desc.bNumEndpoints != 2U) {
        return -USB_ERR_NOTSUPP;
    }
    for (uint8_t i = 0U; i < alt->intf_desc.bNumEndpoints; ++i) {
        struct usb_endpoint_descriptor *ep = &hport->config.intf[intf].altsetting[0].ep[i].ep_desc;
        uint16_t mps = USB_GET_MAXPACKETSIZE(ep->wMaxPacketSize);
        if ((ep->bmAttributes & USB_ENDPOINT_TYPE_MASK) != USB_ENDPOINT_TYPE_INTERRUPT ||
            mps < 18U || mps > XBOX_PACKET_BYTES || ep->bInterval == 0U ||
            (hport->speed >= USB_SPEED_HIGH && ep->bInterval > 16U)) {
            return -USB_ERR_NOTSUPP;
        }
        if ((ep->bEndpointAddress & 0x80U) != 0U) {
            if (in_ep != NULL) {
                return -USB_ERR_NOTSUPP;
            }
            in_ep = ep;
        } else {
            if (out_ep != NULL) {
                return -USB_ERR_NOTSUPP;
            }
            out_ep = ep;
        }
    }
    if (in_ep == NULL || out_ep == NULL) {
        return -USB_ERR_NOTSUPP;
    }
    for (uint8_t i = 0U; i < XBOX_COUNT; ++i) {
        if (g_xbox[i].active == 0U) {
            pad = &g_xbox[i];
            memset(pad, 0, sizeof(*pad));
            pad->index = i;
            break;
        }
    }
    if (pad == NULL) {
        return -USB_ERR_NOMEM;
    }
    /* Keep the audio interface idle, as the controller expects on a host
     * without its headset driver. SET_CONFIGURATION already selected alt 0. */
    if (hport->config.config_desc.bNumInterfaces > 1U) {
        int rc = usbh_set_interface(hport, 1U, 0U);
        if (rc < 0) {
            return rc;
        }
    }
    slot = usb_gamepad_input_connect();
    if (slot < 0) {
        return -USB_ERR_NOMEM;
    }
    pad->slot = (uint8_t)slot;
    pad->active = 1U;
    pad->input_connected = 1U;
    pad->hport = hport;
    pad->in_ep = in_ep;
    pad->out_ep = out_ep;
    pad->sequence = 1U;
    pad->connected_at = cherryusb_baremetal_ms();
    pad->hat = 8U;
    hport->config.intf[intf].priv = pad;
    snprintf(hport->config.intf[intf].devname, CONFIG_USBHOST_DEV_NAMELEN,
             "/dev/xboxone%u", pad->index);
    cherryusb_printf("[usb1] Xbox One connected slot=%u vid=%04x pid=%04x intf=0\r\n",
                     pad->slot, hport->device_desc.idVendor, hport->device_desc.idProduct);
    cherryusb_printf("[usb1] Xbox One endpoints in=%02x mps=%u interval=%u out=%02x mps=%u interval=%u\r\n",
                     in_ep->bEndpointAddress, USB_GET_MAXPACKETSIZE(in_ep->wMaxPacketSize),
                     in_ep->bInterval, out_ep->bEndpointAddress,
                     USB_GET_MAXPACKETSIZE(out_ep->wMaxPacketSize), out_ep->bInterval);
    xbox_poll_pad(pad, cherryusb_baremetal_ms());
    return 0;
}

static int xbox_disconnect(struct usbh_hubport *hport, uint8_t intf)
{
    xbox_one_t *pad = (xbox_one_t *)hport->config.intf[intf].priv;
    if (pad != NULL && pad->active != 0U) {
        pad->active = 0U; /* Kill callbacks must not publish or rearm input. */
        (void)usbh_kill_urb(&pad->in_urb);
        (void)usbh_kill_urb(&pad->out_urb);
        if (pad->input_connected != 0U) {
            usb_gamepad_input_disconnect(pad->slot);
        }
        cherryusb_printf("[usb1] Xbox One disconnected slot=%u\r\n", pad->slot);
        memset(pad, 0, sizeof(*pad));
    }
    hport->config.intf[intf].priv = NULL;
    hport->config.intf[intf].devname[0] = '\0';
    return 0;
}

void usb_xbox_one_poll(void)
{
    const uint32_t now = cherryusb_baremetal_ms();
    for (uint8_t i = 0U; i < XBOX_COUNT; ++i) {
        if (g_xbox[i].active != 0U && g_xbox[i].hport->connected) {
            xbox_poll_pad(&g_xbox[i], now);
        }
    }
}

void usb_xbox_one_stop(void)
{
    for (uint8_t i = 0U; i < XBOX_COUNT; ++i) {
        if (g_xbox[i].active != 0U) {
            (void)xbox_disconnect(g_xbox[i].hport, 0U);
        }
    }
}

void usb_xbox_one_dump_status(uint32_t uart_base)
{
    char line[160];
    for (uint8_t i = 0U; i < XBOX_COUNT; ++i) {
        const xbox_one_t *pad = &g_xbox[i];
        if (pad->active != 0U) {
            snprintf(line, sizeof(line),
                     "xboxone%u: slot=%u hello=%u init=%u/%u clone=%u/%u reports=%lu errors=%lu in=%u out=%u stopped=%u\r\n",
                     i, pad->slot, pad->hello_seen, pad->init_step, XBOX_INIT_COUNT,
                     pad->clone_step, XBOX_CLONE_COUNT,
                     (unsigned long)pad->reports, (unsigned long)pad->errors,
                     pad->in_pending, pad->out_pending, pad->faulted);
            uart_puts(uart_base, line);
            xbox_rx_status(pad, line, sizeof(line));
            uart_puts(uart_base, line);
        }
    }
}

static const uint16_t xbox_one_ids[][2] = { { 0x045e, 0x02ea }, { 0, 0 } };
static const struct usbh_class_driver xbox_one_driver = {
    .driver_name = "xboxone",
    .connect = xbox_connect,
    .disconnect = xbox_disconnect
};
CLASS_INFO_DEFINE const struct usbh_class_info usb_xbox_one_class_info = {
    .match_flags = USB_CLASS_MATCH_VID_PID | USB_CLASS_MATCH_INTF_CLASS |
                   USB_CLASS_MATCH_INTF_SUBCLASS | USB_CLASS_MATCH_INTF_PROTOCOL |
                   USB_CLASS_MATCH_INTF_NUM,
    .bInterfaceClass = 0xff,
    .bInterfaceSubClass = 0x47,
    .bInterfaceProtocol = 0xd0,
    .bInterfaceNumber = 0,
    .id_table = xbox_one_ids,
    .class_driver = &xbox_one_driver
};
