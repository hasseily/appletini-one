#!/usr/bin/env python3
"""Compile and exercise the portable HTTP service core on recorded edge cases."""
import os
from pathlib import Path
import subprocess
from test_onee_input_service import find_native_c_compiler

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/http-service-tests"


def main():
    compiler = find_native_c_compiler()
    if compiler is None:
        raise RuntimeError("A native C compiler is required for the HTTP parser checks")
    OUT.mkdir(parents=True, exist_ok=True)
    executable = OUT / "http_core_host.exe"
    env = {k:v for k,v in os.environ.items() if k.lower() != "path"}
    env["PATH"] = str(compiler.parent) + os.pathsep + next((v for k,v in os.environ.items() if k.lower() == "path"), "")
    command = [str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror",
               "-I" + str(ROOT / "ps_sources/frontend"),
               str(ROOT / "scripts/fixtures/http_core_host.c"),
               str(ROOT / "ps_sources/frontend/http_core.c"),
               str(ROOT / "ps_sources/frontend/http_seed.c"),
               str(ROOT / "ps_sources/lib/crc32.c"), "-o", str(executable)]
    subprocess.run(command, cwd=ROOT, env=env, check=True)
    subprocess.run([str(executable)], cwd=ROOT, env=env, check=True)


if __name__ == "__main__":
    main()
