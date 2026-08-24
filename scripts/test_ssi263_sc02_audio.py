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
LEGACY_SPEECH_RE = re.compile(
    r"(?i)(?<![a-z0-9])(?:sc(?:[-_ ]?0?1)a?|votrax)(?![a-z0-9])"
)


def strip_verilog_comments(source: str) -> str:
    source = re.sub(r"/\*.*?\*/", "", source, flags=re.DOTALL)
    return re.sub(r"//[^\r\n]*", "", source)


def parse_case_table(source: str, name: str) -> list[int]:
    match = re.search(
        rf"function automatic[^;]+\b{re.escape(name)}\s*\([^;]*;"
        rf"(?P<body>.*?)endfunction",
        source,
        re.DOTALL,
    )
    if not match:
        raise RuntimeError(f"unable to find RTL coefficient table {name}")
    values = [
        int(value)
        for value in re.findall(
            rf"\b{re.escape(name)}\s*=\s*\d+'s?d(\d+)",
            match.group("body"),
        )
    ]
    if len(values) != 16:
        raise RuntimeError(f"{name} has {len(values)} entries, expected 16")
    return values


def parse_constant(source: str, name: str) -> int:
    match = re.search(
        rf"\b{re.escape(name)}\s*=\s*\d+'s?d(\d+)", source
    )
    if not match:
        raise RuntimeError(f"unable to find RTL coefficient {name}")
    return int(match.group(1))


def parse_capacitor_weights(source: str, name: str) -> list[int]:
    match = re.search(
        rf"function automatic[^;]+\b{re.escape(name)}\s*\([^;]*;"
        rf"(?P<body>.*?)endfunction",
        source,
        re.DOTALL,
    )
    if not match:
        raise RuntimeError(f"unable to find RTL capacitor table {name}")
    weights = {
        int(bit): int(value)
        for bit, value in re.findall(
            r"if\s*\(code\[(\d)\]\)\s*total\s*=\s*total\s*\+\s*(\d+)",
            match.group("body"),
        )
    }
    if set(weights) != {0, 1, 2, 3}:
        raise RuntimeError(f"{name} does not define all four capacitor bits")
    return [weights[index] for index in range(4)]


def circuit_ratio_checks(source: str) -> None:
    """Check only the capacitor values and exact fixed-point ratios."""
    q14 = 16384
    capacitor_weights = {
        name: parse_capacitor_weights(source, f"{name}_capacitance")
        for name in (
            "f1",
            "f2",
            "f2_res",
            "f3",
            "f4",
            "voice",
            "fric1",
            "fric2",
            "filter_amp",
        )
    }
    expected_weights = {
        "f1": [160, 330, 660, 1300],
        "f2": [280, 560, 1120, 2300],
        "f2_res": [220, 430, 870, 1800],
        "f3": [210, 420, 820, 1640],
        "f4": [200, 400, 820, 1620],
        "voice": [220, 430, 870, 1800],
        "fric1": [270, 512, 1068, 2160],
        "fric2": [270, 530, 1082, 2160],
        "filter_amp": [76, 150, 300, 600],
    }
    if capacitor_weights != expected_weights:
        raise RuntimeError(
            "formant capacitor banks no longer match sheets 1 and 2: "
            f"{capacitor_weights}"
        )

    def selected_totals(weights: list[int]) -> list[int]:
        return [
            sum(value for bit, value in enumerate(weights) if code & (1 << bit))
            for code in range(16)
        ]

    def ratio_q14(numerator: int, denominator: int) -> int:
        return (numerator * q14 + denominator // 2) // denominator

    def ratio_q12(numerator: int, denominator: int) -> int:
        return (numerator * 4096 + denominator // 2) // denominator

    expected_tables = {
        "f1_b_q14": [
            ratio_q14(250 + total, 11500)
            for total in selected_totals(expected_weights["f1"])
        ],
        "f2_b_q14": [
            ratio_q14(500 + total, 6800)
            for total in selected_totals(expected_weights["f2"])
        ],
        "f2_alpha_q14": [
            ratio_q14(6800, 7000 + total)
            for total in selected_totals(expected_weights["f2_res"])
        ],
        "f2_a_q14": [
            ratio_q14(4700, 7000 + total)
            for total in selected_totals(expected_weights["f2_res"])
        ],
        "f3_b_q14": [
            ratio_q14(820 + total, 4700)
            for total in selected_totals(expected_weights["f3"])
        ],
        "f4_b_q14": [
            ratio_q14(1670 + total, 4300)
            for total in selected_totals(expected_weights["f4"])
        ],
        "voice_gain": [
            ratio_q12(total, 3300)
            for total in selected_totals(expected_weights["voice"])
        ],
        "fric1_gain": [
            ratio_q12(total, 3900)
            for total in selected_totals(expected_weights["fric1"])
        ],
        "fric2_gain": [
            ratio_q12(total, 3900)
            for total in selected_totals(expected_weights["fric2"])
        ],
        "fric1_source_magnitude": [
            (65536 * 3 * total + (653 * 3900) // 2) // (653 * 3900)
            for total in selected_totals(expected_weights["fric1"])
        ],
        "fric2_edge_magnitude": [
            (65536 * 6 * total + (653 * 3900) // 2) // (653 * 3900)
            for total in selected_totals(expected_weights["fric2"])
        ],
        "filter_amp_gain": [
            ratio_q14(total, 2750)
            for total in selected_totals(expected_weights["filter_amp"])
        ],
    }
    tables = {
        name: parse_case_table(source, name) for name in expected_tables
    }
    for name, expected in expected_tables.items():
        if tables[name] != expected:
            raise RuntimeError(
                f"{name} no longer equals its rounded capacitor ratios: "
                f"{tables[name]}"
            )

    expected_constants = {
        "F1_ALPHA_Q14": 16104,
        "F1_A_Q14": 3781,
        "F1_G_Q14": 3781,
        "F2_FRIC_H_Q14": 2409,
        "F3_ALPHA_Q14": 15715,
        "F3_A_Q14": 13040,
        "F3_G_Q14": 6687,
        "F4_ALPHA_Q14": 15656,
        "F4_A_Q14": 17112,
        "F5_ALPHA_Q14": 15154,
        "F5_A_Q14": 20645,
        "F5_B_Q14": 22320,
        "F5_FRIC_BASE_G_Q14": 5051,
        "F5_FRIC_SW_G_Q14": 16252,
        "FILTER_OUTPUT_ALPHA_Q14": 16086,
        "FRIC_DRIVE_MAG_Q16": 301,
        "FRIC_DRIVE_EDGE_Q16": 602,
    }
    constants = {
        name: parse_constant(source, name) for name in expected_constants
    }
    if constants != expected_constants:
        raise RuntimeError(
            "fixed formant charge ratios no longer match the schematic: "
            f"{constants}"
        )

    filter_amplitude = tables["filter_amp_gain"]

    if filter_amplitude[0] != 0 or filter_amplitude[-1] != 6709:
        raise RuntimeError("filter-amplitude endpoints are not 0 and 6709")
    if any(
        filter_amplitude[index] <= filter_amplitude[index - 1]
        for index in range(1, 16)
    ):
        raise RuntimeError("filter-amplitude codes are not strictly monotonic")
    for name in ("f1_b_q14", "f2_b_q14", "f3_b_q14", "f4_b_q14"):
        if any(
            tables[name][index] <= tables[name][index - 1]
            for index in range(1, 16)
        ):
            raise RuntimeError(f"{name} is not strictly increasing")
    for name in ("f2_alpha_q14", "f2_a_q14"):
        if any(
            tables[name][index] >= tables[name][index - 1]
            for index in range(1, 16)
        ):
            raise RuntimeError(f"{name} does not add damping with RES code")

def static_checks() -> None:
    source = (ROOT / "hdl" / "apple" / "ssi263_sc02_audio.sv").read_text(
        encoding="utf-8"
    )
    required = (
        "module ssi263_sc02_audio #(",
        "parameter logic [3:0] NOISE_D1_SEED",
        "parameter logic U60_OPEN_P3_LEVEL = 1'b0",
        "parameter logic U75_OPEN_P1_LEVEL = 1'b0",
        "input  logic               pd_rst_n",
        "input  logic               pw_3",
        "input  logic               noise_clock_ce",
        "input  logic               noise_shift_ce",
        "input  logic               fric1_sw",
        "input  logic               fric2_sw",
        "input  logic               voice_toggle",
        "input  logic               filter_phase_ce",
        "noise_d1_q <= {noise_d1_q[2:0], noise_d3_q[3]};",
        "noise_d2_q <= {noise_d2_q[3:0], noise_d4_q[4]};",
        "noise_d3_q <= {noise_d3_q[2:0], noise_d2_q[4]};",
        "noise_d4_q <= {noise_d4_q[3:0], noise_feedback};",
        "u60_parallel_value = {U60_OPEN_P3_LEVEL, 1'b0, 2'b11};",
        "voice_count_after_phi1 = voice_count_q;",
        "if (pd_rst_n && voice_load_pending_q)",
        "voice_count_after_phi1 = u60_parallel_value;",
        "else if (voice_count_q != 4'hF)",
        "voice_count_after_phi1 = voice_count_q + 4'h1;",
        "u60_tc = (voice_count_q == 4'hF);",
        "u60_tc_after_phi1 = (voice_count_after_phi1 == 4'hF);",
        "voice_source_after_phi1 = (powered_down || u60_tc_after_phi1) ?",
        "voice_count_q <= 4'hF;",
        "if (!pd_rst_n) begin",
        "pitch_sync1_q <= 1'b0;",
        "pitch_sync2_q <= 1'b0;",
        "voice_load_pending_q <= 1'b0;",
        "voice_count_q <= voice_count_after_phi1;",
        "u75_parallel_value = {2'b00, U75_OPEN_P1_LEVEL, 1'b1};",
        "noise_count_next = (noise_count_q == 4'hF) ?",
        "u75_parallel_value : noise_count_q + 4'h1;",
        "noise_force = ~(noise_count_q[2] | noise_count_q[3]);",
        "noise_bit = !(noise_d3_q[3] | (pw_3 && !voice_toggle)) &&",
        "(!voice_toggle || (voice_amp_code == 4'd0));",
        "voice_source = voice_magnitude;",
        "fric_drive = noise_bit ? FRIC_DRIVE_MAG_Q16 :",
        "-FRIC_DRIVE_MAG_Q16;",
        "FRIC_DRIVE_EDGE_Q16: begin",
        "fric2_drive_charge = fric2_edge_magnitude(fric_amp_code);",
        "-FRIC_DRIVE_EDGE_Q16: begin",
        "fric1_source = noise_bit ?",
        "-fric1_source_magnitude(fric_amp_code) :",
        "fric1_source_magnitude(fric_amp_code);",
        "fric2_source = fric2_source_next;",
        "if (fric_drive != fric_drive_history_q)",
        "fric2_source_state_q <= fric2_source_next;",
        "fric_drive_history_q <= fric_drive;",
        "fric_drive_history_q <= -FRIC_DRIVE_MAG_Q16;",
        "if (noise_clock_ce)",
        "noise_count_q <= noise_count_next;",
        "if (noise_shift_ce) begin",
        "fric2_source_phi0_q <= fric2_source_next;",
        "fric2_sw_phi0_q <= fric2_sw;",
        "c143_live_delta =",
        "$signed({c143_source_plate_q[23], c143_source_plate_q}) -",
        "$signed({fric1_source[23], fric1_source});",
        "c143_delta_with_live = c143_phi0_delta_q;",
        "c143_delta_with_live = c143_phi0_delta_q + c143_live_delta;",
        "c143_phi0_delta_q <= fric1_sw ?",
        "c143_live_delta : 25'sd0;",
        "c143_source_plate_q <= fric1_source;",
        "c143_delta_hold_q <= c143_delta_with_live;",
        "c143_phi0_delta_q <= 25'sd0;",
        "c143_source_plate_q <= 24'sd0;",
        "f1_input_q <= voice_source_after_phi1;",
        "f1_code_phi0_q <= f1_code;",
        "f2_code_phi0_q <= f2_code;",
        "f2_res_code_phi0_q <= f2_res_code;",
        "f3_code_phi0_q <= f3_code;",
        "f4_code_phi0_q <= f4_code;",
        "filter_amp_phi0_q <= filter_amp_code;",
        "engine_b_q <= f1_b_q14(f1_code_phi0_q);",
        "f2_alpha_q14(f2_res_code_phi0_q)",
        "f2_a_q14(f2_res_code_phi0_q)",
        "engine_b_q <= f2_b_q14(f2_code_phi0_q);",
        "engine_b_q <= f3_b_q14(f3_code_phi0_q);",
        "engine_b_q <= f4_b_q14(f4_code_phi0_q);",
        "engine_b_q <= F5_B_Q14;",
        "engine_output_delta_q <=",
        "engine_output_delta_q <= c143_delta_hold_q;",
        "F2_FRIC_H_Q14",
        "fric2_base_history_q[23]",
        "F5_FRIC_BASE_G_Q14",
        "fric2_sw_history_q[23]",
        "F5_FRIC_SW_G_Q14",
        "fric2_base_history_q <=",
        "if (fric2_sw_phi0_q)",
        "fric2_sw_history_q <=",
        "engine_stage_q <= 4'd8;",
        "34-cycle scheduler",
        "engine_operand_a = engine_output_delta_q;",
        "engine_coefficient_a = engine_h_q;",
        "engine_operand_a = {output_hold_q[23], output_hold_q};",
        "engine_coefficient_a = FILTER_OUTPUT_ALPHA_Q14;",
        "engine_operand_b = output_sum_q;",
        "engine_coefficient_b = filter_amp_gain(filter_amp_phi0_q);",
        "output_sum_q <=",
        "output_old_state_q <= engine_state_y_q;",
        "engine_stage_q <= 4'd9;",
        "output_old_state_q[23]",
        "f5_state_q[23]",
        "output_hold_q <= engine_output_next;",
        "if (closure)",
        "reconstruction_hold_q <= output_hold_q;",
        "if (audio_tick) begin",
        "$signed({{3{reconstruction_hold_q[23]}}",
        "reconstruction_hold_q}) >>> 1",
        "audio_sample <= 16'sd0;",
        "f1_input_history_q",
        "f3_side_input_q",
        "f3_side_history_q",
        "engine_busy_q",
        "engine_overrun_q",
        "engine_state_y_q <= f1_state_q;",
        "engine_state_p_q <= f1_history_q;",
        "engine_state_y_q <= f2_state_q;",
        "engine_state_p_q <= f2_history_q;",
        "engine_state_y_q <= f3_state_q;",
        "engine_state_p_q <= f3_history_q;",
        "engine_state_y_q <= f4_state_q;",
        "engine_state_p_q <= f4_history_q;",
        "engine_state_y_q <= f5_state_q;",
        "engine_state_p_q <= f5_history_q;",
        "engine_operand_b = engine_main_delta_q;",
        "engine_coefficient_a = engine_alpha_q;",
        "engine_coefficient_b = engine_a_q;",
        "engine_coefficient_a = engine_b_q;",
        "engine_operand_a = engine_side_delta_q;",
        "engine_coefficient_a = engine_g_q;",
        "engine_g_q <= F1_G_Q14;",
        "engine_g_q <= F3_G_Q14;",
        "recurrence_accumulator = product_a_q_ext + product_b_q_ext;",
        "charge_accumulator = recurrence_accumulator_q + product_a_q_ext;",
        "rounded_charge_accumulator = charge_accumulator +",
        "engine_charge_next = sat24_from48(",
        "state_y_q14 =",
        "section_accumulator = state_y_q14 - product_a_q_ext;",
        "complete_section_accumulator = section_accumulator_q +",
        "rounded_section_accumulator = complete_section_accumulator +",
        "engine_section_next = sat24_from48(",
        "output_accumulator = product_a_q_ext + product_b_q_ext;",
        "rounded_output_accumulator = output_accumulator +",
        "engine_output_next = sat24_from48(",
        "engine_product_a = engine_operand_a * engine_coefficient_a;",
        "engine_product_b = engine_operand_b * engine_coefficient_b;",
        "if (!value[47] && (|value[46:23]))",
        "else if (value[47] && !(&value[46:23]))",
    )
    for text in required:
        if text not in source:
            raise RuntimeError(f"native audio contract is missing: {text}")

    forbidden = (
        "FRIC1_COUPLING_Q14",
        "FRIC2_COUPLING_Q14",
        "F3_FRIC_G_Q14",
        "fric1_injection_q",
        "fric2_injection_q",
        "fric1_history_q",
        "fric1_edge_q",
        "fric1_edge_accumulator",
        "input_mix_stage_q",
        "33-cycle scheduler",
        "32-cycle scheduler",
        "reconstruction_hold_q <= engine_output_next",
        "engine_b_q <= f1_b_q14(f1_code);",
        "f2_alpha_q14(f2_res_code)",
        "f2_a_q14(f2_res_code)",
        "engine_b_q <= f2_b_q14(f2_code);",
        "engine_b_q <= f3_b_q14(f3_code);",
        "engine_b_q <= f4_b_q14(f4_code);",
        "filter_amp_gain(filter_amp_code)",
        "fric_source_phi0_q",
        "fric1_source_phi0_q",
        "fric1_sw_phi0_q",
        "fric1_partial_q",
        "_cos_q14",
        "_radius_q14",
        "pole_a1",
        "pole_r2",
        "pole_b0",
        "stop_class",
        "source_voiced",
        "source_fricative",
        "LINE_OUTPUT_SHIFT",
        "dc_input",
        "dc_delta_q",
        "dc_sum_q",
        "dc_output_q",
        "dc_filtered_wide",
        "voice_shape_q",
        "voiced_q",
        "voice_terminal_event",
        "fric_drive = 18'sd0;",
        "fric_drive = noise_bit ? 18'sd65536",
    )
    for text in forbidden:
        if text in source:
            raise RuntimeError(f"native audio contract retains stale path: {text}")

    if LEGACY_SPEECH_RE.search(strip_verilog_comments(source)):
        raise RuntimeError("native SC-02 audio must not depend on legacy speech")
    if re.search(r"\bfric_source\b", source):
        raise RuntimeError("U157 and U152 must not collapse into one source")
    if re.search(r"fric[12]_(?:state|quadrature|input)_q", source):
        raise RuntimeError(
            "FRIC_1 and FRIC_2 are tract injections, not output resonators"
        )
    if re.search(
        r"\bengine_(?:state_y1|state_y2|input|alpha|a|b)\b", source
    ):
        raise RuntimeError(
            "section operands must be registered before the DSP scheduler"
        )
    if re.search(r"stop_(?:armed|release)", source):
        raise RuntimeError("the circuit must not use an invented stop state")
    if re.search(r"voice_count_q\s*<=\s*4'h0", source):
        raise RuntimeError("U60 must not load or clear to zero")
    if re.search(
        r"filter_frequency\s*==\s*(?:8'h)?ff", source, re.IGNORECASE
    ):
        raise RuntimeError("FILT=FF is maximum rate, not a silence selector")
    if re.search(
        r"/\s*(?:3300|3900)\b", strip_verilog_comments(source)
    ):
        raise RuntimeError("source-derived gain tables must not infer dividers")
    for signal in ("phone_active", "fricative", "voiced", "pw_2"):
        if re.search(rf"\binput\s+logic[^;]*\b{signal}\b", source):
            raise RuntimeError(
                f"native audio retains invented source input {signal}"
            )
    if source.count("engine_operand_a * engine_coefficient_a") != 1 or source.count(
        "engine_operand_b * engine_coefficient_b"
    ) != 1:
        raise RuntimeError("charge scheduler must expose two RTL product lanes")
    if re.search(r"sat24_from48\s*\(\s*engine_product_[ab]", source):
        raise RuntimeError("every DSP product must be registered before saturation")
    circuit_ratio_checks(source)


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
