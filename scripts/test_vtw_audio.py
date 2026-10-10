#!/usr/bin/env python3
"""Run vTW sound slowdown and actual VIA/AY write tests with Verilator.

Run from any directory: python3 scripts/test_vtw_audio.py
No Vivado is needed. Test-only LUT truth tables replace the Xilinx primitives;
the core, bus wrapper, VIA, AY and Phasor logic are the production RTL.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

import test_phasor_demo_filter as phasor
import test_vtw as vtw

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/vtw_audio_sim"


def run(command: list[str], directory: Path, log: str) -> str:
    result = subprocess.run(command, cwd=directory, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (directory / log).write_text(result.stdout, encoding="utf-8")
    if result.returncode:
        raise RuntimeError(f"{log}: exit {result.returncode}\n{result.stdout[-4000:]}")
    return result.stdout


def main() -> None:
    verilator = shutil.which(os.environ.get("VERILATOR", "verilator"))
    if not verilator:
        raise RuntimeError("Verilator is required (or set VERILATOR to its executable)")
    OUT.mkdir(parents=True, exist_ok=True)
    vtw.static_checks()
    core_sources = [ROOT / name for name in vtw.SOURCES
                    if "/sim/" not in name or name.endswith("/tb_vtw_slowdown.sv")]
    core_sources.append(ROOT / "hdl/sim/tb_xilinx_lut_models.sv")
    card_sources = list(dict.fromkeys(core_sources + [
        source for source in phasor.SOURCES if "/sim/" not in str(source)
    ] + [ROOT / "hdl/sim/tb_vtw_sound_writes.sv"]))
    tracked = card_sources + [phasor.ROM, Path(__file__).resolve()]
    hashes = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
              for p in tracked}
    cases = [("policy", "tb_vtw_slowdown", [], core_sources, "VTW SLOWDOWN PASS")]
    for speed, label in ((0, "full"), (3, "turbo")):
        for indexed in (0, 1):
            cases.append((f"{label}-{'indexed' if indexed else 'absolute'}",
                          "tb_vtw_sound_writes",
                          [f"-GCORE_SPEED={speed}", f"-GINDEXED_STORES={indexed}"],
                          card_sources, "VTW SOUND WRITES PASS"))
    results = {}
    for name, top, parameters, sources, marker in cases:
        directory = OUT / name
        directory.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(phasor.ROM, directory / phasor.ROM.name)
        command = [verilator, "--binary", "--timing", "--assert", "-Wno-fatal",
                   "--build-jobs", "2", "--top-module", top,
                   "--Mdir", str(directory / "obj"), "-o", "test_audio",
                   *parameters, *map(str, sources)]
        (directory / "command.json").write_text(json.dumps(command, indent=2) + "\n")
        run(command, directory, "compile.log")
        output = run([str(directory / "obj/test_audio")], directory, "run.log")
        if marker not in output or " FAIL" in output:
            raise RuntimeError(f"{name} failed\n{output[-4000:]}")
        results[name] = [line for line in output.splitlines()
                         if "SOUND " in line or marker in line]
        print(f"PASS {name}: {results[name][-1]}", flush=True)
    for name, digest in hashes.items():
        if hashlib.sha256((ROOT / name).read_bytes()).hexdigest() != digest:
            raise RuntimeError(f"Source changed during simulation: {name}; rerun")
    report = {
        "status": "PASS", "static_checks": "PASS", "cases": results,
        "source_sha256": hashes,
        "scope": "Functional RTL simulation, including the real 65C02/bus/VIA/AY path. "
                 "No electrical timing, synthesis, bitstream or board validation.",
    }
    (OUT / "report.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
