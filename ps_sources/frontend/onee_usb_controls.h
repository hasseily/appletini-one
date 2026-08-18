#ifndef ONEE_USB_CONTROLS_H
#define ONEE_USB_CONTROLS_H

#include <stdint.h>

#include "onee_fixed_mode.h"
#include "usb_hid_service.h"
#include "usb_hid.h"

#define ONEE_USB_ROUTE_PUSH_NOW (1U << 0)
#define ONEE_USB_ROUTE_CONSUME_SOURCE (1U << 1)

#define ONEE_USB_MODIFIER_LGUI ((uint8_t)(1U << \
    (HID_KBD_USAGE_LGUI - HID_KBD_USAGE_LCTRL)))
#define ONEE_USB_MODIFIER_RGUI ((uint8_t)(1U << \
    (HID_KBD_USAGE_RGUI - HID_KBD_USAGE_LCTRL)))
#define ONEE_USB_MODIFIER_GUI ((uint8_t)(ONEE_USB_MODIFIER_LGUI | \
                                         ONEE_USB_MODIFIER_RGUI))

static inline uint8_t onee_usb_fixed_usage_reserved(uint8_t usage)
{
    return (usage == HID_KBD_USAGE_PAUSE ||
            usage == HID_KBD_USAGE_LALT ||
            usage == HID_KBD_USAGE_RALT) ? 1U : 0U;
}

static inline uint8_t onee_usb_apple_usage_allowed(uint8_t usage,
                                                    uint8_t onee_fixed_mode)
{
    return (onee_fixed_mode != 0U &&
            onee_usb_fixed_usage_reserved(usage) != 0U) ? 0U : 1U;
}

static inline uint8_t onee_usb_fixed_apple_modifier(
    uint8_t modifier)
{
    /* ONE//e assigns the Apple keys to Alt only. The GUI keys remain
     * ordinary host keys and do not assert either Apple key. */
    return (uint8_t)(modifier & (uint8_t)~ONEE_USB_MODIFIER_GUI);
}

static inline int8_t onee_usb_axis_direction(int32_t value,
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

static inline uint8_t onee_usb_hat_active(int32_t value)
{
    return (value >= 0 && value <= 7) ? 1U : 0U;
}

static inline usb_hid_menu_action_t onee_usb_fixed_keyboard_action(
    uint8_t usage,
    uint8_t menu_capture)
{
    if (usage != HID_KBD_USAGE_PAUSE) {
        return USB_HID_MENU_ACTION_NONE;
    }
    return (menu_capture != 0U) ? USB_HID_MENU_ACTION_CLOSE :
                                  USB_HID_MENU_ACTION_OPEN;
}

static inline usb_hid_menu_action_t onee_usb_keyboard_action(
    uint8_t usage,
    uint8_t menu_capture,
    uint8_t onee_fixed_mode)
{
    if (onee_fixed_mode == 0U) {
        return USB_HID_MENU_ACTION_NONE;
    }
    return onee_usb_fixed_keyboard_action(usage, menu_capture);
}

static inline uint8_t onee_usb_fixed_route(
    uint8_t usage,
    usb_hid_menu_action_t action)
{
    uint8_t route = 0U;

    if (onee_usb_fixed_usage_reserved(usage) != 0U) {
        route |= ONEE_USB_ROUTE_CONSUME_SOURCE;
    }
    if (action != USB_HID_MENU_ACTION_NONE) {
        route |= ONEE_USB_ROUTE_PUSH_NOW;
    }
    return route;
}

#endif /* ONEE_USB_CONTROLS_H */
