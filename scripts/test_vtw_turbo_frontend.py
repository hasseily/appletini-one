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
    assert "vtw_service_turbo_enabled() == 0U" in speed_command
    assert "config_menu_read_settings_from_path(menu, cfg_path, 1U, NULL)" in source
    assert "config_menu_save_settings_to_path(menu, cfg_path, NULL)" in source
    for start, end in (
        ("static void config_menu_load_settings(", "static void config_menu_restore_onee_intent("),
        ("static uint8_t config_menu_read_settings_from_path(",
         "uint8_t config_menu_save_profile_settings("),
    ):
        loader = extract(source, start, end)
        assert loader.index("menu->vtw_turbo_enabled = CONFIG_DEFAULT_VTW_TURBO_ENABLED;") < loader.index("line = strtok(")
        assert loader.index("config_menu_parse_key_value(menu, key, value);") < loader.index("config_menu_coerce_vtw_speed(menu);")

    compiler = find_native_c_compiler()
    if compiler is None:
        raise RuntimeError("native TURBO frontend checks require a C compiler")
    BUILD.mkdir(parents=True, exist_ok=True)
    presets_start = source.index("typedef struct {", source.index("/* Speed presets shown"))
    presets = source[presets_start:source.index("/* Single authority", presets_start)]
    setter = extract(source, "void config_menu_set_vtw_speed(config_menu_t *menu,",
                     "/* Mockingboard / Phasor")
    coerce = extract(source, "static void config_menu_coerce_vtw_speed(",
                     "/* Config 104 exposed")
    parse = extract(source, 'strcmp(key, "vtw.speed.mode") == 0) {',
                    '} else if (strcmp(key, "vtw.slug.key")')
    parse = "    if (" + parse + "    }\n"
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
            uint8_t vtw_turbo_enabled, vtw_slug_key_enabled;
            uint16_t vtw_slowdown_mask, vtw_slowdown_cycles;
            uint8_t mouse_slot2_enabled, mouse_sensitivity;
            uint8_t applicard_slot5_enabled, slot5_processor, applicard_resource_max;
            struct {
                void *ctx;
                void (*set_vtw_turbo_enabled)(void *, uint8_t);
                void (*set_vtw_config)(void *, uint8_t, uint8_t, uint8_t,
                                       uint8_t, uint8_t);
            } platform;
        } config_menu_t;
        static char saved[1024];
        static unsigned saves, applied, gates;
        static uint8_t applied_mode, turbo_allowed;
        static const char *config_menu_on_off(uint8_t value)
        {
            return value ? "ON" : "OFF";
        }
        static uint8_t config_menu_bool_text(const char *value)
        {
            return strcmp(value, "ON") == 0;
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
        static void apply_turbo(void *ctx, uint8_t enable)
        {
            (void)ctx;
            turbo_allowed = enable;
            ++gates;
            if (enable == 0U && applied_mode == CARD_CTRL_VTW_SPEED_TURBO) {
                applied_mode = CARD_CTRL_VTW_SPEED_FULL;
            }
        }
        static void config_menu_save_settings(config_menu_t *menu)
        {
            ++saves;
        #define APPEND_CFG(...) (void)snprintf(saved, sizeof(saved), __VA_ARGS__)
    ''') + serializer + textwrap.dedent(r'''
        #undef APPEND_CFG
        }
        static void parse_setting(config_menu_t *menu, const char *key,
                                  const char *value)
        {
    ''') + parse + "\n}\n" + textwrap.dedent(r'''
        static void parse_mode(config_menu_t *menu, const char *value)
        {
            parse_setting(menu, "vtw.speed.mode", value);
        }
        void config_menu_set_vtw_speed(config_menu_t *, uint8_t, uint8_t);
    ''') + coerce + presets + setter + textwrap.dedent(r'''
        int main(void)
        {
            config_menu_t menu = {0};
            config_menu_t loaded = {0};
            const char *line;
            menu.vtw_enabled = 1U;
            menu.vtw_ignore_c074 = 1U;
            menu.vtw_disable_disk2_accel = 1U;
            menu.platform.set_vtw_config = apply;
            menu.platform.set_vtw_turbo_enabled = apply_turbo;
            assert(CARD_CTRL_VTW_SPEED_TURBO == 3U);
            config_menu_set_vtw_speed(&menu, CARD_CTRL_VTW_SPEED_FULL, 37U);
            assert(strcmp(config_menu_vtw_speed_label(&menu), "33 MHz") == 0);
            config_menu_set_vtw_speed(&menu, CARD_CTRL_VTW_SPEED_TURBO, 37U);
            assert(menu.vtw_speed_mode == CARD_CTRL_VTW_SPEED_FULL);
            assert(applied == 1U && saves == 1U);
            config_menu_vtw_cycle_speed(&menu, 1);
            assert(menu.vtw_speed_mode == CARD_CTRL_VTW_SPEED_1MHZ);
            config_menu_vtw_cycle_speed(&menu, -1);
            assert(menu.vtw_speed_mode == CARD_CTRL_VTW_SPEED_FULL);
            assert(strstr(saved, "vtw.turbo.enabled=OFF\n"));

            config_menu_vtw_set_turbo_enabled(&menu, 1U);
            assert(turbo_allowed == 1U);
            assert(menu.vtw_speed_mode == CARD_CTRL_VTW_SPEED_FULL);
            config_menu_vtw_cycle_speed(&menu, 1);
            assert(menu.vtw_speed_mode == CARD_CTRL_VTW_SPEED_TURBO);
            assert(applied_mode == CARD_CTRL_VTW_SPEED_TURBO);
            assert(strcmp(config_menu_vtw_speed_label(&menu), "TURBO") == 0);
            line = strstr(saved, "vtw.speed.mode=");
            assert(line && strstr(saved, "vtw.speed.mode=3\n"));
            assert(strstr(saved, "vtw.turbo.enabled=ON\n"));
            parse_mode(&loaded, line + strlen("vtw.speed.mode="));
            parse_setting(&loaded, "vtw.turbo.enabled", "ON");
            config_menu_coerce_vtw_speed(&loaded);
            assert(loaded.vtw_speed_mode == CARD_CTRL_VTW_SPEED_TURBO);
            assert(strcmp(config_menu_vtw_speed_label(&loaded), "TURBO") == 0);
            config_menu_vtw_cycle_speed(&menu, -1);
            assert(menu.vtw_speed_mode == CARD_CTRL_VTW_SPEED_FULL);
            config_menu_vtw_cycle_speed(&menu, 1);
            config_menu_vtw_cycle_speed(&menu, 1);
            assert(menu.vtw_speed_mode == CARD_CTRL_VTW_SPEED_1MHZ);
            config_menu_vtw_cycle_speed(&menu, -1);
            assert(menu.vtw_speed_mode == CARD_CTRL_VTW_SPEED_TURBO);
            assert(saves == applied + gates);
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

            /* Disabling drops the selection and persists both values. */
            config_menu_vtw_set_turbo_enabled(&menu, 0U);
            assert(menu.vtw_speed_mode == CARD_CTRL_VTW_SPEED_FULL);
            assert(applied_mode == CARD_CTRL_VTW_SPEED_FULL && turbo_allowed == 0U);
            assert(strstr(saved, "vtw.turbo.enabled=OFF\n"));
            assert(strstr(saved, "vtw.speed.mode=0\n"));
            config_menu_vtw_cycle_speed(&menu, 1);
            assert(menu.vtw_speed_mode == CARD_CTRL_VTW_SPEED_1MHZ);
            config_menu_vtw_cycle_speed(&menu, -1);
            assert(menu.vtw_speed_mode == CARD_CTRL_VTW_SPEED_FULL);

            /* An old config with only mode=3 requires a fresh opt-in. */
            loaded.vtw_turbo_enabled = 0U;
            parse_mode(&loaded, "3");
            assert(strcmp(config_menu_vtw_speed_label(&loaded), "33 MHz") == 0);
            config_menu_coerce_vtw_speed(&loaded);
            assert(loaded.vtw_speed_mode == CARD_CTRL_VTW_SPEED_FULL);
            /* Either key order retains TURBO when permission is present. */
            parse_setting(&loaded, "vtw.turbo.enabled", "ON");
            parse_mode(&loaded, "3");
            config_menu_coerce_vtw_speed(&loaded);
            assert(loaded.vtw_speed_mode == CARD_CTRL_VTW_SPEED_TURBO);
            parse_setting(&loaded, "vtw.turbo.enabled", "OFF");
            config_menu_coerce_vtw_speed(&loaded);
            assert(loaded.vtw_speed_mode == CARD_CTRL_VTW_SPEED_FULL);

            /* Opt-in changes alone leave ordinary speed presets intact. */
            config_menu_set_vtw_speed(&menu, CARD_CTRL_VTW_SPEED_DIVIDED, 19U);
            config_menu_vtw_set_turbo_enabled(&menu, 1U);
            config_menu_vtw_set_turbo_enabled(&menu, 0U);
            assert(menu.vtw_speed_mode == CARD_CTRL_VTW_SPEED_DIVIDED);
            assert(menu.vtw_pace_divider == 19U);
            config_menu_set_vtw_speed(&menu, 255U, 0U);
            assert(menu.vtw_speed_mode == CARD_CTRL_VTW_SPEED_FULL);
            assert(menu.vtw_pace_divider == 2U);
            assert(saves == applied + gates);
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
