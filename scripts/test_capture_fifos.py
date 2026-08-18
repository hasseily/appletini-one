#!/usr/bin/env python3
"""Build and run the Apple renderer and SDD capture module tests."""

from __future__ import annotations

import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "build" / "capture_fifo_sim"


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


def run_bench(top: str, marker: str) -> str:
    snapshot = f"{top}_snap"
    run([
        vivado_tool("xelab"), top,
        "-s", snapshot,
    ], f"xelab_{top}.log")
    output = run([
        vivado_tool("xsim"), snapshot, "--runall",
    ], f"xsim_{top}.log")
    if marker not in output:
        print(output)
        raise RuntimeError(f"{top} did not report success")
    return output


def main() -> int:
    if OUT_DIR.exists():
        shutil.rmtree(OUT_DIR)
    OUT_DIR.mkdir(parents=True)

    run([
        vivado_tool("xvlog"), "--sv",
        str(ROOT / "hdl" / "globals.sv"),
        str(ROOT / "hdl" / "apple" / "apple_cycle_capture_pkg.sv"),
        str(ROOT / "hdl" / "sim" / "xpm_fifo_sync_model.sv"),
        str(ROOT / "hdl" / "apple" / "apple_cycle_capture.sv"),
        str(ROOT / "hdl" / "apple" / "sdd_bus_tap.sv"),
        str(ROOT / "hdl" / "sim" / "tb_apple_cycle_capture.sv"),
        str(ROOT / "hdl" / "sim" / "tb_sdd_bus_tap.sv"),
    ], "xvlog.log")

    run_bench("tb_apple_cycle_capture", "APPLE CYCLE CAPTURE PASS")
    sdd_output = run_bench("tb_sdd_bus_tap", "SDD BUS TAP PASS")

    print("Apple renderer and SDD capture module tests passed")
    if "KNOWN GAP:" in sdd_output:
        for line in sdd_output.splitlines():
            if "KNOWN GAP:" in line:
                print(line.strip())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
