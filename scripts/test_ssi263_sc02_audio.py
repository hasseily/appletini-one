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


def spectral_checks(source: str) -> None:
    """Reject the old common mid-band resonance with exact RTL poles."""
    cosine = {
        section: parse_case_table(source, f"{section}_cos_q14")
        for section in ("f1", "f2", "f3", "f4")
    }
    radius = {
        "f1": parse_constant(source, "F1_RADIUS_Q14"),
        "f3": parse_constant(source, "F3_RADIUS_Q14"),
        "f4": parse_constant(source, "F4_RADIUS_Q14"),
        "f5": parse_constant(source, "F5_RADIUS_Q14"),
    }
    f2_radius = parse_case_table(source, "f2_radius_q14")
    f5_cosine = parse_constant(source, "F5_COS_Q14")
    filter_amplitude = parse_case_table(source, "filter_amp_gain")

    if filter_amplitude[0] != 0 or filter_amplitude[-1] != 4096:
        raise RuntimeError("filter-amplitude endpoints are not zero and unity")
    if any(
        filter_amplitude[index] <= filter_amplitude[index - 1]
        for index in range(1, 16)
    ):
        raise RuntimeError("filter-amplitude codes are not strictly monotonic")

    def coefficients(cos_q14: int, radius_q14: int) -> tuple[int, int, int]:
        a1 = (cos_q14 * radius_q14 + 4096) // 8192
        r2 = (radius_q14 * radius_q14 + 8192) // 16384
        b0 = 16384 - a1 + r2
        if not (0 < b0 < 32768 and 0 < r2 < 16384):
            raise RuntimeError("all-pole coefficient left its stable Q14 range")
        if a1 * a1 >= 4 * r2 * 16384:
            raise RuntimeError("all-pole section lost its conjugate pole pair")
        return a1, r2, b0

    for section in ("f1", "f3", "f4"):
        for value in cosine[section]:
            coefficients(value, radius[section])
    for value in cosine["f2"]:
        for radius_value in f2_radius:
            coefficients(value, radius_value)
    coefficients(f5_cosine, radius["f5"])

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
        cosine["f3"][f_f34] == cosine["f3"][sch_f34]
        and cosine["f4"][f_f34] == cosine["f4"][sch_f34]
    ):
        raise RuntimeError("F and SCH collapsed onto one fixed hiss spectrum")

    phase_rate_hz = (14_318_180 / 14) / (2 * (256 - 0xE8))

    def section_response(
        frequency_hz: float, cos_q14: int, radius_q14: int
    ) -> complex:
        a1, r2, b0 = coefficients(cos_q14, radius_q14)
        z1 = cmath.exp(-2j * math.pi * frequency_hz / phase_rate_hz)
        return (b0 / 16384) / (
            1 - (a1 / 16384) * z1 + (r2 / 16384) * z1 * z1
        )

    def low_formant_peak(phone: int) -> float:
        row = rom[phone * 8 : phone * 8 + 8]
        f1_code, f2_code, res_code, f34_code = (
            value >> 4 for value in row[:4]
        )
        samples = []
        for frequency in range(80, 1202, 2):
            response = section_response(
                frequency, cosine["f1"][f1_code], radius["f1"]
            )
            response *= section_response(
                frequency, cosine["f2"][f2_code], f2_radius[res_code]
            )
            response *= section_response(
                frequency, cosine["f3"][f34_code], radius["f3"]
            )
            response *= section_response(
                frequency, cosine["f4"][f34_code], radius["f4"]
            )
            response *= section_response(frequency, f5_cosine, radius["f5"])
            samples.append((frequency, abs(response)))
        peaks = [
            samples[index]
            for index in range(1, len(samples) - 1)
            if samples[index][1] > samples[index - 1][1]
            and samples[index][1] >= samples[index + 1][1]
        ]
        if not peaks:
            raise RuntimeError(f"phone {phone:02X} has no low-formant peak")
        return max(peaks, key=lambda item: item[1])[0]

    expected_peaks = {
        0x01: (150, 300),   # E
        0x07: (300, 500),   # I
        0x08: (350, 550),   # A
        0x0A: (500, 700),   # EH
        0x0C: (700, 950),   # AE
        0x0E: (750, 1050),  # AH
        0x11: (400, 600),   # O
        0x18: (450, 700),   # UH
    }
    measured = {phone: low_formant_peak(phone) for phone in expected_peaks}
    for phone, (minimum, maximum) in expected_peaks.items():
        if not minimum <= measured[phone] <= maximum:
            raise RuntimeError(
                f"phone {phone:02X} low formant {measured[phone]:.0f} Hz "
                f"left {minimum}-{maximum} Hz"
            )
    if measured[0x0C] - measured[0x08] < 200:
        raise RuntimeError("A and AE collapsed into a common formant peak")
    if measured[0x0E] - measured[0x01] < 500:
        raise RuntimeError("E and AH collapsed into a common formant peak")


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
        "FRIC1_COUPLING_Q14 = 208",
        "FRIC2_COUPLING_Q14 = 768",
        "LINE_OUTPUT_SHIFT = 2",
        "stop_class = pw_2 && !pw_3;",
        "source_voiced = stop_class ? 1'b0 : voiced;",
        "source_fricative = stop_class ? 1'b0 : fricative;",
        "u72a_fric_gate = !(source_pw3 && !voice_toggle);",
        "noise_bit = !noise_d4_q[4] && u72a_fric_gate;",
        "voice_source = 24'sd0;",
        "voice_source = voice_magnitude;",
        "fric_source_phi0_q <= fric_source;",
        "fric1_partial_q <= sat24_add(",
        "fric2_injection_q <= sat24_add(",
        "fric1_injection_q <= sat24_add(",
        "f3_input_q <= sat24_add(",
        "fric1_sw_phi0_q ?",
        "fric1_injection_q : 24'sd0",
        "f5_input_q <= sat24_add(",
        "fric2_sw_phi0_q ?",
        "fric2_injection_q : 24'sd0",
        "input_mix_stage_q <= 2'd1;",
        "input_mix_stage_q <= 2'd2;",
        "input_mix_stage_q <= 2'd3;",
        "reconstruction_hold_q <= engine_output_next;",
        "dc_delta_q <= {dc_input[23], dc_input} -",
        "dc_sum_q <= {{1{dc_output_q[25]}}, dc_output_q} +",
        "dc_filtered_wide = dc_sum_q - (dc_sum_q >>> 8);",
        "dc_output_q <= sat26_from27(dc_filtered_wide);",
        "audio_sample <= sat16_from27(",
        "dc_filtered_wide >>> LINE_OUTPUT_SHIFT",
        "f1_state_q",
        "f1_history_q",
        "f2_state_q",
        "f2_history_q",
        "f3_state_q",
        "f4_state_q",
        "f5_state_q",
        "engine_busy_q",
        "engine_overrun_q",
        "engine_stage_q",
        "32-cycle scheduler",
        "(product_a_q_ext + 48'sd4096) >>> 13",
        "(product_b_q_ext + 48'sd8192) >>> 14",
        "pole_b0_wide = 18'sd16384 -",
        "recurrence_accumulator = product_a_q_ext - product_b_q_ext;",
        "recurrence_accumulator_q <= recurrence_accumulator;",
        "section_accumulator_q <= recurrence_accumulator_q +",
        "rounded_section_accumulator = section_accumulator_q +",
        "engine_section_next = sat24_from48(",
        "product_a_q <= engine_product_a;",
        "product_b_q <= engine_product_b;",
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
    if re.search(r"fric[12]_(?:state|quadrature|input)_q", source):
        raise RuntimeError(
            "FRIC_1 and FRIC_2 are tract injections, not output resonators"
        )
    if re.search(r"stop_(?:armed|release)", source):
        raise RuntimeError("stop phones must not use an invented release state")
    if re.search(
        r"filter_frequency\s*==\s*(?:8'h)?ff", source, re.IGNORECASE
    ):
        raise RuntimeError("FILT=FF is maximum rate, not a silence selector")
    if re.search(r"/\s*(?:3300|3900|1126)\b", source):
        raise RuntimeError("source-derived gain tables must not infer dividers")
    if source.count("engine_operand_a * engine_coefficient_a") != 1 or source.count(
        "engine_operand_b * engine_coefficient_b"
    ) != 1:
        raise RuntimeError("low-pass scheduler must expose two RTL product lanes")
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
