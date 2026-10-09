#!/usr/bin/env python3
"""Compare stock and vTW SSI263 register writes through the physical bus RTL."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build" / "vtw_ssi263_bus_sim"
SOURCES = [
    "hdl/globals.sv", "hdl/cdc_bus_sampled.sv",
    "hdl/apple/soft_switch_manager.sv", "hdl/apple/apple_bus_wrapper.sv",
    "hdl/apple/apple_bus_write_arbiter.sv", "hdl/apple/vtw_bus_engine.sv",
    "hdl/apple/ssi263_parameter_rom.sv", "hdl/apple/ssi263_native_controller.sv",
    "hdl/apple/ssi263_native_pitch.sv", "hdl/apple/ssi263_native_source.sv",
    "hdl/apple/ssi263_native_tract.sv", "hdl/apple/ssi263_native_engine.sv",
    "hdl/apple/ssi263_response_timing.sv", "hdl/apple/ssi263_stereo_mixer.sv",
    "hdl/apple/ssi263_bus_wrapper.sv", "hdl/apple/ssi263_voice.sv",
    "hdl/apple/ssi263_xck_ce.sv", "hdl/apple/via6522.v", "hdl/apple/YM2149.sv",
    "hdl/apple/mockingboard.sv", "hdl/sim/tb_vtw_ssi263_bus.sv",
]
ROM = "hdl/apple/ssi263_sc02_rom.mem"


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(tool: str, args: list[str]) -> str:
    executable = shutil.which(f"{tool}.bat") or shutil.which(tool)
    if not executable:
        raise RuntimeError(f"Cannot find Vivado simulator {tool}")
    result = subprocess.run([executable, *args], cwd=OUT, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    log = OUT / f"{tool}.log"
    log.write_text(result.stdout, encoding="utf-8")
    if result.returncode:
        raise RuntimeError(f"{tool} failed; see {log}\n{result.stdout[-5000:]}")
    return result.stdout


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    hashes = {name: sha(ROOT / name) for name in [*SOURCES, ROM]}
    shutil.copyfile(ROOT / ROM, OUT / Path(ROM).name)
    run("xvlog", ["--sv", *[str(ROOT / name) for name in SOURCES]])
    run("xelab", ["tb_vtw_ssi263_bus", "-s", "tb_vtw_ssi263_bus_snap",
                  "--timescale", "1ns/1ps", "-L", "unisims_ver"])
    text = run("xsim", ["tb_vtw_ssi263_bus_snap", "--runall"])
    match = re.search(r"VTW SSI263 BUS PASS checks=(\d+) transfers=(\d+) "
                      r"primary=(\d+) secondary=(\d+)", text)
    if not match or "VTW SSI263 BUS FAIL" in text:
        raise RuntimeError(f"Missing RTL pass; see {OUT / 'xsim.log'}\n{text[-5000:]}")
    counts = dict(zip(("checks", "transfers", "primary", "secondary"), map(int, match.groups())))
    for name, digest in hashes.items():
        if sha(ROOT / name) != digest:
            raise RuntimeError(f"Source changed during simulation: {name}")
    report = {
        "status": "PASS", "counts": counts,
        "coverage": "Stock and vTW physical bus; Mockingboard and Phasor modes; demo FF232 initialization; all 256 FF bytes at aliases 4..7 on primary, secondary and both sockets",
        "checks": "Exact-once socket and native engine events; matching register/data; native FF latches; vTW write loopback and address diagnostics",
        "limitation": "Clean digital pin model and direct vTW sync requests; no CPU program, electrical ringing, board or audio validation",
        "source_sha256": hashes,
    }
    (OUT / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print("VTW SSI263 BUS PASS " + json.dumps(counts))


if __name__ == "__main__":
    main()
