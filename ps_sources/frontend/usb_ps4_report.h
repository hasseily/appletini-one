#ifndef USB_PS4_REPORT_H
#define USB_PS4_REPORT_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "usb_gamepad_input.h"

/* Wired DualShock 4 input report 1. The HID descriptor lists X/Y/Z/Rz
 * together, which CherryUSB reduces to a usage range. Decode the known
 * Sony layout so right-stick Y cannot become a trigger axis.
 * Protocol reference: TinyUSB examples/host/hid_controller/src/hid_app.c
 * and Linux drivers/hid/hid-playstation.c. */
static inline uint8_t usb_ps4_supported(uint16_t vid, uint16_t pid)
{
    return (uint8_t)(vid == 0x054CU &&
                     (pid == 0x05C4U || pid == 0x09CCU));
}

static inline uint8_t usb_ps4_decode(
    const uint8_t *data, uint32_t len,
    onee_input_joystick_report_t *report,
    uint8_t *hat, uint8_t *extra_active)
{
    if (data == NULL || report == NULL || hat == NULL ||
        extra_active == NULL || len != 64U || data[0] != 1U) {
        return 0U;
    }

    memset(report, 0, sizeof(*report));
    report->axis_valid_mask = (uint8_t)((1U << ONEE_INPUT_AXIS_COUNT) - 1U);
    report->buttons_valid = 1U;
    report->axis[ONEE_INPUT_AXIS_X] = data[1];
    report->axis[ONEE_INPUT_AXIS_Y] = data[2];
    report->axis[ONEE_INPUT_AXIS_RX] = data[3];
    report->axis[ONEE_INPUT_AXIS_RY] = data[4];
    report->axis[ONEE_INPUT_AXIS_Z] = data[8];
    report->axis[ONEE_INPUT_AXIS_RZ] = data[9];
    for (uint8_t axis = 0U; axis < ONEE_INPUT_AXIS_COUNT; ++axis) {
        report->logical_max[axis] = 255;
    }

    /* Shared button order: Cross, Circle, Square, Triangle, L1, R1,
     * Share, Options. The first three also supply Apple PB0..PB2. */
    report->buttons = (uint8_t)(((data[5] >> 5U) & 0x03U) |
                               ((data[5] >> 2U) & 0x04U) |
                               ((data[5] >> 4U) & 0x08U) |
                               ((data[6] << 4U) & 0x30U) |
                               ((data[6] << 2U) & 0xC0U));
    *hat = (uint8_t)(data[5] & 0x0FU);
    if (*hat > 7U) {
        *hat = 8U;
    }

    if (data[6] & 0x40U) report->buttons |= UINT32_C(1) << USB_GAMEPAD_BUTTON_LEFT_STICK;
    if (data[6] & 0x80U) report->buttons |= UINT32_C(1) << USB_GAMEPAD_BUTTON_RIGHT_STICK;
    if (data[7] & 1U) report->buttons |= UINT32_C(1) << USB_GAMEPAD_BUTTON_HOME;
    if (data[6] & 4U) report->buttons |= UINT32_C(1) << USB_GAMEPAD_BUTTON_LEFT_TRIGGER;
    if (data[6] & 8U) report->buttons |= UINT32_C(1) << USB_GAMEPAD_BUTTON_RIGHT_TRIGGER;
    if (data[7] & 2U) report->buttons |= UINT32_C(1) << USB_GAMEPAD_BUTTON_TOUCHPAD;
    /* Byte 7 bits 2..7 are a report counter, never buttons. */
    *extra_active = 0U;
    return 1U;
}

#endif /* USB_PS4_REPORT_H */
