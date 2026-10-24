#!/usr/bin/env python3
"""Run the legacy packager and prove unsafe frontend ELFs never reach bootgen."""

from __future__ import annotations

import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
PACKAGER = ROOT / "scripts/make_firmware_bin.bat"


def frontend_elf(*, stack=65536, stack_span=None, omit=None, heap=67108864):
    """Make an ELF32 ARM symbol table without needing a cross compiler."""
    stack_span = stack if stack_span is None else stack_span
    values = {
        "_STACK_SIZE": stack,
        "_stack_end": 0x05000000,
        "_stack": 0x05000000 + stack_span,
        "_HEAP_SIZE": heap,
        "_heap_start": 0x01000000,
        "_heap": 0x01000000,
        "_heap_end": 0x01000000 + heap,
    }
    if omit is not None:
        del values[omit]
    names = bytearray(b"\0")
    symbols = bytearray(16)
    for name, value in values.items():
        offset = len(names)
        names.extend(name.encode("ascii") + b"\0")
        symbols.extend(struct.pack("<IIIBBH", offset, value, 0, 0x10, 0, 0xFFF1))
    section_names = b"\0.shstrtab\0.strtab\0.symtab\0"
    ident = b"\x7fELF\x01\x01\x01" + bytes(9)
    body = bytearray(52)
    sections = [bytes(40)]
    for name, kind, data, link, alignment, entry_size in (
        (1, 3, section_names, 0, 1, 0),
        (11, 3, names, 0, 1, 0),
        (19, 2, symbols, 2, 4, 16),
    ):
        body.extend(bytes((-len(body)) % alignment))
        offset = len(body)
        body.extend(data)
        sections.append(struct.pack("<10I", name, kind, 0, 0, offset,
                                    len(data), link, 1 if kind == 2 else 0,
                                    alignment, entry_size))
    body.extend(bytes((-len(body)) % 4))
    section_offset = len(body)
    body.extend(b"".join(sections))
    body[:52] = struct.pack("<16sHHIIIIIHHHHHH", ident, 2, 40, 1,
                           0, 0, section_offset, 0x05000000,
                           52, 0, 0, 40, len(sections), 1)
    return bytes(body)


@unittest.skipUnless(os.name == "nt", "the legacy packager runs under Windows cmd")
class FirmwareMemoryGateTests(unittest.TestCase):
    def run_packager(self, selected_elf, default_elf):
        with tempfile.TemporaryDirectory(prefix="firmware memory gate ") as temp_name:
            work = Path(temp_name)
            tool_bin = work / "fake Vitis/bin"
            tool_bin.mkdir(parents=True)
            marker = work / "bootgen called.txt"
            (tool_bin / "bootgen.bat").write_text(
                '@echo off\n> "%PACKAGING_TEST_MARKER%" echo called\nexit /b 73\n',
                encoding="ascii",
            )
            selected = work / "selected frontend.elf"
            selected.write_bytes(selected_elf)
            default = work / "vitis_workspace/frontend/build/frontend.elf"
            default.parent.mkdir(parents=True)
            default.write_bytes(default_elf)
            fsbl = work / "fsbl.elf"
            fsbl.write_bytes(b"FSBL is outside the frontend memory contract")
            bit = work / "selected.bit"
            bit.write_bytes(b"bitstream placeholder")
            env = {key: value for key, value in os.environ.items()
                   if key.upper() != "PATH"}
            env["PATH"] = str(Path(sys.executable).parent) + os.pathsep + os.environ.get("PATH", "")
            env["XILINX_VITIS"] = str(tool_bin.parent)
            env["PACKAGING_TEST_MARKER"] = str(marker)
            command = subprocess.list2cmdline([
                str(PACKAGER), str(work / "FIRMWARE.BIN"),
                str(fsbl), str(bit), str(selected),
            ])
            result = subprocess.run(command, cwd=work, env=env, shell=True,
                                    capture_output=True, text=True, timeout=30)
            return result, marker.exists()

    def test_valid_selected_frontend_reaches_bootgen_with_unsafe_default(self):
        result, reached = self.run_packager(frontend_elf(), frontend_elf(stack=8192))
        self.assertTrue(reached, result.stdout + result.stderr)
        self.assertEqual(result.returncode, 73, result.stdout + result.stderr)

    def test_unsafe_selected_frontend_is_rejected_before_bootgen(self):
        for name, invalid in (
            ("old 8 KiB stack", frontend_elf(stack=8192)),
            ("mismatched stack span", frontend_elf(stack_span=8192)),
            ("missing stack symbol", frontend_elf(omit="_stack_end")),
            ("wrong heap reservation", frontend_elf(heap=8192)),
            ("not an ELF", b"invalid frontend"),
        ):
            with self.subTest(name=name):
                result, reached = self.run_packager(invalid, frontend_elf())
                self.assertFalse(reached, result.stdout + result.stderr)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("frontend memory audit failed", result.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
