#!/usr/bin/env python3
"""Build and run the schematic-derived SSI-263 / SC-02 audio test."""

from __future__ import annotations

import re
import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "build" / "ssi263_sc02_audio_charge_sim"
PASS_MARKER = "SSI263 SC02 AUDIO PASS"
LEGACY_SPEECH_RE = re.compile(
    r"(?i)(?<![a-z0-9])(?:sc(?:[-_ ]?0?1)a?|votrax)(?![a-z0-9])"
)

CAPACITOR_BANKS = {
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

DIVIDER_DENOMINATORS = (
    2750,
    3300,
    3450,
    3730,
    3900,
    4300,
    4500,
    4700,
    4900,
    6800,
    7000,
    7220,
    7430,
    7650,
    7870,
    8090,
    8300,
    8520,
    8800,
    9020,
    9230,
    9450,
    9670,
    9890,
    10100,
    10320,
    11500,
    11700,
)


def strip_verilog_comments(source: str) -> str:
    source = re.sub(r"/\*.*?\*/", "", source, flags=re.DOTALL)
    return re.sub(r"//[^\r\n]*", "", source)


def require_all(source: str, strings: tuple[str, ...], contract: str) -> None:
    for item in strings:
        if item not in source:
            raise RuntimeError(f"{contract} is missing: {item}")


def compact(source: str) -> str:
    return re.sub(r"\s+", "", strip_verilog_comments(source))


def engine_stage_blocks(source: str) -> dict[int, str]:
    match = re.search(
        r"case\s*\(\s*engine_stage_q\s*\)(.*?)\n\s*endcase",
        source,
        re.DOTALL,
    )
    if not match:
        raise RuntimeError("charge engine stage selector is missing")
    case_body = match.group(1)
    starts = list(re.finditer(r"(?m)^\s*(\d+)\s*:", case_body))
    blocks: dict[int, str] = {}
    for index, stage_match in enumerate(starts):
        end = starts[index + 1].start() if index + 1 < len(starts) else len(case_body)
        blocks[int(stage_match.group(1))] = compact(
            case_body[stage_match.end() : end]
        )
    return blocks


def require_bank(
    stage_source: str,
    mask: str,
    plates: tuple[str, ...],
    weights: list[int],
    name: str,
) -> None:
    for plate in plates:
        if plate not in stage_source:
            raise RuntimeError(f"{name} is missing retained plate {plate}")
    for bit, weight in enumerate(weights):
        pattern = (
            rf"{re.escape(mask)}\[{bit}\]\?[^;]{{0,160}}?"
            rf"-?16'sd{weight}(?=[:;])"
        )
        if not re.search(pattern, stage_source):
            raise RuntimeError(
                f"{name} bit {bit} does not use schematic capacitor {weight} pF"
            )


def circuit_checks(source: str) -> None:
    """Check the net-derived values, not sampled audio or acoustic tuning."""
    stages = engine_stage_blocks(source)
    bank_anchors = {
        "f1": (6, "active_event_q.f1_mask", "f1_plate"),
        "f2": (6, "active_event_q.f2_mask", "f2_plate"),
        "f2_res": (5, "active_event_q.f2_res_mask", "f2_res_plate"),
        "f3": (8, "active_event_q.f3_mask", "f3_plate"),
        "f4": (8, "active_event_q.f4_mask", "f4_plate"),
        "voice": (4, "active_event_q.voice_mask", "voice_plate"),
        "fric1": (4, "active_event_q.fric_mask", "fric1_plate"),
        "fric2": (2, "active_event_q.fric_mask", None),
        "filter_amp": (11, "active_event_q.filter_mask", "filter_plate"),
    }
    for name, weights in CAPACITOR_BANKS.items():
        stage, mask, plate_prefix = bank_anchors[name]
        plates = (
            tuple(f"{plate_prefix}{bit}_q" for bit in range(4))
            if plate_prefix
            else ()
        )
        require_bank(stages[stage], mask, plates, weights, name)

    if "job_old_fric_source_q" not in stages[2]:
        raise RuntimeError("U152 edge does not use the prior physical drive")

    # These are the fixed feedback and cross-coupling capacitors used by the
    # charge equations. Dynamic tests below check their signs and phase use.
    require_all(
        compact(source),
        (
            "functionautomaticlogic[13:0]cap_sum4",
            "numerator_denominator=14'd3300",
            "numerator_denominator=14'd3900",
            "numerator_coefficient0=16'sd3600",
            "numerator_coefficient0=-16'sd3600",
            "numerator_coefficient0=-16'sd5700",
            "numerator_coefficient0=active_event_q.phase?-16'sd9300:-16'sd5700",
            "numerator_input_denominator_q<=numerator_denominator",
            "numerator_input_valid_q<=1",
            "numerator_denominator_q<=numerator_input_denominator_q",
            "products_valid_q<=1",
            "engine_div_numerator_q<=numerator_sum",
            "engine_div_denominator_q<=numerator_denominator_q",
            "numerator_valid_q<=1",
        ),
        "U116/U157/U152/U154 charge equation",
    )

    for lane in range(6):
        require_all(
            compact(source),
            (
                f"numerator_operand{lane}_q<=numerator_operand{lane}",
                f"numerator_coefficient{lane}_q<=numerator_coefficient{lane}",
                f"numerator_product{lane}=numerator_operand{lane}_q*"
                f"numerator_coefficient{lane}_q",
                f"numerator_product{lane}_q<=numerator_product{lane}",
            ),
            "shared charge multiplier",
        )
    synthesizable = re.sub(
        r"(?m)^\s*`[^\r\n]*", "", strip_verilog_comments(source)
    )
    if re.search(r"(?<!/)/(?![/*])", synthesizable):
        raise RuntimeError("production charge engine contains a runtime divide")

    compact_source = compact(source)
    require_all(
        compact_source,
        (
            "reciprocal_product=reciprocal_operand_q*reciprocal_multiplier_q",
            "reciprocal_q0=reciprocal_product_q[60:37]",
            "correction_operand={1'b0,reciprocal_q0}+25'd1",
            "correction_operand_q<=correction_operand",
            "correction_input_denominator_q<=reciprocal_denominator_q",
            "correction_input_q0_q<=reciprocal_q0",
            "correction_input_absolute_q<=reciprocal_absolute_q",
            "correction_input_negative_q<=reciprocal_negative_q",
            "correction_input_zero_q<=reciprocal_zero_q",
            "correction_input_overflow_q<=reciprocal_overflow_q",
            "correction_input_denominator_valid_q<="
            "reciprocal_denominator_valid_q",
            "correction_input_valid_q<=1",
            "correction_product=correction_operand_q*"
            "correction_input_denominator_q",
            "if(correction_product_q<={{2{1'b0}},correction_absolute_q})",
            "engine_step_valid=divider_result_valid_q",
            "divider_invalid_denominator_q",
        ),
        "exact reciprocal divider pipeline",
    )
    for old_state in (
        "divider_busy_q",
        "divider_iteration_q",
        "divider_remainder_q",
        "divider_quotient_q",
    ):
        if re.search(rf"\b{old_state}\b", source):
            raise RuntimeError(f"removed radix divider state returned: {old_state}")
    for denominator in DIVIDER_DENOMINATORS:
        multiplier = (1 << 37) // denominator
        pattern = (
            rf"14'd{denominator}:reciprocal_multiplier="
            rf"26'h0*{multiplier:X};"
        )
        if not re.search(pattern, compact_source, re.IGNORECASE):
            raise RuntimeError(
                f"reciprocal constant is wrong or missing for {denominator}"
            )

    require_all(
        source,
        (
            "voice_plate0_q",
            "voice_plate1_q",
            "voice_plate2_q",
            "voice_plate3_q",
            "fric1_plate0_q",
            "fric1_plate1_q",
            "fric1_plate2_q",
            "fric1_plate3_q",
            "fric2_source_state_q",
            "fric2_shape_state_q",
            "c150_phi1_delta_q",
            "c150_phi1_delta_hold_q",
            "c151_source_plate_q",
            "c151_phi1_delta_q",
            "c151_phi1_delta_hold_q",
            "f1_fixed_plate_q",
            "f1_plate0_q",
            "f1_plate1_q",
            "f1_plate2_q",
            "f1_plate3_q",
            "f2_res_plate0_q",
            "f2_res_plate1_q",
            "f2_res_plate2_q",
            "f2_res_plate3_q",
            "f2_fixed_plate_q",
            "f2_plate0_q",
            "f2_plate1_q",
            "f2_plate2_q",
            "f2_plate3_q",
            "f3_fixed_plate_q",
            "f3_plate0_q",
            "f3_plate1_q",
            "f3_plate2_q",
            "f3_plate3_q",
            "f4_fixed_plate_q",
            "f4_plate0_q",
            "f4_plate1_q",
            "f4_plate2_q",
            "f4_plate3_q",
            "f5_fixed_plate_q",
            "filter_plate0_q",
            "filter_plate1_q",
            "filter_plate2_q",
            "filter_plate3_q",
        ),
        "retained switched-capacitor state",
    )

    # C128 resets in Phi0. At a Phi1 boundary it adds an absolute -2700*x
    # term; a live Phi1 U116 change adds -5400*dx with C205. C127 consumes
    # the resulting same-event F1 change.
    if "f1_input_history_q" in source:
        raise RuntimeError("C128 still uses an invented prior-cycle history")
    require_all(
        compact(source),
        (
            "numerator_coefficient1=16'sd2700",
            "numerator_coefficient2=-16'sd2700",
            "numerator_coefficient0=-16'sd5400",
            "numerator_coefficient0=-16'sd2000",
            "$signed({f1_state_q[23],f1_state_q})-"
            "$signed({f1_old_for_c127_q[23],f1_old_for_c127_q})",
        ),
        "C128/C127 same-event charge path",
    )

    # These tables hid switch-plate memory and must never return. A selected
    # code is a set of switches, not one acoustic gain value.
    forbidden_tables = (
        "voice_gain",
        "fric1_source_magnitude",
        "fric2_edge_magnitude",
        "f1_b_q14",
        "f2_b_q14",
        "f2_alpha_q14",
        "f2_a_q14",
        "f3_b_q14",
        "f4_b_q14",
        "fric1_gain",
        "fric2_gain",
        "filter_amp_gain",
    )
    for name in forbidden_tables:
        if re.search(rf"function automatic[^;]+\b{re.escape(name)}\b", source):
            raise RuntimeError(f"aggregate source table returned: {name}")

    forbidden_state = (
        "fric2_source_phi0_q",
        "fric2_base_history_q",
        "fric2_sw_history_q",
        "fric2_sw_phi0_q",
    )
    for name in forbidden_state:
        if re.search(rf"\b{re.escape(name)}\b", source):
            raise RuntimeError(
                f"C150/C151 still use an absolute per-cycle source: {name}"
            )


def static_checks() -> None:
    source = (ROOT / "hdl" / "apple" / "ssi263_sc02_audio.sv").read_text(
        encoding="utf-8"
    )
    uncommented = strip_verilog_comments(source)

    require_all(
        source,
        (
            "module ssi263_sc02_audio #(",
            "parameter logic signed [17:0] VOICE_TRIM_U116_STEP_Q16 = 18'sd2048",
            "localparam logic signed [17:0] FRIC_DRIVE_MAG_Q16 = 18'sd301;",
            "parameter logic [3:0] NOISE_D1_SEED",
            "u60_parallel_value=4'b1011;",
            "u75_parallel_value=4'b0001;",
            "noise_d1_q<={noise_d1_q[2:0],noise_d3_q[3]};",
            "noise_d2_q<={noise_d2_q[3:0],noise_d4_q[4]};",
            "noise_d3_q<={noise_d3_q[2:0],noise_d2_q[4]};",
            "noise_d4_q<={noise_d4_q[3:0],noise_feedback};",
            "voice_count_after_phi1=u60_parallel_value;",
            "noise_count_next=(noise_count_q==4'hf)?",
            "noise_bit=!(noise_d3_q[3]|(pw_3&&!voice_toggle))&&",
            "fric_drive=noise_bit?FRIC_DRIVE_MAG_Q16:-FRIC_DRIVE_MAG_Q16;",
            "engine_overrun_q",
            "reconstruction_hold_q",
            "if(audio_tick)audio_sample<=sat16_from27(",
        ),
        "native SSI-263 audio contract",
    )

    for signal in (
        "pd_rst_n",
        "noise_clock_ce",
        "noise_shift_ce",
        "fric1_sw",
        "fric2_sw",
        "filter_phase_ce",
        "filter_phase",
    ):
        if not re.search(rf"\binput\s+logic[^,;]*\b{signal}\b", source):
            raise RuntimeError(f"native SSI-263 audio input is missing: {signal}")

    if LEGACY_SPEECH_RE.search(uncommented):
        raise RuntimeError("native SSI-263 audio depends on legacy speech")
    if re.search(r"stop_(?:armed|release)", uncommented):
        raise RuntimeError("the circuit contains an invented stop state")
    if re.search(r"voice_count_q\s*<=\s*4'h0", uncommented):
        raise RuntimeError("U60 must not load or clear to zero")
    if "U60_OPEN_P3_LEVEL" in source or "U75_OPEN_P1_LEVEL" in source:
        raise RuntimeError("schematic-tied counter presets remain configurable")
    if re.search(
        r"filter_frequency\s*==\s*(?:8'h)?ff", uncommented, re.IGNORECASE
    ):
        raise RuntimeError("FILT=FF is maximum rate, not a mute selector")
    if re.search(r"\binput\s+logic[^;]*\bpowered_down\b", uncommented):
        raise RuntimeError("the schematic audio path must not hard-mute on CTL")
    for signal in ("phone_active", "fricative", "voiced", "pw_2"):
        if re.search(rf"\binput\s+logic[^;]*\b{signal}\b", uncommented):
            raise RuntimeError(f"invented source input remains: {signal}")

    circuit_checks(source)


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
        [
            vivado_tool("xsim"),
            "tb_ssi263_sc02_audio_snap",
            "--maxdeltaid",
            "100",
            "--runall",
        ],
        "xsim.log",
    )
    if PASS_MARKER not in output or "SSI263 SC02 AUDIO FAIL" in output:
        print(output)
        raise RuntimeError("schematic SSI-263 / SC-02 audio test did not pass")
    print(next(line for line in output.splitlines() if PASS_MARKER in line))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
