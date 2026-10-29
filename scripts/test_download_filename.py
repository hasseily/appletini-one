#!/usr/bin/env python3
"""Exercise the production FAT download filename sanitizer."""
import os
from pathlib import Path
import subprocess
from test_onee_input_service import find_native_c_compiler

ROOT = Path(__file__).resolve().parents[1]

def main():
    compiler = find_native_c_compiler()
    if not compiler:
        raise RuntimeError("Native C compiler required")
    out = ROOT / "build/download-filename-tests"
    out.mkdir(parents=True, exist_ok=True)
    env = {k: v for k, v in os.environ.items() if k.lower() != "path"}
    env["PATH"] = str(compiler.parent) + os.pathsep + next(
        (v for k, v in os.environ.items() if k.lower() == "path"), "")
    exe = out / "download_filename_host.exe"
    subprocess.run([str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I" + str(ROOT / "ps_sources/frontend"),
                    str(ROOT / "scripts/fixtures/download_filename_host.c"),
                    str(ROOT / "ps_sources/frontend/download_filename.c"),
                    "-o", str(exe)], cwd=ROOT, env=env, check=True)
    result = subprocess.run([str(exe)], cwd=ROOT, env=env, capture_output=True, text=True)
    (out / "RESULT.log").write_text(result.stdout + result.stderr)
    result.check_returncode()
    print(result.stdout, end="")

if __name__ == "__main__":
    main()
