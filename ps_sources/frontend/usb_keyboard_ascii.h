#ifndef USB_KEYBOARD_ASCII_H
#define USB_KEYBOARD_ASCII_H

#include <stdint.h>
#include "usb_hid.h"

/* USB menu text uses the same US layout as the ONE//e keyboard bridge. */
static inline uint8_t usb_keyboard_text_from_usage(uint8_t usage,
                                                    uint8_t modifier,
                                                    uint8_t caps_lock)
{
    static const char numbers[] = "1234567890";
    static const char shifted_numbers[] = "!@#$%^&*()";
    static const char punctuation[] = "-=[]\\#;'`,./";
    static const char shifted_punctuation[] = "_+{}|~:\"~<>?";
    const uint8_t shift = (modifier & 0x22U) != 0U;

    /* Control, Alt and GUI chords are not printable text. */
    if ((modifier & 0xDDU) != 0U) {
        return 0U;
    }
    if (usage >= HID_KBD_USAGE_A && usage < HID_KBD_USAGE_A + 26U) {
        return (uint8_t)(((shift ^ caps_lock) != 0U ? 'A' : 'a') +
                          usage - HID_KBD_USAGE_A);
    }
    if (usage >= HID_KBD_USAGE_1 && usage <= HID_KBD_USAGE_0) {
        return (uint8_t)(shift ? shifted_numbers[usage - HID_KBD_USAGE_1] :
                                numbers[usage - HID_KBD_USAGE_1]);
    }
    if (usage >= HID_KBD_USAGE_HYPHEN && usage <= HID_KBD_USAGE_DIV) {
        return (uint8_t)(shift ? shifted_punctuation[usage - HID_KBD_USAGE_HYPHEN] :
                                punctuation[usage - HID_KBD_USAGE_HYPHEN]);
    }
    switch (usage) {
    case HID_KBD_USAGE_ENTER:
    case HID_KBD_USAGE_KPDEMTER: return '\r';
    case HID_KBD_USAGE_ESCAPE: return 0x1BU;
    case HID_KBD_USAGE_DELETE:
    case HID_KBD_USAGE_DELFWD: return '\b';
    case HID_KBD_USAGE_SPACE: return ' ';
    case HID_KBD_USAGE_KPDDIV: return '/';
    case HID_KBD_USAGE_KPDMUL: return '*';
    case HID_KBD_USAGE_KPDHMINUS: return '-';
    case HID_KBD_USAGE_KPDPLUS: return '+';
    case HID_KBD_USAGE_KPD0: return '0';
    case HID_KBD_USAGE_KPDDECIMALPT: return '.';
    default:
        if (usage >= HID_KBD_USAGE_KPD1 && usage < HID_KBD_USAGE_KPD1 + 9U) {
            return (uint8_t)('1' + usage - HID_KBD_USAGE_KPD1);
        }
        return 0U;
    }
}

#endif /* USB_KEYBOARD_ASCII_H */
