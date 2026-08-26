#!/usr/bin/env python3
"""Build and run focused SSI-263/SC-01 hybrid core checks."""

from __future__ import annotations

import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "build" / "ssi263_hybrid_core_sim"
PASS_MARKER = "SSI263 HYBRID CORE PASS"


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
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    run(
        [
            vivado_tool("xvlog"),
            "--sv",
            str(ROOT / "hdl" / "apple" / "ssi263_formant_pkg.sv"),
            str(ROOT / "hdl" / "apple" / "sc01a_digital_core.sv"),
            str(ROOT / "hdl" / "sim" / "tb_ssi263_hybrid_core.sv"),
        ],
        "xvlog.log",
    )
    run(
        [
            vivado_tool("xelab"),
            "tb_ssi263_hybrid_core",
            "-s",
            "tb_ssi263_hybrid_core_snap",
            "--timescale",
            "1ns/1ps",
        ],
        "xelab.log",
    )
    output = run(
        [vivado_tool("xsim"), "tb_ssi263_hybrid_core_snap", "--runall"],
        "xsim.log",
    )
    if PASS_MARKER not in output or "SSI263 HYBRID CORE FAIL" in output:
        print(output)
        raise RuntimeError("SSI-263 hybrid core test did not pass")
    print(next(line for line in output.splitlines() if PASS_MARKER in line))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
