#!/usr/bin/env python3
"""Measure every preset using the generated AMD Clocking Wizard/MMCM model.

Run after Vivado generate_target all for zynq_ps_bd.bd. This is independent
of the behavioral AXI mock in test_display_modes_rtl.py.
"""
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build" / "display_clock_wizard"
IP_NAME = "zynq_ps_bd_clk_wiz_0_0"
BD = ROOT / "project/appletini_yarz.gen/sources_1/bd/zynq_ps_bd"
IP = BD / "ip" / IP_NAME


def tool(name: str) -> str:
    found = shutil.which(name + ".bat") or shutil.which(name)
    if not found:
        raise RuntimeError(f"Vivado simulator tool not found: {name}")
    return found


def run(command: list[str], log: str) -> str:
    result = subprocess.run(command, cwd=BUILD, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (BUILD / log).write_text(result.stdout, encoding="utf-8")
    if result.returncode:
        raise RuntimeError(result.stdout)
    return result.stdout


def main() -> None:
    assert (IP / (IP_NAME + ".v")).exists(), "Generate the Clocking Wizard IP first"
    BUILD.mkdir(parents=True, exist_ok=True)
    files = {p.stem.lower(): p for p in IP.rglob("*.vhd")}
    ordered: list[Path] = []
    added: set[str] = set()

    def add(name: str) -> None:
        if name in added:
            return
        added.add(name)
        source = files[name].read_text()
        for dependency in re.findall(r"(?:use|entity)\s+work\.(\w+)", source, re.I):
            if dependency.lower() in files:
                add(dependency.lower())
        ordered.append(files[name])

    for name in files:
        add(name)
    library = "display_clock_vendor"
    run([tool("xvhdl"), "--work", library] + [str(p) for p in ordered], "compile_vhdl.log")
    include = next((BD / "ipshared").rglob("mmcm_pll_drp_func_7s_mmcm.vh")).parent
    vivado = Path(tool("xvlog")).resolve().parent.parent
    sources = [
        ROOT / "hdl/video2/video_pkg.sv", ROOT / "hdl/cdc_bit_sync.sv",
        ROOT / "hdl/video2/video_mode_control.sv",
        ROOT / "hdl/sim/tb_video_clock_wizard.sv",
        vivado / "data/verilog/src/glbl.v",
    ] + [IP / (IP_NAME + suffix) for suffix in
         (".v", "_clk_wiz.v", "_mmcm_pll_drp.v")]
    run([tool("xvlog"), "--sv", "--work", library, "-i", str(include)] + [str(p) for p in sources],
        "compile_verilog.log")
    run([tool("xelab"), library + ".tb_video_clock_wizard", library + ".glbl", "-L", "unisims_ver",
         "-L", "unimacro_ver", "-L", "secureip", "-s", "video_clock_wizard_snap"],
        "elaborate.log")
    output = run([tool("xsim"), "video_clock_wizard_snap", "--runall"], "run.log")
    assert "VIDEO CLOCK WIZARD PASS" in output, output
    for line in output.splitlines():
        if line.startswith("CLOCK ") or "VIDEO CLOCK WIZARD PASS" in line:
            print(line)


if __name__ == "__main__":
    main()
