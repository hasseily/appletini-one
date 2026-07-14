#!/usr/bin/env python3
"""Source-level regression tests for the virtual RamWorks aux card size."""

from __future__ import annotations

from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]


class TestFailure(Exception):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise TestFailure(message)


def read(rel_path: str) -> str:
    return (REPO_ROOT / rel_path).read_text(encoding="utf-8")


def test_ramworks_bank_select_reaches_8192k() -> None:
    globals_sv = read("hdl/globals.sv")
    softswitch = read("hdl/apple/soft_switch_manager.sv")
    smartport_card = read("hdl/apple/smartport_card.sv")
    apple_top = read("hdl/apple/apple_top.sv")
    regs = read("ps_sources/frontend/card_control_regs.h")
    smartport_service = read("ps_sources/frontend/smartport_service.c")
    menu_tabs = read("ps_sources/frontend/config_menu_device_tabs.c")

    require("logic [6:0] sw_ramworks_bank;" in globals_sv,
            "SoftSwitchState must carry 7 RamWorks bank-select bits")
    require("logic [6:0]  ss_ramworks_bank;" in softswitch,
            "soft-switch manager must store bank numbers 0..127")
    require("ab_read.data[7] == 1'b0" in softswitch and
            "ss_ramworks_bank <= ab_read.data[6:0];" in softswitch,
            "C071/C073 must accept RamWorks bank writes 0..127")
    require("q_aux_bank_full = {1'b0, ss_ramworks_bank} + 8'd1;" in softswitch and
            "logic [7:0]  q_bank_sel;" in softswitch and
            "q_psram_addr[23:16] = q_bank_sel;" in softswitch,
            "address translation must map bank 128 into PSRAM address bit 23")
    require("logic [20:0] sss_snapshot_q;" in smartport_card and
            "sss.sw_ramworks_bank," in smartport_card and
            "as_client.rdata = {11'h0, sss_snapshot_q};" in smartport_card,
            "SmartPort soft-switch snapshots must carry the seventh bank bit")
    require("wire [20:0] current_softswitch_state" in apple_top and
            "CARD_CTRL_REG_SOFTSW_STATE:        as_client_rdata_q <= {11'h000, current_softswitch_state};" in apple_top,
            "card-control soft-switch state register must expose 21 bits")

    require("#define CARD_CTRL_SOFTSW_STATE_MASK           0x001FFFFFUL" in regs and
            "#define CARD_CTRL_SOFTSW_RAMWORKS_BANK_MASK   0x7FU" in regs,
            "PS-side card-control masks must match the 7-bit bank field")
    require("#define SP_RAMDISK_BASE          0x30000000U" in smartport_service and
            "#define SP_RAMDISK_BLOCKS        65535U" in smartport_service and
            "#define SP_RAMDISK_BITMAP_BLOCKS 16U" in smartport_service and
            "0x30000000-0x31FFFFFF is reserved DDR" in smartport_service and
            "smartport: RAM32 32MB ram disk mounted" in smartport_service,
            "SmartPort RAM disk must expose the 32 MB physical-CPU storage path")
    require("RAM32: 32MB volatile ram disk" in menu_tabs,
            "RAM device menu text must advertise the 32 MB size")


TESTS = [
    test_ramworks_bank_select_reaches_8192k,
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
        print(f"{len(TESTS) - len(failures)} of {len(TESTS)} RamWorks tests passed; "
              f"{len(failures)} failed")
        return 1
    print(f"{len(TESTS)} RamWorks tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
