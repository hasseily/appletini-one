#!/usr/bin/env python3
"""Exercise the production EHCI completion scanner with emulated DMA descriptors."""

import os
import subprocess
from pathlib import Path

from test_onee_input_service import find_native_c_compiler


ROOT = Path(__file__).resolve().parents[1]
CHERRYUSB = ROOT / "third_party" / "CherryUSB"
BUILD = ROOT / "build" / "usb_ehci_completion_test"


def main() -> int:
    compiler = find_native_c_compiler()
    if compiler is None:
        print("SKIP EHCI completion runtime tests: no native C compiler")
        return 0
    BUILD.mkdir(parents=True, exist_ok=True)
    executable = BUILD / "usb_ehci_completion_harness.exe"
    command = [str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror"]
    if "mingw" in compiler.as_posix().lower():
        command.append("-static")
    for directory in (ROOT / "ps_sources" / "frontend",
                      CHERRYUSB / "common", CHERRYUSB / "core",
                      CHERRYUSB / "class" / "hub", CHERRYUSB / "port" / "ehci"):
        command.append(f"-I{directory}")
    command.extend([
        str(ROOT / "scripts" / "fixtures" / "usb_ehci_completion_harness.c"),
        "-o", str(executable),
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
