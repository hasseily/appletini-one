#!/usr/bin/env python3
"""Source checks for the fail-closed IIgs PS policy."""

import os
from pathlib import Path
import re
import subprocess
from tempfile import TemporaryDirectory

from test_joystick_config_menu import function
from test_onee_input_service import find_native_c_compiler


ROOT = Path(__file__).resolve().parents[1]
FRONTEND = ROOT / "ps_sources" / "frontend"


class TestFailure(AssertionError):
    pass


def read(name: str) -> str:
    return (FRONTEND / name).read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise TestFailure(message)


def between(source: str, start: str, end: str) -> str:
    first = source.find(start)
    last = source.find(end, first + len(start))
    require(first >= 0 and last > first, f"could not isolate {start}")
    return source[first:last]


def test_machine_policy_is_fail_closed() -> None:
    source = read("boot_menu_service.c")
    header = read("boot_menu_service.h")

    require("CARD_CTRL_IIGS_SLOT_MAP_REG" in source and
            "CARD_CTRL_IIGS_SLOT_MAP_VALID_BIT" in source and
            "BOOT_MENU_MACHINE_ID_FAULT" in source and
            "BOOT_MENU_IIGS_SLOT_CONFIG_FAULT" in source,
            "PS must use the live map and reject boot-report faults")
    require("boot_menu_service_host_bus_master_allowed" in header,
            "one central API must decide whether host bus mastering is safe")
    allowed = function(source, "boot_menu_service_host_bus_master_allowed")
    require("CARD_MACHINE_MODE_IIPLUS" in allowed and
            "CARD_MACHINE_MODE_IIE" in allowed and
            "g_machine_identity_fault == 0U" in allowed and
            "CARD_MACHINE_MODE_IIGS" not in allowed,
            "only II/II+ and IIe with valid identity may use host bus masters")
    observe = function(source, "machine_observe_status")
    require("CARD_CTRL_IIGS_PROFILE_ID_FAULT_BIT" in observe and
            "profile_conflict != 0U" in observe,
            "PS must reject conflicts between the physical profile and ROM ID")
    require("g_iigs_seen" in source and
            "machine: IIgs identity latched" in source,
            "an IIgs report must latch the strict policy")


def test_aux_and_machine_override_are_safe() -> None:
    source = read("boot_menu_service.c")
    uart = read("uart_control.c")

    aux = between(source,
                  "static void machine_refresh_aux_policy",
                  "static void machine_apply_mode")
    require("const uint8_t want" in aux and
            "boot_menu_service_host_bus_master_allowed() != 0U" in aux,
            "physical base aux must require the safe host policy")
    require("onee_service_isolation_confirmed() != 0U" in aux and
            "vtw_service_session_active() != 0U" in aux and
            "g_aux_card_present == 0U" in aux,
            "RamWorks may bypass host identity only on the isolated ONE//e bus")
    force = between(source,
                    "void boot_menu_service_force_machine_mode",
                    "uint8_t boot_menu_service_machine_forced")
    require("mode != (int)CARD_MACHINE_MODE_IIGS" in force,
            "the bench override must only make policy stricter")
    require("machine force iie: REFUSED" in uart and
            "machine force iiplus: REFUSED" in uart,
            "UART must refuse unsafe machine overrides")


def test_requested_and_effective_card_state_are_separate() -> None:
    main = read("main.c")

    require("g_card_slot_enable_mask" in main and
            "g_card_slot_effective_mask" in main and
            "card_control_apply_slot_policy" in main,
            "saved slot requests must be kept apart from the live GS mask")
    apply_policy = between(main,
                           "static void card_control_apply_slot_policy",
                           "static void card_control_write_slot_mask")
    require("boot_menu_service_slot_io_allowed" in apply_policy and
            "boot_menu_service_slot_rom_allowed" in apply_policy and
            "iigs_virtual_slots != 0U ? capable : effective" in apply_policy,
            "GS requested slots reach hardware while PS keeps separate effective status")
    require("control_onee_isolated()" in apply_policy and
            "effective = capable;" in apply_policy,
            "ONE//e must keep the requested logical slots on its private bus")
    slot5 = function(main, "control_apply_slot5_service")
    require("g_card_slot_effective_mask" in slot5 and
            "boot_menu_service_host_bus_master_allowed" in slot5 and
            "control_onee_isolated()" in slot5 and
            "control_iigs_virtual_slots_active()" in slot5,
            "private slot-5 modes survive remaps, while AD8088 still requires DMA permission")
    refresh = function(main, "control_refresh_machine_policy")
    require("boot_menu_service_iigs_slot_map" in refresh and
            "((uint32_t)onee_isolated << 15)" in refresh and
            "logical I/O" in refresh and "logical ROM" in refresh,
            "machine, live I/O and ROM mapping, and ONE//e changes refresh PS policy")


def test_slot_features_follow_ownership() -> None:
    main = read("main.c")

    clock = between(main,
                    "static void control_apply_clock_policy",
                    "static void control_set_clock_enabled")
    supersprite = between(main,
                          "static void control_apply_supersprite_policy",
                          "static void control_set_supersprite_enabled")
    ssc = between(main,
                  "static void control_apply_ssc_policy",
                  "static void control_set_ssc_enabled")
    for body, name in ((clock, "no-slot clock"), (supersprite, "SuperSprite"),
                       (ssc, "SSC")):
        require("control_onee_isolated()" in body and
                "control_iigs_virtual_slots_active()" in body,
                f"{name} must keep its hardware intent across valid remaps and on ONE//e")


def test_host_memory_writers_are_blocked() -> None:
    uart = read("uart_control.c")
    sdd = read("usb_sdd_service.c")
    main = read("main.c")

    peek = between(uart, "static int dma_peek_raw_read",
                   "static int dma_probe_write")
    probe = between(uart, "static int dma_probe_write",
                    "int uart_control_dma_bus_write")
    shared = between(uart, "int uart_control_dma_bus_write",
                     "static int parse_rtc_datetime")
    for body, name in ((peek, "peek"), (probe, "probe"), (shared, "shared write")):
        require("boot_menu_service_host_bus_master_allowed()" in body,
                f"DMA {name} must fail closed before touching its registers")
    require("dma host access: BLOCKED by machine policy" in uart,
            "UART commands must report the safety block")
    require("boot_menu_service_host_bus_master_allowed() == 0U" in sdd and
            "SddRamBlocked" in sdd,
            "USB SDD host-memory writes must be rejected and counted")
    require("boot_menu_service_host_bus_master_allowed() != 0U" in main and
            "uart_control_dma_bus_write(0xC029U, 0x01U)" in main,
            "the reset-time C029 write must require an allowed host bus master")


def test_vtw_and_ad8088_recheck_policy_while_running() -> None:
    vtw = read("vtw_service.c")
    ad8088 = read("ad8088_service.c")

    live = between(vtw, "static vtw_ctrl_live_result_t vtw_apply_ctrl_live",
                   "static uint8_t vtw_onee_isolation_confirmed")
    poll = between(vtw, "void vtw_service_poll(void)",
                   "void vtw_service_uart_status")
    require("boot_menu_service_host_bus_master_allowed() == 0U" in live and
            "REG_WRITE(CARD_CTRL_VTW_CTRL_REG, 0U)" in live,
            "live vTW updates must clear CTRL after a restricted identity")
    require("boot_menu_service_host_bus_master_allowed() == 0U" in poll and
            "vtw_session_stop(\"machine safety policy\")" in poll,
            "every host vTW state must stop when the policy turns strict")
    set_enabled = between(ad8088, "void ad8088_service_set_enabled",
                          "uint8_t ad8088_service_is_enabled")
    ad_poll = between(ad8088, "void ad8088_service_poll(void)",
                      "void ad8088_service_uart_status")
    require("boot_menu_service_host_bus_master_allowed() == 0U" in set_enabled and
            "onee_service_isolation_confirmed() == 0U" in set_enabled and
            "AD8088_CONTROL_CANCEL_BUS" in set_enabled,
            "AD8088 enable must fail closed outside isolated ONE//e")
    require("boot_menu_service_host_bus_master_allowed() == 0U" in ad_poll and
            "onee_service_isolation_confirmed() == 0U" in ad_poll and
            "ad8088_service_set_enabled(0U)" in ad_poll,
            "AD8088 must stop if neither safe host nor ONE//e owns its bus")


def test_onee_bypass_requires_full_isolation() -> None:
    onee = read("onee_service.c")
    header = read("onee_service.h")

    confirmed = between(onee,
                        "uint8_t onee_service_isolation_confirmed",
                        "\n}\n")
    for bit in (
            "CARD_CTRL_ONEE_STATUS_REQUEST_BIT",
            "CARD_CTRL_ONEE_STATUS_EFFECTIVE_BIT",
            "CARD_CTRL_ONEE_STATUS_ISOLATED_BIT",
            "CARD_CTRL_ONEE_STATUS_SELECTED_BIT",
            "CARD_CTRL_ONEE_STATUS_HDL_PRESENT_BIT"):
        require(bit in confirmed, f"ONE//e safe path must require {bit}")
    require("CARD_CTRL_ONEE_STATUS_OUTPUTS_OFF_BIT" in confirmed and
            "CARD_CTRL_ONEE_STATUS_ACTIVITY_BIT" in confirmed and
            "CARD_CTRL_ONEE_STATUS_LOCKOUT_BIT" in confirmed and
            "CARD_CTRL_ONEE_STATUS_APPLE_POWER_BIT" in confirmed and
            "onee_status_pl_ready(g_status) != 0U" in confirmed and
            "onee_inhibit_reason(g_status) == CARD_CTRL_ONEE_INHIBIT_NONE" in confirmed,
            "ONE//e bypass must reject faults, hazards, and invalid PL status")
    require("onee_service_isolation_confirmed" in header,
            "ONE//e must publish one shared isolation decision")


def test_profile_is_a_working_setting_applied_before_reset_release() -> None:
    menu = read("config_menu.c")
    main = read("main.c")
    load = function(menu, "config_menu_load_settings")
    require('menu->iigs_slot3_profile = 0U;' in load and
            'strcmp(key, "host.iigs.slot3") == 0' in load and
            'menu->iigs_slot3_profile = config_menu_bool_text(value)' in load,
            "each working config must opt in to physical slot 3")
    parse_profile = function(menu, "config_menu_read_settings_from_path")
    require("iigs_slot3_profile" not in parse_profile and
            "host.iigs.slot3" not in function(menu, "config_menu_parse_key_value"),
            "portable profiles cannot change the hardware installation setting")
    save = function(menu, "config_menu_save_settings_to_path")
    require('if (strcmp(path, APPLETINI_CFG_PATH) == 0) {\n'
            '        APPEND_CFG("host.iigs.slot3=' in save,
            "only the working settings file stores the physical slot profile")
    startup = function(main, "main")
    retry = startup.index("config_menu_retry_settings_if_needed(&config_menu)")
    gate = function(main, "control_prepare_iigs_slot3")
    ready = startup.index("if (control_prepare_iigs_slot3(&config_menu))")
    release = startup.index("card_control_mark_cpu0_ready();")
    require(retry < ready < release and
            startup.count("card_control_mark_cpu0_ready();") == 1 and
            main.count("REG_WRITE(CARD_CTRL_IIGS_PROFILE_REG") == 1 and
            "menu->settings_loaded != 0U && menu->iigs_slot3_profile != 0U" in gate,
            "the profile must only be written after saved settings load and before RESET release")


def test_native_slot_policy() -> None:
    compiler = Path(os.environ["CC"]) if "CC" in os.environ else find_native_c_compiler()
    require(compiler is not None, "set CC to a native C compiler to run the policy test")
    boot = read("boot_menu_service.c")
    main = read("main.c")
    definitions = "\n".join(re.findall(
        r"^#define BOOT_MENU_(?:MMIO_BASE|REG_IIGS_SLOT_CONFIG|MACHINE_ID_FAULT|IIGS_SLOT_CONFIG_FAULT) +.*$",
        boot, re.M))
    code = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "card_control_regs.h"
#define CONFIG_SLOT5_PROCESSOR_Z80 0U
#define CONFIG_SLOT5_PROCESSOR_AD8088 1U
#define CONFIG_SLOT5_PROCESSOR_TRANSPORTER 2U
#define UART0_BASE 0U
#define CARD_CTRL_AUX_PROBE_REG CARD_CTRL_REG_ADDR(0x6AU)
"""
    code += definitions + r"""

static uint8_t g_machine_mode_applied = CARD_MACHINE_MODE_UNKNOWN;
static uint8_t g_machine_identity_fault;
static uint8_t g_iigs_policy_fault_seen;
static uint8_t g_machine_id_raw, g_iigs_seen, g_machine_forced;
static uint8_t g_aux_provide_applied, g_aux_card_present, g_aux_probe_seen;
static uint32_t g_iigs_slot_map;
static uint8_t g_slot5_processor;
static uint32_t g_card_slot_enable_mask;
static uint32_t g_card_slot_effective_mask;
static uint32_t g_host_policy_snapshot = UINT32_MAX;
static uint8_t g_clock_requested, g_ssc_requested, g_supersprite_requested;
static uint8_t z80, ad8088, pct, clock_enabled, disk2_enabled, isolated;
typedef struct {
    uint8_t settings_loaded;
    uint8_t iigs_slot3_profile;
} config_menu_t;
static uint32_t host_map, host_fault, host_profile, slot_enable_reg, feature_reg;
static uint32_t reset_release;
static unsigned profile_writes, boot_preparations, menu_warnings;
static unsigned rtc_updates;
static unsigned z80_changes, pct_changes;
static int g_rtc;

static uint32_t REG_READ(uint32_t reg)
{
    switch (reg) {
    case CARD_CTRL_IIGS_SLOT_MAP_REG: return host_map;
    case CARD_CTRL_IIGS_PROFILE_REG: return host_profile;
    case CARD_CTRL_AUX_PROBE_REG: return 0U;
    case BOOT_MENU_REG_IIGS_SLOT_CONFIG: return host_fault;
    case CARD_CTRL_FEATURE_ENABLE_REG: return feature_reg;
    default: assert(false); return 0;
    }
}

static void REG_WRITE(uint32_t reg, uint32_t value)
{
    switch (reg) {
    case CARD_CTRL_SLOT_ENABLE_REG: slot_enable_reg = value; return;
    case CARD_CTRL_IIGS_PROFILE_REG:
        assert(value == CARD_CTRL_IIGS_PROFILE_SLOT3_BIT);
        ++profile_writes;
        return;
    case RESET_RELEASE_REG: reset_release = value; return;
    case CARD_CTRL_FEATURE_ENABLE_REG: feature_reg = value; return;
    default: assert(false); return;
    }
}

static void uart_puts(uint32_t uart, const char *text) { (void)uart; (void)text; }
static void uart_putdec(uint32_t uart, uint32_t value) { (void)uart; (void)value; }
static void uart_puthex(uint32_t uart, uint32_t value) { (void)uart; (void)value; }
static void machine_apply_mode(uint8_t mode) { g_machine_mode_applied = mode; }
static void machine_refresh_aux_policy(void) {}
static void machine_refresh_boot_rom_policy(void) {}
static void config_menu_set_status(config_menu_t *menu, uint8_t warning, const char *text)
{
    assert(menu != NULL && warning && text != NULL);
    ++menu_warnings;
}
static void vtw_service_prepare_boot_start(void) { ++boot_preparations; }
static uint8_t vtw_service_session_active(void) { return 0U; }
static uint8_t control_onee_isolated(void) { return isolated; }
static uint32_t http_service_filter_slot_mask(uint32_t mask) { return mask; }
static void no_slot_clock_control_set_enabled(uint8_t enable) { clock_enabled = enable; }
static void no_slot_clock_control_publish_rtc(const int *rtc) { (void)rtc; ++rtc_updates; }
static uint8_t applicard_service_is_enabled(void) { return z80; }
static void applicard_service_set_enabled(uint8_t enable) { ++z80_changes; z80 = enable; }
static uint8_t ad8088_service_is_enabled(void) { return ad8088; }
static void ad8088_service_set_enabled(uint8_t enable) { ad8088 = enable; }
static void pc_transporter_service_set_enabled(bool enable)
{
    if (pct != enable) {
        ++pct_changes;
        pct = enable;
    }
}
static void vtw_service_set_disk2_config_enabled(uint8_t enable) { disk2_enabled = enable; }
"""
    for name in ("machine_mode_from_id", "machine_refresh_iigs_slot_policy",
                 "machine_observe_status", "boot_menu_service_machine_mode",
                 "boot_menu_service_host_bus_master_allowed", "machine_slot_allowed",
                 "boot_menu_service_slot_io_allowed", "boot_menu_service_slot_rom_allowed",
                 "boot_menu_service_iigs_slot_map"):
        code += function(boot, name)
    for name in ("control_prepare_iigs_slot3", "card_control_mark_cpu0_ready",
                 "control_iigs_virtual_slots_active", "card_control_apply_slot_policy",
                 "control_apply_slot5_service", "control_apply_disk2_policy",
                 "control_apply_clock_policy", "control_apply_supersprite_policy",
                 "control_apply_ssc_policy", "control_refresh_machine_policy"):
        code += function(main, name)
    startup = function(main, "main")
    startup_gate = between(startup, "    if (control_prepare_iigs_slot3(&config_menu))",
                           "    config_menu_start_boot_dhcp(&config_menu);")
    code += "\nstatic void startup_release(config_menu_t config_menu) {\n" + startup_gate + "}\n"
    code += r"""
static void apply(void)
{
    machine_refresh_iigs_slot_policy();
    card_control_apply_slot_policy();
    control_apply_slot5_service((g_card_slot_enable_mask & (1U << 5)) != 0U);
    control_apply_disk2_policy();
    control_apply_clock_policy();
    control_apply_supersprite_policy();
    control_apply_ssc_policy();
}

int main(void)
{
    config_menu_t menu = {1U, 0U};
    startup_release(menu);
    assert(reset_release == RESET_RELEASE_CPU0_READY_BIT);
    assert(!profile_writes && !menu_warnings && boot_preparations == 1U);

    reset_release = 0U;
    menu.iigs_slot3_profile = 1U;
    startup_release(menu);
    assert(!reset_release && profile_writes == 1U);
    assert(menu_warnings == 1U && boot_preparations == 1U);

    host_profile = UINT32_MAX;
    startup_release(menu);
    assert(!reset_release && profile_writes == 2U);
    assert(menu_warnings == 2U && boot_preparations == 1U);

    host_profile = CARD_CTRL_IIGS_PROFILE_SLOT3_BIT;
    startup_release(menu);
    assert(reset_release == RESET_RELEASE_CPU0_READY_BIT && profile_writes == 3U);
    assert(menu_warnings == 2U && boot_preparations == 2U);
    host_profile = 0U;

    const uint32_t requested = 0xF6U;
    const uint32_t features = CARD_CTRL_FEATURE_SUPERSPRITE_ENABLE_BIT |
                              CARD_CTRL_FEATURE_SSC_ENABLE_BIT;
    g_card_slot_enable_mask = requested;
    g_clock_requested = g_ssc_requested = g_supersprite_requested = 1U;
    apply();
    assert(slot_enable_reg == 0x80U && g_card_slot_effective_mask == 0U);
    assert(!z80 && !pct && !clock_enabled && !disk2_enabled && !feature_reg);
    assert(!boot_menu_service_host_bus_master_allowed());

    g_machine_mode_applied = CARD_MACHINE_MODE_IIGS;
    host_map = 0x808001F6U;
    apply();
    assert(slot_enable_reg == 0x80U && g_card_slot_effective_mask == 0x80U);
    assert(!z80 && !pct && !disk2_enabled);
    assert(clock_enabled && feature_reg == CARD_CTRL_FEATURE_SUPERSPRITE_ENABLE_BIT);
    rtc_updates = 0U;

    host_profile = CARD_CTRL_IIGS_PROFILE_SLOT3_BIT;
    host_map = 0xF6F601F6U;
    apply();
    assert(slot_enable_reg == requested && g_card_slot_effective_mask == requested);
    assert(z80 && !ad8088 && !pct && z80_changes == 1U);
    assert(clock_enabled && disk2_enabled && feature_reg == features && rtc_updates == 1U);
    assert(!boot_menu_service_host_bus_master_allowed());

    control_refresh_machine_policy();
    const unsigned rtc_updates_before_remap = rtc_updates;

    /* I/O and ROM can disagree. Then let the GS map every slot internal. */
    host_map = 0x00800180U;
    machine_refresh_iigs_slot_policy();
    control_refresh_machine_policy();
    assert(g_card_slot_effective_mask == 0x80U);
    assert(boot_menu_service_slot_io_allowed(7U) && !boot_menu_service_slot_rom_allowed(7U));
    host_map = 0x00000180U;
    machine_refresh_iigs_slot_policy();
    control_refresh_machine_policy();
    assert(g_card_slot_effective_mask == 0U && slot_enable_reg == requested);
    assert(z80 && z80_changes == 1U && clock_enabled && disk2_enabled && feature_reg == features);
    assert(rtc_updates == rtc_updates_before_remap);

    g_slot5_processor = CONFIG_SLOT5_PROCESSOR_TRANSPORTER;
    apply();
    assert(!z80 && pct && pct_changes == 1U);
    host_map = 0xF6F601F6U;
    apply();
    assert(pct && pct_changes == 1U && !ad8088);
    g_slot5_processor = CONFIG_SLOT5_PROCESSOR_AD8088;
    apply();
    assert(!pct && !ad8088 && slot_enable_reg == (requested & ~(1U << 5)));

    g_slot5_processor = CONFIG_SLOT5_PROCESSOR_TRANSPORTER;
    apply();
    assert(pct);
    host_fault = BOOT_MENU_MACHINE_ID_FAULT;
    apply();
    assert(!pct && !clock_enabled && !disk2_enabled && !feature_reg);
    assert(!g_card_slot_effective_mask && g_card_slot_enable_mask == requested);

    host_fault = 0U;
    g_machine_mode_applied = CARD_MACHINE_MODE_UNKNOWN;
    machine_observe_status(3U << 12);
    assert(g_machine_identity_fault && g_machine_mode_applied == CARD_MACHINE_MODE_UNKNOWN);
    assert(!boot_menu_service_host_bus_master_allowed());

    g_machine_identity_fault = 0U;
    g_machine_id_raw = 0U;
    machine_observe_status(4U << 12);
    assert(g_iigs_seen && g_machine_mode_applied == CARD_MACHINE_MODE_IIGS);
    assert(boot_menu_service_iigs_slot_map() == host_map);
    host_profile |= CARD_CTRL_IIGS_PROFILE_ID_FAULT_BIT;
    machine_observe_status(4U << 12);
    assert(g_machine_identity_fault && !boot_menu_service_iigs_slot_map());

    g_slot5_processor = CONFIG_SLOT5_PROCESSOR_AD8088;
    isolated = 1U;
    apply();
    assert(ad8088 && slot_enable_reg == requested);
    puts("GS ownership, remaps, slot 5, host faults, and isolated ONE//e");
    return 0;
}
"""
    with TemporaryDirectory() as tmp:
        source = Path(tmp) / "iigs_policy.c"
        executable = Path(tmp) / "iigs_policy.exe"
        source.write_text(code, encoding="utf-8")
        env = dict(os.environ, PATH=str(compiler.parent) + os.pathsep + os.environ.get("PATH", ""))
        subprocess.run([str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-I", str(FRONTEND), str(source), "-o", str(executable)], env=env, check=True)
        subprocess.run([str(executable)], env=env, check=True)


TESTS = [
    test_machine_policy_is_fail_closed,
    test_aux_and_machine_override_are_safe,
    test_requested_and_effective_card_state_are_separate,
    test_slot_features_follow_ownership,
    test_host_memory_writers_are_blocked,
    test_vtw_and_ad8088_recheck_policy_while_running,
    test_onee_bypass_requires_full_isolation,
    test_profile_is_a_working_setting_applied_before_reset_release,
    test_native_slot_policy,
]


def main() -> int:
    failed = 0
    for test in TESTS:
        try:
            test()
            print(f"PASS {test.__name__}")
        except TestFailure as exc:
            failed += 1
            print(f"FAIL {test.__name__}: {exc}")
    print(f"\n{len(TESTS) - failed}/{len(TESTS)} tests passed")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
