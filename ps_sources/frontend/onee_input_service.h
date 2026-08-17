#ifndef ONEE_INPUT_SERVICE_H
#define ONEE_INPUT_SERVICE_H

#include <stdint.h>

/* CherryUSB assigns HID interfaces stable minor numbers in the range 0..7.
 * Keeping the same count here makes keyboard aggregation and joystick owner
 * selection independent of USB connection order. */
#define ONEE_INPUT_DEVICE_SLOT_COUNT 8U

typedef enum {
    ONEE_INPUT_AXIS_X = 0,
    ONEE_INPUT_AXIS_Y,
    ONEE_INPUT_AXIS_Z,
    ONEE_INPUT_AXIS_RX,
    ONEE_INPUT_AXIS_RY,
    ONEE_INPUT_AXIS_RZ,
    ONEE_INPUT_AXIS_COUNT
} onee_input_axis_t;

typedef struct {
    uint8_t axis_valid_mask;
    uint8_t buttons_valid;
    uint8_t buttons;
    int32_t axis[ONEE_INPUT_AXIS_COUNT];
    int32_t logical_min[ONEE_INPUT_AXIS_COUNT];
    int32_t logical_max[ONEE_INPUT_AXIS_COUNT];
} onee_input_joystick_report_t;

void onee_input_service_init(void);
void onee_input_service_poll(void);

/* HID reports update saved physical state even while ONE//e is off, but the
 * service discards key edges and performs no input-bridge writes then.
 * The keyboard call returns one only when an active Ctrl+Alt+Delete reset
 * chord consumed forward Delete, so the normal USB binding path can omit
 * that one key. */
uint8_t onee_input_service_keyboard_report(uint8_t slot,
                                           uint8_t modifier,
                                           const uint8_t *keys,
                                           uint32_t key_count);
void onee_input_service_joystick_report(
    uint8_t slot,
    const onee_input_joystick_report_t *report);
void onee_input_service_disconnect(uint8_t slot);
void onee_input_service_release_all(void);

#endif /* ONEE_INPUT_SERVICE_H */
