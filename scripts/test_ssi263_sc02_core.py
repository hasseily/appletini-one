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
        "parameter_sweep_q <= 7'h7F;",
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
        "if (u20_clock_enable)",
        "u20b_q <= selector_flags[3];",
        "if (!filter_phase_q)",
        "fric1_sw_q <= u20b_q;",
        "if (filter_phase_q)",
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
