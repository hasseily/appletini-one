#!/usr/bin/env python3
"""Focused source and native checks for the ONE//e vTW runtime path."""

import shutil
import subprocess
import textwrap
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
FRONTEND = REPO_ROOT / "ps_sources" / "frontend"
VTW_C = FRONTEND / "vtw_service.c"
VTW_H = FRONTEND / "vtw_service.h"
ONEE_C = FRONTEND / "onee_service.c"
ONEE_H = FRONTEND / "onee_service.h"
MAIN_C = FRONTEND / "main.c"
BUILD = REPO_ROOT / "build" / "onee_vtw_runtime_test"


class TestFailure(AssertionError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise TestFailure(message)


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def between(source: str, start_marker: str, end_marker: str) -> str:
    start = source.find(start_marker)
    end = source.find(end_marker, start + len(start_marker))
    require(start >= 0 and end > start,
            f"could not isolate {start_marker}")
    return source[start:end]


def test_real_runtime_hooks_are_bound_after_vtw_init() -> None:
    header = read(ONEE_H)
    main = read(MAIN_C)

    require("onee_service_runtime_suspend_fn" in header and
            "onee_service_runtime_running_fn" in header and
            "onee_runtime_start" in main and
            "return vtw_service_onee_start(" in main and
            "vtw_service_onee_suspend();" in main and
            "vtw_service_onee_stop();" in main and
            "return vtw_service_onee_running();" in main,
            "main must bind start, suspend, stop, and actual-running hooks")
    bind = main.find("onee_service_bind_runtime(onee_runtime_start,")
    require(bind > main.find("vtw_service_init(UART0_BASE);") and
            "onee_service_bind_runtime(NULL" not in main,
            "ONE//e hooks must be real and bound only after vTW init")


def test_cold_start_is_direct_and_isolation_first() -> None:
    source = read(VTW_C)
    start = between(source,
                    "uint8_t vtw_service_onee_start(",
                    "void vtw_service_onee_suspend(void)")

    isolate = start.find("vtw_onee_isolation_confirmed() == 0U")
    reset = start.find("vtw_onee_ctrl_value(0U, 1U)")
    cold = start.find("vtw_shadow_force_cold_start(1U)")
    rom = start.find("vtw_shadow_load_fixed_rom(0U, 1U)")
    reset_release = start.find("vtw_onee_ctrl_value(0U, 0U)")
    core_release = start.find("vtw_onee_ctrl_value(1U, 0U)")
    require(0 <= isolate < reset < cold < rom < reset_release < core_release,
            "ONE//e must confirm isolation, hold reset, cold-load, then release the core")
    require("g_state != VTW_ST_IDLE" in start and
            "CARD_CTRL_VTW_STATUS_ENABLE_EFF" in start and
            "CARD_CTRL_VTW_STATUS_CORE_RUN" in start,
            "start must exclude a host session and prove the core was released")

    forbidden = (
        "boot_menu_service_machine_mode",
        "boot_menu_service_slot7_handed_off",
        "CARD_CTRL_VTW_STATUS_BUS_OWNED",
        "VTW_ST_TAKE_BUS",
        "VTW_RES_HOLD_MS",
        "XTime_GetTime",
    )
    require(all(token not in start for token in forbidden),
            "ONE//e start must not use host identity, /DMA, physical reset timing, or handoff")


def test_rom_and_cold_signature_helpers_are_shared() -> None:
    source = read(VTW_C)
    start = between(source,
                    "uint8_t vtw_service_onee_start(",
                    "void vtw_service_onee_suspend(void)")
    host_load = between(source, "case VTW_ST_LOAD_ROM: {", "case VTW_ST_RUN:")
    rom_helper = between(source,
                         "static uint8_t vtw_shadow_load_fixed_rom",
                         "static void vtw_apply_ctrl_options_live")
    cold_helper = between(source,
                          "static uint8_t vtw_shadow_force_cold_start",
                          "static uint8_t vtw_shadow_load_fixed_rom")

    require("vtw_shadow_force_cold_start(1U)" in start and
            "vtw_shadow_load_fixed_rom(0U, 1U)" in start and
            "vtw_shadow_force_cold_start(0U)" in host_load and
            "vtw_shadow_load_fixed_rom(iiplus_patch, 0U)" in host_load,
            "stand-alone and host paths must use the same fixed-ROM helpers")
    require("apple2e_cpu_rom[i]" in rom_helper and
            "VTW_SHADOW_ROM_PHYS" in rom_helper and
            "VTW_ONEE_ROM_CHECK" not in source and
            "vtw_onee_isolation_confirmed() == 0U" in rom_helper and
            "0x003F3U" in cold_helper and
            cold_helper.count("CARD_CTRL_VTW_SHADOW_DATA_REG") == 2,
            "helpers must cold-load the Enhanced //e ROM and check isolation per byte")


def test_standalone_forces_synthetic_disk2_without_changing_options() -> None:
    source = read(VTW_C)
    ctrl = between(source,
                   "static uint32_t vtw_onee_ctrl_value",
                   "static uint8_t vtw_onee_isolation_confirmed")
    start = between(source,
                    "uint8_t vtw_service_onee_start(",
                    "void vtw_service_onee_suspend(void)")

    require("CARD_CTRL_VTW_CTRL_DISABLE_D2_ACCEL_BIT" in ctrl and
            "vtw_eff_mode()" in ctrl and "vtw_eff_divider()" in ctrl,
            "ONE//e must force the Disk II shortcut off while retaining speed options")
    shutdown = between(source,
                       "static void vtw_onee_shutdown",
                       "uint8_t vtw_service_onee_start(")
    suspend = between(source,
                      "void vtw_service_onee_suspend(void)",
                      "void vtw_service_onee_stop(void)")
    stop = between(source,
                   "void vtw_service_onee_stop(void)",
                   "uint8_t vtw_service_onee_running(void)")
    disk2_config = between(source,
                           "void vtw_service_set_disk2_config_enabled",
                           "void vtw_service_set_enabled")
    main = read(MAIN_C)
    require("g_disk2_config_enabled" in start and
            "disk2_service_set_enabled(1U);" in start and
            start.find("disk2_service_set_enabled(1U);") <
            start.find("vtw_shadow_force_cold_start(1U)") and
            "disk2_service_set_enabled(g_disk2_config_enabled);" in shutdown and
            "g_onee_disk2_override_active != 0U" in disk2_config and
            "vtw_service_set_disk2_config_enabled(enable);" in main and
            "g_card_slot_enable_mask" in main and
            "CARD_CTRL_SLOT_DISK2" in main,
            "ONE//e must keep one effective Disk II service owner")
    require(start.count("vtw_service_onee_suspend();") == 3 and
            "vtw_onee_shutdown(0U);" in suspend and
            "vtw_onee_shutdown(1U);" in stop and
            shutdown.find("REG_WRITE(CARD_CTRL_VTW_CTRL_REG, 0U);") <
            shutdown.find("disk2_service_set_enabled(g_disk2_config_enabled);"),
            "failed starts must suspend while terminal stop clears the session")
    require("card_control_write_slot_mask" not in start and
            "control_set_slot_enabled" not in start and
            "config_menu_save_settings" not in start,
            "the Disk II session override must not mutate or save the slot mask")
    mutations = (
        "g_intent_enabled =",
        "g_speed_mode =",
        "g_pace_divider =",
        "g_ignore_c074 =",
        "g_disable_disk2_accel =",
    )
    require(all(token not in start for token in mutations),
            "ONE//e must not overwrite the saved host-vTW intent or options")


def test_onee_reset_uses_private_runtime_paths() -> None:
    main = read(MAIN_C)
    reset = between(main,
                    "static void ui_handle_apple_reset",
                    "static ui_input_t ui_make_input")
    uart = read(FRONTEND / "uart_control.c")

    require("config_menu_apply_boot_runtime(menu);" in reset and
            "if ((onee_service_status() &\n"
            "         CARD_CTRL_ONEE_STATUS_EFFECTIVE_BIT) == 0U) {\n"
            "        (void)uart_control_dma_bus_write(0xC029U, 0x01U);\n"
            "    }" in reset,
            "ONE//e reset must reapply config without issuing host IIgs DMA")
    require("ONE//e remains running" in uart and
            "vtw_service_onee_running() != 0U" in uart,
            "UART host-intent commands must report a live ONE//e session truthfully")
    require('"vtw: %s, session=%s, host-intent=%s, machine=%s' in read(VTW_C),
            "vtw status must separate the live session from saved host intent")


def test_stop_order_and_runtime_drop_preserves_request() -> None:
    vtw = read(VTW_C)
    onee = read(ONEE_C)
    shutdown = between(vtw,
                       "static void vtw_onee_shutdown",
                       "uint8_t vtw_service_onee_start(")
    suspend = between(vtw,
                      "void vtw_service_onee_suspend(void)",
                      "void vtw_service_onee_stop(void)")
    stop = between(vtw,
                   "void vtw_service_onee_stop(void)",
                   "uint8_t vtw_service_onee_running(void)")
    vtw_poll = between(vtw,
                       "void vtw_service_poll(void)",
                       "void vtw_service_uart_status")
    disarm = between(onee,
                     "static void onee_service_disarm",
                     "void onee_service_init(void)")
    poll = between(onee,
                   "void onee_service_poll(void)",
                   "onee_service_state_t onee_service_state(void)")

    require("REG_WRITE(CARD_CTRL_VTW_CTRL_REG, 0U);" in shutdown and
            "clear_speed_override != 0U" in shutdown and
            "vtw_override_clear();" in shutdown and
            "vtw_onee_shutdown(0U);" in suspend and
            "vtw_onee_shutdown(1U);" in stop,
            "stand-alone stop must clear VTW CTRL with a literal zero")
    require("vtw_service_onee_suspend();" in vtw_poll,
            "a dropped private core must preserve its speed for service retry")
    require(disarm.find("onee_service_stop_runtime();") <
            disarm.find("onee_service_write_request(0U);"),
            "a safety disarm must stop vTW before clearing the ONE//e request")
    require("g_runtime_running(g_runtime_ctx) == 0U" in poll and
            "onee_service_suspend_runtime();" in poll and
            "g_runtime_retry_polls = 0U;" in poll and
            "ONEE_EFFECTIVE_WAIT_POLL_LIMIT" not in onee and
            "g_effective_wait_polls" not in onee,
            "a private runtime drop must retain the selected PL request and retry")
    require(onee.count("onee_service_write_request(1U);") == 2 and
            "g_restore_pending != 0U" in poll and
            "onee_status_can_start(g_status) == 0U" in poll and
            "(g_status & CARD_CTRL_ONEE_STATUS_REQUEST_BIT) == 0U" in poll and
            "onee_service_force_persisted(0U);" in poll and
            "onee_service_disarm(1U);" in poll,
            "a lost request must save OFF; only manual or guarded reboot restore may write high")


def test_running_state_requires_released_core_status() -> None:
    source = read(ONEE_C)
    state = between(source,
                    "onee_service_state_t onee_service_state(void)",
                    "uint32_t onee_service_status(void)")
    poll = between(source,
                   "void onee_service_poll(void)",
                   "onee_service_state_t onee_service_state(void)")

    require("g_runtime_running(g_runtime_ctx) != 0U" in state and
            "g_runtime_running(g_runtime_ctx) == 0U" in poll,
            "UI RUNNING must track the released soft core, not PL effective alone")


def test_menu_closes_once_only_after_true_running() -> None:
    source = read(MAIN_C)
    close = between(source,
                    "static void ui_close_menu_on_onee_running",
                    "static void ui_save_screenshot")
    loop_start = source.rfind("while (1) {")
    require(loop_start >= 0, "could not isolate the frontend main loop")
    loop = source[loop_start:]

    require("CONFIG_MENU_ONEE_MODE_RUNNING" in close and
            "g_onee_ui_running_seen == 0U" in close and
            "ui_set_boot_menu_visible(s, menu, 0U);" in close and
            "ui_sync_usb_menu_capture(menu);" in close and
            "g_onee_ui_running_seen = running;" in close,
            "the menu must close once on the true RUNNING transition")
    require("onee_service_request_start" not in close and
            "onee_service_request_stop" not in close and
            "START REQUESTED" not in close and "LOCKED" not in close,
            "request and refusal states must not close the menu")
    poll_pos = loop.find("config_menu_poll_onee_mode(&config_menu);")
    close_pos = loop.find("ui_close_menu_on_onee_running(&ui, &config_menu);")
    require(0 <= poll_pos < close_pos,
            "the close check must use the freshly polled core-running state")
    require("case USB_HID_MENU_ACTION_OPEN:" in source and
            "ui_set_boot_menu_visible(s, menu, 1U);" in source,
            "USB menu reopen must remain available so RUNNING can be stopped")


def test_normal_host_state_machine_remains_after_onee_gate() -> None:
    source = read(VTW_C)
    poll = between(source,
                   "void vtw_service_poll(void)",
                   "void vtw_service_uart_status")

    require(poll.find("vtw_onee_control_active()") < poll.find("switch (g_state)") and
            "g_intent_enabled == 0U" in poll and
            "boot_menu_service_machine_mode()" in poll and
            "boot_menu_service_slot7_handed_off()" in poll and
            "CARD_CTRL_VTW_STATUS_BUS_OWNED" in poll,
            "the saved host intent must resume through the unchanged host takeover path")


def test_live_speed_controls_share_one_verified_writer() -> None:
    source = read(VTW_C)
    main = read(MAIN_C)
    writer = between(source,
                     "static vtw_ctrl_live_result_t vtw_apply_ctrl_live",
                     "static uint8_t vtw_onee_isolation_confirmed")
    toggle = between(source,
                     "void vtw_service_speed_toggle(uint8_t allow_onee_preselect)",
                     "void vtw_service_speed_step(int8_t dir, uint8_t allow_onee_preselect)")
    step = between(source,
                   "void vtw_service_speed_step(int8_t dir, uint8_t allow_onee_preselect)",
                   "void vtw_service_slug_toggle(uint8_t allow_onee_preselect)")
    slug = between(source,
                   "void vtw_service_slug_toggle(uint8_t allow_onee_preselect)",
                   "void vtw_service_set_slowdown")
    configured = between(source,
                         "void vtw_service_set_speed(",
                         "void vtw_service_set_ignore_c074")
    options = between(source,
                      "static void vtw_apply_ctrl_options_live",
                      "static uint8_t vtw_ms_elapsed")

    require("g_onee_running != 0U" in writer and
            "vtw_onee_ctrl_value(1U, 0U)" in writer and
            "g_state == VTW_ST_RUN" in writer and
            "vtw_ctrl_value(1U, 1U, 0U)" in writer and
            "REG_WRITE(CARD_CTRL_VTW_CTRL_REG, desired);" in writer and
            "REG_READ(CARD_CTRL_VTW_CTRL_REG) != desired" in writer,
            "host and ONE//e live changes must share a verified CTRL writer")
    require("vtw_apply_ctrl_live()" in configured and
            "vtw_apply_ctrl_live()" in options and
            "vtw_apply_ctrl_live()" in between(
                source, "static uint8_t vtw_override_apply", "/* Nearest"),
            "menu, compatibility options, and USB overrides must use that writer")
    gate = between(source,
                   "static uint8_t vtw_speed_request_allowed",
                   "void vtw_service_set_slug_enabled")
    require(all("vtw_speed_request_allowed(allow_onee_preselect) == 0U" in action
                for action in (toggle, step, slug)) and
            "allow_onee_preselect != 0U" in gate and
            "g_intent_enabled != 0U" in gate and
            "g_state != VTW_ST_IDLE" in gate and
            "g_onee_running != 0U" in gate and
            "vtw_onee_control_active() != 0U" in gate and
            '"TW NEXT: %s"' in source and
            "VTW_CTRL_LIVE_NONE" in between(
                source, "static uint8_t vtw_override_apply", "/* Nearest"),
            "requested sessions must queue a pre-takeover speed while true off stays off")
    usb_handler = between(main,
                          "static void ui_handle_usb_menu_event",
                          "static void ui_set_bezel")
    require("allow_onee_preselect =\n"
            "        (uint8_t)(config_menu_is_active(menu) != 0U);" in usb_handler and
            "vtw_service_speed_toggle(allow_onee_preselect);" in usb_handler and
            usb_handler.count(
                "vtw_service_speed_step(") == 2 and
            usb_handler.count(
                ", allow_onee_preselect);") == 2 and
            "vtw_service_slug_toggle(allow_onee_preselect);" in usb_handler,
            "USB speed actions must pass the current menu context explicitly")
    require("TW: CONTROL WRITE FAILED" in source and
            all("vtw_override_apply(" in action
                for action in (toggle, step, slug)),
            "live USB labels must report success only after a verified write")


def test_onee_menu_pause_preserves_the_live_machine() -> None:
    source = read(VTW_C)
    header = read(VTW_H)
    regs = read(FRONTEND / "card_control_regs.h")
    core = read(REPO_ROOT / "hdl" / "apple" / "vtw_core_top.sv")
    top = read(REPO_ROOT / "hdl" / "apple" / "apple_top.sv")
    main = read(MAIN_C)
    setter = between(source,
                     "uint8_t vtw_service_onee_set_paused(uint8_t paused)",
                     "uint8_t vtw_service_onee_paused(void)")
    onee_ctrl = between(source,
                        "static uint32_t vtw_onee_ctrl_value",
                        "typedef enum")
    main_pause = between(main,
                         "static void ui_sync_onee_menu_pause",
                         "static void ui_set_boot_menu_visible")

    require("CARD_CTRL_VTW_CTRL_PAUSE_BIT       (1UL << 8)" in regs and
            "input  logic                    pause" in core and
            "assign core_en = core_res_n && !pause && !rw_hold_q" in core and
            ".pause(vtw_ctrl_q[8])" in top,
            "VTW_CTRL bit 8 must gate only the 65C02 completion edge")
    require("CARD_CTRL_VTW_CTRL_PAUSE_BIT" in onee_ctrl and
            "g_onee_pause_requested" in onee_ctrl and
            "vtw_apply_ctrl_live()" in setter and
            "CARD_CTRL_VTW_STATUS_ENABLE_EFF" in setter and
            "CARD_CTRL_VTW_STATUS_CORE_RUN" in setter,
            "pause must preserve the live ONE//e control word and run status")
    start = between(source,
                    "uint8_t vtw_service_onee_start(",
                    "uint8_t vtw_service_onee_set_paused(uint8_t paused)")
    not_running = setter[setter.find("if (g_onee_running == 0U)"):
                         setter.find("if (vtw_onee_isolation_confirmed()")]
    require("g_onee_pause_requested = paused;" in not_running and
            "g_onee_pause_requested = 0U;" not in start,
            "a menu-open pause request must survive a delayed ONE//e start")
    require("vtw_service_onee_set_paused(uint8_t paused)" in header and
            "vtw_service_onee_paused(void)" in header and
            "usb_hid_service_all_input_released()" in main_pause and
            main_pause.find("usb_hid_service_all_input_released()") <
            main_pause.find("vtw_service_onee_set_paused(0U)",
                            main_pause.find("usb_hid_service_all_input_released()")),
            "menu close must wait for USB key release before resuming")


TESTS = [
    test_real_runtime_hooks_are_bound_after_vtw_init,
    test_cold_start_is_direct_and_isolation_first,
    test_rom_and_cold_signature_helpers_are_shared,
    test_standalone_forces_synthetic_disk2_without_changing_options,
    test_onee_reset_uses_private_runtime_paths,
    test_stop_order_and_runtime_drop_preserves_request,
    test_running_state_requires_released_core_status,
    test_menu_closes_once_only_after_true_running,
    test_normal_host_state_machine_remains_after_onee_gate,
    test_live_speed_controls_share_one_verified_writer,
    test_onee_menu_pause_preserves_the_live_machine,
]


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


def run_native_speed_control_test() -> bool:
    compiler = find_native_c_compiler()
    if compiler is None:
        print("SKIP native_speed_control_test: no host C compiler")
        return True

    if BUILD.exists():
        shutil.rmtree(BUILD)
    BUILD.mkdir(parents=True)
    harness = BUILD / "onee_vtw_runtime_harness.c"
    executable = BUILD / "onee_vtw_runtime_harness.exe"
    (BUILD / "xiltimer.h").write_text(textwrap.dedent(r'''
        #ifndef XILTIMER_H
        #define XILTIMER_H
        #include <stdint.h>
        typedef uint64_t XTime;
        #define COUNTS_PER_SECOND 100000000ULL
        void XTime_GetTime(XTime *value);
        #endif
    '''), encoding="utf-8")
    harness.write_text(textwrap.dedent(r'''
        #include <stdint.h>
        #include <stdio.h>
        #include <string.h>

        static uint32_t test_reg_read(uint32_t address);
        static void test_reg_write(uint32_t address, uint32_t value);
        uint8_t boot_menu_service_machine_mode(void);
        const char *boot_menu_service_machine_name(void);
        uint8_t boot_menu_service_slot7_handed_off(void);
        void disk2_service_set_enabled(uint8_t enabled);
        void uart_puts(uint32_t base, const char *s);

        #define BOOT_MENU_SERVICE_H
        #define DISK2_SERVICE_H
        #define COMMON_H
        #define UART_H
        #define REG_READ(address) test_reg_read((uint32_t)(address))
        #define REG_WRITE(address, value) \
            test_reg_write((uint32_t)(address), (uint32_t)(value))
        #include "../../ps_sources/frontend/vtw_service.c"

        const uint8_t apple2e_cpu_rom[16384] = { 0U };

        typedef struct {
            uint32_t address;
            uint32_t value;
        } write_event_t;

        static uint32_t registers[256];
        static write_event_t writes[32768];
        static uint32_t write_count;
        static uint32_t ctrl_read_count;
        static uint8_t ctrl_write_sticks;
        static uint32_t first_core_run_ctrl;
        static uint8_t machine_mode;
        static uint8_t disk2_enabled;
        static XTime fake_time;

        static uint32_t reg_index(uint32_t address)
        {
            return (address - APPLE_DEBUG_BASE) / 4U;
        }

        static uint32_t test_reg_read(uint32_t address)
        {
            if (address == CARD_CTRL_VTW_CTRL_REG) {
                ++ctrl_read_count;
            }
            return registers[reg_index(address)];
        }

        static void test_reg_write(uint32_t address, uint32_t value)
        {
            if (write_count < (sizeof(writes) / sizeof(writes[0]))) {
                writes[write_count].address = address;
                writes[write_count].value = value;
            }
            ++write_count;
            if (address != CARD_CTRL_VTW_CTRL_REG || ctrl_write_sticks != 0U) {
                registers[reg_index(address)] = value;
            }
            if (address == CARD_CTRL_VTW_CTRL_REG && ctrl_write_sticks != 0U) {
                uint32_t status = registers[reg_index(CARD_CTRL_VTW_STATUS_REG)] &
                    ~(CARD_CTRL_VTW_STATUS_ENABLE_EFF |
                      CARD_CTRL_VTW_STATUS_CORE_RUN);

                if ((value & CARD_CTRL_VTW_CTRL_ENABLE_BIT) != 0U) {
                    status |= CARD_CTRL_VTW_STATUS_ENABLE_EFF;
                }
                if ((value & CARD_CTRL_VTW_CTRL_CORE_RUN_BIT) != 0U) {
                    status |= CARD_CTRL_VTW_STATUS_CORE_RUN;
                    if (first_core_run_ctrl == 0U) {
                        first_core_run_ctrl = value;
                    }
                }
                registers[reg_index(CARD_CTRL_VTW_STATUS_REG)] = status;
            }
        }

        uint8_t boot_menu_service_machine_mode(void)
        {
            return machine_mode;
        }

        const char *boot_menu_service_machine_name(void)
        {
            return "test";
        }

        uint8_t boot_menu_service_slot7_handed_off(void)
        {
            return 1U;
        }

        void disk2_service_set_enabled(uint8_t enabled)
        {
            disk2_enabled = enabled;
        }

        void uart_puts(uint32_t base, const char *s)
        {
            (void)base;
            (void)s;
        }

        void XTime_GetTime(XTime *value)
        {
            *value = fake_time;
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

        static uint32_t ctrl_value(void)
        {
            return registers[reg_index(CARD_CTRL_VTW_CTRL_REG)];
        }

        static uint32_t ctrl_speed(void)
        {
            return (ctrl_value() >> CARD_CTRL_VTW_CTRL_SPEED_SHIFT) &
                   CARD_CTRL_VTW_CTRL_SPEED_MASK;
        }

        static uint32_t ctrl_divider(void)
        {
            return (ctrl_value() >> CARD_CTRL_VTW_CTRL_DIVIDER_SHIFT) &
                   CARD_CTRL_VTW_CTRL_DIVIDER_MASK;
        }

        static int check(uint8_t condition, const char *message)
        {
            if (condition == 0U) {
                fprintf(stderr, "FAIL: %s\n", message);
                return 0;
            }
            return 1;
        }

        static int check_start_ctrl_words(uint32_t first_write,
                                          uint32_t mode,
                                          uint32_t divider,
                                          const char *message)
        {
            uint32_t ctrl_count = 0U;

            for (uint32_t i = first_write; i < write_count; ++i) {
                uint32_t value;

                if (writes[i].address != CARD_CTRL_VTW_CTRL_REG) {
                    continue;
                }
                value = writes[i].value;
                if (((value >> CARD_CTRL_VTW_CTRL_SPEED_SHIFT) &
                     CARD_CTRL_VTW_CTRL_SPEED_MASK) != mode ||
                    ((value >> CARD_CTRL_VTW_CTRL_DIVIDER_SHIFT) &
                     CARD_CTRL_VTW_CTRL_DIVIDER_MASK) != divider) {
                    return check(0U, message);
                }
                ++ctrl_count;
            }
            return check(ctrl_count == 3U, message);
        }

        static void reset_fixture(void)
        {
            memset(registers, 0, sizeof(registers));
            memset(writes, 0, sizeof(writes));
            write_count = 0U;
            ctrl_read_count = 0U;
            ctrl_write_sticks = 1U;
            first_core_run_ctrl = 0U;
            machine_mode = CARD_MACHINE_MODE_IIE;
            disk2_enabled = 0U;
            fake_time = 0U;
            g_uart_base = 0U;
            g_intent_enabled = 0U;
            g_speed_mode = CARD_CTRL_VTW_SPEED_FULL;
            g_pace_divider = 37U;
            g_ovr_active = 0U;
            g_ovr_mode = 0U;
            g_ovr_div = 0U;
            g_slug_enabled = 0U;
            g_ignore_c074 = 0U;
            g_disable_disk2_accel = 0U;
            memset(g_last_action_text, 0, sizeof(g_last_action_text));
            g_slowdown_mask = 0U;
            g_slowdown_cycles = 0U;
            g_state = VTW_ST_IDLE;
            g_take_polls = 0U;
            g_sessions_started = 0U;
            g_announced_wait = 0U;
            g_announced_handoff_wait = 0U;
            g_onee_running = 0U;
            g_onee_disk2_override_active = 0U;
            g_disk2_config_enabled = 0U;
            g_res_phase_start = 0U;
            vtw_service_init(0U);
            write_count = 0U;
            ctrl_read_count = 0U;
        }

        static void set_onee_isolated(void)
        {
            registers[reg_index(CARD_CTRL_ONEE_MODE_REG)] =
                CARD_CTRL_ONEE_STATUS_REQUEST_BIT |
                CARD_CTRL_ONEE_STATUS_EFFECTIVE_BIT |
                CARD_CTRL_ONEE_STATUS_ISOLATED_BIT |
                CARD_CTRL_ONEE_STATUS_SELECTED_BIT |
                CARD_CTRL_ONEE_STATUS_HDL_PRESENT_BIT |
                (CARD_CTRL_ONEE_STATUS_SIGNATURE <<
                 CARD_CTRL_ONEE_STATUS_SIGNATURE_SHIFT);
        }

        static int test_host_live_controls(void)
        {
            uint32_t before;

            reset_fixture();
            machine_mode = CARD_MACHINE_MODE_IIPLUS;
            g_intent_enabled = 1U;
            g_state = VTW_ST_RUN;

            vtw_service_set_speed(CARD_CTRL_VTW_SPEED_DIVIDED, 37U);
            if (!check(writes_to(CARD_CTRL_VTW_CTRL_REG) == 1U &&
                       ctrl_read_count == 1U &&
                       (ctrl_value() & (CARD_CTRL_VTW_CTRL_ENABLE_BIT |
                                        CARD_CTRL_VTW_CTRL_CORE_RUN_BIT |
                                        CARD_CTRL_VTW_CTRL_IIPLUS_BTNS_BIT)) ==
                                       (CARD_CTRL_VTW_CTRL_ENABLE_BIT |
                                        CARD_CTRL_VTW_CTRL_CORE_RUN_BIT |
                                        CARD_CTRL_VTW_CTRL_IIPLUS_BTNS_BIT) &&
                       ctrl_speed() == CARD_CTRL_VTW_SPEED_DIVIDED &&
                       ctrl_divider() == 37U,
                       "host menu speed did not use verified live CTRL")) {
                return 0;
            }

            before = write_count;
            vtw_service_speed_toggle(0U);
            if (!check(write_count == before + 1U &&
                       ctrl_speed() == CARD_CTRL_VTW_SPEED_1MHZ &&
                       strcmp(vtw_service_last_action_text(),
                              "TW: 1 MHz default") == 0,
                       "host USB toggle did not change the running core")) {
                return 0;
            }

            before = write_count;
            vtw_service_set_ignore_c074(1U);
            if (!check(write_count == before + 1U &&
                       (ctrl_value() & CARD_CTRL_VTW_CTRL_IGNORE_C074_BIT) != 0U,
                       "host option did not use the live writer")) {
                return 0;
            }
            return 1;
        }

        static int test_onee_live_controls_without_host_intent(void)
        {
            uint32_t before;
            uint32_t before_reads;
            const uint32_t onee_run =
                CARD_CTRL_VTW_CTRL_ENABLE_BIT |
                CARD_CTRL_VTW_CTRL_CORE_RUN_BIT |
                CARD_CTRL_VTW_CTRL_DISABLE_D2_ACCEL_BIT;

            reset_fixture();
            vtw_service_set_speed(CARD_CTRL_VTW_SPEED_FULL, 37U);
            g_onee_running = 1U;
            g_intent_enabled = 0U;

            vtw_service_speed_toggle(0U);
            if (!check(write_count == 1U && ctrl_read_count == 1U &&
                       ctrl_value() ==
                           (onee_run |
                            (CARD_CTRL_VTW_SPEED_1MHZ <<
                             CARD_CTRL_VTW_CTRL_SPEED_SHIFT)) &&
                       strcmp(vtw_service_last_action_text(),
                              "TW: 1 MHz default") == 0,
                       "ONE//e toggle did not write and read back its exact CTRL word")) {
                return 0;
            }

            before_reads = ctrl_read_count;
            vtw_service_speed_step(1, 0U);
            if (!check(ctrl_read_count == before_reads + 1U &&
                       ctrl_value() ==
                           (onee_run |
                            (CARD_CTRL_VTW_SPEED_DIVIDED <<
                             CARD_CTRL_VTW_CTRL_SPEED_SHIFT) |
                            (51UL << CARD_CTRL_VTW_CTRL_DIVIDER_SHIFT)),
                       "ONE//e speed step lost its exact live or Disk II state")) {
                return 0;
            }

            vtw_service_set_slug_enabled(1U);
            vtw_service_slug_toggle(0U);
            if (!check(ctrl_speed() == CARD_CTRL_VTW_SPEED_DIVIDED &&
                       ctrl_divider() == VTW_SLUG_DIVIDER,
                       "ONE//e slug key did not reach CTRL")) {
                return 0;
            }
            before = write_count;
            before_reads = ctrl_read_count;
            vtw_service_set_slug_enabled(0U);
            if (!check(write_count == before + 1U &&
                       ctrl_read_count == before_reads + 1U &&
                       ctrl_value() ==
                           (onee_run |
                            (37UL << CARD_CTRL_VTW_CTRL_DIVIDER_SHIFT)),
                       "ONE//e slug disable did not restore through the live writer")) {
                return 0;
            }

            vtw_service_set_disk2_accel_disabled(0U);
            if (!check((ctrl_value() &
                        CARD_CTRL_VTW_CTRL_DISABLE_D2_ACCEL_BIT) != 0U,
                       "ONE//e live options cleared its forced Disk II bit")) {
                return 0;
            }
            return 1;
        }

        static int test_failed_live_writes_and_pending_choice(void)
        {
            uint32_t before;

            reset_fixture();
            g_onee_running = 1U;
            (void)vtw_apply_ctrl_live();
            before = ctrl_value();
            ctrl_write_sticks = 0U;
            vtw_service_speed_toggle(0U);
            if (!check(ctrl_value() == before &&
                       g_ovr_active == 0U && g_ovr_mode == 0U &&
                       g_ovr_div == 0U && ctrl_read_count == 2U &&
                       strcmp(vtw_service_last_action_text(),
                              "TW: CONTROL WRITE FAILED") == 0,
                       "failed CTRL readback claimed a speed change")) {
                return 0;
            }

            ctrl_write_sticks = 1U;
            g_onee_running = 0U;
            g_intent_enabled = 0U;
            g_state = VTW_ST_IDLE;
            before = write_count;
            vtw_service_speed_toggle(0U);
            if (!check(write_count == before && g_ovr_active == 0U &&
                       strcmp(vtw_service_last_action_text(), "TW: OFF") == 0,
                       "truly inactive speed choice did not stay off")) {
                return 0;
            }

            g_intent_enabled = 1U;
            vtw_service_speed_toggle(0U);
            if (!check(write_count == before &&
                       g_ovr_active != 0U &&
                       g_ovr_mode == CARD_CTRL_VTW_SPEED_1MHZ &&
                       strcmp(vtw_service_last_action_text(),
                              "TW NEXT: 1 MHz default") == 0,
                       "idle speed choice was not queued for takeover")) {
                return 0;
            }
            return 1;
        }

        static int test_menu_preselect_context_boundary(void)
        {
            uint32_t before;
            uint32_t start_write;

            /* Saved host vTW stays off, but a mapped speed key used in the
             * open Appletini menu must stage the next ONE//e session. */
            reset_fixture();
            vtw_service_set_speed(CARD_CTRL_VTW_SPEED_DIVIDED, 37U);
            before = write_count;
            vtw_service_speed_step(1, 1U);
            if (!check(g_intent_enabled == 0U && write_count == before &&
                       g_ovr_active != 0U &&
                       g_ovr_mode == CARD_CTRL_VTW_SPEED_DIVIDED &&
                       g_ovr_div == 19U &&
                       strcmp(vtw_service_last_action_text(),
                              "TW NEXT: 7 MHz") == 0,
                       "open-menu speed key did not queue ONE//e preselection")) {
                return 0;
            }
            set_onee_isolated();
            start_write = write_count;
            if (!check(vtw_service_onee_start(0U) != 0U &&
                       ctrl_speed() == CARD_CTRL_VTW_SPEED_DIVIDED &&
                       ctrl_divider() == 19U,
                       "menu preselection did not reach ONE//e CTRL") ||
                !check_start_ctrl_words(
                    start_write,
                    CARD_CTRL_VTW_SPEED_DIVIDED,
                    19U,
                    "menu preselection changed during ONE//e start")) {
                return 0;
            }
            vtw_service_onee_stop();

            /* The same key outside the menu must not turn a truly disabled
             * host-vTW setup into a pending session. */
            reset_fixture();
            vtw_service_set_speed(CARD_CTRL_VTW_SPEED_DIVIDED, 37U);
            before = write_count;
            vtw_service_speed_step(1, 0U);
            if (!check(g_intent_enabled == 0U && write_count == before &&
                       g_ovr_active == 0U &&
                       strcmp(vtw_service_last_action_text(), "TW: OFF") == 0,
                       "closed-menu speed key queued while vTW was off")) {
                return 0;
            }
            return 1;
        }

        static int test_onee_configured_and_pending_speed_boundaries(void)
        {
            uint32_t run_ctrl;
            uint32_t before;
            uint32_t start_write;

            /* A configured divided speed must be in the first control word
             * that releases the ONE//e core. */
            reset_fixture();
            vtw_service_set_speed(CARD_CTRL_VTW_SPEED_DIVIDED, 37U);
            set_onee_isolated();
            start_write = write_count;
            if (!check(vtw_service_onee_start(0U) != 0U &&
                       first_core_run_ctrl != 0U &&
                       ((first_core_run_ctrl >> CARD_CTRL_VTW_CTRL_SPEED_SHIFT) &
                        CARD_CTRL_VTW_CTRL_SPEED_MASK) ==
                           CARD_CTRL_VTW_SPEED_DIVIDED &&
                       ((first_core_run_ctrl >> CARD_CTRL_VTW_CTRL_DIVIDER_SHIFT) &
                        CARD_CTRL_VTW_CTRL_DIVIDER_MASK) == 37U &&
                       ctrl_value() == first_core_run_ctrl,
                       "configured speed was absent from first ONE//e release")) {
                return 0;
            }
            if (!check_start_ctrl_words(start_write,
                                        CARD_CTRL_VTW_SPEED_DIVIDED,
                                        37U,
                                        "configured speed changed during ONE//e start")) {
                return 0;
            }
            vtw_service_onee_stop();

            /* A key used after the ONE//e request becomes active but before
             * core release must stage the first session speed. */
            reset_fixture();
            vtw_service_set_speed(CARD_CTRL_VTW_SPEED_DIVIDED, 37U);
            set_onee_isolated();
            vtw_service_speed_step(1, 0U);
            if (!check(g_ovr_active != 0U &&
                       g_ovr_mode == CARD_CTRL_VTW_SPEED_DIVIDED &&
                       g_ovr_div == 19U &&
                       strcmp(vtw_service_last_action_text(),
                              "TW NEXT: 7 MHz") == 0,
                       "ONE//e boot-window speed was not queued")) {
                return 0;
            }
            start_write = write_count;
            first_core_run_ctrl = 0U;
            if (!check(vtw_service_onee_start(0U) != 0U,
                       "ONE//e queued-speed start failed") ||
                !check_start_ctrl_words(start_write,
                                        CARD_CTRL_VTW_SPEED_DIVIDED,
                                        19U,
                                        "queued speed changed during ONE//e start")) {
                return 0;
            }

            /* A runtime rung is session state. The PS service must not
             * replace it during its normal polls; the RTL regression drives
             * the private RESET line and proves the same word survives it. */
            run_ctrl = ctrl_value();
            before = write_count;
            vtw_service_poll();
            vtw_service_poll();
            if (!check(write_count == before && ctrl_value() == run_ctrl &&
                       ctrl_speed() == CARD_CTRL_VTW_SPEED_DIVIDED &&
                       ctrl_divider() == 19U && g_ovr_active != 0U,
                       "ONE//e warm-reset polls changed the runtime rung")) {
                return 0;
            }

            /* A recoverable runtime stop must leave the exact queued/live
             * rung intact and use it in every word of the retry start. */
            before = write_count;
            vtw_service_onee_suspend();
            if (!check(write_count == before + 1U && ctrl_value() == 0U &&
                       g_onee_running == 0U && g_ovr_active != 0U &&
                       g_ovr_mode == CARD_CTRL_VTW_SPEED_DIVIDED &&
                       g_ovr_div == 19U,
                       "recoverable ONE//e suspend lost the exact override")) {
                return 0;
            }
            memset(writes, 0, sizeof(writes));
            write_count = 0U;
            ctrl_read_count = 0U;
            set_onee_isolated();
            start_write = write_count;
            first_core_run_ctrl = 0U;
            if (!check(vtw_service_onee_start(0U) != 0U &&
                       ctrl_speed() == CARD_CTRL_VTW_SPEED_DIVIDED &&
                       ctrl_divider() == 19U && g_ovr_active != 0U,
                       "ONE//e retry did not keep its exact override") ||
                !check_start_ctrl_words(start_write,
                                        CARD_CTRL_VTW_SPEED_DIVIDED,
                                        19U,
                                        "ONE//e retry changed its speed words")) {
                return 0;
            }

            /* A terminal session stop clears the non-persistent override. A
             * new manual session returns to the configured 3.6 MHz baseline. */
            vtw_service_onee_stop();
            memset(writes, 0, sizeof(writes));
            write_count = 0U;
            ctrl_read_count = 0U;
            first_core_run_ctrl = 0U;
            set_onee_isolated();
            start_write = write_count;
            if (!check(g_ovr_active == 0U &&
                       vtw_service_onee_start(0U) != 0U &&
                       first_core_run_ctrl != 0U &&
                       ctrl_speed() == CARD_CTRL_VTW_SPEED_DIVIDED &&
                       ctrl_divider() == 37U,
                       "new ONE//e session did not return to configured speed")) {
                return 0;
            }
            if (!check_start_ctrl_words(start_write,
                                        CARD_CTRL_VTW_SPEED_DIVIDED,
                                        37U,
                                        "new session start lost configured speed")) {
                return 0;
            }
            return 1;
        }

        static int test_disk2_session_override_tracks_latest_config(void)
        {
            reset_fixture();
            vtw_service_set_disk2_config_enabled(0U);
            if (!check(disk2_enabled == 0U,
                       "saved Disk II off did not reach an idle service")) {
                return 0;
            }

            g_onee_disk2_override_active = 1U;
            disk2_service_set_enabled(1U);
            vtw_service_set_disk2_config_enabled(0U);
            if (!check(disk2_enabled == 1U && g_disk2_config_enabled == 0U,
                       "reset/config reapply disabled Disk II during ONE//e")) {
                return 0;
            }
            vtw_service_set_disk2_config_enabled(1U);
            if (!check(disk2_enabled == 1U && g_disk2_config_enabled == 1U,
                       "live saved-state change defeated the ONE//e override")) {
                return 0;
            }

            g_onee_running = 1U;
            vtw_service_onee_stop();
            if (!check(disk2_enabled == 1U &&
                       g_onee_disk2_override_active == 0U,
                       "ONE//e stop did not apply the latest saved on state")) {
                return 0;
            }

            g_onee_disk2_override_active = 1U;
            disk2_service_set_enabled(1U);
            vtw_service_set_disk2_config_enabled(0U);
            g_onee_running = 1U;
            vtw_service_onee_stop();
            if (!check(disk2_enabled == 0U &&
                       g_onee_disk2_override_active == 0U,
                       "ONE//e stop restored a stale rather than latest off state")) {
                return 0;
            }
            return 1;
        }

        int main(void)
        {
            if (!test_host_live_controls() ||
                !test_onee_live_controls_without_host_intent() ||
                !test_failed_live_writes_and_pending_choice() ||
                !test_menu_preselect_context_boundary() ||
                !test_onee_configured_and_pending_speed_boundaries() ||
                !test_disk2_session_override_tracks_latest_config()) {
                return 1;
            }
            puts("ONEE VTW LIVE CONTROL NATIVE PASS");
            return 0;
        }
    '''), encoding="utf-8")

    compile_cmd = [
        str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror",
        str(harness), "-o", str(executable), f"-I{BUILD}",
        f"-I{FRONTEND}", f"-I{REPO_ROOT / 'ps_sources' / 'lib'}",
    ]
    if "mingw" in compiler.as_posix().lower():
        compile_cmd.insert(5, "-static")
    compiled = subprocess.run(
        compile_cmd, cwd=REPO_ROOT, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    )
    if compiled.returncode != 0:
        print(compiled.stdout)
        print("FAIL native_speed_control_test: host compilation failed")
        return False
    ran = subprocess.run(
        [str(executable)], cwd=REPO_ROOT, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    )
    if (ran.returncode != 0 or
            "ONEE VTW LIVE CONTROL NATIVE PASS" not in ran.stdout):
        print(ran.stdout)
        print("FAIL native_speed_control_test: harness failed")
        return False
    print("PASS native_speed_control_test")
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
    native_ok = run_native_speed_control_test()
    if failures or not native_ok:
        print(f"{len(TESTS) - len(failures)} of {len(TESTS)} tests passed; "
              f"{len(failures)} failed")
        return 1
    print(f"{len(TESTS)} ONE//e vTW source checks and native test passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
