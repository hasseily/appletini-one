#!/usr/bin/env python3
"""Compare the standalone RTL pitch adapter with the host pitch state.

The default check needs Verilator. --xsim runs hdl/sim/tb_ssi263_native_pitch.sv
in Vivado simulation instead; that bench checks the glide rate in closed form.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import shutil
import subprocess

from ssi263_host_data import write_tables
from test_ssi263_native_controller import ROOT, HOST, digest, linux_path, run

RTL = ROOT / "hdl/apple/ssi263_native_pitch.sv"
HARNESS = ROOT / "scripts/fixtures/ssi263_native/pitch_parity.cpp"
BENCH = ROOT / "hdl/sim/tb_ssi263_native_pitch.sv"


def xsim(out: Path) -> None:
    for step, command, required in (
            ("compile", ["xvlog", "--sv", str(RTL), str(BENCH)], None),
            ("elaborate", ["xelab", "tb_ssi263_native_pitch", "-s", "ssi263_native_pitch_sim"], None),
            ("simulation", ["xsim", "ssi263_native_pitch_sim", "--runall"], "SSI263 NATIVE PITCH PASS")):
        tool = shutil.which(command[0] + ".bat") or shutil.which(command[0])
        if not tool:
            raise FileNotFoundError(f"Vivado tool {command[0]} is not on PATH")
        result = subprocess.run([tool, *command[1:]], cwd=out, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (out / f"xsim_{step}.log").write_text(result.stdout, encoding="utf-8")
        if result.returncode or (required and required not in result.stdout):
            raise RuntimeError(f"xsim {step} failed: {result.stdout[-2000:]}")
    print(next(line for line in result.stdout.splitlines() if "PASS" in line))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/test_ssi263_native_pitch")
    parser.add_argument("--xsim", action="store_true", help="run the Vivado closed-form glide bench instead")
    args = parser.parse_args()
    out = args.build_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    if args.xsim:
        xsim(out)
        return
    tables = out / "tables.txt"
    table_metadata = write_tables(tables)
    command = ["verilator", "--cc", "--exe", "--build", "-j", "4", "--Wall",
               "-Wno-DECLFILENAME", "--top-module", "ssi263_native_pitch", "--Mdir", linux_path(out),
               "-CFLAGS", f"-std=c++17 -O2 -I{linux_path(HOST)}", linux_path(RTL), linux_path(HARNESS)]
    run(command, out, out / "compile.log")
    output = run([linux_path(out / "Vssi263_native_pitch"), linux_path(tables)], out, out / "simulation.log")
    result = next(json.loads(line) for line in output.splitlines() if line.startswith('{"status"'))
    result.update({"source_sha256": {str(path.relative_to(ROOT)).replace('\\', '/'): digest(path)
                                    for path in (RTL, HARNESS, HOST / "baseline.h")},
                   "tables": table_metadata,
                   "coverage": ["all 4096 inflection words in all four functions",
                                "all 8 slope settings at RATE 0, 8 and 15 in both directions",
                                "full-range glides", "write-before-XCK ordering", "no-XCK holds",
                                "CTL/function changes",
                                "cold seed and retained glide seed", "FF alias writes", "deterministic random traffic"],
                   "physical_ssi_behavior_verified": False})
    (out / "validation.json").write_text(json.dumps(result, indent=2) + "\n")
    print(f"Native pitch: {result['observations']:,} observations, {result['checks']:,} exact state checks")


if __name__ == "__main__":
    main()
