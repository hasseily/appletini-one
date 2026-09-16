#!/usr/bin/env python3
"""Run the Disk II sample player and its real DDR fetcher in Xsim."""

from __future__ import annotations

import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "build" / "disk2_sound_sim"
SOURCES = (
    "hdl/globals.sv",
    "hdl/apple/disk2_sound_pkg.sv",
    "hdl/audio_pcm16_ddr_fetcher.sv",
    "hdl/apple/disk2_sound_player.sv",
    "hdl/sim/tb_disk2_sound.sv",
)


def run(tool_name: str, args: list[str], log_name: str) -> str:
    tool = shutil.which(f"{tool_name}.bat") or shutil.which(tool_name)
    if not tool:
        raise FileNotFoundError(f"unable to locate Vivado tool {tool_name}")
    completed = subprocess.run(
        [tool, *args], cwd=OUT_DIR, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=180,
    )
    (OUT_DIR / log_name).write_text(completed.stdout, encoding="utf-8")
    if completed.returncode != 0:
        raise RuntimeError(f"{tool_name} failed:\n{completed.stdout}")
    return completed.stdout


def main() -> int:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    run("xvlog", ["--sv", *[str(ROOT / path) for path in SOURCES]], "xvlog.log")
    run("xelab", ["tb_disk2_sound", "-s", "disk2_sound_snap"], "xelab.log")
    output = run("xsim", ["disk2_sound_snap", "--runall"], "xsim.log")
    if "DISK2 SOUND PASS" not in output:
        raise RuntimeError(f"Disk II sound regression did not pass:\n{output}")
    for line in output.splitlines():
        if "DISK2 SOUND" in line:
            print(line.strip())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
