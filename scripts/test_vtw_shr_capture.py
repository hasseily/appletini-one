#!/usr/bin/env python3
"""Run real vTW RTL SHR capture-only tests with Verilator (no Vivado needed)."""
from pathlib import Path
import json
import shutil
import subprocess

import test_vtw

ROOT = test_vtw.ROOT
OUT = ROOT / "build" / "vtw_shr_capture_sim"


def run(command, log):
    result = subprocess.run(command, cwd=OUT, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    log.write_text(result.stdout, encoding="utf-8")
    if result.returncode:
        raise RuntimeError(f"RTL test failed; see {log}\n{result.stdout[-5000:]}")
    return result.stdout


def main():
    tool = shutil.which("verilator")
    if not tool:
        raise RuntimeError("verilator is required")
    OUT.mkdir(parents=True, exist_ok=True)
    # Combinational truth-table models for the only Xilinx primitives used
    # by this fixture. The CPU, shadow, mirror and bus engine are production.
    primitives = OUT / "xilinx_luts.sv"
    primitives.write_text(
        "module LUT6 #(parameter [63:0] INIT=0)(input I0,I1,I2,I3,I4,I5,output O);"
        " assign O=INIT[{I5,I4,I3,I2,I1,I0}]; endmodule\n"
        "module LUT2 #(parameter [3:0] INIT=0)(input I0,I1,output O);"
        " assign O=INIT[{I1,I0}]; endmodule\n")
    cases = [
        ("tb_vtw_video_policy", ["hdl/apple/vtw_video_policy.sv",
                                  "hdl/sim/tb_vtw_video_policy.sv"], [],
         "VTW VIDEO POLICY PASS"),
        ("tb_vtw_turbo", [s for s in test_vtw.SOURCES
                           if not s.startswith("hdl/sim/")] +
                          ["hdl/sim/tb_vtw_turbo.sv", str(primitives)],
         ["+define+VTW_SHR_CAPTURE_TEST", f"+incdir+{ROOT / 'hdl/sim'}"],
         "VTW SHR CAPTURE PASS"),
    ]
    for top, sources, options, marker in cases:
        build = OUT / top
        command = [tool, "--binary", "--timing", "--assert", "-Wno-fatal",
                   "--top-module", top, "--Mdir", str(build),
                   "--build-jobs", "2", "-o", top, *options,
                   *(str(ROOT / s) for s in sources)]
        (OUT / f"{top}-command.json").write_text(json.dumps(command, indent=2))
        run(command, OUT / f"{top}-build.log")
        output = run([str(build / top)], OUT / f"{top}-run.log")
        if marker not in output:
            raise RuntimeError(output)
        print(output.strip())


if __name__ == "__main__":
    main()
