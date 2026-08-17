#!/usr/bin/env python3
"""Source and native checks for the ONE//e Boot Settings action."""

import shutil
import subprocess
import textwrap
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
FRONTEND = REPO_ROOT / "ps_sources" / "frontend"
CONFIG_H = FRONTEND / "config_menu.h"
CONFIG_C = FRONTEND / "config_menu.c"
CONFIG_TABS_C = FRONTEND / "config_menu_main_tabs.c"
CONFIG_HELP_C = FRONTEND / "config_menu_help.c"
CONFIG_INTERNAL_H = FRONTEND / "config_menu_internal.h"
CONTROL_H = FRONTEND / "card_control_regs.h"
ONEE_H = FRONTEND / "onee_service.h"
ONEE_C = FRONTEND / "onee_service.c"
MAIN_C = FRONTEND / "main.c"
VITIS_SCRIPT = REPO_ROOT / "scripts" / "create_vitis_workspace.py"
NATIVE_BUILD = REPO_ROOT / "build" / "onee_service_native"


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
    internal = read(CONFIG_INTERNAL_H)
    source = read(CONFIG_C)
    tabs = read(CONFIG_TABS_C)
    help_source = read(CONFIG_HELP_C)
    boot_draw = function_slice(tabs,
                               "void config_menu_draw_boot_settings",
                               "void config_menu_draw_video")
    video_draw = function_slice(tabs,
                                "void config_menu_draw_video",
                                "void config_menu_draw_clock")

    require("CONFIG_MENU_BOOT_ONEE_ITEM 2U" in header and
            "CONFIG_MENU_BOOT_USB_BIND_RESET_ITEM 3U" in header and
            "#define CONFIG_MENU_BOOT_ONEE_STANDARD_ITEM \\\n"
            "    CONFIG_MENU_BOOT_USB_BIND_RESET_ITEM" in header,
            "ONE//e must occupy Boot Settings row 2 before USB bindings")
    require("CONFIG_MENU_ONEE_MODE_OFF = 0" in header and
            "CONFIG_MENU_ONEE_MODE_RUNNING" in header and
            "CONFIG_MENU_ONEE_MODE_LOCKED" in header,
            "the menu must expose OFF, RUNNING, and LOCKED states")
    require('"ONE//e standalone"' in boot_draw and
            "config_menu_onee_mode_text(menu)" in boot_draw and
            "y + (row_h * 2)" in boot_draw,
            "Boot Settings must draw the session action in the free third row")
    require('"ONE//e video standard"' in boot_draw and
            "config_menu_onee_video_standard_text(menu)" in boot_draw and
            "y + (row_h * 3)" in boot_draw and
            "if (onee_fixed != 0U)" in boot_draw and
            '"ONE//e video standard"' not in video_draw and
            "CONFIG_VIDEO_ITEM_ONEE_STANDARD" not in internal,
            "the ONE//e-only video standard must sit below standalone, not on Video")
    require("return CONFIG_MENU_BOOT_ONEE_STANDARD_ITEM + 1U;" in source and
            "case CONFIG_TAB_VIDEO:\n        return CONFIG_VIDEO_ITEM_COUNT;" in source,
            "ONE//e Boot Settings navigation must include the standard while Video stays fixed")
    require("standard_was_visible != 0U &&\n"
            "        config_menu_onee_fixed_bindings_active(menu) == 0U" in source and
            "menu->item_focus = CONFIG_MENU_BOOT_ONEE_ITEM;" in source,
            "a stopped ONE//e must not turn focused PAL/NTSC into Reset USB Bindings")

    help_lines = [
        "Runs the built-in Enhanced Apple //e on Appletini's soft 65C02 without an Apple host.",
        "The selection survives a card power cycle and starts only after the connector is quiet.",
        "Any Apple-bus activity stops ONE//e and saves it OFF.",
        "After the connector is quiet, select this item again to save it ON.",
    ]
    require(all(f'"{line}"' in help_source for line in help_lines) and
            "OVERRIDE(2,  boot_onee)" in help_source,
            "row 2 help must match the stand-alone plan exactly")
    require("HELP(boot_onee_standard," in help_source and
            "OVERRIDE(CONFIG_MENU_BOOT_ONEE_STANDARD_HELP_ITEM, boot_onee_standard)" in
            help_source and
            "help_item = CONFIG_MENU_BOOT_ONEE_STANDARD_HELP_ITEM;" in source and
            "help_item == CONFIG_MENU_BOOT_ONEE_STANDARD_ITEM" in source,
            "the shared item 3 must resolve ONE//e standard help only while active")


def test_onee_standard_keeps_global_callback_and_actions() -> None:
    source = read(CONFIG_C)
    adjust = function_slice(source,
                            "static uint8_t config_menu_adjust_focused_value",
                            "static int config_menu_reload_smartport_device")
    activate = function_slice(source,
                              "static void config_menu_activate_item",
                              "void config_menu_init")

    require("menu->tab == CONFIG_TAB_BOOT_SETTINGS" in adjust and
            "menu->item_focus == CONFIG_MENU_BOOT_ONEE_STANDARD_ITEM" in adjust and
            "config_menu_onee_fixed_bindings_active(menu) != 0U" in adjust and
            "config_menu_toggle_onee_video_standard(menu);" in adjust,
            "left/right must change the relocated standard only in active ONE//e")
    boot_activate = activate[activate.index("case CONFIG_TAB_BOOT_SETTINGS:"):
                             activate.index("case CONFIG_TAB_PROFILES:")]
    require("menu->item_focus == CONFIG_MENU_BOOT_USB_BIND_RESET_ITEM" in boot_activate and
            "if (config_menu_onee_fixed_bindings_active(menu) != 0U) {\n"
            "                config_menu_toggle_onee_video_standard(menu);" in boot_activate and
            '"USB MENU BINDINGS RESET"' in boot_activate,
            "Enter must toggle the standard in ONE//e and reset bindings outside it")
    require("menu->platform.set_onee_video_50hz(menu->platform.ctx," in source and
            "config_menu_save_settings_to_path(\n"
            "            menu, APPLETINI_CFG_PATH, NULL)" in source,
            "the relocated row must keep its live callback and global persistence")


def test_global_persistent_action_is_not_profiled() -> None:
    source = read(CONFIG_C)
    header = read(CONFIG_H)
    toggle = function_slice(source,
                            "static void config_menu_toggle_onee_mode",
                            "static uint8_t config_menu_adjust_focused_value")
    poll = function_slice(source,
                          "void config_menu_poll_onee_mode",
                          "uint8_t config_menu_is_active")

    require('"onee.standalone.persisted"' in source and
            "menu->onee_persisted_enabled = config_menu_bool_text(value);" in source and
            'if (strcmp(path, APPLETINI_CFG_PATH) == 0)' in source,
            "ONE//e must serialize its latch only in the global config")
    require('if (strcmp(key, "onee.standalone.persisted") != 0 &&' in source and
            'strcmp(key, "onee.video.standard") != 0' in source and
            "ONE//e state and video standard are global" in source,
            "profile loads must ignore global ONE//e choices")
    require("onee_persisted_enabled" in header and
            "onee_persist_write_failed" in header and
            "onee_persist_retry_polls" in header and
            "restore_onee_mode_intent" in header and
            "get_onee_mode_persist_update" in header and
            "ack_onee_mode_persist_update" in header,
            "the menu platform must expose restore and write-ack plumbing")
    require("config_menu_apply_runtime" not in toggle and
            "config_menu_poll_onee_mode(menu);" in toggle,
            "the ONE//e action must persist through the service update path")
    require("menu->onee_persisted_enabled != 0U" in toggle,
            "a saved but safety-blocked ON intent must still allow manual OFF")
    query_pos = poll.find("get_onee_mode_persist_update(")
    save_pos = poll.find("config_menu_save_settings_to_path(")
    ack_pos = poll.find("ack_onee_mode_persist_update(")
    require(0 <= query_pos < save_pos < ack_pos and
            "menu->onee_persist_write_failed = 1U;" in poll and
            "ONEE_PERSIST_RETRY_POLL_LIMIT" in poll and
            "--menu->onee_persist_retry_polls;" in poll and
            '"ONE//e STATE WRITE RECOVERED"' in poll,
            "OFF/ON updates must stay dirty until the global write succeeds")
    require("menu->onee_mode_state = CONFIG_MENU_ONEE_MODE_OFF;" in source and
            "menu->onee_mode_status = 0U;" in source and
            "menu->onee_persisted_enabled = 0U;" in source,
            "a first boot with no saved key must start from a safe OFF default")
    require("menu->item_focus == CONFIG_MENU_BOOT_ONEE_ITEM" in source and
            "config_menu_toggle_onee_mode(menu);\n            break;" in source,
            "activation must handle ONE//e separately and leave before save/apply")


def test_global_persistence_is_synced_before_onee_request() -> None:
    source = read(CONFIG_C)
    save = function_slice(source,
                          "uint8_t config_menu_save_settings_to_path",
                          "void config_menu_save_settings")
    commit = function_slice(source,
                            "static FRESULT config_menu_commit_global_cfg",
                            "static FRESULT config_menu_open_cfg")
    open_cfg = function_slice(source,
                              "static FRESULT config_menu_open_cfg",
                              "const char *config_menu_boot_timeout_text")
    toggle = function_slice(source,
                            "static void config_menu_toggle_onee_mode",
                            "static uint8_t config_menu_adjust_focused_value")

    require('#define APPLETINI_CFG_TMP_PATH "0:/appletini_cfg.tmp"' in source and
            '#define APPLETINI_CFG_BAK_PATH "0:/appletini_cfg.bak"' in source,
            "the global config must have fixed same-directory temp and backup names")
    write_pos = save.find("f_write(&file")
    sync_pos = save.find("f_sync(&file)")
    close_pos = save.find("f_close(&file)")
    commit_pos = save.find("config_menu_commit_global_cfg()")
    require(0 <= write_pos < sync_pos < close_pos < commit_pos and
            "close_fr != FR_OK" in save,
            "global settings must be fully written, synced, and closed before commit")
    require("f_unlink(APPLETINI_CFG_BAK_PATH)" in commit and
            "f_rename(APPLETINI_CFG_PATH, APPLETINI_CFG_BAK_PATH)" in commit and
            "f_rename(APPLETINI_CFG_TMP_PATH, APPLETINI_CFG_PATH)" in commit and
            "f_rename(APPLETINI_CFG_BAK_PATH, APPLETINI_CFG_PATH)" in commit,
            "global commit must retain and restore the prior synced config")
    require("fr == FR_NO_FILE" in open_cfg and
            "f_rename(APPLETINI_CFG_BAK_PATH, APPLETINI_CFG_PATH)" in open_cfg,
            "boot must recover the backup if power failed during replacement")

    save_on_pos = toggle.find("config_menu_save_settings_to_path(")
    request_on_pos = toggle.find(
        "accepted = menu->platform.set_onee_mode(menu->platform.ctx, 1U);")
    require(0 <= save_on_pos < request_on_pos and
            '"ONE//e ON REFUSED: STATE NOT SAVED"' in toggle and
            "menu->onee_persisted_enabled = 0U;" in toggle,
            "manual ON must not call the service until the synced global ON write succeeds")
    poll_pos = toggle.find("config_menu_poll_onee_mode(menu);", request_on_pos)
    reject_pos = toggle.find("if (accepted == 0U)", poll_pos)
    require(0 <= request_on_pos < poll_pos < reject_pos,
            "a rejected request must let the service's forced OFF update save before refusal")


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


def test_only_manual_or_guarded_restore_can_start() -> None:
    source = read(ONEE_C)
    start = function_slice(source,
                           "uint8_t onee_service_request_start",
                           "void onee_service_request_stop")
    poll = function_slice(source,
                          "void onee_service_poll",
                          "onee_service_state_t onee_service_state")

    require(source.count("onee_service_write_request(1U);") == 2 and
            "onee_service_write_request(1U);" in start and
            "g_restore_pending != 0U" in poll and
            "onee_status_can_start(g_status) == 0U" in poll,
            "only manual start or one safety-checked reboot restore may write high")
    require("onee_service_force_persisted(0U);" in poll and
            "onee_service_disarm(1U);" in poll and
            "onee_status_pl_ready(g_status) == 0U" in poll and
            "onee_status_has_hazard(g_status)" in poll and
            "(g_status & CARD_CTRL_ONEE_STATUS_REQUEST_BIT) == 0U" in poll,
            "polling must revoke persistence for activity or a lost request")
    require("g_lockout_latched = 1U;" in source and
            "g_lockout_latched = 0U;" in start,
            "an activity stop must stay locally locked until a new user start")
    require("onee_service_force_persisted(0U);" in start,
            "a hazard during manual ON must queue OFF even before service intent changes")


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


def test_selected_request_has_no_software_expiry() -> None:
    source = read(ONEE_C)
    poll = function_slice(source,
                          "void onee_service_poll",
                          "onee_service_state_t onee_service_state")
    pending = poll[poll.find("REQUEST is still high"):]

    require("ONEE_EFFECTIVE_WAIT_POLL_LIMIT" not in source and
            "g_effective_wait_polls" not in source and
            "onee_service_disarm" not in pending and
            "onee_service_suspend_runtime();" in pending,
            "a safe REQUEST=1/EFFECTIVE=0 state must retain the user selection without expiry")
    require("ONEE_RUNTIME_RETRY_POLL_LIMIT" in source and
            "g_runtime_retry_polls" in source and
            "onee_service_suspend_runtime();" in poll and
            "g_runtime_retry_polls = ONEE_RUNTIME_RETRY_POLL_LIMIT;" in poll,
            "a private soft-core start failure must stop and retry without clearing selection")
    require("g_runtime_running(g_runtime_ctx) == 0U" in poll and
            "g_runtime_retry_polls = 0U;" in poll and
            "if (onee_status_pl_ready(g_status) == 0U)" in poll and
            "if (onee_status_has_hazard(g_status) != 0U)" in poll and
            "if ((g_status & CARD_CTRL_ONEE_STATUS_REQUEST_BIT) == 0U)" in poll,
            "a released-core drop must retry while only safety failures disarm the mode")


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


def test_native_latched_selection_lifecycle() -> None:
    compiler = find_native_c_compiler()
    if compiler is None:
        print("SKIP native_latched_selection_lifecycle: no host C compiler")
        return

    if NATIVE_BUILD.exists():
        shutil.rmtree(NATIVE_BUILD)
    NATIVE_BUILD.mkdir(parents=True)
    harness = NATIVE_BUILD / "onee_service_harness.c"
    executable = NATIVE_BUILD / "onee_service_harness.exe"
    harness.write_text(textwrap.dedent(r'''
        #include <stdint.h>
        #include <stdio.h>

        static uint32_t mode_status;
        static uint32_t high_writes;
        static uint32_t low_writes;
        static uint32_t start_calls;
        static uint32_t suspend_calls;
        static uint32_t stop_calls;
        static uint32_t start_failures;
        static uint32_t runtime_speed_override;
        static uint32_t last_start_speed_override;
        static uint8_t runtime_live;

        static uint32_t test_reg_read(uint32_t address)
        {
            (void)address;
            return mode_status;
        }

        static void test_reg_write(uint32_t address, uint32_t value)
        {
            (void)address;
            if ((value & 1U) != 0U) {
                ++high_writes;
            } else {
                ++low_writes;
            }
        }

        #define COMMON_H
        #define REG_READ(address) test_reg_read((uint32_t)(address))
        #define REG_WRITE(address, value) \
            test_reg_write((uint32_t)(address), (uint32_t)(value))
        #include "onee_service.c"

        static uint32_t status_base(void)
        {
            return (CARD_CTRL_ONEE_STATUS_SIGNATURE <<
                    CARD_CTRL_ONEE_STATUS_SIGNATURE_SHIFT) |
                   CARD_CTRL_ONEE_STATUS_HDL_PRESENT_BIT;
        }

        static uint32_t status_safe_off(void)
        {
            return status_base() |
                   CARD_CTRL_ONEE_STATUS_QUIET_BIT |
                   CARD_CTRL_ONEE_STATUS_RESELECT_ARMED_BIT |
                   (CARD_CTRL_ONEE_INHIBIT_MANUAL_OFF <<
                    CARD_CTRL_ONEE_STATUS_INHIBIT_SHIFT);
        }

        static uint32_t status_request_pending(void)
        {
            return status_base() |
                   CARD_CTRL_ONEE_STATUS_REQUEST_BIT |
                   CARD_CTRL_ONEE_STATUS_ISOLATED_BIT |
                   CARD_CTRL_ONEE_STATUS_QUIET_BIT |
                   (CARD_CTRL_ONEE_INHIBIT_RESELECT_REQUIRED <<
                    CARD_CTRL_ONEE_STATUS_INHIBIT_SHIFT);
        }

        static uint32_t status_effective(void)
        {
            return status_base() |
                   CARD_CTRL_ONEE_STATUS_REQUEST_BIT |
                   CARD_CTRL_ONEE_STATUS_EFFECTIVE_BIT |
                   CARD_CTRL_ONEE_STATUS_ISOLATED_BIT |
                   CARD_CTRL_ONEE_STATUS_QUIET_BIT |
                   CARD_CTRL_ONEE_STATUS_SELECTED_BIT;
        }

        static uint8_t runtime_start(void *ctx)
        {
            (void)ctx;
            ++start_calls;
            last_start_speed_override = runtime_speed_override;
            if (start_failures != 0U) {
                --start_failures;
                runtime_live = 0U;
                return 0U;
            }
            runtime_live = 1U;
            return 1U;
        }

        static void runtime_suspend(void *ctx)
        {
            (void)ctx;
            ++suspend_calls;
            runtime_live = 0U;
        }

        static void runtime_stop(void *ctx)
        {
            (void)ctx;
            ++stop_calls;
            runtime_live = 0U;
            runtime_speed_override = 0U;
        }

        static uint8_t runtime_running(void *ctx)
        {
            (void)ctx;
            return runtime_live;
        }

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
            uint32_t high_before;
            uint32_t low_before;
            uint32_t stop_before;

            mode_status = status_safe_off();
            onee_service_init();
            onee_service_bind_runtime(runtime_start, runtime_suspend,
                                      runtime_stop,
                                      runtime_running, NULL);
            runtime_speed_override = 19U;
            if (!check(high_writes == 0U && low_writes == 1U,
                       "firmware boot did not force the session off") ||
                !check(onee_service_request_start() == 1U && high_writes == 1U,
                       "manual selection did not write the sole high request")) {
                return 1;
            }

            mode_status = status_request_pending();
            high_before = high_writes;
            for (uint32_t i = 0U; i < 512U; ++i) {
                onee_service_poll();
            }
            if (!check(high_writes == high_before && low_writes == 1U &&
                       start_calls == 0U && g_manual_request != 0U,
                       "safe request wait expired or rewrote the PL request")) {
                return 1;
            }

            mode_status = status_effective();
            start_failures = 1U;
            onee_service_poll();
            if (!check(start_calls == 1U && suspend_calls == 1U &&
                       stop_calls == 0U && runtime_speed_override == 19U &&
                       last_start_speed_override == 19U &&
                       g_manual_request != 0U && high_writes == high_before,
                       "runtime start failure lost the selected mode or speed")) {
                return 1;
            }
            for (uint32_t i = 0U; i < ONEE_RUNTIME_RETRY_POLL_LIMIT; ++i) {
                onee_service_poll();
            }
            if (!check(start_calls == 1U && g_manual_request != 0U,
                       "runtime retry delay changed the selection")) {
                return 1;
            }
            onee_service_poll();
            if (!check(start_calls == 2U && runtime_live != 0U &&
                       runtime_speed_override == 19U &&
                       last_start_speed_override == 19U &&
                       onee_service_state() == ONEE_SERVICE_STATE_RUNNING,
                       "selected mode did not retry at its exact speed")) {
                return 1;
            }

            stop_before = stop_calls;
            onee_service_poll();
            if (!check(start_calls == 2U && stop_calls == stop_before &&
                       high_writes == high_before,
                       "ordinary virtual-machine operation changed selection")) {
                return 1;
            }

            runtime_live = 0U;
            onee_service_poll();
            if (!check(g_manual_request != 0U &&
                       suspend_calls == 2U && stop_calls == stop_before &&
                       runtime_speed_override == 19U,
                       "released-core drop cleared the selection or speed")) {
                return 1;
            }
            onee_service_poll();
            if (!check(start_calls == 3U && runtime_live != 0U &&
                       last_start_speed_override == 19U &&
                       onee_service_state() == ONEE_SERVICE_STATE_RUNNING,
                       "released-core drop did not restart at the exact speed")) {
                return 1;
            }

            mode_status = status_base() |
                          CARD_CTRL_ONEE_STATUS_ACTIVITY_BIT |
                          CARD_CTRL_ONEE_STATUS_LOCKOUT_BIT |
                          (CARD_CTRL_ONEE_INHIBIT_APPLE_ACTIVITY <<
                           CARD_CTRL_ONEE_STATUS_INHIBIT_SHIFT);
            onee_service_poll();
            if (!check(g_manual_request == 0U && low_writes == 2U &&
                       stop_calls == stop_before + 1U &&
                       runtime_speed_override == 0U &&
                       onee_service_state() == ONEE_SERVICE_STATE_LOCKED,
                       "Apple activity did not latch off and clear session speed")) {
                return 1;
            }
            mode_status = status_safe_off();
            high_before = high_writes;
            for (uint32_t i = 0U; i < 512U; ++i) {
                onee_service_poll();
            }
            if (!check(high_writes == high_before &&
                       onee_service_state() == ONEE_SERVICE_STATE_LOCKED,
                       "quiet polling restarted an activity-locked mode")) {
                return 1;
            }

            runtime_speed_override = 23U;
            if (!check(onee_service_request_start() == 1U &&
                       high_writes == high_before + 1U,
                       "fresh manual selection did not clear local lockout")) {
                return 1;
            }
            mode_status = status_effective();
            onee_service_poll();
            mode_status = status_safe_off();
            high_before = high_writes;
            stop_before = stop_calls;
            onee_service_poll();
            for (uint32_t i = 0U; i < 512U; ++i) {
                onee_service_poll();
            }
            if (!check(g_manual_request == 0U && high_writes == high_before &&
                       stop_calls == stop_before + 1U &&
                       runtime_speed_override == 0U &&
                       onee_service_state() == ONEE_SERVICE_STATE_LOCKED,
                       "lost request echo did not clear the session speed")) {
                return 1;
            }

            mode_status = status_safe_off();
            runtime_speed_override = 29U;
            if (!check(onee_service_request_start() == 1U &&
                       high_writes == high_before + 1U,
                       "lost-request lock did not require a fresh manual ON")) {
                return 1;
            }
            mode_status = status_effective();
            onee_service_poll();
            low_before = low_writes;
            high_before = high_writes;
            stop_before = stop_calls;
            onee_service_request_stop();
            mode_status = status_safe_off();
            onee_service_poll();
            if (!check(g_manual_request == 0U && low_writes == low_before + 1U &&
                       stop_calls == stop_before + 1U &&
                       runtime_speed_override == 0U &&
                       onee_service_state() == ONEE_SERVICE_STATE_OFF,
                       "explicit manual OFF did not clear the session speed")) {
                return 1;
            }
            if (!check(onee_service_request_start() == 1U &&
                       high_writes == high_before + 1U,
                       "manual OFF followed by fresh ON did not restart selection")) {
                return 1;
            }

            puts("ONEE LATCHED SELECTION NATIVE PASS");
            return 0;
        }
    '''), encoding="utf-8")

    command = [
        str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror",
        str(harness), "-o", str(executable), f"-I{FRONTEND}",
    ]
    if "mingw" in compiler.as_posix().lower():
        command.insert(5, "-static")
    compiled = subprocess.run(
        command, cwd=REPO_ROOT, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    )
    require(compiled.returncode == 0,
            f"native lifecycle harness did not compile:\n{compiled.stdout}")
    ran = subprocess.run(
        [str(executable)], cwd=REPO_ROOT, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    )
    require(ran.returncode == 0 and
            "ONEE LATCHED SELECTION NATIVE PASS" in ran.stdout,
            f"native lifecycle harness failed:\n{ran.stdout}")


def test_native_persistent_reboot_lifecycle() -> None:
    compiler = find_native_c_compiler()
    if compiler is None:
        print("SKIP native_persistent_reboot_lifecycle: no host C compiler")
        return

    NATIVE_BUILD.mkdir(parents=True, exist_ok=True)
    harness = NATIVE_BUILD / "onee_persistence_harness.c"
    executable = NATIVE_BUILD / "onee_persistence_harness.exe"
    harness.write_text(textwrap.dedent(r'''
        #include <stdint.h>
        #include <stdio.h>

        static uint32_t mode_status;
        static uint32_t high_writes;
        static uint32_t low_writes;
        static uint32_t start_calls;
        static uint32_t stop_calls;
        static uint8_t runtime_live;

        static uint32_t test_reg_read(uint32_t address)
        {
            (void)address;
            return mode_status;
        }

        static void test_reg_write(uint32_t address, uint32_t value)
        {
            (void)address;
            if ((value & 1U) != 0U) {
                ++high_writes;
            } else {
                ++low_writes;
            }
        }

        #define COMMON_H
        #define REG_READ(address) test_reg_read((uint32_t)(address))
        #define REG_WRITE(address, value) \
            test_reg_write((uint32_t)(address), (uint32_t)(value))
        #include "onee_service.c"

        static uint32_t status_base(void)
        {
            return (CARD_CTRL_ONEE_STATUS_SIGNATURE <<
                    CARD_CTRL_ONEE_STATUS_SIGNATURE_SHIFT) |
                   CARD_CTRL_ONEE_STATUS_HDL_PRESENT_BIT;
        }

        static uint32_t status_safe_off(void)
        {
            return status_base() |
                   CARD_CTRL_ONEE_STATUS_QUIET_BIT |
                   CARD_CTRL_ONEE_STATUS_RESELECT_ARMED_BIT |
                   (CARD_CTRL_ONEE_INHIBIT_MANUAL_OFF <<
                    CARD_CTRL_ONEE_STATUS_INHIBIT_SHIFT);
        }

        static uint32_t status_effective(void)
        {
            return status_base() |
                   CARD_CTRL_ONEE_STATUS_REQUEST_BIT |
                   CARD_CTRL_ONEE_STATUS_EFFECTIVE_BIT |
                   CARD_CTRL_ONEE_STATUS_ISOLATED_BIT |
                   CARD_CTRL_ONEE_STATUS_QUIET_BIT |
                   CARD_CTRL_ONEE_STATUS_SELECTED_BIT;
        }

        static uint32_t status_activity(void)
        {
            return status_base() |
                   CARD_CTRL_ONEE_STATUS_ACTIVITY_BIT |
                   CARD_CTRL_ONEE_STATUS_LOCKOUT_BIT |
                   (CARD_CTRL_ONEE_INHIBIT_APPLE_ACTIVITY <<
                    CARD_CTRL_ONEE_STATUS_INHIBIT_SHIFT);
        }

        static uint8_t runtime_start(void *ctx)
        {
            (void)ctx;
            ++start_calls;
            runtime_live = 1U;
            return 1U;
        }

        static void runtime_suspend(void *ctx)
        {
            (void)ctx;
            runtime_live = 0U;
        }

        static void runtime_stop(void *ctx)
        {
            (void)ctx;
            ++stop_calls;
            runtime_live = 0U;
        }

        static uint8_t runtime_running(void *ctx)
        {
            (void)ctx;
            return runtime_live;
        }

        static void bind_runtime(void)
        {
            onee_service_bind_runtime(runtime_start, runtime_suspend,
                                      runtime_stop, runtime_running, NULL);
        }

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
            uint8_t persist_value = 0xFFU;
            uint32_t high_before;

            /* Model the two-phase menu path: the SD file already says ON,
             * while the service still has its reset-time OFF value. Apple
             * activity in that window must force a new durable-OFF request. */
            mode_status = status_activity();
            onee_service_init();
            bind_runtime();
            high_before = high_writes;
            if (!check(onee_service_request_start() == 0U &&
                       high_writes == high_before &&
                       onee_service_persist_update_pending(&persist_value) != 0U &&
                       persist_value == 0U,
                       "hazard rejection did not force OFF after staged ON")) {
                return 1;
            }
            onee_service_persist_update_ack(0U);

            /* The menu saves ON before it asks the service to start. Model a
             * non-hazard refusal while the service still holds reset-time
             * OFF, then prove that a later manual OFF still queues storage
             * OFF instead of being dropped as a duplicate. */
            mode_status = status_safe_off();
            onee_service_init();
            high_before = high_writes;
            if (!check(onee_service_request_start() == 0U &&
                       high_writes == high_before &&
                       onee_service_persist_update_pending(NULL) == 0U,
                       "unbound runtime did not refuse staged ON cleanly")) {
                return 1;
            }
            onee_service_request_stop();
            if (!check(onee_service_persist_update_pending(&persist_value) != 0U &&
                       persist_value == 0U,
                       "manual OFF after refused staged ON did not queue OFF")) {
                return 1;
            }
            onee_service_persist_update_ack(0U);

            /* A saved ON value remains inert until both runtime hooks and the
             * exact PL quiet/reselect safety state are present. */
            mode_status = status_safe_off();
            onee_service_init();
            onee_service_restore_persisted(1U);
            onee_service_poll();
            if (!check(high_writes == 0U && g_restore_pending != 0U,
                       "restore started before runtime was ready")) {
                return 1;
            }
            bind_runtime();
            onee_service_poll();
            if (!check(high_writes == 1U && g_manual_request != 0U &&
                       g_restore_pending == 0U && g_persisted_intent != 0U &&
                       onee_service_persist_update_pending(NULL) == 0U,
                       "safe reboot did not restore the saved ON intent once")) {
                return 1;
            }
            mode_status = status_effective();
            onee_service_poll();
            if (!check(onee_service_state() == ONEE_SERVICE_STATE_RUNNING,
                       "restored request did not start the runtime")) {
                return 1;
            }

            /* A second simulated card boot proves that ON was not a session
             * latch. init itself always drives the PL request low first. */
            runtime_live = 0U;
            mode_status = status_safe_off();
            high_before = high_writes;
            onee_service_init();
            onee_service_restore_persisted(1U);
            bind_runtime();
            onee_service_poll();
            if (!check(high_writes == high_before + 1U &&
                       g_persisted_intent != 0U,
                       "saved ON intent did not survive a simulated reboot")) {
                return 1;
            }
            mode_status = status_effective();
            onee_service_poll();

            /* PL activity wins first, stops the core, revokes saved ON, and
             * queues an OFF write. Later quiet time cannot reassert REQUEST. */
            mode_status = status_activity();
            onee_service_poll();
            if (!check(g_manual_request == 0U && g_persisted_intent == 0U &&
                       onee_service_persist_update_pending(&persist_value) != 0U &&
                       persist_value == 0U &&
                       onee_service_state() == ONEE_SERVICE_STATE_LOCKED,
                       "Apple activity did not revoke and queue saved OFF")) {
                return 1;
            }
            onee_service_restore_persisted(1U);
            if (!check(g_persisted_intent == 0U &&
                       onee_service_persist_update_pending(&persist_value) != 0U &&
                       persist_value == 0U,
                       "late stale config replaced an unsaved activity OFF")) {
                return 1;
            }
            mode_status = status_safe_off();
            high_before = high_writes;
            for (uint32_t i = 0U; i < 256U; ++i) {
                onee_service_poll();
            }
            if (!check(high_writes == high_before,
                       "quiet time restarted an activity-cleared intent")) {
                return 1;
            }
            onee_service_persist_update_ack(0U);
            if (!check(onee_service_persist_update_pending(NULL) == 0U,
                       "OFF persistence acknowledgement did not clear dirty state")) {
                return 1;
            }

            /* A fresh manual selection both clears the local lock and queues
             * saved ON; manual OFF queues saved OFF. */
            if (!check(onee_service_request_start() == 1U &&
                       high_writes == high_before + 1U &&
                       onee_service_persist_update_pending(&persist_value) != 0U &&
                       persist_value == 1U,
                       "manual reselect did not queue saved ON")) {
                return 1;
            }
            onee_service_restore_persisted(0U);
            if (!check(g_persisted_intent != 0U &&
                       onee_service_persist_update_pending(&persist_value) != 0U &&
                       persist_value == 1U,
                       "late stale config replaced an unsaved manual ON")) {
                return 1;
            }
            onee_service_persist_update_ack(1U);
            onee_service_request_stop();
            if (!check(onee_service_persist_update_pending(&persist_value) != 0U &&
                       persist_value == 0U,
                       "manual OFF did not queue saved OFF")) {
                return 1;
            }

            /* A restored ON intent cannot start against missing safety logic.
             * It is kept for a later valid boot/state instead of being erased
             * by a bad or incomplete PL image. */
            mode_status = 0U;
            high_before = high_writes;
            onee_service_init();
            onee_service_restore_persisted(1U);
            bind_runtime();
            for (uint32_t i = 0U; i < 256U; ++i) {
                onee_service_poll();
            }
            if (!check(high_writes == high_before &&
                       g_restore_pending != 0U && g_persisted_intent != 0U &&
                       onee_service_state() == ONEE_SERVICE_STATE_LOCKED,
                       "missing PL enabled ONE//e or erased saved intent")) {
                return 1;
            }
            mode_status = status_safe_off();
            onee_service_poll();
            if (!check(high_writes == high_before + 1U,
                       "restored intent did not wait for a valid safe PL state")) {
                return 1;
            }

            /* Activity already present at restore time clears the saved value
             * without issuing even a transient high request. */
            mode_status = status_activity();
            high_before = high_writes;
            onee_service_init();
            onee_service_restore_persisted(1U);
            bind_runtime();
            onee_service_poll();
            if (!check(high_writes == high_before &&
                       g_persisted_intent == 0U &&
                       onee_service_persist_update_pending(&persist_value) != 0U &&
                       persist_value == 0U,
                       "unsafe reboot restore wrote high or failed to save OFF")) {
                return 1;
            }

            puts("ONEE PERSISTENT REBOOT NATIVE PASS");
            return 0;
        }
    '''), encoding="utf-8")

    command = [
        str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror",
        str(harness), "-o", str(executable), f"-I{FRONTEND}",
    ]
    if "mingw" in compiler.as_posix().lower():
        command.insert(5, "-static")
    compiled = subprocess.run(
        command, cwd=REPO_ROOT, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    )
    require(compiled.returncode == 0,
            f"native persistence harness did not compile:\n{compiled.stdout}")
    ran = subprocess.run(
        [str(executable)], cwd=REPO_ROOT, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    )
    require(ran.returncode == 0 and
            "ONEE PERSISTENT REBOOT NATIVE PASS" in ran.stdout,
            f"native persistence harness failed:\n{ran.stdout}")


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
            "onee_service_runtime_suspend_fn" in service_header and
            "onee_service_runtime_stop_fn" in service_header and
            "onee_service_runtime_running_fn" in service_header and
            "onee_service_bind_runtime" in service_header and
            "onee_runtime_start" in frontend and
            "return vtw_service_onee_start(" in frontend and
            "vtw_service_onee_suspend();" in frontend and
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
    test_onee_standard_keeps_global_callback_and_actions,
    test_global_persistent_action_is_not_profiled,
    test_global_persistence_is_synced_before_onee_request,
    test_disk2_boot_choice_is_independent_of_physical_slot6,
    test_only_manual_or_guarded_restore_can_start,
    test_start_refuses_unsafe_or_missing_pl,
    test_selected_request_has_no_software_expiry,
    test_native_latched_selection_lifecycle,
    test_native_persistent_reboot_lifecycle,
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
