#!/usr/bin/env python3
"""Run the wired Xbox 360 transport and CherryUSB class against a fake HCD."""

import os
import subprocess
from pathlib import Path

from test_onee_input_service import find_native_c_compiler


ROOT = Path(__file__).resolve().parents[1]
FRONTEND = ROOT / "ps_sources" / "frontend"
CHERRYUSB = ROOT / "third_party" / "CherryUSB"
BUILD = ROOT / "build" / "usb_xbox_360_test"


def main() -> int:
    config = (FRONTEND / "usb_config.h").read_text(encoding="utf-8")
    assert "CONFIG_USBHOST_MAX_ENDPOINTS 5" in config
    assert "CONFIG_USBHOST_MAX_XBOX_CLASS 4" in config
    for name in ("create_vitis_workspace.py", "build_multiboot_arm.py"):
        script = (ROOT / "scripts" / name).read_text(encoding="utf-8")
        assert "CherryUSB/class/vendor/xbox" in script, name
        assert "CherryUSB/class/vendor/xbox/usbh_xbox.c" in script, name
    compiler = find_native_c_compiler()
    if compiler is None:
        print("SKIP Xbox 360 runtime tests: no native C compiler")
        return 0
    BUILD.mkdir(parents=True, exist_ok=True)
    executable = BUILD / "usb_xbox_360_harness.exe"
    # CherryUSB's weak fallback callbacks deliberately leave their arguments unused.
    command = [str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror",
               "-Wno-unused-parameter"]
    if "mingw" in compiler.as_posix().lower():
        command.append("-static")
    for directory in (FRONTEND, ROOT / "ps_sources" / "lib",
                      CHERRYUSB / "common", CHERRYUSB / "core",
                      CHERRYUSB / "class" / "hub", CHERRYUSB / "class" / "vendor" / "xbox"):
        command.append(f"-I{directory}")
    command.extend([
        str(ROOT / "scripts" / "fixtures" / "usb_xbox_360_harness.c"),
        str(FRONTEND / "usb_xbox_360.c"),
        str(CHERRYUSB / "class" / "vendor" / "xbox" / "usbh_xbox.c"),
        "-o", str(executable),
    ])
    environment = {key: value for key, value in os.environ.items() if key.lower() != "path"}
    old_path = next((value for key, value in os.environ.items() if key.lower() == "path"), "")
    environment["PATH"] = str(compiler.parent) + os.pathsep + old_path
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
