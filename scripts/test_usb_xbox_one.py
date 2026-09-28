#!/usr/bin/env python3
"""Run the wired Xbox One driver against real CherryUSB headers and a fake HCD."""

import os
import subprocess
from pathlib import Path

from test_onee_input_service import find_native_c_compiler


ROOT = Path(__file__).resolve().parents[1]
FRONTEND = ROOT / "ps_sources" / "frontend"
CHERRYUSB = ROOT / "third_party" / "CherryUSB"
BUILD = ROOT / "build" / "usb_xbox_one_test"


def main() -> int:
    compiler = find_native_c_compiler()
    if compiler is None:
        print("SKIP Xbox One runtime tests: no native C compiler")
        return 0
    BUILD.mkdir(parents=True, exist_ok=True)
    executable = BUILD / "usb_xbox_one_harness.exe"
    command = [str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror"]
    if "mingw" in compiler.as_posix().lower():
        command.append("-static")
    for directory in (FRONTEND, ROOT / "ps_sources" / "lib",
                      CHERRYUSB / "common", CHERRYUSB / "core",
                      CHERRYUSB / "class" / "hub"):
        command.append(f"-I{directory}")
    command.extend([
        str(ROOT / "scripts" / "fixtures" / "usb_xbox_one_harness.c"),
        str(FRONTEND / "usb_xbox_one.c"), "-o", str(executable),
    ])
    environment = os.environ.copy()
    environment["PATH"] = str(compiler.parent) + os.pathsep + environment.get("PATH", "")
    compiled = subprocess.run(command, cwd=ROOT, text=True, env=environment,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if compiled.returncode:
        print(compiled.stdout)
        return 1
    result = subprocess.run([str(executable)], cwd=ROOT, text=True, env=environment,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    print(result.stdout, end="")
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
