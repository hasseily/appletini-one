#!/usr/bin/env python3
"""Exercise the production CherryUSB clock with the Zynq BSP's macro form."""

import os
import subprocess
from pathlib import Path

from test_onee_input_service import find_native_c_compiler


ROOT = Path(__file__).resolve().parents[1]
FRONTEND = ROOT / "ps_sources" / "frontend"
BUILD = ROOT / "build" / "cherryusb_time_test"


def main() -> int:
    compiler = find_native_c_compiler()
    if compiler is None:
        print("SKIP CherryUSB time tests: no native C compiler")
        return 0
    BUILD.mkdir(parents=True, exist_ok=True)
    (BUILD / "xiltimer.h").write_text(
        "#ifndef TEST_XILTIMER_H\n#define TEST_XILTIMER_H\n#include <stdint.h>\n"
        "typedef uint64_t XTime;\nvoid XTime_GetTime(XTime *value);\n"
        "#define TEST_CPU_FREQ UINT32_C(666666687)\n"
        "#define TEST_SLEEP_FREQ TEST_CPU_FREQ/2\n"
        "#define COUNTS_PER_SECOND TEST_SLEEP_FREQ\n#endif\n", encoding="utf-8")
    (BUILD / "sleep.h").write_text(
        "#include <stdint.h>\nvoid usleep(uint32_t microseconds);\n", encoding="utf-8")
    executable = BUILD / "cherryusb_time_harness.exe"
    command = [str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror"]
    if "mingw" in compiler.as_posix().lower():
        command.append("-static")
    for directory in (BUILD, FRONTEND, ROOT / "third_party" / "CherryUSB" / "common"):
        command.append(f"-I{directory}")
    command.extend([
        str(ROOT / "scripts" / "fixtures" / "cherryusb_time_harness.c"),
        str(FRONTEND / "cherryusb_baremetal_osal.c"), "-o", str(executable),
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
