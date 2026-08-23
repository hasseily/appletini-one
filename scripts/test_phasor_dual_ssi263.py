#!/usr/bin/env python3
"""Build and run the card-level dual SSI-263 Phasor regression."""

from __future__ import annotations

import re
import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "build" / "phasor_dual_ssi263_sim"
PASS_MARKER = "PHASOR DUAL SSI263 PASS"

RTL_SOURCES = (
    ROOT / "hdl" / "globals.sv",
    ROOT / "hdl" / "apple" / "via6522.v",
    ROOT / "hdl" / "apple" / "YM2149.sv",
    ROOT / "hdl" / "apple" / "ssi263_xck_ce.sv",
    ROOT / "hdl" / "apple" / "ssi263_sc02_core.sv",
    ROOT / "hdl" / "apple" / "ssi263_sc02_audio.sv",
    ROOT / "hdl" / "apple" / "ssi263_voice.sv",
    ROOT / "hdl" / "apple" / "mockingboard.sv",
    ROOT / "hdl" / "sim" / "tb_phasor_dual_ssi263.sv",
)


def static_checks() -> None:
    missing = [str(path) for path in RTL_SOURCES if not path.is_file()]
    if missing:
        raise RuntimeError(f"dual SSI-263 simulation sources missing: {missing}")

    bench = RTL_SOURCES[-1].read_text(encoding="utf-8")
    required = (
        "mockingboard dut (",
        "A5+A6 write did not update both independent SSI registers",
        "dual native read did not give cleared A5 priority over A6",
        "Echo+ read selected an SSI data driver",
        "native IRQ did not OR the two pending A/R pins",
        "VIA CA1 edge latches did not observe the SSI requests",
        "Apple RESET lost a selected-write falling-edge collision",
        "held fricative phone did not cause repeated HCC4006 shifts",
        "8 *\n                (4096 - dut.ssi263_secondary_i.core_i.pitch_inflection)",
        "secondary voiced events changed the other SSI socket",
    )
    for text in required:
        if text not in bench:
            raise RuntimeError(f"card-level regression coverage missing: {text}")

    compiled_names = "\n".join(path.name.lower() for path in RTL_SOURCES)
    if re.search(r"(?:sc01|votrax|formant_backend|bus_wrapper)", compiled_names):
        raise RuntimeError("card-level test must compile only native SSI-263 speech RTL")


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
    shutil.copyfile(
        ROOT / "hdl" / "apple" / "ssi263_sc02_rom.mem",
        OUT_DIR / "ssi263_sc02_rom.mem",
    )
    run(
        [vivado_tool("xvlog"), "--sv", *(str(path) for path in RTL_SOURCES)],
        "xvlog.log",
    )
    run(
        [
            vivado_tool("xelab"),
            "tb_phasor_dual_ssi263",
            "-s",
            "tb_phasor_dual_ssi263_snap",
            "--timescale",
            "1ns/1ps",
        ],
        "xelab.log",
    )
    output = run(
        [vivado_tool("xsim"), "tb_phasor_dual_ssi263_snap", "--runall"],
        "xsim.log",
    )
    if PASS_MARKER not in output or "PHASOR DUAL SSI263 FAIL" in output:
        print(output)
        raise RuntimeError("card-level dual SSI-263 Phasor regression did not pass")
    print(next(line for line in output.splitlines() if PASS_MARKER in line))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
