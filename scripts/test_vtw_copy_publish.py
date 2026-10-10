#!/usr/bin/env python3
"""Run copy/publication RTL against shadow, ordered capture and delayed PSRAM.

Verilator is sufficient. The production capture logic uses the existing
functional XPM FIFO storage model; this does not validate board timing or
CPU1/DDR consumer throughput. All stalls in the test remain real handshakes.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/vtw_copy_publish_sim"
COMMON = ["hdl/globals.sv", "hdl/apple/vtw_shadow.sv",
          "hdl/apple/vtw_copy_engine.sv", "hdl/sim/tb_vtw_copy_engine.sv"]
PUBLISH = ["hdl/globals.sv", "hdl/apple/apple_cycle_capture_pkg.sv",
           "hdl/sim/xpm_fifo_sync_model.sv", "hdl/apple/vtw_shadow.sv",
           "hdl/apple/vtw_video_capture_mux.sv", "hdl/apple/vtw_copy_engine.sv",
           "hdl/apple/apple_cycle_capture.sv", "hdl/sim/tb_vtw_copy_engine.sv",
           "hdl/sim/tb_vtw_copy_publish.sv"]


def run(command: list[str], directory: Path, log: str) -> str:
    result = subprocess.run(command, cwd=directory, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (directory / log).write_text(result.stdout)
    if result.returncode:
        raise RuntimeError(f"{directory / log}: exit {result.returncode}\n{result.stdout[-4000:]}")
    return result.stdout


def static_checks() -> None:
    top = (ROOT / "hdl/apple/apple_top.sv").read_text()
    required = [
        "CARD_CTRL_REG_VTW_COPY_PUBLISH = 8'hB6",
        "CARD_CTRL_REG_VTW_COPY_ROWS = 8'hB7",
        ".copy_rows(as_vtw_phasor_wdata[4]), .rows_minus1(vtw_copy_rows_q[7:0])",
        ".source_gap(vtw_copy_rows_q[15:8]), .destination_gap(vtw_copy_rows_q[23:16])",
        "as_client_rdata_q <= 32'h56435031",
        "as_client_rdata_q <= {28'd0, 1'b1, capture_transport_enabled,",
        "shr_capture_active_w, 1'b1}",
        "wire vtw_copy_publish_active = shr_capture_active_w && capture_transport_enabled",
        ".publish(as_vtw_phasor_wdata[3]), .publish_active(vtw_copy_publish_active)",
        ".copy_valid(vtw_copy_publish_valid), .copy_addr(vtw_copy_publish_addr)",
        ".copy_data(vtw_copy_publish_data), .copy_ready(vtw_copy_publish_ready)",
        ".direct_valid(capture_direct_valid)", ".direct_addr(capture_direct_addr)",
        ".direct_data(capture_direct_data)", ".direct_ready(capture_direct_ready)",
        ".video_record_ready(vtw_video_record_ready)",
        ".sh_en(vtw_copy_busy ? vtw_copy_sh_en : vtw_sh_port_en)",
        "if (vtw_copy_release_q && !vtw_copy_busy)",
        "vtw_bus_owned && ab_read.res && vtw_arm_rw_hold_state",
    ]
    for item in required:
        if item not in top:
            raise AssertionError(f"Publication integration contract changed: {item}")
    assert "apple/vtw_video_capture_mux.sv" in (ROOT / "hdl/hdl_sources.txt").read_text().splitlines()
    # Older bitstreams return zero at B6, and retain ordinary COPY/FILL.
    assert "default: as_client_rdata_q <= 32'h00000000" in top


def main() -> None:
    tool = shutil.which(os.environ.get("VERILATOR", "verilator"))
    if not tool:
        raise RuntimeError("Verilator is required (or set VERILATOR)")
    OUT.mkdir(parents=True, exist_ok=True)
    static_checks()
    tracked = set(COMMON + PUBLISH + ["hdl/sim/tb_vtw_copy_rows.sv","hdl/apple/apple_top.sv", "hdl/hdl_sources.txt",
                                     "scripts/test_vtw_copy_publish.py"])
    hashes = {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in sorted(tracked)}
    results = {}
    for name, top, sources, marker in [
        ("legacy", "tb_vtw_copy_engine", COMMON, "VTW COPY ENGINE PASS"),
        ("publish", "tb_vtw_copy_publish", PUBLISH, "VTW COPY PUBLISH PASS"),
        ("rows", "tb_vtw_copy_rows", PUBLISH + ["hdl/sim/tb_vtw_copy_rows.sv"], "VTW COPY ROWS PASS"),
    ]:
        directory = OUT / name
        directory.mkdir(exist_ok=True)
        command = [tool, "--binary", "--timing", "--assert", "-Wno-fatal",
                   "--build-jobs", "2", "--top-module", top,
                   "--Mdir", str(directory / "obj"), "-o", "test_copy",
                   *(str(ROOT / s) for s in sources)]
        (directory / "command.json").write_text(json.dumps(command, indent=2) + "\n")
        run(command, directory, "compile.log")
        output = run([str(directory / "obj/test_copy")], directory, "run.log")
        if marker not in output or " FAIL" in output:
            raise RuntimeError(output)
        results[name] = [line for line in output.splitlines() if " MEASURE " in line or "ROWS COMPARISON" in line or marker in line]
        print(results[name][-1], flush=True)
    for name, digest in hashes.items():
        if hashlib.sha256((ROOT / name).read_bytes()).hexdigest() != digest:
            raise RuntimeError(f"Source changed during simulation: {name}; rerun")
    frame = next(line for line in results["publish"] if "FRAME MEASURE" in line)
    clocks = int(re.search(r"clocks=(\d+)", frame)[1])
    report = {"status": "PASS", "static_checks": "PASS", "cases": results,
              "source_sha256": hashes, "frame_engine_clocks": clocks,
              "frame_engine_ms_at_133333333_hz": clocks / 133333.333,
              "scope": "Production RTL engine, shadow, mux and capture logic; functional XPM FIFO model. "
                       "Frame cost excludes ARM/AXI descriptor submission, cache flush, external PSRAM latency "
                       "and uncalibrated CPU1/DDR capture-consumer stalls. No synthesis or board validation."}
    (OUT / "report.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
