#!/usr/bin/env python3
"""Replay Phasor demo speech writes through the real card and SSI voices."""
from __future__ import annotations

import csv
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/phasor_demo_filter_sim"
SOURCES = [ROOT / name for name in (
    "hdl/globals.sv", "hdl/apple/ssi263_formant_pkg.sv",
    "hdl/apple/sc01a_digital_core.sv", "hdl/apple/ssi263_formant_backend.sv",
    "hdl/apple/ssi263_bus_wrapper.sv", "hdl/apple/ssi263_voice.sv",
    "hdl/apple/ssi263_xck_ce.sv", "hdl/apple/via6522.v", "hdl/apple/YM2149.sv",
    "hdl/apple/mockingboard.sv", "hdl/sim/tb_phasor_demo_filter.sv",
)]


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def tool(name: str) -> str:
    found = shutil.which(name + ".bat") or shutil.which(name)
    if not found:
        raise FileNotFoundError(f"Vivado simulation tool not found: {name}")
    return found


def run(args: list[str], name: str) -> str:
    result = subprocess.run(args, cwd=OUT, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    (OUT / name).write_text(result.stdout, encoding="utf-8")
    if result.returncode:
        raise RuntimeError(f"Simulation command failed; see {OUT / name}")
    return result.stdout


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    source_hashes = {p.relative_to(ROOT).as_posix(): sha(p) for p in SOURCES}
    source_hashes[Path(__file__).relative_to(ROOT).as_posix()] = sha(Path(__file__))
    run([tool("xvlog"), "--sv", *map(str, SOURCES)], "compile.log")
    run([tool("xelab"), "tb_phasor_demo_filter", "-s", "phasor_demo_filter",
         "--timescale", "1ns/1ps"], "elaborate.log")
    text = run([tool("xsim"), "phasor_demo_filter", "--runall"], "simulate.log")
    match = re.search(r"PHASOR DEMO FILTER PASS checks=(\d+) writes=(\d+) "
                      r"source_checks=(\d+) audio_frames=(\d+)", text)
    assert match and "PHASOR DEMO FILTER FAIL" not in text, "Missing RTL PASS marker"
    counts = dict(zip(("checks", "writes", "source_checks", "audio_frames"), map(int, match.groups())))
    audio = {0: [], 128: [], 255: []}
    with (OUT / "phasor_audio.csv").open(newline="") as stream:
        for record in csv.reader(stream):
            ff, index, primary, secondary, left, right = map(int, record)
            assert index == len(audio[ff])
            assert all(-32768 <= v <= 32767 for v in (primary, secondary, left, right))
            audio[ff].append((primary, secondary, left, right))
    audio_metrics = {}
    for ff, samples in audio.items():
        assert len(samples) == 1024
        changed = sum(p != s for p, s, _, _ in samples)
        nonzero = sum(p != 0 for p, _, _, _ in samples)
        assert nonzero > 32
        assert changed == 0 if ff == 128 else changed > 32
        audio_metrics[ff] = dict(samples=len(samples), different_from_128=changed, nonzero=nonzero)
    assert [s for _, s, _, _ in audio[0]] == [s for _, s, _, _ in audio[128]] == [s for _, s, _, _ in audio[255]], "Neutral reference changed between reset-identical runs"
    for name, digest in source_hashes.items():
        assert sha(ROOT / name) == digest, f"Source changed during simulation: {name}"
    report = dict(status="PASS", counts=counts, audio=audio_metrics,
                  mapping="step_q8=128+FF; FF128 bypass; reset register remains0",
                  bus_coverage="MB/native; primary/secondary/both; all256 values at aliases4..7; Echo+ and nonselected cycles rejected",
                  demo_provenance=dict(version="PHASOR1 Ver1.1.0", default_pitch=232,
                      sha256="1ac1856df6e54f026f642c5f75e004fb6fc044c95eed1d413d6604d17dc1f9f8",
                      url="https://downloads.reactivemicro.com/Apple%20II%20Items/Hardware/Phasor/Software/PHASOR1.DSK",
                      tts_code="$775B-$776B; Pitch RAM$770F -> slot4$C444"),
                  source_sha256=source_hashes,
                  audio_csv_sha256=sha(OUT / "phasor_audio.csv"),
                  limitation="Compressed simulation cadence; proves bus/voice/output response, not physical-frequency calibration or hardware behavior")
    (OUT / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print("PHASOR DEMO FILTER PASS " + json.dumps(counts))


if __name__ == "__main__":
    main()
