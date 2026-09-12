#!/usr/bin/env python3
"""Check wide vTW shadow RAM and the host sequencer in an isolated XSim run."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from test_w65c02_core import run, vivado_tool


ROOT = Path(__file__).resolve().parents[1]
TOP = "tb_vtw_shadow_wide"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=ROOT / "build" / "vtw_shadow_wide")
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    try:
        sources = [ROOT / name for name in (
            "hdl/globals.sv",
            "hdl/apple/vtw_shadow.sv",
            "hdl/apple/vtw_shadow_host_port.sv",
            "hdl/sim/tb_vtw_shadow_wide.sv",
        )]
        run([vivado_tool("xvlog"), "--sv", *map(str, sources)], out, out / "xvlog.log")
        run([vivado_tool("xelab"), TOP, "-s", TOP], out, out / "xelab.log")
        output = run([vivado_tool("xsim"), TOP, "--runall", "--nolog"], out, out / "xsim.log")
        passed = [line for line in output.splitlines() if "VTW SHADOW WIDE PASS" in line]
        if not passed or "FAIL" in output or "Fatal:" in output:
            raise RuntimeError(f"wide-shadow bench failed; see {out / 'xsim.log'}")
        print(passed[-1], flush=True)
        return 0
    except (OSError, RuntimeError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
