#ifndef SLOT2_GAMEPAD_SERVICE_H
#define SLOT2_GAMEPAD_SERVICE_H

#include <stdint.h>
#include "onee_input_service.h"

typedef enum {
    SLOT2_CARD_OFF = 0,
    SLOT2_CARD_MOUSE,
    SLOT2_CARD_FOUR_PLAY,
    SLOT2_CARD_SNES_MAX,
    SLOT2_CARD_COUNT
} slot2_card_t;

#define SLOT2_GAMEPAD_PLAYER_COUNT 4U
#define SLOT2_GAMEPAD_DEVICE_AUTO 0U
#define SLOT2_GAMEPAD_DEVICE_OFF (ONEE_INPUT_DEVICE_SLOT_COUNT + 1U)
#define SLOT2_GAMEPAD_HAT_NEUTRAL 8U
#define SLOT2_GAMEPAD_HAT_UNCHANGED 0xFFU

typedef struct {
    uint8_t card;
    uint8_t available;
    uint8_t blocked;
    uint8_t present;
    uint8_t slots[SLOT2_GAMEPAD_PLAYER_COUNT];
    uint16_t buttons[SLOT2_GAMEPAD_PLAYER_COUNT];
} slot2_gamepad_snapshot_t;

/* Slot-2 cards are independent of the legacy paddle mapping and vTW.
 * Device choices: Auto, shared USB input slot + 1, or Off. Auto keeps each
 * player's assignment until that device disconnects or its choice changes. */
void slot2_gamepad_service_init(void);
void slot2_gamepad_service_set_card(uint8_t card);
void slot2_gamepad_service_set_player_devices(const uint8_t devices[4]);
uint8_t slot2_gamepad_service_available(void);
void slot2_gamepad_service_report(uint8_t slot,
    const onee_input_joystick_report_t *report, uint8_t hat);
void slot2_gamepad_service_disconnect(uint8_t slot);
void slot2_gamepad_service_release_all(void);
void slot2_gamepad_service_set_blocked(uint8_t blocked);
void slot2_gamepad_service_poll(void);
void slot2_gamepad_service_get_snapshot(slot2_gamepad_snapshot_t *snapshot);

#endif /* SLOT2_GAMEPAD_SERVICE_H */
