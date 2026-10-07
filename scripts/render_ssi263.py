#!/usr/bin/env python3
"""Render SSI register traces to stereo WAV with a standalone C++ model.

No Vivado, FPGA build or hardware is used. A C++17 compiler is needed only
when the host source changes; the resulting executable runs independently.
"""
from __future__ import annotations

import argparse
from dataclasses import asdict
from fractions import Fraction
import hashlib
import html
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import time
import wave
import zipfile

import numpy as np
from ssi263_host_data import (DEMO_NAMES, Event, Trace, load_calibration,
                              make_demo, write_tables)

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "scripts/ssi263_host"
CACHE = ROOT / "build/ssi263_host"
PROFILES = ("baseline", "pitch", "transitions", "prototype")
BALANCED_REFERENCE = ROOT / "scripts/fixtures/ssi263_host/balanced_reference.json"
MODEL_REFERENCE = ROOT / "scripts/fixtures/ssi263_host/amp_zero_hold_reference.json"
MODEL_REVISION = json.loads(MODEL_REFERENCE.read_text(encoding="utf-8"))["id"]
DEFAULT_PROTOTYPE_GAIN = 1
DEFAULT_VOICE_TRIM = 16384


def reference_settings(reference: str | None = None) -> dict:
    """Select gains for the current engine, not a historical waveform replay."""
    if reference is None:
        return {"articulation_reference": 8, "prototype_gain": DEFAULT_PROTOTYPE_GAIN,
                "voice_trim": DEFAULT_VOICE_TRIM}
    if reference != "balanced":
        raise ValueError("unknown SSI listening reference")
    return json.loads(BALANCED_REFERENCE.read_text(encoding="utf-8"))["parameters"]


DESCRIPTION = {
    "baseline": "Current-engine software baseline",
    "pitch": "Baseline with exact SSI pitch timing",
    "transitions": "Native scan and linear transitions, existing sound generator and filters",
    "prototype": "Native source and five-formant prototype circuit candidate",
}
LIMITS = {
    "baseline": ["Sample-event port of current RTL; fabric pipeline latency and D7/IRQ are not simulated.",
                 "Register effects on baseline audio round up to the next 48 kHz sample, preserving write order.",
                 "Current mapped coefficients, pitch approximation, linear FF law and sound shaping remain."],
    "pitch": ["Experimental waveform-phase adapter uses exact SSI pitch periods with the existing pulse shape.",
              "The existing filter law, transition rules and sound shaping remain."],
    "transitions": ["Prototype scanner and DDA arithmetic; production internal-phase equivalence is unverified.",
                    "Articulation clock uses a fixed reference RATE, independent of live speech RATE.",
                    "Absolute transition speed is provisional; reference RATE is an explicit model setting.",
                    "Source/envelope and noise routes are not yet fully connected in this profile.",
                    "Existing coefficient conversion, waveform, FF law and generic output shaping remain.",
                    "Host-only filter and shaping arithmetic keeps fractional state to avoid fixed-point limit cycles; this is not an analog prototype change.",
                    "Host amplitude zero still mutes immediately; native amplitude-envelope timing is incomplete."],
    "prototype": ["Experimental ideal circuit from archived SC-02 prototype; final SSI differences remain unresolved.",
                  "Prototype conflict: articulation uses fixed reference RATE instead of live RATE coupling.",
                  "Prototype conflict: CTL release restarts a full duration interval rather than a free-running prototype counter.",
                  "Prototype conflict: CTL high clears pending source sync and forces excitation to zero; the archive treats CTL and external reset separately.",
                  "Immediate pitch words reach the next divider reload; transitioned pitch retains the running engine's glide policy at 48 kHz boundaries.",
                  "Voice trim, output gain and deterministic cold seeds are model settings, not measured chip values.",
                  "Five ideal switched-capacitor formants retain individual charge; no op-amp or analog-output response.",
                  "U148 output hold is sampled at 48 kHz without an analog reconstruction or anti-alias filter.",
                  "AMP zero retains stored filter amplitude while VA/FA target zero; the remaining U166B permit is provisional."],
}


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_hashes() -> dict[str, str]:
    paths = sorted(SOURCE.glob("*.h")) + sorted(SOURCE.glob("*.cpp"))
    paths += [Path(__file__), BALANCED_REFERENCE, MODEL_REFERENCE, ROOT / "scripts/ssi263_host_data.py",
              ROOT / "hdl/apple/ssi263_formant_pkg.sv", ROOT / "hdl/apple/ssi263_sc02_rom.mem"]
    return {path.relative_to(ROOT).as_posix(): digest(path) for path in paths}


def find_compiler() -> str:
    requested = os.environ.get("CXX")
    if requested:
        found = shutil.which(requested) or (requested if Path(requested).is_file() else None)
        if not found:
            raise ValueError("CXX must name one C++17 compiler executable, without arguments")
        return str(found)
    for name in ("g++", "clang++"):
        if found := shutil.which(name):
            return found
    # This checkout's bundled MinGW is a normal standalone C++ compiler.
    # Neither the compiler nor the produced executable invokes Vivado.
    bundled = Path("E:/AMDDesignTools/2025.2/tps/mingw/10.0.0/win64.o/nt/bin/g++.exe")
    if os.name == "nt" and bundled.is_file():
        return str(bundled)
    raise ValueError("Install a C++17 compiler (g++ or clang++), or set CXX to its executable")


def build_host(force: bool = False) -> Path:
    CACHE.mkdir(parents=True, exist_ok=True)
    executable = CACHE / ("ssi263_host.exe" if os.name == "nt" else "ssi263_host")
    stamp = CACHE / "build.json"
    hashes = {p.relative_to(ROOT).as_posix(): digest(p)
              for p in sorted(SOURCE.iterdir()) if p.suffix in (".h", ".cpp")}
    if not force and executable.is_file() and stamp.is_file():
        previous = json.loads(stamp.read_text())
        if previous.get("source_sha256") == hashes and previous.get("binary_sha256") == digest(executable):
            return executable
    compiler = find_compiler()
    environment = os.environ.copy()
    environment["PATH"] = str(Path(compiler).parent) + os.pathsep + environment.get("PATH", "")
    command = [compiler, "-std=c++17", "-O3", "-Wall", "-Wextra", "-Werror"]
    if os.name == "nt":
        command.append("-static")
    command += [str(p) for p in sorted(SOURCE.glob("*.cpp"))] + ["-o", str(executable)]
    result = subprocess.run(command, env=environment, capture_output=True, text=True)
    (CACHE / "compile.log").write_text(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError(f"Host compiler failed:\n{result.stdout}{result.stderr}")
    stamp.write_text(json.dumps({"source_sha256": hashes, "binary_sha256": digest(executable),
                                 "compiler": compiler, "command": command}, indent=2) + "\n")
    return executable


def seconds_to_ticks(value: str, clock: int) -> int:
    number = Fraction(value) * clock
    return -(-number.numerator // number.denominator)


def load_json_trace(path: Path) -> Trace:
    source = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(source, dict) or not isinstance(source.get("events"), list):
        raise ValueError("trace must be an object containing an events array")
    clock = source.get("effective_clock_hz", 1_015_625)
    if type(clock) is not int:
        raise ValueError("effective_clock_hz must be an integer")
    end = source.get("duration_ticks")
    if end is None:
        end = seconds_to_ticks(str(source["duration_seconds"]), clock)
    if type(end) is not int:
        raise ValueError("duration_ticks must be an integer")
    events = []
    for index, row in enumerate(source["events"]):
        if not isinstance(row, dict):
            raise ValueError("each event must be an object")
        tick = row.get("tick")
        if tick is None:
            tick = seconds_to_ticks(str(row["seconds"]), clock)
        fields = (tick, row["socket"], row["register"], row["value"])
        if any(type(field) is not int for field in fields):
            raise ValueError("event tick/socket/register/value must be integers")
        events.append(Event(*fields, index))
    trace = Trace(events, clock, end,
                  {"name": path.stem, "input_sha256": digest(path), "source": str(path.resolve())})
    trace.validate()
    return trace


def render(trace: Trace, out: Path, profile: str, executable: Path | None = None,
           start_tick: int = 0, end_tick: int | None = None,
           articulation_reference: int = 8, prototype_gain: int = DEFAULT_PROTOTYPE_GAIN,
           voice_trim: int = DEFAULT_VOICE_TRIM) -> dict:
    trace.validate()
    end_tick = trace.duration_ticks if end_tick is None else end_tick
    if type(start_tick) is not int or type(end_tick) is not int:
        raise ValueError("range ticks must be integers")
    if not 0 <= start_tick < end_tick <= trace.duration_ticks:
        raise ValueError("range must satisfy 0 <= start < end <= trace duration")
    first_frame = -(-start_tick * 48000 // trace.xck_hz)
    last_frame = -(-end_tick * 48000 // trace.xck_hz)
    if first_frame == last_frame:
        raise ValueError("range contains no 48 kHz sample; choose a longer range")
    if (profile not in PROFILES or type(articulation_reference) is not int or
            not 0 <= articulation_reference <= 15):
        raise ValueError("invalid profile or articulation reference RATE")
    if (type(prototype_gain) is not int or not 1 <= prototype_gain <= 1024 or
            type(voice_trim) is not int or not 0 <= voice_trim <= 131071):
        raise ValueError("prototype gain must be 1..1024 and voice trim 0..131071")
    out.mkdir(parents=True, exist_ok=True)
    executable = build_host() if executable is None else executable
    tables = out / "tables.txt"
    table_metadata = write_tables(tables)
    included = [event for event in trace.events if event.tick < end_tick]
    stimulus = out / "events.txt"
    stimulus.write_text(f"SSIHOST1 {trace.xck_hz} {start_tick} {end_tick} {len(included)}\n" +
                        "".join(f"{e.tick} {e.socket} {e.register} {e.value}\n" for e in included),
                        encoding="ascii")
    pcm_path = out / f"{profile}.pcm"
    command = [str(executable), str(tables), str(stimulus), str(pcm_path), profile,
               str(articulation_reference), str(prototype_gain), str(voice_trim)]
    begun = time.perf_counter()
    result = subprocess.run(command, capture_output=True, text=True)
    elapsed = time.perf_counter() - begun
    if result.returncode:
        raise RuntimeError(f"Host render failed: {result.stderr}")
    engine_result = json.loads(result.stdout)
    pcm_bytes = pcm_path.read_bytes()
    if len(pcm_bytes) != engine_result["frames"] * 4:
        raise RuntimeError("incomplete stereo PCM output")
    samples = np.frombuffer(pcm_bytes, dtype="<i2").reshape(-1, 2)
    wav_path = out / f"{profile}.wav"
    with wave.open(str(wav_path), "wb") as wav:
        wav.setnchannels(2)
        wav.setsampwidth(2)
        wav.setframerate(48000)
        wav.writeframes(pcm_bytes)
    statistics = []
    for channel in samples.T:
        values = channel.astype(np.float64)
        statistics.append({"peak": int(np.abs(values).max(initial=0)),
                           "rms": float(np.sqrt(np.mean(values * values))),
                           "rail_samples": int(np.count_nonzero((channel == -32768) | (channel == 32767))),
                           "nonzero_samples": int(np.count_nonzero(channel))})
    report = {
        "profile": profile, "description": DESCRIPTION[profile], "firmware_target": "F1.2.5-d1",
        "native_model_revision": MODEL_REVISION if profile in ("prototype", "transitions") else None,
        "sample_rate": 48000, "channels": 2, "gain": "fixed model gain; no normalization",
        "effective_xck_hz": trace.xck_hz, "articulation_reference_rate": articulation_reference,
        "prototype_gain": prototype_gain if profile == "prototype" else None,
        "voice_trim_q16": voice_trim if profile == "prototype" else None,
        "start_tick": start_tick, "end_tick": end_tick, "trace_duration_ticks": trace.duration_ticks,
        "trace": trace.metadata, "checked_ssi_writes": len(trace.events),
        "render_seconds": elapsed, "audio_seconds": len(samples) / 48000,
        "rendered_audio_per_wall_second": len(samples) / 48000 / max(elapsed, 1e-9),
        "pcm_sha256": hashlib.sha256(pcm_bytes).hexdigest(), "wav_sha256": digest(wav_path),
        "binary_sha256": digest(executable), "source_sha256": source_hashes(),
        "tables": table_metadata, "statistics": statistics, **engine_result,
        "limits": ["Raw stereo SSI model output; no AY markers, Phasor analog mix or DAC response.",
                   "A requested range replays all earlier events and state before trimming.",
                   "Prototype agreement and useful listening output do not establish physical SSI fidelity."] + LIMITS[profile],
    }
    ff = trace.metadata.get("filter_frequency")
    if ff is not None:
        report["demo_native_filter_clock_hz"] = trace.xck_hz / (2 * (256 - ff))
    inflection = trace.metadata.get("inflection_word")
    if inflection is not None:
        baseline_hz = 20000 / max(1, ((4096 - inflection) * 5) // 32)
        exact_hz = trace.xck_hz / (8 * (4096 - inflection))
        report["demo_pitch_comparison"] = {"baseline_hz": baseline_hz, "exact_hz": exact_hz,
                                           "difference_cents": 1200 * math.log2(baseline_hz / exact_hz)}
    (out / f"{profile}.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def write_listen_page(out: Path, reports: list[dict], comparison: list[dict] | None = None) -> Path:
    def cards(group: list[dict], prefix: str = "") -> str:
        result = []
        for r in group:
            note = ""
            if r["profile"] == "prototype" and "demo_native_filter_clock_hz" in r:
                ff = r["trace"]["filter_frequency"]
                note = (f'<p>FF=${ff:02X}: native filter clock {r["demo_native_filter_clock_hz"]:,.1f} Hz. '
                        'The retained-filter versions use a different FF law.</p>')
            elif r["profile"] == "pitch" and "demo_pitch_comparison" in r:
                pitch = r["demo_pitch_comparison"]
                note = (f'<p>This demo changes pitch from {pitch["baseline_hz"]:.2f} to '
                        f'{pitch["exact_hz"]:.2f} Hz ({abs(pitch["difference_cents"]):.1f} cents). '
                        'The pulse shape and filters stay the same.</p>')
            elif r["profile"] == "transitions":
                note = '<p>Uses extra filter precision to remove numerical ringing during silence. Phoneme fades remain active.</p>'
            result.append(f'<section><h2>{html.escape(r["description"])}</h2>{note}'
                          f'<audio controls preload="metadata" src="{prefix}{r["profile"]}.wav?v={r["wav_sha256"][:12]}"></audio>'
                          f'<p>{r["audio_seconds"]:.2f}s audio, rendered in {r["render_seconds"]:.2f}s. '
                          f'<a href="{prefix}{r["profile"]}.json">Model notes</a></p></section>')
        return "\n".join(result)
    content = cards(reports)
    if comparison:
        ff = comparison[0]["trace"]["filter_frequency"]
        content += (f'<h1>Filter-clock comparison: FF=${ff:02X}</h1>'
                    '<p>Same phrase, pitch, rate, articulation and model gains. Only the FF register changes. '
                    'Every model in this group receives the same revised trace. '
                    'This checks the clock setting; it is not a fit to hardware.</p>' + cards(comparison, f"ff-{ff}/"))
    page = out / "listen.html"
    page.write_text('<!doctype html><meta charset="utf-8"><title>SSI listening comparisons</title>'
                    '<style>body{max-width:800px;margin:48px auto;padding:0 24px;background:#171b20;'
                    'color:#e3e9ef;font:16px/1.5 system-ui}section{padding:18px 24px;margin:20px 0;'
                    'background:#232a32;border-radius:12px}h2{font-size:19px}audio{width:100%}'
                    'a{color:#9ecfff}p{color:#b8c4d0}</style><h1>SSI listening comparisons</h1>'
                    '<p>Same register stream. Each model uses its documented gain, with no normalization. '
                    'These software models are for listening '
                    'and iteration; physical SSI matching awaits the tester recording.</p>'
                    '<p>The prototype follows the SSI filter divider. Equal FF bytes do not imply equal '
                    'filter response in the older models: FF=$80 is their nominal setting, but gives '
                    'only about 4 kHz with the native divider at PAL clock.</p>' + content,
                    encoding="utf-8")
    return page


def write_bundle(out: Path, reports: list[dict], executable: Path,
                 comparison: list[dict] | None = None) -> Path:
    """Package the checked inputs, compiled program and listening output."""
    path = out / "SSI263-HOST-LISTEN.zip"
    groups = [("", reports)]
    if comparison:
        groups.append((f'ff-{comparison[0]["trace"]["filter_frequency"]}/', comparison))
    commands = ["@echo off", 'cd /d "%~dp0"']
    for prefix, group in groups:
        for report in group:
            profile = report["profile"]
            commands.append(f'"{executable.name}" {prefix}tables.txt {prefix}events.txt {prefix}rerender-{profile}.wav '
                            f'{profile} {report["articulation_reference_rate"]} '
                            f'{report.get("prototype_gain") or DEFAULT_PROTOTYPE_GAIN} '
                            f'{report["voice_trim_q16"] if report.get("voice_trim_q16") is not None else DEFAULT_VOICE_TRIM}')
            commands.append("if errorlevel 1 exit /b 1")
    commands.append("pause")
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as bundle:
        bundle.write(executable, executable.name)
        for prefix, group in groups:
            for name in ("tables.txt", "events.txt", "trace.json", "listen.html"):
                bundle.write(out / prefix / name, prefix + name)
            for report in group:
                for extension in ("wav", "json"):
                    name = f'{report["profile"]}.{extension}'
                    bundle.write(out / prefix / name, prefix + name)
        bundle.write(ROOT / "docs/SSI263_HOST_RENDERER.md", "MODEL-NOTES.md")
        if os.name == "nt":
            bundle.writestr("render.cmd", "\r\n".join(commands) + "\r\n")
        bundle.writestr("README.txt",
            "Extract this folder and open listen.html to compare the bundled WAVs.\n"
            "On Windows, render.cmd creates rerender-*.wav from tables.txt/events.txt.\n"
            "The static executable needs no Python, compiler, Vivado or hardware.\n"
            "JSON reports describe the original bundled renders. MODEL-NOTES.md lists limits.\n"
            "These are experimental listening models, pending physical SSI comparison.\n")
    return path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--demo", choices=DEMO_NAMES, default=None)
    source.add_argument("--trace", type=Path, help="JSON register events")
    source.add_argument("--calibration", type=Path, help="checked physical-test ZIP")
    parser.add_argument("--engine", choices=(*PROFILES, "all"), default="all")
    parser.add_argument("--output", type=Path, default=CACHE / "listen")
    parser.add_argument("--clock", type=int, default=1_015_625, help="effective XCK Hz for built-in demos")
    parser.add_argument("--ff", type=int, default=128, help="filter register for built-in demos")
    parser.add_argument("--compare-ff", type=int, help="also render the built-in demo with this FF value")
    parser.add_argument("--articulation", type=int, default=5, help="articulation register for demos")
    parser.add_argument("--reference", choices=("balanced",), help="accepted gains with the current engine; not historical audio replay")
    parser.add_argument("--articulation-reference-rate", type=int)
    parser.add_argument("--prototype-gain", type=int)
    parser.add_argument("--voice-trim", type=int, help="prototype voice drive, Q16")
    parser.add_argument("--start-seconds", default="0")
    parser.add_argument("--end-seconds")
    parser.add_argument("--rebuild", action="store_true")
    parser.add_argument("--build-only", action="store_true")
    parser.add_argument("--bundle", action="store_true", help="also create a portable listening ZIP")
    args = parser.parse_args()
    settings = reference_settings(args.reference)
    for key, value in (("articulation_reference", args.articulation_reference_rate),
                       ("prototype_gain", args.prototype_gain), ("voice_trim", args.voice_trim)):
        if value is not None:
            if args.reference and value != settings[key]:
                parser.error("--reference fixes the model settings; omit it for experiments")
            settings[key] = value
    if not 0 <= args.ff <= 255 or not 0 <= args.articulation <= 7:
        parser.error("--ff must be 0..255 and --articulation must be 0..7")
    if args.compare_ff is not None and (not 0 <= args.compare_ff <= 255 or args.trace or args.calibration):
        parser.error("--compare-ff must be 0..255 and applies only to built-in demos")
    executable = build_host(args.rebuild)
    if args.build_only:
        print(executable)
        return
    if args.calibration:
        trace = load_calibration(args.calibration)
    elif args.trace:
        trace = load_json_trace(args.trace)
    else:
        trace = make_demo(args.demo or "hello", args.clock, args.ff, args.articulation)
    start = seconds_to_ticks(args.start_seconds, trace.xck_hz)
    end = None if args.end_seconds is None else seconds_to_ticks(args.end_seconds, trace.xck_hz)
    profiles = PROFILES if args.engine == "all" else (args.engine,)
    def run_group(stimulus: Trace, directory: Path) -> list[dict]:
        reports = []
        for profile in profiles:
            report = render(stimulus, directory, profile, executable, start, end,
                            **settings)
            reports.append(report)
            print(f'{profile}: {report["audio_seconds"]:.2f}s stereo WAV in '
                  f'{report["render_seconds"]:.2f}s -> {directory / (profile + ".wav")}')
        (directory / "trace.json").write_text(json.dumps({"effective_clock_hz": stimulus.xck_hz,
            "duration_ticks": stimulus.duration_ticks, "events": [asdict(e) for e in stimulus.events]}, indent=2) + "\n")
        return reports
    reports = run_group(trace, args.output)
    comparison = None
    if args.compare_ff is not None:
        directory = args.output / f"ff-{args.compare_ff}"
        comparison = run_group(make_demo(args.demo or "hello", args.clock, args.compare_ff, args.articulation), directory)
        write_listen_page(directory, comparison)
    print(write_listen_page(args.output, reports, comparison))
    if args.bundle:
        print(write_bundle(args.output, reports, executable, comparison))


if __name__ == "__main__":
    try:
        main()
    except (ValueError, RuntimeError, OSError, KeyError) as error:
        raise SystemExit(str(error)) from error
