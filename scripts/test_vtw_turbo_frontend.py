#!/usr/bin/env python3
"""Native menu, persistence, and UART contract checks for vTW TURBO."""

from pathlib import Path
import subprocess
import textwrap

from test_onee_vtw_runtime import find_native_c_compiler


ROOT = Path(__file__).resolve().parents[1]
FRONTEND = ROOT / "ps_sources" / "frontend"
BUILD = ROOT / "build" / "vtw_turbo_frontend_test"


def extract(source: str, start: str, end: str) -> str:
    first = source.index(start)
    return source[first:source.index(end, first)]


def main() -> int:
    source = (FRONTEND / "config_menu.c").read_text(encoding="utf-8")
    uart = (FRONTEND / "uart_control.c").read_text(encoding="utf-8")
    speed_command = extract(uart, 'if (str_ieq(argv[1], "speed")',
                            'if (str_ieq(argv[1], "dump")')
    assert 'str_ieq(argv[2], "turbo")' in speed_command
    assert "mode = CARD_CTRL_VTW_SPEED_TURBO;" in speed_command
    assert "config_menu_set_vtw_speed(g_config_menu, mode, divider);" in speed_command
    assert "vtw_service_set_speed(mode, divider);" in speed_command
    assert "config_menu_read_settings_from_path(menu, cfg_path, 1U, NULL)" in source
    assert "config_menu_save_settings_to_path(menu, cfg_path, NULL)" in source

    compiler = find_native_c_compiler()
    if compiler is None:
        raise RuntimeError("native TURBO frontend checks require a C compiler")
    BUILD.mkdir(parents=True, exist_ok=True)
    presets_start = source.index("typedef struct {", source.index("/* Speed presets shown"))
    presets = source[presets_start:source.index("/* Single authority", presets_start)]
    setter = extract(source, "void config_menu_set_vtw_speed(config_menu_t *menu,",
                     "/* Mockingboard / Phasor")
    parse = extract(source, 'strcmp(key, "vtw.speed.mode") == 0) {',
                    '} else if (strcmp(key, "vtw.slug.key")')
    parse = parse[parse.index("{") + 1:]
    serializer = extract(source, '    APPEND_CFG("mouse.slot2.enabled=',
                         "    for (uint32_t binding")
    harness = BUILD / "vtw_turbo_frontend.c"
    executable = BUILD / "vtw_turbo_frontend.exe"
    harness.write_text(textwrap.dedent(r'''
        #include <assert.h>
        #include <stdint.h>
        #include <stdio.h>
        #include <stdlib.h>
        #include <string.h>
        #include "card_control_regs.h"

        #define CONFIG_SLOT5_PROCESSOR_AD8088 1U
        typedef struct {
            uint8_t vtw_enabled, vtw_speed_mode, vtw_pace_divider;
            uint8_t vtw_ignore_c074, vtw_disable_disk2_accel;
            uint8_t vtw_slug_key_enabled;
            uint16_t vtw_slowdown_mask, vtw_slowdown_cycles;
            uint8_t mouse_slot2_enabled, mouse_sensitivity;
            uint8_t applicard_slot5_enabled, slot5_processor, applicard_resource_max;
            struct {
                void *ctx;
                void (*set_vtw_config)(void *, uint8_t, uint8_t, uint8_t,
                                       uint8_t, uint8_t);
            } platform;
        } config_menu_t;
        static char saved[1024];
        static unsigned saves, applied;
        static uint8_t applied_mode;
        static const char *config_menu_on_off(uint8_t value)
        {
            return value ? "ON" : "OFF";
        }
        static void config_menu_set_status(config_menu_t *menu, uint8_t error,
                                            const char *message)
        {
            (void)menu; (void)error; (void)message;
        }
        static void apply(void *ctx, uint8_t enabled, uint8_t mode, uint8_t divider,
                          uint8_t ignore, uint8_t disk2)
        {
            (void)ctx;
            assert(enabled == 1U && divider >= 2U);
            assert(ignore == 1U && disk2 == 1U);
            applied_mode = mode;
            ++applied;
        }
        static void config_menu_save_settings(config_menu_t *menu)
        {
            ++saves;
        #define APPEND_CFG(...) (void)snprintf(saved, sizeof(saved), __VA_ARGS__)
    ''') + serializer + textwrap.dedent(r'''
        #undef APPEND_CFG
        }
        static void parse_mode(config_menu_t *menu, const char *value)
        {
    ''') + parse + "\n}\n" + textwrap.dedent(r'''
        void config_menu_set_vtw_speed(config_menu_t *, uint8_t, uint8_t);
    ''') + presets + setter + textwrap.dedent(r'''
        int main(void)
        {
            config_menu_t menu = {0};
            config_menu_t loaded = {0};
            const char *line;
            menu.vtw_enabled = 1U;
            menu.vtw_ignore_c074 = 1U;
            menu.vtw_disable_disk2_accel = 1U;
            menu.platform.set_vtw_config = apply;
            assert(CARD_CTRL_VTW_SPEED_TURBO == 3U);
            config_menu_set_vtw_speed(&menu, CARD_CTRL_VTW_SPEED_FULL, 37U);
            assert(strcmp(config_menu_vtw_speed_label(&menu), "MAX Speed") == 0);
            config_menu_vtw_cycle_speed(&menu, 1);
            assert(menu.vtw_speed_mode == CARD_CTRL_VTW_SPEED_TURBO);
            assert(applied_mode == CARD_CTRL_VTW_SPEED_TURBO);
            assert(strcmp(config_menu_vtw_speed_label(&menu), "TURBO") == 0);
            line = strstr(saved, "vtw.speed.mode=");
            assert(line && strstr(saved, "vtw.speed.mode=3\n"));
            parse_mode(&loaded, line + strlen("vtw.speed.mode="));
            assert(loaded.vtw_speed_mode == CARD_CTRL_VTW_SPEED_TURBO);
            assert(strcmp(config_menu_vtw_speed_label(&loaded), "TURBO") == 0);
            config_menu_vtw_cycle_speed(&menu, -1);
            assert(menu.vtw_speed_mode == CARD_CTRL_VTW_SPEED_FULL);
            config_menu_vtw_cycle_speed(&menu, 1);
            config_menu_vtw_cycle_speed(&menu, 1);
            assert(menu.vtw_speed_mode == CARD_CTRL_VTW_SPEED_1MHZ);
            config_menu_vtw_cycle_speed(&menu, -1);
            assert(menu.vtw_speed_mode == CARD_CTRL_VTW_SPEED_TURBO);
            assert(saves == applied && saves == 6U);
            for (uint8_t mode = 0U; mode <= CARD_CTRL_VTW_SPEED_TURBO; ++mode) {
                char number[8];
                snprintf(number, sizeof(number), "%u", (unsigned)mode);
                parse_mode(&loaded, number);
                assert(loaded.vtw_speed_mode == mode);
                config_menu_set_vtw_speed(&menu, mode, 37U);
                assert(menu.vtw_speed_mode == mode && applied_mode == mode);
            }
            parse_mode(&loaded, "4");
            assert(loaded.vtw_speed_mode == CARD_CTRL_VTW_SPEED_FULL);
            parse_mode(&loaded, "259");
            assert(loaded.vtw_speed_mode == CARD_CTRL_VTW_SPEED_FULL);
            config_menu_set_vtw_speed(&menu, 255U, 0U);
            assert(menu.vtw_speed_mode == CARD_CTRL_VTW_SPEED_FULL);
            assert(menu.vtw_pace_divider == 2U);
            puts("VTW TURBO FRONTEND PASS");
            return 0;
        }
    '''), encoding="utf-8")
    command = [str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror",
               "-static", str(harness), "-o", str(executable), f"-I{FRONTEND}"]
    subprocess.run(command, cwd=ROOT, check=True)
    subprocess.run([str(executable)], cwd=ROOT, check=True)
    print("PASS TURBO UART and shared profile serializer checks")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
