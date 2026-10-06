#!/usr/bin/env python3
"""Compare native SSI source RTL with independent prototype-latch vectors."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import time

from ssi263_control_reference import write_vectors


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build" / "test_ssi263_source_control"
RTL = ROOT / "hdl" / "apple" / "ssi263_source_control.sv"
BENCH = ROOT / "hdl" / "sim" / "tb_ssi263_source_control.sv"
REFERENCE = ROOT / "scripts" / "ssi263_control_reference.py"


def tool(name: str) -> str:
    found = shutil.which(f"{name}.bat") or shutil.which(name)
    if not found:
        raise FileNotFoundError(f"Vivado tool {name} is not on PATH")
    return found


def run(command: list[str], log: str, expected: str | None = None,
        expected_fatal: bool = False) -> str:
    print("+", " ".join(command), flush=True)
    result = subprocess.run(command, cwd=BUILD, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    output = result.stdout
    (BUILD / log).write_text(output, encoding="utf-8")
    print(output, end="", flush=True)
    if expected_fatal:
        if expected is None or expected not in output or "Missing " in output:
            raise RuntimeError(f"{log}: required phase-contract rejection was absent")
    elif result.returncode != 0 or (expected is not None and expected not in output):
        raise RuntimeError(f"{log}: exit={result.returncode}, expected={expected!r}")
    return output


def synthesize() -> None:
    script = BUILD / "synth_source_control.tcl"
    script.write_text(
        f"read_verilog -sv {{{RTL.as_posix()}}}\n"
        "synth_design -top ssi263_source_control -part xc7z020clg484-2 -mode out_of_context\n"
        "create_clock -name clk -period 7.5 [get_ports clk]\n"
        "report_utilization -file source_control_utilization.rpt\n"
        "report_timing_summary -file source_control_timing.rpt\n"
        "puts {SSI263 SOURCE SYNTHESIS PASS}\n",
        encoding="utf-8",
    )
    run([tool("vivado"), "-mode", "batch", "-source", str(script)],
        "synthesis.log", "SSI263 SOURCE SYNTHESIS PASS")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--synth", action="store_true",
                        help="also synthesize the source control in isolation")
    args = parser.parse_args()
    started = time.monotonic()
    BUILD.mkdir(parents=True, exist_ok=True)
    count = write_vectors(BUILD / "source_control_vectors.mem")
    run([tool("xvlog"), "--sv", str(RTL), str(BENCH)], "compile.log")
    run([tool("xelab"), "tb_ssi263_source_control", "-s", "ssi263_source_control_sim"],
        "elaborate.log")
    run([tool("xsim"), "ssi263_source_control_sim", "--runall"],
        "simulation.log", f"SSI263 SOURCE CONTROL PASS cases={count}")
    contracts = {
        "invalid_phone": "serialize phone and selector writes",
        "invalid_selectors": "only one selector edge per clk",
        "invalid_phi0": "serialize Phi0 and possible U20 writes",
    }
    for case, message in contracts.items():
        run([tool("xsim"), "ssi263_source_control_sim", "--runall", "-testplusarg", case],
            f"{case}.log", f"SSI263 source contract: {message}", expected_fatal=True)
    if args.synth:
        synthesize()
    (BUILD / "validation.json").write_text(json.dumps({
        "status": "passed",
        "prototype_reference_vectors": count,
        "phase_contract_rejections": list(contracts),
        "standalone_synthesis": args.synth,
        "source_generator_or_native_audio_implemented": False,
        "production_reset_or_coincident_phase_order_verified": False,
        "sha256": {str(path.relative_to(ROOT)).replace("\\", "/"):
                   hashlib.sha256(path.read_bytes()).hexdigest()
                   for path in (RTL, BENCH, REFERENCE)},
        "elapsed_seconds": round(time.monotonic() - started, 3),
    }, indent=2) + "\n", encoding="utf-8")
    print(f"SSI-263 source control: {count} reference vectors and 3 phase contracts passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
