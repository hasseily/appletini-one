#!/usr/bin/env python3
"""Build and run the joined ONE//e native video-path regression."""

from __future__ import annotations

import re
import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "build" / "onee_video_path_sim"

SOURCES = [
    "hdl/globals.sv",
    "hdl/reset_sync.sv",
    "hdl/apple/apple_cycle_capture_pkg.sv",
    "hdl/sim/xpm_fifo_sync_model.sv",
    "hdl/apple/apple_timing_gen.sv",
    "hdl/apple/apple_virtual_bus.sv",
    "hdl/apple/apple_bus_write_arbiter.sv",
    "hdl/apple/soft_switch_manager.sv",
    "hdl/apple/onee_motherboard_io.sv",
    "hdl/apple/onee_cold_slot_scan.sv",
    "hdl/apple/apple_cycle_capture.sv",
    "hdl/apple/vtw_shadow.sv",
    "hdl/apple/vtw_bus_engine.sv",
    "hdl/apple/w65c02_core.sv",
    "hdl/apple/vtw_core_top.sv",
    "hdl/sim/tb_onee_video_path.sv",
    "hdl/sim/tb_onee_cold_slot_scan.sv",
    "hdl/sim/tb_onee_rom_cold_boot.sv",
]


def vivado_tool(name: str) -> str:
    tool = shutil.which(f"{name}.bat") or shutil.which(name)
    if tool:
        return tool
    raise FileNotFoundError(f"unable to locate Vivado tool {name}")


def run(command: list[str], log_name: str) -> str:
    completed = subprocess.run(
        command,
        cwd=OUT_DIR,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    (OUT_DIR / log_name).write_text(completed.stdout, encoding="utf-8")
    if completed.returncode != 0:
        print(completed.stdout)
        raise RuntimeError(
            f"{Path(command[0]).name} failed with {completed.returncode}"
        )
    return completed.stdout


def make_embedded_rom_mem() -> None:
    source = ROOT / "ps_sources" / "frontend" / "apple2e_cpu_rom_data.c"
    values = [
        int(value, 16)
        for value in re.findall(r"0x([0-9A-Fa-f]{2})", source.read_text())
    ]
    if len(values) != 16384:
        raise RuntimeError(
            f"embedded Enhanced //e ROM has {len(values)} bytes, want 16384"
        )
    if values[0x3FFC:0x3FFE] != [0x62, 0xFA]:
        raise RuntimeError("embedded Enhanced //e ROM reset vector is not $FA62")
    (OUT_DIR / "onee_enhanced_cpu_rom.mem").write_text(
        "".join(f"{value:02X}\n" for value in values),
        encoding="ascii",
    )


def run_bench(top: str, snapshot: str, marker: str) -> None:
    run(
        [
            vivado_tool("xelab"),
            top,
            "-s",
            snapshot,
            "--timescale",
            "1ns/1ps",
            "-L",
            "unisims_ver",
        ],
        f"xelab_{top}.log",
    )
    output = run(
        [vivado_tool("xsim"), snapshot, "--runall"],
        f"xsim_{top}.log",
    )
    if marker not in output:
        print(output)
        raise RuntimeError(f"{top} did not report success")


def main() -> int:
    if OUT_DIR.exists():
        shutil.rmtree(OUT_DIR)
    OUT_DIR.mkdir(parents=True)
    make_embedded_rom_mem()
    shutil.copy2(ROOT / "hdl" / "apple" / "disk2_slot6.mem", OUT_DIR)
    shutil.copy2(
        ROOT / "hdl" / "apple" / "smartport_a2retronet_style_c700.mem",
        OUT_DIR,
    )

    run(
        [vivado_tool("xvlog"), "--sv", *[str(ROOT / src) for src in SOURCES]],
        "xvlog.log",
    )
    run_bench(
        "tb_onee_video_path",
        "onee_video_path_snap",
        "ONEE VIDEO PATH PASS",
    )
    run_bench(
        "tb_onee_cold_slot_scan",
        "onee_cold_slot_scan_snap",
        "ONEE COLD SLOT SCAN RESET PASS",
    )
    run_bench(
        "tb_onee_rom_cold_boot_disk2",
        "onee_rom_disk2_boot_path_snap",
        "ONEE ROM DISK2 BOOT PATH PASS",
    )
    run_bench(
        "tb_onee_rom_cold_boot_smartport",
        "onee_rom_smartport_boot_path_snap",
        "ONEE ROM SMARTPORT BOOT PATH PASS",
    )

    print("ONE//e video path and both real-ROM boot-target paths passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
