#!/usr/bin/env python3
"""Exercise saved controller identities against the production registry."""

from pathlib import Path
import subprocess

from test_onee_input_service import find_native_c_compiler


ROOT = Path(__file__).resolve().parents[1]
FRONTEND = ROOT / "ps_sources" / "frontend"
BUILD = ROOT / "build" / "usb_controller_identity_test"


def main() -> int:
    compiler = find_native_c_compiler()
    if compiler is None:
        raise RuntimeError("No native C compiler available for controller identity checks")
    BUILD.mkdir(parents=True, exist_ok=True)
    executable = BUILD / "usb_controller_identity.exe"
    command = [str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror",
               str(ROOT / "scripts" / "fixtures" / "usb_controller_identity_harness.c"),
               str(FRONTEND / "usb_controller_identity.c"),
               "-I", str(FRONTEND), "-o", str(executable)]
    if "mingw" in str(compiler).lower():
        command.insert(1, "-static")
    subprocess.run(command, check=True)
    subprocess.run([str(executable)], check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
