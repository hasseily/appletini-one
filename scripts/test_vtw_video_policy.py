#!/usr/bin/env python3
"""Check TURBO active-video ranges against an interval-based XSim model."""

import test_vtw


def main():
    test_vtw.OUT_DIR = test_vtw.ROOT / "build" / "vtw_video_policy_sim"
    test_vtw.OUT_DIR.mkdir(parents=True, exist_ok=True)
    sources = ["hdl/apple/vtw_video_policy.sv",
               "hdl/sim/tb_vtw_video_policy.sv"]
    test_vtw.run([test_vtw.vivado_tool("xvlog"), "--sv",
                  *(str(test_vtw.ROOT / path) for path in sources)],
                 test_vtw.OUT_DIR / "compile.log")
    top = "tb_vtw_video_policy"
    test_vtw.run([test_vtw.vivado_tool("xelab"), top, "-s", top + "_snap"],
                 test_vtw.OUT_DIR / "elaborate.log")
    output = test_vtw.run([test_vtw.vivado_tool("xsim"), top + "_snap", "--runall"],
                          test_vtw.OUT_DIR / "simulation.log")
    test_vtw.require("VTW VIDEO POLICY PASS" in output, output)
    print(next(line for line in output.splitlines()
               if "VTW VIDEO POLICY PASS" in line))


if __name__ == "__main__":
    main()
