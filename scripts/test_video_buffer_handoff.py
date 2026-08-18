#!/usr/bin/env python3
"""Stress the renderer/compositor handoff and late DVI-reader restart."""

from __future__ import annotations

import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HANDOFF = ROOT / "ps_sources" / "frontend" / "apple_fb_handoff.c"
RENDERER = ROOT / "ps_sources" / "frontend" / "apple_cycle_renderer.c"
EGRESS = ROOT / "ps_sources" / "frontend" / "apple_cycle_egress.c"
FB_READER = ROOT / "hdl" / "video2" / "fb_reader.sv"
BENCH = ROOT / "hdl" / "sim" / "tb_fb_reader_restart.sv"
BUILD = ROOT / "build" / "video_buffer_handoff_sim"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def merged_orders(left: tuple[str, ...], right: tuple[str, ...]):
    if not left:
        yield right
    elif not right:
        yield left
    else:
        for tail in merged_orders(left[1:], right):
            yield (left[0],) + tail
        for tail in merged_orders(left, right[1:]):
            yield (right[0],) + tail


def pick_third(published: int, active: int) -> int:
    if active >= 3 or published == active:
        return 1 if published == 0 else 0
    return 3 - published - active


def count_unsafe_claims(validated: bool) -> tuple[int, int, int]:
    producer = ("p_pub", "p_seq", "p_pick")
    reader = (
        ("r_seq", "r_pub", "r_active", "r_pub2", "r_seq2", "r_done")
        if validated
        else ("r_seq", "r_pub", "r_active", "r_done")
    )
    unsafe = 0
    returned = 0
    retried = 0
    for order in merged_orders(producer, reader):
        pub, seq, active = 1, 10, 0
        next_writer = None
        seq0 = pub0 = pub2 = seq2 = None
        result = None
        for op in order:
            if op == "p_pub":
                pub = 2
            elif op == "p_seq":
                seq = 11
            elif op == "p_pick":
                next_writer = pick_third(2, active)
            elif op == "r_seq":
                seq0 = seq
            elif op == "r_pub":
                pub0 = pub
            elif op == "r_active":
                active = pub0
            elif op == "r_pub2":
                pub2 = pub
            elif op == "r_seq2":
                seq2 = seq
            elif op == "r_done":
                if validated and (pub2 != pub0 or seq2 != seq0):
                    retried += 1
                else:
                    result = pub0
                    returned += 1
        if result is not None and result == next_writer:
            unsafe += 1
    return unsafe, returned, retried


def static_contract_checks() -> None:
    handoff = HANDOFF.read_text(encoding="utf-8")
    renderer = RENDERER.read_text(encoding="utf-8")
    egress = EGRESS.read_text(encoding="utf-8")
    reader = FB_READER.read_text(encoding="utf-8")

    require("localparam S_DRAIN       = 3'd3;" in reader,
            "fb_reader needs a drain state for accepted old AXI reads")
    require("!axi_read_if.arvalid && outstanding == 4'd0" in reader,
            "fb_reader restart must wait for pending AR and R traffic")
    require("(state == S_BURST) && !vblank_latched" in reader,
            "cancelled-frame R data must not enter the FIFO")
    require("published_after != published || seq_after != seq" in handoff and
            "continue;" in handoff and '"dsb sy"' in handoff,
            "reader claim must reserve, verify, and retry a raced slot")
    require("legacy_shadow_cache_matches" in renderer and
            "g_acr_legacy_frames_skipped++" in renderer and
            "g_acr_legacy_cache_rebuilds++" in renderer,
            "static legacy full-shadow modes need a generation cache")
    require("if (sw_text(s_current_sw) || sw_mixed(s_current_sw))" in renderer,
            "legacy cache must not freeze TEXT or MIXED flash cadence")
    require("s_legacy_cache_border_color == s_vidhd_border_color" in renderer and
            "s_legacy_settle_border_color = s_vidhd_border_color;" in renderer,
            "legacy cache must republish changed frame metadata")
    require("g_legacy_video_shadow_generation++" in egress and
            "video_addr >= 0x0400U" in egress and
            "video_addr <= 0x0BFFU" in egress,
            "legacy generation must cover both text/lores pages")


def run_handoff_interleavings() -> None:
    old_unsafe, old_returned, _ = count_unsafe_claims(False)
    new_unsafe, new_returned, new_retried = count_unsafe_claims(True)
    require(old_returned > 0 and old_unsafe > 0,
            "interleaving model did not reproduce the original slot race")
    require(new_returned > 0 and new_retried > 0,
            "validated model must exercise stable claims and retries")
    require(new_unsafe == 0,
            f"validated handoff still has {new_unsafe} unsafe schedules")
    print(f"PASS handoff interleavings: old unsafe={old_unsafe}, "
          f"validated unsafe={new_unsafe}, retries={new_retried}")


def vivado_tool(name: str) -> str:
    tool = shutil.which(f"{name}.bat") or shutil.which(name)
    if tool is None:
        raise FileNotFoundError(f"unable to locate Vivado tool {name}")
    return tool


def run(command: list[str], log_name: str) -> str:
    done = subprocess.run(
        command,
        cwd=BUILD,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    (BUILD / log_name).write_text(done.stdout, encoding="utf-8")
    if done.returncode != 0:
        print(done.stdout)
        raise RuntimeError(f"{Path(command[0]).name} failed: {done.returncode}")
    return done.stdout


def run_fb_reader_simulation() -> None:
    if BUILD.exists():
        shutil.rmtree(BUILD)
    BUILD.mkdir(parents=True)
    run([
        vivado_tool("xvlog"), "--sv",
        str(ROOT / "hdl" / "globals.sv"),
        str(ROOT / "hdl" / "video2" / "video_pkg.sv"),
        str(FB_READER), str(BENCH),
    ], "xvlog.log")
    run([
        vivado_tool("xelab"), "tb_fb_reader_restart",
        "-s", "tb_fb_reader_restart_snap",
    ], "xelab.log")
    output = run([
        vivado_tool("xsim"), "tb_fb_reader_restart_snap", "--runall",
    ], "xsim.log")
    require("FB READER RESTART PASS" in output,
            "late-frame restart simulation did not report PASS")
    require("FAIL:" not in output,
            "late-frame restart simulation reported a failed check")
    print("PASS fb_reader late-frame AXI drain simulation")


def run_fb_reader_synthesis() -> None:
    synth_tcl = BUILD / "synth_fb_reader.tcl"
    synth_tcl.write_text(
        "read_verilog -sv {" + (ROOT / "hdl" / "globals.sv").as_posix() + "}\n"
        "read_verilog -sv {" +
        (ROOT / "hdl" / "video2" / "video_pkg.sv").as_posix() + "}\n"
        "read_verilog -sv {" + FB_READER.as_posix() + "}\n"
        "synth_design -top fb_reader -part xc7z020clg484-2\n"
        "report_utilization -file fb_reader_utilization.rpt\n"
        "write_checkpoint -force fb_reader_synth.dcp\n"
        "quit\n",
        encoding="utf-8",
    )
    output = run([
        vivado_tool("vivado"), "-mode", "batch", "-nolog", "-nojournal",
        "-source", str(synth_tcl),
    ], "synth.log")
    require("synth_design completed successfully" in output,
            "isolated fb_reader synthesis did not complete")
    print("PASS fb_reader isolated synthesis")


def main() -> int:
    static_contract_checks()
    run_handoff_interleavings()
    run_fb_reader_simulation()
    run_fb_reader_synthesis()
    print("video buffer handoff tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
