#!/usr/bin/env python3
"""Focused source checks for the ONE//e USB input firmware path."""

import shutil
import subprocess
import textwrap
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FRONTEND = ROOT / "ps_sources" / "frontend"
SERVICE_C = FRONTEND / "onee_input_service.c"
SERVICE_H = FRONTEND / "onee_input_service.h"
USB_C = FRONTEND / "usb_hid_service.c"
VITIS_SCRIPT = ROOT / "scripts" / "create_vitis_workspace.py"
BUILD = ROOT / "build" / "onee_input_service_test"


class TestFailure(AssertionError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise TestFailure(message)


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def between(source: str, start: str, end: str) -> str:
    first = source.find(start)
    last = source.find(end, first + len(start))
    require(first >= 0 and last > first, f"could not isolate {start}")
    return source[first:last]


def test_bridge_registers_and_hard_runtime_gate() -> None:
    source = read(SERVICE_C)
    active = between(source,
                     "static uint8_t onee_input_bridge_active",
                     "static void onee_key_queue_clear")
    poll = between(source,
                   "void onee_input_service_poll",
                   "uint8_t onee_input_service_keyboard_report")

    for offset in ("0x5CU", "0x5DU", "0x5EU", "0x5FU"):
        require(offset in source, f"missing input bridge offset {offset}")
    require("onee_service_state() != ONEE_SERVICE_STATE_RUNNING" in active and
            "CARD_CTRL_ONEE_STATUS_EFFECTIVE_BIT" in active and
            "ONEE_INPUT_STATUS_ENABLED_BIT" in active and
            "ONEE_INPUT_STATUS_SIGNATURE" in active,
            "writes must require RUNNING, EFFECTIVE, and the live bridge signature")
    require(poll.find("onee_input_bridge_active() == 0U") <
            poll.find("REG_WRITE("),
            "the poll gate must precede every bridge write")


def test_queue_is_edge_only_and_backpressured() -> None:
    source = read(SERVICE_C)
    keyboard = between(source,
                       "uint8_t onee_input_service_keyboard_report",
                       "void onee_input_service_joystick_report")
    poll = between(source,
                   "void onee_input_service_poll",
                   "uint8_t onee_input_service_keyboard_report")

    require("ONEE_INPUT_QUEUE_DEPTH 32U" in source and
            "g_key_queue_count >= ONEE_INPUT_QUEUE_DEPTH" in source and
            "g_key_queue_drop_count" in source,
            "a bounded software queue must absorb hardware FIFO backpressure")
    require("onee_key_in_report(usage," in keyboard and
            "slot->keys," in keyboard and
            "slot->key_count)" in keyboard and
            "onee_key_queue_push(code);" in keyboard,
            "only a new HID usage may enqueue an Apple key")
    require("ONEE_INPUT_FIFO_STATUS_FULL_BIT       (1UL << 12)" in source and
            "ONEE_INPUT_FIFO_STATUS_FULL_BIT" in poll and
            "REG_WRITE(ONEE_INPUT_KEY_FIFO_REG" in poll and
            "--g_key_queue_count;" in poll,
            "the software queue must retain keys while the PL FIFO is full")


def test_ascii_control_and_navigation_mapping() -> None:
    source = read(SERVICE_C)
    translate = between(source,
                        "static uint8_t onee_ascii_from_usage",
                        "static uint8_t onee_normalize_axis")

    require("shift ^ caps_lock" in translate and
            "return (uint8_t)(letter + 1U);" in translate,
            "letters must honor Shift/Caps Lock and Ctrl-A through Ctrl-Z")
    expected = (
        "HID_KBD_USAGE_ENTER", "HID_KBD_USAGE_ESCAPE",
        "HID_KBD_USAGE_DELETE", "HID_KBD_USAGE_TAB",
        "HID_KBD_USAGE_SPACE", "HID_KBD_USAGE_DELFWD",
        "HID_KBD_USAGE_RIGHT", "HID_KBD_USAGE_LEFT",
        "HID_KBD_USAGE_DOWN", "HID_KBD_USAGE_UP",
        "HID_KBD_USAGE_KPD1", "HID_KBD_USAGE_KPD0",
    )
    require(all(token in translate for token in expected) and
            "unshifted_number" in translate and
            "shifted_punctuation" in translate,
            "ASCII, control, arrow, punctuation, and keypad maps are incomplete")


def test_apple_keys_caps_and_cold_reboot_chord() -> None:
    service = read(SERVICE_C)
    usb = read(USB_C)
    keyboard = between(service,
                       "uint8_t onee_input_service_keyboard_report",
                       "void onee_input_service_joystick_report")

    require("slot->modifier & HID_MOD_LALT" in service and
            "ONEE_INPUT_LIVE_OPEN_APPLE_BIT" in service and
            "slot->modifier & HID_MOD_RALT" in service and
            "ONEE_INPUT_LIVE_CLOSED_APPLE_BIT" in service and
            "HID_MOD_LALT | HID_MOD_LGUI" not in service and
            "HID_MOD_RALT | HID_MOD_RGUI" not in service,
            "only Left and Right Alt may map to Open and Closed Apple")
    require("usage == HID_KBD_USAGE_CAPSLOCK" in keyboard and
            "g_caps_lock ^= 1U;" in keyboard,
            "Caps Lock must toggle on a new HID usage edge")
    require("HID_KBD_USAGE_DELFWD" in keyboard and
            "(modifier & HID_MOD_CTRL)" in keyboard and
            "(modifier & HID_MOD_ALT)" in keyboard and
            "g_cold_reboot_pending = 1U;" in keyboard and
            "reset_delete_consumed" in keyboard and
            "reset_modifier_consumed" in keyboard and
            "onee_input_service_take_cold_reboot_request" in service and
            "void onee_input_service_prepare_cold_reboot" in service and
            "ONEE_INPUT_CONTROL_FIFO_FLUSH_BIT |" in service and
            "ONEE_INPUT_CONTROL_RELEASE_LIVE_BIT" in service and
            "ONEE_INPUT_CONTROL_RESET_BIT" not in service,
            "Ctrl+Alt+forward Delete must queue an ordered cold reboot and flush stale input")
    require("onee_reset_chord" in usb and
            "HID_KBD_USAGE_DELFWD" in usb,
            "the consumed reset Delete key must not reach normal USB bindings")


def test_held_key_repeat_contract() -> None:
    source = read(SERVICE_C)
    repeat_select = between(source,
                            "static void onee_repeat_reselect",
                            "static void onee_repeat_poll")
    repeat_poll = between(source,
                          "static void onee_repeat_poll",
                          "static uint8_t onee_normalize_axis")
    keyboard = between(source,
                       "uint8_t onee_input_service_keyboard_report",
                       "void onee_input_service_joystick_report")

    require("ONEE_INPUT_REPEAT_DELAY_MS 500U" in source and
            "ONEE_INPUT_REPEAT_RATE_MS  100U" in source and
            "cherryusb_baremetal_ms()" in source and
            "onee_time_reached" in source and
            "g_repeat_deadline_ms = now + ONEE_INPUT_REPEAT_RATE_MS" in source,
            "repeat delay and rate must use wrap-safe wall time without catch-up")
    require("key_press_order" in source and
            "order > best_order" in repeat_select and
            "g_repeat_slot = best_slot" in repeat_select and
            "g_repeat_usage = best_usage" in repeat_select,
            "the newest held repeatable key across all HID slots must win")
    require("usage == HID_KBD_USAGE_CAPSLOCK" in source and
            "usage == HID_KBD_USAGE_PAUSE" in source and
            "onee_ascii_from_usage(usage, modifier, g_caps_lock) != 0U" in source,
            "Caps Lock, Pause, and unmapped usages must never become repeat keys")
    require("g_slots[g_repeat_slot].modifier" in repeat_poll and
            "current modifier byte and the current Caps Lock" in repeat_poll and
            "onee_key_queue_push(code);" in repeat_poll,
            "each repeat must use current modifiers and Caps Lock")
    require("next_press_order" in keyboard and
            "onee_repeat_reselect();" in keyboard and
            "onee_repeat_clear_press_orders();" in source and
            "onee_repeat_reselect();" in between(
                source,
                "void onee_input_service_disconnect",
                "void onee_input_service_release_all"),
            "press, release, disconnect, and session transitions must reselect or cancel")


def test_four_paddles_are_normalized_with_stable_fallbacks() -> None:
    source = read(SERVICE_C)
    normalize = between(source,
                        "static uint8_t onee_normalize_axis",
                        "static uint32_t onee_live_word")
    paddles = between(source,
                      "static uint32_t onee_paddles_word",
                      "static void onee_input_session_stop")

    require("value < logical_min" in normalize and
            "value > logical_max" in normalize and
            "position * 255" in normalize and
            "return 0x80U" in normalize,
            "axes must clamp and normalize their HID logical range to 0..255")
    require("ONEE_INPUT_AXIS_X" in paddles and
            "ONEE_INPUT_AXIS_Y" in paddles and
            "ONEE_INPUT_AXIS_RX" in paddles and
            "ONEE_INPUT_AXIS_Z" in paddles and
            "ONEE_INPUT_AXIS_RY" in paddles and
            "ONEE_INPUT_AXIS_RZ" in paddles,
            "PDL0-3 must use X/Y/Rx/Ry with Z/Rz fallbacks")


def test_lowest_slot_owns_joystick_and_disconnect_recenters() -> None:
    source = read(SERVICE_C)
    live = between(source,
                   "static uint32_t onee_live_word",
                   "static uint8_t onee_axis_or_neutral")
    paddles = between(source,
                      "static uint32_t onee_paddles_word",
                      "static void onee_input_session_stop")
    disconnect = between(source,
                         "void onee_input_service_disconnect",
                         "void onee_input_service_release_all")

    require("joystick_owner == ONEE_INPUT_DEVICE_SLOT_COUNT" in live and
            "joystick_owner = i;" in live and
            "break;" in paddles,
            "the lowest active HID slot must own buttons and all paddles")
    require("memset(&g_slots[slot_index], 0" in disconnect and
            "g_live_dirty = 1U" in disconnect and
            "g_paddles_dirty = 1U" in disconnect and
            "ONEE_INPUT_NEUTRAL_PADDLES" in source,
            "disconnect must release buttons and select the next owner or neutral")


def test_hid_parser_feeds_boot_keyboard_and_absolute_joystick() -> None:
    source = read(USB_C)
    keyboard = between(source,
                       "static void hid_process_keyboard_usages",
                       "static void hid_process_boot_keyboard_report")
    report = between(source,
                     "static void hid_process_report_protocol_report",
                     "static void hid_process_report(")

    require("onee_input_service_keyboard_report(slot->index" in keyboard,
            "boot and report-protocol keyboards must share the ONE//e hook")
    require("hid_report_info_has_absolute_joystick" in source and
            "HID_MAINITEM_RELATIVE" in source and
            "slot->interface_protocol == HID_PROTOCOL_MOUSE" in source,
            "joystick detection must require non-mouse absolute HID axes")
    require("onee_input_service_joystick_report(slot->index" in report and
            "onee_joystick.buttons_valid = button_seen" in report,
            "parsed absolute axes and PB0-PB2 must reach the ONE//e service")


def test_usb_lifecycle_drives_input_lifecycle() -> None:
    source = read(USB_C)
    init = between(source,
                   "int usb_hid_service_init",
                   "int usb_hid_service_start")
    stop = between(source,
                   "void usb_hid_service_stop",
                   "void usb_hid_service_set_sensitivity")
    poll = source[source.find("void usb_hid_service_poll"):]

    require("onee_input_service_init();" in init and
            "onee_input_service_release_all();" in stop and
            "onee_input_service_disconnect(slot->index);" in source,
            "USB init, stop, and disconnect must release saved ONE//e input")
    first_input = poll.find("onee_input_service_poll();")
    usb_host = poll.find("cherryusb_host_poll")
    second_input = poll.find("onee_input_service_poll();", first_input + 1)
    require(0 <= first_input < usb_host < second_input,
            "USB poll must arm before reports and flush newly parsed input after")


def test_frontend_build_includes_input_service() -> None:
    build = read(VITIS_SCRIPT)

    require('"../../../ps_sources/frontend/onee_input_service.c"' in build,
            "the Vitis frontend component must compile the ONE//e input service")


TESTS = (
    test_bridge_registers_and_hard_runtime_gate,
    test_queue_is_edge_only_and_backpressured,
    test_ascii_control_and_navigation_mapping,
    test_apple_keys_caps_and_cold_reboot_chord,
    test_held_key_repeat_contract,
    test_four_paddles_are_normalized_with_stable_fallbacks,
    test_lowest_slot_owns_joystick_and_disconnect_recenters,
    test_hid_parser_feeds_boot_keyboard_and_absolute_joystick,
    test_usb_lifecycle_drives_input_lifecycle,
    test_frontend_build_includes_input_service,
)


def find_native_c_compiler() -> Path | None:
    for name in ("gcc", "cc", "clang"):
        found = shutil.which(name)
        if found:
            return Path(found)
    xilinx = Path("C:/Xilinx")
    if xilinx.exists():
        matches = sorted(
            xilinx.glob(
                "*/tps/mingw/*/win64.o/nt/bin/x86_64-w64-mingw32-gcc.exe"
            ),
            reverse=True,
        )
        if matches:
            return matches[0]
    return None


def run_native_behavior_test() -> bool:
    compiler = find_native_c_compiler()
    if compiler is None:
        print("SKIP native_behavior_test: no host C compiler")
        return True

    if BUILD.exists():
        shutil.rmtree(BUILD)
    BUILD.mkdir(parents=True)
    harness = BUILD / "onee_input_service_harness.c"
    executable = BUILD / "onee_input_service_harness.exe"
    harness.write_text(textwrap.dedent(r'''
        #include <stdint.h>
        #include <stdio.h>
        #include <string.h>

        static uint32_t test_reg_read(uint32_t address);
        static void test_reg_write(uint32_t address, uint32_t value);
        static uint32_t test_time_ms;

        uint32_t cherryusb_baremetal_ms(void)
        {
            return test_time_ms;
        }

        #define COMMON_H
        #define REG_READ(address) test_reg_read((uint32_t)(address))
        #define REG_WRITE(address, value) \
            test_reg_write((uint32_t)(address), (uint32_t)(value))
        #include "../../ps_sources/frontend/onee_input_service.c"

        typedef struct {
            uint32_t address;
            uint32_t value;
        } write_event_t;

        static uint32_t registers[256];
        static write_event_t writes[256];
        static uint32_t write_count;
        static onee_service_state_t service_state;

        static uint32_t reg_index(uint32_t address)
        {
            return (address - APPLE_DEBUG_BASE) / 4U;
        }

        static uint32_t test_reg_read(uint32_t address)
        {
            return registers[reg_index(address)];
        }

        static void test_reg_write(uint32_t address, uint32_t value)
        {
            writes[write_count].address = address;
            writes[write_count].value = value;
            ++write_count;
        }

        onee_service_state_t onee_service_state(void)
        {
            return service_state;
        }

        static void fail(const char *message)
        {
            fprintf(stderr, "FAIL: %s\n", message);
        }

        static uint32_t writes_to(uint32_t address)
        {
            uint32_t count = 0U;
            for (uint32_t i = 0U; i < write_count; ++i) {
                if (writes[i].address == address) {
                    ++count;
                }
            }
            return count;
        }

        static uint32_t last_write(uint32_t address)
        {
            for (uint32_t i = write_count; i != 0U; --i) {
                if (writes[i - 1U].address == address) {
                    return writes[i - 1U].value;
                }
            }
            return 0xFFFFFFFFU;
        }

        static void release_keys(uint8_t slot)
        {
            (void)onee_input_service_keyboard_report(slot, 0U, NULL, 0U);
        }

        int main(void)
        {
            uint8_t boot_keys[6] = { HID_KBD_USAGE_A, 0U, 0U, 0U, 0U, 0U };
            uint8_t pause_key[1] = { HID_KBD_USAGE_PAUSE };
            uint8_t delete_key[1] = { HID_KBD_USAGE_DELFWD };
            uint8_t repeat_keys[2] = {
                HID_KBD_USAGE_A, HID_KBD_USAGE_A + 1U
            };
            uint8_t other_key[1] = { HID_KBD_USAGE_A + 2U };
            uint32_t before;
            uint32_t before_control;
            onee_input_joystick_report_t joystick;

            memset(registers, 0, sizeof(registers));
            memset(writes, 0, sizeof(writes));
            service_state = ONEE_SERVICE_STATE_OFF;
            onee_input_service_init();

            (void)onee_input_service_keyboard_report(2U, 0U,
                                                       boot_keys, 6U);
            onee_input_service_poll();
            if (write_count != 0U) {
                fail("input bridge write while ONE//e was off");
                return 1;
            }

            service_state = ONEE_SERVICE_STATE_RUNNING;
            registers[0x5BU] = CARD_CTRL_ONEE_STATUS_EFFECTIVE_BIT;
            registers[0x5FU] = (0xE1UL << 24) | (1UL << 9);
            onee_input_service_poll();
            if (writes_to(ONEE_INPUT_CONTROL_REG) != 1U ||
                last_write(ONEE_INPUT_PADDLES_REG) != 0x80808080UL ||
                writes_to(ONEE_INPUT_KEY_FIFO_REG) != 0U) {
                fail("session activation did not flush, release, and center");
                return 1;
            }

            release_keys(2U);
            (void)onee_input_service_keyboard_report(
                2U, HID_MOD_LSHIFT, boot_keys, 6U);
            onee_input_service_poll();
            if (last_write(ONEE_INPUT_KEY_FIFO_REG) != 'A') {
                fail("Shift+A did not emit upper-case Apple ASCII");
                return 1;
            }
            before = writes_to(ONEE_INPUT_KEY_FIFO_REG);
            (void)onee_input_service_keyboard_report(
                2U, HID_MOD_LSHIFT, boot_keys, 6U);
            onee_input_service_poll();
            if (writes_to(ONEE_INPUT_KEY_FIFO_REG) != before) {
                fail("held HID key emitted more than one Apple event");
                return 1;
            }

            /* A short tap emits one edge even if the service polls many times
             * before the USB release report arrives. */
            release_keys(2U);
            boot_keys[0] = HID_KBD_USAGE_DOWN;
            (void)onee_input_service_keyboard_report(
                2U, 0U, boot_keys, 6U);
            onee_input_service_poll();
            before = writes_to(ONEE_INPUT_KEY_FIFO_REG);
            for (uint32_t i = 0U; i < 10000U; ++i) {
                onee_input_service_poll();
            }
            test_time_ms += 20U;
            release_keys(2U);
            onee_input_service_poll();
            test_time_ms += ONEE_INPUT_REPEAT_DELAY_MS +
                            ONEE_INPUT_REPEAT_RATE_MS;
            onee_input_service_poll();
            if (writes_to(ONEE_INPUT_KEY_FIFO_REG) != before ||
                last_write(ONEE_INPUT_KEY_FIFO_REG) != 0x0AU) {
                fail("brief Down tap generated extra Apple key events");
                return 1;
            }

            /* Start a fresh repeat interval. The initial edge remains single,
             * then the held key fires at the exact wall-time delay. */
            release_keys(2U);
            boot_keys[0] = HID_KBD_USAGE_A;
            (void)onee_input_service_keyboard_report(
                2U, HID_MOD_LSHIFT, boot_keys, 6U);
            onee_input_service_poll();
            before = writes_to(ONEE_INPUT_KEY_FIFO_REG);
            test_time_ms += ONEE_INPUT_REPEAT_DELAY_MS - 1U;
            if (writes_to(ONEE_INPUT_KEY_FIFO_REG) != before) {
                fail("held key repeated before the initial wall-time delay");
                return 1;
            }
            onee_input_service_poll();
            test_time_ms += 1U;
            onee_input_service_poll();
            if (writes_to(ONEE_INPUT_KEY_FIFO_REG) != before + 1U ||
                last_write(ONEE_INPUT_KEY_FIFO_REG) != 'A') {
                fail("held key did not repeat at the initial wall-time delay");
                return 1;
            }

            /* Repeats intentionally use current modifiers, not the mapping
             * captured on the original edge. */
            (void)onee_input_service_keyboard_report(
                2U, 0U, boot_keys, 6U);
            before = writes_to(ONEE_INPUT_KEY_FIFO_REG);
            test_time_ms += ONEE_INPUT_REPEAT_RATE_MS - 1U;
            onee_input_service_poll();
            if (writes_to(ONEE_INPUT_KEY_FIFO_REG) != before) {
                fail("held key repeated before the steady wall-time rate");
                return 1;
            }
            test_time_ms += 1U;
            onee_input_service_poll();
            if (last_write(ONEE_INPUT_KEY_FIFO_REG) != 'a') {
                fail("repeat did not apply the current Shift state");
                return 1;
            }

            /* A long frontend stall emits one late repeat, not one event for
             * every missed 100 ms interval. Repeated polls at the same wall
             * time must remain quiet. */
            before = writes_to(ONEE_INPUT_KEY_FIFO_REG);
            test_time_ms += 1000U;
            onee_input_service_poll();
            for (uint32_t i = 0U; i < 10000U; ++i) {
                onee_input_service_poll();
            }
            if (writes_to(ONEE_INPUT_KEY_FIFO_REG) != before + 1U) {
                fail("late repeat tried to catch up from a frontend stall");
                return 1;
            }

            /* The 32-bit millisecond clock wraps after about 49 days. Keep
             * the same exact 500 ms delay across that wrap. */
            release_keys(2U);
            test_time_ms = UINT32_MAX - 200U;
            (void)onee_input_service_keyboard_report(
                2U, 0U, boot_keys, 6U);
            onee_input_service_poll();
            before = writes_to(ONEE_INPUT_KEY_FIFO_REG);
            test_time_ms += ONEE_INPUT_REPEAT_DELAY_MS - 1U;
            onee_input_service_poll();
            if (writes_to(ONEE_INPUT_KEY_FIFO_REG) != before) {
                fail("held key repeated early across millisecond wrap");
                return 1;
            }
            test_time_ms += 1U;
            onee_input_service_poll();
            if (writes_to(ONEE_INPUT_KEY_FIFO_REG) != before + 1U) {
                fail("held key did not repeat on time across millisecond wrap");
                return 1;
            }

            /* A newer repeatable key wins. Releasing it reselects the most
             * recent repeatable key which remains held. */
            (void)onee_input_service_keyboard_report(
                2U, 0U, repeat_keys, 2U);
            onee_input_service_poll();
            before = writes_to(ONEE_INPUT_KEY_FIFO_REG);
            test_time_ms += ONEE_INPUT_REPEAT_DELAY_MS - 1U;
            onee_input_service_poll();
            if (writes_to(ONEE_INPUT_KEY_FIFO_REG) != before) {
                fail("newest key repeated before its initial delay");
                return 1;
            }
            test_time_ms += 1U;
            onee_input_service_poll();
            if (writes_to(ONEE_INPUT_KEY_FIFO_REG) != before + 1U ||
                last_write(ONEE_INPUT_KEY_FIFO_REG) != 'b') {
                fail("newest held repeatable key did not win");
                return 1;
            }
            (void)onee_input_service_keyboard_report(
                2U, 0U, boot_keys, 1U);
            onee_input_service_poll();
            before = writes_to(ONEE_INPUT_KEY_FIFO_REG);
            test_time_ms += ONEE_INPUT_REPEAT_DELAY_MS - 1U;
            onee_input_service_poll();
            if (writes_to(ONEE_INPUT_KEY_FIFO_REG) != before) {
                fail("reselected key repeated before a fresh delay");
                return 1;
            }
            test_time_ms += 1U;
            onee_input_service_poll();
            if (writes_to(ONEE_INPUT_KEY_FIFO_REG) != before + 1U ||
                last_write(ONEE_INPUT_KEY_FIFO_REG) != 'a') {
                fail("release did not reselect the newest remaining key");
                return 1;
            }

            (void)onee_input_service_keyboard_report(
                1U, 0U, other_key, 1U);
            onee_input_service_poll();
            if (last_write(ONEE_INPUT_KEY_FIFO_REG) != 'c') {
                fail("newer key on another HID slot did not take ownership");
                return 1;
            }
            onee_input_service_disconnect(1U);
            onee_input_service_poll();
            before = writes_to(ONEE_INPUT_KEY_FIFO_REG);
            test_time_ms += ONEE_INPUT_REPEAT_DELAY_MS - 1U;
            onee_input_service_poll();
            if (writes_to(ONEE_INPUT_KEY_FIFO_REG) != before) {
                fail("disconnect reselect repeated before a fresh delay");
                return 1;
            }
            test_time_ms += 1U;
            onee_input_service_poll();
            if (writes_to(ONEE_INPUT_KEY_FIFO_REG) != before + 1U ||
                last_write(ONEE_INPUT_KEY_FIFO_REG) != 'a') {
                fail("disconnect did not reselect the newest remaining key");
                return 1;
            }

            /* Caps Lock, Pause, an unmapped F1, and a modifier-only report
             * must leave no repeat source once all mapped keys are up. */
            release_keys(2U);
            boot_keys[0] = HID_KBD_USAGE_CAPSLOCK;
            (void)onee_input_service_keyboard_report(2U, 0U,
                                                       boot_keys, 6U);
            onee_input_service_poll();
            boot_keys[0] = HID_KBD_USAGE_F1;
            (void)onee_input_service_keyboard_report(2U, 0U,
                                                       boot_keys, 6U);
            onee_input_service_poll();
            (void)onee_input_service_keyboard_report(2U, 0U,
                                                       pause_key, 1U);
            onee_input_service_poll();
            (void)onee_input_service_keyboard_report(2U, HID_MOD_LSHIFT,
                                                       NULL, 0U);
            before = writes_to(ONEE_INPUT_KEY_FIFO_REG);
            test_time_ms += ONEE_INPUT_REPEAT_DELAY_MS +
                            ONEE_INPUT_REPEAT_RATE_MS;
            onee_input_service_poll();
            if (writes_to(ONEE_INPUT_KEY_FIFO_REG) != before) {
                fail("non-repeatable HID input generated a repeat");
                return 1;
            }

            /* Restore Caps Lock for the remaining lower-case checks. */
            boot_keys[0] = HID_KBD_USAGE_CAPSLOCK;
            (void)onee_input_service_keyboard_report(2U, 0U,
                                                       boot_keys, 6U);
            onee_input_service_poll();

            release_keys(2U);
            boot_keys[0] = HID_KBD_USAGE_A + 1U;
            registers[0x5CU] = 1UL << 12;
            (void)onee_input_service_keyboard_report(2U, 0U,
                                                       boot_keys, 6U);
            onee_input_service_poll();
            if (writes_to(ONEE_INPUT_KEY_FIFO_REG) != before) {
                fail("software queue ignored PL FIFO full backpressure");
                return 1;
            }
            registers[0x5CU] = 0U;
            onee_input_service_poll();
            if (last_write(ONEE_INPUT_KEY_FIFO_REG) != 'b') {
                fail("queued key did not drain after PL FIFO became ready");
                return 1;
            }

            release_keys(2U);
            before = writes_to(ONEE_INPUT_KEY_FIFO_REG);
            before_control = writes_to(ONEE_INPUT_CONTROL_REG);
            if (onee_input_service_keyboard_report(
                    2U, HID_MOD_LCTRL | HID_MOD_RALT,
                    delete_key, 1U) == 0U) {
                fail("Ctrl+Alt+Delete was not consumed as a cold reboot chord");
                return 1;
            }
            onee_input_service_poll();
            if (writes_to(ONEE_INPUT_CONTROL_REG) != before_control ||
                writes_to(ONEE_INPUT_KEY_FIFO_REG) != before ||
                (last_write(ONEE_INPUT_LIVE_REG) & 7U) != 0U) {
                fail("cold reboot chord bypassed the ordered main-loop path");
                return 1;
            }
            if (onee_input_service_take_cold_reboot_request() == 0U ||
                onee_input_service_take_cold_reboot_request() != 0U) {
                fail("cold reboot edge was not consumed exactly once");
                return 1;
            }
            onee_input_service_prepare_cold_reboot();
            if (writes_to(ONEE_INPUT_CONTROL_REG) != before_control + 1U ||
                last_write(ONEE_INPUT_CONTROL_REG) !=
                    (ONEE_INPUT_CONTROL_FIFO_FLUSH_BIT |
                     ONEE_INPUT_CONTROL_RELEASE_LIVE_BIT) ||
                writes_to(ONEE_INPUT_KEY_FIFO_REG) != before) {
                fail("cold reboot did not flush queued PL and live input");
                return 1;
            }

            /* Every chord member stays hidden through any release order. */
            (void)onee_input_service_keyboard_report(
                2U, HID_MOD_RALT, delete_key, 1U);
            onee_input_service_poll();
            (void)onee_input_service_keyboard_report(
                2U, 0U, delete_key, 1U);
            onee_input_service_poll();
            if (writes_to(ONEE_INPUT_KEY_FIFO_REG) != before ||
                (last_write(ONEE_INPUT_LIVE_REG) & 7U) != 0U) {
                fail("a released Ctrl+Alt+Delete member leaked to Apple input");
                return 1;
            }

            release_keys(2U);
            (void)onee_input_service_keyboard_report(
                2U, HID_MOD_LALT | HID_MOD_RALT, NULL, 0U);
            onee_input_service_poll();
            if ((last_write(ONEE_INPUT_LIVE_REG) & 7U) != 6U) {
                fail("Open and Closed Apple live bits are wrong");
                return 1;
            }
            (void)onee_input_service_keyboard_report(
                2U, HID_MOD_LGUI | HID_MOD_RGUI, NULL, 0U);
            onee_input_service_poll();
            if ((last_write(ONEE_INPUT_LIVE_REG) & 7U) != 0U) {
                fail("GUI modifiers leaked into the fixed Apple keys");
                return 1;
            }

            memset(&joystick, 0, sizeof(joystick));
            joystick.buttons_valid = 1U;
            joystick.buttons = 5U;
            joystick.axis_valid_mask =
                (1U << ONEE_INPUT_AXIS_X) |
                (1U << ONEE_INPUT_AXIS_Y) |
                (1U << ONEE_INPUT_AXIS_Z) |
                (1U << ONEE_INPUT_AXIS_RZ);
            joystick.axis[ONEE_INPUT_AXIS_X] = -32768;
            joystick.logical_min[ONEE_INPUT_AXIS_X] = -32768;
            joystick.logical_max[ONEE_INPUT_AXIS_X] = 32767;
            joystick.axis[ONEE_INPUT_AXIS_Y] = 32767;
            joystick.logical_min[ONEE_INPUT_AXIS_Y] = -32768;
            joystick.logical_max[ONEE_INPUT_AXIS_Y] = 32767;
            joystick.axis[ONEE_INPUT_AXIS_Z] = 512;
            joystick.logical_min[ONEE_INPUT_AXIS_Z] = 0;
            joystick.logical_max[ONEE_INPUT_AXIS_Z] = 1023;
            joystick.axis[ONEE_INPUT_AXIS_RZ] = 25;
            joystick.logical_min[ONEE_INPUT_AXIS_RZ] = 0;
            joystick.logical_max[ONEE_INPUT_AXIS_RZ] = 100;
            onee_input_service_joystick_report(3U, &joystick);
            onee_input_service_poll();
            if (last_write(ONEE_INPUT_PADDLES_REG) != 0x4080FF00UL ||
                (last_write(ONEE_INPUT_LIVE_REG) & 0x38U) != 0x28U) {
                fail("axis normalization or PB0-PB2 mapping is wrong");
                return 1;
            }

            joystick.buttons = 2U;
            joystick.axis_valid_mask = (1U << ONEE_INPUT_AXIS_X) |
                                       (1U << ONEE_INPUT_AXIS_Y);
            joystick.axis[ONEE_INPUT_AXIS_X] = 32767;
            joystick.axis[ONEE_INPUT_AXIS_Y] = -32768;
            onee_input_service_joystick_report(1U, &joystick);
            onee_input_service_poll();
            if ((last_write(ONEE_INPUT_PADDLES_REG) & 0xFFFFU) != 0x00FFU ||
                (last_write(ONEE_INPUT_LIVE_REG) & 0x38U) != 0x10U) {
                fail("lowest HID slot did not take deterministic ownership");
                return 1;
            }

            onee_input_service_disconnect(1U);
            onee_input_service_disconnect(3U);
            onee_input_service_poll();
            if (last_write(ONEE_INPUT_PADDLES_REG) != 0x80808080UL ||
                (last_write(ONEE_INPUT_LIVE_REG) & 0x38U) != 0U) {
                fail("joystick disconnect did not recenter and release buttons");
                return 1;
            }

            release_keys(2U);
            boot_keys[0] = HID_KBD_USAGE_A + 3U;
            (void)onee_input_service_keyboard_report(2U, 0U,
                                                       boot_keys, 6U);
            onee_input_service_poll();
            before = write_count;
            registers[0x5BU] = 0U;
            onee_input_service_poll();
            release_keys(2U);
            boot_keys[0] = HID_KBD_USAGE_A + 2U;
            (void)onee_input_service_keyboard_report(2U, 0U,
                                                       boot_keys, 6U);
            test_time_ms += ONEE_INPUT_REPEAT_DELAY_MS +
                            ONEE_INPUT_REPEAT_RATE_MS;
            onee_input_service_poll();
            if (write_count != before) {
                fail("writes continued after EFFECTIVE dropped");
                return 1;
            }

            puts("ONEE INPUT SERVICE NATIVE PASS");
            return 0;
        }
    '''), encoding="utf-8")

    compile_cmd = [
        str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror",
        str(harness), "-o", str(executable),
        f"-I{FRONTEND}",
        f"-I{ROOT / 'ps_sources' / 'lib'}",
        f"-I{ROOT / 'third_party' / 'CherryUSB' / 'common'}",
        f"-I{ROOT / 'third_party' / 'CherryUSB' / 'class' / 'hid'}",
    ]
    if "mingw" in compiler.as_posix().lower():
        compile_cmd.insert(5, "-static")
    compiled = subprocess.run(
        compile_cmd, cwd=ROOT, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    )
    if compiled.returncode != 0:
        print(compiled.stdout)
        print("FAIL native_behavior_test: host compilation failed")
        return False
    ran = subprocess.run(
        [str(executable)], cwd=ROOT, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    )
    if ran.returncode != 0 or "ONEE INPUT SERVICE NATIVE PASS" not in ran.stdout:
        print(ran.stdout)
        print("FAIL native_behavior_test: harness failed")
        return False
    print("PASS native_behavior_test")
    return True


def main() -> int:
    failures = []
    for test in TESTS:
        try:
            test()
        except TestFailure as exc:
            failures.append((test.__name__, str(exc)))
            print(f"FAIL {test.__name__}: {exc}")
        else:
            print(f"PASS {test.__name__}")
    native_ok = run_native_behavior_test()
    if failures or not native_ok:
        print(f"{len(TESTS) - len(failures)} of {len(TESTS)} tests passed; "
              f"{len(failures)} failed")
        return 1
    print(f"{len(TESTS)} source checks and native behavior test passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
