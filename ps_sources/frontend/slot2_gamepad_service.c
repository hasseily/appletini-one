/* USB gamepads for the slot-2 4Play and SNES MAX card interfaces. */
#include "slot2_gamepad_service.h"

#include <stddef.h>
#include <string.h>

#include "card_control_regs.h"
#include "../lib/common.h"

typedef struct {
    uint8_t connected;
    uint8_t buttons;
    uint8_t hat;
    int8_t x;
    int8_t y;
} slot2_input_t;

static slot2_input_t g_inputs[ONEE_INPUT_DEVICE_SLOT_COUNT];
static uint8_t g_devices[SLOT2_GAMEPAD_PLAYER_COUNT];
static uint8_t g_auto_slots[SLOT2_GAMEPAD_PLAYER_COUNT];
static uint8_t g_card;
static uint8_t g_blocked;
static uint8_t g_published;
static uint32_t g_published_lo;
static uint32_t g_published_hi;

static int8_t slot2_axis_direction(int32_t value, int32_t minimum, int32_t maximum)
{
    const int64_t span = (int64_t)maximum - minimum;
    const int64_t position = (int64_t)value - minimum;
    if (span <= 0) {
        return 0;
    }
    /* The middle third is neutral. Wide arithmetic also handles signed
     * 32-bit HID ranges without overflow. */
    return position * 3 < span ? -1 : (position * 3 > span * 2 ? 1 : 0);
}

static void slot2_resolve_auto(void)
{
    const uint8_t players = g_card == SLOT2_CARD_SNES_MAX ? 2U : SLOT2_GAMEPAD_PLAYER_COUNT;
    uint8_t used = 0U;
    for (uint8_t player = players; player < SLOT2_GAMEPAD_PLAYER_COUNT; ++player) {
        g_auto_slots[player] = ONEE_INPUT_DEVICE_SLOT_COUNT;
    }
    for (uint8_t player = 0U; player < players; ++player) {
        if (g_devices[player] >= 1U && g_devices[player] <= ONEE_INPUT_DEVICE_SLOT_COUNT) {
            used |= (uint8_t)(1U << (g_devices[player] - 1U));
        }
    }
    /* Reserve surviving assignments before filling holes. Removing player 1
     * must not move player 2's still-connected controller into player 1. */
    for (uint8_t player = 0U; player < players; ++player) {
        const uint8_t slot = g_auto_slots[player];
        if (g_devices[player] != SLOT2_GAMEPAD_DEVICE_AUTO ||
            slot >= ONEE_INPUT_DEVICE_SLOT_COUNT || !g_inputs[slot].connected ||
            (used & (uint8_t)(1U << slot)) != 0U) {
            g_auto_slots[player] = ONEE_INPUT_DEVICE_SLOT_COUNT;
        } else {
            used |= (uint8_t)(1U << slot);
        }
    }
    for (uint8_t player = 0U; player < players; ++player) {
        if (g_devices[player] != SLOT2_GAMEPAD_DEVICE_AUTO ||
            g_auto_slots[player] < ONEE_INPUT_DEVICE_SLOT_COUNT) {
            continue;
        }
        for (uint8_t slot = 0U; slot < ONEE_INPUT_DEVICE_SLOT_COUNT; ++slot) {
            if (g_inputs[slot].connected && (used & (uint8_t)(1U << slot)) == 0U) {
                g_auto_slots[player] = slot;
                used |= (uint8_t)(1U << slot);
                break;
            }
        }
    }
}

static uint16_t slot2_snes_buttons(const slot2_input_t *input)
{
    const uint8_t buttons = input->buttons;
    const uint8_t hat = input->hat;
    /* Preserve face-button positions across Xbox/PS4 and SNES labels:
     * bottom/left/right/top become SNES B/Y/A/X. */
    uint16_t snes = (uint16_t)((buttons & 0x01U) |
        ((buttons & 0x04U) >> 1) | ((buttons & 0xC0U) >> 4) |
        ((buttons & 0x02U) << 7) | ((buttons & 0x08U) << 6) |
        ((buttons & 0x30U) << 6));
    if (hat <= 7U) {
        if (hat == 7U || hat == 0U || hat == 1U) snes |= 1U << 4;
        if (hat == 3U || hat == 4U || hat == 5U) snes |= 1U << 5;
        if (hat == 5U || hat == 6U || hat == 7U) snes |= 1U << 6;
        if (hat == 1U || hat == 2U || hat == 3U) snes |= 1U << 7;
    } else {
        if (input->y < 0) snes |= 1U << 4;
        if (input->y > 0) snes |= 1U << 5;
        if (input->x < 0) snes |= 1U << 6;
        if (input->x > 0) snes |= 1U << 7;
    }
    return snes;
}

uint8_t slot2_gamepad_service_available(void)
{
    return (REG_READ(CARD_CTRL_SLOT2_CONTROL_REG) >> CARD_CTRL_SLOT2_SIGNATURE_SHIFT)
        == CARD_CTRL_SLOT2_SIGNATURE;
}

void slot2_gamepad_service_get_snapshot(slot2_gamepad_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }
    slot2_resolve_auto();
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->card = g_card;
    snapshot->available = slot2_gamepad_service_available();
    snapshot->blocked = g_blocked;
    memset(snapshot->slots, ONEE_INPUT_DEVICE_SLOT_COUNT, sizeof(snapshot->slots));
    const uint8_t players = g_card == SLOT2_CARD_SNES_MAX ? 2U : SLOT2_GAMEPAD_PLAYER_COUNT;
    for (uint8_t player = 0U; player < players; ++player) {
        const uint8_t device = g_devices[player];
        const uint8_t slot = device == SLOT2_GAMEPAD_DEVICE_AUTO ? g_auto_slots[player] :
            (device <= ONEE_INPUT_DEVICE_SLOT_COUNT ? (uint8_t)(device - 1U) :
             ONEE_INPUT_DEVICE_SLOT_COUNT);
        snapshot->slots[player] = slot;
        if (slot < ONEE_INPUT_DEVICE_SLOT_COUNT && g_inputs[slot].connected) {
            snapshot->present |= (uint8_t)(1U << player);
            snapshot->buttons[player] = g_blocked ? 0U : slot2_snes_buttons(&g_inputs[slot]);
        }
    }
}

void slot2_gamepad_service_poll(void)
{
    slot2_gamepad_snapshot_t state;
    slot2_gamepad_service_get_snapshot(&state);
    if (!state.available) {
        g_published = 0U;
        return;
    }
    const uint32_t low = state.buttons[0] | ((uint32_t)state.buttons[1] << 16);
    const uint32_t high = state.buttons[2] | ((uint32_t)state.buttons[3] << 16);
    const uint32_t control = g_card | ((uint32_t)state.present << CARD_CTRL_SLOT2_PRESENT_SHIFT);
    if (!g_published || low != g_published_lo || high != g_published_hi ||
        (REG_READ(CARD_CTRL_SLOT2_CONTROL_REG) & CARD_CTRL_SLOT2_STATE_MASK) != control) {
        REG_WRITE(CARD_CTRL_SLOT2_STATE_LO_REG, low);
        REG_WRITE(CARD_CTRL_SLOT2_STATE_HI_REG, high);
        REG_WRITE(CARD_CTRL_SLOT2_CONTROL_REG, control);
        g_published_lo = low;
        g_published_hi = high;
        g_published = 1U;
    }
}

void slot2_gamepad_service_init(void)
{
    memset(g_inputs, 0, sizeof(g_inputs));
    memset(g_devices, 0, sizeof(g_devices));
    memset(g_auto_slots, ONEE_INPUT_DEVICE_SLOT_COUNT, sizeof(g_auto_slots));
    g_card = SLOT2_CARD_OFF;
    g_blocked = 0U;
    g_published = 0U;
}

void slot2_gamepad_service_set_card(uint8_t card)
{
    g_card = card < SLOT2_CARD_COUNT ? card : SLOT2_CARD_OFF;
    slot2_gamepad_service_poll();
}

void slot2_gamepad_service_set_player_devices(const uint8_t devices[4])
{
    if (devices == NULL) {
        return;
    }
    for (uint8_t player = 0U; player < SLOT2_GAMEPAD_PLAYER_COUNT; ++player) {
        const uint8_t device = devices[player] <= SLOT2_GAMEPAD_DEVICE_OFF ?
            devices[player] : SLOT2_GAMEPAD_DEVICE_OFF;
        if (device != g_devices[player]) {
            g_auto_slots[player] = ONEE_INPUT_DEVICE_SLOT_COUNT;
        }
        g_devices[player] = device;
    }
    slot2_gamepad_service_poll();
}

void slot2_gamepad_service_report(uint8_t slot,
    const onee_input_joystick_report_t *report, uint8_t hat)
{
    if (slot >= ONEE_INPUT_DEVICE_SLOT_COUNT || report == NULL) {
        return;
    }
    slot2_input_t *input = &g_inputs[slot];
    if (!input->connected) {
        input->hat = SLOT2_GAMEPAD_HAT_NEUTRAL;
    }
    input->connected = 1U;
    if (report->buttons_valid) {
        input->buttons = report->buttons;
    }
    if (hat != SLOT2_GAMEPAD_HAT_UNCHANGED) {
        input->hat = hat <= 7U ? hat : SLOT2_GAMEPAD_HAT_NEUTRAL;
    }
    if (report->axis_valid_mask & (1U << ONEE_INPUT_AXIS_X)) {
        input->x = slot2_axis_direction(report->axis[ONEE_INPUT_AXIS_X],
            report->logical_min[ONEE_INPUT_AXIS_X], report->logical_max[ONEE_INPUT_AXIS_X]);
    }
    if (report->axis_valid_mask & (1U << ONEE_INPUT_AXIS_Y)) {
        input->y = slot2_axis_direction(report->axis[ONEE_INPUT_AXIS_Y],
            report->logical_min[ONEE_INPUT_AXIS_Y], report->logical_max[ONEE_INPUT_AXIS_Y]);
    }
    /* Assign on arrival, so two reports in one USB poll keep arrival order. */
    slot2_resolve_auto();
}

void slot2_gamepad_service_disconnect(uint8_t slot)
{
    if (slot < ONEE_INPUT_DEVICE_SLOT_COUNT) {
        memset(&g_inputs[slot], 0, sizeof(g_inputs[slot]));
        for (uint8_t player = 0U; player < SLOT2_GAMEPAD_PLAYER_COUNT; ++player) {
            if (g_auto_slots[player] == slot) {
                g_auto_slots[player] = ONEE_INPUT_DEVICE_SLOT_COUNT;
            }
        }
    }
}

void slot2_gamepad_service_release_all(void)
{
    memset(g_inputs, 0, sizeof(g_inputs));
    memset(g_auto_slots, ONEE_INPUT_DEVICE_SLOT_COUNT, sizeof(g_auto_slots));
    slot2_gamepad_service_poll();
}

void slot2_gamepad_service_set_blocked(uint8_t blocked)
{
    g_blocked = blocked != 0U;
    slot2_gamepad_service_poll();
}
