#!/usr/bin/env python3
"""Run glyph uploads through the production ARM service and CPU1 renderer.

Uses the existing Windows MSVC x86 render fixture so physical-address words
remain pointer-sized. No Vitis build or hardware is needed.
"""

import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build" / "video_rom_service_test"


def main():
    if os.name != "nt":
        raise RuntimeError("The production renderer fixture requires Windows MSVC x86")
    BUILD.mkdir(parents=True, exist_ok=True)
    vswhere = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / (
        "Microsoft Visual Studio/Installer/vswhere.exe")
    install = subprocess.check_output([
        str(vswhere), "-latest", "-products", "*", "-requires",
        "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"
    ], text=True).strip()
    if not install:
        raise RuntimeError("MSVC x86 tools are required for the renderer fixture")
    vcvars = Path(install) / "VC/Auxiliary/Build/vcvars32.bat"
    sources = [ROOT / "scripts/host_render_harness/video_rom_service_fixture.c"]
    sources += [ROOT / "ps_sources/frontend" / name for name in (
        "video_rom_service.c", "glyph_api.c", "apple_cycle_renderer.c",
        "appletini_ntsc.c", "appletini_csbits.c", "apple2e_video_rom_data.c")]
    includes = [ROOT / name for name in (
        "scripts/host_render_harness", "scripts/host_render_harness/xil_stub", "ps_sources/frontend")]
    command = f'call "{vcvars}" >nul 2>&1\n'
    command += ('cl /nologo /W3 /O2 /std:c11 /D_CRT_SECURE_NO_WARNINGS '
                '/DHARNESS_VIDEO_ROM_SERVICE /FI harness_prefix.h ')
    command += " ".join(f'/I"{path}"' for path in includes)
    command += f' /Fo"{BUILD}\\\\" /Fe"{BUILD / "video_rom_service.exe"}" '
    command += " ".join(f'"{path}"' for path in sources)
    batch = BUILD / "compile.bat"
    batch.write_text("@echo off\n" + command + "\n", encoding="utf-8")
    result = subprocess.run(["cmd", "/c", str(batch)], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    (BUILD / "compile.log").write_text(result.stdout, encoding="utf-8")
    if result.returncode:
        print(result.stdout)
        result.check_returncode()
    result = subprocess.run([str(BUILD / "video_rom_service.exe")], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    (BUILD / "test.log").write_text(result.stdout, encoding="utf-8")
    print(result.stdout.strip())
    result.check_returncode()


if __name__ == "__main__":
    main()
