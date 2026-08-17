#ifndef ONEE_USB_CONTROLS_H
#define ONEE_USB_CONTROLS_H

#include <stdint.h>

#include "onee_fixed_mode.h"
#include "usb_hid_service.h"
#include "usb_hid.h"

#define ONEE_USB_ROUTE_PUSH_NOW (1U << 0)

#define ONEE_USB_MODIFIER_LSHIFT ((uint8_t)(1U << \
    (HID_KBD_USAGE_LSHIFT - HID_KBD_USAGE_LCTRL)))
#define ONEE_USB_MODIFIER_RSHIFT ((uint8_t)(1U << \
    (HID_KBD_USAGE_RSHIFT - HID_KBD_USAGE_LCTRL)))
#define ONEE_USB_MODIFIER_SHIFT ((uint8_t)(ONEE_USB_MODIFIER_LSHIFT | \
                                           ONEE_USB_MODIFIER_RSHIFT))

static inline uint8_t onee_usb_fixed_usage_reserved(uint8_t usage)
{
    return (usage == HID_KBD_USAGE_PAUSE ||
            usage == HID_KBD_USAGE_PRINTSCN ||
            usage == HID_KBD_USAGE_KPD0 ||
            usage == HID_KBD_USAGE_KPDHMINUS ||
            usage == HID_KBD_USAGE_KPDPLUS) ? 1U : 0U;
}

static inline uint8_t onee_usb_apple_usage_allowed(uint8_t usage,
                                                    uint8_t onee_fixed_mode)
{
    return (onee_fixed_mode != 0U &&
            onee_usb_fixed_usage_reserved(usage) != 0U) ? 0U : 1U;
}

static inline uint8_t onee_usb_fixed_apple_modifier(
    uint8_t modifier,
    uint8_t printscreen_down)
{
    if (printscreen_down != 0U) {
        modifier &= (uint8_t)~ONEE_USB_MODIFIER_SHIFT;
    }
    return modifier;
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
    uint8_t modifier,
    uint8_t menu_capture)
{
    const uint8_t shift = (uint8_t)(modifier & ONEE_USB_MODIFIER_SHIFT);

    switch (usage) {
    case HID_KBD_USAGE_PAUSE:
        return USB_HID_MENU_ACTION_OPEN;
    case HID_KBD_USAGE_PRINTSCN:
        return (shift != 0U) ? USB_HID_MENU_ACTION_SCREENSHOT_1080P :
                               USB_HID_MENU_ACTION_SCREENSHOT_A2;
    case HID_KBD_USAGE_KPD0:
        return USB_HID_MENU_ACTION_VTW_SPEED_TOGGLE;
    case HID_KBD_USAGE_KPDPLUS:
        return USB_HID_MENU_ACTION_VTW_SPEED_UP;
    case HID_KBD_USAGE_KPDHMINUS:
        return USB_HID_MENU_ACTION_VTW_SPEED_DOWN;
    default:
        break;
    }

    if (menu_capture == 0U) {
        return USB_HID_MENU_ACTION_NONE;
    }
    switch (usage) {
    case HID_KBD_USAGE_PAGEUP:
        return USB_HID_MENU_ACTION_PREV_TAB;
    case HID_KBD_USAGE_PAGEDOWN:
        return USB_HID_MENU_ACTION_NEXT_TAB;
    case HID_KBD_USAGE_LEFT:
        return USB_HID_MENU_ACTION_LEFT;
    case HID_KBD_USAGE_RIGHT:
        return USB_HID_MENU_ACTION_RIGHT;
    case HID_KBD_USAGE_UP:
        return USB_HID_MENU_ACTION_ITEM_UP;
    case HID_KBD_USAGE_DOWN:
        return USB_HID_MENU_ACTION_ITEM_DOWN;
    case HID_KBD_USAGE_ENTER:
    case HID_KBD_USAGE_KPDEMTER:
        return USB_HID_MENU_ACTION_SELECT;
    case HID_KBD_USAGE_ESCAPE:
        return USB_HID_MENU_ACTION_CLOSE;
    default:
        return USB_HID_MENU_ACTION_NONE;
    }
}

static inline usb_hid_menu_action_t onee_usb_keyboard_action(
    uint8_t usage,
    uint8_t modifier,
    uint8_t menu_capture,
    uint8_t onee_fixed_mode)
{
    if (onee_fixed_mode == 0U) {
        return USB_HID_MENU_ACTION_NONE;
    }
    return onee_usb_fixed_keyboard_action(usage, modifier, menu_capture);
}

static inline uint8_t onee_usb_fixed_route(usb_hid_menu_action_t action)
{
    /* The fixed map has no saved-binding fallback. Break opens, Escape
     * closes, and every other action comes only from the fixed key table. */
    return (action != USB_HID_MENU_ACTION_NONE) ?
        ONEE_USB_ROUTE_PUSH_NOW : 0U;
}

#endif /* ONEE_USB_CONTROLS_H */
