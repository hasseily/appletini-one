#!/usr/bin/env python3
"""Exercise wired DualShock 4 report decoding through the production header."""

from __future__ import annotations

import os
import subprocess
from pathlib import Path

from test_onee_input_service import find_native_c_compiler


ROOT = Path(__file__).resolve().parents[1]
FRONTEND = ROOT / "ps_sources" / "frontend"
BUILD = ROOT / "build" / "usb_ps4_test"

# Packet offsets and button meanings are independently documented by TinyUSB:
# https://github.com/hathach/tinyusb/blob/master/examples/host/hid_controller/src/hid_app.c
# Sony USB IDs and the 64-byte report are also in Linux hid-playstation.c.
HARNESS = r"""
#include <stdio.h>
#include <string.h>
#include "usb_ps4_report.h"

#define CHECK(condition, message) do { \
    if (!(condition)) { fprintf(stderr, "FAIL: %s\n", message); return 1; } \
} while (0)

static void neutral(uint8_t data[65])
{
    memset(data, 0, 65);
    data[0] = 1;
    data[1] = data[2] = data[3] = data[4] = 128;
    data[5] = 8;
}

int main(void)
{
    uint8_t data[65];
    uint8_t hat = 99, extra = 99;
    onee_input_joystick_report_t report, unchanged;
    const uint8_t button_byte[] = {5, 5, 5, 5, 6, 6, 6, 6};
    const uint8_t button_mask[] = {0x20, 0x40, 0x10, 0x80, 1, 2, 0x10, 0x20};
    const uint8_t extra_byte[] = {6, 6, 6, 6, 7, 7};
    const uint8_t extra_mask[] = {4, 8, 0x40, 0x80, 1, 2};
    const uint8_t axis_byte[] = {1, 2, 8, 3, 4, 9};

    CHECK(usb_ps4_supported(0x054c, 0x05c4), "first DS4 revision missing");
    CHECK(usb_ps4_supported(0x054c, 0x09cc), "second DS4 revision missing");
    CHECK(!usb_ps4_supported(0x045e, 0x09cc), "non-Sony device claimed");
    CHECK(!usb_ps4_supported(0x054c, 0x0ce6), "DualSense claimed as DS4");

    neutral(data);
    CHECK(usb_ps4_decode(data, 64, &report, &hat, &extra), "neutral rejected");
    CHECK(report.axis_valid_mask == 0x3f && report.buttons_valid,
          "complete state not published");
    CHECK(hat == 8 && extra == 0 && report.buttons == 0,
          "neutral controller holds a button or direction");
    CHECK(report.axis[0] == 128 && report.axis[1] == 128 &&
          report.axis[3] == 128 && report.axis[4] == 128 &&
          report.axis[2] == 0 && report.axis[5] == 0,
          "resting sticks or triggers wrong");

    /* Move one physical axis at a time through its whole range. This catches
     * the generic parser's Rz/right-stick-Y versus Rx/trigger alias. */
    for (unsigned axis = 0; axis < 6; ++axis) {
        for (unsigned value = 0; value < 256; ++value) {
            neutral(data);
            data[axis_byte[axis]] = (uint8_t)value;
            CHECK(usb_ps4_decode(data, 64, &report, &hat, &extra), "axis rejected");
            for (unsigned other = 0; other < 6; ++other) {
                int expected = other == axis ? (int)value :
                               (other == 2 || other == 5 ? 0 : 128);
                CHECK(report.axis[other] == expected, "axis moved another control");
                CHECK(report.logical_min[other] == 0 && report.logical_max[other] == 255,
                      "unsigned axis range wrong");
            }
        }
    }
    for (unsigned button = 0; button < 8; ++button) {
        neutral(data);
        data[button_byte[button]] |= button_mask[button];
        CHECK(usb_ps4_decode(data, 64, &report, &hat, &extra), "button rejected");
        CHECK(report.buttons == (1U << button) && extra == 0,
              "button order does not match shared Xbox/Apple mapping");
        neutral(data);
        CHECK(usb_ps4_decode(data, 64, &report, &hat, &extra) && report.buttons == 0,
              "button release lost");
    }
    for (unsigned button = 0; button < 6; ++button) {
        neutral(data);
        data[extra_byte[button]] |= extra_mask[button];
        CHECK(usb_ps4_decode(data, 64, &report, &hat, &extra) && extra == 1,
              "held auxiliary button missed by release guard");
        CHECK(report.buttons == 0, "auxiliary button leaked into Apple buttons");
    }
    neutral(data);
    data[7] = 0xfc;
    CHECK(usb_ps4_decode(data, 64, &report, &hat, &extra) && extra == 0,
          "packet counter blocks release");
    for (unsigned direction = 0; direction < 16; ++direction) {
        neutral(data);
        data[5] = (uint8_t)direction;
        CHECK(usb_ps4_decode(data, 64, &report, &hat, &extra), "hat rejected");
        CHECK(hat == (direction < 8 ? direction : 8), "hat direction/null wrong");
    }
    neutral(data);
    memset(&report, 0xa5, sizeof(report));
    unchanged = report;
    hat = 55; extra = 66;
    for (unsigned length = 0; length < 64; ++length) {
        CHECK(!usb_ps4_decode(data, length, &report, &hat, &extra),
              "truncated USB packet accepted");
    }
    CHECK(!usb_ps4_decode(data, 65, &report, &hat, &extra), "oversized packet accepted");
    data[0] = 0x11;
    CHECK(!usb_ps4_decode(data, 64, &report, &hat, &extra), "Bluetooth report accepted");
    data[0] = 1;
    CHECK(!usb_ps4_decode(NULL, 64, &report, &hat, &extra), "null data accepted");
    CHECK(!usb_ps4_decode(data, 64, NULL, &hat, &extra), "null report accepted");
    CHECK(!usb_ps4_decode(data, 64, &report, NULL, &extra), "null hat accepted");
    CHECK(!usb_ps4_decode(data, 64, &report, &hat, NULL), "null extra accepted");
    CHECK(!memcmp(&report, &unchanged, sizeof(report)) && hat == 55 && extra == 66,
          "rejected packet changed held state");
    puts("PASS wired PS4 reports: six axes, eight buttons, hat, release guard, bounds");
    return 0;
}
"""


def main() -> int:
    compiler = find_native_c_compiler()
    if compiler is None:
        print("SKIP PS4 runtime tests: no native C compiler")
        return 0
    BUILD.mkdir(parents=True, exist_ok=True)
    source = BUILD / "usb_ps4_harness.c"
    executable = BUILD / "usb_ps4_harness.exe"
    source.write_text(HARNESS, encoding="utf-8")
    command = [str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror"]
    if "mingw" in compiler.as_posix().lower():
        command.append("-static")
    command.extend([f"-I{FRONTEND}", str(source), "-o", str(executable)])
    environment = os.environ.copy()
    environment["PATH"] = str(compiler.parent) + os.pathsep + environment.get("PATH", "")
    result = subprocess.run(command, cwd=ROOT, env=environment, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode:
        print(result.stdout, end="")
        return result.returncode
    result = subprocess.run([str(executable)], cwd=ROOT, env=environment, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    print(result.stdout, end="")
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
