#!/usr/bin/env python3
"""Simulate output-mask arithmetic, exclusions, CDC and frame association."""
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build/video_pixel_mask_test"

def tool(name):
    found = shutil.which(name + ".bat") or shutil.which(name)
    if not found:
        fallback = Path("E:/AMDDesignTools/2025.2/Vivado/bin") / (name + ".bat")
        if fallback.is_file(): found = str(fallback)
    if not found: raise RuntimeError(f"Missing Vivado simulator: {name}")
    return found

def run(args, name):
    result = subprocess.run(args, cwd=BUILD, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    (BUILD / name).write_text(result.stdout, encoding="utf-8")
    if result.returncode: raise RuntimeError(result.stdout)
    return result.stdout

def main():
    BUILD.mkdir(parents=True, exist_ok=True)
    run([tool("xvlog"), "--sv"] + [str(ROOT / x) for x in (
        "hdl/globals.sv", "hdl/reset_sync.sv", "hdl/video2/video_mask_config.sv",
        "hdl/video2/video_pixel_mask.sv", "hdl/sim/tb_video_pixel_mask.sv")], "compile.log")
    glbl = Path(tool("xvlog")).resolve().parents[1] / "data/verilog/src/glbl.v"
    run([tool("xvlog"), str(glbl)], "glbl_compile.log")
    for case, marker in (("tb_video_pixel_mask", "VIDEO PIXEL MASK PASS"),
                         ("tb_video_mask_config", "VIDEO MASK CONFIG PASS")):
        run([tool("xelab"), case, "glbl", "-L", "xpm", "-s", case], case + "_elab.log")
        output = run([tool("xsim"), case, "--runall"], case + "_run.log")
        if marker not in output or "Fatal:" in output or "Error:" in output:
            raise RuntimeError(output)
        print(next(x for x in output.splitlines() if marker in x))

if __name__ == "__main__": main()
