#!/usr/bin/env python3
"""Exercise SSI FF rate, real tract spectra, hot writes and unchanged timing."""

from __future__ import annotations

import csv
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

import numpy as np


ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build" / "ssi263_filter_frequency_sim"
SOURCES = [
    ROOT / "hdl/apple/ssi263_formant_pkg.sv",
    ROOT / "hdl/apple/sc01a_digital_core.sv",
    ROOT / "hdl/apple/ssi263_formant_backend.sv",
    ROOT / "hdl/apple/ssi263_bus_wrapper.sv",
    ROOT / "hdl/sim/tb_ssi263_filter_frequency.sv",
    ROOT / "hdl/sim/tb_ssi263_filter_register.sv",
]


def run(command: list[str], name: str) -> str:
    result = subprocess.run(command, cwd=OUT, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (OUT / name).write_text(result.stdout, encoding="utf-8")
    if result.returncode:
        raise RuntimeError(f"{command[0]} failed; see {OUT / name}\n{result.stdout}")
    return result.stdout


def tool(name: str) -> str:
    found = shutil.which(name + ".bat") or shutil.which(name)
    if found is None:
        raise FileNotFoundError(f"Vivado tool not found: {name}")
    return found


def check_signed_f1_recurrence(impulses: list[list[int]]) -> int:
    """Compare the RTL samples with a wide signed integer recurrence."""
    package = SOURCES[0].read_text()
    entries = dict((int(key, 16), -int(value) if minus else int(value))
                   for key, minus, value in re.findall(
                       r"7'h([0-9A-F]+): sc01a_f1_coeff = (-?)16'sd(\d+);",
                       package))
    coefficients = [entries[(7 << 3) | tap] for tap in range(7)]
    checked = 0
    for setting, samples in enumerate(impulses):
        step = (128, 256, 383)[setting]
        phase = 0
        x_history = [0, 0, 0]
        y_history = [0, 0, 0]
        output = 0
        # One zero frame precedes the impulse in the RTL fixture.
        for index in range(-1, len(samples)):
            source = 16384 if index == 0 else 0
            phase += step
            passes, phase = divmod(phase, 256)
            for _ in range(passes):
                terms = [source, *x_history, *y_history]
                accumulator = sum(c * x for c, x in zip(coefficients, terms))
                assert -(1 << 55) <= accumulator < (1 << 55)
                output = max(-(1 << 23), min((1 << 23) - 1, accumulator >> 15))
                x_history = [source, *x_history[:2]]
                y_history = [output, *y_history[:2]]
            if index >= 0:
                assert samples[index] == output, (
                    f"signed F1 mismatch setting={setting} sample={index}: "
                    f"RTL={samples[index]} oracle={output}")
                checked += 1
        assert min(samples) < 0 < max(samples), "signed polarity coverage missing"
    return checked


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    run([tool("xvlog"), "--sv", *map(str, SOURCES)], "xvlog.log")
    run([tool("xelab"), "tb_ssi263_filter_frequency", "-s", "filter_frequency",
         "--timescale", "1ns/1ps"], "frequency_xelab.log")
    output = run([tool("xsim"), "filter_frequency", "--runall"], "frequency_xsim.log")
    match = re.search(r"SSI263 FILTER FREQUENCY PASS checks=(\d+) "
                      r"core_checks=(\d+) frames=(\d+) max_latency=(\d+) "
                      r"responses=(\d+) done=(\d+)", output)
    if match is None or "SSI263 FILTER FREQUENCY FAIL" in output:
        raise RuntimeError(f"RTL test did not pass; see {OUT / 'frequency_xsim.log'}")
    counts = dict(zip(("checks", "core_checks", "frames", "max_latency",
                       "responses", "done"), map(int, match.groups())))
    if counts["max_latency"] >= 300:
        raise AssertionError("two-pass tract exceeded its bounded fabric budget")
    run([tool("xelab"), "tb_ssi263_filter_register", "-s", "filter_register",
         "--timescale", "1ns/1ps"], "register_xelab.log")
    register_output = run([tool("xsim"), "filter_register", "--runall"],
                          "register_xsim.log")
    register_match = re.search(r"SSI263 FILTER REGISTER PASS checks=(\d+) writes=1024",
                               register_output)
    if register_match is None:
        raise RuntimeError("register alias/IRQ/reset test did not pass")
    counts["register_checks"] = int(register_match[1])

    impulse = [[], [], []]
    with (OUT / "f1_impulses.csv").open(newline="") as handle:
        for setting, sample, value in csv.reader(handle):
            assert len(impulse[int(setting)]) == int(sample)
            impulse[int(setting)].append(int(value))
    peaks = []
    signed_samples = check_signed_f1_recurrence(impulse)
    for samples in impulse:
        assert len(samples) == 1024
        # Zero padding locates the resonant peak between captured DFT bins;
        # no synthetic DSP model substitutes for the actual RTL response.
        magnitudes = np.abs(np.fft.rfft(np.array(samples, dtype=float), 32768))
        frequencies = np.fft.rfftfreq(32768, 1 / 48000)
        candidates = (frequencies >= 150) & (frequencies <= 8000)
        selected = np.flatnonzero(candidates)[np.argmax(magnitudes[candidates])]
        peaks.append(float(frequencies[selected]))
    ratios = [peaks[0] / peaks[1], peaks[2] / peaks[1]]
    if not (0.45 < ratios[0] < 0.55 and 1.40 < ratios[1] < 1.60):
        raise AssertionError(f"wrong formant shift: peaks={peaks}, ratios={ratios}")
    frames = np.loadtxt(OUT / "frames.csv", delimiter=",", dtype=np.int64)
    assert np.all((-32768 <= frames[:, 3]) & (frames[:, 3] <= 32767))
    # Full voiced-phone windows include its initial attack. Short code-sweep
    # windows can legitimately land before the first audible source pulse.
    steady = np.loadtxt(OUT / "steady_audio.csv", delimiter=",", dtype=np.int64)
    steady_peaks = []
    for setting in range(3):
        rows = steady[steady[:, 0] == setting]
        assert len(rows) == 2048
        assert np.any(rows[:, 2] != 0), f"setting={setting} muted speech"
        if setting == 1:
            assert np.array_equal(rows[:, 2], rows[:, 3]), "neutral audio changed"
        else:
            assert np.any(rows[:, 2] != rows[:, 3]), "FF did not change audio"
        steady_peaks.append(int(np.max(np.abs(rows[:, 2]))))
    summary = {
        "status": "pass", **counts,
        "f1_peaks_hz_low_neutral_high": peaks,
        "f1_relative_peak_low_high": ratios,
        "voiced_audio_peaks_low_neutral_high": steady_peaks,
        "signed_f1_oracle_samples": signed_samples,
        "all_ff_codes": 256,
        "source_sha256": {
            str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in [*SOURCES, Path(__file__).resolve()]
        },
    }
    (OUT / "results.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
