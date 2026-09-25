#!/usr/bin/env python3
"""Check counted TURBO guest time against real Disk II controller behavior.

Uses a separate simulation directory so CPU and full-vTW checks can run in
parallel. No board access or firmware build occurs.
"""

from __future__ import annotations

import re
import shutil
import sys
from pathlib import Path

import test_vtw as vtw


BENCHES = [
    ("tb_disk2_time_ready", "DISK2 TIME READY EQUIVALENCE PASS"),
    ("tb_disk2_woz_cell_due", "DISK2 WOZ CELL DUE EQUIVALENCE PASS"),
    ("tb_disk2_vtw_read", "DISK2 VTW READ PASS"),
    ("tb_disk2_physical_bus", "DISK2 PHYSICAL BUS PASS"),
    ("tb_disk2_woz_rw", "DISK2 COUNTED WOZ TIME PASS"),
    ("tb_vtw_disk2_speed_matrix", "VTW DISK2 SPEED MATRIX PASS"),
    ("tb_vtw_disk2_woz_e2e", "VTW DISK2 WOZ E2E PASS"),
]


def write_time_ready_bench() -> Path:
    """Compare the production predicate with the original two-guard logic."""
    source = (vtw.ROOT / "hdl/apple/disk2_card.sv").read_text(encoding="utf-8")
    match = re.search(r"\bassign vtw_time_ready\s*=.*?;", source, re.DOTALL)
    if match is None:
        raise RuntimeError("Disk II time-ready assignment not found")
    path = vtw.OUT_DIR / "tb_disk2_time_ready.sv"
    path.write_text("""`timescale 1ns / 1ps
module tb_disk2_time_ready;
    logic vtw_active, enabled, vtw_native_cycle_active;
    logic vtw_write_timing_active, vtw_drive_spinning_q;
    logic vtw_media_snapshot_valid, vtw_media_wait_q, vtw_drive_loaded_q;
    logic stream_line_hit_q, vtw_stream_pos_match_q, track_woz_q;
    logic woz_weak_refill_pending_q, weak_refill_idle;
    logic vtw_woz_cell_due_valid_q, vtw_woz_cell_due_q, woz_cached_ready_q;
    logic ticks_idle, vtw_cycle_tick, multi_tick;
    struct packed { logic res; } ab_read;
    wire [1:0] woz_weak_refill_stage_q = weak_refill_idle ? 2'd0 : 2'd3;
    wire [4:0] vtw_ticks_pending_q = ticks_idle ? 5'd0 : 5'd31;
    wire [3:0] vtw_tick_count = multi_tick ? 4'd15 : 4'd1;
    wire vtw_time_ready;
""" + match.group(0) + """

    // The old predicate intentionally retains separate media/sequencer
    // bypasses. Compare every combination of the twenty input predicates,
    // including reset, native/write mode, stale snapshots and new tick debt.
    logic old_media_ready, old_sequencer_ready, expected_ready;
    initial begin
        for (int sample = 0; sample < (1 << 20); sample++) begin
            {vtw_active, enabled, ab_read.res, vtw_native_cycle_active,
             vtw_write_timing_active, vtw_drive_spinning_q,
             vtw_media_snapshot_valid, vtw_media_wait_q, vtw_drive_loaded_q,
             stream_line_hit_q, vtw_stream_pos_match_q, track_woz_q,
             woz_weak_refill_pending_q, weak_refill_idle,
             vtw_woz_cell_due_valid_q, vtw_woz_cell_due_q,
             woz_cached_ready_q, ticks_idle, vtw_cycle_tick, multi_tick} = sample;
            #1;
            old_media_ready =
                !vtw_active || !enabled || !ab_read.res ||
                vtw_write_timing_active || !vtw_drive_spinning_q ||
                (vtw_media_snapshot_valid &&
                 (!vtw_media_wait_q || (vtw_drive_loaded_q && stream_line_hit_q)));
            old_sequencer_ready =
                !(vtw_active && !vtw_native_cycle_active && !vtw_write_timing_active) ||
                !enabled || !ab_read.res || !vtw_drive_spinning_q ||
                (vtw_media_snapshot_valid &&
                 (!vtw_media_wait_q ||
                  (vtw_drive_loaded_q && stream_line_hit_q && vtw_stream_pos_match_q &&
                   (!track_woz_q ||
                    (!woz_weak_refill_pending_q && woz_weak_refill_stage_q == 2'd0 &&
                     vtw_woz_cell_due_valid_q &&
                     (!vtw_woz_cell_due_q || woz_cached_ready_q))))));
            expected_ready = !vtw_active || !enabled || !ab_read.res ||
                (old_media_ready && old_sequencer_ready &&
                 vtw_ticks_pending_q == 5'd0 &&
                 !(vtw_cycle_tick && vtw_tick_count > 4'd1));
            if (vtw_time_ready !== expected_ready)
                $fatal(1, "Readiness changed for input predicates %020b", sample);
        end
        $display("DISK2 TIME READY EQUIVALENCE PASS: 1048576 combinations");
        $finish;
    end
endmodule
""", encoding="utf-8")
    return path


def write_woz_cell_due_bench() -> Path:
    """Check the RTL threshold against the original add/compare for all inputs."""
    source = (vtw.ROOT / "hdl/apple/disk2_card.sv").read_text(encoding="utf-8")
    threshold = re.search(
        r"function automatic logic \[7:0\] woz_cell_threshold\b.*?endfunction",
        source, re.DOTALL,
    )
    due = re.search(r"\bwire woz_cell_due\s*=.*?;", source, re.DOTALL)
    if threshold is None or due is None:
        raise RuntimeError("Disk II WOZ due threshold not found")
    path = vtw.OUT_DIR / "tb_disk2_woz_cell_due.sv"
    path.write_text("""`timescale 1ns / 1ps
module tb_disk2_woz_cell_due;
    logic [7:0] track_bit_timing_q;
    logic [15:0] woz_bit_accum_q;
""" + threshold.group(0) + """
    wire [7:0] woz_cell_threshold_q = woz_cell_threshold(track_bit_timing_q);
""" + due.group(0) + """
    integer effective_timing;
    logic expected_due;
    initial begin
        for (int timing = 0; timing < 256; timing++) begin
            track_bit_timing_q = timing;
            effective_timing = timing < 8 ? 32 : timing;
            for (int accum = 0; accum < 65536; accum++) begin
                woz_bit_accum_q = accum;
                expected_due = accum + 8 >= effective_timing;
                #1;
                if (woz_cell_due !== expected_due)
                    $fatal(1, "WOZ due changed: timing=%0d accum=%0d", timing, accum);
            end
        end
        $display("DISK2 WOZ CELL DUE EQUIVALENCE PASS: 16777216 combinations");
        $finish;
    end
endmodule
""", encoding="utf-8")
    return path


def main() -> int:
    vtw.OUT_DIR = vtw.ROOT / "build" / "disk2_turbo_time_sim"
    vtw.OUT_DIR.mkdir(parents=True, exist_ok=True)
    for source in vtw.MEM_FILES:
        shutil.copyfile(vtw.ROOT / source, vtw.OUT_DIR / Path(source).name)
    sources = [source for source in vtw.SOURCES
               if not source.startswith("hdl/sim/") and
               not source.endswith("vtw_shadow_host_port.sv")]
    sources.extend(f"hdl/sim/{bench}.sv" for bench, _ in BENCHES
                   if bench not in ("tb_disk2_time_ready", "tb_disk2_woz_cell_due"))
    try:
        sources.append(str(write_time_ready_bench()))
        sources.append(str(write_woz_cell_due_bench()))
        vtw.run([vtw.vivado_tool("xvlog"), "--sv"] +
                [str(vtw.ROOT / source) for source in sources],
                vtw.OUT_DIR / "xvlog.log")
        for bench, pass_text in BENCHES:
            vtw.run([vtw.vivado_tool("xelab"), bench, "-s", bench + "_snap",
                     "--timescale", "1ns/1ps", "-L", "unisims_ver"],
                    vtw.OUT_DIR / f"xelab_{bench}.log")
            output = vtw.run([vtw.vivado_tool("xsim"), bench + "_snap", "--runall"],
                             vtw.OUT_DIR / f"xsim_{bench}.log")
            if pass_text not in output or "FAIL:" in output or "Fatal:" in output:
                print(output)
                raise RuntimeError(f"{bench} did not pass")
            print(f"PASS {bench}", flush=True)
    except (OSError, RuntimeError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    print(f"{len(BENCHES)} Disk II TURBO time benches passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
