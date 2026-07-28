#!/usr/bin/env python3
"""Convert docs/Apple2e_Enhanced.rom (16384-byte binary main CPU ROM,
$C000-$FFFF) into ps_sources/frontend/apple2e_cpu_rom_data.c.

Idempotent: re-run whenever the .rom changes.

This is the fixed Enhanced //e main ROM the virtual TransWarp loads into
its BRAM shadow whenever it accelerates. Every machine (//e or II+) runs
this exact ROM when accelerated, so the accelerated personality is always
an Enhanced //e -- see vtw_service.c (VTW_ST_LOAD_ROM) and
README_VIRTUAL_TRANSWARP.md.

Offsets $0000-$00FF map to $C000-$C0FF (the I/O window; zero-filled in the
image and never ROM-routed by translate_apple_addr) and $0100-$3FFF map to
$C100-$FFFF (the internal $Cxxx firmware plus the $D000-$FFFF monitor /
Applesoft ROM).
"""

import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "docs" / "Apple2e_Enhanced.rom"
DST = ROOT / "ps_sources" / "frontend" / "apple2e_cpu_rom_data.c"

EXPECTED_BYTES = 16384


def main() -> int:
    if not SRC.exists():
        print(f"ERROR: source not found: {SRC}", file=sys.stderr)
        return 1

    data = SRC.read_bytes()
    if len(data) != EXPECTED_BYTES:
        print(
            f"ERROR: expected {EXPECTED_BYTES} bytes, got {len(data)}",
            file=sys.stderr,
        )
        return 1

    # Sanity: the Enhanced //e reset vector at $FFFC/$FFFD ($3FFC/$3FFD) is
    # $FA62. Warn (do not fail) if a different image is dropped in.
    reset_vec = data[0x3FFC] | (data[0x3FFD] << 8)
    if reset_vec != 0xFA62:
        print(
            f"WARNING: reset vector is ${reset_vec:04X}, expected $FA62 "
            "(Enhanced //e). Continuing anyway.",
            file=sys.stderr,
        )

    out = [
        "/* GENERATED -- do not edit by hand. Regenerate via",
        " *   scripts/gen_apple2e_cpu_rom_c.py",
        " * Source: docs/Apple2e_Enhanced.rom (16 KB Enhanced Apple //e main",
        " * CPU ROM, $C000-$FFFF). Loaded into the vTW shadow on every",
        " * accelerated session -- see vtw_service.c. */",
        "",
        "#include <stdint.h>",
        "",
        "const uint8_t apple2e_cpu_rom[16384] = {",
    ]
    for i in range(0, EXPECTED_BYTES, 12):
        chunk = ", ".join(f"0x{b:02x}" for b in data[i : i + 12])
        out.append(f"    {chunk},")
    out += ["};", ""]

    DST.write_text("\n".join(out))
    print(f"wrote {DST} ({len(data)} bytes, reset vector ${reset_vec:04X})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
