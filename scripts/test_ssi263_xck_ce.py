#!/usr/bin/env python3
"""Compile and run the physical-Q3 SSI-263 clock-enable bench."""

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
        "q3_sync2_d_q",
        '(* ASYNC_REG = "TRUE" *)',
        "assign xck_ce = q3_sync2_q && !q3_sync2_d_q;",
    )
    missing = [item for item in required if item not in source]
    if missing:
        raise RuntimeError(f"physical-Q3 XCK contract missing: {missing}")
    forbidden = (
        "XCK_NUMERATOR_HZ",
        "XCK_DENOMINATOR",
        "FABRIC_HZ",
        "accumulator_q",
        "MODULUS",
    )
    leaked = [item for item in forbidden if item in source]
    if leaked:
        raise RuntimeError(f"nominal XCK oscillator leaked into RTL: {leaked}")


def run(command: list[str]) -> None:
    print("+", " ".join(command))
    subprocess.run(command, cwd=BUILD_DIR, check=True)


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
    run([vivado_tool("xsim"), "tb_ssi263_xck_ce_sim", "--runall"])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
