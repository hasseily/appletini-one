#!/usr/bin/env python3
"""Exercise the I2C wrapper with a native mock of the XIicPs transfer API."""

import os
from pathlib import Path
import subprocess
import tempfile

from test_onee_input_service import find_native_c_compiler


ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    compiler = find_native_c_compiler()
    if compiler is None:
        raise RuntimeError("A native C compiler is required")

    build = ROOT / "build"
    build.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="i2c_cooperative_", dir=build) as temporary:
        lib = ROOT / "ps_sources/lib"
        fixtures = ROOT / "scripts/fixtures"
        executable = Path(temporary) / ("i2c_cooperative.exe" if os.name == "nt" else "i2c_cooperative")
        env = dict(os.environ)
        env["PATH"] = str(compiler.parent) + os.pathsep + env["PATH"]
        command = [str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror",
                   f"-I{lib}", f"-I{fixtures / 'i2c_cooperative_bsp'}",
                   str(lib / "i2c.c"), str(fixtures / "i2c_cooperative_host.c"),
                   "-o", str(executable)]
        subprocess.run(command, cwd=ROOT, env=env, check=True)
        subprocess.run([str(executable)], cwd=ROOT, env=env, check=True, timeout=10)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
