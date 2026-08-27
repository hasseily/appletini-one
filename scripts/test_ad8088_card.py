#!/usr/bin/env python3
"""Source and protocol regressions for the ALF AD8088 Plus personality.

This test is intentionally independent of Vivado/Vitis. The instruction-core
smoke test lives in test_ad8088_cpu.c and is built with a host C compiler.
"""

from __future__ import annotations

import math
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def text(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


class Mailbox:
    """Small reference model of the documented port-0 flag handshake."""

    def __init__(self) -> None:
        self.ports = [0] * 16
        self.flag = True
        self.pending = False
        self.running = False
        self.abort = False
        self.seq = 0

    def apple_write(self, port: int, value: int) -> None:
        port &= 15
        value &= 255
        self.ports[port] = value
        if port != 0:
            return
        if self.running:
            if self.flag:
                self.flag = False
            elif value == 0:
                self.abort = True
        else:
            self.flag = False
            self.pending = True
            self.seq = (self.seq + 1) & 255

    def finish(self, seq: int) -> None:
        if seq == self.seq:
            self.pending = False
        self.flag = True


def test_mailbox() -> None:
    mb = Mailbox()
    require(mb.flag, "monitor must power up ready")
    mb.apple_write(12, 0x34)
    mb.apple_write(13, 0x12)
    mb.apple_write(0, 29)
    require(mb.pending and not mb.flag and mb.seq == 1,
            "port-0 command must lower ready and post one sequence")
    mb.finish(2)
    require(mb.pending, "stale command ACK must not consume a new command")
    mb.finish(1)
    require(not mb.pending and mb.flag, "matching completion must raise ready")

    mb.running = True
    mb.flag = True
    mb.apple_write(0, 0x55)
    require(not mb.flag and not mb.abort,
            "Apple ACK of an 8088 signal must only clear the flag")
    mb.flag = True
    mb.apple_write(0, 0)
    require(not mb.abort and not mb.flag,
            "zero must acknowledge a pending 8088 signal before aborting")
    mb.apple_write(0, 0)
    require(mb.abort,
            "zero with no pending signal must stop the running program")


def test_float_format() -> None:
    # Manual example for pi: little-endian signed mantissa $6487ED51 and
    # exponent $82. Value = mantissa * 2^(exponent-159).
    raw = bytes((0x51, 0xED, 0x87, 0x64, 0x82))
    mantissa = int.from_bytes(raw[:4], "little", signed=True)
    value = math.ldexp(float(mantissa), raw[4] - 159)
    require(abs(value - math.pi) < 1.0e-8,
            "documented AD8088 floating-point scale is wrong")
    require(math.ldexp(123, 0x9F - 159) == 123.0,
            "$9F integer exponent contract is wrong")


def test_hdl() -> None:
    card = text("hdl/apple/applicard_card.sv")
    top = text("hdl/apple/apple_top.sv")
    bench = text("hdl/sim/tb_applicard_card.sv")
    bus_bench = text("hdl/sim/tb_applicard_bus_master.sv")

    for needle in (
        "MODE_AD8088",
        "AXI_REG_AD_STATUS",
        "AXI_REG_AD_PORTS3",
        "AXI_REG_BUS_COMMAND",
        "AXI_REG_BUS_BUFFER0",
        "BUS_DMA_GRACE",
        "BUS_BULK_NEXT",
        "BUS_RELEASE_PARK",
        "BUS_RELEASE_GUARD",
        "wr_dma_data_en",
        "disk2_timing_active || vtw_enabled || vtw_bus_owned",
        "ad_cmd_pending_q",
        "ad_abort_req_q",
    ):
        require(needle in card, f"HDL contract missing: {needle}")
    require("(io_idx == 4'h0) ? {ad_flag_q, 7'b0} : 8'hFF" in card,
            "only AD8088 port 0 may be readable")
    require("BUS_PARK_ADDR = 16'h0200" in card and
            "BUS_RECOVERY_CYCLES = 2'd2" in card and
            "bus_recovery_q" in card and
            "wire bus_master_owns" in card and
            "!bus_master_owns" in card,
            "sparse DMA must park owned cycles, give the CPU recovery time, "
            "and suppress ghost slot I/O")
    require(".card_enabled(card_slot5_enable)" in top,
            "AD8088 front end must be disabled with slot 5")
    require(".disk2_timing_active(disk2_sound_spinning)" in top,
            "sparse DMA must be blocked during Disk II timing")
    require(".vtw_bus_owned(vtw_bus_owned)" in top,
            "AD8088 sparse DMA must not contend with vTW")
    require(".vtw_enabled(vtw_enable_eff || onee_enable_effective)" in top,
            "AD8088 sparse DMA must block before vTW or ONE//e bus ownership")
    require("as_common.wdata[7]" in card and "AD8088_CONTROL_CANCEL_BUS" in
            text("ps_sources/frontend/ad8088_service.c"),
            "timed-out sparse DMA must have an explicit cancel path")
    require("AD8088 command event/status" in bench and
            "One sparse Apple-memory read" in bench and
            "busy monitor accepted overlapping command" in bench,
            "AD8088 HDL regression coverage is missing")
    require("DMA write missed the 210 ns CAS deadline" in bus_bench and
            "DMA write began before 155 ns" in bus_bench and
            "DMA release invalid" in bus_bench and
            "bulk read used" in bus_bench and
            "bulk write used" in bus_bench and
            "DMA held 6502" in bus_bench and
            "owned bus residue posted ghost mailbox command" in bus_bench and
            "DMA recovery gap only gave the CPU" in bus_bench and
            "acquisition park missing" in bus_bench and
            "release park missing" in bus_bench and
            "Reproduce Reboot Camp's full padded IO.SYS + MSDOS.SYS" in
            bus_bench and
            "cancel left Apple bus pins driven" in bus_bench,
            "AD8088 pin-level DMA protocol coverage is missing")


def test_machine_and_monitor() -> None:
    machine_h = text("ps_sources/frontend/ad8088_machine.h")
    machine_c = text("ps_sources/frontend/ad8088_machine.c")
    service = text("ps_sources/frontend/ad8088_service.c")
    service_h = text("ps_sources/frontend/ad8088_service.h")
    regs = text("ps_sources/frontend/applicard_regs.h")
    card = text("hdl/apple/applicard_card.sv")
    cpuconf = text("third_party/xtulator_cpu/cpuconf.h")
    cpu_h = text("third_party/xtulator_cpu/cpu.h")
    license_text = text("third_party/xtulator_cpu/LICENSE.txt")

    for needle in (
        "AD8088_BASE_RAM_END          0x00010000U",
        "AD8088_APPLE_WINDOW_BASE     0x00010000U",
        "AD8088_APPLE_WINDOW_END      0x00020000U",
        "AD8088_PLUS_RAM_BASE         0x00020000U",
        "AD8088_PLUS_RAM_END          0x00030000U",
        "AD8088_PLUS_RAM_HIGH_BASE    0x00040000U",
        "AD8088_PLUS_RAM_HIGH_END     0x000C0000U",
        "AD8088_AD128K_BASE           0x00040000U",
        "AD8088_AD128K_END            0x00060000U",
    ):
        require(needle in machine_h, f"memory map missing: {needle}")
    require("writes to absent/reserved memory are ignored" in machine_c,
            "reserved-memory behavior must be explicit")
    require("AD8088_RETURN_LINEAR" in machine_c and "0xF4U" in machine_c,
            "clean-room far-return sentinel is missing")
    selectable = {"#define CPU_8086", "#define CPU_186",
                  "#define CPU_V20", "#define CPU_286"}
    active_cpu_defs = [line.strip() for line in cpuconf.splitlines()
                       if line.strip() in selectable and
                       not line.startswith("//")]
    require(active_cpu_defs == ["#define CPU_8086"],
            "core must expose the original 8088 instruction set")
    require("void *opaque;" in cpu_h,
            "CPU callbacks must bind to the AD8088 machine instance")
    require("AD8088_RAM_BASE                 0x3F800000U" in service_h,
            "AD8088 must not alias the Z80 card's 2 MB DDR arena")
    require("GNU GPL VERSION 2" in license_text.upper() and
            "OR LATER" in license_text.upper(),
            "vendored CPU license is missing")

    require("command >= 29U && command <= 32U" in service,
            "integer PROM commands 29-32 are not dispatched")
    require("command >= 33U && command <= 47U" in service,
            "floating-point PROM commands 33-47 are not dispatched")
    for command, name in ((251, "SEQUENCE"), (252, "RANDOM"),
                          (253, "SET MEMORY"), (254, "MOVE DATA"),
                          (255, "CALL")):
        require(f"command == {command}U" in service and name in service,
                f"PROM command {command} ({name}) is missing")
    require("command >= 48U && command <= 247U" in service and
            "ad8088_start_user_command" in service,
            "user command table dispatch is missing")
    require("Graphics and MET-reserved commands are accepted as no-ops" in service,
            "unsupported original-ROM command scope must stay explicit")
    require("ad8088_pl_bus_bulk" in service and
            "AD8088_BUS_COMMAND_BULK" in service and
            "AD8088_BUS_STATUS_SEQ" in service,
            "bulk DMA must use a sequence-matched hardware transaction")
    require("AD8088_BUS_BUFFER_BYTES       4U" in regs and
            "as_common.wdata[23:16] > 8'd3" in card,
            "PL and PS must cap DMA holds at four Apple bytes")
    require("ad8088_async_fail" in service and
            "AD_MONITOR_RECOVER" in service and
            "ad8088_poll_recover" in service and
            "AD8088_CONTROL_SET_FLAG" in service,
            "bulk failure must cancel safely and restore READY")
    require("AD8088_XFER_BLOCKED" in service and
            "if (rc == AD8088_XFER_BLOCKED) return rc;" in service and
            "if (rc == AD8088_XFER_BLOCKED) return;" in service,
            "blocked commands must remain pending for a later retry")
    require("AD8088_BULK_POLL_BUDGET_US" in service and
            "for (;;)" in service and
            "now - start" in service,
            "bulk service must run repeated safe bursts within a time budget")


def test_ui_and_build() -> None:
    config = text("ps_sources/frontend/config_menu.c")
    config_h = text("ps_sources/frontend/config_menu.h")
    tabs = text("ps_sources/frontend/config_menu_device_tabs.c")
    main_c = text("ps_sources/frontend/main.c")
    vitis = text("scripts/create_vitis_workspace.py")

    require("#define APPLETINI_CFG_VERSION 116U" in config,
            "slot-5 personality must remain in the current config schema")
    require('"slot5.processor"' in config and
            '"slot5.processor=%s' in config,
            "slot-5 personality must load and save")
    require("CONFIG_SLOT5_PROCESSOR_Z80" in config_h and
            "CONFIG_SLOT5_PROCESSOR_AD8088" in config_h,
            "slot-5 selector enum missing")
    require('"ALF AD8088 Plus (640K)"' in tabs and '"Z80 Appli-Card"' in tabs,
            "UI must offer both mutually exclusive slot-5 cards")
    require("control_apply_slot5_service" in main_c and
            "applicard_service_set_enabled(0U)" in main_c and
            "ad8088_service_set_enabled(0U)" in main_c,
            "personality switch must disarm both CPU services first")
    require("config_menu_ad8088_active" in config and
            "TURN TRANSWARP OFF BEFORE ENABLING AD8088" in config and
            "DISABLE SLOT 5 AD8088 BEFORE TRANSWARP" in config,
            "config must prevent two Apple-bus masters from running together")
    for needle in (
        "ps_sources/frontend/ad8088_machine.c",
        "ps_sources/frontend/ad8088_service.c",
        "third_party/xtulator_cpu/cpu.c",
        "third_party/xtulator_cpu",
    ):
        require(needle in vitis, f"Vitis registration missing: {needle}")


def test_hardware_test_disk_sources() -> None:
    source = text("software/ad8088_test.a65")
    builder = text("scripts/build_ad8088_test_disks.py")
    readme = text("README_AD8088.md")

    for needle in ("CMD_UMUL    = 29", "CMD_FILL    = 253",
                   "CMD_MOVE    = 254", "CMD_CALL    = 255",
                   "X86_CODE", "8088 EXECUTED THIS LINE", "WAIT_READY"):
        require(needle in source, f"visible AD8088 disk test missing: {needle}")
    require("jsr WAIT_READY" in source and "WAIT_BUSY" not in source,
            "disk test must use the manual's READY-before-write rule")

    # Each result prefix is followed by PASS or FAIL. Keep the complete line
    # within the Apple II's native 40-column display.
    result_prefixes = (
        "MAILBOX READY:                      ",
        "8088 INTEGER COMMAND:               ",
        "AD128K FILL/COPY:                   ",
        "8088 CODE + APPLE WINDOW:           ",
    )
    for prefix in result_prefixes:
        require(prefix in source, f"missing 40-column result line: {prefix!r}")
        require(len(prefix + "PASS") <= 40,
                f"result line exceeds 40 columns: {prefix + 'PASS'!r}")
    require("AD8088_Test.dsk" in builder and "AD8088_Test.po" in builder,
            "builder must package both DOS 3.3 and ProDOS test images")
    require("Original-ROM graphics/MET commands" in readme and
            "Virtual TransWarp and" in readme and
            "cannot run together" in readme,
            "AD8088 compatibility and bus-master limits must be explicit")


def main() -> None:
    test_mailbox()
    test_float_format()
    test_hdl()
    test_machine_and_monitor()
    test_ui_and_build()
    test_hardware_test_disk_sources()
    print("test_ad8088_card: all checks passed")


if __name__ == "__main__":
    main()
