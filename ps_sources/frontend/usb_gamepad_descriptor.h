/* SPDX-License-Identifier: GPL-3.0-only */
/* Adapted from Multitini One 6f2a90286956bf8e4956dfc9b94c94dfc3335b05. */
#ifndef APPLETINI_USB_GAMEPAD_DESCRIPTOR_H
#define APPLETINI_USB_GAMEPAD_DESCRIPTOR_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "usbh_hid.h"

#define APPLETINI_USB_HID_MAX_APPLICATIONS 16U
#define APPLETINI_USB_HID_MAX_COLLECTION_DEPTH 16U
#define APPLETINI_USB_HID_MAX_KEYBOARD_ITEMS 16U
#define APPLETINI_USB_HID_MAX_KEYBOARD_USAGES 64U
#define APPLETINI_USB_HID_NO_APPLICATION UINT8_MAX
enum {
    APPLETINI_USB_HID_APPLICATION_GAMEPAD = 1U,
    APPLETINI_USB_HID_APPLICATION_AXIS = 2U,
    APPLETINI_USB_HID_APPLICATION_RELATIVE = 4U,
    APPLETINI_USB_HID_APPLICATION_HAT = 8U,
    APPLETINI_USB_HID_APPLICATION_KEYBOARD = 16U,
    APPLETINI_USB_HID_APPLICATION_MOUSE = 32U,
    APPLETINI_USB_HID_APPLICATION_BUTTONS = 64U
};
typedef struct {
    uint16_t usage_page, usage;
    uint8_t capabilities;
} appletini_usb_hid_application_t;
typedef struct {
    struct usbh_hid_report_item item;
    uint8_t application, usage_index, usage_count;
} appletini_usb_hid_keyboard_item_t;
typedef struct {
    appletini_usb_hid_application_t applications[APPLETINI_USB_HID_MAX_APPLICATIONS];
    uint8_t application_count;
    /* Index in applications[], for each retained report_items[] entry. */
    uint8_t field_application[CONFIG_USB_HID_MAX_REPORT_ITEMS];
    /* Keyboard arrays/bitmaps are compact chunks, outside the axis budget.
     * usage_count==0 selects the item's range; otherwise use keyboard_usages.
     * Explicit usages preserve their order, including nonconsecutive keys. */
    appletini_usb_hid_keyboard_item_t keyboard_items[APPLETINI_USB_HID_MAX_KEYBOARD_ITEMS];
    uint16_t keyboard_usages[APPLETINI_USB_HID_MAX_KEYBOARD_USAGES];
    uint8_t keyboard_item_count, keyboard_usage_count;
    uint16_t input_bits[256];
} appletini_usb_hid_descriptor_meta_t;

/* For a Variable item, index is the field number. For an Array, it is the
 * reported selector minus logical_min (validate that subtraction first).
 * Variables repeat the last usage; out-of-range array selectors mean no key. */
static inline uint16_t appletini_usb_hid_keyboard_usage(const appletini_usb_hid_descriptor_meta_t *meta,
    const appletini_usb_hid_keyboard_item_t *keyboard, uint32_t index)
{
    const struct usbh_hid_report_item *item = &keyboard->item;
    const uint8_t variable = (item->report_flags & HID_MAINITEM_VARIABLE) != 0U;
    if (keyboard->usage_count != 0U) {
        if (index >= keyboard->usage_count) {
            if (!variable) return 0;
            index = keyboard->usage_count - 1U;
        }
        return meta->keyboard_usages[keyboard->usage_index + index];
    }
    const uint32_t count = (uint32_t)item->attribute.usage_max - item->attribute.usage_min + 1U;
    if (index >= count) {
        if (!variable) return 0;
        index = count - 1U;
    }
    return (uint16_t)(item->attribute.usage_min + index);
}

static inline int appletini_usb_hid_add_keyboard_item(appletini_usb_hid_descriptor_meta_t *meta,
    const struct usbh_hid_report_item_attribute *globals, uint32_t flags,
    uint32_t offset, uint32_t count, uint8_t application,
    const uint32_t *usages, unsigned usage_count, uint16_t usage_min, uint16_t usage_max)
{
    if (meta->keyboard_item_count == APPLETINI_USB_HID_MAX_KEYBOARD_ITEMS ||
        usage_count > APPLETINI_USB_HID_MAX_KEYBOARD_USAGES - meta->keyboard_usage_count) return -1;
    appletini_usb_hid_keyboard_item_t *keyboard = &meta->keyboard_items[meta->keyboard_item_count++];
    keyboard->item.report_type = HID_REPORT_INPUT;
    keyboard->item.report_flags = (uint16_t)flags;
    keyboard->item.report_bit_offset = offset;
    keyboard->item.attribute = *globals;
    keyboard->item.attribute.report_count = count;
    keyboard->item.attribute.usage_page = HID_USAGE_PAGE_KEYBOARD_KEYPAD;
    keyboard->item.attribute.usage_min = usage_min;
    keyboard->item.attribute.usage_max = usage_max;
    keyboard->application = application;
    keyboard->usage_index = meta->keyboard_usage_count;
    keyboard->usage_count = (uint8_t)usage_count;
    for (unsigned i = 0; i < usage_count; ++i)
        meta->keyboard_usages[meta->keyboard_usage_count++] =
            (usages[i] >> 16U) == HID_USAGE_PAGE_KEYBOARD_KEYPAD ? (uint16_t)usages[i] : 0;
    return 0;
}

static inline int appletini_usb_hid_retain_keyboard(appletini_usb_hid_descriptor_meta_t *meta,
    const struct usbh_hid_report_item_attribute *globals, uint32_t flags,
    uint32_t offset, uint8_t application, const uint32_t *usages,
    unsigned usage_count, uint32_t usage_min, uint32_t usage_max, uint8_t have_range)
{
    if (globals->report_count == 0U) return 0;
    if (usage_count == 0U) {
        if (have_range != 3U || usage_max < usage_min ||
            (usage_min >> 16U) != HID_USAGE_PAGE_KEYBOARD_KEYPAD ||
            (usage_max >> 16U) != HID_USAGE_PAGE_KEYBOARD_KEYPAD) return 0;
        return appletini_usb_hid_add_keyboard_item(meta, globals, flags, offset,
            globals->report_count, application, NULL, 0, (uint16_t)usage_min, (uint16_t)usage_max);
    }
    if ((flags & HID_MAINITEM_VARIABLE) == 0U) {
        unsigned key_usages = 0;
        for (unsigned i = 0; i < usage_count; ++i)
            if ((usages[i] >> 16U) == HID_USAGE_PAGE_KEYBOARD_KEYPAD) ++key_usages;
        if (key_usages == 0U) return 0;
        return appletini_usb_hid_add_keyboard_item(meta, globals, flags, offset,
            globals->report_count, application, usages, usage_count, 0, 0);
    }
    /* Explicit variable fields may mix pages. Keep each run of keyboard
     * fields with its original bit offset; other fields stay in their path. */
    for (uint32_t field = 0; field < globals->report_count;) {
        const uint32_t first = field;
        const unsigned usage_index = field < usage_count ? field : usage_count - 1U;
        if ((usages[usage_index] >> 16U) != HID_USAGE_PAGE_KEYBOARD_KEYPAD) { ++field; continue; }
        do {
            ++field;
        } while (field < globals->report_count &&
            (usages[field < usage_count ? field : usage_count - 1U] >> 16U) == HID_USAGE_PAGE_KEYBOARD_KEYPAD);
        const unsigned available = usage_count - usage_index;
        const unsigned retained = field - first < available ? field - first : available;
        if (appletini_usb_hid_add_keyboard_item(meta, globals, flags,
            offset + first * globals->report_size, field - first, application,
            usages + usage_index, retained, 0, 0) < 0) return -1;
    }
    return 0;
}

static inline uint8_t appletini_usb_hid_application_kind(uint32_t usage)
{
    if ((usage >> 16U) != HID_USAGE_PAGE_GENERIC_DESKTOP_CONTROLS) return 0;
    switch ((uint16_t)usage) {
    case HID_DESKTOP_USAGE_JOYSTICK:
    case HID_DESKTOP_USAGE_GAMEPAD:
    case HID_DESKTOP_USAGE_MULTIAXIS: return APPLETINI_USB_HID_APPLICATION_GAMEPAD;
    case HID_DESKTOP_USAGE_KEYBOARD:
    case HID_DESKTOP_USAGE_KEYPAD: return APPLETINI_USB_HID_APPLICATION_KEYBOARD;
    case HID_DESKTOP_USAGE_MOUSE: return APPLETINI_USB_HID_APPLICATION_MOUSE;
    default: return 0;
    }
}

/* Discover applications before retaining fields: a wheel can follow hundreds
 * of unrelated keys or precede the application's buttons in another report.
 * Collection ancestry is separate from global Push/Pop. Every main data item
 * advances its own report ID/type offset even when it has no bindable fields. */
static inline int appletini_usb_hid_descriptor_pass(const uint8_t *data,
    uint32_t length, struct usbh_hid_report_info *info,
    appletini_usb_hid_descriptor_meta_t *meta, uint8_t retain)
{
    struct usbh_hid_report_item_attribute globals = {0}, stack[8];
    uint32_t offsets[256][3] = {{0}};
    uint32_t usages[64], usage_min = 0, usage_max = 0;
    unsigned usage_count = 0, stack_count = 0, depth = 0;
    uint8_t ancestry[APPLETINI_USB_HID_MAX_COLLECTION_DEPTH];
    uint8_t application = APPLETINI_USB_HID_NO_APPLICATION, application_count = 0;
    uint8_t have_range = 0;
    for (uint32_t pos = 0; pos < length;) {
        const uint8_t prefix = data[pos++];
        if (prefix == 0xfeU) {
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
            case 3: globals.physical_min = (uint32_t)signed_value; break;
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
            case 3: case 4: case 5: case 7: case 8: case 9: break;
            default: return -1;
            }
        } else if (type == 0U) {
            if (tag == 10U) {
                if (depth == APPLETINI_USB_HID_MAX_COLLECTION_DEPTH) return -1;
                ancestry[depth++] = application;
                if (value == 1U) { /* Application collection. */
                    if (application_count == APPLETINI_USB_HID_MAX_APPLICATIONS) return -1;
                    application = application_count++;
                    if (!retain) {
                        const uint32_t usage = usage_count != 0U ? usages[0] :
                            (have_range & 1U) != 0U ? usage_min : 0;
                        appletini_usb_hid_application_t *app = &meta->applications[application];
                        app->usage_page = (uint16_t)(usage >> 16U);
                        app->usage = (uint16_t)usage;
                        app->capabilities = appletini_usb_hid_application_kind(usage);
                        meta->application_count = application_count;
                    }
                }
            } else if (tag == 12U) {
                if (depth == 0U) return -1;
                application = ancestry[--depth];
            } else if (tag == 8U || tag == 9U || tag == 11U) {
                const unsigned report_type = tag == 8U ? 0U : tag == 9U ? 1U : 2U;
                uint32_t *offset = &offsets[globals.report_id][report_type];
                const uint32_t bits = globals.report_count * globals.report_size;
                if (bits > 32768U || *offset > 32768U - bits) return -1;
                if (tag == 8U && (value & HID_MAINITEM_CONSTANT) == 0U &&
                    globals.report_size != 0U && application != APPLETINI_USB_HID_NO_APPLICATION) {
                    appletini_usb_hid_application_t *app = &meta->applications[application];
                    if (retain && appletini_usb_hid_retain_keyboard(meta, &globals, value,
                        *offset, application, usages, usage_count, usage_min, usage_max, have_range) < 0) return -1;
                    for (uint32_t field = 0; field < globals.report_count; ++field) {
                        uint32_t usage = 0;
                        if (usage_count != 0U)
                            usage = usages[field < usage_count ? field : usage_count - 1U];
                        else if (have_range == 3U && usage_max >= usage_min)
                            usage = field <= usage_max - usage_min ? usage_min + field : usage_max;
                        const uint16_t page = (uint16_t)(usage >> 16U);
                        const uint16_t id = (uint16_t)usage;
                        const uint8_t variable = (value & HID_MAINITEM_VARIABLE) != 0U;
                        const uint8_t relative = (value & HID_MAINITEM_RELATIVE) != 0U;
                        const uint8_t button = page == HID_USAGE_PAGE_BUTTON && !relative &&
                            (variable ? id >= 1U && id <= 32U :
                             have_range == 3U && (usage_min >> 16U) == HID_USAGE_PAGE_BUTTON &&
                             (usage_max >> 16U) == HID_USAGE_PAGE_BUTTON &&
                             (uint16_t)usage_min <= 32U && (uint16_t)usage_max >= 1U);
                        const uint8_t hat = page == HID_USAGE_PAGE_GENERIC_DESKTOP_CONTROLS &&
                            variable && !relative && (id == HID_DESKTOP_USAGE_HATSWITCH ||
                             (id >= HID_DESKTOP_USAGE_DPAD_UP && id <= HID_DESKTOP_USAGE_DPAD_LEFT));
                        const uint8_t axis = variable &&
                            ((page == HID_USAGE_PAGE_GENERIC_DESKTOP_CONTROLS &&
                              id >= HID_DESKTOP_USAGE_X && id <= HID_DESKTOP_USAGE_WHEEL) ||
                             (page == 2U && (id == 0xc4U || id == 0xc5U)) ||
                             (page == 0x0cU && id == 0x0238U)); /* Consumer AC Pan. */
                        if (!button && !axis && !hat) continue;
                        if (!retain) {
                            if (button) app->capabilities |= APPLETINI_USB_HID_APPLICATION_BUTTONS;
                            if (hat) app->capabilities |= APPLETINI_USB_HID_APPLICATION_HAT;
                            if (axis) app->capabilities |= APPLETINI_USB_HID_APPLICATION_AXIS;
                            if (axis && relative) app->capabilities |= APPLETINI_USB_HID_APPLICATION_RELATIVE;
                            continue;
                        }
                        if ((app->capabilities & (APPLETINI_USB_HID_APPLICATION_GAMEPAD |
                            APPLETINI_USB_HID_APPLICATION_AXIS | APPLETINI_USB_HID_APPLICATION_HAT)) == 0U) continue;
                        if (info->report_item_count == CONFIG_USB_HID_MAX_REPORT_ITEMS) return -1;
                        const uint32_t index = info->report_item_count++;
                        struct usbh_hid_report_item *item = &info->report_items[index];
                        item->report_type = HID_REPORT_INPUT;
                        item->report_flags = (uint16_t)value;
                        item->report_bit_offset = *offset + field * globals.report_size;
                        item->attribute = globals;
                        item->attribute.report_count = 1U;
                        item->attribute.usage_page = page;
                        item->attribute.usage_min = variable ? id : (uint16_t)usage_min;
                        item->attribute.usage_max = variable ? id : (uint16_t)usage_max;
                        meta->field_application[index] = application;
                    }
                }
                *offset += bits;
                if (retain && tag == 8U) meta->input_bits[globals.report_id] = (uint16_t)*offset;
            } else return -1;
            usage_count = 0;
            have_range = 0;
            usage_min = usage_max = 0;
        } else return -1;
    }
    return stack_count == 0U && depth == 0U ? 0 : -1;
}

/* Retain gamepad controls and axes/buttons from any axis-bearing application,
 * including relative mouse/rotary controls. Keyboard-only auxiliary buttons
 * are not controllers. Metadata preserves collection identity for dispatch. */
static inline int appletini_usb_parse_input_descriptor(const uint8_t *data,
    uint32_t length, struct usbh_hid_report_info *info,
    appletini_usb_hid_descriptor_meta_t *meta)
{
    appletini_usb_hid_descriptor_meta_t local_meta;
    if (info == NULL) return -1;
    if (meta == NULL) meta = &local_meta;
    memset(info, 0, sizeof(*info));
    memset(meta, 0, sizeof(*meta));
    memset(meta->field_application, APPLETINI_USB_HID_NO_APPLICATION, sizeof(meta->field_application));
    if (data == NULL || appletini_usb_hid_descriptor_pass(data, length, info, meta, 0) < 0 ||
        appletini_usb_hid_descriptor_pass(data, length, info, meta, 1) < 0) {
        memset(info, 0, sizeof(*info));
        memset(meta, 0, sizeof(*meta));
        memset(meta->field_application, APPLETINI_USB_HID_NO_APPLICATION, sizeof(meta->field_application));
        return -1;
    }
    return 0;
}

static inline int appletini_usb_parse_gamepad_descriptor(const uint8_t *data,
    uint32_t length, struct usbh_hid_report_info *info)
{
    appletini_usb_hid_descriptor_meta_t meta;
    if (appletini_usb_parse_input_descriptor(data, length, info, &meta) < 0) return -1;
    for (unsigned i = 0; i < meta.application_count; ++i)
        if (meta.applications[i].capabilities & APPLETINI_USB_HID_APPLICATION_GAMEPAD) return 1;
    return 0;
}

#endif
