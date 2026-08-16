#!/usr/bin/env python3
"""Build and run the source-level ONE//e mode safety-guard test."""

from __future__ import annotations

import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "build" / "onee_mode_safety_guard_sim"
RTL = ROOT / "hdl" / "apple" / "onee_mode_safety_guard.sv"
BENCH = ROOT / "hdl" / "sim" / "tb_onee_mode_safety_guard.sv"
HDL_SOURCES = ROOT / "hdl" / "hdl_sources.txt"


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
    sources = HDL_SOURCES.read_text(encoding="utf-8").splitlines()

    require(
        sources.count("apple/onee_mode_safety_guard.sv") == 1,
        "ONE//e safety guard must appear once in hdl_sources.txt",
    )
    for signal in (
        "apple_power_present_raw",
        "apple_phi0_raw",
        "apple_7m_raw",
        "apple_q3_raw",
        "apple_m2sel_raw",
        "apple_m2b0_raw",
        "apple_devsel_n_raw",
        "apple_reset_n_raw",
        "apple_inh_n_raw",
        "apple_irq_n_raw",
        "apple_nmi_n_raw",
        "apple_rdy_n_raw",
        "apple_dma_n_raw",
    ):
        require(signal in rtl, f"safety guard must monitor {signal}")

    require(
        '(* ASYNC_REG = "TRUE" *)' in rtl,
        "raw Apple inputs must pass through marked synchronizers",
    )
    require(
        "wire apple_nonidle_level =" in rtl and
        "|(apple_raw_levels ^ APPLE_IDLE_LEVELS);" in rtl and
        "apple_power_present_raw || apple_input_transition_now ||" in rtl and
        "apple_nonidle_level;" in rtl,
        "every non-idle raw level must be a direct continuous veto",
    )
    require(
        "~resetn | apple_power_present_raw | apple_input_transition_now |"
        in rtl and "apple_nonidle_level;" in rtl,
        "reset, raw transitions, and held non-idle levels must asynchronously "
        "set the sticky lockout",
    )
    require(
        "always_ff @(posedge clk or posedge activity_lockout_set)" in rtl,
        "sticky lockout must use the fail-safe asynchronous set",
    )
    require(
        "assign physical_bus_isolate =" in rtl and
        "manual_enable_request || onee_selected || physical_isolation_hold;"
        in rtl,
        "physical bus isolation must assert on request and remain held",
    )
    require(
        "always_ff @(posedge clk or posedge physical_isolation_set)" in rtl and
        "(manual_enable_request || onee_selected);" in rtl and
        "else if (!resetn)" in rtl and
        "else if (activity_lockout_clear)" in rtl,
        "physical isolation hold must catch activity and clear only on rearm",
    )
    require(
        "!manual_enable_request ||" in rtl and
        "apple_power_present_raw ||" in rtl and
        "apple_activity_now ||" in rtl and
        "apple_activity_lockout ||" in rtl and
        "!onee_selected;" in rtl,
        "effective output kill must directly include request, raw activity, "
        "sticky lockout, and selected state",
    )
    require(
        "apple_activity_quiet &&" in rtl and
        "!manual_enable_request &&" in rtl,
        "lockout clear must require both quiet Apple inputs and manual off",
    )
    for reason in (
        "INHIBIT_RESET",
        "INHIBIT_APPLE_POWER",
        "INHIBIT_APPLE_ACTIVITY",
        "INHIBIT_ACTIVITY_LOCKOUT",
        "INHIBIT_RESELECT_REQUIRED",
        "INHIBIT_MANUAL_OFF",
    ):
        require(reason in rtl, f"missing status reason {reason}")


def main() -> int:
    static_contract_checks()

    if OUT_DIR.exists():
        shutil.rmtree(OUT_DIR)
    OUT_DIR.mkdir(parents=True)

    run([
        vivado_tool("xvlog"), "--sv", str(RTL), str(BENCH),
    ], "xvlog.log")
    run([
        vivado_tool("xelab"), "tb_onee_mode_safety_guard",
        "-s", "tb_onee_mode_safety_guard_snap",
    ], "xelab.log")
    output = run([
        vivado_tool("xsim"), "tb_onee_mode_safety_guard_snap", "--runall",
    ], "xsim.log")

    require(
        "ONEE MODE SAFETY GUARD PASS" in output,
        "simulation did not report the ONE//e safety pass marker",
    )
    print("ONE//e mode safety guard source test passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
