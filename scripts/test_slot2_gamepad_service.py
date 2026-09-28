#!/usr/bin/env python3
"""Run the slot-2 USB service against a fake atomic FPGA register bank."""

import subprocess
from pathlib import Path

from test_onee_input_service import find_native_c_compiler

ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    compiler = find_native_c_compiler()
    if compiler is None:
        raise RuntimeError("A native C compiler is required")
    build = ROOT / "build" / "slot2_gamepad_service"
    build.mkdir(parents=True, exist_ok=True)
    executable = build / "slot2_gamepad_service.exe"
    command = [str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror"]
    if "mingw" in compiler.as_posix().lower():
        command.append("-static")
    command += [str(ROOT / "scripts/fixtures/slot2_gamepad_service_harness.c"),
                "-o", str(executable)]
    subprocess.run(command, cwd=ROOT, check=True)
    return subprocess.run([str(executable)], cwd=ROOT).returncode


if __name__ == "__main__":
    raise SystemExit(main())
