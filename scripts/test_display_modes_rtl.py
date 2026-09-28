#!/usr/bin/env python3
"""Check the PS/PL mode ABI and simulate timing, clock switching, and HP0 drain."""
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build" / "display_modes_rtl"


def run(command: list[str], log: str) -> str:
    result = subprocess.run(command, cwd=BUILD, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (BUILD / log).write_text(result.stdout, encoding="utf-8")
    if result.returncode:
        raise RuntimeError(result.stdout)
    return result.stdout


def tool(name: str) -> str:
    found = shutil.which(name + ".bat") or shutil.which(name)
    if not found:
        raise RuntimeError(f"Vivado simulator tool not found: {name}")
    return found


def main() -> None:
    BUILD.mkdir(parents=True, exist_ok=True)
    package = (ROOT / "hdl/video2/video_pkg.sv").read_text()
    header = (ROOT / "ps_sources/frontend/display_modes.h").read_text()
    assert "VIDEO_DEFAULT_MODE = 4'd4" in package
    assert "VIDEO_MODE_COUNT = 6" in package
    expected = [(1024, 768), (1200, 800), (1280, 1024), (1680, 1050), (1920, 1080), (1360, 768)]
    rows = re.findall(r"(\d+|default): video_mode = '\{(\d+), (\d+),", package)
    pl_modes = {4 if mode == "default" else int(mode): (int(w), int(h))
                for mode, w, h in rows}
    assert pl_modes == dict(enumerate(expected))
    c_rows = re.findall(r'\{\s*(\d+)U,\s*(\d+)U,\s*"(\d+)x(\d+)"', header)
    assert [(int(w), int(h)) for w, h, _, _ in c_rows] == expected
    assert all(w == label_w and h == label_h for w, h, label_w, label_h in c_rows)
    assert re.search(r"DISPLAY_MODE_COUNT\s+6", header)
    assert re.search(r"DISPLAY_MODE_DEFAULT\s+4", header)
    sources = [
        "hdl/globals.sv", "hdl/video2/video_pkg.sv",
        "hdl/video2/video_timing_gen.sv", "hdl/video2/video_mode_control.sv",
        "hdl/video2/fb_reader.sv", "hdl/cdc_bit_sync.sv",
        "hdl/cdc_pulse_toggle.sv", "hdl/reset_sync.sv", "hdl/video2/video_top.sv",
        "hdl/sim/tb_fb_reader_restart.sv", "hdl/sim/tb_fb_reader_modes.sv",
        "hdl/sim/tb_video_output_modes.sv",
    ]
    run([tool("xvlog"), "--sv"] + [str(ROOT / p) for p in sources], "compile.log")
    cases = {
        "tb_video_output_modes": "VIDEO OUTPUT TIMING PASS",
        "tb_video_mode_control": "VIDEO MODE CONTROL PASS",
        "tb_fb_reader_modes": "FB READER MODES PASS",
        "tb_fb_reader_restart": "FB READER RESTART PASS",
    }
    for bench, marker in cases.items():
        snapshot = bench + "_snap"
        run([tool("xelab"), bench, "-s", snapshot], bench + "_elaborate.log")
        output = run([tool("xsim"), snapshot, "--runall"], bench + "_run.log")
        assert marker in output and "FAIL:" not in output, output
        print("PASS:", marker)
    print("PASS: display mode PS/PL catalog and RTL source integration")


if __name__ == "__main__":
    main()
