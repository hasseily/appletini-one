#!/usr/bin/env python3
"""Build and run the isolated ONE//e speaker-audio source test."""

from __future__ import annotations

import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "build" / "onee_speaker_audio_sim"
RTL = ROOT / "hdl" / "apple" / "onee_speaker_audio.sv"
BENCH = ROOT / "hdl" / "sim" / "tb_onee_speaker_audio.sv"


def vivado_tool(name: str) -> str:
    tool = shutil.which(f"{name}.bat") or shutil.which(name)
    if tool:
        return tool
    raise FileNotFoundError(f"unable to locate Vivado tool {name}")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


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


def static_contract_checks() -> None:
    rtl = RTL.read_text(encoding="utf-8")

    for token in (
        "input  logic                       clk",
        "input  logic                       resetn",
        "input  logic                       enabled",
        "input  logic                       speaker_level",
        "input  logic                       audio_sample_tick",
        "output logic signed [15:0]         audio_mono",
    ):
        require(token in rtl, f"missing speaker-audio contract: {token}")

    require(
        "target_sample = SPEAKER_AMPLITUDE;" in rtl and
        "target_sample = -SPEAKER_AMPLITUDE;" in rtl and
        "dc_error_wide =" in rtl and
        "dc_next_wide =" in rtl and
        "highpass_wide =" in rtl and
        "{dc_error_wide[17], dc_error_wide}" in rtl and
        "{dc_step_wide[17], dc_step_wide}" in rtl,
        "speaker path must subtract a tracked DC estimate from both levels",
    )
    require(
        "else if (!enabled) begin" in rtl and
        "audio_mono    <= 16'sd0;" in rtl and
        "else if (audio_sample_tick) begin" in rtl,
        "reset/disable must silence output and samples must change on ticks",
    )
    require(
        "function automatic logic signed [15:0] saturate_pcm16" in rtl and
        "16'sh7FFF" in rtl and
        "-16'sd32768" in rtl,
        "speaker output must saturate to signed 16-bit bounds",
    )


def main() -> int:
    static_contract_checks()

    if OUT_DIR.exists():
        shutil.rmtree(OUT_DIR)
    OUT_DIR.mkdir(parents=True)

    run([
        vivado_tool("xvlog"), "--sv", str(RTL), str(BENCH),
    ], "xvlog.log")
    run([
        vivado_tool("xelab"), "tb_onee_speaker_audio",
        "-s", "tb_onee_speaker_audio_snap",
    ], "xelab.log")
    output = run([
        vivado_tool("xsim"), "tb_onee_speaker_audio_snap", "--runall",
    ], "xsim.log")

    require(
        "ONEE SPEAKER AUDIO PASS" in output,
        "simulation did not report the speaker-audio pass marker",
    )
    print("ONE//e speaker audio source test passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
