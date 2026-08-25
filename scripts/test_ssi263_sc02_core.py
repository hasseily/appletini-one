#!/usr/bin/env python3
"""Build and run the focused native SSI-263 / SC-02 core test."""

from __future__ import annotations

import re
import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "build" / "ssi263_sc02_core_sim"
PASS_MARKER = "SSI263 SC02 CORE PASS"
LEGACY_SPEECH_RE = re.compile(
    r"(?i)(?<![a-z0-9])(?:sc(?:[-_ ]?0?1)a?|votrax)(?![a-z0-9])"
)


def strip_verilog_comments(source: str) -> str:
    source = re.sub(r"/\*.*?\*/", "", source, flags=re.DOTALL)
    return re.sub(r"//[^\r\n]*", "", source)


def static_checks() -> None:
    source = (ROOT / "hdl" / "apple" / "ssi263_sc02_core.sv").read_text(
        encoding="utf-8"
    )
    required = (
        "input  logic        xck_ce",
        "input  logic        div2",
        "input  logic        write_active",
        "output logic        d7_pending",
        "output logic        ar_drive_low",
        "output logic [11:0] pitch_inflection",
        "output logic [7:0]  transitioned_inflection_state",
        "output logic        voice_clock_ce",
        "output logic        voice_toggle",
        "output logic        pitch_period_ce",
        "output logic        noise_clock_ce",
        "output logic        noise_shift_ce",
        "output logic        fric1_sw",
        "output logic        fric2_sw",
        "output logic        closure",
        "output logic        articulation_step_ce",
        "output logic        inflection_step_ce",
        "output logic        parameter_write_ce",
        "assign write_end = write_active_q && !write_active;",
        "assign rom_address = {duration_phoneme_q[5:0], selector_q};",
        "voice_clock_ticks_left_q <= voice_clock_tick_count(",
        "assign closure = filter_phase_ce_q && !filter_phase_q;",
        "assign write_commit = write_end && (pd_rst_n || !REVISION_AP);",
        "assign u104c = pw_3_q && !u62_q;",
        "assign ampct0 = !u104c;",
        "assign ampct_zero = !(ampct_q[1] | ampct_q[2] | ampct_q[3]);",
        "assign ampct_up = ampct0 && ampct_enable;",
        "assign ampct_nco = ampct_up ? (ampct_q != 4'd15) :",
        "(ampct_q != 4'd0);",
        "assign u62_reset = u104c || ampct_nco;",
        "noise_clock_ce_q <= u41c_level && !u41c_level_q;",
        "noise_shift_ce_q <= !u41c_level && u41c_level_q;",
        "if (u68_clock_level && !u68_clock_level_q) begin",
        "ampct_q <= ampct_q + 4'd1;",
        "ampct_q <= ampct_q - 4'd1;",
        "assign u20_clock_enable =",
        "logic [3:0] parameter_resa_q [0:7];",
        "logic [3:0] parameter_resb_q [0:7];",
        "logic [3:0] parameter_resc_q [0:7];",
        "logic [7:0] parameter_rescy_q;",
        "assign transition_a_clr =",
        "assign transition_bc_sum =",
        "u96_write_permit = tc_edge_window_q && u32b_write_gate;",
        "3'd2:\n                u96_write_permit = tc_edge_window_q;",
        "u96_write_permit = duration_edge_window_q &&",
        "3'd5:\n                u96_write_permit = pw_0_q &&",
        "3'd6:\n                u96_write_permit = pw_1_q &&",
        "parameter_resb_q[selector_q] <=",
        "parameter_resc_q[selector_q] <= 4'd8;",
        "parameter_rescy_q[selector_q] <=",
        "transition_bc_sum[3:0];",
        "assign selector_write_level = slow_div_q[1]",
        "assign selector_latch_level = slow_div_q[1]",
        "logic [3:0] u28_q;",
        "assign u28_tc_level = (&u28_q) && rate_clock_div2_q;",
        "assign duration_clock_rise = u28_tc_level && !u28_tc_level_q;",
        "assign u29a_level = !(u28_tc_level || phone_write_active);",
        "u28_q <= {",
        "2'b11,\n                                                duration_phoneme_q[7:6]",
        "u28_q <= u28_q + 4'd1;",
        "logic [3:0] u36_q;",
        "assign response_reset_level = powered_down || u183a_q ||",
        "(!response_phoneme_q && frame_ack_active);",
        "if (phone_write_active || phone_write_release)\n"
        "                u36_q <= 4'd0;",
        "if (response_phoneme_q && u37_q == 4'hF &&",
        "u36_q <= u36_q + 4'd1;",
        "if (!response_phoneme_q &&\n                                                u36_q == 4'hE",
        "assign u38_equal = (u37_q == (selector_flags[0] ? 4'd2 : 4'd6));",
        "assign pw0_set_level = selector_latch_level && selector_q == 3'd0",
        "assign pw1_set_level = selector_latch_level && selector_q == 3'd1",
        "assign pw3_load_level = selector_latch_level && selector_q == 3'd2",
        "u37_q <= u37_q + 4'd1;",
        "else if (phone_write_active)\n                pw_0_q <= 1'b0;",
        "else if (phone_write_active)\n                pw_1_q <= 1'b0;",
        "pw_3_q <= u183a_q || !selector_flags[1];",
        "if (selector_latch_level) begin",
        "3'd4: filter_amp_first_q <= parameter_resa_q[4];",
        "f1_code_q <= f1_first_q;",
        "f2_code_q <= f2_first_q;",
        "filter_amp_code_q <= filter_amp_masked;",
        "if (u20_clock_enable)",
        "u20b_q <= selector_flags[3];",
        "if (!filter_phase_q)",
        "if (filter_phase_q)",
        "fric_amp_code_q <= fric_amp_first_q;\n"
        "                fric1_sw_q <= u20b_q;",
        "fric2_sw_q <= !u20b_q;",
    )
    for text in required:
        if text not in source:
            raise RuntimeError(f"native core contract is missing: {text}")
    if LEGACY_SPEECH_RE.search(strip_verilog_comments(source)):
        raise RuntimeError("native SSI-263 core must not depend on legacy speech")
    if "provisional" in source.lower():
        raise RuntimeError("native SSI-263 core still describes provisional logic")
    for signal in ("phone_fricative", "phone_voiced", "fricative", "voiced"):
        if re.search(rf"\boutput\s+logic[^;]*\b{signal}\b", source):
            raise RuntimeError(f"native core retains invented output {signal}")
    stripped_source = strip_verilog_comments(source)
    if "ampct_q != 4'd9" in stripped_source or re.search(
        r"ampct_q\s*<=\s*\(ampct_q\s*==", stripped_source
    ):
        raise RuntimeError("U68 retains an invented BCD terminal or wrap")
    if "f3_f4_code_q" in stripped_source:
        raise RuntimeError("F3/F4 still share a phase-latch register")
    for stale_timer in (
        "frame_ticks_left_q",
        "frames_left_q",
        "frame_tick_count",
        "boundary_frame_count",
    ):
        if stale_timer in stripped_source:
            raise RuntimeError(
                f"DONE still depends on abstract response timer {stale_timer}"
            )
    if re.search(r"pw_[013]_q\s*<=\s*!?selector_flags", stripped_source):
        raise RuntimeError("timed PW latch still follows a ROM flag directly")
    if "parameter_sweep_q" in stripped_source or re.search(
        r"function\s+automatic\s+logic\s+\[3:0\]\s+move_one_toward\b",
        stripped_source,
    ):
        raise RuntimeError("parameter transition still uses the move-one guess")
    u96_block = re.search(
        r"always_comb\s+begin\s+case\s*\(selector_q\)(.*?)endcase\s+end",
        stripped_source,
        flags=re.DOTALL,
    )
    if not u96_block:
        raise RuntimeError("could not isolate the prototype U96 truth table")
    if not re.search(
        r"3'd2:\s*u96_write_permit\s*=\s*tc_edge_window_q\s*;",
        u96_block.group(1),
    ):
        raise RuntimeError("U96 slot 2 must use U208B's TC window")
    if re.search(
        r"rate_inflection_q\s*\[\s*7\s*:\s*4\s*\]\s*==\s*4'hF",
        u96_block.group(1),
    ):
        raise RuntimeError("grounded prototype P2/R301 override became active")
    if "P2/R301 is grounded on the prototype" not in source:
        raise RuntimeError("core no longer states the prototype X=0 wiring")

    checked_paths = (
        ROOT / "hdl" / "apple" / "ssi263_sc02_core.sv",
        ROOT / "hdl" / "sim" / "tb_ssi263_sc02_core.sv",
        Path(__file__),
    )
    for path in checked_paths:
        data = path.read_bytes()
        if data.endswith(b"\n\n") or data.endswith(b"\r\n\r\n"):
            raise RuntimeError(f"trailing blank line in {path}")


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
            str(ROOT / "hdl" / "sim" / "tb_ssi263_sc02_core.sv"),
        ],
        "xvlog.log",
    )
    run(
        [
            vivado_tool("xelab"),
            "tb_ssi263_sc02_core",
            "-s",
            "tb_ssi263_sc02_core_snap",
            "--timescale",
            "1ns/1ps",
        ],
        "xelab.log",
    )
    output = run(
        [vivado_tool("xsim"), "tb_ssi263_sc02_core_snap", "--runall"],
        "xsim.log",
    )
    if PASS_MARKER not in output or "SSI263 SC02 CORE FAIL" in output:
        print(output)
        raise RuntimeError("native SSI-263 / SC-02 core test did not pass")
    print(next(line for line in output.splitlines() if PASS_MARKER in line))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
