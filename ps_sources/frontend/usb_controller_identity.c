/* SPDX-License-Identifier: GPL-3.0-only */
#include "usb_controller_identity.h"

#include <stdio.h>
#include <string.h>

static usb_controller_device_t g_controller_devices[USB_CONTROLLER_SLOT_COUNT];

void usb_controller_identity_init(void)
{
    memset(g_controller_devices, 0, sizeof(g_controller_devices));
}

uint8_t usb_controller_identity_valid(const usb_controller_identity_t *identity)
{
    return identity != NULL && identity->vendor_id != 0U &&
        identity->transport <= USB_CONTROLLER_TRANSPORT_PS4 &&
        (identity->serial_hash != 0U || identity->port_path != 0U);
}

uint8_t usb_controller_identity_equal(const usb_controller_identity_t *a,
                                      const usb_controller_identity_t *b)
{
    if (!usb_controller_identity_valid(a) || !usb_controller_identity_valid(b) ||
        a->vendor_id != b->vendor_id || a->product_id != b->product_id ||
        a->descriptor_hash != b->descriptor_hash ||
        a->transport != b->transport || a->interface_id != b->interface_id) {
        return 0U;
    }
    if (a->serial_hash != 0U || b->serial_hash != 0U) {
        return a->serial_hash != 0U && a->serial_hash == b->serial_hash;
    }
    return a->port_path == b->port_path;
}

uint32_t usb_controller_identity_hash(uint32_t hash, const void *bytes, size_t length)
{
    const uint8_t *data = (const uint8_t *)bytes;
    if (data != NULL) {
        for (size_t i = 0U; i < length; ++i) {
            hash = (hash ^ data[i]) * UINT32_C(16777619);
        }
    }
    return hash;
}

void usb_controller_identity_set(uint8_t slot, const usb_controller_device_t *device)
{
    if (slot >= USB_CONTROLLER_SLOT_COUNT) {
        return;
    }
    if (device == NULL || !device->connected) {
        usb_controller_identity_disconnect(slot);
        return;
    }
    g_controller_devices[slot] = *device;
    g_controller_devices[slot].connected = 1U;
    g_controller_devices[slot].generic_axes = device->generic_axes != 0U;
    g_controller_devices[slot].name[USB_CONTROLLER_NAME_BYTES - 1U] = '\0';
}

void usb_controller_identity_disconnect(uint8_t slot)
{
    if (slot < USB_CONTROLLER_SLOT_COUNT) {
        memset(&g_controller_devices[slot], 0, sizeof(g_controller_devices[slot]));
    }
}

uint8_t usb_controller_identity_get(uint8_t slot, usb_controller_device_t *device)
{
    if (device != NULL) {
        memset(device, 0, sizeof(*device));
    }
    if (slot >= USB_CONTROLLER_SLOT_COUNT || !g_controller_devices[slot].connected) {
        return 0U;
    }
    if (device != NULL) {
        *device = g_controller_devices[slot];
    }
    return 1U;
}

uint8_t usb_controller_identity_bind(uint8_t device, usb_controller_identity_t *identity)
{
    if (identity == NULL) {
        return 0U;
    }
    memset(identity, 0, sizeof(*identity));
    if (device > 0U && device <= USB_CONTROLLER_SLOT_COUNT) {
        const usb_controller_device_t *live = &g_controller_devices[device - 1U];
        if (live->connected && usb_controller_identity_valid(&live->identity)) {
            *identity = live->identity;
            return 1U;
        }
    }
    return 0U;
}

uint8_t usb_controller_identity_resolve(const usb_controller_identity_t *identity,
                                        uint8_t device, uint8_t connected_mask)
{
    if (usb_controller_identity_valid(identity)) {
        uint8_t match = USB_CONTROLLER_SLOT_COUNT;
        for (uint8_t slot = 0U; slot < USB_CONTROLLER_SLOT_COUNT; ++slot) {
            if ((connected_mask & (1U << slot)) != 0U &&
                g_controller_devices[slot].connected &&
                usb_controller_identity_equal(identity, &g_controller_devices[slot].identity)) {
                /* A duplicated serial cannot safely select either device. */
                if (match != USB_CONTROLLER_SLOT_COUNT) {
                    return USB_CONTROLLER_SLOT_COUNT;
                }
                match = slot;
            }
        }
        return match;
    }
    if (device != 0U) {
        const uint8_t slot = (uint8_t)(device - 1U);
        return slot < USB_CONTROLLER_SLOT_COUNT &&
            (connected_mask & (1U << slot)) != 0U ? slot : USB_CONTROLLER_SLOT_COUNT;
    }
    for (uint8_t slot = 0U; slot < USB_CONTROLLER_SLOT_COUNT; ++slot) {
        if ((connected_mask & (1U << slot)) != 0U &&
            (!g_controller_devices[slot].connected || !g_controller_devices[slot].generic_axes)) {
            return slot;
        }
    }
    return USB_CONTROLLER_SLOT_COUNT;
}

static uint8_t identity_hex(const char **text, unsigned digits, uint32_t *value)
{
    uint32_t parsed = 0U;
    for (unsigned i = 0U; i < digits; ++i) {
        const unsigned char c = (unsigned char)**text;
        unsigned nibble;
        if (c >= '0' && c <= '9') {
            nibble = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            nibble = c - 'a' + 10U;
        } else if (c >= 'A' && c <= 'F') {
            nibble = c - 'A' + 10U;
        } else {
            return 0U;
        }
        ++*text;
        parsed = (parsed << 4U) | nibble;
    }
    *value = parsed;
    return 1U;
}

uint8_t usb_controller_identity_parse(const char *text, usb_controller_identity_t *identity)
{
    static const unsigned widths[] = {4U, 4U, 2U, 2U, 8U, 8U, 8U};
    uint32_t values[7];
    usb_controller_identity_t parsed = {0};
    if (identity == NULL) {
        return 0U;
    }
    memset(identity, 0, sizeof(*identity));
    if (text == NULL) {
        return 0U;
    }
    if (strcmp(text, "None") == 0 || strcmp(text, "none") == 0 ||
        strcmp(text, "NONE") == 0) {
        return 1U;
    }
    for (unsigned i = 0U; i < 7U; ++i) {
        if (!identity_hex(&text, widths[i], &values[i])) {
            return 0U;
        }
        if (i < 6U && *text++ != ':') {
            return 0U;
        }
    }
    if (*text != '\0') {
        return 0U;
    }
    parsed.vendor_id = (uint16_t)values[0];
    parsed.product_id = (uint16_t)values[1];
    parsed.interface_id = (uint8_t)values[2];
    parsed.transport = (uint8_t)values[3];
    parsed.descriptor_hash = values[4];
    parsed.serial_hash = values[5];
    parsed.port_path = values[6];
    if (!usb_controller_identity_valid(&parsed)) {
        return 0U;
    }
    *identity = parsed;
    return 1U;
}

uint8_t usb_controller_identity_format(const usb_controller_identity_t *identity,
                                       char *text, size_t size)
{
    int length;
    if (text == NULL || size == 0U) {
        return 0U;
    }
    if (!usb_controller_identity_valid(identity)) {
        length = snprintf(text, size, "None");
    } else {
        length = snprintf(text, size, "%04X:%04X:%02X:%02X:%08lX:%08lX:%08lX",
            (unsigned)identity->vendor_id, (unsigned)identity->product_id,
            (unsigned)identity->interface_id, (unsigned)identity->transport,
            (unsigned long)identity->descriptor_hash, (unsigned long)identity->serial_hash,
            (unsigned long)identity->port_path);
    }
    if (length < 0 || (size_t)length >= size) {
        text[0] = '\0';
        return 0U;
    }
    return 1U;
}
