#!/usr/bin/env python3
"""Build and run the Phasor per-SSI output-stage regression."""

from __future__ import annotations

import re
import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "build" / "phasor_ssi263_output_stage_sim"
STAGE = ROOT / "hdl" / "apple" / "phasor_ssi263_output_stage.sv"
AUDIO = ROOT / "hdl" / "apple" / "ssi263_sc02_audio.sv"
VOICE = ROOT / "hdl" / "apple" / "ssi263_voice.sv"
CARD = ROOT / "hdl" / "apple" / "mockingboard.sv"
BENCH = ROOT / "hdl" / "sim" / "tb_phasor_ssi263_output_stage.sv"
PASS_MARKER = "PHASOR SSI263 OUTPUT PASS"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def constant(source: str, name: str) -> int:
    match = re.search(rf"\b{name}\b\s*=\s*18'sd(\d+)", source)
    if not match:
        raise RuntimeError(f"unable to read {name}")
    return int(match.group(1))


def static_checks() -> None:
    stage = STAGE.read_text(encoding="utf-8")
    audio = AUDIO.read_text(encoding="utf-8")
    voice = VOICE.read_text(encoding="utf-8")
    card = CARD.read_text(encoding="utf-8")
    bench = BENCH.read_text(encoding="utf-8")

    voice_trim = constant(audio, "VOICE_TRIM_U116_STEP_Q16")
    fric_drive = constant(audio, "FRIC_DRIVE_MAG_Q16")
    shift_match = re.search(r"OUTPUT_GAIN_SHIFT\s*=\s*(\d+)", stage)
    require(shift_match is not None, "output gain shift is missing")
    gain_shift = int(shift_match.group(1))

    require(voice_trim == 2048, "POT3 test calibration must be 1/32 rail")
    require(fric_drive == 301, "the drawn fricative divider must remain 301")
    require(gain_shift == 5, "the common SSI output gain must be x32")
    require((voice_trim << gain_shift) == 65536,
            "POT3 and output gain must preserve the former voiced level")
    require((fric_drive << gain_shift) == 9632,
            "the common stage must raise the fixed fricative source by x32")

    require("always_ff @(posedge clk)" in stage and
            "gained_sample_q <= gained_sample;" in stage and
            ".line_audio(ssi0_line_audio)" in card and
            ".line_audio(ssi1_line_audio)" in card and
            ".card_audio(ssi0_audio)" in card and
            ".card_audio(ssi1_audio)" in card and
            card.count("phasor_ssi263_output_stage ssi263_") == 2,
            "each SSI socket needs one registered card output stage")
    require("phasor_ssi263_output_stage" not in voice and
            "OUTPUT_GAIN_SHIFT" not in voice and
            "OUTPUT_GAIN_SHIFT" not in audio,
            "card gain must stay outside the SSI tract and socket wrapper")
    require(card.count("                    ssi0_audio);") == 3 and
            card.count("                    ssi1_audio);") == 3 and
            "mix_speech(\n                    ssi0_line_audio" not in card and
            "mix_speech(\n                    ssi1_line_audio" not in card,
            "only staged SSI audio may enter the PSG mixer")

    for marker in (
        "16'sd1023",
        "16'sd1024",
        "-16'sd1024",
        "-16'sd1025",
        "card disable did not mask the stage at its boundary",
    ):
        require(marker in bench, f"output-stage boundary coverage missing: {marker}")


def vivado_tool(name: str) -> str:
    tool = shutil.which(f"{name}.bat") or shutil.which(name)
    if tool:
        return tool
    raise FileNotFoundError(f"unable to locate Vivado tool {name}")


def run(command: list[str], log_name: str) -> str:
    completed = subprocess.run(
        command,
        cwd=OUT_DIR,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    output = completed.stdout or ""
    (OUT_DIR / log_name).write_text(output, encoding="utf-8")
    if completed.returncode != 0:
        print(output)
        raise RuntimeError(
            f"{Path(command[0]).name} failed with {completed.returncode}; "
            f"see {OUT_DIR / log_name}"
        )
    return output


def main() -> int:
    static_checks()
    if OUT_DIR.exists():
        shutil.rmtree(OUT_DIR)
    OUT_DIR.mkdir(parents=True)
    run([vivado_tool("xvlog"), "--sv", str(STAGE), str(BENCH)], "xvlog.log")
    run([
        vivado_tool("xelab"),
        "tb_phasor_ssi263_output_stage",
        "-s",
        "tb_phasor_ssi263_output_stage_snap",
        "--timescale",
        "1ns/1ps",
    ], "xelab.log")
    output = run([
        vivado_tool("xsim"),
        "tb_phasor_ssi263_output_stage_snap",
        "--runall",
    ], "xsim.log")
    require(PASS_MARKER in output and "PHASOR SSI263 OUTPUT FAIL:" not in output,
            "Phasor SSI output-stage regression did not pass")
    print(next(line for line in output.splitlines() if PASS_MARKER in line))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
