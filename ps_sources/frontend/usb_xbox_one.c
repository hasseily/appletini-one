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
#define XBOX_OUT_TIMEOUT_MS 1000U
#define XBOX_ACK_COUNT 4U
#define XBOX_INIT_COUNT 4U

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
    uint32_t reports;
    uint32_t errors;
    uint8_t active;
    uint8_t slot;
    uint8_t index;
    uint8_t in_pending;
    uint8_t out_pending;
    uint8_t in_retry;
    uint8_t out_retry;
    uint8_t in_halted;
    uint8_t out_halted;
    uint8_t init_step;
    uint8_t sequence;
    uint8_t tx_len;
    uint8_t tx_ack;
    uint8_t ack_sequence[XBOX_ACK_COUNT];
    uint8_t ack_count;
    uint8_t guide;
    uint8_t stick_buttons;
    uint8_t hat;
    uint8_t error_logged;
} xbox_one_t;

static xbox_one_t g_xbox[XBOX_COUNT];
/* Each DMA buffer occupies whole cache lines, separate from driver state. */
static uint8_t g_xbox_in[XBOX_COUNT][USB_ALIGN_UP(XBOX_PACKET_BYTES, CONFIG_USB_ALIGN_SIZE)]
    USB_MEM_ALIGNX;
static uint8_t g_xbox_out[XBOX_COUNT][USB_ALIGN_UP(XBOX_PACKET_BYTES, CONFIG_USB_ALIGN_SIZE)]
    USB_MEM_ALIGNX;

static void xbox_error(xbox_one_t *pad, int error)
{
    pad->errors++;
    if (pad->error_logged == 0U) {
        cherryusb_printf("[usb1] Xbox One transfer error slot=%u err=%d\r\n",
                         pad->slot, error);
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
    if (pad->tx_len != 0U && pad->tx_ack != 0U &&
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
        xbox_error(pad, -USB_ERR_NOMEM);
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
        pad->hat = xbox_hat(data[5]);
        pad->reports++;
        if (pad->reports == 1U) {
            cherryusb_printf("[usb1] Xbox One input active slot=%u joystick=1\r\n",
                             pad->slot);
        }
    } else {
        return;
    }
    usb_gamepad_input_report(pad->slot, report, pad->hat,
                             (uint8_t)(pad->guide | pad->stick_buttons));
}

static void xbox_in_complete(void *arg, int nbytes)
{
    xbox_one_t *pad = (xbox_one_t *)arg;
    pad->in_pending = 0U;
    if (pad->active == 0U) {
        return;
    }
    if (nbytes > 0 && nbytes <= (int)XBOX_PACKET_BYTES) {
        pad->error_logged = 0U;
        xbox_input(pad, g_xbox_in[pad->index], (uint32_t)nbytes);
    } else {
        if (nbytes == -USB_ERR_STALL) {
            pad->in_halted = 1U;
        }
        if (nbytes != 0) {
            xbox_error(pad, nbytes);
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
    if (pad->active == 0U) {
        return;
    }
    if (nbytes == (int)pad->tx_len) {
        if (pad->tx_ack == 0U) {
            pad->init_step++;
        }
        pad->tx_len = 0U;
    } else {
        if (nbytes == -USB_ERR_STALL) {
            pad->out_halted = 1U;
        }
        xbox_error(pad, nbytes < 0 ? nbytes : -USB_ERR_IO);
        pad->out_retry = 1U;
        pad->out_retry_at = cherryusb_baremetal_ms();
    }
}

static int xbox_clear_halt(xbox_one_t *pad, struct usbh_urb *urb)
{
    struct usb_setup_packet *setup = pad->hport->setup;
    int rc;

    /* Only poll performs control requests; completion callbacks just mark
     * the halted endpoint. The shared EP0 wait still pumps USB/background work. */
    setup->bmRequestType = USB_REQUEST_DIR_OUT | USB_REQUEST_STANDARD |
                           USB_REQUEST_RECIPIENT_ENDPOINT;
    setup->bRequest = USB_REQUEST_CLEAR_FEATURE;
    setup->wValue = USB_FEATURE_ENDPOINT_HALT;
    setup->wIndex = urb->ep->bEndpointAddress;
    setup->wLength = 0U;
    rc = usbh_control_transfer(pad->hport, setup, NULL);
    if (rc >= 0) {
        urb->data_toggle = 0U;
    } else {
        xbox_error(pad, rc);
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
    if (pad->active == 0U || !pad->hport->connected) {
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
        xbox_error(pad, rc);
    }
}

static void xbox_prepare_out(xbox_one_t *pad)
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

    if (pad->tx_len != 0U) {
        return; /* Retain the bytes and sequence through retries. */
    }
    if (pad->ack_count != 0U) {
        memcpy(data, ack, sizeof(ack));
        data[2] = pad->ack_sequence[0];
        pad->ack_count--;
        memmove(pad->ack_sequence, pad->ack_sequence + 1, pad->ack_count);
        pad->tx_len = sizeof(ack);
        pad->tx_ack = 1U;
    } else if (pad->init_step < XBOX_INIT_COUNT) {
        pad->tx_len = init_len[pad->init_step];
        memcpy(data, init[pad->init_step], pad->tx_len);
        data[2] = pad->sequence++;
        pad->tx_ack = 0U;
    }
}

static void xbox_poll_pad(xbox_one_t *pad, uint32_t now)
{
    int rc;
    xbox_submit_in(pad, now);
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
    /* Listen before waking the controller. Never block frontend polling. */
    if (pad->in_pending == 0U ||
        (pad->out_retry != 0U && (uint32_t)(now - pad->out_retry_at) < XBOX_RETRY_MS)) {
        return;
    }
    xbox_prepare_out(pad);
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
    if (pad->active == 0U || !pad->hport->connected) {
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
        xbox_error(pad, rc);
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
    pad->hport = hport;
    pad->in_ep = in_ep;
    pad->out_ep = out_ep;
    pad->hat = 8U;
    hport->config.intf[intf].priv = pad;
    snprintf(hport->config.intf[intf].devname, CONFIG_USBHOST_DEV_NAMELEN,
             "/dev/xboxone%u", pad->index);
    cherryusb_printf("[usb1] Xbox One connected slot=%u vid=%04x pid=%04x intf=0\r\n",
                     pad->slot, hport->device_desc.idVendor, hport->device_desc.idProduct);
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
        usb_gamepad_input_disconnect(pad->slot);
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
    char line[128];
    for (uint8_t i = 0U; i < XBOX_COUNT; ++i) {
        const xbox_one_t *pad = &g_xbox[i];
        if (pad->active != 0U) {
            snprintf(line, sizeof(line),
                     "xboxone%u: slot=%u init=%u/%u reports=%lu errors=%lu in=%u out=%u\r\n",
                     i, pad->slot, pad->init_step, XBOX_INIT_COUNT,
                     (unsigned long)pad->reports, (unsigned long)pad->errors,
                     pad->in_pending, pad->out_pending);
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
