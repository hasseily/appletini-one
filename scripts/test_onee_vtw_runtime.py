#!/usr/bin/env python3
"""Focused source checks for the ONE//e soft-CPU cold-start path."""

from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
FRONTEND = REPO_ROOT / "ps_sources" / "frontend"
VTW_C = FRONTEND / "vtw_service.c"
VTW_H = FRONTEND / "vtw_service.h"
ONEE_C = FRONTEND / "onee_service.c"
ONEE_H = FRONTEND / "onee_service.h"
MAIN_C = FRONTEND / "main.c"


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

    require("onee_service_runtime_running_fn" in header and
            "onee_runtime_start" in main and
            "return vtw_service_onee_start(" in main and
            "vtw_service_onee_stop();" in main and
            "return vtw_service_onee_running();" in main,
            "main must bind start, stop, and actual-running hooks")
    bind = main.find("onee_service_bind_runtime(onee_runtime_start,")
    require(bind > main.find("vtw_service_init(UART0_BASE);") and
            "onee_service_bind_runtime(NULL" not in main,
            "ONE//e hooks must be real and bound only after vTW init")


def test_cold_start_is_direct_and_isolation_first() -> None:
    source = read(VTW_C)
    start = between(source,
                    "uint8_t vtw_service_onee_start(",
                    "void vtw_service_onee_stop(void)")

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
                    "void vtw_service_onee_stop(void)")
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
                    "void vtw_service_onee_stop(void)")

    require("CARD_CTRL_VTW_CTRL_DISABLE_D2_ACCEL_BIT" in ctrl and
            "vtw_eff_mode()" in ctrl and "vtw_eff_divider()" in ctrl,
            "ONE//e must force the Disk II shortcut off while retaining speed options")
    stop = between(source,
                   "void vtw_service_onee_stop(void)",
                   "uint8_t vtw_service_onee_running(void)")
    main = read(MAIN_C)
    require("g_onee_disk2_restore_enabled" in start and
            "disk2_service_set_enabled(1U);" in start and
            start.find("disk2_service_set_enabled(1U);") <
            start.find("vtw_shadow_force_cold_start(1U)") and
            "disk2_service_set_enabled(g_onee_disk2_restore_enabled);" in stop and
            "g_card_slot_enable_mask" in main and
            "CARD_CTRL_SLOT_DISK2" in main,
            "ONE//e must session-enable Disk II and restore the saved bit-6 state")
    require(start.count("vtw_service_onee_stop();") == 3 and
            stop.find("REG_WRITE(CARD_CTRL_VTW_CTRL_REG, 0U);") <
            stop.find("disk2_service_set_enabled(g_onee_disk2_restore_enabled);"),
            "every failed start and stop must clear vTW before restoring Disk II")
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


def test_stop_and_effective_drop_clear_vtw_before_onee_request() -> None:
    vtw = read(VTW_C)
    onee = read(ONEE_C)
    stop = between(vtw,
                   "void vtw_service_onee_stop(void)",
                   "uint8_t vtw_service_onee_running(void)")
    disarm = between(onee,
                     "static void onee_service_disarm",
                     "void onee_service_init(void)")
    poll = between(onee,
                   "void onee_service_poll(void)",
                   "onee_service_state_t onee_service_state(void)")

    require("REG_WRITE(CARD_CTRL_VTW_CTRL_REG, 0U);" in stop,
            "stand-alone stop must clear VTW CTRL with a literal zero")
    require(disarm.find("onee_service_stop_runtime();") <
            disarm.find("onee_service_write_request(0U);") and
            "g_effective_seen != 0U" in poll and
            "onee_service_disarm(1U);" in poll,
            "an effective drop must stop vTW before clearing the ONE//e request")


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


TESTS = [
    test_real_runtime_hooks_are_bound_after_vtw_init,
    test_cold_start_is_direct_and_isolation_first,
    test_rom_and_cold_signature_helpers_are_shared,
    test_standalone_forces_synthetic_disk2_without_changing_options,
    test_stop_and_effective_drop_clear_vtw_before_onee_request,
    test_running_state_requires_released_core_status,
    test_menu_closes_once_only_after_true_running,
    test_normal_host_state_machine_remains_after_onee_gate,
]


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
    if failures:
        print(f"{len(TESTS) - len(failures)} of {len(TESTS)} tests passed; "
              f"{len(failures)} failed")
        return 1
    print(f"{len(TESTS)} ONE//e vTW runtime tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
