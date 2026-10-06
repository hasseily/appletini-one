#!/usr/bin/env python3
"""Exhaustive SSI divider simulation using independent rollover-count vectors.

The oracle counts clocks until a preset 8-/12-bit counter rolls over, then
accounts for the datasheet's fixed /2 and /8 stages. It does not read, parse,
or copy the RTL's complement/concatenation implementation. All inflections
run for at least two periods; every FF value also runs with DIV2 enabled.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import shutil
import subprocess
import time


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build" / "test_ssi263_clock_core"
RTL = ROOT / "hdl" / "apple" / "ssi263_clock_core.sv"
BENCH = ROOT / "hdl" / "sim" / "tb_ssi263_clock_core.sv"


def rollover_periods(bits: int, fixed_divisor: int) -> list[int]:
    mask = (1 << bits) - 1
    periods = []
    for preset in range(mask + 1):
        counter = preset
        clocks = 0
        while True:
            counter = (counter + 1) & mask
            clocks += fixed_divisor
            if counter == 0:
                periods.append(clocks)
                break
    return periods


def vivado_tool(name: str) -> str:
    found = shutil.which(f"{name}.bat") or shutil.which(name)
    if not found:
        raise FileNotFoundError(f"Vivado tool {name} is not on PATH")
    return found


def run(command: list[str], log_name: str, required: str | None = None) -> str:
    print("+", " ".join(command), flush=True)
    result = subprocess.run(command, cwd=BUILD, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    output = result.stdout
    (BUILD / log_name).write_text(output, encoding="utf-8")
    print(output, end="", flush=True)
    if result.returncode != 0 or (required is not None and required not in output):
        raise RuntimeError(f"{log_name} failed; exit={result.returncode}")
    return output


def synthesize() -> None:
    # Out-of-context only. This checks cost and synthesis legality, not routed
    # timing or whether the complete audio engine meets its processing budget.
    script = BUILD / "synth_clock_core.tcl"
    script.write_text(
        f"read_verilog -sv {{{RTL.as_posix()}}}\n"
        "synth_design -top ssi263_clock_core -part xc7z020clg484-2 -mode out_of_context\n"
        "create_clock -name clk -period 7.5 [get_ports clk]\n"
        "report_utilization -file clock_core_utilization.rpt\n"
        "report_timing_summary -file clock_core_timing.rpt\n"
        "puts {SSI263 CLOCK SYNTHESIS PASS}\n",
        encoding="utf-8",
    )
    run([vivado_tool("vivado"), "-mode", "batch", "-source", str(script)],
        "synthesis.log", "SSI263 CLOCK SYNTHESIS PASS")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--synth", action="store_true", help="also synthesize the isolated clock core")
    args = parser.parse_args()
    started = time.monotonic()
    BUILD.mkdir(parents=True, exist_ok=True)
    pitch = rollover_periods(12, 8)
    filters = rollover_periods(8, 2)
    assert (pitch[0], pitch[4095], filters[0], filters[255]) == (32768, 8, 512, 2)
    assert all(a - b == 8 for a, b in zip(pitch, pitch[1:]))
    assert all(a - b == 2 for a, b in zip(filters, filters[1:]))
    for name, values in (("pitch", pitch), ("filter", filters)):
        (BUILD / f"{name}_periods.mem").write_text(
            "".join(f"{value:04x}\n" for value in values), encoding="ascii"
        )
    run([vivado_tool("xvlog"), "--sv", str(RTL), str(BENCH)], "compile.log")
    run([vivado_tool("xelab"), "tb_ssi263_clock_core", "-s", "ssi263_clock_core_sim"],
        "elaborate.log")
    run([vivado_tool("xsim"), "ssi263_clock_core_sim", "--runall"],
        "simulation.log", "SSI263 CLOCK CORE PASS cases=4621")
    if args.synth:
        synthesize()
    (BUILD / "validation.json").write_text(json.dumps({
        "status": "passed",
        "immediate_inflection_values": len(pitch),
        "filter_values_per_div2_mode": len(filters),
        "div2_filter_modes": [False, True],
        "div2_pitch_boundary_values": [0, 1, 4094, 4095],
        "raw_regional_clocks_hz": [2031250, 2040968],
        "caller_policy_cases": 7,
        "standalone_synthesis": args.synth,
        "production_integration": False,
        "silicon_live_write_phase_verified": False,
        "elapsed_seconds": round(time.monotonic() - started, 3),
    }, indent=2) + "\n", encoding="utf-8")
    print("SSI-263 clock core: exhaustive simulation passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
