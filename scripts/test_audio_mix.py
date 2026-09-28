#!/usr/bin/env python3
"""Simulate the real top-level saturating mixer against arithmetic references."""

from __future__ import annotations

import re
import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build" / "audio_mix_sim"
TOP = ROOT / "hdl" / "appletini_yarz_top.sv"
BENCH = ROOT / "hdl" / "sim" / "tb_audio_mix.sv"


def run(tool_name: str, args: list[str]) -> str:
    tool = shutil.which(f"{tool_name}.bat") or shutil.which(tool_name)
    if not tool:
        raise FileNotFoundError(f"unable to locate Vivado tool {tool_name}")
    result = subprocess.run(
        [tool, *args], cwd=OUT, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, timeout=180,
    )
    (OUT / f"{tool_name}.log").write_text(result.stdout, encoding="utf-8")
    if result.returncode:
        raise RuntimeError(f"{tool_name} failed:\n{result.stdout}")
    return result.stdout


def main() -> int:
    # Compile the actual helper, rather than a copy that could drift from it.
    source = TOP.read_text(encoding="utf-8")
    helper = re.search(
        r"function automatic logic signed \[15:0\] sat_add16\(.*?endfunction",
        source, re.DOTALL,
    )
    if helper is None:
        raise RuntimeError("top-level sat_add16 helper was not found")
    OUT.mkdir(parents=True, exist_ok=True)
    wrapper = OUT / "audio_mix_current.sv"
    wrapper.write_text(
        "`timescale 1ns / 1ps\n"
        "module audio_mix_current(\n"
        "    input logic signed [15:0] a, b, c,\n"
        "    output wire signed [15:0] pair, mixed\n"
        ");\n" + helper.group(0) + "\n"
        "assign pair = sat_add16(a, b);\n"
        "assign mixed = sat_add16(sat_add16(a, b), c);\n"
        "endmodule\n",
        encoding="utf-8",
    )
    run("xvlog", ["--sv", str(wrapper), str(BENCH)])
    run("xelab", ["tb_audio_mix", "-s", "audio_mix_snap"])
    output = run("xsim", ["audio_mix_snap", "--runall"])
    if "AUDIO MIX PASS" not in output:
        raise RuntimeError(f"audio mixer simulation did not pass:\n{output}")
    for line in output.splitlines():
        if "AUDIO MIX PASS" in line:
            print(line.strip())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
