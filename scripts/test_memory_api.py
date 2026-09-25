#!/usr/bin/env python3
"""Compile and execute the actual memory API core against a native mock backend."""

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
    with tempfile.TemporaryDirectory(prefix="appletini-memory-api-") as temporary:
        executable = Path(temporary) / "memory_api_host"
        command = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1",
                   "-I", str(ROOT / "ps_sources/frontend"),
                   str(ROOT / "ps_sources/frontend/memory_api.c"),
                   str(ROOT / "scripts/fixtures/memory_api_host.c"),
                   "-o", str(executable)]
        if os.environ.get("MEMORY_API_SANITIZE", "1") != "0":
            command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
