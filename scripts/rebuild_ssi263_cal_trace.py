#!/usr/bin/env python3
"""Rebuild the SSI-263 calibration write trace from the calibration disk.

The original package SSI263-CAL-PAL-01.zip (manifest, register list and CPU
write trace) is lost. The disk that the tester returned still holds the exact
player (CAL.SYSTEM), the stream (CAL.PAL) and the status log of the real run
(CAL.LOG). This tool runs that player in a 65C02 emulator (py65), with a
6522 timer model and a ProDOS MLI stub, and records every SSI-263 and AY
register write with its CPU cycle.

The SSI-263 status reads are fed from the real CAL.LOG, so the emulated run
follows the same code paths as the run on the real Phasor. The tool then
checks that the emulated log equals the real log, and that the emulated
writes equal an independent decode of the stream.

The output is a reconstruction. Its file hashes differ from the lost package,
and its segment names are stream record numbers, not the original test names.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import io
import json
from pathlib import Path
import zipfile

from py65.devices.mpu65c02 import MPU
from py65.memory import ObservableMemory

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_DISK = ROOT / "build/ssi263_calibration/returned/SSI263-CAL-real-hw.po"
DEFAULT_OUT = ROOT / "build/ssi263_calibration/reconstructed"
PAL_CPU_HZ = 1_015_625          # 14.25 MHz * 65 / 912, PAL //e average
POLL_LOOP_CYCLES = 24           # $2065..$20EA while a run waits for a tick
POLL_PC, POLL_READ_PC, IDLE_KEY_PC = 0x2065, 0x206A, 0x20E5


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def read_prodos(image: bytes) -> dict[str, bytes]:
    """Root-directory files of a ProDOS-order volume."""
    block = lambda n: image[n * 512:(n + 1) * 512]
    files, key, first = {}, 2, True
    while key:
        data = block(key)
        for slot in range(13):
            entry = data[4 + slot * 39:4 + (slot + 1) * 39]
            storage, length = entry[0] >> 4, entry[0] & 15
            if storage == 0 or (first and slot == 0):
                continue
            name = entry[1:1 + length].decode("ascii")
            pointer = entry[0x11] | entry[0x12] << 8
            eof = entry[0x15] | entry[0x16] << 8 | entry[0x17] << 16
            if storage == 1:
                blocks = [pointer]
            elif storage == 2:
                index = block(pointer)
                blocks = [index[i] | index[256 + i] << 8 for i in range(256)]
            else:
                continue
            files[name] = b"".join(block(n) if n else bytes(512) for n in blocks)[:eof]
        key, first = data[2] | data[3] << 8, False
    return files


def decode_stream(stream: bytes) -> tuple[dict, list[tuple[int, int, int, int, int]]]:
    """PHS1 stream -> header and (record, tick, target, register, value)."""
    if stream[:4] != b"PHS1":
        raise ValueError("not a PHS1 stream")
    header = {"tick_rate_hz": int.from_bytes(stream[4:6], "little"),
              "duration_ticks": int.from_bytes(stream[8:12], "little"),
              "records": int.from_bytes(stream[12:16], "little")}
    writes, position, tick = [], 16, 0
    for record in range(header["records"]):
        tick += int.from_bytes(stream[position:position + 2], "little")
        count = stream[position + 2]
        position += 3
        for _ in range(count):
            address, value = stream[position], stream[position + 1]
            position += 2
            writes.append((record, tick, address >> 4, address & 15, value))
    if position != len(stream):
        raise ValueError("stream length disagrees with its record count")
    return header, writes


def decode_log(log: bytes) -> list[tuple[int, int]]:
    """CAL.LOG -> (tick, state). State bit 0 is $C420 D7, bit 1 is $C440 D7."""
    if log[:4] != b"S26L":
        raise ValueError("not a calibration log")
    count = int.from_bytes(log[6:8], "little")
    return [(int.from_bytes(log[32 + 3 * n:34 + 3 * n], "little"), log[34 + 3 * n])
            for n in range(count)]


class Machine:
    """CAL.SYSTEM on a 65C02 with a Phasor in slot 4 (native mode)."""

    def __init__(self, files: dict[str, bytes], status_log: list[tuple[int, int]]):
        self.files = files
        self.status_log = status_log
        self.memory = ObservableMemory(subject=[0] * 0x10000)
        self.mpu = MPU(memory=self.memory, pc=0x2000)
        ram = self.memory._subject
        ram[0x2000:0x2000 + len(files["CAL.SYSTEM"])] = list(files["CAL.SYSTEM"])
        for address in range(0xC100, 0x10000):
            ram[address] = 0x60                      # any ROM call returns at once
        self.key = 0
        self.via2 = {"latch": 0, "counter_seen": 0, "acr": 0}
        self.timer_start = None                       # cycle of the T1C-H write
        self.next_underflow = None
        self.ifr6 = False
        self.period = None
        self.ay = {0: {"ora": 0, "address": 0}, 1: {"ora": 0, "address": 0}}
        self.pending = []                             # writes made by this instruction
        self.events = []                              # (end cycle, target, reg, value, pc)
        self.poll_entry = None
        self.tick_entries = []                        # poll-entry cycle of each tick
        self.saved = {}                               # files the player wrote
        self.open_files = {}
        self.memory.subscribe_to_read(range(0xC000, 0xC100), self.read_io)
        self.memory.subscribe_to_write(range(0xC000, 0xC100), self.write_io)
        self.memory.subscribe_to_read(range(0xC400, 0xC500), self.read_card)
        self.memory.subscribe_to_write(range(0xC400, 0xC500), self.write_card)

    # -- timer ---------------------------------------------------------
    def update_timer(self, cycle: int) -> None:
        while self.next_underflow is not None and self.next_underflow <= cycle:
            self.ifr6 = True
            self.next_underflow += self.period

    def tick_now(self) -> int:
        ram = self.memory._subject
        return ram[0x1022] | ram[0x1023] << 8 | ram[0x1024] << 16 | ram[0x1025] << 24

    # -- bus hooks -----------------------------------------------------
    def read_io(self, address: int):
        return self.key if address == 0xC000 else 0

    def write_io(self, address: int, value: int):
        if 0xC010 <= address <= 0xC01F:
            self.key &= 0x7F

    def read_card(self, address: int):
        cycle = self.mpu.processorCycles + 3          # read on the fourth cycle
        if address in (0xC420, 0xC440):
            state, tick = 0, self.tick_now()
            for log_tick, log_state in self.status_log:
                if log_tick > tick:
                    break
                state = log_state
            bit = state & 1 if address == 0xC420 else (state >> 1) & 1
            return bit << 7
        if address == 0xC486:
            return self.via2["latch"] & 0xFF
        if address == 0xC487:
            return self.via2["latch"] >> 8
        if address == 0xC48D:
            self.update_timer(cycle)
            if self.mpu.pc - 1 == POLL_READ_PC or self.mpu.pc - 3 == POLL_READ_PC:
                self.poll_entry = self.mpu.processorCycles
            return 0xC0 if self.ifr6 else 0
        return 0

    def write_card(self, address: int, value: int):
        cycle = self.mpu.processorCycles + 3
        if 0xC420 <= address <= 0xC427:
            self.pending.append((4, address - 0xC420, value))
        elif 0xC440 <= address <= 0xC447:
            self.pending.append((5, address - 0xC440, value))
        elif address in (0xC41F, 0xC48F):
            self.ay[address == 0xC48F]["ora"] = value
        elif address in (0xC410, 0xC480):
            via = self.ay[address == 0xC480]
            if value & 3 == 3:                        # BDIR+BC1: latch address
                via["address"] = via["ora"]
            elif value & 3 == 2:                      # BDIR: write data
                chip = (address == 0xC480) + 2 * ((value >> 4) & 1)
                self.pending.append((chip, via["address"] & 15, via["ora"]))
        elif address == 0xC486:
            self.via2["latch"] = (self.via2["latch"] & 0xFF00) | value
        elif address == 0xC487:
            self.via2["latch"] = (self.via2["latch"] & 0x00FF) | value << 8
        elif address == 0xC484:
            self.via2["latch"] = (self.via2["latch"] & 0xFF00) | value
        elif address == 0xC485:
            self.via2["latch"] = (self.via2["latch"] & 0x00FF) | value << 8
            self.period = self.via2["latch"] + 2
            self.timer_start = self.mpu.processorCycles + 4   # end of STA abs
            self.next_underflow = self.timer_start + self.period
            self.ifr6 = False
        elif address == 0xC48B:
            self.via2["acr"] = value
            if not value & 0x40:
                self.next_underflow = None
        elif address == 0xC48D:
            self.update_timer(cycle)
            if value & 0x40:
                self.ifr6 = False

    # -- ProDOS MLI ----------------------------------------------------
    def mli(self) -> None:
        mpu, ram = self.mpu, self.memory._subject
        low = ram[0x100 + ((mpu.sp + 1) & 0xFF)]
        high = ram[0x100 + ((mpu.sp + 2) & 0xFF)]
        mpu.sp = (mpu.sp + 2) & 0xFF
        call = (low | high << 8) + 1
        command, params = ram[call], ram[call + 1] | ram[call + 2] << 8
        word = lambda offset: ram[params + offset] | ram[params + offset + 1] << 8
        if command == 0xC8:                           # OPEN
            pointer = word(1)
            name = bytes(ram[pointer + 1:pointer + 1 + ram[pointer]]).decode("ascii")
            name = name.rsplit("/", 1)[-1].upper()
            reference = len(self.open_files) + 1
            self.open_files[reference] = [name, 0]
            ram[params + 5] = reference
            if name not in self.files:
                self.files[name] = b""
        elif command == 0xD1:                         # GET_EOF
            size = len(self.files[self.open_files[ram[params + 1]][0]])
            ram[params + 2:params + 5] = [size & 255, size >> 8 & 255, size >> 16]
        elif command == 0xCA:                         # READ
            entry = self.open_files[ram[params + 1]]
            data = self.files[entry[0]][entry[1]:entry[1] + word(4)]
            ram[word(2):word(2) + len(data)] = list(data)
            entry[1] += len(data)
            ram[params + 6], ram[params + 7] = len(data) & 255, len(data) >> 8
        elif command == 0xCB:                         # WRITE
            entry = self.open_files[ram[params + 1]]
            data = bytes(ram[word(2):word(2) + word(4)])
            self.saved[entry[0]] = self.saved.get(entry[0], b"")[:entry[1]] + data
            entry[1] += len(data)
            ram[params + 6], ram[params + 7] = len(data) & 255, len(data) >> 8
        elif command == 0x65:                         # QUIT
            raise RuntimeError("player called QUIT")
        mpu.a, mpu.p = 0, (mpu.p & ~0x83) | 0x02      # no error: C clear, Z set
        mpu.pc = call + 3

    # -- run -----------------------------------------------------------
    def run(self) -> None:
        mpu, ram = self.mpu, self.memory._subject
        started = False
        for _ in range(200_000_000):
            if mpu.pc == 0xBF00:
                self.mli()
                continue
            if mpu.pc == IDLE_KEY_PC and not started and ram[0x1000]:
                self.key, started = 0xA0, True        # SPACE starts the capture
            if started and ram[0x1002] == 2 and mpu.pc == IDLE_KEY_PC:
                return                                # DONE, log saved
            if (mpu.pc == POLL_PC and ram[0x1010] and self.next_underflow is not None
                    and not self.ifr6 and not self.key & 0x80):
                gap = self.next_underflow - (mpu.processorCycles + 9)
                if gap > 0:                           # skip whole idle poll loops
                    mpu.processorCycles += -(-gap // POLL_LOOP_CYCLES) * POLL_LOOP_CYCLES
            self.poll_entry = None
            running_before, tick_before = ram[0x1010], self.tick_now()
            mpu.step()
            if self.poll_entry is not None and self.ifr6:
                self.tick_entries.append((tick_before + 1, self.poll_entry))
            for target, register, value in self.pending:
                self.events.append((mpu.processorCycles, target, register, value,
                                    self.tick_now(), bool(running_before)))
            self.pending.clear()
        raise RuntimeError("player did not finish")


def rebuild(disk: Path, out: Path) -> dict:
    image = disk.read_bytes()
    files = read_prodos(image)
    header, stream_writes = decode_stream(files["CAL.PAL"])
    real_log = files["CAL.LOG"]
    machine = Machine(dict(files), decode_log(real_log))
    machine.run()
    period, start = machine.period, machine.timer_start
    entries = dict(machine.tick_entries)

    # The player also writes silence before and after the stream. Stream
    # writes are those made while the run flag is set, from the first
    # stream record on.
    played = [e for e in machine.events if e[5]]
    first = next(i for i, e in enumerate(played) if e[1:4] == stream_writes[0][2:5])
    played = played[first:first + len(stream_writes)]
    if [e[1:4] for e in played] != [w[2:5] for w in stream_writes]:
        raise RuntimeError("emulated writes differ from the decoded stream")

    rows = []
    for index, (event, write) in enumerate(zip(played, stream_writes)):
        cycle, target, register, value, tick, _ = event
        record, stream_tick = write[0], write[1]
        if tick != stream_tick:
            raise RuntimeError(f"write {index}: player tick {tick}, stream tick {stream_tick}")
        if tick == 0:
            origin, offset = "before_timer_start", cycle - start
        else:
            origin, offset = "poll_entry_on_timer_tick", cycle - entries[tick]
        rows.append({"index": index, "event_id": f"w{index:05d}",
                     "segment_id": f"rec{record:04d}", "tick": tick, "target": target,
                     "register": register, "value": value,
                     "nominal_cycle_offset": offset, "origin": origin,
                     "emulated_cpu_cycle": cycle - start,
                     "poll_entry_lag_cycles": 0 if tick == 0 else
                     entries[tick] - (start + tick * period)})
    other = [e for e in machine.events if e not in played]
    saved_log = machine.saved.get("CAL.LOG", b"")
    log_records = decode_log(real_log)
    log_match = decode_log(saved_log) == log_records if saved_log[:4] == b"S26L" else False

    def chip(target: int) -> str:
        return {0: "AY chip 0: VIA at $C41x, select bit 3",
                1: "AY chip 1: VIA at $C48x, select bit 3",
                2: "AY chip 2: VIA at $C41x, select bit 4",
                3: "AY chip 3: VIA at $C48x, select bit 4",
                4: "SSI-263 at $C420 (top, P8433)",
                5: "SSI-263 at $C440 (bottom, P8431)"}[target]

    out.mkdir(parents=True, exist_ok=True)
    trace = io.StringIO(newline="")
    writer = csv.DictWriter(trace, fieldnames=list(rows[0]), lineterminator="\n")
    writer.writeheader()
    writer.writerows(rows)
    registers = io.StringIO(newline="")
    writer = csv.writer(registers, lineterminator="\n")
    writer.writerow(["event_id", "tick", "target", "register", "value", "segment_id",
                     "chip", "address", "nominal_seconds"])
    for row in rows:
        address = "" if row["target"] < 4 else \
            f"{(0xC420 if row['target'] == 4 else 0xC440) + row['register']:04X}"
        seconds = (row["tick"] * period + row["nominal_cycle_offset"]) / PAL_CPU_HZ
        writer.writerow([row["event_id"], row["tick"], row["target"], row["register"],
                         row["value"], row["segment_id"], chip(row["target"]), address,
                         f"{seconds:.6f}"])
    ssi = sum(row["target"] >= 4 for row in rows)
    lags = [row["poll_entry_lag_cycles"] for row in rows if row["tick"]]
    manifest = {
        "name": "SSI263-CAL-PAL-01 (reconstructed)",
        "reconstructed": True,
        "note": "Rebuilt from the returned calibration disk by scripts/"
                "rebuild_ssi263_cal_trace.py. The original package is lost. Segment "
                "IDs are stream record numbers, not the original test names.",
        "source": {"disk_sha256": sha(image),
                   "CAL.SYSTEM_sha256": sha(files["CAL.SYSTEM"]),
                   "CAL.PAL_sha256": sha(files["CAL.PAL"]),
                   "CAL.LOG_sha256": sha(real_log)},
        "clock": {"effective_clock_hz": PAL_CPU_HZ, "timer_period_cpu_cycles": period,
                  "tick_rate_hz_nominal": header["tick_rate_hz"]},
        "duration_ticks": header["duration_ticks"],
        "stream_records": header["records"],
        "targets": {str(n): chip(n) for n in range(6)},
        "events": [{key: row[key] for key in
                    ("event_id", "tick", "target", "register", "value", "segment_id")}
                   for row in rows],
    }
    validation = {"main": {
        "verified_register_writes": len(rows),
        "ssi_writes": ssi, "ay_writes": len(rows) - ssi,
        "trace_model": "py65 65C02 run of CAL.SYSTEM; 6522 T1 free-run timer; SSI "
                       "status reads replayed from the real CAL.LOG; instruction-end "
                       "write cycles",
        "emulated_writes_equal_stream_decode": True,
        "emulated_log_equals_real_log": log_match,
        "real_log_change_records": len(log_records),
        "ticks_reached": max(tick for tick, _ in machine.tick_entries),
        "poll_entry_lag_cycles_min_max": [min(lags), max(lags)],
        "writes_outside_stream": len(other),
    }}
    members = {"manifest.json": json.dumps(manifest, indent=2) + "\n",
               "player_trace.csv": trace.getvalue(),
               "registers.csv": registers.getvalue(),
               "validation.json": json.dumps(validation, indent=2) + "\n"}
    sums = "".join(f"{sha(text.encode('utf-8'))}  {name}\n" for name, text in members.items())
    members["SHA256SUMS.txt"] = sums
    for name, text in members.items():
        (out / name).write_text(text, encoding="utf-8", newline="\n")
    package = out / "SSI263-CAL-PAL-01-reconstructed.zip"
    with zipfile.ZipFile(package, "w", zipfile.ZIP_DEFLATED) as archive:
        for name, text in members.items():
            archive.writestr(name, text)
    return dict(validation["main"], period=period, package=str(package),
                package_sha256=sha(package.read_bytes()))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--disk", type=Path, default=DEFAULT_DISK)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    args = parser.parse_args()
    print(json.dumps(rebuild(args.disk, args.out), indent=2))


if __name__ == "__main__":
    main()
