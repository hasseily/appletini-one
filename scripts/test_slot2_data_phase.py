#!/usr/bin/env python3
"""Check captured Apple data against live-bus Mouse and boot-menu consumers."""

from pathlib import Path
import re
import shutil

from test_w65c02_core import run, vivado_tool


ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build" / "slot2_data_phase"
SNAPSHOT = "tb_slot2_data_phase_snapshot"


def compact_source(path: str) -> str:
    source = (ROOT / path).read_text(encoding="utf-8")
    source = re.sub(r"/\*.*?\*/|//[^\n]*", "", source, flags=re.S)
    return re.sub(r"\s+", "", source)


def source_checks() -> None:
    bus = compact_source("hdl/apple/apple_virtual_bus.sv")
    top = compact_source("hdl/apple/apple_top.sv")
    required_bus = (
        "assigndata_phase_data=cycle_data_q;",
        "ab_read.data=phase_data_q?cycle_data_q:bus_data_live;",
        "ab_read.data_en=phase_data;",
        "wirephase_data=phase_data_q;",
    )
    required_top = (
        ".data_phase_data(virtual_data_phase_data)",
        "data_phase_ab_read=ab_read;",
        "data_phase_ab_read.data=onee_enable_effective?"
        "virtual_data_phase_data:physical_ab_read.data;",
        ".ab_read(gate_ab(data_phase_ab_read,card_slot2_bus_enable))",
        "boot_menu_ab_read=gate_ab(slot7_devsel_ab_read,!onee_enable_effective);",
        "boot_menu_ab_read.data=physical_ab_read.data;",
        ".ab_read(boot_menu_ab_read)",
    )
    for token in required_bus:
        if token not in bus:
            raise RuntimeError(f"captured-byte bus contract changed: {token}")
    for token in required_top:
        if token not in top:
            raise RuntimeError(f"captured-byte top routing changed: {token}")
    print("PASS captured-byte export and top-level routing checks")


def main() -> int:
    source_checks()
    OUT.mkdir(parents=True, exist_ok=True)
    for name in ("mouse_card_slot2.mem", "boot_menu_slot7.mem",
                 "boot_menu_slot7_reloc.mem", "boot_menu_slot7_c8.mem"):
        shutil.copyfile(ROOT / "hdl" / "apple" / name, OUT / name)
    sources = (
        "hdl/globals.sv", "hdl/apple/apple_virtual_bus.sv",
        "hdl/apple/mouse_card.sv", "hdl/apple/boot_menu_card.sv",
        "hdl/sim/tb_slot2_data_phase.sv",
    )
    run([vivado_tool("xvlog"), "--sv", *[str(ROOT / p) for p in sources]],
        OUT, OUT / "xvlog.log")
    run([vivado_tool("xelab"), "tb_slot2_data_phase", "-s", SNAPSHOT,
         "--timescale", "1ns/1ps"], OUT, OUT / "xelab.log")
    output = run([vivado_tool("xsim"), SNAPSHOT, "--runall"], OUT, OUT / "xsim.log")
    if "SLOT2 DATA PHASE PASS" not in output or "FAIL:" in output:
        print(output)
        raise RuntimeError("captured-byte integration simulation failed")
    print("PASS original/captured Mouse and boot-menu data-phase equivalence")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
