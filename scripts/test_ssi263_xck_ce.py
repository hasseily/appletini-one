#!/usr/bin/env python3
"""Run physical-Q3, standalone cadence and safe SSI-263 source handoffs."""

from __future__ import annotations

import shutil
import subprocess
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
BUILD_DIR = REPO_ROOT / "build" / "test_ssi263_xck_ce"


def static_checks() -> None:
    source = (REPO_ROOT / "hdl" / "apple" / "ssi263_xck_ce.sv").read_text(
        encoding="utf-8"
    )
    required = (
        "input  logic q3_raw",
        "q3_sync1_q",
        "q3_sync2_q",
        '(* ASYNC_REG = "TRUE" *)',
        "input  logic standalone",
        "input  logic phi0",
    )
    missing = [item for item in required if item not in source]
    if missing:
        raise RuntimeError(f"SSI-263 XCK source contract missing: {missing}")


def run(command: list[str]) -> str:
    print("+", " ".join(command))
    result = subprocess.run(command, cwd=BUILD_DIR, check=True, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    print(result.stdout)
    (BUILD_DIR / (Path(command[0]).stem + ".log")).write_text(
        result.stdout, encoding="utf-8"
    )
    return result.stdout


def vivado_tool(name: str) -> str:
    tool = shutil.which(f"{name}.bat") or shutil.which(name)
    if tool:
        return tool
    raise FileNotFoundError(f"unable to locate Vivado tool {name}")


def main() -> int:
    static_checks()
    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    run([
        vivado_tool("xvlog"), "--sv",
        str(REPO_ROOT / "hdl" / "apple" / "ssi263_xck_ce.sv"),
        str(REPO_ROOT / "hdl" / "sim" / "tb_ssi263_xck_ce.sv"),
    ])
    run([
        vivado_tool("xelab"), "tb_ssi263_xck_ce",
        "-s", "tb_ssi263_xck_ce_sim",
    ])
    output = run([vivado_tool("xsim"), "tb_ssi263_xck_ce_sim", "--runall"])
    # XSim can return zero even after a testbench $fatal.
    if ("SSI263 XCK CE PASS" not in output or
            "SSI263 XCK HANDOFF SWEEP: cases=2096" not in output or
            output.count("SSI263 XCK STANDALONE:") != 4 or "FAIL" in output or
            "Fatal:" in output):
        raise RuntimeError("SSI263 XCK regression failed; see xsim.log")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
