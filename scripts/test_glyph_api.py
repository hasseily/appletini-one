#!/usr/bin/env python3
"""Run the production glyph API core against a native renderer mock."""

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
    with tempfile.TemporaryDirectory(prefix="appletini-glyph-api-") as temporary:
        executable = Path(temporary) / "glyph_api_host"
        command = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1",
                   "-I", str(ROOT / "ps_sources/frontend"),
                   str(ROOT / "ps_sources/frontend/glyph_api.c"),
                   str(ROOT / "scripts/fixtures/glyph_api_host.c"),
                   "-o", str(executable)]
        if os.environ.get("GLYPH_API_SANITIZE", "1") != "0":
            command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
