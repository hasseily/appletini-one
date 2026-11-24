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


def test_ramworks_bank_select_reaches_16mb() -> None:
    globals_sv = read("hdl/globals.sv")
    softswitch = read("hdl/apple/soft_switch_manager.sv")
    smartport_card = read("hdl/apple/smartport_card.sv")
    apple_top = read("hdl/apple/apple_top.sv")
    regs = read("ps_sources/frontend/card_control_regs.h")
    smartport_service = read("ps_sources/frontend/smartport_service.c")
    menu_tabs = read("ps_sources/frontend/config_menu_device_tabs.c")

    psram_driver = read("hdl/apple/psram_driver.sv")
    require(globals_sv.count("logic [7:0] sw_ramworks_bank;") == 2,
            "SoftSwitchState and TranslateState must carry 8 bank-select bits")
    require("logic [7:0]  ss_ramworks_bank;" in softswitch,
            "soft-switch manager must store bank numbers 0..254")
    require("(ab_read.data[7] == 1'b0 ||" in softswitch and
            "(ramworks_16mb && ab_read.data != 8'hFF)))" in softswitch and
            "ss_ramworks_bank <= ab_read.data;" in softswitch,
            "C071/C073 must accept 0..127, and 128..254 only in 16 MB mode")
    # One line is 4 bytes on each chip, so the chip address is half of
    # the byte address. Without this, half of each chip is never used.
    require("wire [23:0] chip_addr = {1'b0, addr_q[23:1]};" in psram_driver and
            psram_driver.count("{cmd_q, chip_addr, ") == 4 and
            "{cmd_q, addr_q, " not in psram_driver,
            "PSRAM driver must use all 8 MB of each chip")
    vtw_core = read("hdl/apple/vtw_core_top.sv")
    # Every switch of the translation state must invalidate the TURBO cache.
    # A fixed-width copy lost 80STORE when the bank register grew.
    require("TranslateState turbo_mapping;" in vtw_core and
            "wire [18:0] turbo_mapping" not in vtw_core and
            "(turbo_mapping != turbo_mapping_q)" in vtw_core,
            "the TURBO cache must compare the complete translation state")
    require("ramworks_16mb_q <= as_common.wdata[1];" in apple_top and
            apple_top.count(".ramworks_16mb(ramworks_16mb_q),") == 2,
            "register 0x62 bit 1 must select 16 MB for both switch managers")
    # translate_apple_addr moved to globals.sv (shared with the virtual
    # TransWarp's private switch copy); the manager feeds it xlate_st.
    require("q_aux_bank_full = st.sw_ramworks_bank + 8'd1;" in globals_sv and
            "logic [7:0]  q_bank_sel;" in globals_sv and
            "q_psram_addr[23:16] = q_bank_sel;" in globals_sv,
            "address translation must map banks 128..255 into PSRAM address bit 23")
    require("xlate_st.sw_ramworks_bank = ss_ramworks_bank;" in softswitch,
            "soft-switch manager must feed its bank register to the shared translator")
    require("logic [22:0] sss_snapshot_q;" in smartport_card and
            "sss.sw_ramworks_bank," in smartport_card and
            "as_client.rdata = {9'h0, sss_snapshot_q};" in smartport_card,
            "SmartPort soft-switch snapshots must carry the eighth bank bit")
    require("wire [21:0] current_softswitch_state" in apple_top and
            "CARD_CTRL_REG_SOFTSW_STATE:        as_client_rdata_q <= {10'h000, current_softswitch_state};" in apple_top,
            "card-control soft-switch state register must expose 22 bits")

    require("#define CARD_CTRL_SOFTSW_STATE_MASK           0x003FFFFFUL" in regs and
            "#define CARD_CTRL_SOFTSW_RAMWORKS_BANK_MASK   0xFFU" in regs,
            "PS-side card-control masks must match the 8-bit bank field")
    require("#define SP_SSS_RAMWORKS_MASK    0xFFUL" in smartport_service and
            "#define SP_SSS_VTW_WIDE_BIT     (1UL << 22)" in smartport_service,
            "SmartPort service must decode the 23-bit snapshot")
    require("#define SP_RAMDISK_BASE          0x30000000U" in smartport_service and
            "#define SP_RAMDISK_BLOCKS        65535U" in smartport_service and
            "#define SP_RAMDISK_BITMAP_BLOCKS 16U" in smartport_service and
            "0x30000000-0x31FFFFFF is reserved DDR" in smartport_service and
            "smartport: RAM32 32MB ram disk mounted" in smartport_service,
            "SmartPort RAM disk must expose the 32 MB physical-CPU storage path")
    require("RAM32: 32MB volatile ram disk" in menu_tabs,
            "RAM device menu text must advertise the 32 MB size")


TESTS = [
    test_ramworks_bank_select_reaches_16mb,
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
