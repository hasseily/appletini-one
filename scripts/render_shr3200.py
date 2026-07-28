#!/usr/bin/env python3
"""Host-side reference decoder for SHR-3200 bank images.

Implements exactly the decode rules of apple_cycle_renderer.c's SHR-3200
path so real art can validate the firmware logic without hardware:

- input: 39168-byte file = 32768-byte $2000-$9FFF bank image followed by
  6400 bytes of 200 per-line 32-byte palettes (the loader's main-$2000
  payload)
- magic $B3 $B2 $B0 $B0 ("3200") at bank offset $9DFC-$2000
- ctrl block at $9DF8-$2000: {pagedMode, bank, pal_lo, pal_hi}
- per line y: palette = pal_base + y*32 in the ctrl-selected bank,
  entries are $0RGB little-endian, and the palette INDEX IS REVERSED
  (pixel nibble idx selects entry 15-idx)

Usage: render_shr3200.py IMAGE.shr OUT.png [--no-reverse]
--no-reverse renders with a straight index for A/B comparison.
"""

import struct
import sys
import zlib

BANK_SIZE = 0x8000          # $2000-$9FFF
PAL_BLOCK = 200 * 32
MAGIC_OFF = 0x9DFC - 0x2000
CTRL_OFF = 0x9DF8 - 0x2000


def fail(msg: str) -> None:
    print(f"error: {msg}", file=sys.stderr)
    raise SystemExit(1)


def write_png(path: str, width: int, height: int, rgb_rows: list) -> None:
    raw = b"".join(b"\x00" + row for row in rgb_rows)
    def chunk(tag: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data)))
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", ihdr))
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))


def main() -> None:
    argv = [a for a in sys.argv[1:] if a != "--no-reverse"]
    reverse = "--no-reverse" not in sys.argv[1:]
    if len(argv) != 2:
        fail("usage: render_shr3200.py IMAGE.shr OUT.png [--no-reverse]")
    data = open(argv[0], "rb").read()
    if len(data) < BANK_SIZE + PAL_BLOCK:
        fail(f"file is {len(data)} bytes; need {BANK_SIZE + PAL_BLOCK}")
    bank = data[:BANK_SIZE]
    if bank[MAGIC_OFF:MAGIC_OFF + 4] != b"\xb3\xb2\xb0\xb0":
        fail("no SHR-3200 magic at $9DFC")
    paged, pal_bank, pal_lo, pal_hi = bank[CTRL_OFF:CTRL_OFF + 4]
    pal_addr = pal_lo | (pal_hi << 8)
    print(f"ctrl: paged={paged} bank={'aux' if pal_bank else 'main'} "
          f"palettes at ${pal_addr:04X}")
    # The renderer reads palettes from the ctrl-selected bank; the file
    # appends the loader's main-$2000 payload, so mirror that placement.
    if pal_bank == 0:
        pal = data[BANK_SIZE:BANK_SIZE + PAL_BLOCK]
        if pal_addr != 0x2000:
            print(f"note: ctrl points at ${pal_addr:04X}, file payload "
                  "loads at $2000; using the appended block anyway")
    else:
        pal = bank[pal_addr - 0x2000:pal_addr - 0x2000 + PAL_BLOCK]

    rows = []
    for y in range(200):
        line_pal = pal[y * 32:(y + 1) * 32]
        row = bytearray()
        for xb in range(160):
            byte = bank[y * 160 + xb]
            for idx in (byte >> 4, byte & 0x0F):
                entry = (15 - idx) if reverse else idx
                lo = line_pal[entry * 2]
                hi = line_pal[entry * 2 + 1]
                r = (hi & 0x0F) * 17
                g = (lo >> 4) * 17
                b = (lo & 0x0F) * 17
                row += bytes((r, g, b, r, g, b))     # 2x horizontal
        rows.append(bytes(row))
        rows.append(bytes(row))                       # 2x vertical
    write_png(argv[1], 640, 400, rows)
    print(f"wrote {argv[1]} ({'reversed' if reverse else 'straight'} "
          "palette index)")


if __name__ == "__main__":
    main()
