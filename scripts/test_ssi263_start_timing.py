#!/usr/bin/env python3
"""Build and run the focused SSI263/Votrax start timing test."""

from __future__ import annotations

import shutil
import subprocess
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "build" / "ssi263_start_timing_sim"
PASS_MARKER = "SSI263 START TIMING PASS"


def static_checks() -> None:
    source = (ROOT / "hdl" / "apple" / "ssi263_bus_wrapper.sv").read_text(
        encoding="utf-8"
    )
    if not re.search(
        r"\(\*\s*KEEP\s*=\s*\"TRUE\"\s*\*\)\s*"
        r"logic\s+backend_start_q\s*;",
        source,
    ):
        raise RuntimeError(
            "backend_start_q must remain a kept private register"
        )
    if "assign formant_backend_start = backend_start_q;" not in source:
        raise RuntimeError("formant backend start must use backend_start_q directly")


def vivado_tool(name: str) -> str:
    tool = shutil.which(f"{name}.bat") or shutil.which(name)
    if tool:
        return tool
    raise FileNotFoundError(f"unable to locate Vivado tool {name}")


def run(command: list[str], log_name: str) -> str:
    completed = subprocess.run(
        command,
        cwd=OUT_DIR,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    output = completed.stdout or ""
    (OUT_DIR / log_name).write_text(output, encoding="utf-8")
    if completed.returncode != 0:
        print(output)
        raise RuntimeError(
            f"{Path(command[0]).name} failed with {completed.returncode}; "
            f"see {OUT_DIR / log_name}"
        )
    return output


def main() -> int:
    static_checks()
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    run(
        [
            vivado_tool("xvlog"),
            "--sv",
            str(ROOT / "hdl" / "apple" / "ssi263_formant_pkg.sv"),
            str(ROOT / "hdl" / "apple" / "sc01a_digital_core.sv"),
            str(ROOT / "hdl" / "apple" / "ssi263_formant_backend.sv"),
            str(ROOT / "hdl" / "apple" / "ssi263_bus_wrapper.sv"),
            str(ROOT / "hdl" / "sim" / "tb_ssi263_start_timing.sv"),
        ],
        "xvlog.log",
    )
    run(
        [
            vivado_tool("xelab"),
            "tb_ssi263_start_timing",
            "-s",
            "tb_ssi263_start_timing_snap",
            "--timescale",
            "1ns/1ps",
        ],
        "xelab.log",
    )
    output = run(
        [vivado_tool("xsim"), "tb_ssi263_start_timing_snap", "--runall"],
        "xsim.log",
    )
    if PASS_MARKER not in output or "SSI263 START TIMING FAIL" in output:
        print(output)
        raise RuntimeError("SSI263/Votrax start timing test did not pass")
    print(next(line for line in output.splitlines() if PASS_MARKER in line))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
