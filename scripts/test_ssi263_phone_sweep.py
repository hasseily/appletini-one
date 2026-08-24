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
REPRESENTATIVE_PHONES = {0x01, 0x24, 0x25, 0x27, 0x28, 0x29, 0x2C, 0x30, 0x32, 0x34}


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
    rom = read_rom()
    stops = {
        phone
        for phone in range(64)
        if rom[phone * 8 + 2] & 0x04 and not rom[phone * 8 + 2] & 0x02
    }
    expected_stops = {0x24, 0x25, 0x27, 0x28, 0x29}
    if stops != expected_stops:
        raise RuntimeError(
            f"ROM PW2/PW3 stop set is {sorted(stops)}, expected "
            f"{sorted(expected_stops)}"
        )

    audio_source = (
        ROOT / "hdl" / "apple" / "ssi263_sc02_audio.sv"
    ).read_text(encoding="utf-8")
    if re.search(r"stop_(?:armed|release)", audio_source):
        raise RuntimeError("stop phones must not use an invented burst state")
    for required in (
        "stop_class = pw_2 && !pw_3;",
        "source_voiced = stop_class ? 1'b0 : voiced;",
        "source_fricative = stop_class ? 1'b0 : fricative;",
    ):
        if required not in audio_source:
            raise RuntimeError(f"native stop-source contract is missing: {required}")

    testbench = (
        ROOT / "hdl" / "sim" / "tb_ssi263_phone_sweep.sv"
    ).read_text(encoding="utf-8")
    for required in (
        "ssi263_voice dut (",
        "$readmemh(\"ssi263_sc02_rom.mem\", expected_rom);",
        "phone_index < 64",
        "stop_mask == 64'h000003B000000000",
        "test_held_stop_and_following_phone();",
    ):
        if required not in testbench:
            raise RuntimeError(f"integrated phone sweep is missing: {required}")


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


def representative_lines(output: str) -> list[str]:
    selected = []
    pattern = re.compile(r"SSI263 PHONE phone=([0-9a-fA-F]{2})\b")
    for line in output.splitlines():
        match = pattern.search(line)
        if match and int(match.group(1), 16) in REPRESENTATIVE_PHONES:
            selected.append(line)
        elif line.startswith(("SSI263 STOP ", "SSI263 REPRESENTATIVE")):
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
    for line in representative_lines(output):
        print(line)
    print(next(line for line in output.splitlines() if PASS_MARKER in line))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
