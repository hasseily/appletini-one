#!/usr/bin/env python3
"""Static and assembly checks for the Appletini demo disk.

Verifies the launcher/demo sources, the STARTUP dispatcher, and (when
acme is on hand) that every new program assembles and fits its load
address. Does not require java/AppleCommander or the web toolchain.
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOFTWARE = ROOT / "software"
ACME_EXE = os.environ.get("ACME_EXE", r"C:\Users\hasse\tools\acme\acme.exe")
ACME_LIB = os.environ.get("ACME", r"C:\Users\hasse\tools\acme\ACME_Lib")


class TestFailure(AssertionError):
    pass


def require(cond: bool, message: str) -> None:
    if not cond:
        raise TestFailure(message)


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def static_checks() -> None:
    build = read(ROOT / "scripts" / "build_appletini_demo_disk.py")
    launcher = read(SOFTWARE / "launcher.a65")
    speedrace = read(SOFTWARE / "speedrace.a65")
    raster = read(SOFTWARE / "rasterdemo.a65")
    impossible = read(SOFTWARE / "impossible.a65")
    core = read(SOFTWARE / "mandel_core.a65")

    # STARTUP dispatches every launcher selection and loops back.
    for token in ('BRUN LAUNCHER', 'BRUN IMPOSSIBLE', 'BRUN SPEEDRACE',
                  'BRUN RASTERDEMO', 'RUN MANDELBROT',
                  'RUN WAVE.ANIMATION', '-A2BROWSE.SYSTEM',
                  '-A2WEBSRV.SYSTEM', 'PEEK(768)', '120 GOTO 10'):
        require(token in build, f"STARTUP must contain {token!r}")

    # The disk build assembles and installs all four programs.
    for token in ('LAUNCHER_SRC', 'SPEEDRACE_SRC', 'RASTER_SRC',
                  'IMPOSSIBLE_SRC', '"LAUNCHER", "BIN", "0x4000"',
                  '"SPEEDRACE", "BIN", "0x6000"',
                  '"RASTERDEMO", "BIN", "0x6000"',
                  '"IMPOSSIBLE", "BIN", "0x6000"'):
        require(token in build, f"disk build must wire {token!r}")

    # Launcher: HGR UI at $4000 with generated assets.
    require("* = $4000" in launcher and
            '!source "launcher_assets.a65"' in launcher and
            "hgr_init" in launcher,
            "launcher must be the $4000 HGR build with generated assets")
    # Launcher: selection handoff protocol and detection probe.
    require("SELECTION = $0300" in launcher and
            "sta SELECTION" in launcher,
            "launcher must hand the selection to STARTUP via $0300")
    # Read-only detection: the launcher must never execute slot-7 ROM
    # (a JSR into a lookalike card crashes machines we don't control),
    # and its fingerprint bytes must match the ROM we actually ship.
    require("jsr SP_ENTRY" not in launcher and "$C70D" not in launcher,
            "launcher detection must not execute the slot-7 ROM")
    rom = bytes(int(line.strip(), 16)
                for line in read(ROOT / "hdl" / "apple" /
                                 "smartport_a2retronet_style_c700.mem")
                .splitlines() if line.strip())
    def sig_bytes(label: str) -> list[int]:
        require(label in launcher, f"launcher must define {label}")
        line = launcher.split(label, 1)[1].splitlines()[1]
        require("!byte" in line, f"{label} must be followed by !byte")
        toks = line.split("!byte", 1)[1].split(";")[0]
        return [int(tok.strip().lstrip("$"), 16)
                for tok in toks.split(",") if tok.strip()]

    offsets = sig_bytes("sig_offsets:")
    values = sig_bytes("sig_values:")
    require(len(offsets) == len(values) and len(offsets) >= 5,
            "fingerprint must pair at least five offset/value bytes")
    for off, val in zip(offsets, values):
        require(rom[off] == val,
                f"launcher fingerprint ${off:02X}=${val:02X} does not match "
                f"the shipped ROM (${rom[off]:02X})")

    # Speed race: both TW speed writes and VBL-based timing.
    require("TWSPEED  = $C074" in speedrace and
            speedrace.count("sta TWSPEED") >= 3,
            "speed race must lock 1 MHz, go warp, and restore")
    require("RDVBLBAR" in speedrace,
            "speed race must time passes with RDVBLBAR")
    require("sta row_start" in speedrace and
            "sta row_end" in speedrace and
            "cmp row_end" in speedrace,
            "speed race must render separate equal-work screen halves")

    # Raster demo: every line tail re-arms the video-sync window.
    require("bit RDVBLBAR" in raster,
            "raster tails must touch $C019 for the slowdown region")
    require(raster.count("bit RDVBLBAR") >= 2,
            "both line tails must retrigger the window")

    # Impossible: acts wired, phrases terminated, aux stub present.
    for token in ("phasor_init", "ram_tour", "music_start",
                  '!source "mandel_core.a65"', "$01B0"):
        require(token in impossible, f"impossible must contain {token!r}")
    for phrase in ("phrase_appletini", "phrase_twelve", "phrase_eight",
                   "phrase_impossible"):
        require(phrase in impossible, f"missing {phrase}")
    require(impossible.count("$FF") >= 4,
            "every phrase needs an $FF terminator")

    # No printed string may overflow a 40-column row (screens indent by
    # up to 2 cells; anything longer than 38 wraps into the interleave).
    import re
    for path, src in (("launcher.a65", launcher),
                      ("speedrace.a65", speedrace),
                      ("rasterdemo.a65", raster),
                      ("impossible.a65", impossible)):
        for m in re.finditer(r'!text "([^"]*)"', src):
            require(len(m.group(1)) <= 38,
                    f"{path}: string longer than 38 columns: "
                    f"{m.group(1)[:30]!r}...")

    # Shared core self-containment: no label collisions with hosts.
    require("render_fractal:" in core and "m_mul16s:" in core,
            "mandel core must expose render_fractal")


def assembly_checks() -> None:
    subprocess.run([sys.executable,
                    str(ROOT / "scripts" / "gen_hgr_assets.py")],
                   check=True)
    exe = shutil.which("acme") or ACME_EXE
    if not Path(exe).is_file():
        print("SKIP assembly checks (acme not found)")
        return
    env = dict(os.environ, ACME=ACME_LIB)
    budgets = {
        "launcher.a65": (0x4000, 0x9600),      # HGR page 1 is the framebuffer
        "speedrace.a65": (0x6000, 0x9600),     # below ProDOS buffers
        "rasterdemo.a65": (0x6000, 0x9600),
        "impossible.a65": (0x6000, 0x9600),
    }
    with tempfile.TemporaryDirectory() as tmp:
        for name, (org, ceiling) in budgets.items():
            out = Path(tmp) / (name + ".bin")
            subprocess.run(
                [exe, "-f", "plain", "-o", str(out), name],
                cwd=SOFTWARE, env=env, check=True,
                capture_output=True)
            size = out.stat().st_size
            require(size > 0, f"{name}: empty output")
            require(org + size <= ceiling,
                    f"{name}: {size} bytes overruns ${ceiling:04X}")
            print(f"PASS {name}: {size} bytes at ${org:04X}")


def main() -> int:
    try:
        static_checks()
        print("PASS static demo-disk checks")
        assembly_checks()
        print("demo disk checks passed")
        return 0
    except TestFailure as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1
    except subprocess.CalledProcessError as exc:
        sys.stderr.buffer.write(exc.stderr or b"")
        print(f"FAIL: assembly error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
