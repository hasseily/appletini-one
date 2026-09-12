#!/usr/bin/env python3
"""Check counted TURBO guest time against real Disk II controller behavior.

Uses a separate simulation directory so CPU and full-vTW checks can run in
parallel. No board access or firmware build occurs.
"""

from __future__ import annotations

import shutil
import sys
from pathlib import Path

import test_vtw as vtw


BENCHES = [
    ("tb_disk2_vtw_read", "DISK2 VTW READ PASS"),
    ("tb_disk2_physical_bus", "DISK2 PHYSICAL BUS PASS"),
    ("tb_disk2_woz_rw", "DISK2 COUNTED WOZ TIME PASS"),
    ("tb_vtw_disk2_speed_matrix", "VTW DISK2 SPEED MATRIX PASS"),
    ("tb_vtw_disk2_woz_e2e", "VTW DISK2 WOZ E2E PASS"),
]


def main() -> int:
    vtw.OUT_DIR = vtw.ROOT / "build" / "disk2_turbo_time_sim"
    vtw.OUT_DIR.mkdir(parents=True, exist_ok=True)
    for source in vtw.MEM_FILES:
        shutil.copyfile(vtw.ROOT / source, vtw.OUT_DIR / Path(source).name)
    sources = [source for source in vtw.SOURCES
               if not source.startswith("hdl/sim/") and
               not source.endswith("vtw_shadow_host_port.sv")]
    sources.extend(f"hdl/sim/{bench}.sv" for bench, _ in BENCHES)
    try:
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
    print("5 Disk II TURBO time benches passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
