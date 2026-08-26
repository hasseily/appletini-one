#!/usr/bin/env python3
"""Check SSI phone-start history masking and the anti-crackle waveform."""

from __future__ import annotations

import hashlib
import shutil
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "build" / "ssi263_history_mask_sim"
WAVE_DIR = OUT_DIR / "waveform"
PASS_MARKER = "SSI263 HISTORY MASK PASS"
REFERENCE_WAVE_SHA256 = (
    "cfbf54f71df24b36d399ac90c5efb56daa9ce1a99d4751e895b9b896fcb5aea7"
)
SEGMENT_SAMPLES = 1_440
SEGMENT_COUNT = 6


def vivado_tool(name: str) -> str:
    tool = shutil.which(f"{name}.bat") or shutil.which(name)
    if tool:
        return tool
    raise FileNotFoundError(f"unable to locate Vivado tool {name}")


def run(command: list[str], cwd: Path, log_path: Path) -> str:
    completed = subprocess.run(
        command,
        cwd=cwd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    output = completed.stdout or ""
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.write_text(output, encoding="utf-8")
    if completed.returncode != 0:
        print(output)
        raise RuntimeError(
            f"{Path(command[0]).name} failed with {completed.returncode}; "
            f"see {log_path}"
        )
    return output


def run_mask_bench() -> None:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    run(
        [
            vivado_tool("xvlog"),
            "--sv",
            str(ROOT / "hdl" / "apple" / "ssi263_formant_pkg.sv"),
            str(ROOT / "hdl" / "apple" / "sc01a_digital_core.sv"),
            str(ROOT / "hdl" / "apple" / "ssi263_formant_backend.sv"),
            str(ROOT / "hdl" / "sim" / "tb_ssi263_history_mask.sv"),
        ],
        OUT_DIR,
        OUT_DIR / "xvlog.log",
    )
    run(
        [
            vivado_tool("xelab"),
            "tb_ssi263_history_mask",
            "-s",
            "tb_ssi263_history_mask_snap",
            "--timescale",
            "1ns/1ps",
        ],
        OUT_DIR,
        OUT_DIR / "xelab.log",
    )
    output = run(
        [vivado_tool("xsim"), "tb_ssi263_history_mask_snap", "--runall"],
        OUT_DIR,
        OUT_DIR / "xsim.log",
    )
    if PASS_MARKER not in output or "SSI263 HISTORY MASK FAIL" in output:
        print(output)
        raise RuntimeError("SSI263 history-mask RTL test did not pass")


def run_waveform_regression() -> None:
    run(
        [
            sys.executable,
            str(ROOT / "scripts" / "sim_ssi263_formant_rtl.py"),
            "--preset",
            "phones",
            "--phones",
            "2D,20,0C,30,07,29",
            "--segment-ms",
            "30",
            "--out-dir",
            str(WAVE_DIR),
        ],
        ROOT,
        OUT_DIR / "waveform.log",
    )

    sample_path = WAVE_DIR / "rtl_audio_i16.txt"
    digest = hashlib.sha256(sample_path.read_bytes()).hexdigest()
    if digest != REFERENCE_WAVE_SHA256:
        raise RuntimeError(
            "history masks changed the full-clear reference waveform: "
            f"expected {REFERENCE_WAVE_SHA256}, got {digest}"
        )

    samples = [int(line) for line in sample_path.read_text().splitlines()]
    expected_count = SEGMENT_SAMPLES * SEGMENT_COUNT
    if len(samples) != expected_count:
        raise RuntimeError(
            f"expected {expected_count} waveform samples, got {len(samples)}"
        )
    max_step = max(abs(b - a) for a, b in zip(samples, samples[1:]))
    if max_step > 3_000:
        raise RuntimeError(f"anti-crackle slew limit exceeded: {max_step}")
    for index in range(SEGMENT_COUNT):
        segment = samples[index * SEGMENT_SAMPLES:(index + 1) * SEGMENT_SAMPLES]
        mean = sum(segment) / len(segment)
        if abs(mean) > 500.0:
            raise RuntimeError(
                f"segment {index} accumulated abnormal DC: mean {mean:.2f}"
            )


def main() -> int:
    run_mask_bench()
    run_waveform_regression()
    print("SSI263 HISTORY MASK PASS")
    print(f"Reference waveform SHA256 {REFERENCE_WAVE_SHA256}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
