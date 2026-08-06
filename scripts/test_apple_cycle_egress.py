#!/usr/bin/env python3
"""Build and run the focused Apple-cycle egress ring-space test."""

from __future__ import annotations

import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "build" / "apple_cycle_egress_sim"


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
    (OUT_DIR / log_name).write_text(completed.stdout, encoding="utf-8")
    if completed.returncode != 0:
        print(completed.stdout)
        raise RuntimeError(
            f"{Path(command[0]).name} failed with {completed.returncode}"
        )
    return completed.stdout


def main() -> int:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    glbl = (Path(vivado_tool("xvlog")).parents[1] / "data" / "verilog" /
            "src" / "glbl.v")
    run([
        vivado_tool("xvlog"), "--sv",
        str(ROOT / "hdl" / "globals.sv"),
        str(ROOT / "hdl" / "apple" / "apple_cycle_capture_pkg.sv"),
        str(ROOT / "hdl" / "apple" / "apple_cycle_egress.sv"),
        str(ROOT / "hdl" / "sim" / "tb_apple_cycle_egress.sv"),
    ], "xvlog.log")
    run([vivado_tool("xvlog"), str(glbl)], "xvlog_glbl.log")
    run([
        vivado_tool("xelab"), "tb_apple_cycle_egress", "glbl",
        "-L", "unisims_ver", "-s", "tb_apple_cycle_egress_snap",
    ], "xelab.log")
    output = run([
        vivado_tool("xsim"), "tb_apple_cycle_egress_snap", "--runall",
    ], "xsim.log")
    if "APPLE CYCLE EGRESS RING FLAGS PASS" not in output:
        print(output)
        raise RuntimeError("ring-space test did not report success")
    print("Apple-cycle egress ring-space test passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
