#!/usr/bin/env python3
"""Test the post-engine SSI volume/pan mixer and production AY/speech sum."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build/test_ssi263_mixer"
RTL = ROOT / "hdl/apple/ssi263_stereo_mixer.sv"
CARD = ROOT / "hdl/apple/mockingboard.sv"
BENCH = ROOT / "scripts/fixtures/ssi263_native/mixer_tb.sv"
HARNESS = ROOT / "scripts/fixtures/ssi263_native/mixer_compare.cpp"


def linux_path(path):
    path = path.resolve()
    return "/mnt/" + path.drive[0].lower() + path.as_posix()[2:] if os.name == "nt" else str(path)


def run(command, log, expected=None):
    result = subprocess.run(command, cwd=BUILD, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (BUILD / log).write_text(result.stdout, encoding="utf-8")
    if result.returncode or (expected and expected not in result.stdout):
        raise RuntimeError(f"{log} failed:\n{result.stdout}")
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--synth", action="store_true")
    args = parser.parse_args()
    BUILD.mkdir(parents=True, exist_ok=True)
    source = CARD.read_text()
    functions = []
    for name in ("mix2_to_wide", "mix4_to_wide", "mix_speech"):
        match = re.search(r"function automatic[^;]*?\b" + name + r"\([^;]*;.*?endfunction", source, re.S)
        if not match:
            raise RuntimeError(f"Production function {name} missing")
        functions.append(match.group())
    (BUILD / "mockingboard_mix_functions.svh").write_text("\n\n".join(functions)+"\n")
    prefix = ["wsl", "-d", "Ubuntu", "--"] if os.name == "nt" else []
    obj = BUILD / "obj_dir"
    run(prefix + ["verilator", "--cc", "--exe", "--build", "-j", "4", "--top-module", "mixer_tb",
                  "--Mdir", linux_path(obj), "-I" + linux_path(BUILD), "-CFLAGS", "-std=c++17 -O2",
                  linux_path(RTL), linux_path(BENCH), linux_path(HARNESS)], "compile.log")
    output = run(prefix + [linux_path(obj / "Vmixer_tb")], "simulation.log", "SSI263 MIXER PASS")
    match = re.search(r"checks=(\d+) cycles=(\d+) publications=(\d+)", output)
    if not match:
        raise RuntimeError(output)
    result = dict(zip(("checks", "cycles", "publications"), map(int, match.groups())))
    if args.synth:
        vivado = shutil.which("vivado.bat") or shutil.which("vivado")
        if not vivado:
            raise RuntimeError("Vivado not found on PATH")
        tcl = BUILD / "mixer_synth.tcl"
        tcl.write_text(
            f"read_verilog -sv {{{RTL.as_posix()}}}\n"
            "synth_design -top ssi263_stereo_mixer -part xc7z020clg484-2 -mode out_of_context\n"
            "create_clock -name clk -period 7.5 [get_ports clk]\n"
            "report_utilization -file mixer_utilization.rpt\n"
            "report_timing_summary -file mixer_timing.rpt\n"
            "write_checkpoint -force mixer_synth.dcp\n"
            "puts {SSI263 MIXER SYNTHESIS PASS}\n")
        run([vivado, "-mode", "batch", "-source", str(tcl)], "synthesis.log", "SSI263 MIXER SYNTHESIS PASS")
    result.update(status="passed", standalone_synthesis=args.synth,
                  unity_gain_db=0, default_gain_db=2, gain_range_db=[-5,5], default_pan="F0",
                  ramp_step_q14=64, maximum_pan_ramp_samples=456,
                  sha256={p.relative_to(ROOT).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in (RTL,CARD,BENCH,HARNESS)})
    (BUILD / "validation.json").write_text(json.dumps(result,indent=2)+"\n")
    print(output.strip())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
