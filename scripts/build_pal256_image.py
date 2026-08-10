#!/usr/bin/env python3
"""Convert source art to Appletini SHR4 PAL256 or PAL256i."""

from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image, ImageOps


BANK_SIZE = 0x8000
PIXEL_BYTES = 320 * 100
CTRL_OFFSET = 0x7DF8
MAGIC_OFFSET = 0x7DFC
PALETTE_OFFSET = 0x7E00
SHR4_MAGIC = bytes((0xD3, 0xC8, 0xD2, 0xB4))


def fit_source(source: Path, height: int, stretch: bool) -> Image.Image:
    image = Image.open(source).convert("RGB")
    if stretch:
        return image.resize((320, height), Image.Resampling.LANCZOS)
    return ImageOps.fit(image, (320, height), Image.Resampling.LANCZOS,
                        centering=(0.5, 0.5))


def quantize(image: Image.Image) -> Image.Image:
    # The IIgs palette stores four bits per channel. Snap before choosing the
    # 256 entries so the preview and packed palette remain pixel-identical.
    snapped = image.point(lambda v: min(255, ((v + 8) // 17) * 17))
    return snapped.quantize(colors=256, method=Image.Quantize.MEDIANCUT,
                            dither=Image.Dither.FLOYDSTEINBERG)


def make_bank(pixels: bytes, palette: list[int], paged: int) -> bytes:
    if len(pixels) != PIXEL_BYTES:
        raise ValueError(f"field has {len(pixels)} pixels, expected {PIXEL_BYTES}")

    bank = bytearray(BANK_SIZE)
    bank[:PIXEL_BYTES] = pixels
    bank[CTRL_OFFSET] = paged
    bank[MAGIC_OFFSET:MAGIC_OFFSET + 4] = SHR4_MAGIC

    for idx in range(256):
        r, g, b = palette[idx * 3:idx * 3 + 3]
        bank[PALETTE_OFFSET + idx * 2] = (g & 0xF0) | (b >> 4)
        bank[PALETTE_OFFSET + idx * 2 + 1] = 0x20 | (r >> 4)
    return bytes(bank)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--interlaced", action="store_true",
                        help="pack 320x200 as AUX top 100 then MAIN bottom 100")
    parser.add_argument("--stretch", action="store_true",
                        help="resize without cropping")
    parser.add_argument("--preview", type=Path)
    args = parser.parse_args()

    height = 200 if args.interlaced else 100
    image = fit_source(args.source, height, args.stretch)
    indexed = quantize(image)
    palette = indexed.getpalette()[:768]
    indices = bytes(indexed.getdata())

    if args.interlaced:
        aux = make_bank(indices[:PIXEL_BYTES], palette, 1)
        main = make_bank(indices[PIXEL_BYTES:], palette, 1)
        payload = aux + main
    else:
        payload = make_bank(indices, palette, 0)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(payload)
    if args.preview is not None:
        args.preview.parent.mkdir(parents=True, exist_ok=True)
        indexed.convert("RGB").save(args.preview)

    print(f"wrote {args.output} ({len(payload)} bytes, 320x{height})")
    if args.preview is not None:
        print(f"wrote {args.preview}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
