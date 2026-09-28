#!/usr/bin/env python3
"""Run main's slot-2 callbacks with the real gamepad service and fake registers."""
from pathlib import Path
import subprocess

from test_onee_input_service import find_native_c_compiler
from test_usb_gamepad_input import extract_function


ROOT = Path(__file__).resolve().parents[1]
FRONT = ROOT / "ps_sources/frontend"


def main() -> int:
    source = (FRONT / "main.c").read_text()
    names = ("control_set_slot_enabled", "control_set_slot2_card",
             "control_set_slot2_player_devices")
    functions = "\n\n".join(extract_function(source, name)[1] for name in names)
    harness = (ROOT / "scripts/fixtures/slot2_runtime_control_harness.c").read_text()
    harness = harness.replace("/* PRODUCTION_CALLBACKS */", functions)
    compiler = find_native_c_compiler()
    if compiler is None:
        raise RuntimeError("A native C compiler is required")
    build = ROOT / "build/slot2_runtime_control"
    build.mkdir(parents=True, exist_ok=True)
    c_file = build / "slot2_runtime_control.c"
    c_file.write_text(harness)
    executable = build / "slot2_runtime_control.exe"
    command = [str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror"]
    if "mingw" in compiler.as_posix().lower():
        command.append("-static")
    command += [str(c_file), "-I", str(FRONT), "-o", str(executable)]
    subprocess.run(command, cwd=ROOT, check=True)
    return subprocess.run([str(executable)], cwd=ROOT).returncode


if __name__ == "__main__":
    raise SystemExit(main())
