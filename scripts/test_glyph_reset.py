#!/usr/bin/env python3
"""Run the production SmartPort glyph reset/submit guards with fake MMIO.

Extract the actual small guard functions, avoiding unrelated Cortex-A9
assembly and FatFs in the full service. The glyph core itself is unmodified.
"""

from pathlib import Path
import os
import re
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def function(source: str, name: str) -> str:
    match = re.search(r"^(?:static )?(?:void|uint8_t) " + re.escape(name) +
                      r"\([^;]*?\)\s*\{", source, re.MULTILINE)
    if match is None:
        raise AssertionError(f"Missing production function: {name}")
    opening = source.index("{", match.start())
    depth = 1
    end = opening + 1
    while depth:
        if source[end] == "{":
            depth += 1
        elif source[end] == "}":
            depth -= 1
        end += 1
    return source[match.start():end]


def main() -> int:
    compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("gcc")
    if compiler is None:
        raise RuntimeError("A native C compiler is required")
    service = (ROOT / "ps_sources/frontend/smartport_service.c").read_text()
    hardware = (ROOT / "ps_sources/frontend/memory_api_hw.c").read_text()
    poll = function(service, "smartport_service_poll")
    reset = function(service, "smartport_service_apple_reset")
    if poll.index("sp_glyph_check_reset();") > poll.index("execute_command();"):
        raise AssertionError("Check the reset edge before dispatch")
    if "sp_glyph_check_reset();" not in reset or "glyph_api_reset();" in reset:
        raise AssertionError("A late UI hook must preserve a new-session upload")
    start = service.index("static uint32_t g_glyph_reset_sequence;")
    end = service.index("static uint16_t build_sp_status(", start)
    # This span includes the real reset globals, reset helper, submit/status
    # adapters, and backend table. Do not reimplement their logic in the test.
    guards = service[start:end]
    state_match = re.search(r"typedef struct \{.*?\} memory_hw_state_t;\s*"
                            r"static memory_hw_state_t state;", hardware, re.DOTALL)
    if state_match is None:
        raise AssertionError("Missing production transport state")
    definitions = []
    for macro in ("MEM_SP_STATUS", "MEM_SP_EXEC_PENDING"):
        match = re.search(r"^#define " + macro + r"\s+.*$", hardware, re.MULTILINE)
        if match is None:
            raise AssertionError(f"Missing production macro: {macro}")
        definitions.append(match.group())
    hardware_guards = "\n".join(definitions) + "\n" + state_match.group() + "\n"
    hardware_guards += function(hardware, "memory_api_hw_prepare") + "\n"
    hardware_guards += function(hardware, "memory_api_hw_response_valid") + "\n"
    with tempfile.TemporaryDirectory(prefix="appletini-glyph-reset-") as temporary:
        directory = Path(temporary)
        (directory / "glyph_reset_production.h").write_text(hardware_guards + guards)
        executable = directory / "glyph_reset_host"
        command = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1",
                   "-I", str(directory), "-I", str(ROOT / "ps_sources/frontend"),
                   str(ROOT / "ps_sources/frontend/glyph_api.c"),
                   str(ROOT / "scripts/fixtures/glyph_reset_host.c"),
                   "-o", str(executable)]
        if os.environ.get("GLYPH_API_SANITIZE", "1") != "0":
            command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
