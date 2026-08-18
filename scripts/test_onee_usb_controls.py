#!/usr/bin/env python3
"""Native behavior checks for the fixed ONE//e USB controls."""

from __future__ import annotations

import shutil
import subprocess
import textwrap
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build" / "onee_usb_controls_test"
FRONTEND = ROOT / "ps_sources" / "frontend"
CHERRY_HID = ROOT / "third_party" / "CherryUSB" / "class" / "hid"


def find_compiler() -> str | None:
    for name in ("gcc", "cc", "clang"):
        compiler = shutil.which(name)
        if compiler:
            return compiler
    xilinx = Path("C:/Xilinx")
    if xilinx.exists():
        matches = sorted(
            xilinx.glob(
                "*/tps/mingw/*/win64.o/nt/bin/x86_64-w64-mingw32-gcc.exe"
            ),
            reverse=True,
        )
        if matches:
            return str(matches[0])
    return None


def main() -> int:
    compiler = find_compiler()
    if compiler is None:
        print("SKIP ONE//e USB control native test: no host C compiler")
        return 0

    if BUILD.exists():
        shutil.rmtree(BUILD)
    BUILD.mkdir(parents=True)
    harness = BUILD / "onee_usb_controls_test.c"
    executable = BUILD / "onee_usb_controls_test.exe"
    harness.write_text(textwrap.dedent(r'''
        #include <stdint.h>
        #include <stdio.h>
        #define __PACKED __attribute__((packed))
        #include "onee_usb_controls.h"

        static int check(int condition, const char *message)
        {
            if (!condition) {
                fprintf(stderr, "FAIL: %s\n", message);
                return 0;
            }
            return 1;
        }

        int main(void)
        {
            usb_hid_menu_action_t action;
            uint8_t route;

            if (!check(onee_usb_fixed_mode_active(
                           CARD_CTRL_ONEE_STATUS_REQUEST_BIT) != 0U &&
                       onee_usb_fixed_mode_active(
                           CARD_CTRL_ONEE_STATUS_EFFECTIVE_BIT) != 0U &&
                       onee_usb_fixed_mode_active(
                           CARD_CTRL_ONEE_STATUS_REQUEST_BIT |
                           CARD_CTRL_ONEE_STATUS_EFFECTIVE_BIT) != 0U &&
                       onee_usb_fixed_mode_active(
                           CARD_CTRL_ONEE_STATUS_SELECTED_BIT) == 0U,
                       "fixed routing must use only REQUEST/EFFECTIVE") ||
                !check(onee_usb_keyboard_action(
                           HID_KBD_USAGE_PAUSE, 0U, 1U) ==
                           USB_HID_MENU_ACTION_OPEN,
                       "Pause/Break must open the ONE//e menu") ||
                !check(onee_usb_keyboard_action(
                           HID_KBD_USAGE_PAUSE, 1U, 1U) ==
                           USB_HID_MENU_ACTION_CLOSE,
                       "Pause/Break must close the ONE//e menu") ||
                !check(onee_usb_keyboard_action(
                           HID_KBD_USAGE_PAUSE, 0U, 0U) ==
                           USB_HID_MENU_ACTION_NONE,
                       "Pause/Break must retain normal routing outside ONE//e") ||
                !check(onee_usb_apple_usage_allowed(
                           HID_KBD_USAGE_PAUSE, 1U) == 0U &&
                       onee_usb_apple_usage_allowed(
                           HID_KBD_USAGE_PAUSE, 0U) != 0U &&
                       onee_usb_apple_usage_allowed(
                           HID_KBD_USAGE_PRINTSCN, 1U) != 0U &&
                       onee_usb_apple_usage_allowed(
                           HID_KBD_USAGE_KPD0, 1U) != 0U,
                       "only fixed ONE//e control keys may be withheld from Apple input") ||
                !check(onee_usb_fixed_keyboard_action(
                           HID_KBD_USAGE_PAGEUP, 1U) ==
                           USB_HID_MENU_ACTION_NONE &&
                       onee_usb_fixed_keyboard_action(
                           HID_KBD_USAGE_PRINTSCN, 0U) ==
                           USB_HID_MENU_ACTION_NONE &&
                       onee_usb_fixed_keyboard_action(
                           HID_KBD_USAGE_KPDPLUS, 0U) ==
                           USB_HID_MENU_ACTION_NONE,
                       "all non-fixed keys must use saved bindings") ||
                !check(onee_usb_fixed_apple_modifier(
                           (uint8_t)((1U << 0) | (1U << 1) | (1U << 2) |
                                     (1U << 3) | (1U << 6) | (1U << 7))) ==
                           (uint8_t)((1U << 0) | (1U << 1) |
                                     (1U << 2) | (1U << 6)),
                       "Alt must own the Apple keys while GUI is removed") ||
                !check(onee_usb_axis_direction(127, 0, 255) == 0 &&
                       onee_usb_axis_direction(0, 0, 255) < 0 &&
                       onee_usb_axis_direction(255, 0, 255) > 0,
                       "absolute axes must distinguish neutral from held directions") ||
                !check(onee_usb_hat_active(0) != 0U &&
                       onee_usb_hat_active(7) != 0U &&
                       onee_usb_hat_active(8) == 0U,
                       "hat directions must stay active until the neutral value")) {
                return 1;
            }

            action = onee_usb_fixed_keyboard_action(HID_KBD_USAGE_F1, 0U);
            route = onee_usb_fixed_route(HID_KBD_USAGE_F1, action);
            if (!check(route == 0U,
                       "a non-fixed key must fall through to its saved binding")) {
                return 1;
            }

            action = onee_usb_fixed_keyboard_action(
                HID_KBD_USAGE_PAUSE, 0U);
            route = onee_usb_fixed_route(HID_KBD_USAGE_PAUSE, action);
            if (!check(route == (ONEE_USB_ROUTE_PUSH_NOW |
                                 ONEE_USB_ROUTE_CONSUME_SOURCE),
                       "Pause/Break must act at once without a saved fallback")) {
                return 1;
            }

            route = onee_usb_fixed_route(
                HID_KBD_USAGE_LALT, USB_HID_MENU_ACTION_NONE);
            if (!check(route == ONEE_USB_ROUTE_CONSUME_SOURCE &&
                       onee_usb_fixed_route(
                           HID_KBD_USAGE_RALT,
                           USB_HID_MENU_ACTION_NONE) ==
                           ONEE_USB_ROUTE_CONSUME_SOURCE,
                       "fixed Apple modifiers must not use saved bindings")) {
                return 1;
            }

            puts("ONEE USB CONTROLS NATIVE PASS");
            return 0;
        }
    '''), encoding="utf-8")

    compile_result = subprocess.run(
        [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
         str(harness), "-o", str(executable),
         f"-I{FRONTEND}", f"-I{CHERRY_HID}"],
        cwd=ROOT, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    if compile_result.returncode != 0:
        print(compile_result.stdout)
        return 1
    run_result = subprocess.run(
        [str(executable)], cwd=ROOT, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    )
    print(run_result.stdout, end="")
    return 0 if (run_result.returncode == 0 and
                 "ONEE USB CONTROLS NATIVE PASS" in run_result.stdout) else 1


if __name__ == "__main__":
    raise SystemExit(main())
