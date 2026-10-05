/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef APPLETINI_USB_GAMEPAD_DESCRIPTOR_H
#define APPLETINI_USB_GAMEPAD_DESCRIPTOR_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "usbh_hid.h"

/* Adapted from Multitini One 1c4f8be (GPL-3.0-only).
 * Returns 1 for a Game Pad/Joystick application collection, 0 otherwise.
 * Keyboard and mouse fields on composite interfaces retain their positions.
 *
 * The transport parser's usage range loses explicit, nonconsecutive usages.
 * Retain each gamepad input field and each report ID's independent bit offset.
 * Output, feature, constant and unrelated fields still advance their reports.
 * Bounds are deliberate: 32 buttons, six axes and D-pad fit the input budget. */
static inline int appletini_usb_parse_gamepad_descriptor(const uint8_t *data,
    uint32_t length, struct usbh_hid_report_info *info)
{
    struct usbh_hid_report_item_attribute globals = {0}, stack[8];
    uint32_t offsets[256][3] = {{0}};
    uint32_t usages[64], usage_min = 0, usage_max = 0;
    unsigned usage_count = 0, stack_count = 0;
    uint8_t have_range = 0, gamepad_collection = 0;
    if (data == NULL || info == NULL) return -1;
    memset(info, 0, sizeof(*info));
    for (uint32_t pos = 0; pos < length;) {
        const uint8_t prefix = data[pos++];
        if (prefix == 0xfeU) { /* Long items are reserved, never read past them. */
            if (length - pos < 2U || data[pos] > length - pos - 2U) return -1;
            pos += (uint32_t)data[pos] + 2U;
            continue;
        }
        unsigned size = prefix & 3U;
        if (size == 3U) size = 4U;
        if (size > length - pos) return -1;
        uint32_t value = 0;
        for (unsigned byte = 0; byte < size; ++byte)
            value |= (uint32_t)data[pos + byte] << (8U * byte);
        pos += size;
        const unsigned type = (prefix >> 2U) & 3U, tag = prefix >> 4U;
        if (type == 1U) {
            int32_t signed_value = (int32_t)value;
            if (size != 0U && size < 4U && (value & (UINT32_C(1) << (size * 8U - 1U))))
                signed_value = (int32_t)(value | (UINT32_MAX << (size * 8U)));
            switch (tag) {
            case 0: globals.usage_page = (uint16_t)value; break;
            case 1: globals.logical_min = signed_value; break;
            case 2: globals.logical_max = globals.logical_min < 0 ? signed_value : (int32_t)value; break;
            case 3: globals.physical_min = signed_value; break;
            case 4: globals.physical_max = value; break;
            case 5: globals.unit_exponent = value; break;
            case 6: globals.unit = value; break;
            case 7: if (value > 32U) return -1; globals.report_size = (uint8_t)value; break;
            case 8:
                if (value == 0U || value > 255U) return -1;
                globals.report_id = (uint8_t)value; info->using_report_id = true; break;
            case 9: if (value > 4096U) return -1; globals.report_count = value; break;
            case 10:
                if (stack_count == 8U) return -1;
                stack[stack_count++] = globals; break;
            case 11:
                if (stack_count == 0U) return -1;
                globals = stack[--stack_count]; break;
            default: return -1;
            }
        } else if (type == 2U) {
            const uint32_t usage = size == 4U ? value :
                ((uint32_t)globals.usage_page << 16U) | value;
            switch (tag) {
            case 0:
                if (usage_count == 64U) return -1;
                usages[usage_count++] = usage; break;
            case 1: usage_min = usage; have_range = 1; break;
            case 2: usage_max = usage; have_range |= 2; break;
            /* Designators and strings do not affect input field positions. */
            case 3: case 4: case 5: case 7: case 8: case 9: break;
            default: return -1;
            }
        } else if (type == 0U) {
            if (tag == 8U || tag == 9U || tag == 11U) {
                const unsigned report_type = tag == 8U ? 0U : tag == 9U ? 1U : 2U;
                uint32_t *offset = &offsets[globals.report_id][report_type];
                const uint32_t bits = globals.report_count * globals.report_size;
                if (bits > 32768U || *offset > 32768U - bits) return -1;
                if (tag == 8U && (value & HID_MAINITEM_CONSTANT) == 0U) {
                    for (uint32_t field = 0; field < globals.report_count; ++field) {
                        uint32_t usage = 0;
                        if (usage_count != 0U)
                            usage = usages[field < usage_count ? field : usage_count - 1U];
                        else if (have_range == 3U && usage_max >= usage_min)
                            usage = field <= usage_max - usage_min ? usage_min + field : usage_max;
                        const uint16_t page = (uint16_t)(usage >> 16U);
                        const uint16_t id = (uint16_t)usage;
                        const uint8_t variable = (value & HID_MAINITEM_VARIABLE) != 0U;
                        const uint8_t button = page == HID_USAGE_PAGE_BUTTON &&
                            (variable ? id >= 1U && id <= 32U :
                             have_range == 3U && (uint16_t)usage_min <= 32U && (uint16_t)usage_max >= 1U);
                        const uint8_t desktop = page == HID_USAGE_PAGE_GENERIC_DESKTOP_CONTROLS && variable &&
                            ((id >= HID_DESKTOP_USAGE_X && id <= HID_DESKTOP_USAGE_RZ) ||
                             id == HID_DESKTOP_USAGE_HATSWITCH || id == HID_DESKTOP_USAGE_WHEEL ||
                             (id >= HID_DESKTOP_USAGE_DPAD_UP && id <= HID_DESKTOP_USAGE_DPAD_LEFT));
                        const uint8_t trigger = page == 2U && variable && (id == 0xc4U || id == 0xc5U);
                        const uint8_t keyboard = page == HID_USAGE_PAGE_KEYBOARD_KEYPAD;
                        if (!button && !desktop && !trigger && !keyboard) continue;
                        if (info->report_item_count == CONFIG_USB_HID_MAX_REPORT_ITEMS) return -1;
                        struct usbh_hid_report_item *item = &info->report_items[info->report_item_count++];
                        item->report_type = HID_REPORT_INPUT;
                        item->report_flags = (uint16_t)value;
                        item->report_bit_offset = *offset + field * globals.report_size;
                        item->attribute = globals;
                        item->attribute.report_count = 1U;
                        item->attribute.usage_page = page;
                        item->attribute.usage_min = variable ? id : (uint16_t)usage_min;
                        item->attribute.usage_max = variable ? id : (uint16_t)usage_max;
                    }
                }
                *offset += bits;
            } else if (tag == 10U) {
                /* Application collection: Joystick or Game Pad. Keep this
                 * identity for button-only pads with no axes or hat. */
                if (value == 1U && usage_count != 0U &&
                    (usages[0] == 0x00010004U || usages[0] == 0x00010005U))
                    gamepad_collection = 1U;
            } else if (tag != 12U) return -1;
            usage_count = 0;
            have_range = 0;
            usage_min = usage_max = 0;
        } else return -1;
    }
    return stack_count == 0U ? gamepad_collection : -1;
}

#endif
