#!/usr/bin/env python3
"""Check the active SSI targets against the pinned die-read bit matrix."""

from __future__ import annotations

import csv
import hashlib
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
BITS = ROOT / "scripts/fixtures/ssi263_native/ssi263p_die_bits.csv"
ROM = ROOT / "hdl/apple/ssi263_sc02_rom.mem"
HOST = ROOT / "scripts/fixtures/ssi263_host/ssi263_formant_pkg.sv"
BITS_SHA256 = "73b7a6adeabaa1976e0d2b1eba500da72e4908453a66b5e718e69e7ba4ac6d8d"
CONTROL_NIBBLES_SHA256 = "ec766017bd444b275405bfb2825083e58e59ea1113328b5ba1f881ad0760118d"
ACTIVE_ROM_SHA256 = "ea494f047de11c533cb36a51d8686949206cedb91ab4855bdf9bde9500f828d9"

# Selector slots 0/1/2/3/5/6 store F1/F2/NAS/F3/VA/FA target codes.
FIELD_ROWS = {
    0: (10, 16, 22, 28),
    1: (9, 15, 21, 27),
    2: (8, 14, 20, 26),
    3: (7, 13, 19, 25),
    5: (6, 12, 18, 24),
    6: (5, 11, 17, 23),
}


def main() -> None:
    assert hashlib.sha256(BITS.read_bytes()).hexdigest() == BITS_SHA256
    with BITS.open(newline="") as stream:
        rows = list(csv.DictReader(stream))
    assert len(rows) == 64
    rom = bytes(int(token, 16) for token in re.sub(r"//[^\n]*", "", ROM.read_text()).split())
    assert len(rom) == 512
    assert hashlib.sha256(rom).hexdigest() == ACTIVE_ROM_SHA256
    assert hashlib.sha256(bytes(value & 15 for value in rom)).hexdigest() == CONTROL_NIBBLES_SHA256

    set_bits = 0
    for phone, row in enumerate(rows):
        assert int(row["code"], 16) == phone
        bits = [int(row["PAR" if bit == 28 else f"b{bit:02d}"]) for bit in range(29)]
        assert set(bits) <= {0, 1}
        set_bits += sum(bits)
        assert bits[8] == bits[14] == 0
        for selector, bit_rows in FIELD_ROWS.items():
            target = sum(bits[bit] << (3 - index) for index, bit in enumerate(bit_rows))
            assert rom[phone * 8 + selector] >> 4 == target, (phone, selector)

    assert set_bits == 749
    package = HOST.read_text()
    for phone in range(64):
        label = f"6'h{phone:02X}" if phone != 63 else "default"
        match = re.search(
            rf"{label}: ssi263_sc02_rom_row = 64'h([0-9A-F]{{16}});", package
        )
        assert match, phone
        assert bytes.fromhex(match[1])[::-1] == rom[phone * 8:phone * 8 + 8], phone

    print("SSI263 DIE ROM PASS: 1856 raw bits, 384 decoded targets, 512 ROM bytes")


if __name__ == "__main__":
    main()
