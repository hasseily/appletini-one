#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t test_read(uint32_t address);
static void test_write(uint32_t address, uint32_t value);
#define COMMON_H
#define REG_READ(address) test_read((uint32_t)(address))
#define REG_WRITE(address, value) test_write((uint32_t)(address), (uint32_t)(value))
#include "../../ps_sources/frontend/slot2_gamepad_service.c"

static uint32_t control;
static uint32_t staged[2];
static uint32_t live[2];
static unsigned writes;
static unsigned checks;

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static uint32_t test_read(uint32_t address)
{
    assert(address == CARD_CTRL_SLOT2_CONTROL_REG);
    return control;
}

static void test_write(uint32_t address, uint32_t value)
{
    /* Every update must stage LO, then HI, then publish one complete tuple. */
    const uint32_t addresses[] = {CARD_CTRL_SLOT2_STATE_LO_REG,
        CARD_CTRL_SLOT2_STATE_HI_REG, CARD_CTRL_SLOT2_CONTROL_REG};
    assert(address == addresses[writes % 3]);
    if (writes % 3 < 2) {
        staged[writes % 3] = value;
    } else {
        control = (control & 0xFFFF0000U) | (value & 0xF3U);
        memcpy(live, staged, sizeof(live));
    }
    ++writes;
}

static void reset_service(void)
{
    control = 0x53320001U;
    memset(staged, 0, sizeof(staged));
    memset(live, 0, sizeof(live));
    writes = 0;
    slot2_gamepad_service_init();
    slot2_gamepad_service_set_card(SLOT2_CARD_SNES_MAX);
}

static void report_buttons(uint8_t slot, uint8_t buttons, uint8_t hat)
{
    onee_input_joystick_report_t report = {0};
    report.buttons_valid = 1;
    report.buttons = buttons;
    slot2_gamepad_service_report(slot, &report, hat);
}

static slot2_gamepad_snapshot_t snapshot(void)
{
    slot2_gamepad_snapshot_t result;
    slot2_gamepad_service_get_snapshot(&result);
    return result;
}

static int test_buttons_and_hats(void)
{
    /* Literal SNES serial positions, independent of the service's mapping. */
    const uint16_t expected_buttons[] = {0x001, 0x100, 0x002, 0x200,
        0x400, 0x800, 0x004, 0x008};
    const uint16_t expected_hats[] = {0x010, 0x090, 0x080, 0x0A0,
        0x020, 0x060, 0x040, 0x050, 0};
    reset_service();
    for (unsigned button = 0; button < 8; ++button) {
        report_buttons(3, (uint8_t)(1U << button), 8);
        CHECK(snapshot().buttons[0] == expected_buttons[button]);
    }
    report_buttons(3, 255, 8);
    CHECK(snapshot().buttons[0] == 0xF0F);
    for (unsigned hat = 0; hat < 9; ++hat) {
        report_buttons(3, 0, (uint8_t)hat);
        CHECK(snapshot().buttons[0] == expected_hats[hat]);
    }
    report_buttons(3, 0, 0);
    report_buttons(3, 2, SLOT2_GAMEPAD_HAT_UNCHANGED);
    CHECK(snapshot().buttons[0] == 0x110);
    report_buttons(3, 0, 200);
    CHECK(snapshot().buttons[0] == 0);
    return 0;
}

static int test_axes_and_partial_reports(void)
{
    onee_input_joystick_report_t report = {0};
    reset_service();
    report.buttons_valid = 1;
    report.buttons = 1;
    report.axis_valid_mask = 3;
    report.logical_max[0] = 255;
    report.logical_max[1] = 255;
    report.axis[0] = 0;
    report.axis[1] = 255;
    slot2_gamepad_service_report(0, &report, 8);
    CHECK(snapshot().buttons[0] == 0x061);
    report.buttons_valid = 0;
    report.axis_valid_mask = 1;
    report.axis[0] = 255;
    slot2_gamepad_service_report(0, &report, SLOT2_GAMEPAD_HAT_UNCHANGED);
    CHECK(snapshot().buttons[0] == 0x0A1);
    report.axis_valid_mask = 0;
    slot2_gamepad_service_report(0, &report, 0);
    CHECK(snapshot().buttons[0] == 0x011); /* D-pad overrides both stick axes. */
    slot2_gamepad_service_report(0, &report, 8);
    CHECK(snapshot().buttons[0] == 0x0A1);
    report.axis_valid_mask = 3;
    report.axis[0] = 85;
    report.axis[1] = 170;
    slot2_gamepad_service_report(0, &report, 8);
    CHECK(snapshot().buttons[0] == 0x001);
    report.axis[0] = 84;
    report.axis[1] = 171;
    slot2_gamepad_service_report(0, &report, 8);
    CHECK(snapshot().buttons[0] == 0x061);
    report.logical_min[0] = report.logical_min[1] = INT32_MIN;
    report.logical_max[0] = report.logical_max[1] = INT32_MAX;
    report.axis[0] = INT32_MAX;
    report.axis[1] = INT32_MIN;
    slot2_gamepad_service_report(0, &report, 8);
    CHECK(snapshot().buttons[0] == 0x091);
    report.logical_max[0] = INT32_MIN;
    report.logical_min[1] = INT32_MAX;
    report.logical_max[1] = INT32_MIN;
    slot2_gamepad_service_report(0, &report, 8);
    CHECK(snapshot().buttons[0] == 0x001);
    for (int32_t minimum = -1; minimum <= 0; ++minimum) {
        report.logical_min[0] = report.logical_min[1] = minimum;
        report.logical_max[0] = report.logical_max[1] = minimum + 2;
        for (int32_t position = 0; position <= 2; ++position) {
            const uint16_t expected[] = {0x051, 0x001, 0x0A1};
            report.axis[0] = report.axis[1] = minimum + position;
            slot2_gamepad_service_report(0, &report, 8);
            CHECK(snapshot().buttons[0] == expected[position]);
        }
    }
    return 0;
}

static int test_player_ownership(void)
{
    const uint8_t order[] = {5, 3, 7, 1, 0};
    uint8_t choices[] = {0, 0, 0, 0};
    slot2_gamepad_snapshot_t state;
    reset_service();
    slot2_gamepad_service_set_card(SLOT2_CARD_FOUR_PLAY);
    for (unsigned i = 0; i < sizeof(order); ++i) {
        report_buttons(order[i], (uint8_t)(1U << i), 8);
    }
    state = snapshot();
    CHECK(state.present == 15);
    for (unsigned i = 0; i < 4; ++i) CHECK(state.slots[i] == order[i]);
    slot2_gamepad_service_disconnect(5);
    state = snapshot();
    CHECK(state.slots[0] == 0 && state.slots[1] == 3 && state.slots[2] == 7 &&
          state.slots[3] == 1); /* Only the vacant player changes. */
    slot2_gamepad_service_disconnect(3);
    state = snapshot();
    CHECK(state.present == 13 && state.slots[1] == 8 && state.buttons[1] == 0);
    report_buttons(6, 255, 8);
    CHECK(snapshot().slots[1] == 6 && snapshot().slots[2] == 7);
    choices[0] = 8; /* Explicit choice reserves that device before Auto. */
    choices[1] = 8; /* Intentional duplicate mappings are allowed. */
    choices[2] = 9;
    slot2_gamepad_service_set_player_devices(choices);
    state = snapshot();
    CHECK(state.slots[0] == 7 && state.slots[1] == 7 && state.slots[2] == 8 &&
          state.slots[3] == 1 && state.present == 11);
    slot2_gamepad_service_disconnect(7);
    CHECK(snapshot().present == 8); /* Explicit does not fall back. */
    choices[0] = 250;
    slot2_gamepad_service_set_player_devices(choices);
    CHECK(snapshot().slots[0] == 8);
    slot2_gamepad_service_release_all();
    report_buttons(7, 2, 8);
    state = snapshot();
    CHECK(state.card == SLOT2_CARD_FOUR_PLAY && state.present == 2 &&
          state.slots[1] == 7 && state.slots[0] == 8);
    slot2_gamepad_service_report(8, NULL, 0);
    slot2_gamepad_service_report(0, NULL, 0);
    slot2_gamepad_service_disconnect(255);
    slot2_gamepad_service_set_player_devices(NULL);
    slot2_gamepad_service_get_snapshot(NULL);
    CHECK(snapshot().present == 2);
    return 0;
}

static int test_publish_and_blocking(void)
{
    unsigned before;
    reset_service();
    CHECK(writes == 3 && control == 0x53320003 && live[0] == 0 && live[1] == 0);
    slot2_gamepad_service_set_card(SLOT2_CARD_FOUR_PLAY);
    report_buttons(0, 1, 0);
    report_buttons(1, 2, 2);
    report_buttons(2, 4, 4);
    report_buttons(3, 8, 6);
    CHECK(writes == 6); /* USB reports do not expose half a multi-player update. */
    slot2_gamepad_service_poll();
    CHECK(writes == 9 && control == 0x533200F2 && live[0] == 0x01800011 &&
          live[1] == 0x02400022);
    before = writes;
    slot2_gamepad_service_poll();
    CHECK(writes == before);
    slot2_gamepad_service_set_blocked(1);
    CHECK(live[0] == 0 && live[1] == 0 && snapshot().present == 15);
    report_buttons(0, 16, 8);
    slot2_gamepad_service_poll();
    CHECK(live[0] == 0 && live[1] == 0);
    slot2_gamepad_service_set_blocked(0);
    CHECK(live[0] == 0x01800400 && live[1] == 0x02400022);
    slot2_gamepad_service_set_card(SLOT2_CARD_FOUR_PLAY);
    CHECK(control == 0x533200F2);
    slot2_gamepad_service_disconnect(0);
    slot2_gamepad_service_poll();
    CHECK(live[0] == 0x01800000 && control == 0x533200E2);
    slot2_gamepad_service_set_card(255);
    CHECK((control & 3) == SLOT2_CARD_OFF);
    before = writes;
    control = 0; /* An older FPGA must never receive these writes. */
    report_buttons(0, 255, 0);
    slot2_gamepad_service_set_card(SLOT2_CARD_FOUR_PLAY);
    slot2_gamepad_service_poll();
    CHECK(writes == before && !snapshot().available);
    control = 0x53320001;
    slot2_gamepad_service_poll();
    CHECK(writes == before + 3 && control == 0x533200F2);
    CHECK(live[0] == 0x01800F1F);
    slot2_gamepad_service_set_blocked(1);
    slot2_gamepad_service_release_all();
    CHECK(snapshot().blocked && snapshot().card == SLOT2_CARD_FOUR_PLAY &&
          snapshot().present == 0 && live[0] == 0 && live[1] == 0);
    return 0;
}

static int test_snes_has_only_two_players(void)
{
    uint8_t choices[] = {0, 0, 1, 2};
    reset_service();
    /* Hidden 4Play selections cannot reserve a SNES player's USB controller. */
    slot2_gamepad_service_set_player_devices(choices);
    for (uint8_t slot = 0; slot < 4; ++slot) report_buttons(slot, 1, 8);
    CHECK(snapshot().present == 3 && snapshot().slots[0] == 0 &&
          snapshot().slots[1] == 1 && snapshot().slots[2] == 8);
    slot2_gamepad_service_disconnect(0);
    CHECK(snapshot().slots[0] == 2 && snapshot().slots[1] == 1);
    memset(choices, 0, sizeof(choices));
    slot2_gamepad_service_set_player_devices(choices);
    slot2_gamepad_service_set_card(SLOT2_CARD_FOUR_PLAY);
    CHECK(snapshot().present == 7 && snapshot().slots[0] == 2 &&
          snapshot().slots[1] == 1 && snapshot().slots[2] == 3);
    slot2_gamepad_service_set_card(SLOT2_CARD_SNES_MAX);
    slot2_gamepad_service_disconnect(2);
    CHECK(snapshot().present == 3 && snapshot().slots[0] == 3 &&
          snapshot().slots[1] == 1);
    return 0;
}

int main(void)
{
    if (test_buttons_and_hats() || test_axes_and_partial_reports() ||
        test_player_ownership() || test_publish_and_blocking() ||
        test_snes_has_only_two_players()) return 1;
    printf("SLOT2 GAMEPAD SERVICE PASS: %u checks\n", checks);
    return 0;
}
