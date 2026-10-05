#ifndef USB_GAMEPAD_INPUT_H
#define USB_GAMEPAD_INPUT_H

#include <stdint.h>

#include "onee_input_service.h"

/* Vendor gamepads share the eight menu/input slots with HID interfaces. */
int usb_gamepad_input_connect(void);
void usb_gamepad_input_disconnect(uint8_t slot);

#define USB_GAMEPAD_BUTTON_LEFT_STICK 8U
#define USB_GAMEPAD_BUTTON_RIGHT_STICK 9U
#define USB_GAMEPAD_BUTTON_HOME 10U
#define USB_GAMEPAD_BUTTON_LEFT_TRIGGER 11U
#define USB_GAMEPAD_BUTTON_RIGHT_TRIGGER 12U
#define USB_GAMEPAD_BUTTON_TOUCHPAD 13U

/* Buttons: A, B, X, Y, LB, RB, View, Menu in bits 0..7. Hat: clockwise
 * from north (0..7), or 8 for neutral. bits 8..13 are the named extra buttons above.
 * extra_active retains compatibility with transport-only release guards. The report also feeds the live preview. */
void usb_gamepad_input_report(
    uint8_t slot,
    const onee_input_joystick_report_t *report,
    uint8_t hat,
    uint8_t extra_active);

#endif /* USB_GAMEPAD_INPUT_H */
