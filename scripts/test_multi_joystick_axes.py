#!/usr/bin/env python3
"""Run the real input service with multiple joysticks and both fake PL bridges."""

import subprocess
import textwrap
from pathlib import Path

from test_onee_input_service import find_native_c_compiler


ROOT = Path(__file__).resolve().parents[1]
FRONTEND = ROOT / "ps_sources" / "frontend"
BUILD = ROOT / "build" / "multi_joystick_axes_test"


def main() -> int:
    compiler = find_native_c_compiler()
    if compiler is None:
        print("SKIP multi_joystick_axes: no host C compiler")
        return 0

    BUILD.mkdir(parents=True, exist_ok=True)
    harness = BUILD / "multi_joystick_axes_harness.c"
    executable = BUILD / "multi_joystick_axes_harness.exe"
    harness.write_text(textwrap.dedent(r'''
        #include <stdint.h>
        #include <stdio.h>
        #include <string.h>

        static uint32_t test_reg_read(uint32_t address);
        static void test_reg_write(uint32_t address, uint32_t value);
        uint32_t cherryusb_baremetal_ms(void) { return 0U; }

        #define COMMON_H
        #define REG_READ(address) test_reg_read((uint32_t)(address))
        #define REG_WRITE(address, value) \
            test_reg_write((uint32_t)(address), (uint32_t)(value))
        #include "../../ps_sources/frontend/onee_input_service.c"

        static uint32_t registers[256];
        static uint32_t last_writes[256];
        static uint32_t write_count;

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
            ++write_count;
            last_writes[reg_index(address)] = value;
            if (address == CARD_CTRL_VTW_JOYSTICK_CONTROL_REG) {
                registers[reg_index(address)] =
                    (registers[reg_index(address)] & ~0x0FUL) | (value & 0x0FU);
            }
        }

        onee_service_state_t onee_service_state(void)
        {
            return ONEE_SERVICE_STATE_RUNNING;
        }

        #define CHECK(condition, message) do { \
            if (!(condition)) { \
                fprintf(stderr, "FAIL line %d: %s\n", __LINE__, message); \
                return 1; \
            } \
        } while (0)

        static void reset_service(void)
        {
            memset(registers, 0, sizeof(registers));
            memset(last_writes, 0, sizeof(last_writes));
            write_count = 0U;
            registers[0x5BU] = CARD_CTRL_ONEE_STATUS_EFFECTIVE_BIT;
            registers[0x5FU] = (0xE1UL << 24) | (1UL << 9);
            registers[reg_index(CARD_CTRL_VTW_JOYSTICK_CONTROL_REG)] =
                (CARD_CTRL_VTW_JOYSTICK_SIGNATURE <<
                 CARD_CTRL_VTW_JOYSTICK_SIGNATURE_SHIFT) |
                CARD_CTRL_VTW_JOYSTICK_ENABLED_BIT;
            onee_input_service_init();
            onee_input_service_poll();
        }

        static void report_axes(uint8_t slot, uint8_t mask,
                                const uint8_t axes[6], uint8_t buttons_valid,
                                uint8_t buttons)
        {
            onee_input_joystick_report_t report;
            memset(&report, 0, sizeof(report));
            report.axis_valid_mask = mask;
            report.buttons_valid = buttons_valid;
            report.buttons = buttons;
            for (uint8_t axis = 0U; axis < ONEE_INPUT_AXIS_COUNT; ++axis) {
                report.axis[axis] = axes[axis];
                report.logical_max[axis] = 255;
            }
            onee_input_service_joystick_report(slot, &report);
        }

        static int expect_bridges(uint32_t paddles, uint8_t buttons,
                                  uint8_t present)
        {
            onee_input_service_poll();
            CHECK(last_writes[reg_index(ONEE_INPUT_PADDLES_REG)] == paddles,
                  "ONE//e paddle output differs from expected mapping");
            CHECK(last_writes[reg_index(CARD_CTRL_VTW_JOYSTICK_PADDLES_REG)] ==
                  paddles, "vTW and ONE//e must share paddle values");
            CHECK((last_writes[reg_index(ONEE_INPUT_LIVE_REG)] & 0x38U) ==
                  (uint32_t)buttons << 3, "ONE//e button owner changed");
            CHECK(last_writes[reg_index(CARD_CTRL_VTW_JOYSTICK_CONTROL_REG)] ==
                  ((uint32_t)buttons << 1 | present),
                  "vTW button ownership or joystick presence changed");
            return 0;
        }

        static int test_independent_devices(void)
        {
            const uint8_t axes1[6] = {20, 30, 40, 50, 60, 70};
            const uint8_t axes3[6] = {24, 64, 44, 54, 64, 74};
            const uint8_t axes5[6] = {28, 38, 48, 192, 68, 78};
            const uint8_t axes7[6] = {32, 42, 52, 62, 72, 250};
            const uint8_t partial[6] = {77, 0, 0, 0, 0, 0};
            const uint8_t lower[6] = {240, 241, 242, 243, 244, 245};
            const uint8_t moved[6] = {40, 0, 0, 0, 0, 0};
            onee_input_joystick_config_t config;
            onee_input_joystick_snapshot_t snapshot;
            uint32_t before;

            reset_service();
            report_axes(1U, 0x3FU, axes1, 1U, 1U);
            report_axes(3U, 0x3FU, axes3, 1U, 2U);
            report_axes(5U, 0x3FU, axes5, 1U, 4U);
            report_axes(7U, 0x3FU, axes7, 1U, 7U);
            onee_input_service_default_joystick_config(&config);
            config.paddle[0].device = 2U;
            config.paddle[0].source = ONEE_INPUT_JOYSTICK_SOURCE_X;
            config.paddle[1].device = 4U;
            config.paddle[1].source = ONEE_INPUT_JOYSTICK_SOURCE_Y;
            config.paddle[2].device = 6U;
            config.paddle[2].source = ONEE_INPUT_JOYSTICK_SOURCE_RX;
            config.paddle[3].device = 8U;
            config.paddle[3].source = ONEE_INPUT_JOYSTICK_SOURCE_RZ;
            onee_input_service_set_joystick_config(&config);
            CHECK(expect_bridges(0xFAC04014UL, 1U, 1U) == 0,
                  "four simultaneous devices did not supply separate paddles");

            before = write_count;
            onee_input_service_get_joystick_snapshot(&snapshot);
            CHECK(write_count == before && snapshot.connected_mask == 0xAAU &&
                  snapshot.owner_slot == 1U && snapshot.buttons == 1U,
                  "snapshot lost connected devices, button owner, or wrote MMIO");
            for (uint8_t paddle = 0U; paddle < ONEE_INPUT_PADDLE_COUNT; ++paddle) {
                CHECK(snapshot.paddle_slots[paddle] == 2U * paddle + 1U,
                      "snapshot reported the wrong per-paddle device");
            }
            CHECK(snapshot.devices[7].connected != 0U &&
                  snapshot.devices[7].axis_valid_mask == 0x3FU &&
                  snapshot.devices[7].axis[ONEE_INPUT_AXIS_RZ] == 250U,
                  "last slot or its sixth axis is absent from preview");

            config.paddle[0].invert = 1U;
            config.paddle[1].sensitivity_percent = 50U;
            config.paddle[2].deadzone_percent = 25U;
            config.paddle[3].sensitivity_percent = 200U;
            onee_input_service_set_joystick_config(&config);
            CHECK(expect_bridges(0xFFAB60EBUL, 1U, 1U) == 0,
                  "per-paddle transforms crossed device boundaries");

            report_axes(7U, 1U << ONEE_INPUT_AXIS_X, partial, 0U, 0U);
            CHECK(expect_bridges(0xFFAB60EBUL, 1U, 1U) == 0,
                  "partial report erased another axis from its device");
            onee_input_service_get_joystick_snapshot(&snapshot);
            CHECK(snapshot.devices[7].axis[ONEE_INPUT_AXIS_X] == 77U &&
                  snapshot.devices[7].axis[ONEE_INPUT_AXIS_RZ] == 250U &&
                  snapshot.devices[7].buttons == 7U,
                  "partial reports must preserve absent axes and buttons");

            report_axes(0U, 0x3FU, lower, 1U, 4U);
            CHECK(expect_bridges(0xFFAB60EBUL, 4U, 1U) == 0,
                  "a lower slot stole explicitly mapped paddles");
            onee_input_service_disconnect(0U);
            CHECK(expect_bridges(0xFFAB60EBUL, 1U, 1U) == 0,
                  "unrelated disconnect disturbed explicit mappings");
            onee_input_service_disconnect(3U);
            CHECK(expect_bridges(0xFFAB80EBUL, 1U, 1U) == 0,
                  "disconnect must center only paddles mapped to that slot");
            onee_input_service_get_joystick_snapshot(&snapshot);
            CHECK(snapshot.paddle_slots[1] == ONEE_INPUT_DEVICE_SLOT_COUNT &&
                  snapshot.devices[3].connected == 0U &&
                  snapshot.devices[3].axis_valid_mask == 0U,
                  "disconnected slot remained available in preview");
            report_axes(3U, 1U << ONEE_INPUT_AXIS_X, partial, 0U, 0U);
            CHECK(expect_bridges(0xFFAB80EBUL, 1U, 1U) == 0,
                  "same-slot reconnect reused an old axis value");
            onee_input_service_get_joystick_snapshot(&snapshot);
            CHECK(snapshot.paddle_slots[1] == 3U &&
                  snapshot.devices[3].axis_valid_mask == 1U &&
                  snapshot.devices[3].axis[ONEE_INPUT_AXIS_Y] == 128U &&
                  snapshot.devices[3].buttons == 0U,
                  "reconnect leaked stale axes or buttons");

            report_axes(3U, 0x3FU, axes3, 1U, 2U);
            onee_input_service_set_blocked(1U);
            CHECK(expect_bridges(0x80808080UL, 0U, 1U) == 0,
                  "menu blocking must center both hardware consumers");
            report_axes(1U, 1U << ONEE_INPUT_AXIS_X, moved, 0U, 0U);
            CHECK(expect_bridges(0x80808080UL, 0U, 1U) == 0,
                  "menu blocking leaked live motion");
            onee_input_service_get_joystick_snapshot(&snapshot);
            CHECK(snapshot.paddles[0] == 215U && snapshot.paddles[1] == 96U &&
                  snapshot.paddles[2] == 171U && snapshot.paddles[3] == 255U &&
                  snapshot.devices[1].axis[ONEE_INPUT_AXIS_X] == 40U,
                  "menu preview must retain live motion from all devices");
            onee_input_service_set_blocked(0U);
            CHECK(expect_bridges(0xFFAB60D7UL, 1U, 1U) == 0,
                  "menu exit must apply saved inputs without another report");

            onee_input_service_set_joystick_config(NULL);
            CHECK(expect_bridges(0x3C321E28UL, 1U, 1U) == 0,
                  "Auto must retain lowest-slot X/Y/Rx/Ry behavior");
            onee_input_service_get_joystick_config(&config);
            config.paddle[0].device = 255U;
            config.paddle[1].device = 9U;
            onee_input_service_set_joystick_config(&config);
            onee_input_service_get_joystick_config(&config);
            CHECK(config.paddle[0].device == 0U && config.paddle[1].device == 0U,
                  "invalid device selectors must sanitize to Auto");
            CHECK(expect_bridges(0x3C321E28UL, 1U, 1U) == 0,
                  "invalid device selector read outside slot storage");

            for (uint8_t paddle = 0U; paddle < ONEE_INPUT_PADDLE_COUNT; ++paddle) {
                config.paddle[paddle].source = ONEE_INPUT_JOYSTICK_SOURCE_OFF;
                config.paddle[paddle].invert = 1U;
                config.paddle[paddle].device = 8U;
            }
            onee_input_service_set_joystick_config(&config);
            CHECK(expect_bridges(0x80808080UL, 1U, 1U) == 0,
                  "all Off must center paddles and preserve button ownership");
            onee_input_service_get_joystick_snapshot(&snapshot);
            for (uint8_t paddle = 0U; paddle < ONEE_INPUT_PADDLE_COUNT; ++paddle) {
                CHECK(snapshot.paddle_slots[paddle] == ONEE_INPUT_DEVICE_SLOT_COUNT,
                      "Off paddle still reports a selected live device");
            }
            return 0;
        }

        static int test_all_48_sources(void)
        {
            onee_input_joystick_config_t config;
            onee_input_joystick_snapshot_t snapshot;
            uint8_t axes[6];

            reset_service();
            for (uint8_t slot = 0U; slot < 8U; ++slot) {
                for (uint8_t axis = 0U; axis < 6U; ++axis) {
                    axes[axis] = 10U + slot * 30U + axis * 4U;
                }
                report_axes(slot, 0x3FU, axes, 1U, slot & 7U);
            }
            onee_input_service_get_joystick_snapshot(&snapshot);
            CHECK(snapshot.connected_mask == 0xFFU,
                  "all eight device slots must coexist");
            onee_input_service_default_joystick_config(&config);
            for (uint8_t slot = 0U; slot < 8U; ++slot) {
                for (uint8_t axis = 0U; axis < 6U; ++axis) {
                    const uint8_t expected = 10U + slot * 30U + axis * 4U;
                    for (uint8_t paddle = 0U; paddle < 4U; ++paddle) {
                        config.paddle[paddle].device = slot + 1U;
                        config.paddle[paddle].source =
                            ONEE_INPUT_JOYSTICK_SOURCE_X + axis;
                    }
                    onee_input_service_set_joystick_config(&config);
                    CHECK(expect_bridges(0x01010101UL * expected, 0U, 1U) == 0,
                          "one of 48 axes could not feed every Apple paddle");
                }
            }
            onee_input_service_release_all();
            CHECK(expect_bridges(0x80808080UL, 0U, 0U) == 0,
                  "teardown must release all mapped devices");
            return 0;
        }

        int main(void)
        {
            if (test_independent_devices() || test_all_48_sources()) {
                return 1;
            }
            puts("MULTI JOYSTICK AXES NATIVE PASS");
            return 0;
        }
    '''), encoding="utf-8")

    command = [str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror"]
    if "mingw" in compiler.as_posix().lower():
        command.append("-static")
    command.extend([
        str(harness), "-o", str(executable),
        f"-I{FRONTEND}", f"-I{ROOT / 'ps_sources' / 'lib'}",
        f"-I{ROOT / 'third_party' / 'CherryUSB' / 'common'}",
        f"-I{ROOT / 'third_party' / 'CherryUSB' / 'class' / 'hid'}",
    ])
    compiled = subprocess.run(command, cwd=ROOT, text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if compiled.returncode:
        print(compiled.stdout)
        return 1
    ran = subprocess.run([str(executable)], cwd=ROOT, text=True,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    print(ran.stdout, end="")
    return ran.returncode


if __name__ == "__main__":
    raise SystemExit(main())
