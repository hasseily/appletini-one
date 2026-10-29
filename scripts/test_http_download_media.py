#!/usr/bin/env python3
"""Run the actual main.c SD handover callbacks against controlled services."""
import os
from pathlib import Path
import re
import subprocess
from test_onee_input_service import find_native_c_compiler

ROOT = Path(__file__).resolve().parents[1]

def function(source, name):
    match = re.search(r"^static (?:void|int) " + name + r"\([^;]*?\)\s*\{", source, re.M)
    if not match:
        raise RuntimeError("Missing callback: " + name)
    start = source.index("{", match.start())
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]

def main():
    compiler = find_native_c_compiler()
    if not compiler:
        raise RuntimeError("Native C compiler required")
    out = ROOT / "build/http-download-media-tests"
    out.mkdir(parents=True, exist_ok=True)
    source = (ROOT / "ps_sources/frontend/main.c").read_text()
    callbacks = ("control_set_usb0_sd_remote_mount", "control_poll_usb0_sd_handover",
                 "control_set_ethernet_ftp_sd_remote", "ui_screenshot_sd_write_complete",
                 "ui_poll_screenshot_media_refresh")
    (out / "main_download_media.inc").write_text("\n\n".join(function(source, n) for n in callbacks))
    env = {k: v for k, v in os.environ.items() if k.lower() != "path"}
    env["PATH"] = str(compiler.parent) + os.pathsep + next(
        (v for k, v in os.environ.items() if k.lower() == "path"), "")
    exe = out / "http_download_media_host.exe"
    subprocess.run([str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I" + str(out), str(ROOT / "scripts/fixtures/http_download_media_host.c"),
                    "-o", str(exe)], cwd=ROOT, env=env, check=True)
    result = subprocess.run([str(exe)], cwd=ROOT, env=env, capture_output=True, text=True)
    (out / "RESULT.log").write_text(result.stdout + result.stderr)
    result.check_returncode()
    print(result.stdout, end="")

if __name__ == "__main__":
    main()
