#!/usr/bin/env python3
"""Build and run the integrated 64-phone SSI-263/SC-02 audio sweep."""

from __future__ import annotations

import re
import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "build" / "ssi263_phone_sweep_sim"
PASS_MARKER = "SSI263 PHONE SWEEP PASS"
SELECTED_ROWS = {0x00, 0x01, 0x27, 0x30, 0x3F}


def read_rom() -> list[int]:
    rom = []
    for line in (ROOT / "hdl" / "apple" / "ssi263_sc02_rom.mem").read_text(
        encoding="ascii"
    ).splitlines():
        token = line.split("//", 1)[0].strip()
        if token:
            rom.append(int(token, 16))
    if len(rom) != 512:
        raise RuntimeError(f"native ROM has {len(rom)} bytes, expected 512")
    return rom


def static_checks() -> None:
    read_rom()
    audio_source = (
        ROOT / "hdl" / "apple" / "ssi263_sc02_audio.sv"
    ).read_text(encoding="utf-8")
    core_source = (
        ROOT / "hdl" / "apple" / "ssi263_sc02_core.sv"
    ).read_text(encoding="utf-8")
    invented = (
        "phone_voiced",
        "phone_fricative",
        "stop_class",
        "source_voiced",
        "source_fricative",
    )
    for name in invented:
        if name in audio_source or name in core_source:
            raise RuntimeError(f"native circuit retains invented control {name}")

    testbench = (
        ROOT / "hdl" / "sim" / "tb_ssi263_phone_sweep.sv"
    ).read_text(encoding="utf-8")
    for required in (
        "ssi263_voice dut (",
        "$readmemh(\"ssi263_sc02_rom.mem\", expected_rom);",
        "phone_index < 64",
        "GUIDE_PHONE_COUNT = 10",
        "task automatic observe_guide_phone(",
        "write_register(3'd1, 8'h50);",
        "write_register(3'd2, 8'hA8);",
        "write_register(3'd4, 8'hE9);",
        "write_register(3'd3, 8'h5C);",
        "duration_events == 16",
        "guide phone %02h missed a qualified PW gate",
        "guide HF phone did not open the fricative amplitude",
        "guide EH1 phone did not open the voice amplitude",
        "guide HF phone did not advance U68",
        "guide HF phone did not advance filter amplitude",
        "check_ampzero_targets();",
        "U32 timed PW0 latch update mismatch",
        "U33 timed PW1 latch update mismatch",
        "U11 PW1-gated PW3 load mismatch",
        "U34 timed PW3 latch update mismatch",
        "U83/U84 DDA selected-state mismatch",
        "U83/U84 parameter_write_ce mismatch",
        "WRITE changed nonselected U83/U84 state",
        "parameter_write_ce escaped the U96 write gate",
        "U96 slot 2 did not use the TC window",
        "grounded prototype RATE=F override became active",
        "64-phone sweep did not exercise the voice U96 write",
        "64-phone sweep did not exercise the fricative U96 write",
        "dut.core_i.pw_2_q == expected_rom[row + 2][2]",
        "dut.core_i.pw_5_q == !expected_rom[row + 2][2]",
        "64-phone sweep produced no reconstructed audio activity",
        "64-phone sweep did not carry fricative state into audio",
        "phone %02h reached a 24-bit internal rail",
        "phone %02h produced unknown PCM",
        "SSI263 ROM ROW",
        "SSI263 GUIDE PHONE",
    ):
        if required not in testbench:
            raise RuntimeError(f"integrated phone sweep is missing: {required}")
    forbidden_direct_matches = (
        "dut.core_i.f1_code == expected_rom",
        "dut.core_i.f2_code == expected_rom",
        "dut.core_i.f2_res_code == expected_rom",
        "dut.core_i.f3_code == expected_rom",
        "dut.core_i.f4_code == expected_rom",
        "dut.core_i.voice_amp_code == expected_rom",
        "dut.core_i.fric_amp_code == expected_rom",
        "dut.core_i.pw_0 == expected_rom",
        "dut.core_i.pw_1 == expected_rom",
        "dut.core_i.pw_3 == !expected_rom",
        "controls did not reach its ROM row",
    )
    for forbidden in forbidden_direct_matches:
        if forbidden in testbench:
            raise RuntimeError(
                "integrated sweep still requires a direct ROM output: "
                f"{forbidden}"
            )
    code_only = re.sub(r"/\*.*?\*/", "", testbench, flags=re.DOTALL)
    code_only = re.sub(r"//[^\r\n]*", "", code_only)
    if re.search(r"\b(?:force|release)\b", code_only):
        raise RuntimeError(
            "integrated duration proof must not force internal timing state"
        )
    for name in (*invented, "acoustic", "rms", "occupancy"):
        if name in testbench.lower():
            raise RuntimeError(f"integrated sweep retains acoustic assumption {name}")


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


def selected_lines(output: str) -> list[str]:
    selected = []
    for line in output.splitlines():
        if line.startswith("SSI263 GUIDE PHONE"):
            selected.append(line)
            continue
        if not line.startswith("SSI263 ROM ROW phone="):
            continue
        phone = int(line.split("phone=", 1)[1][:2], 16)
        if (phone in SELECTED_ROWS or
                "positive_rails=0 negative_rails=0" not in line):
            selected.append(line)
    return selected


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
            str(ROOT / "hdl" / "apple" / "ssi263_voice.sv"),
            str(ROOT / "hdl" / "sim" / "tb_ssi263_phone_sweep.sv"),
        ],
        "xvlog.log",
    )
    run(
        [
            vivado_tool("xelab"),
            "tb_ssi263_phone_sweep",
            "-s",
            "tb_ssi263_phone_sweep_snap",
            "--timescale",
            "1ns/1ps",
        ],
        "xelab.log",
    )
    output = run(
        [vivado_tool("xsim"), "tb_ssi263_phone_sweep_snap", "--runall"],
        "xsim.log",
    )
    if PASS_MARKER not in output or "SSI263 PHONE SWEEP FAIL" in output:
        print(output)
        raise RuntimeError("integrated SSI-263 phone sweep did not pass")
    for line in selected_lines(output):
        print(line)
    print(next(line for line in output.splitlines() if PASS_MARKER in line))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
