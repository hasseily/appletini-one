#ifndef ONEE_INPUT_SERVICE_H
#define ONEE_INPUT_SERVICE_H

#include <stdint.h>

/* HID interfaces and vendor gamepads share these slots. HID keeps its
 * CherryUSB minor number when that slot is free; both transports otherwise
 * use the first free slot for keyboard aggregation and joystick ownership. */
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

#define ONEE_INPUT_PADDLE_COUNT 4U
#define ONEE_INPUT_JOYSTICK_SENSITIVITY_MIN 25U
#define ONEE_INPUT_JOYSTICK_SENSITIVITY_MAX 200U
#define ONEE_INPUT_JOYSTICK_DEADZONE_MAX 50U

typedef enum {
    ONEE_INPUT_JOYSTICK_SOURCE_AUTO = 0,
    ONEE_INPUT_JOYSTICK_SOURCE_X,
    ONEE_INPUT_JOYSTICK_SOURCE_Y,
    ONEE_INPUT_JOYSTICK_SOURCE_Z,
    ONEE_INPUT_JOYSTICK_SOURCE_RX,
    ONEE_INPUT_JOYSTICK_SOURCE_RY,
    ONEE_INPUT_JOYSTICK_SOURCE_RZ,
    ONEE_INPUT_JOYSTICK_SOURCE_OFF
} onee_input_joystick_source_t;

typedef struct {
    uint8_t source;
    uint8_t device; /* 0: first joystick; 1..8: shared USB input slot + 1. */
    uint8_t invert;
    uint8_t deadzone_percent;
    uint16_t sensitivity_percent;
} onee_input_joystick_paddle_config_t;

typedef struct {
    onee_input_joystick_paddle_config_t paddle[ONEE_INPUT_PADDLE_COUNT];
} onee_input_joystick_config_t;

typedef struct {
    uint8_t connected;
    uint8_t axis_valid_mask;
    uint8_t axis[ONEE_INPUT_AXIS_COUNT];
    uint8_t buttons;
} onee_input_joystick_device_snapshot_t;

typedef struct {
    uint8_t connected;
    uint8_t owner_slot; /* ONEE_INPUT_DEVICE_SLOT_COUNT when disconnected. */
    uint8_t active;     /* Physical-host vTW hardware gate; separate from USB. */
    uint8_t axis_valid_mask;
    uint8_t axis[ONEE_INPUT_AXIS_COUNT];
    uint8_t paddles[ONEE_INPUT_PADDLE_COUNT];
    uint8_t buttons;
    uint8_t connected_mask;
    /* Resolved sources; SLOT_COUNT means disabled or disconnected. */
    uint8_t paddle_slots[ONEE_INPUT_PADDLE_COUNT];
    onee_input_joystick_device_snapshot_t devices[ONEE_INPUT_DEVICE_SLOT_COUNT];
} onee_input_joystick_snapshot_t;

/* The same mapping serves ONE//e and physical-host vTW. Auto defaults to
 * the first joystick and X/Y/Rx/Ry, with Z/Rz fallback for the last two
 * paddles. Each paddle may instead select its own USB slot and axis.
 * A missing explicit device/axis stays centered. Deadzone is a
 * percentage of each half-range around center; sensitivity scales the
 * remaining travel. Snapshot stays live while menu input delivery is blocked. */
void onee_input_service_default_joystick_config(
    onee_input_joystick_config_t *config);
void onee_input_service_set_joystick_config(
    const onee_input_joystick_config_t *config);
void onee_input_service_get_joystick_config(
    onee_input_joystick_config_t *config);
void onee_input_service_get_joystick_snapshot(
    onee_input_joystick_snapshot_t *snapshot);

void onee_input_service_init(void);
void onee_input_service_poll(void);
/* Consume one Ctrl+Alt+Delete edge. The main loop uses this to order the
 * private cold reboot with the SmartPort reset hook before RES# releases. */
uint8_t onee_input_service_take_cold_reboot_request(void);
/* Drop all queued Apple key data after private RES# is asserted. This keeps
 * a key accepted just before Ctrl+Alt+Delete from reaching the new boot. */
void onee_input_service_prepare_cold_reboot(void);

/* HID reports update saved physical state for both input consumers. Keyboard
 * edges and Apple keys reach only an active ONE//e input bridge. The lowest
 * joystick slot supplies raw buttons; each paddle can use a separate slot.
 * Physical-host vTW receives the same mapping when its separate hardware
 * bridge is present and enabled. Each consumer
 * keeps its own session and pending-update state.
 * The keyboard call returns one only when an active Ctrl+Alt+Delete reboot
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
/* Routing/menu changes discard keyboard edges, not saved joystick reports.
 * Blocking releases guest buttons/paddles while keeping preview state live. */
void onee_input_service_release_keyboard(void);
void onee_input_service_set_blocked(uint8_t blocked);
/* True USB teardown clears both keyboard and joystick presence. */
void onee_input_service_release_all(void);

#endif /* ONEE_INPUT_SERVICE_H */
