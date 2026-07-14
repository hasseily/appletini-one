#!/usr/bin/env python3
"""
Convert an image (PNG/JPG/...) to RGB565 C source for framebuffer blitting.

Requires:
  pip install pillow

Example:
  py scripts/png_to_rgb565_c.py ^
      --input C:\images\logo.png ^
      --name logo ^
      --out-dir vitis_workspace/text_ui_test ^
      --max-width 320 ^
      --max-height 180
"""

from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Convert image to RGB565 C array")
    p.add_argument("--input", required=True, help="Input image path")
    p.add_argument("--name", required=True, help="Output symbol/file prefix")
    p.add_argument("--out-dir", default=".", help="Output directory")
    p.add_argument("--max-width", type=int, default=0, help="Optional max width (0 disables)")
    p.add_argument("--max-height", type=int, default=0, help="Optional max height (0 disables)")
    return p.parse_args()


def rgb_to_565(r: int, g: int, b: int) -> int:
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def resize_if_needed(img: Image.Image, max_w: int, max_h: int) -> Image.Image:
    if max_w <= 0 or max_h <= 0:
        return img
    if img.width <= max_w and img.height <= max_h:
        return img
    out = img.copy()
    out.thumbnail((max_w, max_h), Image.Resampling.LANCZOS)
    return out


def emit_h(name: str) -> str:
    guard = f"{name.upper()}_RGB565_H"
    return f"""#ifndef {guard}
#define {guard}

#include <stdint.h>

extern const uint16_t {name}_pixels[];
extern const uint16_t {name}_width;
extern const uint16_t {name}_height;

#endif
"""


def emit_c(name: str, w: int, h: int, pixels: list[int]) -> str:
    lines = [f'#include "{name}_rgb565.h"', "", f"const uint16_t {name}_width = {w}U;", f"const uint16_t {name}_height = {h}U;", "", f"const uint16_t {name}_pixels[] = {{"]
    cols = 10
    for i in range(0, len(pixels), cols):
        chunk = ", ".join(f"0x{v:04X}" for v in pixels[i : i + cols])
        lines.append(f"    {chunk},")
    lines.append("};")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    args = parse_args()
    in_path = Path(args.input)
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    img = Image.open(in_path).convert("RGB")
    img = resize_if_needed(img, args.max_width, args.max_height)

    pixels_565: list[int] = []
    for y in range(img.height):
        for x in range(img.width):
            r, g, b = img.getpixel((x, y))
            pixels_565.append(rgb_to_565(r, g, b))

    h_path = out_dir / f"{args.name}_rgb565.h"
    c_path = out_dir / f"{args.name}_rgb565.c"

    h_path.write_text(emit_h(args.name), encoding="utf-8")
    c_path.write_text(emit_c(args.name, img.width, img.height, pixels_565), encoding="utf-8")

    print(f"Wrote: {h_path}")
    print(f"Wrote: {c_path}")
    print(f"Size: {img.width}x{img.height} ({len(pixels_565)} pixels)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
