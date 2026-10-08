#!/usr/bin/env python3
"""Test the separate native SSI controller, ROM, clocks and source controls."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build/test_ssi263_control_core"
SOURCES = [ROOT / "hdl/apple" / filename for filename in
           ("ssi263_parameter_rom.sv", "ssi263_clock_core.sv",
            "ssi263_source_control.sv", "ssi263_control_core.sv")]
ROM = ROOT / "hdl/apple/ssi263_sc02_rom.mem"
BENCH = ROOT / "hdl/sim/tb_ssi263_control_core.sv"


def tool(name):
    found = shutil.which(f"{name}.bat") or shutil.which(name)
    if not found:
        raise RuntimeError(f"Vivado tool {name} not found on PATH")
    return found


def run(command, log, expected=None, reject=False):
    result = subprocess.run(command, cwd=BUILD, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    (BUILD / log).write_text(result.stdout, encoding="utf-8")
    if ((not reject and result.returncode) or
            (expected and expected not in result.stdout)):
        raise RuntimeError(f"{log} failed:\n{result.stdout}")
    if not reject and "Fatal:" in result.stdout:
        raise RuntimeError(f"Unexpected fatal error in {log}")
    return result.stdout


def source_checks():
    data = bytes(int(value, 16) for line in ROM.read_text().splitlines()
                 if (value := line.split("//", 1)[0].strip()))
    assert len(data) == 512
    assert hashlib.sha256(data).hexdigest() == "ea494f047de11c533cb36a51d8686949206cedb91ab4855bdf9bde9500f828d9"
    assert hashlib.sha256(data + bytes(1536)).hexdigest() == "849baa20baae3d756f26813cf4e4f47392573e735cb4c66afdc434f9932147e0"
    # Build and synthesize with only these native files: no inherited digital
    # core, table package or pulse source may be required to resolve a module.
    for source in SOURCES:
        code = re.sub(r"//[^\n]*|/\*.*?\*/", "", source.read_text(), flags=re.DOTALL)
        assert "sc01" not in code.lower(), source
        assert "audio_tick" not in code, source
    manifest = (ROOT / "hdl/hdl_sources.txt").read_text().splitlines()
    for source in (*SOURCES, ROM):
        relative = source.relative_to(ROOT / "hdl").as_posix()
        assert manifest.count(relative) == 1, relative
    shutil.copyfile(ROM, BUILD / ROM.name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--synth", action="store_true")
    args = parser.parse_args()
    BUILD.mkdir(parents=True, exist_ok=True)
    source_checks()
    run([tool("xvlog"), "--sv", *map(str, SOURCES), str(BENCH)], "compile.log")
    run([tool("xelab"), "tb_ssi263_control_core", "-s", "ssi263_control_core_sim"], "elaborate.log")
    output = run([tool("xsim"), "ssi263_control_core_sim", "--runall"], "simulation.log",
                 "SSI263 CONTROL CORE PASS")
    match = re.search(r"SSI263 CONTROL CORE PASS checks=(\d+) bytes=512 sockets=2", output)
    if not match:
        raise RuntimeError(output)
    for case, message in (("scan_before_phone", "establish a phone before scanning"),
                          ("phone_scan_overlap", "serialize phone and selector writes")):
        run([tool("xsim"), "ssi263_control_core_sim", "--runall",
             "-testplusarg", case], f"contract_{case}.log",
            f"SSI263 control contract: {message}", reject=True)
    if args.synth:
        tcl = BUILD / "synth_control_core.tcl"
        tcl.write_text(
            "".join(f"read_verilog -sv {{{path.as_posix()}}}\n" for path in SOURCES) +
            "synth_design -top ssi263_control_core -part xc7z020clg484-2 -mode out_of_context\n"
            "create_clock -name clk -period 7.5 [get_ports clk]\n"
            "report_utilization -file control_core_utilization.rpt\n"
            "report_timing_summary -file control_core_timing.rpt\n"
            "puts {SSI263 CONTROL SYNTHESIS PASS}\n", encoding="utf-8")
        run([tool("vivado"), "-mode", "batch", "-source", str(tcl)], "synthesis.log",
            "SSI263 CONTROL SYNTHESIS PASS")
    result = {"status": "passed", "checks": int(match.group(1)),
              "native_rom_bytes": 512, "sockets": 2, "contract_rejections": 2,
              "standalone_synthesis": args.synth, "production_audio_connected": False,
              "sha256": {str(path.relative_to(ROOT)).replace("\\", "/"):
                         hashlib.sha256(path.read_bytes()).hexdigest() for path in (*SOURCES, ROM, BENCH)}}
    (BUILD / "validation.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"SSI263 CONTROL CORE PASS: {result['checks']} checks, all 512 ROM bytes, two sockets, two phase contracts")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
