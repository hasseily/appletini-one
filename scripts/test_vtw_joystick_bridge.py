#!/usr/bin/env python3
"""Check atomic USB joystick updates and the physical-vTW register boundary."""
from pathlib import Path
import sys

from test_w65c02_core import run, vivado_tool

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build" / "vtw_joystick_bridge"
TOP = "tb_vtw_joystick_bridge"


def main() -> int:
    top = (ROOT / "hdl/apple/apple_top.sv").read_text()
    block = top.split("vtw_joystick_bridge vtw_joystick_bridge_i (", 1)[1].split(");", 1)[0]
    if ".enabled          (vtw_enable_eff && !onee_selected)" not in block:
        raise RuntimeError("joystick bridge must remain limited to physical-host vTW")
    for signal in ("ps_wstrb", "joystick_active", "joystick_buttons", "joystick_paddles"):
        if "." + signal not in block:
            raise RuntimeError(f"missing joystick bridge signal: {signal}")
    sources = (ROOT / "hdl/hdl_sources.txt").read_text().splitlines()
    if "apple/vtw_joystick_bridge.sv" not in sources:
        raise RuntimeError("joystick bridge missing from project sources")
    OUT.mkdir(parents=True, exist_ok=True)
    run([vivado_tool("xvlog"), "--sv",
         str(ROOT / "hdl/apple/vtw_joystick_bridge.sv"),
         str(ROOT / "hdl/sim/tb_vtw_joystick_bridge.sv")], OUT, OUT / "xvlog.log")
    run([vivado_tool("xelab"), TOP, "-s", TOP], OUT, OUT / "xelab.log")
    output = run([vivado_tool("xsim"), TOP, "--runall", "--nolog"], OUT, OUT / "xsim.log")
    passed = [line for line in output.splitlines() if "VTW JOYSTICK BRIDGE PASS" in line]
    if not passed or "Fatal:" in output:
        raise RuntimeError(f"joystick bridge simulation failed; see {OUT / 'xsim.log'}")
    print(passed[-1])
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
