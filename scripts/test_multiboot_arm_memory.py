#!/usr/bin/env python3
"""Exercise the ELF memory guard and optionally check real linked firmware."""

import argparse
from pathlib import Path
import struct
import tempfile
import unittest

from build_multiboot_arm import audit_frontend_memory
from frontend_memory import audit_frontend_elf


def addresses(stack=0x10000, stack_span=None, heap=0x04000000, heap_span=None):
    stack_span = stack if stack_span is None else stack_span
    heap_span = heap if heap_span is None else heap_span
    return {"_STACK_SIZE": stack, "_stack": 0x05010000,
            "_stack_end": 0x05010000 - stack_span,
            "_HEAP_SIZE": heap, "_heap_start": 0x00B00000,
            "_heap_end": 0x00B00000 + heap_span}


def symbols(**kwargs):
    return "\n".join(f"{value:08x} A {name}" for name, value in addresses(**kwargs).items())


def elf_fixture(values=None, undefined=None, duplicate=None):
    """Small linked ARM ELF with real section, symbol and string tables."""
    values = addresses() if values is None else values
    strings = bytearray(b"\0")
    table = bytearray(16)  # ELF's null symbol.
    entries = list(values.items()) + ([duplicate] if duplicate else [])
    for name, value in entries:
        name_offset = len(strings)
        strings.extend(name.encode("ascii") + b"\0")
        table.extend(struct.pack("<IIIBBH", name_offset, value, 0, 0x10, 0,
                                 0 if name == undefined else 0xFFF1))
    string_offset = 52
    symbol_offset = (string_offset + len(strings) + 3) & ~3
    section_offset = symbol_offset + len(table)
    data = bytearray(section_offset + 3 * 40)
    ident = b"\x7fELF\x01\x01\x01" + bytes(9)
    struct.pack_into("<16sHHIIIIIHHHHHH", data, 0, ident, 2, 40, 1, 0, 0,
                     section_offset, 0, 52, 0, 0, 40, 3, 0)
    data[string_offset:string_offset + len(strings)] = strings
    data[symbol_offset:symbol_offset + len(table)] = table
    struct.pack_into("<10I", data, section_offset + 40,
                     0, 3, 0, 0, string_offset, len(strings), 0, 0, 1, 0)
    struct.pack_into("<10I", data, section_offset + 80,
                     0, 2, 0, 0, symbol_offset, len(table), 1, 1, 4, 16)
    return data


class FrontendMemoryTests(unittest.TestCase):
    def audit_bytes(self, data):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "frontend.elf"
            path.write_bytes(data)
            return audit_frontend_elf(path)

    def test_minimum_stack_and_retained_heap(self):
        result = audit_frontend_memory(symbols())
        self.assertEqual(result["stack_bytes"], 65536)
        self.assertEqual(result["heap_bytes"], 64 * 1024 * 1024)
        self.assertTrue(result["stack_span_matches"])

    def test_old_vitis_stack_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "frontend stack"):
            audit_frontend_memory(symbols(stack=8192))

    def test_nominal_size_cannot_hide_smaller_linked_span(self):
        with self.assertRaisesRegex(RuntimeError, "frontend stack"):
            audit_frontend_memory(symbols(stack_span=8192))

    def test_heap_contract_is_checked(self):
        for sample in (symbols(heap=8192), symbols(heap_span=8192)):
            with self.subTest(sample=sample), self.assertRaisesRegex(RuntimeError, "frontend heap"):
                audit_frontend_memory(sample)

    def test_missing_linked_reservation_fails_closed(self):
        with self.assertRaisesRegex(RuntimeError, "missing linked frontend memory symbols"):
            audit_frontend_memory(symbols().replace("_stack_end", "ignored_symbol"))

    def test_real_elf_symbol_tables_are_checked(self):
        for stack in (0x10000, 0x20000):
            with self.subTest(stack=stack):
                result = self.audit_bytes(elf_fixture(addresses(stack=stack)))
                self.assertEqual(result["stack_bytes"], stack)
                self.assertEqual(result["heap_bytes"], 0x04000000)

    def test_elf_rejects_undersized_and_mismatched_reservations(self):
        for sample in (addresses(stack=8192), addresses(stack_span=8192),
                       addresses(heap=8192), addresses(heap_span=8192)):
            with self.subTest(sample=sample), self.assertRaises(RuntimeError):
                self.audit_bytes(elf_fixture(sample))

    def test_elf_missing_undefined_and_conflicting_symbols_fail_closed(self):
        missing = addresses()
        del missing["_stack_end"]
        cases = (
            (elf_fixture(missing), "missing linked"),
            (elf_fixture(undefined="_stack_end"), "undefined linked"),
            (elf_fixture(duplicate=("_stack_end", 1)), "conflicting linked"),
        )
        for data, message in cases:
            with self.subTest(message=message), self.assertRaisesRegex(RuntimeError, message):
                self.audit_bytes(data)

    def test_elf_wrong_format_and_truncation_fail_closed(self):
        valid = elf_fixture()
        wrong_machine = bytearray(valid)
        struct.pack_into("<H", wrong_machine, 18, 62)
        wrong_class = bytearray(valid)
        wrong_class[4] = 2
        wrong_type = bytearray(valid)
        struct.pack_into("<H", wrong_type, 16, 1)
        for data in (b"", b"not an ELF", valid[:51], valid[:-1],
                     wrong_machine, wrong_class, wrong_type):
            with self.subTest(length=len(data)), self.assertRaises(RuntimeError):
                self.audit_bytes(data)

    def test_elf_corrupt_table_offsets_and_names_fail_closed(self):
        valid = elf_fixture()
        section_offset = struct.unpack_from("<I", valid, 32)[0]
        symbol_offset = struct.unpack_from("<I", valid, section_offset + 80 + 16)[0]
        changes = (
            (section_offset + 80 + 24, 99),  # Invalid string-table link.
            (section_offset + 80 + 36, 0),   # Zero symbol-entry size.
            (section_offset + 80 + 16, len(valid)),  # Truncated symbol data.
            (section_offset + 40 + 16, len(valid)),  # Truncated string data.
            (symbol_offset + 16, 0xFFFFFFFF),       # Invalid symbol-name offset.
        )
        for offset, value in changes:
            data = bytearray(valid)
            struct.pack_into("<I", data, offset, value)
            with self.subTest(offset=offset), self.assertRaises(RuntimeError):
                self.audit_bytes(data)

    def test_stack_must_not_overlap_heap(self):
        sample = addresses()
        sample["_stack_end"] = sample["_heap_end"] - 4096
        sample["_stack"] = sample["_stack_end"] + sample["_STACK_SIZE"]
        with self.assertRaisesRegex(RuntimeError, "overlaps"):
            self.audit_bytes(elf_fixture(sample))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--nm", type=Path, help="accepted for old commands; no longer needed")
    parser.add_argument("--elf", action="append", type=Path, default=[], help="ELF which must pass")
    parser.add_argument("--reject-elf", action="append", type=Path, default=[], help="ELF which must fail")
    args = parser.parse_args()
    result = unittest.TextTestRunner(verbosity=2).run(
        unittest.defaultTestLoader.loadTestsFromTestCase(FrontendMemoryTests))
    if not result.wasSuccessful():
        return 1
    for elf in args.elf:
        audit = audit_frontend_elf(elf)
        print(f"PASS {elf}: stack={audit['stack_bytes']}, heap={audit['heap_bytes']}")
    for elf in args.reject_elf:
        try:
            audit_frontend_elf(elf)
        except RuntimeError as error:
            print(f"EXPECTED REJECTION {elf}: {error}")
        else:
            raise RuntimeError(f"unsafe frontend ELF unexpectedly passed: {elf}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
