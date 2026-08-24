#!/usr/bin/env python3
"""Source checks and focused RTL simulation for the linear text overlay."""

from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build" / "linear_text_overlay_sim"


def require(value: bool, message: str) -> None:
    if not value:
        raise RuntimeError(message)


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def tool(name: str) -> str:
    return shutil.which(f"{name}.bat") or shutil.which(name) or ""


def run(command: list[str], log_name: str) -> str:
    completed = subprocess.run(command, cwd=OUT, text=True,
                               stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT)
    (OUT / log_name).write_text(completed.stdout, encoding="utf-8")
    if completed.returncode != 0:
        raise RuntimeError(f"{' '.join(command)} failed; see {log_name}")
    return completed.stdout


def static_checks() -> None:
    card = read("hdl/apple/linear_text_overlay_card.sv")
    capture = read("hdl/apple/apple_cycle_capture.sv")
    top = read("hdl/apple/apple_top.sv")
    renderer = read("ps_sources/frontend/linear_text_overlay.c")
    capture_c = read("ps_sources/frontend/linear_text_overlay_capture.c")
    compositor = read("ps_sources/frontend/compositor.c")
    vitis = read("scripts/create_vitis_workspace.py")
    demo = read("software/textoverlay.a65")
    font = read("ps_sources/frontend/linear_text_overlay_font.c")
    font_header = read("ps_sources/frontend/linear_text_overlay_font.h")
    font_generator = read("scripts/gen_linear_text_overlay_font.py")

    for token in ("CMD_ARM", "CMD_SHOW", "CMD_HIDE", "CMD_OFF",
                  "arm_limit_next <= 17'h0C000", "status_byte",
                  'io_read_data = "L"', "LEGACY_CANVAS_WIDTH  = 16'd1120",
                  "SHR_CANVAS_WIDTH     = 16'd1280",
                  "canvas_shr_active ?",
                  "capture_drop_sticky && !capture_drop_seen_q",
                  "io_write_pending_q <= 1'b0;"):
        require(token in card, f"card lacks {token}")
    require("overlay_command_write" in capture and
            "cap_addr_decode[16] == overlay_capture_bank_aux" in capture,
            "capture must use the ordered command marker and resolved bank")
    require("overlay_capture_drop_internal" in top and
            "overlay_capture_limit" in top and
            ".overlay_canvas_shr_active(shr_capture_active_w)" in top,
            "apple_top must join the overlay and capture paths")
    for token in ("linear_text_overlay_dec_font_8x14",
                  "linear_text_overlay_dec_font_8x16", "FONT_8X16",
                  "LTO_CONFIG_TRANSPARENT", "blink_phase_on",
                  "font_height - 2U", "s_vga_palette[16]",
                  "cell_width = 8U * scale_x",
                  "cell_height = (uint32_t)font_height * scale_y",
                  "gx * scale_x + sx", "canvas_width = COMP_SUBWIN_WIDTH",
                  "canvas_width = COMP_SUBWIN_SHR_WIDTH",
                  "canvas_x >= canvas_width",
                  "canvas_y >= canvas_height"):
        require(token in renderer, f"renderer lacks {token}")
    require("source_row" not in renderer,
            "8x14 must use its own glyph rows, not slice the 8x16 font")
    require("LTO_CTL_SHOW_DRAINED" in capture_c and
            "linear_text_overlay_capture_on_gap" in capture_c and
            "LTO_CTL_MARK_STALE" in capture_c and
            '"dsb sy"' in capture_c,
            "CPU1 must order buffer writes and mark capture gaps stale")
    require("draw_supersprite_overlay(fb);\n        linear_text_overlay_draw(fb,"
            in compositor and "APPLE_FB_DISPLAY_MODE_SHR" in compositor,
            "text overlay must draw above Apple and SuperSprite video")
    require("linear_text_overlay_poll_frame_latch" in compositor and
            "s_frame_ack_pending" in renderer and
            "LTO_POLL_WAIT_LATCH" in renderer and
            "if (overlay_wait_latch)" in compositor,
            "FRAME_PENDING must hold its publish until the changed frame "
            "latches")
    require("linear_text_overlay_capture.c" in vitis and
            "linear_text_overlay_font.c" in vitis,
            "Vitis build must include both CPU overlay halves")
    find_start = demo.index("find_overlay:")
    find_end = demo.index("configure_and_arm:", find_start)
    probe = demo[find_start:find_end]
    require("ldy #$B0" in probe and "lda (SRC),y" in probe and
            "cpx #8" in probe and
            probe.index("lda (SRC),y") < probe.index("sta DEV"),
            "demo must identify the slot ROM before it forms a DEVSEL pointer")
    require("lda (DEV),y" not in probe,
            "demo detection must not read unknown DEVSEL registers")
    for token in ("cp437_font_8x14[256][14]",
                  "dec_font_8x14[32][14]",
                  "cp437_font_8x16[256][16]",
                  "dec_font_8x16[32][16]"):
        require(token in font and token in font_header,
                f"generated font lacks {token}")
    require(font.count("/*") >= 577,
            "generated font must contain both CP437 and DEC font sizes")
    require("ter-u14n.bdf" in font_generator and
            "ter-u16n.bdf" in font_generator and
            "ImageFont" not in font_generator,
            "font generation must use native bitmap sources without scaling")


def simulation() -> None:
    xvlog = tool("xvlog")
    xelab = tool("xelab")
    xsim = tool("xsim")
    require(bool(xvlog and xelab and xsim),
            "Vivado simulation tools are not on PATH")
    OUT.mkdir(parents=True, exist_ok=True)
    run([xvlog, "--sv",
         str(ROOT / "hdl/globals.sv"),
         str(ROOT / "hdl/apple/linear_text_overlay_card.sv"),
         str(ROOT / "hdl/sim/tb_linear_text_overlay.sv")], "xvlog.log")
    run([xelab, "tb_linear_text_overlay", "-s",
         "tb_linear_text_overlay_snap", "--timescale", "1ns/1ps",
         "-L", "unisims_ver"], "xelab.log")
    output = run([xsim, "tb_linear_text_overlay_snap", "--runall"],
                 "xsim.log")
    require("LINEAR TEXT OVERLAY PASS" in output,
            "focused bench did not report success")


def main() -> int:
    try:
        static_checks()
        print("PASS linear text overlay source checks")
        simulation()
        print("PASS linear text overlay RTL simulation")
        return 0
    except (OSError, RuntimeError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
