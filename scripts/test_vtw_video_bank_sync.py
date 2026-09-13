#!/usr/bin/env python3
"""Check deferred TURBO video-bank drains without a firmware build."""

from __future__ import annotations

import sys

import test_vtw as vtw


def main() -> int:
    vtw.OUT_DIR = vtw.ROOT / "build" / "vtw_video_bank_sync_sim"
    vtw.OUT_DIR.mkdir(parents=True, exist_ok=True)
    bench = "tb_vtw_video_bank_sync"
    sources = ["hdl/apple/vtw_video_bank_sync.sv", f"hdl/sim/{bench}.sv"]
    try:
        vtw.run([vtw.vivado_tool("xvlog"), "--sv"] +
                [str(vtw.ROOT / source) for source in sources],
                vtw.OUT_DIR / "xvlog.log")
        vtw.run([vtw.vivado_tool("xelab"), bench, "-s", bench + "_snap",
                 "--timescale", "1ns/1ps"], vtw.OUT_DIR / "xelab.log")
        output = vtw.run([vtw.vivado_tool("xsim"), bench + "_snap", "--runall"],
                         vtw.OUT_DIR / "xsim.log")
        if "VTW VIDEO BANK SYNC PASS" not in output or "Fatal:" in output:
            raise RuntimeError("video bank synchronization bench did not pass")
        print("PASS deferred TURBO video bank synchronization", flush=True)
    except (OSError, RuntimeError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
