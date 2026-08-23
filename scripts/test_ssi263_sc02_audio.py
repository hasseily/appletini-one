#!/usr/bin/env python3
"""Build and run the focused native SSI-263 / SC-02 audio test."""

from __future__ import annotations

import re
import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "build" / "ssi263_sc02_audio_sim"
PASS_MARKER = "SSI263 SC02 AUDIO PASS"


def static_checks() -> None:
    source = (ROOT / "hdl" / "apple" / "ssi263_sc02_audio.sv").read_text(
        encoding="utf-8"
    )
    required = (
        "module ssi263_sc02_audio #(",
        "parameter logic [3:0] NOISE_D1_SEED",
        "input  logic               fricative",
        "input  logic               voiced",
        "input  logic               noise_clock_ce",
        "input  logic               fric1_sw",
        "input  logic               fric2_sw",
        "input  logic               voice_toggle",
        "input  logic               filter_phase_ce",
        "noise_d1_q <= {noise_d1_q[2:0], noise_d3_q[3]};",
        "noise_d2_q <= {noise_d2_q[3:0], noise_d4_q[4]};",
        "noise_d3_q <= {noise_d3_q[2:0], noise_d2_q[4]};",
        "noise_d4_q <= {noise_d4_q[3:0], noise_feedback};",
        "voice_shape_q <= voice_shape_q + 4'h1;",
        "audio_sample <= sat16_from24_q16(output_hold_q);",
        "f1_state_q",
        "f2_res_state_q",
        "f2_state_q",
        "f3_state_q",
        "f4_state_q",
        "f5_state_q",
        "fric1_state_q",
        "fric2_state_q",
        "engine_busy_q",
        "engine_overrun_q",
        "engine_stage_q",
        "53-cycle scheduler",
        "four DSP48E1 cells",
        "product_a_q <= engine_product_a;",
        "product_b_q <= engine_product_b;",
        "drive_i_q <= sat24_from48(product_a_q_ext >>> 14);",
        "engine_product_a = engine_operand_a * engine_coefficient_a;",
        "engine_product_b = engine_operand_b * engine_coefficient_b;",
        "if (!value[47] && (|value[46:23]))",
        "else if (value[47] && !(&value[46:23]))",
    )
    for text in required:
        if text not in source:
            raise RuntimeError(f"native audio contract is missing: {text}")

    if re.search(r"\b(?:sc01|votrax)\b", source, re.IGNORECASE):
        raise RuntimeError("native SC-02 audio must not depend on legacy speech")
    if re.search(
        r"filter_frequency\s*==\s*(?:8'h)?ff", source, re.IGNORECASE
    ):
        raise RuntimeError("FILT=FF is maximum rate, not a silence selector")
    if re.search(r"/\s*(?:3300|3900|1126)\b", source):
        raise RuntimeError("source-derived gain tables must not infer dividers")
    if source.count("engine_operand_a * engine_coefficient_a") != 1 or source.count(
        "engine_operand_b * engine_coefficient_b"
    ) != 1:
        raise RuntimeError("resonator scheduler must expose two RTL product lanes")
    if re.search(r"sat24_from48\s*\(\s*engine_product_[ab]", source):
        raise RuntimeError("every DSP product must be registered before saturation")


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
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(
        ROOT / "hdl" / "apple" / "ssi263_sc02_rom.mem",
        OUT_DIR / "ssi263_sc02_rom.mem",
    )
    run(
        [
            vivado_tool("xvlog"),
            "--sv",
            str(ROOT / "hdl" / "apple" / "ssi263_sc02_core.sv"),
            str(ROOT / "hdl" / "apple" / "ssi263_sc02_audio.sv"),
            str(ROOT / "hdl" / "sim" / "tb_ssi263_sc02_audio.sv"),
        ],
        "xvlog.log",
    )
    run(
        [
            vivado_tool("xelab"),
            "tb_ssi263_sc02_audio",
            "-s",
            "tb_ssi263_sc02_audio_snap",
            "--timescale",
            "1ns/1ps",
        ],
        "xelab.log",
    )
    output = run(
        [vivado_tool("xsim"), "tb_ssi263_sc02_audio_snap", "--runall"],
        "xsim.log",
    )
    if PASS_MARKER not in output or "SSI263 SC02 AUDIO FAIL" in output:
        print(output)
        raise RuntimeError("native SSI-263 / SC-02 audio test did not pass")
    print(next(line for line in output.splitlines() if PASS_MARKER in line))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
