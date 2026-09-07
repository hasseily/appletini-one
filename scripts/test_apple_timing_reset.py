#!/usr/bin/env python3
"""Check physical PHI0 scanner phase through Apple RES# in XSim.

Extract the timing wiring and reset policy from apple_top so the bench
exercises the production integration, including its bus-strobe source.
The physical bus wrapper and timing counter compile without extraction.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "build" / "apple_timing_reset_sim"


def timing_harness(top: str) -> str:
    signals_start = top.index("    // Timing generator signals\n")
    signals_end = top.index("    localparam logic [7:0] CARD_CTRL_REG_SLOT_ENABLE_MASK", signals_start)
    reset_start = top.index(
        "    always_ff @(posedge clk) begin\n"
        "        if (!rstn[1]) begin\n"
        "            apple_reset_prev_q"
    )
    reset_end = top.index("    /* Physical host standard from the PHI0 line period", reset_start)
    counter_start = top.index("    apple_timing_gen apple_timing_gen_i (")
    counter_end = top.index("    );", counter_start) + len("    );")
    return """module apple_timing_reset_harness (
    input logic clk,
    input logic [3:0] rstn,
    input globals::AppleBus_read ab_read,
    input logic onee_enable_effective,
    input logic onee_activity_quiet,
    input logic onee_video_50hz_active,
    input logic host_50hz,
    input logic boot_command
);
    wire onee_video_vblank;
    wire video_mode_50hz_out;
    wire apple_vblank_start_pulse;
""" + top[signals_start:signals_end] + top[reset_start:reset_end] + top[counter_start:counter_end] + """
    assign bm_vbl_cmd_pulse = boot_command;
    assign video_mode_50hz_detected_q = host_50hz;
    assign video_mode_50hz_valid_q = 1'b1;
endmodule
"""


def run(name: str, args: list[str], log_name: str) -> str:
    tool = shutil.which(f"{name}.bat") or shutil.which(name)
    if tool is None:
        raise RuntimeError(f"Cannot find {name} on PATH")
    result = subprocess.run(
        [tool, *args], cwd=OUT_DIR, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    )
    (OUT_DIR / log_name).write_text(result.stdout, encoding="utf-8")
    if result.returncode:
        raise RuntimeError(result.stdout)
    return result.stdout


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--top-source", type=Path,
                        default=ROOT / "hdl/apple/apple_top.sv",
                        help="Optional older top source for regression checks")
    args = parser.parse_args()
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    harness = OUT_DIR / "apple_timing_reset_harness.sv"
    harness.write_text(timing_harness(args.top_source.read_text(encoding="utf-8")), encoding="utf-8")
    sources = [
        ROOT / "hdl/globals.sv",
        ROOT / "hdl/cdc_bus_sampled.sv",
        ROOT / "hdl/apple/apple_bus_wrapper.sv",
        ROOT / "hdl/apple/onee_mode_safety_guard.sv",
        ROOT / "hdl/apple/apple_timing_gen.sv",
        harness,
        ROOT / "hdl/sim/tb_apple_timing_reset.sv",
    ]
    run("xvlog", ["--sv", *map(str, sources)], "xvlog.log")
    run("xelab", ["tb_apple_timing_reset", "-s", "timing_reset_snap",
                  "--timescale", "1ns/1ps", "-L", "unisims_ver"], "xelab.log")
    output = run("xsim", ["timing_reset_snap", "--runall"], "xsim.log")
    if "APPLE TIMING RESET PASS" not in output or "Fatal:" in output:
        raise RuntimeError(output)
    print("APPLE TIMING RESET PASS (physical NTSC/PAL and ONE//e reset policy)")


if __name__ == "__main__":
    main()
