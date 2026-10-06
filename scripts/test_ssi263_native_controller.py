#!/usr/bin/env python3
"""Check the fabric native controller against the frozen host on every edge.

Uses Verilator (WSL Ubuntu on Windows). This tests the listening candidate's
exact current behavior, including its documented prototype departures.
It is not a new claim about physical SSI behavior.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

from render_ssi263_mb_audit import FIXTURE, make_trace
from ssi263_host_data import ACTIVE_ROM_SHA256, NATIVE_ROM, make_demo

ROOT = Path(__file__).resolve().parents[1]
RTL = ROOT / "hdl/apple/ssi263_native_controller.sv"
HOST = ROOT / "scripts/ssi263_host"
HARNESS = ROOT / "scripts/fixtures/ssi263_native/controller_parity.cpp"


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def linux_path(path: Path) -> str:
    value = path.resolve().as_posix()
    return "/mnt/" + value[0].lower() + value[2:] if os.name == "nt" else value


def run(command: list[str], cwd: Path, log: Path) -> str:
    if os.name == "nt":
        command = ["wsl", "-d", "Ubuntu", "--", *command]
    result = subprocess.run(command, cwd=cwd, capture_output=True, text=True)
    output = result.stdout + result.stderr
    log.write_text(output, encoding="utf-8")
    if result.returncode:
        raise RuntimeError(f"{log}:\n{output[-12000:]}")
    return output


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/test_ssi263_native_controller")
    parser.add_argument("--art-reference", type=int, nargs="+", default=[0, 8, 15])
    parser.add_argument("--synth", action="store_true", help="also run standalone Vivado synthesis")
    args = parser.parse_args()
    out = args.build_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    if any(value < 0 or value > 15 for value in args.art_reference):
        raise ValueError("ART reference must be 0..15")
    raw = bytes(int(field, 16) for line in NATIVE_ROM.read_text().splitlines()
                for field in line.split("//")[0].split())
    if hashlib.sha256(raw).hexdigest() != ACTIVE_ROM_SHA256:
        raise ValueError("native ROM identity changed")
    shutil.copyfile(NATIVE_ROM, out / NATIVE_ROM.name)

    fixture = json.loads(FIXTURE.read_text(encoding="utf-8"))
    traces = [make_demo("hello_four", filter_frequency=231)]
    for name, rate in (("B", 11), ("E", 12)):
        phrase = next(p for p in fixture["translated_player"]["phrases"] if p["id"] == name)
        traces.append(make_trace(fixture, phrase, rate))
    paths = []
    for index, trace in enumerate(traces):
        path = out / f"listening-{index}.txt"
        path.write_text(f"SSIHOST1 {trace.xck_hz} 0 {trace.duration_ticks} {len(trace.events)}\n" +
                        "".join(f"{e.tick} {e.socket} {e.register} {e.value}\n" for e in trace.events))
        paths.append(path)

    results = []
    for reference in args.art_reference:
        directory = out / f"art-{reference}"
        directory.mkdir(parents=True, exist_ok=True)
        command = ["verilator", "--cc", "--exe", "--build", "-j", "4", "--Wall",
                   "-Wno-DECLFILENAME", "--top-module", "ssi263_native_controller",
                   f"-GART_REFERENCE_RATE={reference}", "--Mdir", linux_path(directory),
                   "-CFLAGS", f"-std=c++17 -O2 -DTEST_ART_REFERENCE_RATE={reference} -I{linux_path(HOST)}",
                   linux_path(RTL), linux_path(HARNESS), linux_path(HOST / "native_control.cpp"),
                   linux_path(HOST / "native_source.cpp")]
        run(command, out, directory / "compile.log")
        executable = directory / "Vssi263_native_controller"
        run_args = [linux_path(executable), linux_path(NATIVE_ROM)]
        if reference == 8:
            run_args.extend(linux_path(path) for path in paths)
        output = run(run_args, out, directory / "simulation.log")
        report = next(json.loads(line) for line in output.splitlines() if line.startswith('{"status"'))
        results.append(report)
        print(f"ART reference {reference}: {report['observations']:,} fabric observations, "
              f"{report['checks']:,} exact state checks", flush=True)

    report = {"status": "passed", "results": results,
              "native_rom_sha256": ACTIVE_ROM_SHA256,
              "source_sha256": {str(path.relative_to(ROOT)).replace('\\', '/'): digest(path)
                                for path in (RTL, HARNESS, HOST / "native_control.h",
                                             HOST / "native_control.cpp", HOST / "native_source.h",
                                             HOST / "native_source.cpp")},
              "coverage": ["all 64 native ROM phones", "all RATE, DUR and ART fields",
                           "all register addresses including FF aliases 4..7",
                           "known/unknown route state", "all scanner phases with coincident writes",
                           "CTL stop/restart, AMP=0, duration and FF boundaries",
                           "ordered random writes, fabric stalls, active resets",
                           "frozen-host U68 feedback during directed listening traces"],
              "limits": ["Preserves current host assumptions; no physical SSI fidelity claim.",
                         "No production audio wrapper or FPGA timing closure tested here."]}
    if args.synth:
        vivado = shutil.which("vivado.bat") or shutil.which("vivado")
        if not vivado:
            raise RuntimeError("--synth needs Vivado on PATH")
        tcl = out / "synth_controller.tcl"
        tcl.write_text(f"read_verilog -sv {{{RTL.as_posix()}}}\n"
                       "synth_design -top ssi263_native_controller -part xc7z020clg484-2 -mode out_of_context\n"
                       "create_clock -name clk -period 7.5 [get_ports clk]\n"
                       "report_utilization -file controller_utilization.rpt\n"
                       "report_timing_summary -file controller_timing.rpt\n"
                       "write_checkpoint -force controller_ooc.dcp\n"
                       "puts {SSI263 NATIVE CONTROLLER SYNTHESIS PASS}\n")
        result = subprocess.run([vivado, "-mode", "batch", "-source", str(tcl),
                                 "-log", "controller_vivado.log", "-journal", "controller_vivado.jou"],
                                cwd=out, capture_output=True, text=True)
        (out / "synthesis.log").write_text(result.stdout + result.stderr)
        if result.returncode or "SSI263 NATIVE CONTROLLER SYNTHESIS PASS" not in result.stdout:
            raise RuntimeError("standalone controller synthesis failed; see synthesis.log")
        timing = (out / "controller_timing.rpt").read_text().splitlines()
        slack = next(float(timing[index + 2].split()[0]) for index, line in enumerate(timing)
                     if "WNS(ns)" in line and "TNS(ns)" in line)
        report["standalone_synthesis"] = {"passed": True, "part": "xc7z020clg484-2",
                                          "clock_period_ns": 7.5, "routing_performed": False,
                                          "post_synthesis_setup_slack_ns": slack,
                                          "timing_closure_claimed": False}
    (out / "validation.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
