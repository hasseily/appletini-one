#!/usr/bin/env python3
"""Exercise GS Phasor slot ownership, relocation, VIA I/O, and PSG cadence."""
from pathlib import Path
import shutil

from test_iigs_bus_safety import extract_top_routing
from test_vtw_ssi263_bus import SOURCES, ROM
from test_w65c02_core import run, vivado_tool

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build" / "iigs_phasor_sim"


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    routing = extract_top_routing(OUT)
    shutil.copyfile(ROOT / ROM, OUT / Path(ROM).name)
    sources = [
        *(ROOT / name for name in SOURCES if not name.endswith("tb_vtw_ssi263_bus.sv")),
        ROOT / "hdl/apple/apple_slot7_devsel_guard.sv",
        routing,
        ROOT / "hdl/sim/tb_iigs_phasor.sv",
    ]
    run([vivado_tool("xvlog"), "--sv", *map(str, sources)], OUT, OUT / "xvlog.log")
    run([vivado_tool("xelab"), "tb_iigs_phasor", "-s", "iigs_phasor_snap",
         "--timescale", "1ns/1ps", "-L", "unisims_ver"], OUT, OUT / "xelab.log")
    output = run([vivado_tool("xsim"), "iigs_phasor_snap", "--runall"], OUT, OUT / "xsim.log")
    required = ("IIGS PURE NOISE T1 DETECTION PASS", "IIGS PHASOR VIA ACCESS PASS",
                "IIGS PHASOR CLOCK PASS", "IIGS PHASOR RELOCATED IRQ PASS")
    if any(marker not in output for marker in required) or "Fatal:" in output:
        raise RuntimeError(output)
    print("IIgs Phasor VIA and PSG clock checks passed")


if __name__ == "__main__":
    main()
