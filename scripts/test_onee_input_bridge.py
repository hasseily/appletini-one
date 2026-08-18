#!/usr/bin/env python3
"""Build, simulate, and synthesize the isolated ONE//e input bridge."""

from __future__ import annotations

import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "build" / "onee_input_bridge_sim"
RTL = ROOT / "hdl" / "apple" / "onee_input_bridge.sv"
BENCH = ROOT / "hdl" / "sim" / "tb_onee_input_bridge.sv"


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

    for token in (
        "input  logic                       enabled",
        "input  logic [7:0]                 ps_addr",
        "input  logic [7:0]                 ps_read_addr",
        "output logic                       keyboard_event_valid",
        "input  logic                       keyboard_event_ready",
        "output logic [6:0]                 keyboard_event_code",
        "output logic [31:0]                paddle_values",
        "output logic                       warm_reset_request",
        "input  logic                       warm_reset_ack",
    ):
        require(token in rtl, f"missing input-bridge contract: {token}")

    for token in (
        "REG_KEY_FIFO       = 8'h5C",
        "REG_LIVE_INPUTS    = 8'h5D",
        "REG_PADDLES        = 8'h5E",
        "REG_CONTROL_STATUS = 8'h5F",
        "localparam logic [31:0] NEUTRAL_PADDLES = 32'h8080_8080",
    ):
        require(token in rtl, f"missing register contract: {token}")

    require(
        "wire key_push = key_push_request && (!key_full || key_pop);" in rtl
        and "wire key_drop = key_push_request && key_full && !key_pop;" in rtl
        and "case ({key_push, key_pop})" in rtl,
        "FIFO must account for a simultaneous push and pop exactly once",
    )
    require(
        "else if (!enabled) begin" in rtl
        and "keyboard_event_valid = 1'b0;" in rtl
        and "warm_reset_request = 1'b0;" in rtl
        and "ps_rdata = 32'h0000_0000;" in rtl,
        "disable must mask outputs at once and clear saved state",
    )
    require(
        "if (warm_reset_ack)" in rtl
        and "if (control_write && ps_wdata[0])" in rtl,
        "warm reset request must use an acknowledge handshake",
    )
    require(
        "if (control_write && ps_wdata[1])" in rtl
        and "if (key_drop)" in rtl,
        "FIFO overflow must be sticky and clearable",
    )


def write_synth_tcl() -> Path:
    synth_tcl = OUT_DIR / "synth_onee_input_bridge.tcl"
    rtl_path = RTL.as_posix()
    synth_tcl.write_text(
        "read_verilog -sv {" + rtl_path + "}\n"
        "synth_design -top onee_input_bridge -part xc7z020clg484-2\n"
        "report_utilization -file onee_input_bridge_utilization.rpt\n"
        "write_checkpoint -force onee_input_bridge_synth.dcp\n"
        "quit\n",
        encoding="utf-8",
    )
    return synth_tcl


def main() -> int:
    static_contract_checks()

    if OUT_DIR.exists():
        shutil.rmtree(OUT_DIR)
    OUT_DIR.mkdir(parents=True)

    run([
        vivado_tool("xvlog"), "--sv", str(RTL), str(BENCH),
    ], "xvlog.log")
    run([
        vivado_tool("xelab"), "tb_onee_input_bridge",
        "-s", "tb_onee_input_bridge_snap",
    ], "xelab.log")
    output = run([
        vivado_tool("xsim"), "tb_onee_input_bridge_snap", "--runall",
    ], "xsim.log")
    require(
        "ONEE INPUT BRIDGE PASS" in output,
        "simulation did not report the input-bridge pass marker",
    )

    synth_tcl = write_synth_tcl()
    synth_output = run([
        vivado_tool("vivado"), "-mode", "batch", "-nolog", "-nojournal",
        "-source", str(synth_tcl),
    ], "synth.log")
    require(
        "synth_design completed successfully" in synth_output,
        "isolated input-bridge synthesis did not complete",
    )

    print("ONE//e input bridge simulation and synthesis passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
