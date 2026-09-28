#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t test_read(uint32_t address);
static void test_write(uint32_t address, uint32_t value);
#define COMMON_H
#define REG_READ(address) test_read((uint32_t)(address))
#define REG_WRITE(address, value) test_write((uint32_t)(address), (uint32_t)(value))
#include "slot2_gamepad_service.c"

#define UART0_BASE 0U
#define OTHER_SLOTS 0x92U
static uint32_t g_card_slot_enable_mask, g_card_slot_effective_mask;
static uint32_t control, staged[2], committed[2];
static char events[128];
static unsigned event_count, write_count, warnings, mouse_initialized;

static void event(char value)
{
    assert(event_count + 1U < sizeof(events));
    events[event_count++] = value;
    events[event_count] = '\0';
}

static uint32_t test_read(uint32_t address)
{
    assert(address == CARD_CTRL_SLOT2_CONTROL_REG);
    return control;
}

static void test_write(uint32_t address, uint32_t value)
{
    const uint32_t addresses[] = {CARD_CTRL_SLOT2_STATE_LO_REG,
        CARD_CTRL_SLOT2_STATE_HI_REG, CARD_CTRL_SLOT2_CONTROL_REG};
    assert(address == addresses[write_count % 3U]);
    assert((control >> CARD_CTRL_SLOT2_SIGNATURE_SHIFT) == CARD_CTRL_SLOT2_SIGNATURE);
    if (write_count % 3U < 2U) {
        staged[write_count % 3U] = value;
        event(write_count % 3U == 0U ? 'L' : 'H');
    } else {
        if ((control & 3U) != (value & 3U)) {
            assert((g_card_slot_enable_mask & (1U << 2)) == 0U);
        }
        control = (control & 0xFFFF0000U) | (value & CARD_CTRL_SLOT2_STATE_MASK);
        memcpy(committed, staged, sizeof(committed));
        event('C');
    }
    ++write_count;
}

static void card_control_write_slot_mask(uint32_t mask)
{
    assert((mask & ~(1U << 2)) == OTHER_SLOTS);
    if ((g_card_slot_enable_mask & (1U << 2)) != 0U && (mask & (1U << 2)) == 0U) {
        mouse_initialized = 0U;
    }
    g_card_slot_enable_mask = g_card_slot_effective_mask = mask;
    event(mask & (1U << 2) ? 'E' : 'D');
}

static void control_apply_slot5_service(uint8_t enabled)
{ (void)enabled; assert(!"slot-2 changes must not touch slot-5 services"); }
static void vtw_service_set_disk2_config_enabled(uint8_t enabled)
{ (void)enabled; assert(!"slot-2 changes must not touch disk services"); }
static void uart_puts(uint32_t base, const char *message)
{
    assert(base == UART0_BASE);
    assert(strstr(message, "requires updated FPGA firmware") != NULL);
    ++warnings;
}

/* PRODUCTION_CALLBACKS */

static void clear_events(void)
{
    memset(events, 0, sizeof(events));
    event_count = 0U;
}

static void reset(uint8_t available)
{
    control = available ? 0x53320001U : 0U;
    g_card_slot_enable_mask = g_card_slot_effective_mask = OTHER_SLOTS;
    write_count = 0U;
    warnings = 0U;
    mouse_initialized = 0U;
    memset(staged, 0, sizeof(staged));
    memset(committed, 0, sizeof(committed));
    clear_events();
    slot2_gamepad_service_init();
}

int main(void)
{
    slot2_gamepad_snapshot_t snapshot;
    reset(1U);
    control_set_slot2_card(NULL, SLOT2_CARD_MOUSE);
    assert(strcmp(events, "DLHCE") == 0);
    mouse_initialized = 1U;
    for (unsigned i = 0U; i < 8U; ++i) {
        clear_events();
        control_set_slot2_card(NULL, SLOT2_CARD_MOUSE);
        assert(strcmp(events, "E") == 0 && mouse_initialized);
    }

    /* An ordinary USB report can change stored state while Mouse remains
     * selected; reapplying the menu still must not reset that mouse. */
    onee_input_joystick_report_t report = {0};
    report.buttons_valid = 1U;
    report.buttons = 1U;
    slot2_gamepad_service_report(0U, &report, SLOT2_GAMEPAD_HAT_NEUTRAL);
    clear_events();
    control_set_slot2_card(NULL, SLOT2_CARD_MOUSE);
    assert(strcmp(events, "LHCE") == 0 && mouse_initialized);

    for (uint8_t old = 0U; old < SLOT2_CARD_COUNT; ++old) {
        for (uint8_t next = 0U; next < SLOT2_CARD_COUNT; ++next) {
            control_set_slot2_card(NULL, old);
            clear_events();
            control_set_slot2_card(NULL, next);
            if (old == next) {
                assert(strcmp(events, next == SLOT2_CARD_OFF ? "D" : "E") == 0);
            } else {
                assert(strcmp(events, next == SLOT2_CARD_OFF ? "DLHCD" : "DLHCE") == 0);
            }
            slot2_gamepad_service_get_snapshot(&snapshot);
            assert(snapshot.card == next);
            assert((control & 3U) == next);
            assert(((g_card_slot_enable_mask >> 2) & 1U) == (next != SLOT2_CARD_OFF));
        }
    }
    assert(warnings == 0U);
    clear_events();
    control_set_slot2_card(NULL, 255U);
    assert(strcmp(events, "DLHCD") == 0 && (control & 3U) == SLOT2_CARD_OFF);
    assert((g_card_slot_enable_mask & (1U << 2)) == 0U);

    /* Device choices use the real service and remain independent of the
     * legacy paddle map. Explicit duplicate assignments are allowed. */
    const uint8_t choices[4] = {1U, 1U, SLOT2_GAMEPAD_DEVICE_OFF, 0U};
    control_set_slot2_player_devices(NULL, choices);
    slot2_gamepad_service_get_snapshot(&snapshot);
    assert(snapshot.slots[0] == 0U && snapshot.slots[1] == 0U &&
           snapshot.slots[2] == ONEE_INPUT_DEVICE_SLOT_COUNT);

    reset(0U);
    control_set_slot2_card(NULL, SLOT2_CARD_MOUSE);
    assert(strcmp(events, "DE") == 0 && write_count == 0U && warnings == 0U);
    mouse_initialized = 1U;
    clear_events();
    control_set_slot2_card(NULL, SLOT2_CARD_MOUSE);
    assert(strcmp(events, "E") == 0 && mouse_initialized);
    for (uint8_t card = SLOT2_CARD_FOUR_PLAY; card <= SLOT2_CARD_SNES_MAX; ++card) {
        clear_events();
        control_set_slot2_card(NULL, card);
        assert(strcmp(events, "DD") == 0 && write_count == 0U);
        assert((g_card_slot_enable_mask & (1U << 2)) == 0U);
    }
    assert(warnings == 2U);
    clear_events();
    control_set_slot2_card(NULL, 255U);
    assert(strcmp(events, "DD") == 0 && write_count == 0U && warnings == 2U);
    slot2_gamepad_service_get_snapshot(&snapshot);
    assert(snapshot.card == SLOT2_CARD_OFF && !snapshot.available);
    puts("PASS slot2 runtime control: Mouse reapply preserves state, all card transitions disable/commit/enable, old FPGA Mouse compatibility and gamepad rejection, invalid Off");
    return 0;
}
