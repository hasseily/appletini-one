#!/usr/bin/env python3
"""Replay Phasor demo speech writes through the real card and SSI voices."""
from __future__ import annotations

import csv
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/phasor_demo_filter_sim"
SOURCES = [ROOT / name for name in (
    "hdl/globals.sv", "hdl/apple/ssi263_parameter_rom.sv",
    "hdl/apple/ssi263_native_controller.sv", "hdl/apple/ssi263_native_pitch.sv",
    "hdl/apple/ssi263_native_source.sv", "hdl/apple/ssi263_native_tract.sv",
    "hdl/apple/ssi263_native_engine.sv", "hdl/apple/ssi263_response_timing.sv",
    "hdl/apple/ssi263_stereo_mixer.sv",
    "hdl/apple/ssi263_bus_wrapper.sv", "hdl/apple/ssi263_voice.sv",
    "hdl/apple/ssi263_xck_ce.sv", "hdl/apple/via6522.v", "hdl/apple/YM2149.sv",
    "hdl/apple/mockingboard.sv", "hdl/sim/tb_phasor_demo_filter.sv",
)]
ROM = ROOT / "hdl/apple/ssi263_sc02_rom.mem"


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def linux_path(path: Path) -> str:
    path = path.resolve()
    return "/mnt/" + path.drive[0].lower() + path.as_posix()[2:] if os.name == "nt" else str(path)


def run(args: list[str], name: str) -> str:
    result = subprocess.run(args, cwd=OUT, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    (OUT / name).write_text(result.stdout, encoding="utf-8")
    if result.returncode:
        raise RuntimeError(f"Simulation command failed; see {OUT / name}\n{result.stdout[-4000:]}")
    return result.stdout


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    source_hashes = {p.relative_to(ROOT).as_posix(): sha(p) for p in (*SOURCES, ROM)}
    source_hashes[Path(__file__).relative_to(ROOT).as_posix()] = sha(Path(__file__))
    shutil.copyfile(ROM, OUT / ROM.name)
    prefix = ["wsl", "-d", "Ubuntu", "--"] if os.name == "nt" else []
    obj = OUT / "obj_dir"
    # Existing VIA/AY/card sources carry legacy width warnings. Keep them in
    # compile.log; new native modules have separate strict lint regressions.
    run(prefix + ["verilator", "--binary", "--timing", "--build", "-j", "4", "-Wno-fatal",
                  "--top-module", "tb_phasor_demo_filter", "--Mdir", linux_path(obj),
                  *map(linux_path, SOURCES)], "compile.log")
    text = run(prefix + [linux_path(obj / "Vtb_phasor_demo_filter")], "simulate.log")
    match = re.search(r"PHASOR DEMO FILTER PASS checks=(\d+) writes=(\d+) "
                      r"source_checks=(\d+) audio_frames=(\d+) completions=(\d+)", text)
    assert match and "PHASOR DEMO FILTER FAIL" not in text, "Missing RTL PASS marker"
    counts = dict(zip(("checks", "writes", "source_checks", "audio_frames", "completions"), map(int, match.groups())))
    audio = {0: [], 128: [], 255: []}
    with (OUT / "phasor_audio.csv").open(newline="") as stream:
        for record in csv.reader(stream):
            ff, index, primary, secondary, left, right = map(int, record)
            assert index == len(audio[ff])
            assert all(-32768 <= v <= 32767 for v in (primary, secondary, left, right))
            audio[ff].append((primary, secondary, left, right))
    audio_metrics = {}
    for ff, samples in audio.items():
        assert len(samples) == 2048
        changed = sum(p != s for p, s, _, _ in samples)
        nonzero = sum(p != 0 for p, _, _, _ in samples)
        assert nonzero > 16
        assert changed == 0 if ff == 128 else changed > 32
        audio_metrics[ff] = dict(samples=len(samples), different_from_128=changed, nonzero=nonzero)
    assert [s for _, s, _, _ in audio[0]] == [s for _, s, _, _ in audio[128]] == [s for _, s, _, _ in audio[255]], "Neutral reference changed between reset-identical runs"
    mode_audio = {(ff, mode): [] for ff in (128, 232) for mode in (0, 5)}
    with (OUT / "phasor_mode_audio.csv").open(newline="") as stream:
        for record in csv.reader(stream):
            ff, mode, index, primary, secondary = map(int, record)
            assert index == len(mode_audio[ff, mode])
            mode_audio[ff, mode].append((primary, secondary))
    for ff in (128, 232):
        assert len(mode_audio[ff, 0]) == len(mode_audio[ff, 5]) == 1024
        assert mode_audio[ff, 0] == mode_audio[ff, 5], f"FF{ff}: card mode changed native SSI PCM"
        assert all(sum(sample[ch] != 0 for sample in mode_audio[ff, 0]) > 16 for ch in (0, 1))
    assert "PHASOR MODE PCM PASS comparisons=4096 frames_per_run=1024" in text
    assert "PHASOR Q3 RINGING PCM PASS comparisons=2048 frames=1024" in text
    for name, digest in source_hashes.items():
        assert sha(ROOT / name) == digest, f"Source changed during simulation: {name}"
    report = dict(status="PASS", counts=counts, audio=audio_metrics,
                  mapping="Native half-phase interval 256-FF effective XCK ticks; FF128 is not a bypass; reset register remains0",
                  bus_coverage="MB/native; primary/secondary/both; all256 values at aliases4..7; Echo+ and nonselected cycles rejected",
                  demo_provenance=dict(version="PHASOR1 Ver1.1.0", default_pitch=232,
                      sha256="1ac1856df6e54f026f642c5f75e004fb6fc044c95eed1d413d6604d17dc1f9f8",
                      url="https://downloads.reactivemicro.com/Apple%20II%20Items/Hardware/Phasor/Software/PHASOR1.DSK",
                      tts_code="$775B-$776B; Pitch RAM$770F -> slot4$C444"),
                  source_sha256=source_hashes,
                  audio_csv_sha256=sha(OUT / "phasor_audio.csv"),
                  mode_invariance=dict(modes=[0, 5], filter_frequencies=[128, 232],
                      frames_per_run=1024, raw_socket_pcm_comparisons=4096,
                      reset_and_q3_sample_phases_identical=True,
                      result="byte-identical native SSI PCM in Mockingboard and Phasor modes",
                      csv_sha256=sha(OUT / "phasor_mode_audio.csv")),
                  fabric_period_ns=7.5, q3_period_fabric_clocks=65,
                  q3_ringing=dict(pulse_widths_ns=[30, 45], filter_frequency=232,
                      frames=1024, raw_socket_pcm_comparisons=2048,
                      result="byte-identical PCM with short high pulses and low notches"),
                  effective_xck_period_fabric_clocks=130, audio_period_fabric_clocks=2778,
                  response_coverage="FF-independent response counters/D7/IRQ; source/envelope state may legitimately differ with FF",
                  limitation="RTL bus/audio proof at physical clock spacing; not physical SSI accuracy or a board hardware result")
    (OUT / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print("PHASOR DEMO FILTER PASS " + json.dumps(counts))


if __name__ == "__main__":
    main()
