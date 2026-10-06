#!/usr/bin/env python3
"""Trace existing SSI attack state without changing the listening model.

The audit replays the same native SSI bus sequences as the listening page.
Each diagnostic PCM must match socket 0 of a normal prototype render exactly.
No filter, envelope, gain, sample-rate conversion or waveform is adjusted.
"""
from __future__ import annotations

import argparse
from bisect import bisect_right
import csv
from dataclasses import asdict
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import wave

import numpy as np

from render_ssi263 import CACHE, ROOT, SOURCE, build_host, digest, find_compiler, render
from render_ssi263_mb_audit import FIXTURE, make_trace
from ssi263_host_data import Trace, make_demo

HARNESS = ROOT / "scripts/ssi263_attack_trace.cpp"
ARCHIVE_COMMIT = "502ae04f68f04e23ce04abaaf0d44990c71e763a"


def archive_evidence(out: Path) -> dict:
    """Keep exact primary archive material alongside the runtime audit."""
    source = out / "prototype_evidence"
    source.mkdir(parents=True, exist_ok=True)
    names = ("sc02_schematic.json", "connectivity/sc02_pin_net_memberships.csv",
             "source_svg/06_digital_4.svg", "source_svg/07_digital_5.svg")
    raw = {}
    for name in names:
        path = "schematics/sc02_prototype/" + name
        raw[name] = subprocess.check_output(["git", "show", f"{ARCHIVE_COMMIT}:{path}"], cwd=ROOT)
        (source / Path(name).name).write_bytes(raw[name])
    refs = {"U68", "U69", "U70", "U71", "U72", "U85", "U96", "U102", "U104", "U111",
            "U161", "U162", "U166", "U167", "U168", "U169", "U177", "U178", "U206", "U208", "P2", "R301"}
    memberships = list(csv.DictReader(io.StringIO(raw[names[1]].decode("utf-8"))))
    # Use the extractor's actual column names; copied records retain source
    # coordinates, implicit-pin flags and unmodified exact net names.
    selected = [r for r in memberships if r.get("physical_ref") in refs and
                r.get("sheet_name") in ("digital_4.SchDoc", "digital_5.SchDoc")]
    if not selected:
        raise ValueError("prototype pin-membership format changed")
    evidence = {"commit": ARCHIVE_COMMIT,
                "source_sha256": {name: hashlib.sha256(value).hexdigest() for name, value in raw.items()},
                "pin_memberships": selected,
                "classification": "Prototype wiring evidence; does not establish final production SSI behavior"}
    (source / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
    return {key: value for key, value in evidence.items() if key != "pin_memberships"}


def build_diagnostic(out: Path) -> Path:
    out.mkdir(parents=True, exist_ok=True)
    executable = out / ("attack_trace.exe" if os.name == "nt" else "attack_trace")
    compiler = find_compiler()
    command = [compiler, "-std=c++17", "-O3", "-Wall", "-Wextra", "-Werror"]
    if os.name == "nt":
        command.append("-static")
    command += [str(HARNESS), *(str(SOURCE / name) for name in
                ("native_control.cpp", "native_source.cpp", "prototype_tract.cpp")),
                "-o", str(executable)]
    result = subprocess.run(command, text=True, capture_output=True)
    (out / "compile.log").write_text(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError(result.stderr)
    (out / "build.json").write_text(json.dumps({
        "compiler": compiler, "command": command, "diagnostic_sha256": digest(HARNESS),
        "host_source_sha256": {p.name: digest(p) for p in sorted(SOURCE.iterdir())
                               if p.suffix in (".h", ".cpp")},
    }, indent=2) + "\n")
    return executable


def load_states(path: Path) -> list[dict]:
    with path.open(newline="", encoding="utf-8") as stream:
        return [{key: value if key == "reason" else int(value)
                 for key, value in row.items()} for row in csv.DictReader(stream)]


def envelope_ramps(states: list[dict], hz: int) -> list[dict]:
    """Report observed U68 rises, without defining an acoustic attack target."""
    ramps = []
    current = None
    enable_tick = None
    for previous, row in zip(states, states[1:]):
        up = not (row["pw3"] == 1 and not row["u62"]) and bool(row["voice_amp"] or row["fric_amp"])
        old_up = not (previous["pw3"] == 1 and not previous["u62"]) and bool(previous["voice_amp"] or previous["fric_amp"])
        if up and not old_up:
            enable_tick = row["tick"]
        old, new = previous["ampct"], row["ampct"]
        if new < old:
            current = None
        elif new > old:
            if current is None:
                current = {"start_tick": row["tick"], "start_count": old,
                           "enable_tick": enable_tick, "phone": row["phone"], "count_edges": []}
            current["count_edges"].append([row["tick"], new])
            if new == 15:
                current["end_tick"] = row["tick"]
                current["first_to_last_increment_ms"] = (row["tick"] - current["start_tick"]) * 1000 / hz
                current["enable_to_full_count_ms"] = ((row["tick"] - enable_tick) * 1000 / hz
                                                      if enable_tick is not None else None)
                current["interval_definition"] = ("First counter increment to count 15; excludes the wait "
                                                   "from enable until the first increment.")
                interval = [r for r in states if current["start_tick"] <= r["tick"] <= row["tick"]]
                full_mask = next((r for r in interval if r["amp_code"] and
                                  r["filter_amp"] == r["amp_code"]), None)
                current["u206_full_host_amp_tick"] = full_mask["tick"] if full_mask else None
                current["first_increment_to_u206_full_amp_ms"] = (
                    (full_mask["tick"] - current["start_tick"]) * 1000 / hz if full_mask else None)
                current["u206_mask_changes"] = [[r["tick"], r["filter_amp"]]
                                                for p, r in zip(states, states[1:])
                                                if current["start_tick"] <= r["tick"] <= row["tick"] and
                                                p["filter_amp"] != r["filter_amp"]]
                ramps.append(current)
                current = None
    return ramps


def summarize(trace: Trace, states: list[dict], samples: np.ndarray) -> dict:
    ticks = [row["tick"] for row in states]
    segments = []
    hz = trace.xck_hz

    def state_at(tick: int) -> dict:
        return states[max(0, bisect_right(ticks, tick) - 1)]

    retained = ("pw0", "pw1", "pw3", "u20", "amp_code", "va_code", "fa_code",
                "ampct", "filter_amp", "fric1", "fric2")
    for index, segment in enumerate(trace.metadata.get("segments", [])):
        start, end = segment["start_tick"], segment["end_tick"]
        before = state_at(start)
        relevant = states[bisect_right(ticks, start):bisect_right(ticks, end - 1)]
        entry = {"index": index, "phone": segment["phone"], "start_tick": start,
                 "end_tick": end, "state_at_write": {k: before[k] for k in retained}}
        for field in ("pw0", "pw1", "voice_amp", "fric_amp", "filter_amp", "ampct"):
            row = before if before[field] > 0 else next((r for r in relevant if r[field] > 0), None)
            entry["first_positive_" + field + "_ms"] = (
                max(0, row["tick"] - start) * 1000 / hz if row else None)
        first_source = next((r for r in [before] + relevant if r["voice_amp"] or r["fric_amp"]), None)
        entry["state_at_first_source_amplitude"] = (
            {k: first_source[k] for k in ("tick", *retained)} if first_source else None)
        # Retained tails can occur immediately after a phone write. These
        # measurements do not label that first sample a new phoneme's attack.
        first = -(-start * 48000 // hz)
        last = min(len(samples), -(-end * 48000 // hz))
        window = samples[first:last].astype(np.int64)
        entry["pcm_peak"] = int(np.max(np.abs(window))) if len(window) else 0
        entry["pcm_rms"] = float(np.sqrt(np.mean(window * window))) if len(window) else 0
        entry["amp_steps"] = [[r["tick"], r["amp_a"], r["amp_c"], r["amp_target"]]
                              for p, r in zip(states, states[1:])
                              if start <= r["tick"] < end and
                              (r["amp_a"], r["amp_c"]) != (p["amp_a"], p["amp_c"])]
        segments.append(entry)
    return {"segments": segments, "u68_rises_to_15": envelope_ramps(states, hz)}


def audit_trace(trace: Trace, out: Path, diagnostic: Path, renderer: Path) -> dict:
    out.mkdir(parents=True, exist_ok=True)
    # The regular renderer writes its exact ordered input and tables here.
    reference = render(trace, out / "reference", "prototype", renderer)
    result = subprocess.run([str(diagnostic), str(out / "reference/tables.txt"),
                             str(out / "reference/events.txt"), str(out / "states.csv"),
                             str(out / "diagnostic.pcm")], check=True, capture_output=True, text=True)
    metrics = json.loads(result.stdout)
    samples = np.fromfile(out / "diagnostic.pcm", dtype="<i2")
    with wave.open(str(out / "reference/prototype.wav"), "rb") as wav:
        normal = np.frombuffer(wav.readframes(wav.getnframes()), dtype="<i2").reshape(-1, 2)
    if not np.array_equal(samples, normal[:, 0]):
        raise AssertionError("diagnostic scheduler changed the prototype's socket-0 PCM")
    states = load_states(out / "states.csv")
    summary = {"name": trace.metadata["name"], "metadata": trace.metadata,
               "effective_clock_hz": trace.xck_hz, "duration_ticks": trace.duration_ticks,
               "trace_events": [asdict(e) for e in trace.events if e.socket == 0],
               "metrics": metrics, "pcm_matches_normal_render": True,
               "normal_render_wav_sha256": reference["wav_sha256"],
               "mono_pcm_sha256": hashlib.sha256(samples.tobytes()).hexdigest(),
               "state_csv_sha256": digest(out / "states.csv"),
               "first_nonzero_pcm_frame": (int(np.flatnonzero(samples)[0]) if np.any(samples) else None),
               "pcm_peak": int(np.max(np.abs(samples.astype(np.int64)))),
               **summarize(trace, states, samples)}
    (out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    return summary


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=CACHE / "attack_audit")
    args = parser.parse_args()
    out = args.output.resolve()
    evidence = archive_evidence(out)
    diagnostic = build_diagnostic(out)
    renderer = build_host()
    fixture = json.loads(FIXTURE.read_text(encoding="utf-8"))
    reports = {}
    for rate in (11, 12):
        for phrase in fixture["translated_player"]["phrases"]:
            if phrase["id"] not in ("A", "B", "E"):
                continue
            name = f"rate-{rate:X}/{phrase['id']}"
            reports[name] = audit_trace(make_trace(fixture, phrase, rate), out / name,
                                        diagnostic, renderer)
            print(f"{name}: identical PCM; {reports[name]['metrics']['state_records']} state records")
    reports["hello_four"] = audit_trace(make_demo("hello_four", filter_frequency=231),
                                       out / "hello_four", diagnostic, renderer)
    (out / "manifest.json").write_text(json.dumps({
        "purpose": "Read-only attack timing audit; no sound changes or acoustic fitting",
        "archive_commit": ARCHIVE_COMMIT, "prototype_evidence": evidence, "reports": reports,
        "limits": ["Physical SSI WAV pending; prototype is not final SSI silicon.",
                   "mb-audit register sequences use nominal requests, omitting 6502 IRQ latency.",
                   "State rows record relevant state changes and bus writes, not every XCK edge.",
                   "Tract output columns describe those instants; diagnostic.pcm contains every output sample.",
                   "U20=-1 means unknown; source FRIC1 fallback does not make it known."]}, indent=2) + "\n")
    print(out / "manifest.json")


if __name__ == "__main__":
    main()
