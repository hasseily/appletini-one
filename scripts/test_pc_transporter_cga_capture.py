#!/usr/bin/env python3
"""Run the CGA capture and frame handoff against synthetic FPGA registers."""

import os
from pathlib import Path
import subprocess

from test_onee_input_service import find_native_c_compiler


ROOT = Path(__file__).resolve().parents[1]


def main():
    out = ROOT / "build" / "pc_transporter_cga_capture_host"
    out.mkdir(parents=True, exist_ok=True)
    frontend = ROOT / "ps_sources" / "frontend"
    source = (frontend / "pc_transporter_cga_capture.c").read_text(encoding="utf-8")
    barrier = '__asm__ volatile ("dsb sy" ::: "memory");'
    assert source.count(barrier) == 1
    host_source = out / "capture_host.c"
    host_source.write_text(source.replace(barrier, "host_capture_sync();"), encoding="utf-8")
    (out / "xil_io.h").write_text(
        "#include <stdint.h>\n"
        "uint32_t Xil_In32(uintptr_t address);\n"
        "void host_capture_sync(void);\n",
        encoding="utf-8",
    )
    compiler = find_native_c_compiler()
    if compiler is None:
        raise RuntimeError("A native C compiler is required")
    env = dict(os.environ)
    env["PATH"] = str(compiler.parent) + os.pathsep + env["PATH"]
    fixtures = ROOT / "scripts" / "fixtures"
    exe = out / ("capture_test.exe" if os.name == "nt" else "capture_test")
    subprocess.run(
        [str(compiler), "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
         "-I", str(frontend), "-I", str(ROOT / "third_party" / "xtulator_cpu"),
         "-I", str(out), "-I", str(fixtures / "memory_api_bsp"),
         str(host_source), str(fixtures / "pc_transporter_cga_capture_host.c"),
         "-o", str(exe)],
        cwd=ROOT, env=env, check=True,
    )
    subprocess.run([str(exe)], cwd=ROOT, env=env, check=True)


if __name__ == "__main__":
    main()
