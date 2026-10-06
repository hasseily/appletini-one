#!/usr/bin/env python3
"""Compare scheduled SC-02 tract RTL with the frozen host charge model.

Uses Verilator locally or through WSL Ubuntu on Windows. --synth also runs
isolated Vivado synthesis; it does not build or change the production design.
"""
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
BUILD = ROOT / "build/test_ssi263_native_tract"
RTL = ROOT / "hdl/apple/ssi263_native_tract.sv"
BENCH = ROOT / "scripts/fixtures/ssi263_native/tract_tb.sv"
HARNESS = ROOT / "scripts/fixtures/ssi263_native/tract_compare.cpp"
REFERENCE = ROOT / "scripts/ssi263_host/prototype_tract.cpp"
HEADER = REFERENCE.with_suffix(".h")


def linux_path(path):
    path = path.resolve()
    if os.name != "nt":
        return str(path)
    return "/mnt/" + path.drive[0].lower() + path.as_posix()[2:]


def run(command, log, expected=None):
    result = subprocess.run(command, cwd=BUILD, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    (BUILD / log).write_text(result.stdout, encoding="utf-8")
    if result.returncode or (expected and expected not in result.stdout):
        raise RuntimeError(f"{log} failed:\n{result.stdout}")
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--synth", action="store_true")
    parser.add_argument("--no-build", action="store_true", help="Run an already built Verilator harness")
    args = parser.parse_args()
    BUILD.mkdir(parents=True, exist_ok=True)
    prefix = ["wsl", "-d", "Ubuntu", "--"] if os.name == "nt" else []
    obj = BUILD / "obj_dir"
    if not args.no_build:
        run(prefix + ["verilator", "--cc", "--exe", "--build", "-j", "4",
                      "--top-module", "tract_tb", "--Mdir", linux_path(obj),
                      "-CFLAGS", "-std=c++17 -O2 -I" + linux_path(REFERENCE.parent),
                      linux_path(RTL), linux_path(BENCH), linux_path(HARNESS),
                      linux_path(REFERENCE)], "compile.log")
    output = run(prefix + [linux_path(obj / "Vtract_tb")], "simulation.log", "SSI263 NATIVE TRACT PASS")
    match = re.search(r"events=(\d+) checks=(\d+) max_cycles=(\d+) saturations=(\d+)", output)
    if not match:
        raise RuntimeError(output)
    result = dict(zip(("events", "checks", "max_cycles_after_acceptance", "state_saturations"), map(int, match.groups())))
    if args.synth:
        vivado = shutil.which("vivado.bat") or shutil.which("vivado")
        if not vivado:
            raise RuntimeError("Vivado not found on PATH")
        tcl = BUILD / "tract_synth.tcl"
        tcl.write_text(
            f"read_verilog -sv {{{RTL.as_posix()}}}\n"
            "synth_design -top ssi263_native_tract -part xc7z020clg484-2 -mode out_of_context\n"
            "create_clock -name clk -period 7.5 [get_ports clk]\n"
            "report_utilization -file tract_utilization.rpt\n"
            "report_timing_summary -file tract_timing.rpt\n"
            "write_checkpoint -force tract_synth.dcp\n"
            "puts {SSI263 NATIVE TRACT SYNTHESIS PASS}\n", encoding="utf-8")
        run([vivado, "-mode", "batch", "-source", str(tcl)], "synthesis.log", "SSI263 NATIVE TRACT SYNTHESIS PASS")
    result.update(status="passed", state_values_compared_per_event=61,
                  physical_xck_test_period_clocks=130,
                  minimum_tested_event_period_clocks=112, busy_contract_test=True,
                  standalone_synthesis=args.synth, production_audio_connected=False,
                  reference="Frozen host prototype model; not a physical-chip accuracy claim",
                  sha256={p.relative_to(ROOT).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in (RTL, BENCH, HARNESS, REFERENCE, HEADER)})
    (BUILD / "validation.json").write_text(json.dumps(result, indent=2)+"\n", encoding="utf-8")
    print(output.strip())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
