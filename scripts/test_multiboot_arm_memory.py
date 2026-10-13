#!/usr/bin/env python3
"""Exercise the ELF memory guard and optionally check real linked firmware."""

import argparse
from pathlib import Path
import subprocess
import unittest

from build_multiboot_arm import audit_frontend_memory


def symbols(stack=0x10000, stack_span=None, heap=0x04000000, heap_span=None):
    stack_span = stack if stack_span is None else stack_span
    heap_span = heap if heap_span is None else heap_span
    values = {"_STACK_SIZE": stack, "_stack": 0x05010000,
              "_stack_end": 0x05010000 - stack_span,
              "_HEAP_SIZE": heap, "_heap_start": 0x00B00000,
              "_heap_end": 0x00B00000 + heap_span}
    return "\n".join(f"{value:08x} A {name}" for name, value in values.items())


class FrontendMemoryTests(unittest.TestCase):
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--nm", type=Path, help="ARM nm executable for real ELF checks")
    parser.add_argument("--elf", action="append", type=Path, default=[], help="ELF which must pass")
    parser.add_argument("--reject-elf", action="append", type=Path, default=[], help="ELF which must fail")
    args = parser.parse_args()
    if (args.elf or args.reject_elf) and args.nm is None:
        parser.error("real ELF checks require --nm")
    result = unittest.TextTestRunner(verbosity=2).run(
        unittest.defaultTestLoader.loadTestsFromTestCase(FrontendMemoryTests))
    if not result.wasSuccessful():
        return 1
    for elf in args.elf:
        output = subprocess.check_output([str(args.nm), str(elf)], text=True)
        audit = audit_frontend_memory(output)
        print(f"PASS {elf}: stack={audit['stack_bytes']}, heap={audit['heap_bytes']}")
    for elf in args.reject_elf:
        output = subprocess.check_output([str(args.nm), str(elf)], text=True)
        try:
            audit_frontend_memory(output)
        except RuntimeError as error:
            print(f"EXPECTED REJECTION {elf}: {error}")
        else:
            raise RuntimeError(f"unsafe frontend ELF unexpectedly passed: {elf}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
