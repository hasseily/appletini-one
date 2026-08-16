#!/usr/bin/env python3
"""Native behavior checks for the fixed ONE//e USB control map."""

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

        #define SOURCE(usage) ((usb_hid_menu_source_t)(USB_HID_MENU_SOURCE_KEY_BASE | (usage)))

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
            const usb_hid_menu_source_t f1 = SOURCE(HID_KBD_USAGE_F1);
            const usb_hid_menu_source_t page_up = SOURCE(HID_KBD_USAGE_PAGEUP);
            const uint8_t reserved_usages[] = {
                HID_KBD_USAGE_PRINTSCN,
                HID_KBD_USAGE_KPD0,
                HID_KBD_USAGE_KPDPLUS,
                HID_KBD_USAGE_KPDHMINUS
            };

            if (!check(onee_usb_fixed_keyboard_action(
                           HID_KBD_USAGE_PAGEUP, 0U, 1U) ==
                           USB_HID_MENU_ACTION_PREV_TAB,
                       "PageUp must select the previous tab") ||
                !check(onee_usb_fixed_keyboard_action(
                           HID_KBD_USAGE_PAGEDOWN, 0U, 1U) ==
                           USB_HID_MENU_ACTION_NEXT_TAB,
                       "PageDown must select the next tab") ||
                !check(onee_usb_fixed_keyboard_action(
                           HID_KBD_USAGE_UP, 0U, 1U) ==
                           USB_HID_MENU_ACTION_ITEM_UP &&
                       onee_usb_fixed_keyboard_action(
                           HID_KBD_USAGE_DOWN, 0U, 1U) ==
                           USB_HID_MENU_ACTION_ITEM_DOWN &&
                       onee_usb_fixed_keyboard_action(
                           HID_KBD_USAGE_LEFT, 0U, 1U) ==
                           USB_HID_MENU_ACTION_LEFT &&
                       onee_usb_fixed_keyboard_action(
                           HID_KBD_USAGE_RIGHT, 0U, 1U) ==
                           USB_HID_MENU_ACTION_RIGHT,
                       "arrow keys must own menu movement") ||
                !check(onee_usb_fixed_keyboard_action(
                           HID_KBD_USAGE_PRINTSCN, 0U, 0U) ==
                           USB_HID_MENU_ACTION_SCREENSHOT_A2,
                       "PrintScreen must capture the Apple screen") ||
                !check(onee_usb_fixed_keyboard_action(
                           HID_KBD_USAGE_PRINTSCN, (1U << 1), 0U) ==
                           USB_HID_MENU_ACTION_SCREENSHOT_1080P,
                       "Shift+PrintScreen must capture the 1080p screen") ||
                !check(onee_usb_fixed_keyboard_action(
                           HID_KBD_USAGE_PRINTSCN, (1U << 5), 0U) ==
                           USB_HID_MENU_ACTION_SCREENSHOT_1080P,
                       "right Shift+PrintScreen must capture the 1080p screen") ||
                !check(onee_usb_fixed_apple_modifier(
                           (uint8_t)((1U << 0) | (1U << 1) | (1U << 5)),
                           1U) == (1U << 0),
                       "PrintScreen must consume both Shift modifiers only") ||
                !check(onee_usb_fixed_apple_modifier(
                           (uint8_t)((1U << 1) | (1U << 5)), 0U) ==
                           (uint8_t)((1U << 1) | (1U << 5)),
                       "Shift must reach Apple input outside the screenshot chord") ||
                !check(onee_usb_axis_direction(127, 0, 255) == 0 &&
                       onee_usb_axis_direction(0, 0, 255) < 0 &&
                       onee_usb_axis_direction(255, 0, 255) > 0,
                       "absolute axes must distinguish neutral from held directions") ||
                !check(onee_usb_hat_active(0) != 0U &&
                       onee_usb_hat_active(7) != 0U &&
                       onee_usb_hat_active(8) == 0U,
                       "hat directions must stay active until the neutral value") ||
                !check(onee_usb_fixed_keyboard_action(
                           HID_KBD_USAGE_KPDPLUS, 0U, 0U) ==
                           USB_HID_MENU_ACTION_VTW_SPEED_UP &&
                       onee_usb_fixed_keyboard_action(
                           HID_KBD_USAGE_KPDHMINUS, 0U, 0U) ==
                           USB_HID_MENU_ACTION_VTW_SPEED_DOWN &&
                       onee_usb_fixed_keyboard_action(
                           HID_KBD_USAGE_KPD0, 0U, 0U) ==
                           USB_HID_MENU_ACTION_VTW_SPEED_TOGGLE,
                       "keypad plus/minus/zero must own acceleration")) {
                return 1;
            }

            action = onee_usb_fixed_keyboard_action(HID_KBD_USAGE_F1, 0U, 0U);
            route = onee_usb_fixed_route(0U, action, f1, f1);
            if (!check(route == ONEE_USB_ROUTE_HOLD_TOGGLE,
                       "saved long-hold key must still open ONE//e menu")) {
                return 1;
            }

            for (size_t i = 0U;
                 i < sizeof(reserved_usages) / sizeof(reserved_usages[0]);
                 ++i) {
                const usb_hid_menu_source_t source = SOURCE(reserved_usages[i]);

                action = onee_usb_fixed_keyboard_action(
                    reserved_usages[i], 0U, 0U);
                route = onee_usb_fixed_route(1U, action, source, source);
                if (!check(route == (ONEE_USB_ROUTE_HOLD_TOGGLE |
                                     ONEE_USB_ROUTE_DELAY_ACTION),
                           "overlapping reserved toggle must delay its fixed short action")) {
                    return 1;
                }
                route = onee_usb_fixed_route(1U, action, source, f1);
                if (!check(route == ONEE_USB_ROUTE_PUSH_NOW,
                           "reserved fixed key must act at once when it is not the hold key")) {
                    return 1;
                }
            }

            action = onee_usb_fixed_keyboard_action(
                HID_KBD_USAGE_PAGEUP, 0U, 1U);
            route = onee_usb_fixed_route(0U, action, page_up, page_up);
            if (!check(route == (ONEE_USB_ROUTE_HOLD_TOGGLE |
                                 ONEE_USB_ROUTE_DELAY_ACTION),
                       "overlapping toggle must delay its fixed short action")) {
                return 1;
            }
            route = onee_usb_fixed_route(0U, action, page_up, f1);
            if (!check(route == ONEE_USB_ROUTE_PUSH_NOW,
                       "fixed PageUp must act when it is not the hold key")) {
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
