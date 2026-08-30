#!/usr/bin/env python3
"""Build and run the exact serial-monitor number parser on the host."""

from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "ps_sources" / "lib" / "number_parse.h"
SERIAL_BOOT = ROOT / "ps_sources" / "bootloader" / "serial_boot.c"


TEST_SOURCE = r'''
#include <stdint.h>
#include <stdio.h>

#include "number_parse.h"

typedef struct {
    const char *text;
    uint32_t expected;
} valid_case_t;

static int expect_valid(const valid_case_t *item)
{
    uint32_t value = 0x55AA55AAU;

    if (appletini_parse_u32(item->text, &value) != 0 ||
        value != item->expected) {
        fprintf(stderr, "valid parse failed: %s -> 0x%08X\n",
                item->text, (unsigned)value);
        return 1;
    }
    return 0;
}

static int expect_invalid(const char *text)
{
    uint32_t value = 0x55AA55AAU;

    if (appletini_parse_u32(text, &value) == 0 || value != 0x55AA55AAU) {
        fprintf(stderr, "invalid parse accepted: %s\n",
                text == NULL ? "(null)" : text);
        return 1;
    }
    return 0;
}

int main(void)
{
    static const valid_case_t valid[] = {
        {"0", 0U},
        {"291", 291U},
        {"0x123", 0x123U},
        {"0X123", 0x123U},
        {"0x89ABCDEF", 0x89ABCDEFU},
        {"0xffffffff", UINT32_MAX},
        {"4294967295", UINT32_MAX},
        {"0123", 123U},
    };
    static const char *invalid[] = {
        "", "0x", "0X", "+1", "-1", " 1", "1 ", "0xG",
        "0x100000000", "4294967296", "123A", "0x..."
    };
    unsigned i;

    for (i = 0U; i < (sizeof(valid) / sizeof(valid[0])); ++i) {
        if (expect_valid(&valid[i]) != 0) {
            return 1;
        }
    }
    for (i = 0U; i < (sizeof(invalid) / sizeof(invalid[0])); ++i) {
        if (expect_invalid(invalid[i]) != 0) {
            return 1;
        }
    }
    if (expect_invalid(NULL) != 0 ||
        appletini_parse_u32("1", NULL) == 0) {
        return 1;
    }
    puts("serial number parser: PASS");
    return 0;
}
'''


def find_compiler() -> str:
    candidates = [
        shutil.which("gcc"),
        str(Path("C:/msys64/ucrt64/bin/gcc.exe")),
        shutil.which("clang"),
    ]
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return candidate
    raise RuntimeError("no host C compiler found")


def main() -> int:
    if not HEADER.is_file():
        raise RuntimeError(f"missing parser header: {HEADER}")

    serial_source = SERIAL_BOOT.read_text(encoding="utf-8")
    required = (
        "appletini_parse_u32(argv[1], &expected_size)",
        "appletini_parse_u32(argv[2], &expected_crc)",
        "cmd_len == 0U && (c == ':' || c == ';')",
    )
    for text in required:
        if text not in serial_source:
            raise RuntimeError(f"serial monitor does not use tested path: {text}")

    compiler = find_compiler()
    with tempfile.TemporaryDirectory(
        prefix=".appletini-number-parse-", dir=ROOT
    ) as temp:
        temp_path = Path(temp)
        source = temp_path / "test_number_parse.c"
        executable = temp_path / "test_number_parse.exe"
        source.write_text(TEST_SOURCE, encoding="utf-8")
        compiler_env = os.environ.copy()
        compiler_env["TMP"] = str(temp_path)
        compiler_env["TEMP"] = str(temp_path)
        compiler_env["TMPDIR"] = str(temp_path)
        compiler_env["PATH"] = (
            str(Path(compiler).parent) + os.pathsep + compiler_env.get("PATH", "")
        )
        compiled = subprocess.run(
            [
                compiler,
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(HEADER.parent),
                str(source),
                "-o",
                str(executable),
            ],
            capture_output=True,
            text=True,
            env=compiler_env,
        )
        if compiled.returncode != 0:
            raise RuntimeError(
                "host parser build failed:\n"
                f"{compiled.stdout}{compiled.stderr}"
            )
        subprocess.run([str(executable)], check=True)

    print("serial monitor number tests: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
