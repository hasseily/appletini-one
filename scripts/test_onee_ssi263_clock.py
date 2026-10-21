#!/usr/bin/env python3
"""Prove standalone SSI clocks, audio and completion through the real virtual bus."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build" / "onee_ssi263_clock_sim"
SOURCES = [
    "hdl/globals.sv", "hdl/cdc_bus_sampled.sv",
    "hdl/apple/soft_switch_manager.sv", "hdl/apple/apple_virtual_bus.sv",
    "hdl/apple/ssi263_parameter_rom.sv", "hdl/apple/ssi263_native_controller.sv",
    "hdl/apple/ssi263_native_pitch.sv", "hdl/apple/ssi263_native_source.sv",
    "hdl/apple/ssi263_native_tract.sv", "hdl/apple/ssi263_native_engine.sv",
    "hdl/apple/ssi263_response_timing.sv", "hdl/apple/ssi263_stereo_mixer.sv",
    "hdl/apple/ssi263_bus_wrapper.sv", "hdl/apple/ssi263_voice.sv",
    "hdl/apple/ssi263_xck_ce.sv", "hdl/apple/via6522.v", "hdl/apple/YM2149.sv",
    "hdl/apple/mockingboard.sv", "hdl/sim/tb_onee_ssi263_clock.sv",
]
ROM = "hdl/apple/ssi263_sc02_rom.mem"


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(tool: str, arguments: list[str]) -> str:
    executable = shutil.which(f"{tool}.bat") or shutil.which(tool)
    if not executable:
        raise RuntimeError(f"Cannot find Vivado simulator {tool}")
    result = subprocess.run([executable, *arguments], cwd=OUT, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=600)
    log = OUT / f"{tool}.log"
    log.write_text(result.stdout, encoding="utf-8")
    if result.returncode or re.search(r"\b(?:fatal|error)\s*:", result.stdout, re.IGNORECASE):
        raise RuntimeError(f"{tool} failed; see {log}\n{result.stdout[-6000:]}")
    return result.stdout


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    hashes = {name: sha(ROOT / name) for name in [*SOURCES, ROM]}
    shutil.copyfile(ROOT / ROM, OUT / Path(ROM).name)
    start = time.monotonic()
    run("xvlog", ["--sv", *[str(ROOT / name) for name in SOURCES]])
    run("xelab", ["tb_onee_ssi263_clock", "-s", "tb_onee_ssi263_clock_snap",
                  "--timescale", "1ns/1ps", "-L", "unisims_ver"])
    output = run("xsim", ["tb_onee_ssi263_clock_snap", "--runall"])
    match = re.search(r"ONEE SSI263 CLOCK PASS checks=(\d+) standards=(\d+) transitions=(\d+) "
                      r"pending=(\d+) transfers=(\d+) min_xck=(\d+) min_div2=(\d+)", output)
    if not match or "ONEE SSI263 CLOCK FAIL" in output:
        raise RuntimeError(f"Missing directed RTL pass; see {OUT / 'xsim.log'}\n{output[-6000:]}")
    names = ("checks", "standards", "transitions", "pending", "transfers", "minimum_xck", "minimum_div2")
    counts = dict(zip(names, map(int, match.groups())))
    if counts["standards"] != 2 or counts["transitions"] != 131 or counts["pending"] != 2:
        raise RuntimeError(f"Incomplete directed coverage: {counts}")
    for name, digest in hashes.items():
        if sha(ROOT / name) != digest:
            raise RuntimeError(f"Source changed during simulation: {name}")
    report = {
        "status": "PASS", "counts": counts, "seconds": round(time.monotonic() - start, 3),
        "coverage": "Real virtual bus and Mockingboard with two native SSI263AP engines; physical Q3 stopped low/high; standalone idle cadence at NTSC130/PAL131; both voices progress, produce audio, complete and assert virtual IRQ; phase sweep and pending mode/reset transitions",
        "limitation": "Directed digital RTL simulation; no board, physical audio or complete ONE//e CPU/software proof",
        "source_sha256": hashes,
    }
    (OUT / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print("ONEE SSI263 CLOCK PASS " + json.dumps(counts))


if __name__ == "__main__":
    main()
