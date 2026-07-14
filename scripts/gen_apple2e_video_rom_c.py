#!/usr/bin/env python3
"""Convert hdl/apple/apple2e_video_rom_342_0265_a.mem (4096 hex bytes, one
per line) into ps_sources/frontend/apple2e_video_rom_data.c.

Idempotent: re-run whenever the .mem changes.

The .mem file is the same data the FPGA's apple2e_video_rom module loads
via $readmemh, so the C array is bit-identical to what the PL sees. Stage
2b.2's appletini_csbits.c consumes this array in place of AppleWin's
GetResource(IDR_APPLE2E_ENHANCED_VIDEO_ROM) lookup.
"""

import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "hdl" / "apple" / "apple2e_video_rom_342_0265_a.mem"
DST = ROOT / "ps_sources" / "frontend" / "apple2e_video_rom_data.c"

EXPECTED_BYTES = 4096


def main() -> int:
    if not SRC.exists():
        print(f"ERROR: source not found: {SRC}", file=sys.stderr)
        return 1

    bytes_ = []
    for line_no, raw in enumerate(SRC.read_text().splitlines(), start=1):
        line = raw.strip()
        if not line or line.startswith("//"):
            continue
        try:
            bytes_.append(int(line, 16))
        except ValueError:
            print(f"ERROR: {SRC}:{line_no}: bad hex {line!r}", file=sys.stderr)
            return 1

    if len(bytes_) != EXPECTED_BYTES:
        print(
            f"ERROR: expected {EXPECTED_BYTES} bytes, got {len(bytes_)}",
            file=sys.stderr,
        )
        return 1

    out = [
        "/* GENERATED -- do not edit by hand. Regenerate via",
        " *   scripts/gen_apple2e_video_rom_c.py",
        " * Source: hdl/apple/apple2e_video_rom_342_0265_a.mem (4 KB enhanced",
        " * Apple //e video ROM, part 342-0265-A). */",
        "",
        "#include <stdint.h>",
        "",
        "const uint8_t apple2e_video_rom[4096] = {",
    ]
    for i in range(0, EXPECTED_BYTES, 8):
        chunk = ", ".join(f"0x{b:02x}" for b in bytes_[i : i + 8])
        out.append(f"    {chunk},")
    out += ["};", ""]

    DST.write_text("\n".join(out))
    print(f"wrote {DST} ({len(bytes_)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
