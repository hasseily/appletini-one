#!/usr/bin/env python3
"""Run the production gamepad bridge and HID slot lifecycle in a native harness."""

from __future__ import annotations

import re
import subprocess
from pathlib import Path

from test_onee_input_service import find_native_c_compiler


ROOT = Path(__file__).resolve().parents[1]
FRONTEND = ROOT / "ps_sources" / "frontend"
BUILD = ROOT / "build" / "usb_gamepad_input_test"
FUNCTIONS = (
    "usb_hid_menu_source_from_keyboard_usage",
    "usb_hid_menu_source_is_keyboard",
    "mouse_menu_push_event", "mouse_menu_push_action",
    "mouse_menu_push_bindable_action", "mouse_menu_action_from_source",
    "mouse_menu_source_button_mask", "menu_hold_source_valid",
    "hid_slot_reset_menu_state", "hid_slot_reset", "hid_slots_reset_all",
    "hid_slots_reset_menu_state", "hid_slot_from_hid", "hid_slot_find_free",
    "usb_gamepad_input_connect", "usb_gamepad_input_disconnect",
    "hid_source_in_list", "menu_source_is_down", "menu_start_open_close_hold",
    "menu_finish_open_close_hold", "menu_poll_open_close_hold",
    "hid_slots_poll_holds", "mouse_menu_start_ok_hold",
    "mouse_menu_finish_ok_hold", "mouse_menu_push_button_edge",
    "mouse_menu_process_buttons", "hid_axis_active_from_rest",
    "hid_menu_push_hat", "hid_menu_push_axis", "hid_process_gamepad_report",
    "usb_gamepad_input_report", "hid_process_report",
    "hid_slots_retry_reports", "usbh_hid_run", "usbh_hid_stop",
    "usb_hid_service_set_menu_capture", "usb_hid_service_set_joystick_preview",
    "usb_hid_service_set_onee_fixed_mode",
    "usb_hid_service_set_onee_input_blocked",
    "usb_hid_service_all_input_released", "usb_hid_service_set_menu_ok_source",
    "usb_hid_service_set_menu_open_close_source",
    "usb_hid_service_pop_menu_event",
)


def extract_function(source: str, name: str) -> tuple[str, str]:
    match = re.search(
        rf"^(?:static\s+)?[A-Za-z_][A-Za-z0-9_ *\t]*\b{re.escape(name)}"
        r"\s*\([^;{}]*\)\s*\{", source, re.MULTILINE,
    )
    if match is None:
        raise AssertionError(f"missing production function {name}")
    brace = match.end() - 1
    depth = 1
    end = brace + 1
    while depth:
        if source[end] == "{":
            depth += 1
        elif source[end] == "}":
            depth -= 1
        end += 1
    return source[match.start():brace].strip() + ";", source[match.start():end]


def main() -> int:
    compiler = find_native_c_compiler()
    if compiler is None:
        print("SKIP native USB gamepad input: no host C compiler")
        return 0

    source = (FRONTEND / "usb_hid_service.c").read_text(encoding="utf-8")
    slot = re.search(r"typedef struct \{.*?\} usb_hid_slot_t;", source, re.DOTALL)
    if slot is None:
        raise AssertionError("missing production USB slot type")
    functions = [extract_function(source, name) for name in FUNCTIONS]
    harness = (ROOT / "scripts" / "fixtures" /
               "usb_gamepad_input_harness.c").read_text(encoding="utf-8")
    harness = harness.replace("/* PRODUCTION_SLOT */", slot.group())
    harness = harness.replace("/* PRODUCTION_PROTOTYPES */",
                              "\n".join(item[0] for item in functions))
    harness = harness.replace("/* PRODUCTION_FUNCTIONS */",
                              "\n\n".join(item[1] for item in functions))
    BUILD.mkdir(parents=True, exist_ok=True)
    c_file = BUILD / "usb_gamepad_input.c"
    executable = BUILD / "usb_gamepad_input.exe"
    c_file.write_text(harness, encoding="utf-8")
    command = [str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror",
               str(c_file), "-I", str(FRONTEND), "-I",
               str(ROOT / "third_party" / "CherryUSB" / "class" / "hid"),
               "-o", str(executable)]
    if "mingw" in str(compiler).lower():
        command.insert(1, "-static")
    compiled = subprocess.run(command, capture_output=True, text=True)
    if compiled.returncode:
        raise AssertionError("gamepad bridge compile failed:\n" +
                             compiled.stdout + compiled.stderr)
    ran = subprocess.run([str(executable)], capture_output=True, text=True)
    if ran.returncode:
        raise AssertionError("gamepad bridge behavior failed:\n" +
                             ran.stdout + ran.stderr)
    print(ran.stdout.strip())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
