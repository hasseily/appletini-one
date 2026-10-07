#!/usr/bin/env python3
"""Replay a calibration disk's checked CPU trace through both SSI263AP voices.

Examples:
  python scripts/sim_ssi263_calibration.py --prepare-only
  python scripts/sim_ssi263_calibration.py --end-seconds 0.02
  python scripts/sim_ssi263_calibration.py --start-seconds 2 --end-seconds 3.5

A range trims the WAV only: simulation always starts before timer tick zero and
executes every preceding write. With no range, the whole capture is simulated.
This is raw SSI output: AY markers, the board mixer, and analog output are absent.
The recorded CPU offsets are nominal instruction-end cycles, not measured pin
edges. A full two-minute run at the default 133 MHz fabric clock is expensive.
"""
from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass
from fractions import Fraction
import hashlib
import io
import json
from pathlib import Path
import shutil
import struct
import subprocess
import time
import wave
import zipfile

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PACKAGE = ROOT / "build/ssi263_calibration/SSI263-CAL-PAL-01.zip"
SAMPLE_RATE = 48_000
FABRIC_HZ = 133_000_000


@dataclass(frozen=True)
class TraceEvent:
    index: int
    tick: int
    target: int
    register: int
    value: int
    cpu_cycle: int
    cycle_offset: int
    event_id: str
    segment_id: str
    origin: str

    @property
    def address(self) -> int | None:
        # These are slot-4 native Phasor addresses. Targets 4/5 are stream
        # socket IDs, not Apple slot numbers. AY targets 0..3 use VIA traffic.
        return (0xC420 if self.target == 4 else 0xC440) + self.register \
            if self.target in (4, 5) else None


@dataclass
class CaptureTrace:
    manifest: dict
    events: list[TraceEvent]
    cpu_hz: int
    period: int
    package_sha256: str
    trace_sha256: str
    trace_model: str


def parse_trace(manifest: dict, trace_csv: str, validation: dict,
                package_sha256: str = "") -> CaptureTrace:
    """Check every trace write against its manifest; do not infer missing times."""
    clock = manifest["clock"]
    cpu_hz = int(clock["effective_clock_hz"])
    period = int(clock["timer_period_cpu_cycles"])
    if cpu_hz <= 0 or period <= 0 or int(manifest["duration_ticks"]) <= 0:
        raise ValueError("invalid clock or capture duration")
    expected = manifest["events"]
    rows = list(csv.DictReader(io.StringIO(trace_csv)))
    if len(rows) != len(expected) or not rows:
        raise ValueError("trace and manifest event counts differ or are empty")
    if validation["main"]["verified_register_writes"] != len(rows):
        raise ValueError("player validation write count disagrees with trace")
    result = []
    last_cycle = None
    for index, (row, event) in enumerate(zip(rows, expected)):
        tick, target, reg, value = (int(row[key]) for key in
                                    ("tick", "target", "register", "value"))
        if (tick, target, reg, value) != tuple(int(event[key]) for key in
                                              ("tick", "target", "register", "value")):
            raise ValueError(f"trace write {index} disagrees with manifest")
        if not 0 <= tick <= int(manifest["duration_ticks"]):
            raise ValueError(f"invalid tick at write {index}")
        if target not in range(6) or not 0 <= reg <= (7 if target >= 4 else 15):
            raise ValueError(f"invalid target/register at write {index}")
        if not 0 <= value <= 255:
            raise ValueError(f"invalid byte at write {index}")
        offset = int(row["nominal_cycle_offset"])
        origin = row["origin"]
        if tick == 0:
            if origin != "before_timer_start" or offset >= 0:
                raise ValueError("tick-zero writes must precede timer start")
        elif origin != "poll_entry_on_timer_tick" or not 0 <= offset < period:
            raise ValueError(f"invalid tick offset at write {index}")
        cycle = tick * period + offset
        if last_cycle is not None and cycle <= last_cycle:
            raise ValueError("trace write times must strictly increase")
        last_cycle = cycle
        result.append(TraceEvent(index, tick, target, reg, value, cycle, offset,
                                 event["event_id"], event["segment_id"], origin))
    return CaptureTrace(manifest, result, cpu_hz, period, package_sha256,
                        hashlib.sha256(trace_csv.encode("utf-8")).hexdigest(),
                        validation["main"]["trace_model"])


def load_package(path: Path) -> CaptureTrace:
    """Load the supplied ZIP without extracting or executing package content."""
    package_bytes = path.read_bytes()
    with zipfile.ZipFile(io.BytesIO(package_bytes)) as archive:
        if len(archive.namelist()) != len(set(archive.namelist())):
            raise ValueError("duplicate ZIP members")
        hashes = {}
        for line in archive.read("SHA256SUMS.txt").decode("ascii").splitlines():
            digest, name = line.split("  ", 1)
            hashes[name] = digest
        names = ("manifest.json", "player_trace.csv", "registers.csv", "validation.json")
        data = {name: archive.read(name) for name in names}
        for name, value in data.items():
            if hashlib.sha256(value).hexdigest() != hashes.get(name):
                raise ValueError(f"package hash mismatch: {name}")
        manifest = json.loads(data["manifest.json"])
        registers = list(csv.DictReader(io.StringIO(data["registers.csv"].decode("utf-8"))))
        if len(registers) != len(manifest["events"]):
            raise ValueError("register CSV event count disagrees with manifest")
        for row, event in zip(registers, manifest["events"]):
            for key in ("event_id", "tick", "target", "register", "value", "segment_id"):
                if row[key] != str(event[key]):
                    raise ValueError(f"register CSV disagrees with manifest: {key}")
        return parse_trace(manifest, data["player_trace.csv"].decode("utf-8"),
                           json.loads(data["validation.json"]),
                           hashlib.sha256(package_bytes).hexdigest())


def ceil_fraction(value: Fraction) -> int:
    return -(-value.numerator // value.denominator)


def prepare(trace: CaptureTrace, out: Path, start: Fraction = Fraction(0),
            end: Fraction | None = None, fabric_hz: int = FABRIC_HZ) -> dict:
    duration = Fraction(int(trace.manifest["duration_ticks"]) * trace.period, trace.cpu_hz)
    end = duration if end is None else end
    if not 0 <= start < end <= duration:
        raise ValueError("range must satisfy 0 <= start < end <= capture duration")
    if fabric_hz < FABRIC_HZ:
        raise ValueError("fabric clock must be at least the 133 MHz production budget")
    out.mkdir(parents=True, exist_ok=True)
    pre_cycles = max(0, -min(event.cpu_cycle for event in trace.events))
    # Keep 32 fabric clocks after reset before the first register write.
    origin = 32 + ceil_fraction(Fraction(pre_cycles * fabric_hz, trace.cpu_hz))
    start_cycle = origin + ceil_fraction(start * fabric_hz)
    end_cycle = origin + ceil_fraction(end * fabric_hz)
    rows = []
    stimuli = []
    prior = -1
    for event in trace.events:
        fabric_cycle = origin + ceil_fraction(Fraction(event.cpu_cycle * fabric_hz, trace.cpu_hz))
        if fabric_cycle <= prior:
            raise ValueError("fabric quantization merged or reordered writes")
        prior = fabric_cycle
        included = fabric_cycle < end_cycle
        row = dict(event.__dict__, nominal_seconds=event.cpu_cycle / trace.cpu_hz,
                   fabric_cycle=fabric_cycle, address=(f"{event.address:04X}" if event.address else ""),
                   simulated=included, raw_ssi_audio=event.target >= 4)
        rows.append(row)
        if included:
            stimuli.append(f"{fabric_cycle} {event.index} {event.target} {event.register} {event.value}")
    (out / "stimulus.txt").write_text("\n".join(stimuli) + "\n", encoding="ascii")
    with (out / "events.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    details = {
        "build_id": trace.manifest["build_id"],
        "package_sha256": trace.package_sha256, "trace_sha256": trace.trace_sha256,
        "trace_model": trace.trace_model,
        "cpu_hz": trace.cpu_hz, "raw_xck_hz": 2 * trace.cpu_hz,
        "ssi_div2": True, "fabric_hz": fabric_hz, "sample_rate": SAMPLE_RATE,
        "timer_period_cpu_cycles": trace.period,
        "timer_origin_fabric_cycle": origin, "capture_start_fabric_cycle": start_cycle,
        "end_fabric_cycle": end_cycle, "capture_start_seconds": float(start),
        "full_capture_fabric_cycles": origin + ceil_fraction(duration * fabric_hz),
        "capture_end_seconds": float(end), "total_checked_writes": len(rows),
        "replayed_writes": len(stimuli),
        "replayed_ssi_writes": sum(row["simulated"] and row["raw_ssi_audio"] for row in rows),
        "channels": ["target4: slot4 native $C420, secondary SSI", "target5: slot4 native $C440, primary SSI"],
        "segments": trace.manifest["segments"],
        "status": "prepared",
        "limits": [
            "Raw SSI audio only; AY alignment tones and board/analog mixing are absent.",
            "Nominal CPU instruction-end offsets exclude bus waits and timer polling phase.",
            "Q3 begins at a defined simulated phase; the recording's phase is unknown.",
            "Native slot decoding is supplied from the trace; the whole Apple bus/VIA is not simulated.",
            "All preceding writes and state are replayed for a requested WAV range.",
            "D7 tick snapshots are at nominal timer boundaries, not the player's later status reads.",
            "Current production RTL includes uncalibrated SSI tract/source approximations.",
            "Behavioral RTL simulation: logical cycle ratios define time, not the testbench's #5 delay; no routed timing model.",
        ],
    }
    (out / "replay.json").write_text(json.dumps(details, indent=2) + "\n", encoding="utf-8")
    return details


def vivado_tool(name: str) -> str:
    result = shutil.which(name + ".bat") or shutil.which(name)
    if not result:
        raise FileNotFoundError(f"Vivado {name} is not on PATH")
    return result


def run_tool(command: list[str], out: Path, logfile: str) -> float:
    started = time.perf_counter()
    result = subprocess.run(command, cwd=out, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    (out / logfile).write_text(result.stdout, encoding="utf-8")
    if result.returncode:
        raise RuntimeError(f"{command[0]} failed; see {out / logfile}")
    return time.perf_counter() - started


def simulate(out: Path, details: dict) -> dict:
    """Use an isolated xsim work directory so concurrent regressions cannot collide."""
    out = out.resolve()
    shutil.copyfile(ROOT / "hdl/apple/ssi263_sc02_rom.mem",
                    out / "ssi263_sc02_rom.mem")
    sources = [ROOT / "hdl/apple" / name for name in (
        "ssi263_parameter_rom.sv", "ssi263_native_controller.sv",
        "ssi263_native_pitch.sv", "ssi263_native_source.sv",
        "ssi263_native_tract.sv", "ssi263_native_engine.sv",
        "ssi263_response_timing.sv", "ssi263_bus_wrapper.sv",
        "ssi263_voice.sv", "ssi263_xck_ce.sv")]
    sources.append(ROOT / "hdl/sim/tb_ssi263_calibration.sv")
    details["rtl_sha256"] = {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
                              for path in sources}
    timings = {}
    timings["xvlog"] = run_tool([vivado_tool("xvlog"), "-sv", *map(str, sources)], out, "xvlog.log")
    timings["xelab"] = run_tool([vivado_tool("xelab"), "tb_ssi263_calibration", "-s", "calibration"], out, "xelab.log")
    config = [details[key] for key in ("fabric_hz", "cpu_hz", "timer_period_cpu_cycles",
              "timer_origin_fabric_cycle", "capture_start_fabric_cycle", "end_fabric_cycle",
              "replayed_writes")]
    (out / "config.txt").write_text(" ".join(map(str, config)) + "\n", encoding="ascii")
    timings["xsim"] = run_tool([vivado_tool("xsim"), "calibration", "-runall"], out, "xsim.log")
    log = (out / "xsim.log").read_text(encoding="utf-8")
    if "CALIBRATION_REPLAY_PASS" not in log or "Fatal:" in log:
        raise RuntimeError(f"RTL replay did not complete; see {out / 'xsim.log'}")
    details["tool_wall_seconds"] = timings
    details["status"] = "completed"
    return finish_audio(out, details)


def finish_audio(out: Path, details: dict) -> dict:
    count, first, last = 0, None, None
    peaks, clips = [0, 0], [0, 0]
    with (out / "samples.csv").open(newline="") as source, wave.open(str(out / "ssi_raw.wav"), "wb") as wav:
        wav.setnchannels(2)
        wav.setsampwidth(2)
        wav.setframerate(SAMPLE_RATE)
        frames = bytearray()
        for row in csv.DictReader(source):
            cycle, left, right = (int(row[key]) for key in ("fabric_cycle", "target4", "target5"))
            first = cycle if first is None else first
            last = cycle
            for chip, sample in enumerate((left, right)):
                if not -32768 <= sample <= 32767:
                    raise ValueError("RTL sample outside signed 16-bit range")
                peaks[chip] = max(peaks[chip], abs(sample))
                clips[chip] += int(sample in (-32768, 32767))
            frames.extend(struct.pack("<hh", left, right))
            count += 1
            if len(frames) >= 65536:
                wav.writeframesraw(frames)
                frames.clear()
        wav.writeframesraw(frames)
    if not count:
        raise ValueError("requested range produced no audio samples")
    origin, hz = details["timer_origin_fabric_cycle"], details["fabric_hz"]
    details["audio"] = {"file": "ssi_raw.wav", "frames": count,
                        "first_sample_fabric_cycle": first, "last_sample_fabric_cycle": last,
                        "first_sample_seconds": (first - origin) / hz,
                        "peak": peaks, "rail_samples": clips}
    for segment in details["segments"]:
        # Tick windows are nominal. events.csv contains the individual write edges.
        segment["wav_start_sample"] = round((segment["start_seconds"] - details["audio"]["first_sample_seconds"]) * SAMPLE_RATE)
        segment["wav_end_sample"] = round((segment["end_seconds"] - details["audio"]["first_sample_seconds"]) * SAMPLE_RATE)
    (out / "replay.json").write_text(json.dumps(details, indent=2) + "\n", encoding="utf-8")
    return details


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--package", type=Path, default=DEFAULT_PACKAGE)
    parser.add_argument("--out-dir", type=Path, default=ROOT / "build/ssi263_calibration_replay")
    parser.add_argument("--start-seconds", type=Fraction, default=Fraction(0))
    parser.add_argument("--end-seconds", type=Fraction)
    parser.add_argument("--fabric-hz", type=int, default=FABRIC_HZ)
    parser.add_argument("--prepare-only", action="store_true", help="Validate the entire package and write timing/segment metadata; do not run RTL.")
    args = parser.parse_args()
    try:
        trace = load_package(args.package)
        details = prepare(trace, args.out_dir, args.start_seconds, args.end_seconds, args.fabric_hz)
        print(f"Checked all {len(trace.events)} writes; replay prefix has {details['replayed_writes']} writes.")
        print(f"Simulation always starts before timer zero; {details['end_fabric_cycle']:,} fabric cycles required.")
        if not args.prepare_only:
            details = simulate(args.out_dir, details)
            print(f"Wrote {details['audio']['frames']} raw stereo SSI samples to {args.out_dir / 'ssi_raw.wav'}")
        print(f"Metadata: {args.out_dir / 'replay.json'}")
    except (OSError, ValueError, KeyError, RuntimeError, zipfile.BadZipFile) as error:
        parser.exit(1, f"error: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
