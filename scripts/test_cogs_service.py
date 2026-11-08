#!/usr/bin/env python3
"""Run the CoGS catalog and download coordinator with native HTTP and SD fakes."""

from pathlib import Path
import subprocess

from test_onee_input_service import find_native_c_compiler

ROOT = Path(__file__).resolve().parents[1]
FRONT = ROOT / "ps_sources/frontend"
OUT = ROOT / "build/cogs-service-test"


def main():
    compiler = find_native_c_compiler()
    if not compiler:
        raise RuntimeError("Native C compiler required")
    OUT.mkdir(parents=True, exist_ok=True)
    exe = OUT / "cogs_service_host.exe"
    command = [
        str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-DAPPLETINI_HTTP_SERVICE=1", "-I", str(FRONT),
        str(ROOT / "scripts/fixtures/cogs_service_host.c"),
        *(str(FRONT / name) for name in ("cogs_service.c", "cogs_catalog.c", "net_json.c", "http_core.c")),
        "-o", str(exe),
    ]
    subprocess.run(command, cwd=ROOT, check=True)
    subprocess.run([str(exe)], cwd=OUT, check=True, timeout=20)


if __name__ == "__main__":
    main()
