#!/usr/bin/env python3
"""Run the production TURBO CPU/bus benchmark with direct video enabled."""
import test_vtw


def main():
    test_vtw.OUT_DIR = test_vtw.ROOT / "build" / "vtw_video_integrated_sim"
    test_vtw.OUT_DIR.mkdir(parents=True, exist_ok=True)
    sources = [s for s in test_vtw.SOURCES if not s.startswith("hdl/sim/")]
    sources.append("hdl/sim/tb_vtw_turbo.sv")
    test_vtw.run([test_vtw.vivado_tool("xvlog"), "--sv",
                  *(str(test_vtw.ROOT / s) for s in sources)],
                 test_vtw.OUT_DIR / "compile.log")
    test_vtw.run([test_vtw.vivado_tool("xelab"), "tb_vtw_turbo", "-s", "video_snap",
                  "--timescale", "1ns/1ps", "-L", "unisims_ver"],
                 test_vtw.OUT_DIR / "elaborate.log")
    output = test_vtw.run([test_vtw.vivado_tool("xsim"), "video_snap", "--runall"],
                          test_vtw.OUT_DIR / "simulation.log")
    if "VTW TURBO PASS" not in output:
        raise RuntimeError(output)
    for line in output.splitlines():
        if "VTW TURBO" in line:
            print(line)


if __name__ == "__main__":
    main()
