#!/usr/bin/env python3
"""Check ARM service C syntax, then run its backend with fake MMIO and DMA.

Minimal BSP declarations make these native checks reproducible. They do not
replace the supported Vitis firmware build or validate real FPGA timing.
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("gcc")
    if compiler is None:
        raise RuntimeError("A native C compiler is required")
    includes = ["-I", str(ROOT / "scripts/fixtures/memory_api_bsp")]
    flags = ["-std=c11", "-Wall", "-Wextra", "-Werror"]
    sources = [ROOT / "ps_sources/frontend/memory_api.c",
               ROOT / "ps_sources/frontend/memory_api_hw.c",
               ROOT / "ps_sources/lib/psdma.c"]
    subprocess.run([compiler, *flags, "-fsyntax-only", *includes,
                    *map(str, sources)], check=True)
    # Existing SmartPort code contains Cortex-A9 CPSR assembly. Native
    # syntax checking does not assemble it; suppress Clang's host-width
    # warning for that existing assembly, retaining all other diagnostics.
    compiler_version = subprocess.check_output([compiler, "--version"], text=True)
    asm_flags = ["-Wno-asm-operand-widths"] if "clang" in compiler_version.lower() else []
    subprocess.run([compiler, *flags, "-Wno-unused-function", *asm_flags,
                    "-fsyntax-only", *includes,
                    str(ROOT / "ps_sources/frontend/smartport_service.c")], check=True)
    with tempfile.TemporaryDirectory(prefix="appletini-memory-hw-") as temporary:
        executable = Path(temporary) / "memory_api_hw_host"
        command = [compiler, *flags, "-O1", *includes,
                   str(ROOT / "scripts/fixtures/memory_api_hw_host.c"),
                   str(ROOT / "ps_sources/frontend/memory_api.c"),
                   "-o", str(executable)]
        if os.environ.get("MEMORY_API_SANITIZE", "1") != "0":
            command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)
    print("PASS strict native syntax: parser, backend, PSDMA, SmartPort dispatch")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
