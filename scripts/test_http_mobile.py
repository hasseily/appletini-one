#!/usr/bin/env python3
"""Run the production request builder and redirect engine with mobile headers."""
import os
from pathlib import Path
import subprocess
from test_onee_input_service import find_native_c_compiler

ROOT = Path(__file__).resolve().parents[1]


def main():
    compiler = find_native_c_compiler()
    if not compiler:
        raise RuntimeError("Native C compiler required")
    out = ROOT / "build/http-mobile-tests"
    out.mkdir(parents=True, exist_ok=True)
    env = {key: value for key, value in os.environ.items() if key.lower() != "path"}
    env["PATH"] = str(compiler.parent) + os.pathsep + next(
        (value for key, value in os.environ.items() if key.lower() == "path"), "")
    executable = out / "http_mobile_host.exe"
    args = [str(compiler), "-std=c11", "-O1", "-Wall", "-Wextra",
            "-DAPPLETINI_HTTP_SERVICE=1", "-DMBEDTLS_CONFIG_FILE=<http_service_mbedtls_config.h>",
            "-I" + str(ROOT / "scripts/fixtures/http_service_host"),
            "-I" + str(ROOT / "ps_sources/frontend"),
            "-I" + str(ROOT / "third_party/mbedtls/include"),
            str(ROOT / "scripts/fixtures/http_mobile_host.c"),
            str(ROOT / "ps_sources/frontend/http_core.c"),
            str(ROOT / "ps_sources/frontend/http_seed.c"),
            str(ROOT / "ps_sources/frontend/http_roots.c"),
            str(ROOT / "ps_sources/lib/crc32.c")]
    args += [str(path) for path in sorted((ROOT / "third_party/mbedtls/library").glob("*.c"))]
    args += ["-lws2_32", "-o", str(executable)]
    with (out / "BUILD.log").open("w") as log:
        subprocess.run(args, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
    result = subprocess.run([str(executable)], cwd=ROOT, env=env,
                            capture_output=True, text=True, timeout=15)
    (out / "RESULT.log").write_text(result.stdout + result.stderr)
    result.check_returncode()
    print(result.stdout, end="")


if __name__ == "__main__":
    main()
