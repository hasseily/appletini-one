#!/usr/bin/env python3
"""Native behavior checks for the fixed ONE//e USB controls."""

from __future__ import annotations

import shutil
import subprocess
import textwrap
from pathlib import Path

from test_onee_input_service import find_native_c_compiler


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build" / "onee_usb_controls_test"
FRONTEND = ROOT / "ps_sources" / "frontend"
CHERRY_HID = ROOT / "third_party" / "CherryUSB" / "class" / "hid"


def find_compiler() -> str | None:
    compiler = find_native_c_compiler()
    return str(compiler) if compiler is not None else None


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
                           HID_KBD_USAGE_PAUSE, 0U, 0U, 1U) ==
                           USB_HID_MENU_ACTION_OPEN,
                       "Pause/Break must open the ONE//e menu") ||
                !check(onee_usb_keyboard_action(
                           HID_KBD_USAGE_PAUSE, 0U, 1U, 1U) ==
                           USB_HID_MENU_ACTION_CLOSE,
                       "Pause/Break must close the ONE//e menu") ||
                !check(onee_usb_keyboard_action(
                           HID_KBD_USAGE_PAUSE, 0U, 0U, 0U) ==
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
                           HID_KBD_USAGE_PAGEUP, 0U, 1U) ==
                           USB_HID_MENU_ACTION_NONE &&
                       onee_usb_fixed_keyboard_action(
                           HID_KBD_USAGE_PRINTSCN, 0U, 0U) ==
                           USB_HID_MENU_ACTION_NONE &&
                       onee_usb_fixed_keyboard_action(
                           HID_KBD_USAGE_KPDPLUS, 0U, 0U) ==
                           USB_HID_MENU_ACTION_NONE,
                       "other keys must use saved bindings") ||
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

            static const struct {
                uint8_t usage;
                usb_hid_menu_action_t action;
            } navigation[] = {
                {HID_KBD_USAGE_UP, USB_HID_MENU_ACTION_ITEM_UP},
                {HID_KBD_USAGE_DOWN, USB_HID_MENU_ACTION_ITEM_DOWN},
                {HID_KBD_USAGE_LEFT, USB_HID_MENU_ACTION_LEFT},
                {HID_KBD_USAGE_RIGHT, USB_HID_MENU_ACTION_RIGHT},
                {HID_KBD_USAGE_ENTER, USB_HID_MENU_ACTION_SELECT},
                {HID_KBD_USAGE_KPDEMTER, USB_HID_MENU_ACTION_SELECT},
                {HID_KBD_USAGE_TAB, USB_HID_MENU_ACTION_NEXT_TAB},
                {HID_KBD_USAGE_ESCAPE, USB_HID_MENU_ACTION_BACK},
            };
            for (unsigned i = 0U; i < sizeof(navigation) / sizeof(navigation[0]); ++i) {
                const uint8_t usage = navigation[i].usage;
                if (!check(onee_usb_keyboard_action(usage, 0U, 1U, 1U) ==
                               navigation[i].action &&
                           onee_usb_keyboard_action(usage, 0U, 0U, 1U) ==
                               USB_HID_MENU_ACTION_NONE &&
                           onee_usb_keyboard_action(usage, 0U, 1U, 0U) ==
                               USB_HID_MENU_ACTION_NONE &&
                           onee_usb_apple_usage_allowed(usage, 1U) != 0U,
                           "fixed navigation must require the ONE//e menu and retain guest input") ||
                    !check(onee_usb_fixed_route(usage, navigation[i].action) ==
                               (ONEE_USB_ROUTE_PUSH_NOW | ONEE_USB_ROUTE_CONSUME_SOURCE) &&
                           onee_usb_fixed_route(usage, USB_HID_MENU_ACTION_NONE) ==
                               ONEE_USB_ROUTE_CONSUME_SOURCE,
                           "navigation must bypass conflicting saved actions inside and outside the menu")) {
                    return 1;
                }
            }
            for (unsigned shift = 0U; shift < 2U; ++shift) {
                const uint8_t modifier = (uint8_t)(1U << (shift ? 5U : 1U));
                if (!check(onee_usb_keyboard_action(HID_KBD_USAGE_TAB, modifier, 1U, 1U) ==
                               USB_HID_MENU_ACTION_PREV_TAB &&
                           onee_usb_keyboard_action(HID_KBD_USAGE_TAB, modifier, 0U, 1U) ==
                               USB_HID_MENU_ACTION_NONE,
                           "either Shift must reverse Tab only in the ONE//e menu")) {
                    return 1;
                }
            }
            for (unsigned side = 0U; side < 2U; ++side) {
                const uint8_t usage = side ? HID_KBD_USAGE_RGUI : HID_KBD_USAGE_LGUI;
                if (!check(onee_usb_keyboard_action(usage, 0U, 0U, 1U) ==
                               USB_HID_MENU_ACTION_OPEN &&
                           onee_usb_keyboard_action(usage, 0U, 1U, 1U) ==
                               USB_HID_MENU_ACTION_NONE &&
                           onee_usb_keyboard_action(usage, 0U, 0U, 0U) ==
                               USB_HID_MENU_ACTION_NONE &&
                           onee_usb_apple_usage_allowed(usage, 1U) == 0U,
                           "either Windows key must open without closing or reaching guest input")) {
                    return 1;
                }
            }

            action = onee_usb_fixed_keyboard_action(HID_KBD_USAGE_F1, 0U, 0U);
            route = onee_usb_fixed_route(HID_KBD_USAGE_F1, action);
            if (!check(route == 0U,
                       "a non-fixed key must fall through to its saved binding")) {
                return 1;
            }

            action = onee_usb_fixed_keyboard_action(
                HID_KBD_USAGE_PAUSE, 0U, 0U);
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
