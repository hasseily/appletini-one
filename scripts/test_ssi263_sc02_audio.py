#!/usr/bin/env python3
"""Build and run the focused native SSI-263 / SC-02 audio test."""

from __future__ import annotations

import cmath
import math
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


def spectral_checks(source: str) -> None:
    """Check the schematic charge ratios and their nominal voiced spectrum."""
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

    def check_section(
        alpha: float,
        a: float,
        b: float,
        name: str,
        require_conjugate: bool = True,
    ) -> None:
        recurrence = 1.0 + alpha - a * b
        if not 0.0 < alpha < 1.0:
            raise RuntimeError(f"{name} alpha is outside its stable range")
        discriminant = recurrence * recurrence - 4.0 * alpha
        roots = (
            (recurrence + cmath.sqrt(discriminant)) / 2.0,
            (recurrence - cmath.sqrt(discriminant)) / 2.0,
        )
        if any(abs(root) >= 1.0 for root in roots):
            raise RuntimeError(f"{name} has an unstable charge-state pole")
        if require_conjugate and discriminant >= 0.0:
            raise RuntimeError(f"{name} lost its conjugate formant poles")

    for index in range(16):
        check_section(
            constants["F1_ALPHA_Q14"] / q14,
            constants["F1_A_Q14"] / q14,
            tables["f1_b_q14"][index] / q14,
            f"F1 code {index:X}",
        )
        for frequency_code in range(16):
            check_section(
                tables["f2_alpha_q14"][index] / q14,
                tables["f2_a_q14"][index] / q14,
                tables["f2_b_q14"][frequency_code] / q14,
                f"F2 frequency {frequency_code:X}, RES {index:X}",
                require_conjugate=False,
            )
        check_section(
            constants["F3_ALPHA_Q14"] / q14,
            constants["F3_A_Q14"] / q14,
            tables["f3_b_q14"][index] / q14,
            f"F3 code {index:X}",
        )
        check_section(
            constants["F4_ALPHA_Q14"] / q14,
            constants["F4_A_Q14"] / q14,
            tables["f4_b_q14"][index] / q14,
            f"F4 code {index:X}",
        )
    check_section(
        constants["F5_ALPHA_Q14"] / q14,
        constants["F5_A_Q14"] / q14,
        constants["F5_B_Q14"] / q14,
        "F5",
    )

    rom = []
    for line in (ROOT / "hdl" / "apple" / "ssi263_sc02_rom.mem").read_text(
        encoding="ascii"
    ).splitlines():
        token = line.split("//", 1)[0].strip()
        if token:
            rom.append(int(token, 16))
    if len(rom) != 512:
        raise RuntimeError(f"native ROM has {len(rom)} bytes, expected 512")

    stop_phones = {
        phone
        for phone in range(64)
        if (rom[phone * 8 + 2] & 0x04)
        and not (rom[phone * 8 + 2] & 0x02)
    }
    if stop_phones != {0x24, 0x25, 0x27, 0x28, 0x29}:
        raise RuntimeError(
            "PW2&&!PW3 no longer selects exactly B/D/P/T/K: "
            f"{sorted(stop_phones)}"
        )

    f_f34 = rom[0x34 * 8 + 3] >> 4
    sch_f34 = rom[0x32 * 8 + 3] >> 4
    if f_f34 == sch_f34 or (
        tables["f3_b_q14"][f_f34] == tables["f3_b_q14"][sch_f34]
        and tables["f4_b_q14"][f_f34] == tables["f4_b_q14"][sch_f34]
    ):
        raise RuntimeError("F and SCH collapsed onto one fixed hiss spectrum")

    phase_rate_hz = (14_318_180 / 14) / (2 * (256 - 0xE9))
    fundamental_hz = 90.796

    def denominator(
        frequency_hz: float, alpha: float, a: float, b: float
    ) -> tuple[complex, complex]:
        z1 = cmath.exp(-2j * math.pi * frequency_hz / phase_rate_hz)
        return z1, (
            1 - (1 + alpha - a * b) * z1 + alpha * z1 * z1
        )

    def vowel_metrics(phone: int) -> tuple[float, float, float]:
        row = rom[phone * 8 : phone * 8 + 8]
        f1_code, f2_code, res_code, f34_code = (
            value >> 4 for value in row[:4]
        )
        harmonics = []
        harmonic = 1
        while harmonic * fundamental_hz < phase_rate_hz / 2:
            frequency = harmonic * fundamental_hz

            alpha = constants["F1_ALPHA_Q14"] / q14
            a = constants["F1_A_Q14"] / q14
            g = constants["F1_G_Q14"] / q14
            b = tables["f1_b_q14"][f1_code] / q14
            z1, section_denominator = denominator(frequency, alpha, a, b)
            h1 = b * ((a + g) - g * z1) / section_denominator

            alpha = tables["f2_alpha_q14"][res_code] / q14
            a = tables["f2_a_q14"][res_code] / q14
            b = tables["f2_b_q14"][f2_code] / q14
            _, section_denominator = denominator(frequency, alpha, a, b)
            h2 = a * b / section_denominator

            alpha = constants["F3_ALPHA_Q14"] / q14
            a = constants["F3_A_Q14"] / q14
            g = constants["F3_G_Q14"] / q14
            b = tables["f3_b_q14"][f34_code] / q14
            z1, section_denominator = denominator(frequency, alpha, a, b)
            h3_main = a * b / section_denominator
            h3_side = b * g * (1 - z1) / section_denominator
            y3 = (h3_main * h2 + h3_side) * h1

            alpha = constants["F4_ALPHA_Q14"] / q14
            a = constants["F4_A_Q14"] / q14
            b = tables["f4_b_q14"][f34_code] / q14
            _, section_denominator = denominator(frequency, alpha, a, b)
            h4 = a * b / section_denominator

            alpha = constants["F5_ALPHA_Q14"] / q14
            a = constants["F5_A_Q14"] / q14
            b = constants["F5_B_Q14"] / q14
            _, section_denominator = denominator(frequency, alpha, a, b)
            h5 = a * b / section_denominator

            # U146 sees the old-to-new F5 change through Cselected while
            # C172 retains 2700/2750 of its prior output.  FL_AMP=F is a
            # nonzero nominal gain; its scalar cancels the normalized bands,
            # but the output pole and delta zero must remain in the model.
            output_alpha = constants["FILTER_OUTPUT_ALPHA_Q14"] / q14
            output_gain = filter_amplitude[-1] / q14
            h_output = (
                output_gain
                * (z1 - 1)
                / (1 - output_alpha * z1)
            )

            source = sum(
                cmath.exp(-2j * math.pi * frequency * sample / phase_rate_hz)
                for sample in range(15)
            )
            power = abs(source * y3 * h4 * h5 * h_output) ** 2
            harmonics.append((frequency, power))
            harmonic += 1

        total_power = sum(power for _, power in harmonics)
        if total_power <= 0:
            raise RuntimeError(f"phone {phone:02X} has no harmonic power")
        centroid = sum(
            frequency * power for frequency, power in harmonics
        ) / total_power
        low_fraction = sum(
            power for frequency, power in harmonics if 30 <= frequency <= 500
        ) / total_power
        high_fraction = sum(
            power
            for frequency, power in harmonics
            if 1000 <= frequency <= 4000
        ) / total_power
        return centroid, low_fraction, high_fraction

    # These eight native ROM vowels cover the full tract-code range.  Broad
    # charge-model bounds reject both the prior five-section low-band response
    # and an undamped, nearly lossless F2 without copying any reference audio.
    # E1, A, EH, AE, O, OO, UH, ER in the supplied SSI-263 ROM order.
    vowels = (0x02, 0x08, 0x0A, 0x0C, 0x11, 0x13, 0x18, 0x1C)
    metrics = [vowel_metrics(phone) for phone in vowels]
    mean_centroid = sum(value[0] for value in metrics) / len(metrics)
    mean_low = sum(value[1] for value in metrics) / len(metrics)
    mean_high = sum(value[2] for value in metrics) / len(metrics)
    if not 680 <= mean_centroid <= 825:
        raise RuntimeError(
            f"eight-vowel centroid {mean_centroid:.1f} Hz left charge-model bounds"
        )
    if not 0.080 <= mean_low <= 0.145:
        raise RuntimeError(
            f"eight-vowel low-band fraction {mean_low:.3f} left bounds"
        )
    if not 0.065 <= mean_high <= 0.130:
        raise RuntimeError(
            f"eight-vowel 1-4 kHz fraction {mean_high:.3f} left bounds"
        )


def static_checks() -> None:
    source = (ROOT / "hdl" / "apple" / "ssi263_sc02_audio.sv").read_text(
        encoding="utf-8"
    )
    required = (
        "module ssi263_sc02_audio #(",
        "parameter logic [3:0] NOISE_D1_SEED",
        "input  logic               fricative",
        "input  logic               voiced",
        "input  logic               pw_2",
        "input  logic               pw_3",
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
        "LINE_OUTPUT_SHIFT = 3",
        "stop_class = pw_2 && !pw_3;",
        "source_voiced = stop_class ? 1'b0 : voiced;",
        "source_fricative = stop_class ? 1'b0 : fricative;",
        "u72a_fric_gate = !(source_pw3 && !voice_toggle);",
        "noise_bit = !noise_d4_q[4] && u72a_fric_gate;",
        "voice_source = voice_magnitude;",
        "fric1_source = (fric_drive == 18'sd65536) ?",
        "fric2_source = fric2_source_next;",
        "if (fric_drive != fric_drive_history_q)",
        "fric2_source_state_q <= fric2_source_next;",
        "fric_drive_history_q <= fric_drive;",
        "fric1_source_phi0_q <= fric1_source;",
        "fric2_source_phi0_q <= fric2_source_next;",
        "fric1_sw_phi0_q <= source_fric1_sw;",
        "fric2_sw_phi0_q <= source_fric2_sw;",
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
        "dc_input = powered_down ? 24'sd0 : reconstruction_hold_q;",
        "dc_active_stage1_q <= !powered_down;",
        "dc_delta_q <= {dc_input[23], dc_input} -",
        "dc_sum_q <= {{1{dc_output_q[25]}}, dc_output_q} +",
        "dc_filtered_wide = dc_sum_q - (dc_sum_q >>> 8);",
        "dc_output_q <= sat26_from27(dc_filtered_wide);",
        "audio_sample <= sat16_from27(",
        "dc_filtered_wide >>> LINE_OUTPUT_SHIFT",
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
        "fric1_partial_q",
        "_cos_q14",
        "_radius_q14",
        "pole_a1",
        "pole_r2",
        "pole_b0",
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
        raise RuntimeError("stop phones must not use an invented release state")
    if re.search(
        r"filter_frequency\s*==\s*(?:8'h)?ff", source, re.IGNORECASE
    ):
        raise RuntimeError("FILT=FF is maximum rate, not a silence selector")
    if re.search(r"/\s*(?:3300|3900)\b", source):
        raise RuntimeError("source-derived gain tables must not infer dividers")
    if re.search(r"dc_(?:input|active_stage1_q)[^;]*phone_active", source):
        raise RuntimeError("phone end must not hard-mute the post-U148 output")
    if source.count("engine_operand_a * engine_coefficient_a") != 1 or source.count(
        "engine_operand_b * engine_coefficient_b"
    ) != 1:
        raise RuntimeError("charge scheduler must expose two RTL product lanes")
    if re.search(r"sat24_from48\s*\(\s*engine_product_[ab]", source):
        raise RuntimeError("every DSP product must be registered before saturation")
    spectral_checks(source)


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
