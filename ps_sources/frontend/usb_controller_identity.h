/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef USB_CONTROLLER_IDENTITY_H
#define USB_CONTROLLER_IDENTITY_H

#include <stddef.h>
#include <stdint.h>

#define USB_CONTROLLER_SLOT_COUNT 8U
#define USB_CONTROLLER_NAME_BYTES 64U
#define USB_CONTROLLER_IDENTITY_TEXT_BYTES 48U

enum {
    USB_CONTROLLER_TRANSPORT_HID,
    USB_CONTROLLER_TRANSPORT_XBOX360,
    USB_CONTROLLER_TRANSPORT_XBOXONE,
    USB_CONTROLLER_TRANSPORT_PS4
};

typedef struct {
    uint16_t vendor_id, product_id;
    uint32_t descriptor_hash, serial_hash, port_path;
    uint8_t transport, interface_id;
} usb_controller_identity_t;

typedef struct {
    usb_controller_identity_t identity;
    uint32_t buttons_present;
    uint16_t axis_mask;
    uint8_t connected, generic_axes;
    char name[USB_CONTROLLER_NAME_BYTES];
} usb_controller_device_t;

/* Enumeration slots are transient. A saved identity follows a serial-bearing
 * device between ports, or a device without a serial on its saved hub path.
 * A missing saved device never falls back to another controller. */
void usb_controller_identity_init(void);
void usb_controller_identity_set(uint8_t slot, const usb_controller_device_t *device);
void usb_controller_identity_disconnect(uint8_t slot);
uint8_t usb_controller_identity_get(uint8_t slot, usb_controller_device_t *device);
uint8_t usb_controller_identity_valid(const usb_controller_identity_t *identity);
uint8_t usb_controller_identity_equal(const usb_controller_identity_t *a,
                                      const usb_controller_identity_t *b);
uint32_t usb_controller_identity_hash(uint32_t hash, const void *bytes, size_t length);

/* device is the old setting: 0 Auto, 1..8 an explicit input slot. Binding
 * Auto clears the identity. Old files without identities retain slot rules.
 * resolve returns USB_CONTROLLER_SLOT_COUNT when no matching input is live.
 * Auto ignores generic axis devices; explicit bindings can select them. */
uint8_t usb_controller_identity_bind(uint8_t device, usb_controller_identity_t *identity);
uint8_t usb_controller_identity_resolve(const usb_controller_identity_t *identity,
                                        uint8_t device, uint8_t connected_mask);

/* Persist one bounded field alongside the old slot setting. Invalid text
 * clears the destination and fails; None represents a legacy slot binding. */
uint8_t usb_controller_identity_parse(const char *text, usb_controller_identity_t *identity);
uint8_t usb_controller_identity_format(const usb_controller_identity_t *identity,
                                       char *text, size_t size);

#endif /* USB_CONTROLLER_IDENTITY_H */
