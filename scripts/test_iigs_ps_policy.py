#!/usr/bin/env python3
"""Source checks for the fail-closed IIgs PS policy."""

from pathlib import Path


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

    require("BOOT_MENU_REG_IIGS_SLOT_CONFIG" in source and
            "BOOT_MENU_IIGS_SLOT_CONFIG_VALID" in source and
            "BOOT_MENU_MACHINE_ID_FAULT" in source and
            "BOOT_MENU_IIGS_SLOT_CONFIG_FAULT" in source,
            "PS must consume the boot ROM's read-only IIgs C02D report")
    require("boot_menu_service_host_bus_master_allowed" in source and
            "boot_menu_service_host_bus_master_allowed" in header,
            "one central API must decide whether host bus mastering is safe")
    allowed = between(source,
                      "uint8_t boot_menu_service_host_bus_master_allowed",
                      "uint8_t boot_menu_service_slot_allowed")
    require("CARD_MACHINE_MODE_IIPLUS" in allowed and
            "CARD_MACHINE_MODE_IIE" in allowed and
            "CARD_MACHINE_MODE_IIGS" not in allowed,
            "only II/II+ and IIe may use host bus masters")
    slot = between(source,
                   "uint8_t boot_menu_service_slot_allowed",
                   "const char *boot_menu_service_machine_name")
    require("CARD_MACHINE_MODE_UNKNOWN" in slot and
            "BOOT_MENU_IIGS_SLOT_CONFIG_VALID" in slot and
            "slot != 7U" in slot and
            "g_iigs_external_slot_mask & 0x80U" in slot,
            "IIgs must expose only physical slot 7 after valid boot ownership")
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
    require("boot_menu_service_slot_allowed" in apply_policy and
            "REG_WRITE(CARD_CTRL_SLOT_ENABLE_REG" in apply_policy,
            "the central slot writer must apply IIgs ownership before PL writes")
    require("control_onee_isolated()" in apply_policy and
            "effective = g_card_slot_enable_mask;" in apply_policy,
            "ONE//e must keep the requested logical slots on its private bus")
    slot5 = between(main,
                    "static void control_apply_slot5_service",
                    "static void control_set_slot5_processor")
    require("g_card_slot_effective_mask" in slot5 and
            "boot_menu_service_host_bus_master_allowed" in slot5 and
            "control_onee_isolated()" in slot5 and
            "ad8088_service_set_enabled(1U)" in slot5,
            "AD8088 may run only on a safe physical host or isolated ONE//e")
    require("control_refresh_machine_policy" in main and
            "boot_menu_service_iigs_slot_config" in main and
            "((uint16_t)onee_isolated << 15)" in main and
            "IIgs physical slot 7 eligible; logical slots 1-6 blocked" in main,
            "machine, C02D, and ONE//e changes must refresh the live policy")


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
    require("boot_menu_service_slot_allowed" in clock,
            "no-slot clock must only attach below allowed external ROMs")
    require("control_onee_isolated()" in clock and
            "g_card_slot_enable_mask & clock_slots" in clock,
            "no-slot clock must keep its configured ONE//e slots")
    require("boot_menu_service_slot_allowed(7U)" in supersprite,
            "SuperSprite must require external slot 7")
    require("control_onee_isolated()" in supersprite,
            "SuperSprite must remain available on the isolated ONE//e bus")
    require("boot_menu_service_slot_allowed(1U)" in ssc,
            "SSC must require external slot 1")
    require("control_onee_isolated()" in ssc,
            "SSC must remain available on the isolated ONE//e bus")


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


TESTS = [
    test_machine_policy_is_fail_closed,
    test_aux_and_machine_override_are_safe,
    test_requested_and_effective_card_state_are_separate,
    test_slot_features_follow_ownership,
    test_host_memory_writers_are_blocked,
    test_vtw_and_ad8088_recheck_policy_while_running,
    test_onee_bypass_requires_full_isolation,
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
