#!/usr/bin/env python3
"""Check the stack and heap reserved in a linked frontend ELF."""

import argparse
from pathlib import Path
import struct
import sys


FRONTEND_STACK_BYTES = 0x00010000
FRONTEND_HEAP_BYTES = 0x04000000
FRONTEND_MEMORY_LINK_FLAGS = (
    f"-Wl,--defsym,_HEAP_SIZE=0x{FRONTEND_HEAP_BYTES:08X},"
    f"--defsym,_STACK_SIZE=0x{FRONTEND_STACK_BYTES:08X}"
)
MEMORY_SYMBOLS = (
    "_STACK_SIZE", "_stack", "_stack_end",
    "_HEAP_SIZE", "_heap_start", "_heap_end",
)


def _record_symbol(addresses, name, value):
    if name in addresses and addresses[name] != value:
        raise RuntimeError(f"conflicting linked frontend memory symbol: {name}")
    addresses[name] = value


def _audit_addresses(addresses):
    missing = sorted(set(MEMORY_SYMBOLS) - addresses.keys())
    if missing:
        raise RuntimeError(f"missing linked frontend memory symbols: {missing}")
    if any(not 0 <= value <= 0xFFFFFFFF for value in addresses.values()):
        raise RuntimeError("frontend memory symbols must fit 32-bit addresses")
    stack_span = addresses["_stack"] - addresses["_stack_end"]
    heap_span = addresses["_heap_end"] - addresses["_heap_start"]
    if stack_span != addresses["_STACK_SIZE"] or stack_span < FRONTEND_STACK_BYTES:
        raise RuntimeError(
            f"frontend stack must reserve at least {FRONTEND_STACK_BYTES} bytes: {addresses}"
        )
    if heap_span != addresses["_HEAP_SIZE"] or heap_span != FRONTEND_HEAP_BYTES:
        raise RuntimeError(
            f"frontend heap must retain {FRONTEND_HEAP_BYTES} bytes: {addresses}"
        )
    if addresses["_stack_end"] < addresses["_heap_end"]:
        raise RuntimeError("frontend stack overlaps the heap")
    return {
        "symbols": addresses, "stack_bytes": stack_span,
        "minimum_stack_bytes": FRONTEND_STACK_BYTES, "heap_bytes": heap_span,
        "stack_span_matches": True, "heap_span_matches": True,
    }


def audit_frontend_memory(symbols):
    """Accept nm text for existing callers; the ELF CLI needs no toolchain."""
    addresses = {}
    for line in symbols.splitlines():
        fields = line.split()
        if len(fields) == 3 and fields[2] in MEMORY_SYMBOLS:
            try:
                value = int(fields[0], 16)
            except ValueError as error:
                raise RuntimeError(f"invalid frontend memory symbol: {line}") from error
            _record_symbol(addresses, fields[2], value)
    return _audit_addresses(addresses)


def _elf_memory_symbols(data):
    if len(data) < 52 or data[:7] != b"\x7fELF\x01\x01\x01":
        raise RuntimeError("frontend must be a 32-bit little-endian ELF")
    kind, machine, version = struct.unpack_from("<HHI", data, 16)
    header_size = struct.unpack_from("<H", data, 40)[0]
    if kind != 2 or machine != 40 or version != 1 or header_size != 52:
        raise RuntimeError("frontend must be an ELF32 ARM executable")
    section_offset = struct.unpack_from("<I", data, 32)[0]
    section_size, section_count = struct.unpack_from("<HH", data, 46)
    if (section_size != 40 or section_count == 0 or section_offset < 52 or
            section_offset + section_size * section_count > len(data)):
        raise RuntimeError("frontend ELF section table is missing or truncated")
    sections = [
        struct.unpack_from("<10I", data, section_offset + section_size * index)
        for index in range(section_count)
    ]

    def contents(section, label):
        offset, size = section[4:6]
        if offset > len(data) or size > len(data) - offset:
            raise RuntimeError(f"frontend ELF {label} is truncated")
        return data[offset:offset + size]

    addresses = {}
    for section in sections:
        if section[1] != 2:  # SHT_SYMTAB: require linked, unstripped symbols.
            continue
        if section[9] != 16 or section[5] % 16:
            raise RuntimeError("frontend ELF symbol table has an invalid entry size")
        link = section[6]
        if link >= len(sections) or sections[link][1] != 3:
            raise RuntimeError("frontend ELF symbol table has no valid string table")
        strings = contents(sections[link], "string table")
        symbols = contents(section, "symbol table")
        for offset in range(0, len(symbols), 16):
            name_offset, value, _, _, _, index = struct.unpack_from("<IIIBBH", symbols, offset)
            end = strings.find(b"\0", name_offset)
            if name_offset >= len(strings) or end < 0:
                raise RuntimeError("frontend ELF symbol name is out of bounds or unterminated")
            name = strings[name_offset:end].decode("ascii", errors="replace")
            if name not in MEMORY_SYMBOLS:
                continue
            if index == 0:
                raise RuntimeError(f"undefined linked frontend memory symbol: {name}")
            if index != 0xFFF1 and index >= len(sections):  # SHN_ABS or an existing section.
                raise RuntimeError(f"invalid section for frontend memory symbol: {name}")
            _record_symbol(addresses, name, value)
    return addresses


def audit_frontend_elf(path):
    """Read and check the selected ELF itself, rather than requested flags."""
    return _audit_addresses(_elf_memory_symbols(Path(path).read_bytes()))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path, help="linked frontend ELF to check")
    args = parser.parse_args()
    try:
        audit = audit_frontend_elf(args.elf)
    except (OSError, RuntimeError) as error:
        print(f"ERROR: {args.elf}: {error}", file=sys.stderr)
        return 1
    print(f"PASS frontend memory: stack={audit['stack_bytes']}, heap={audit['heap_bytes']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
