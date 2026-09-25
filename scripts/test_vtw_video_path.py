#!/usr/bin/env python3
"""Run TURBO video mirroring, capture ordering, and egress regressions."""
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build" / "vtw_video_path_sim"


def run(name, args, log):
    tool = shutil.which(name + ".bat") or shutil.which(name)
    if not tool:
        raise RuntimeError(f"missing {name}")
    result = subprocess.run([tool, *args], cwd=OUT, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (OUT / log).write_text(result.stdout, encoding="utf-8")
    if result.returncode:
        raise RuntimeError(result.stdout)
    return result.stdout


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    sources = ["hdl/globals.sv", "hdl/apple/apple_cycle_capture_pkg.sv",
               "hdl/sim/xpm_fifo_sync_model.sv", "hdl/apple/apple_cycle_capture.sv",
               "hdl/apple/apple_cycle_egress.sv", "hdl/apple/vtw_video_coalescer.sv",
               "hdl/sim/vtw_video_coalescer_reference.sv",
               "hdl/sim/tb_vtw_video_coalescer.sv",
               "hdl/sim/tb_apple_cycle_capture.sv", "hdl/sim/tb_apple_cycle_egress.sv"]
    run("xvlog", ["--sv", *(str(ROOT / s) for s in sources)], "compile.log")
    for top, marker in [("tb_vtw_video_coalescer", "VTW VIDEO COALESCER PASS"),
                        ("tb_apple_cycle_capture", "APPLE CYCLE CAPTURE PASS"),
                        ("tb_apple_cycle_egress", "APPLE CYCLE EGRESS RING FLAGS PASS")]:
        run("xelab", [top, "-s", top + "_snap"], top + "_elab.log")
        output = run("xsim", [top + "_snap", "--runall"], top + ".log")
        if marker not in output:
            raise RuntimeError(output)
        print(marker)


if __name__ == "__main__":
    main()
