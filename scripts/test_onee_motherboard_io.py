#!/usr/bin/env python3
"""Build and run the isolated ONE//e motherboard-I/O source test."""

from __future__ import annotations

import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "build" / "onee_motherboard_io_sim"
RTL = ROOT / "hdl" / "apple" / "onee_motherboard_io.sv"
BENCH = ROOT / "hdl" / "sim" / "tb_onee_motherboard_io.sv"


def vivado_tool(name: str) -> str:
    tool = shutil.which(f"{name}.bat") or shutil.which(name)
    if tool:
        return tool
    raise FileNotFoundError(f"unable to locate Vivado tool {name}")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def run(command: list[str], log_name: str) -> str:
    completed = subprocess.run(
        command,
        cwd=OUT_DIR,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    (OUT_DIR / log_name).write_text(completed.stdout, encoding="utf-8")
    if completed.returncode != 0:
        print(completed.stdout)
        raise RuntimeError(
            f"{Path(command[0]).name} failed with {completed.returncode}"
        )
    return completed.stdout


def static_contract_checks() -> None:
    rtl = RTL.read_text(encoding="utf-8")

    require(
        "input  globals::AppleBus_read      ab_read" in rtl and
        "input  logic                       enabled" in rtl and
        "output globals::AppleBus_write     ab_write" in rtl and
        "output globals::AppleBus_read      softswitch_ab_read" in rtl,
        "motherboard I/O must use the shared AppleBus contracts",
    )
    require(
        "wire bus_active = resetn && enabled" in rtl and
        "if (enabled)" in rtl and
        "ab_write = '0;" in rtl and
        "if (!resetn || !enabled || !ab_read.res) begin" in rtl,
        "disabled ONE//e must block bus activity and clear owned state",
    )
    for token in (
        "keyboard_event_valid",
        "keyboard_event_ready",
        "keyboard_any_down",
        "keyboard_modifiers_in",
        "pushbuttons",
        "paddle_values",
        "cassette_out",
        "speaker",
        "utility_strobe_pulse",
        "annunciators",
        "paddle_active",
    ):
        require(token in rtl, f"missing motherboard-I/O contract: {token}")

    require(
        "wire keyboard_clear_access = serve_c0xx" in rtl and
        "(!ab_read.rw &&" in rtl and
        "(ab_read.addr[7:4] == 4'h1)" in rtl and
        "(ab_read.rw &&" in rtl and
        "(ab_read.addr[7:0] == 8'h10)" in rtl and
        "read_data  = {keyboard_any_down, keyboard_code_q};" in rtl,
        "writes to C010-C01F and reads of C010 must clear the keyboard strobe",
    )
    for state in (
        "sss.sw_lcram_bank2",
        "sss.sw_ramrd",
        "sss.sw_ramwrt",
        "sss.sw_intcxrom",
        "sss.sw_text",
        "sss.sw_mixed",
        "sss.sw_page2",
        "sss.sw_hires",
        "sss.sw_altcharset",
        "sss.sw_80col",
    ):
        require(state in rtl, f"missing existing SoftSwitchState use: {state}")

    require(
        "softswitch_ab_read = ab_read;" in rtl and
        "if (enabled && !ioudis &&" in rtl and
        "softswitch_ab_read.serve_en = 1'b0;" in rtl,
        "IOUDIS-off C05E/F must be filtered from the existing DHIRES latch",
    )
    require(
        "8'h7E: status_bit = ~ioudis;" in rtl and
        "8'h7F: status_bit = sss.sw_dhires;" in rtl,
        "RDIOUDIS/RDDHIRES status polarity must remain explicit",
    )
    require(
        "if (ab_read.addr[7:4] == 4'h4)" in rtl,
        "the C040-C04F utility-strobe mirrors must share one decoder",
    )
    require(
        "if (ab_read.addr[7:4] == 4'h6) begin" in rtl and
        "unique case (ab_read.addr[2:0])" in rtl,
        "C068-C06F must mirror C060-C067 through the low three address bits",
    )
    require(
        "if (ab_read.addr[7:4] == 4'h7) begin" in rtl and
        "paddle_count_q[3] <= paddle_reload" in rtl and
        "if (ab_read.data_en && ab_read.cycle_valid)" in rtl,
        "all C07x aliases must snapshot paddles which expire on native cycles",
    )
    require(
        "read_data  = {status_bit, floating_bus_data[6:0]};" in rtl and
        "ab_write_d.wr_data_en = read_claim;" in rtl,
        "defined bit-7 reads and unclaimed floating-bus fallback must differ",
    )


def main() -> int:
    static_contract_checks()

    if OUT_DIR.exists():
        shutil.rmtree(OUT_DIR)
    OUT_DIR.mkdir(parents=True)

    run([
        vivado_tool("xvlog"), "--sv",
        str(ROOT / "hdl" / "globals.sv"),
        str(ROOT / "hdl" / "apple" / "soft_switch_manager.sv"),
        str(RTL),
        str(BENCH),
    ], "xvlog.log")
    run([
        vivado_tool("xelab"), "tb_onee_motherboard_io",
        "-s", "tb_onee_motherboard_io_snap",
    ], "xelab.log")
    output = run([
        vivado_tool("xsim"), "tb_onee_motherboard_io_snap", "--runall",
    ], "xsim.log")

    require(
        "ONEE MOTHERBOARD IO PASS" in output,
        "simulation did not report the motherboard-I/O pass marker",
    )
    print("ONE//e motherboard I/O source test passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
