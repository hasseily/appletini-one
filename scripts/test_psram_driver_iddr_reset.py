#!/usr/bin/env python3
"""Compare PSRAM input-IDDR reset choices in the full read-data path.

The runner makes temporary RTL variants from psram_driver.sv. Production RTL
stays unchanged. Each variant keeps the real tape engine, IDELAYE2 cells, IDDR
cells, phase selector, and read shifter; only the two input-IDDR reset modes
differ.
"""

from __future__ import annotations

import re
import shutil
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "build" / "psram_driver_iddr_reset"
DRIVER = ROOT / "hdl" / "apple" / "psram_driver.sv"
APPLE_TOP = ROOT / "hdl" / "apple" / "apple_top.sv"
BENCH = ROOT / "hdl" / "sim" / "tb_psram_driver_iddr_reset.sv"
SIMPLE_BENCH = ROOT / "hdl" / "sim" / "tb_psram_simple.sv"
XDC = ROOT / "hdl" / "constraints" / "appletini_yarz.xdc"
BUILD_SCRIPT = ROOT / "scripts" / "build_and_export_xsa.tcl"

IDDR_BLOCK = re.compile(
    r"(?ms)^\s{12}IDDR #\(.*?^\s{12}\) u_[ab]_iddr \(.*?^\s{12}\);"
)


def vivado_tool(name: str) -> str:
    bat = shutil.which(f"{name}.bat")
    if bat:
        return bat
    tool = shutil.which(name)
    if tool:
        return tool
    raise FileNotFoundError(f"unable to locate Vivado tool {name}")


def run(cmd: list[str], log_name: str) -> str:
    completed = subprocess.run(
        cmd,
        cwd=OUT_DIR,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    (OUT_DIR / log_name).write_text(completed.stdout, encoding="utf-8")
    if completed.returncode != 0:
        print(completed.stdout)
        raise RuntimeError(
            f"{Path(cmd[0]).name} failed with exit code {completed.returncode}"
        )
    return completed.stdout


def rename_module(source: str, module_name: str) -> str:
    marker = "module psram_driver ("
    if source.count(marker) != 1:
        raise RuntimeError("psram_driver module declaration changed")
    return source.replace(marker, f"module {module_name} (", 1)


def make_variant(
    source: str,
    module_name: str,
    srtype: str | None,
    reset_expr: str | None,
) -> str:
    variant = rename_module(source, module_name)
    if srtype is None or reset_expr is None:
        return variant

    matches = list(IDDR_BLOCK.finditer(variant))
    if len(matches) != 2:
        raise RuntimeError(
            f"expected two PSRAM input IDDR blocks, found {len(matches)}"
        )

    def replace_block(match: re.Match[str]) -> str:
        block = match.group(0)
        block, sr_count = re.subn(
            r'\.SRTYPE\("(?:ASYNC|SYNC)"\)',
            f'.SRTYPE("{srtype}")',
            block,
        )
        block, r_count = re.subn(
            r"\.R\s*\(\s*(?:~resetn|1'b0)\s*\)",
            f".R ({reset_expr})",
            block,
        )
        if sr_count != 1 or r_count != 1:
            raise RuntimeError("input IDDR parameter or reset connection changed")
        return block

    return IDDR_BLOCK.sub(replace_block, variant)


def describe_production_mode(source: str) -> str:
    blocks = IDDR_BLOCK.findall(source)
    if len(blocks) != 2:
        raise RuntimeError("could not inspect the two production input IDDRs")
    modes = []
    for block in blocks:
        srtype = re.search(r'\.SRTYPE\("(ASYNC|SYNC)"\)', block)
        reset = re.search(r"\.R\s*\(\s*([^\)]+?)\s*\)", block)
        if srtype is None or reset is None:
            raise RuntimeError("could not read production input IDDR reset mode")
        modes.append((srtype.group(1), reset.group(1)))
    if modes[0] != modes[1]:
        raise RuntimeError("the A and B input IDDR reset modes differ")
    return f'SRTYPE={modes[0][0]} R={modes[0][1]}'


def static_checks(source: str) -> None:
    blocks = IDDR_BLOCK.findall(source)
    if len(blocks) != 2:
        raise RuntimeError("production must contain two generated input-IDDR blocks")
    for block in blocks:
        if '.SRTYPE("ASYNC")' not in block or ".R (1'b0)" not in block:
            raise RuntimeError(
                "both generated input-IDDR blocks must keep ASYNC mode with R tied low"
            )
        if ".INIT_Q1(1'b0)" not in block or ".INIT_Q2(1'b0)" not in block:
            raise RuntimeError("both input-IDDR phases must keep zero INIT values")

    idelay_blocks = re.findall(
        r"(?ms)^\s{12}IDELAYE2 #\(.*?^\s{12}\) u_[ab]_idelay1 \(.*?^\s{12}\);",
        source,
    )
    if len(idelay_blocks) != 2:
        raise RuntimeError("production must contain two generated input-IDELAY blocks")
    for block in idelay_blocks:
        if ".REGRST     (~resetn)" not in block:
            raise RuntimeError("both generated input-IDELAY blocks must retain REGRST")

    # Each of the two blocks sits in a four-lane generate loop, which yields
    # eight physical IDDR and eight IDELAY cells after elaboration.
    if "for (gi = 0; gi < 4; gi = gi + 1)" not in source:
        raise RuntimeError("PSRAM input PHY must retain four generated A/B lanes")

    for signal in ("psram_oe_launch_q", "psram_a_launch_q",
                   "psram_b_launch_q"):
        declaration = (
            '(* KEEP = "TRUE", DONT_TOUCH = "TRUE" *) logic [3:0] '
            f'{signal};'
        )
        if declaration not in source:
            raise RuntimeError(
                f"PSRAM half-cycle launch register is not preserved: {signal}"
            )
    for assignment in (
        "psram_oe  <= psram_oe_launch_q;",
        "psram_a_o <= psram_a_launch_q;",
        "psram_b_o <= psram_b_launch_q;",
        "sh_oe_tape <= sh_oe_tape_d;",
        "sh_wr_a_tape <= sh_wr_a_tape_d;",
        "sh_wr_b_tape <= sh_wr_b_tape_d;",
    ):
        if assignment not in source:
            raise RuntimeError(
                f"PSRAM post-rising-edge launch contract lost: {assignment}"
            )

    apple_top = APPLE_TOP.read_text(encoding="utf-8")
    reset_wiring = re.search(
        r"psram_driver\s+psram_driver_i\s*\(.*?"
        r"\.resetn\s*\(\s*rstn\[0\]\s*\)",
        apple_top,
        re.DOTALL,
    )
    if reset_wiring is None:
        raise RuntimeError("apple_top must keep the PSRAM engine on rstn[0]")

    xdc = XDC.read_text(encoding="utf-8")
    timing_contract = (
        "*psram_a_launch_q_reg*",
        "*psram_b_launch_q_reg*",
        "*psram_oe_launch_q_reg*",
        "*psram_a_o_reg*",
        "*psram_b_o_reg*",
        "*psram_oe_reg*",
        "set_max_delay -datapath_only 2.75",
        "-from [get_cells -hierarchical",
        "-to [get_cells -hierarchical",
    )
    for text in timing_contract:
        if text not in xdc:
            raise RuntimeError(
                f"PSRAM launch placement timing contract lost: {text}"
            )

    build_script = BUILD_SCRIPT.read_text(encoding="utf-8")
    build_contract = (
        "set psram_launch_cells [get_cells -hierarchical",
        "set psram_output_cells [get_cells -hierarchical",
        "[llength $psram_launch_cells] != 12",
        "[llength $psram_output_cells] != 16",
        "Routed PSRAM launch/output timing cell query no longer matches the PHY",
    )
    for text in build_contract:
        if text not in build_script:
            raise RuntimeError(
                f"PSRAM routed-cell count check lost: {text}"
            )
    if re.search(
        r"wait_on_run\s+impl_1.*?open_run\s+impl_1.*?"
        r"set\s+psram_launch_cells.*?set\s+psram_output_cells.*?"
        r"if\s*\{\[llength\s+\$psram_launch_cells\]",
        build_script,
        re.DOTALL,
    ) is None:
        raise RuntimeError(
            "PSRAM routed-cell count check must run after implementation opens"
        )


def check_production_contract(source: str) -> None:
    if describe_production_mode(source) != "SRTYPE=ASYNC R=1'b0":
        raise RuntimeError(
            "production PSRAM input IDDRs must keep ASYNC type with R tied low"
        )
    for block in IDDR_BLOCK.findall(source):
        if block.count(".INIT_Q1(1'b0)") != 1 or \
                block.count(".INIT_Q2(1'b0)") != 1 or \
                block.count(".R (1'b0)") != 1:
            raise RuntimeError(
                "each PSRAM input IDDR must keep zero INIT and R tied low"
            )
    if source.count(".REGRST     (~resetn)") != 2:
        raise RuntimeError(
            "both PSRAM input IDELAY cells must retain fabric reset"
        )
    required = (
        "sh_rd_en_tape <= 0;",
        "read_shift_q <= 64'd0;",
        "if (sh_rd_en_tape[31]) begin",
        "if (cmd_q == QPI_READ) begin",
    )
    for text in required:
        if text not in source:
            raise RuntimeError(
                f"PSRAM input reset/read safety contract lost: {text}"
            )


def main() -> int:
    try:
        source = DRIVER.read_text(encoding="utf-8")
        static_checks(source)
        check_production_contract(source)
        if OUT_DIR.exists():
            shutil.rmtree(OUT_DIR)
        OUT_DIR.mkdir(parents=True)
        variants = (
            ("psram_driver_production.sv", "psram_driver_production", None, None),
            ("psram_driver_async_reset.sv", "psram_driver_async_reset", "ASYNC", "~resetn"),
            ("psram_driver_sync_reset.sv", "psram_driver_sync_reset", "SYNC", "~resetn"),
            ("psram_driver_no_iddr_reset.sv", "psram_driver_no_iddr_reset", "ASYNC", "1'b0"),
        )
        generated = []
        for filename, module_name, srtype, reset_expr in variants:
            path = OUT_DIR / filename
            path.write_text(
                make_variant(source, module_name, srtype, reset_expr),
                encoding="utf-8",
            )
            generated.append(str(path))

        print(f"Production input IDDR mode: {describe_production_mode(source)}")
        glbl = (
            Path(vivado_tool("xvlog")).parents[1]
            / "data"
            / "verilog"
            / "src"
            / "glbl.v"
        )
        run(
            [vivado_tool("xvlog"), "--sv", *generated, str(BENCH)],
            "xvlog.log",
        )
        run([vivado_tool("xvlog"), str(glbl)], "xvlog_glbl.log")
        run(
            [
                vivado_tool("xelab"),
                "tb_psram_driver_iddr_reset",
                "glbl",
                "-s",
                "tb_psram_driver_iddr_reset_snap",
                "--timescale",
                "1ns/1ps",
                "-L",
                "unisims_ver",
            ],
            "xelab.log",
        )
        output = run(
            [
                vivado_tool("xsim"),
                "tb_psram_driver_iddr_reset_snap",
                "--runall",
                "--log",
                "xsim_native.log",
            ],
            "xsim.log",
        )
        for line in output.splitlines():
            if line.startswith("PASS ") or "FAIL:" in line:
                print(line)
        if "FAIL:" in output:
            raise RuntimeError("PSRAM IDDR reset bench reported a failure")
        if "PSRAM IDDR RESET PASS" not in output:
            raise RuntimeError("PSRAM IDDR reset bench did not report success")
        print("PASS PSRAM input-IDDR reset comparison")

        # Run the existing command/handshake stress bench against the exact
        # production driver too. This top needs globals and psram_simple, but
        # uses the same unisim primitive models and glbl top as the focused
        # comparison above.
        run(
            [
                vivado_tool("xvlog"),
                "--sv",
                str(ROOT / "hdl" / "globals.sv"),
                str(ROOT / "hdl" / "apple" / "psram_simple.sv"),
                str(DRIVER),
                str(SIMPLE_BENCH),
            ],
            "xvlog_psram_simple.log",
        )
        run(
            [
                vivado_tool("xelab"),
                "tb_psram_simple",
                "glbl",
                "-s",
                "tb_psram_simple_snap",
                "--timescale",
                "1ns/1ps",
                "-L",
                "unisims_ver",
            ],
            "xelab_psram_simple.log",
        )
        simple_output = run(
            [
                vivado_tool("xsim"),
                "tb_psram_simple_snap",
                "--runall",
                "--log",
                "xsim_psram_simple_native.log",
            ],
            "xsim_psram_simple.log",
        )
        if "FAIL:" in simple_output:
            raise RuntimeError("existing psram_simple bench reported a failure")
        if "ALL HANDSHAKES PASS" not in simple_output:
            raise RuntimeError("existing psram_simple bench did not report success")
        print("PASS existing psram_simple handshake stress bench")
        return 0
    except (OSError, RuntimeError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
