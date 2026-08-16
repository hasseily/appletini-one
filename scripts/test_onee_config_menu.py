#!/usr/bin/env python3
"""Source checks for the session-only ONE//e Boot Settings action."""

from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
FRONTEND = REPO_ROOT / "ps_sources" / "frontend"
CONFIG_H = FRONTEND / "config_menu.h"
CONFIG_C = FRONTEND / "config_menu.c"
CONFIG_TABS_C = FRONTEND / "config_menu_main_tabs.c"
CONFIG_HELP_C = FRONTEND / "config_menu_help.c"
CONTROL_H = FRONTEND / "card_control_regs.h"
ONEE_H = FRONTEND / "onee_service.h"
ONEE_C = FRONTEND / "onee_service.c"
MAIN_C = FRONTEND / "main.c"
VITIS_SCRIPT = REPO_ROOT / "scripts" / "create_vitis_workspace.py"


class TestFailure(AssertionError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise TestFailure(message)


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def function_slice(source: str, name: str, next_name: str) -> str:
    start = source.find(name)
    end = source.find(next_name, start + len(name))
    require(start >= 0 and end > start, f"could not isolate {name}")
    return source[start:end]


def test_control_register_contract() -> None:
    header = read(CONTROL_H)

    require("CARD_CTRL_ONEE_MODE_REG                  CARD_CTRL_REG_ADDR(0x5BU)" in header,
            "ONE//e must use the reserved card-control register at offset 0x5B")
    expected = [
        "CARD_CTRL_ONEE_STATUS_REQUEST_BIT        (1UL << 0)",
        "CARD_CTRL_ONEE_STATUS_EFFECTIVE_BIT      (1UL << 1)",
        "CARD_CTRL_ONEE_STATUS_ISOLATED_BIT       (1UL << 2)",
        "CARD_CTRL_ONEE_STATUS_OUTPUTS_OFF_BIT     (1UL << 3)",
        "CARD_CTRL_ONEE_STATUS_ACTIVITY_BIT       (1UL << 4)",
        "CARD_CTRL_ONEE_STATUS_LOCKOUT_BIT        (1UL << 5)",
        "CARD_CTRL_ONEE_STATUS_QUIET_BIT          (1UL << 6)",
        "CARD_CTRL_ONEE_STATUS_RESELECT_ARMED_BIT (1UL << 7)",
        "CARD_CTRL_ONEE_STATUS_SELECTED_BIT       (1UL << 8)",
        "CARD_CTRL_ONEE_STATUS_ISOLATION_HOLD_BIT (1UL << 9)",
        "CARD_CTRL_ONEE_STATUS_INHIBIT_SHIFT      10U",
        "CARD_CTRL_ONEE_STATUS_APPLE_POWER_BIT    (1UL << 14)",
        "CARD_CTRL_ONEE_STATUS_HDL_PRESENT_BIT    (1UL << 15)",
        "CARD_CTRL_ONEE_STATUS_SIGNATURE          0xE1UL",
    ]
    require(all(item in header for item in expected),
            "firmware register bits must match the frozen PL 0x5B readback")


def test_menu_row_state_and_exact_help() -> None:
    header = read(CONFIG_H)
    tabs = read(CONFIG_TABS_C)
    help_source = read(CONFIG_HELP_C)

    require("CONFIG_MENU_BOOT_ONEE_ITEM 2U" in header and
            "CONFIG_MENU_BOOT_USB_BIND_RESET_ITEM 3U" in header,
            "ONE//e must occupy Boot Settings row 2 before USB bindings")
    require("CONFIG_MENU_ONEE_MODE_OFF = 0" in header and
            "CONFIG_MENU_ONEE_MODE_RUNNING" in header and
            "CONFIG_MENU_ONEE_MODE_LOCKED" in header,
            "the menu must expose OFF, RUNNING, and LOCKED states")
    require('"ONE//e standalone"' in tabs and
            "config_menu_onee_mode_text(menu)" in tabs and
            "y + (row_h * 2)" in tabs,
            "Boot Settings must draw the session action in the free third row")

    help_lines = [
        "Runs the built-in Enhanced Apple //e on Appletini's soft 65C02 without an Apple host.",
        "This mode is session-only and starts off after every card boot.",
        "Any Apple-bus activity stops ONE//e and keeps it off.",
        "After the connector is quiet, select this item again.",
    ]
    require(all(f'"{line}"' in help_source for line in help_lines) and
            "OVERRIDE(2,  boot_onee)" in help_source,
            "row 2 help must match the stand-alone plan exactly")


def test_session_only_action_is_not_serialized() -> None:
    source = read(CONFIG_C)
    toggle = function_slice(source,
                            "static void config_menu_toggle_onee_mode",
                            "static uint8_t config_menu_adjust_focused_value")

    require('"onee.' not in source.lower(),
            "ONE//e must have no saved config or profile key")
    require("config_menu_save_settings" not in toggle and
            "config_menu_apply_runtime" not in toggle,
            "the ONE//e action must not enter generic apply or persistence paths")
    require("menu->onee_mode_state = CONFIG_MENU_ONEE_MODE_OFF;" in source and
            "menu->onee_mode_status = 0U;" in source,
            "every firmware boot must initialize the session state off")
    require("menu->item_focus == CONFIG_MENU_BOOT_ONEE_ITEM" in source and
            "config_menu_toggle_onee_mode(menu);\n            break;" in source,
            "activation must handle ONE//e separately and leave before save/apply")


def test_disk2_boot_choice_is_independent_of_physical_slot6() -> None:
    source = read(CONFIG_C)
    help_source = read(CONFIG_HELP_C)
    apply = function_slice(source,
                           "static void config_menu_apply_boot_runtime_internal",
                           "static void config_menu_apply_smartport_paths")
    adjust = function_slice(source,
                            "static uint8_t config_menu_adjust_focused_value",
                            "static int config_menu_reload_smartport_device")
    activate = function_slice(source,
                              "static void config_menu_activate_item",
                              "void config_menu_init")

    require("config_menu_coerce_boot_device" not in source and
            "ENABLE DISK II TO BOOT SLOT 6" not in source,
            "saved Disk II choice must never be rewritten when physical Slot 6 is off")
    require("if (menu->boot_device == CONFIG_BOOT_DEVICE_DISK2) {\n"
            "            handoff = CONFIG_BOOT_HANDOFF_DISK2;" in apply and
            "menu->boot_device == CONFIG_BOOT_DEVICE_DISK2 &&" not in apply,
            "runtime apply must publish the configured Disk II target without a Slot 6 gate")
    require("menu->disk2_slot6_enabled" not in adjust and
            "menu->disk2_slot6_enabled" not in
            activate[activate.find("case CONFIG_TAB_BOOT_SETTINGS:"):
                     activate.find("case CONFIG_TAB_PROFILES:")],
            "both Boot device actions must allow Disk II while physical Slot 6 is off")
    require("ONE//e always has virtual Disk II. It keeps this choice even when physical Slot 6 is off." in
            help_source and
            "An Apple host falls back to SmartPort when Disk II is selected but physical Slot 6 is off." in
            help_source,
            "Boot device help must explain the ONE//e target and physical-host fallback")


def test_start_is_manual_only_and_activity_never_restarts() -> None:
    source = read(ONEE_C)
    start = function_slice(source,
                           "uint8_t onee_service_request_start",
                           "void onee_service_request_stop")
    poll = function_slice(source,
                          "void onee_service_poll",
                          "onee_service_state_t onee_service_state")

    require(source.count("onee_service_write_request(1U);") == 1 and
            "onee_service_write_request(1U);" in start,
            "only the explicit user-start entry may write request high")
    require("onee_service_write_request(1U);" not in poll and
            "onee_service_disarm(1U);" in poll and
            "onee_status_pl_ready(g_status) == 0U" in poll and
            "onee_status_has_hazard(g_status)" in poll,
            "polling must cancel on missing PL or activity and never reassert the session")
    require("g_lockout_latched = 1U;" in source and
            "g_lockout_latched = 0U;" in start,
            "an activity stop must stay locally locked until a new user start")


def test_start_refuses_unsafe_or_missing_pl() -> None:
    service = read(ONEE_C)
    config = read(CONFIG_C)

    require("onee_status_pl_ready(status) == 0U" in service and
            "CARD_CTRL_ONEE_STATUS_APPLE_POWER_BIT" in service and
            "CARD_CTRL_ONEE_STATUS_ACTIVITY_BIT" in service and
            "CARD_CTRL_ONEE_STATUS_LOCKOUT_BIT" in service and
            "CARD_CTRL_ONEE_STATUS_QUIET_BIT" in service and
            "CARD_CTRL_ONEE_STATUS_RESELECT_ARMED_BIT" in service,
            "start must reject missing HDL, power, activity, lockout, noise, and no rearm")
    status_text = [
        "ONE//e LOCKED: PL MODE NOT READY",
        "ONE//e LOCKED: APPLE POWER PRESENT",
        "ONE//e LOCKED: APPLE BUS ACTIVE",
        "ONE//e LOCKED: APPLE ACTIVITY LOCKOUT",
        "ONE//e LOCKED: WAITING FOR APPLE BUS QUIET",
        "ONE//e LOCKED: RESELECT NOT ARMED",
    ]
    require(all(f'"{text}"' in config for text in status_text),
            "Boot Settings must report each safety refusal")


def test_request_to_effective_wait_is_bounded() -> None:
    source = read(ONEE_C)
    poll = function_slice(source,
                          "void onee_service_poll",
                          "onee_service_state_t onee_service_state")
    wait = poll[poll.find("Bound the whole request-to-effective interval"):]

    require("ONEE_EFFECTIVE_WAIT_POLL_LIMIT" in source and
            "g_effective_wait_polls" in source and
            "if (g_effective_wait_polls < ONEE_EFFECTIVE_WAIT_POLL_LIMIT)" in wait and
            "++g_effective_wait_polls;" in wait and
            "onee_service_force_runtime_off();" in wait and
            "onee_service_disarm(1U);" in wait,
            "REQUEST=1/EFFECTIVE=0 must stop, time out, and latch LOCKED")
    require("CARD_CTRL_ONEE_STATUS_REQUEST_BIT" not in wait and
            "g_runtime_stop(g_runtime_ctx);" in source and
            "onee_service_write_request(0U);" in source,
            "the effective timeout must not depend on request echo and must disarm")


def test_platform_polling_and_standalone_runtime_binding() -> None:
    header = read(CONFIG_H)
    service_header = read(ONEE_H)
    service = read(ONEE_C)
    frontend = read(MAIN_C)
    vitis = read(VITIS_SCRIPT)

    require("uint8_t (*set_onee_mode)(void *ctx, uint8_t enable);" in header and
            "uint8_t (*get_onee_mode_state)(void *ctx);" in header and
            "uint32_t (*get_onee_mode_status)(void *ctx);" in header and
            "void config_menu_poll_onee_mode(config_menu_t *menu);" in header,
            "the menu must use platform callbacks and live polling")
    require("onee_service_init();" in frontend and
            "onee_service_poll();\n        config_menu_poll_onee_mode(&config_menu);" in frontend and
            "menu_platform.set_onee_mode = menu_platform_set_onee_mode;" in frontend,
            "frontend startup and the main loop must wire the ONE//e service")
    require("onee_service_runtime_start_fn" in service_header and
            "onee_service_runtime_stop_fn" in service_header and
            "onee_service_runtime_running_fn" in service_header and
            "onee_service_bind_runtime" in service_header and
            "onee_runtime_start" in frontend and
            "return vtw_service_onee_start(" in frontend and
            "vtw_service_onee_stop();" in frontend and
            "return vtw_service_onee_running();" in frontend and
            "onee_service_bind_runtime(onee_runtime_start," in frontend and
            "onee_service_bind_runtime(NULL" not in frontend and
            frontend.index("vtw_service_init(UART0_BASE);") <
            frontend.index("onee_service_bind_runtime(onee_runtime_start,") and
            "vtw_service" not in service,
            "stand-alone startup must bind the real vTW ONE//e entry after vTW init")
    require('"../../../ps_sources/frontend/onee_service.c"' in vitis,
            "the Vitis frontend build must include the ONE//e service")


TESTS = [
    test_control_register_contract,
    test_menu_row_state_and_exact_help,
    test_session_only_action_is_not_serialized,
    test_disk2_boot_choice_is_independent_of_physical_slot6,
    test_start_is_manual_only_and_activity_never_restarts,
    test_start_refuses_unsafe_or_missing_pl,
    test_request_to_effective_wait_is_bounded,
    test_platform_polling_and_standalone_runtime_binding,
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
        print(f"{len(TESTS) - len(failures)} of {len(TESTS)} ONE//e tests passed; "
              f"{len(failures)} failed")
        return 1
    print(f"{len(TESTS)} ONE//e tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
