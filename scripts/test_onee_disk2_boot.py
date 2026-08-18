#!/usr/bin/env python3
"""Run the real-ROM ONE//e boot through the production virtual Disk II path."""

from __future__ import annotations

import re
import shutil
import subprocess
import sys
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "build" / "onee_disk2_boot_sim"

BOOT_CASES = [
    (
        "DOS 3.3 System Master",
        ROOT / "software" / "DOS 3.3 System Master.dsk",
        False,
        bytes((0x01, 0xA5, 0x27, 0xC9)),
    ),
    (
        "ProDOS 2.4.3",
        ROOT / "software" / "ProDOS_2_4_3.po",
        True,
        bytes((0x01, 0x38, 0xB0, 0x03)),
    ),
]

sys.path.insert(0, str(ROOT / "scripts"))
from test_disk2_standard import (  # noqa: E402
    decode44_pair,
    decode_6and2,
    nibblize_track,
)


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
    "hdl/apple/vtw_shadow.sv",
    "hdl/apple/vtw_bus_engine.sv",
    "hdl/apple/w65c02_core.sv",
    "hdl/apple/vtw_core_top.sv",
    "hdl/apple/disk2_card.sv",
    "hdl/sim/tb_onee_disk2_boot.sv",
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


def make_rom_mem() -> None:
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
        "".join(f"{value:02X}\n" for value in values), encoding="ascii"
    )


def validate_track(track: bytes, boot_sector: bytes) -> tuple[int, int, list[int]]:
    """Check the exact address/data prologues consumed by the slot-6 ROM."""
    first_address = track.find(b"\xD5\xAA\x96")
    if first_address < 0:
        raise RuntimeError("nibblized track has no D5 AA 96 address prologue")
    encoded_address = track[first_address + 3:first_address + 11]
    decoded_address = [decode44_pair(encoded_address, offset) for offset in range(0, 8, 2)]
    volume, track_number, sector, checksum = decoded_address
    if track_number != 0 or sector != 0 or checksum != (volume ^ track_number ^ sector):
        raise RuntimeError(f"invalid first address field {decoded_address!r}")

    first_data = track.find(b"\xD5\xAA\xAD", first_address + 11)
    if first_data < 0:
        raise RuntimeError("nibblized track has no D5 AA AD data prologue")
    decoded_sector = decode_6and2(track[first_data + 3:first_data + 3 + 343])
    if decoded_sector != boot_sector:
        raise RuntimeError("nibblized physical sector 0 does not decode to file sector 0")

    return first_address, first_data, decoded_address


def make_track_mem(
    label: str,
    image_path: Path,
    is_prodos: bool,
    expected_signature: bytes,
) -> None:
    image = image_path.read_bytes()
    if len(image) != 35 * 16 * 256:
        raise RuntimeError(f"{image_path.name} has unexpected size {len(image)}")
    if image[:4] != expected_signature:
        raise RuntimeError(
            f"{label} boot-sector signature changed: {image[:4].hex()}"
        )
    track = nibblize_track(
        image[: 16 * 256], track=0, is_prodos=is_prodos
    )
    if len(track) != 6384:
        raise RuntimeError(f"nibblized track 0 has {len(track)} bytes, want 6384")
    address_pos, data_pos, decoded_address = validate_track(track, image[:256])
    (OUT_DIR / "onee_disk_track0.mem").write_text(
        "".join(f"{value:02X}\n" for value in track), encoding="ascii"
    )
    (OUT_DIR / "onee_expected_boot.mem").write_text(
        "".join(f"{value:02X}\n" for value in expected_signature),
        encoding="ascii",
    )
    print(
        f"{label} track: D5 AA 96 at {address_pos}, "
        f"address={decoded_address}, D5 AA AD at {data_pos}, "
        f"$0800={expected_signature.hex().upper()}"
    )


def main() -> int:
    if OUT_DIR.exists():
        shutil.rmtree(OUT_DIR)
    OUT_DIR.mkdir(parents=True)
    make_rom_mem()
    make_track_mem(*BOOT_CASES[0])
    shutil.copy2(ROOT / "hdl" / "apple" / "disk2_slot6.mem", OUT_DIR)

    run(
        [vivado_tool("xvlog"), "--sv", *[str(ROOT / src) for src in SOURCES]],
        "xvlog.log",
    )
    run(
        [
            vivado_tool("xelab"),
            "tb_onee_disk2_boot",
            "-s",
            "onee_disk2_boot_snap",
            "--timescale",
            "1ns/1ps",
            "-L",
            "unisims_ver",
        ],
        "xelab.log",
    )
    marker = "ONEE DISK2 BOOT PASS"
    for index, boot_case in enumerate(BOOT_CASES):
        label = boot_case[0]
        if index != 0:
            make_track_mem(*boot_case)
        start = time.perf_counter()
        output = run(
            [vivado_tool("xsim"), "onee_disk2_boot_snap", "--runall"],
            f"xsim_{index + 1}.log",
        )
        elapsed = time.perf_counter() - start
        if marker not in output:
            print(output)
            raise RuntimeError(f"{label} did not enter its disk boot sector")
        summary = next(line.strip() for line in output.splitlines() if marker in line)
        print(f"{label}: {summary} wall={elapsed:.1f}s")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
