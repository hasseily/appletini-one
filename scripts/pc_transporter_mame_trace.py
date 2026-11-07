#!/usr/bin/env python3
"""Record a bounded MAME startup of the original PC Transporter 2.0.5 disk."""

import argparse
from collections import Counter
import csv
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile


DISK_SIZE = 819200
DISK_SHA256 = "96f682de1f49d89279fb5ea2def1c89298eb7f72efa2b41e483c2c7beb81619d"
LUA_SCRIPT = Path(__file__).with_suffix(".lua")
ROUTING_WRITERS = (0x7311, 0x7316, 0x7324, 0x7327)


def file_hash(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def read_disk(path):
    if path.suffix.lower() == ".zip":
        with zipfile.ZipFile(path) as archive:
            candidates = [
                item for item in archive.infolist()
                if item.filename.lower().endswith(".po") and item.file_size == DISK_SIZE
            ]
            if len(candidates) != 1:
                raise ValueError("the archive must contain one 800 KiB ProDOS-order disk")
            image = archive.read(candidates[0])
    else:
        if path.stat().st_size != DISK_SIZE:
            raise ValueError("the disk must be the original 800 KiB ProDOS-order image")
        image = path.read_bytes()
    if hashlib.sha256(image).hexdigest() != DISK_SHA256:
        raise ValueError("disk SHA-256 does not match the original 2.0.5 image")
    return image


def summarize(path):
    totals = Counter()
    calibration = Counter()
    milestones = {}
    routing = {}
    pointer = 0
    last_apple = None
    with path.open(newline="", encoding="ascii") as source:
        for event in csv.DictReader(source, delimiter="\t"):
            bus = event["bus"]
            count = int(event["count"])
            address = int(event["address"], 16)
            pc = int(event["pc"], 16)
            totals[bus] += count
            if bus == "apple-slot" and event["address"] == "C500":
                milestones.setdefault("first_slot_5_access", event)
            if bus == "apple-c8":
                last_apple = event
                value = int(event["data"], 16)
                if 0xCF00 <= address <= 0xCF02 and event["direction"] == "w":
                    shift = 8 * (address - 0xCF00)
                    pointer = (pointer & ~(0xFF << shift)) | (value << shift)
                if address == 0xCF03:
                    if event["direction"] == "w" and pc in ROUTING_WRITERS:
                        for offset in range(pointer, pointer + count):
                            if 0xBF400 <= offset < 0xBF800:
                                routing[offset - 0xBF400] = value
                    pointer += count
                if address == 0xCF37 and event["direction"] == "w":
                    milestones.setdefault("first_CF37_write", event)
                if address == 0xCF31 and event["direction"] == "w" and pc == 0x6F5D:
                    milestones.setdefault("first_calibration_gate_write", event)
                if address == 0xCF31 and event["direction"] == "r" and 0x6F48 <= pc < 0x6F90:
                    milestones.setdefault("first_calibration_poll", event)
                    calibration[event["data"]] += count
                if address == 0xCF34 and event["direction"] == "r" and pc == 0x2179:
                    milestones.setdefault("first_portware_request_low_byte", event)
                if address == 0xCF32 and event["direction"] == "w" and pc == 0x2192:
                    milestones.setdefault("first_portware_ack", event)
            if bus in ("apple-keyboard", "pc-io", "v30-reset"):
                milestones.setdefault("first_" + bus, event)
            if bus == "v30-reset" and address == 0xFFFF0 and event["direction"] == "r":
                if int(event["mask"], 16) & 0xFF and int(event["data"], 16) & 0xFF == 0xEA:
                    milestones.setdefault("first_reset_vector_far_jump_byte", event)
    summary = {
        "events_by_bus": dict(totals),
        "milestones": milestones,
        "calibration_CF31_values": dict(calibration),
        "last_apple_c8_access": last_apple,
    }
    if routing:
        summary["routing"] = write_routing(path.with_name("routing.tsv"), routing)
    return summary


def write_routing(path, routing):
    """Decode writes from the 2.0.5 table loader with the patent's nibble layout."""
    nibbles = Counter()
    with path.open("w", newline="", encoding="ascii") as output:
        rows = csv.writer(output, delimiter="\t", lineterminator="\n")
        rows.writerow(["port", "control", "companion", "write_wait", "write_irq", "read_wait", "read_irq"])
        for port in range(0x400):
            if port & ~1 not in routing:
                continue
            control = routing[port & ~1]
            nibble = (control >> (4 * (port & 1))) & 0xF
            nibbles[nibble] += 1
            companion = routing.get(port | 1)
            rows.writerow([
                f"{port:03X}", f"{control:02X}", "" if companion is None else f"{companion:02X}",
                *(int(bool(nibble & (1 << bit))) for bit in range(4)),
            ])
    evidence = {
        "bytes_written_by_loader": len(routing),
        "decoded_ports_by_nibble": {f"{value:X}": count for value, count in sorted(nibbles.items())},
        "decoded_sha256": file_hash(path),
    }
    if len(routing) == 0x400:
        raw = bytes(routing[offset] for offset in range(0x400))
        path.with_suffix(".bin").write_bytes(raw)
        evidence["raw_sha256"] = hashlib.sha256(raw).hexdigest()
    return evidence


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mame", required=True, type=Path, help="MAME executable")
    parser.add_argument("--rompath", required=True, type=Path, help="directory with the MAME ROM sets")
    parser.add_argument("--disk", required=True, type=Path, help="original 2.0.5 ZIP or .po")
    parser.add_argument("--dos-floppy", type=Path, help="optional raw 360 KiB PC boot disk")
    parser.add_argument("--out", required=True, type=Path, help="new directory for this run")
    parser.add_argument("--seconds", type=int, default=20, help="emulated seconds to capture")
    parser.add_argument("--source-revision", help="source revision used to build MAME, if known")
    parser.add_argument("--keys", help="optional Apple input, with a literal \\n for Return")
    parser.add_argument("--key-time", type=float, default=10.0, help="emulated seconds before posting keys")
    args = parser.parse_args()
    if args.seconds < 1:
        parser.error("--seconds must be positive")
    if args.keys and not 0 <= args.key_time < args.seconds:
        parser.error("--key-time must fall within the capture")
    try:
        mame = args.mame.resolve(strict=True)
        rompath = args.rompath.resolve(strict=True)
        if not rompath.is_dir():
            raise ValueError("--rompath must be a directory")
        disk = args.disk.resolve(strict=True)
        floppy = args.dos_floppy.resolve(strict=True) if args.dos_floppy else None
        if floppy and floppy.stat().st_size != 360 * 1024:
            raise ValueError("--dos-floppy must be a raw 360 KiB image")
        output = args.out.resolve()
        image = read_disk(disk)
        output.mkdir(parents=True)
    except (OSError, ValueError, zipfile.BadZipFile) as error:
        parser.error(str(error))

    # CFFA2 accepts raw .hdv media. The copy keeps the ProDOS blocks in their original order.
    media = output / "startup.hdv"
    media.write_bytes(image)
    command = [
        str(mame), "apple2ee", "-noreadconfig", "-nowriteconfig", "-noplugins",
        "-sl4", "", "-sl5", "pcxport", "-sl6", "", "-sl7", "cffa2",
        "-rompath", str(rompath), "-hard1", str(media),
        "-autoboot_delay", "0", "-autoboot_script", str(LUA_SCRIPT),
        "-seconds_to_run", str(args.seconds), "-nothrottle", "-skip_gameinfo",
        "-video", "none", "-sound", "none",
    ]
    if floppy:
        floppy_copy = output / "pc-boot.img"
        shutil.copyfile(floppy, floppy_copy)
        command.extend(["-flop1", str(floppy_copy)])
    env = os.environ.copy()
    env.pop("PCT_TRACE_KEYS", None)
    env.pop("PCT_TRACE_KEY_TIME", None)
    if args.keys:
        env["PCT_TRACE_KEYS"] = args.keys.replace(r"\n", "\n")
        env["PCT_TRACE_KEY_TIME"] = str(args.key_time)

    version = subprocess.check_output(
        [str(mame), "-version"], cwd=output, text=True, errors="replace"
    ).strip()
    evidence = {
        "kind": "MAME execution",
        "version": version,
        "source_revision": args.source_revision,
        "executable_sha256": file_hash(mame),
        "capture_script_sha256": file_hash(LUA_SCRIPT),
        "source_media_sha256": file_hash(disk),
        "startup_disk_sha256": DISK_SHA256,
        "dos_floppy_sha256": file_hash(floppy) if floppy else None,
        "rom_inputs": {
            path.relative_to(rompath).as_posix(): file_hash(path)
            for path in sorted(rompath.rglob("*")) if path.is_file()
        },
        "command": command,
        "key_time_s": args.key_time if args.keys else None,
        "keys": args.keys,
    }
    with (output / "mame.log").open("wb") as log:
        try:
            result = subprocess.run(
                command, cwd=output, env=env, stdout=log, stderr=subprocess.STDOUT,
                timeout=max(60, args.seconds * 5), check=False,
            )
            evidence["exit_code"] = result.returncode
        except subprocess.TimeoutExpired:
            evidence["error"] = "MAME exceeded the wall-time limit"
    evidence["startup_disk_sha256_after"] = file_hash(media)
    if floppy:
        evidence["dos_floppy_sha256_after"] = file_hash(floppy_copy)
    trace = output / "bus.tsv"
    complete = output / "capture.complete"
    if complete.exists() and trace.exists():
        end_time, apple_pc, v30_pc = complete.read_text(encoding="ascii").splitlines()
        evidence["captured_through_s"] = float(end_time)
        evidence["apple_pc_at_stop"] = apple_pc
        evidence["v30_pc_at_stop"] = v30_pc
        evidence["capture"] = summarize(trace)
        if evidence["captured_through_s"] < args.seconds or not evidence["capture"]["events_by_bus"]:
            evidence.setdefault("error", "the trace ended early or contained no bus events")
    else:
        evidence.setdefault("error", "MAME did not finish the Lua capture")
    (output / "run.json").write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
    print(output / "run.json")
    return int(bool(evidence.get("exit_code") or "error" in evidence))


if __name__ == "__main__":
    sys.exit(main())
