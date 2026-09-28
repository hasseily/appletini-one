#ifndef USB_GAMEPAD_INPUT_H
#define USB_GAMEPAD_INPUT_H

#include <stdint.h>

#include "onee_input_service.h"

/* Vendor gamepads share the eight menu/input slots with HID interfaces. */
int usb_gamepad_input_connect(void);
void usb_gamepad_input_disconnect(uint8_t slot);

/* Buttons: A, B, X, Y, LB, RB, View, Menu in bits 0..7. Hat: clockwise
 * from north (0..7), or 8 for neutral. extra_active covers Guide/stick clicks
 * for the input-release guard. The report also feeds the live preview. */
void usb_gamepad_input_report(
    uint8_t slot,
    const onee_input_joystick_report_t *report,
    uint8_t hat,
    uint8_t extra_active);

#endif /* USB_GAMEPAD_INPUT_H */
