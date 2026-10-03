#!/usr/bin/env python3
"""Run PSRAM owned-throughput, native-contention, and handback regressions."""
from pathlib import Path
import sys

import test_psram_driver_iddr_reset as sim


def main() -> int:
    sim.OUT_DIR = sim.ROOT / "build" / "psram_owned"
    sim.OUT_DIR.mkdir(parents=True, exist_ok=True)
    try:
        sources = [
            "hdl/globals.sv", "hdl/apple/vtw_bus_engine.sv",
            "hdl/apple/psram_simple.sv", "hdl/apple/psram_driver.sv",
            "hdl/sim/tb_psram_owned.sv", "hdl/sim/tb_psram_simple.sv",
        ]
        sim.run([sim.vivado_tool("xvlog"), "--sv",
                 *[str(sim.ROOT / path) for path in sources]], "xvlog.log")
        glbl = Path(sim.vivado_tool("xvlog")).parents[1] / "data/verilog/src/glbl.v"
        sim.run([sim.vivado_tool("xvlog"), str(glbl)], "xvlog_glbl.log")
        for bench, marker in [("tb_psram_owned", "PSRAM OWNED PASS"),
                              ("tb_psram_simple", "ALL HANDSHAKES PASS")]:
            sim.run([sim.vivado_tool("xelab"), bench, "glbl", "-s", bench + "_snap",
                     "--timescale", "1ns/1ps", "-L", "unisims_ver"], bench + "_elab.log")
            output = sim.run([sim.vivado_tool("xsim"), bench + "_snap", "--runall"],
                             bench + "_run.log")
            for line in output.splitlines():
                if line.startswith("PASS ") or "FAIL:" in line:
                    print(line)
            if "FAIL:" in output or marker not in output:
                raise RuntimeError(f"{bench} did not pass; see {sim.OUT_DIR}")
            print(f"PASS {bench}")
        return 0
    except (OSError, RuntimeError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
