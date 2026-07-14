#!/usr/bin/env python3
"""
Convert a TTF font into packed 1bpp C font tables for Appletini framebuffer UI.

Requires:
  pip install pillow

Example:
  py scripts/ttf_to_c_font.py ^
      --ttf C:\fonts\PressStart2P.ttf ^
      --size 24 ^
      --name ui_font24 ^
      --out-dir vitis_workspace/text_ui_test
"""

from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Generate C bitmap font from TTF")
    p.add_argument("--ttf", required=True, help="Path to .ttf font file")
    p.add_argument("--size", type=int, default=24, help="Pixel size (default: 24)")
    p.add_argument("--first", type=int, default=32, help="First ASCII code (default: 32)")
    p.add_argument("--last", type=int, default=126, help="Last ASCII code (default: 126)")
    p.add_argument("--name", default="ui_font24", help="C symbol/file prefix (default: ui_font24)")
    p.add_argument("--threshold", type=int, default=128, help="Luma threshold 0..255 (default: 128)")
    p.add_argument("--out-dir", default=".", help="Output directory")
    return p.parse_args()


def glyph_bbox(font: ImageFont.FreeTypeFont, ch: str) -> tuple[int, int, int, int]:
    # left, top, right, bottom relative to text origin
    l, t, r, b = font.getbbox(ch)
    return int(l), int(t), int(r), int(b)


def compute_global_cell(font: ImageFont.FreeTypeFont, first: int, last: int) -> tuple[int, int, int, int]:
    min_l = 10**9
    min_t = 10**9
    max_r = -10**9
    max_b = -10**9

    for code in range(first, last + 1):
        l, t, r, b = glyph_bbox(font, chr(code))
        min_l = min(min_l, l)
        min_t = min(min_t, t)
        max_r = max(max_r, r)
        max_b = max(max_b, b)

    width = max(1, max_r - min_l)
    height = max(1, max_b - min_t)
    return width, height, min_l, min_t


def render_glyph(font: ImageFont.FreeTypeFont, ch: str, width: int, height: int, origin_x: int, origin_y: int, threshold: int) -> list[int]:
    img = Image.new("L", (width, height), 0)
    draw = ImageDraw.Draw(img)
    draw.text((origin_x, origin_y), ch, fill=255, font=font)

    row_bytes = (width + 7) // 8
    out = [0] * (row_bytes * height)
    pix = img.load()

    for y in range(height):
        for x in range(width):
            if pix[x, y] >= threshold:
                idx = y * row_bytes + (x // 8)
                bit = 7 - (x & 7)
                out[idx] |= (1 << bit)
    return out


def c_array_bytes(name: str, values: list[int]) -> str:
    lines = [f"const uint8_t {name}[] = {{"]
    chunk = 12
    for i in range(0, len(values), chunk):
        part = ", ".join(f"0x{v:02X}" for v in values[i : i + chunk])
        lines.append(f"    {part},")
    lines.append("};")
    return "\n".join(lines)


def main() -> int:
    args = parse_args()
    ttf = Path(args.ttf)
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    if args.first < 0 or args.last > 255 or args.first > args.last:
        raise SystemExit("Invalid character range")
    if not (0 <= args.threshold <= 255):
        raise SystemExit("threshold must be 0..255")

    font = ImageFont.truetype(str(ttf), args.size)
    width, height, min_l, min_t = compute_global_cell(font, args.first, args.last)
    row_bytes = (width + 7) // 8
    bytes_per_glyph = row_bytes * height

    data: list[int] = []
    for code in range(args.first, args.last + 1):
        ch = chr(code)
        glyph = render_glyph(
            font=font,
            ch=ch,
            width=width,
            height=height,
            origin_x=-min_l,
            origin_y=-min_t,
            threshold=args.threshold,
        )
        data.extend(glyph)

    sym = args.name
    h_path = out_dir / f"{sym}.h"
    c_path = out_dir / f"{sym}.c"

    header = f"""#ifndef {sym.upper()}_H
#define {sym.upper()}_H

#include <stdint.h>
#include "framebuffer.h"

extern const uint8_t {sym}_data[];
extern const fb_bitmap_font_t {sym};

#endif
"""

    source = f"""#include "{sym}.h"

{c_array_bytes(sym + "_data", data)}

const fb_bitmap_font_t {sym} = {{
    .width = {width}U,
    .height = {height}U,
    .first_char = {args.first}U,
    .last_char = {args.last}U,
    .bytes_per_glyph = {bytes_per_glyph}U,
    .data = {sym}_data,
}};
"""

    h_path.write_text(header, encoding="utf-8")
    c_path.write_text(source, encoding="utf-8")

    print(f"Wrote: {h_path}")
    print(f"Wrote: {c_path}")
    print(f"Font cell: {width}x{height}, bytes/glyph={bytes_per_glyph}, glyphs={args.last - args.first + 1}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
