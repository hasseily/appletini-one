#!/usr/bin/env python3
"""Generate software/launcher_assets.a65 for the HGR launcher.

Emits three tables, dependency-free (no PIL, no PNG):
  - a mixed-case 7x8 bitmap font (ASCII 32..127, byte-aligned cells,
    bit 0 = leftmost pixel, glyphs drawn in columns 0-5),
  - the "APPLETINI ONE" logotype, pre-rendered from the same font at
    3x scale with diagonal smoothing, packed as 40-byte HGR rows,
  - HGR row-address lo/hi tables for all 192 lines.

Run from anywhere; writes next to the launcher source.
"""
from __future__ import annotations

from pathlib import Path

OUT = Path(__file__).resolve().parents[1] / "software" / "launcher_assets.a65"

# ---------------------------------------------------------------------------
# 7x8 font. Each glyph: 8 strings of up to 7 chars; '#' = pixel.
# Glyphs live in columns 0-5, column 6 is inter-character spacing.
# Baseline at row 6; lowercase descenders use rows 6-7.
# ---------------------------------------------------------------------------
F = {
' ': ["       "]*8,
'!': ["  ##   ","  ##   ","  ##   ","  ##   ","  ##   ","       ","  ##   ","       "],
"'": ["  ##   ","  ##   ","   #   ","       ","       ","       ","       ","       "],
'(': ["   ##  ","  ##   ","  ##   ","  ##   ","  ##   ","  ##   ","   ##  ","       "],
')': ["  ##   ","   ##  ","   ##  ","   ##  ","   ##  ","   ##  ","  ##   ","       "],
'+': ["       ","  ##   ","  ##   ","######"," ##    "," ##    ","       ","       "],
',': ["       ","       ","       ","       ","       ","  ##   ","  ##   "," ##    "],
'-': ["       ","       ","       ","###### ","       ","       ","       ","       "],
'.': ["       ","       ","       ","       ","       ","  ##   ","  ##   ","       "],
'/': ["     # ","    ## ","   ##  ","  ##   "," ##    ","##     ","#      ","       "],
'0': [" ####  ","##  ## ","## ### ","###### ","### ## ","##  ## "," ####  ","       "],
'1': ["  ##   "," ###   ","  ##   ","  ##   ","  ##   ","  ##   ","###### ","       "],
'2': [" ####  ","##  ## ","    ## ","   ##  ","  ##   "," ##    ","###### ","       "],
'3': [" ####  ","##  ## ","    ## ","  ###  ","    ## ","##  ## "," ####  ","       "],
'4': ["   ### ","  #### "," ## ## ","###### ","    ## ","    ## ","    ## ","       "],
'5': ["###### ","##     ","#####  ","    ## ","    ## ","##  ## "," ####  ","       "],
'6': [" ####  ","##     ","#####  ","##  ## ","##  ## ","##  ## "," ####  ","       "],
'7': ["###### ","    ## ","   ##  ","   ##  ","  ##   ","  ##   ","  ##   ","       "],
'8': [" ####  ","##  ## ","##  ## "," ####  ","##  ## ","##  ## "," ####  ","       "],
'9': [" ####  ","##  ## ","##  ## "," ##### ","    ## ","    ## "," ####  ","       "],
':': ["       ","  ##   ","  ##   ","       ","  ##   ","  ##   ","       ","       "],
'=': ["       ","       ","###### ","       ","###### ","       ","       ","       "],
'?': [" ####  ","##  ## ","    ## ","   ##  ","  ##   ","       ","  ##   ","       "],
'[': [" ####  "," ##    "," ##    "," ##    "," ##    "," ##    "," ####  ","       "],
']': [" ####  ","   ##  ","   ##  ","   ##  ","   ##  ","   ##  "," ####  ","       "],
'A': [" ####  ","##  ## ","##  ## ","###### ","##  ## ","##  ## ","##  ## ","       "],
'B': ["#####  ","##  ## ","##  ## ","#####  ","##  ## ","##  ## ","#####  ","       "],
'C': [" ####  ","##  ## ","##     ","##     ","##     ","##  ## "," ####  ","       "],
'D': ["#####  ","##  ## ","##  ## ","##  ## ","##  ## ","##  ## ","#####  ","       "],
'E': ["###### ","##     ","##     ","#####  ","##     ","##     ","###### ","       "],
'F': ["###### ","##     ","##     ","#####  ","##     ","##     ","##     ","       "],
'G': [" ####  ","##  ## ","##     ","## ### ","##  ## ","##  ## "," ####  ","       "],
'H': ["##  ## ","##  ## ","##  ## ","###### ","##  ## ","##  ## ","##  ## ","       "],
'I': [" ####  ","  ##   ","  ##   ","  ##   ","  ##   ","  ##   "," ####  ","       "],
'J': ["  #### ","   ##  ","   ##  ","   ##  ","   ##  ","## ##  "," ###   ","       "],
'K': ["##  ## ","## ##  ","####   ","###    ","####   ","## ##  ","##  ## ","       "],
'L': ["##     ","##     ","##     ","##     ","##     ","##     ","###### ","       "],
'M': ["##   ##","### ###","#######","## # ##","##   ##","##   ##","##   ##","       "],
'N': ["##  ## ","### ## ","###### ","## ### ","##  ## ","##  ## ","##  ## ","       "],
'O': [" ####  ","##  ## ","##  ## ","##  ## ","##  ## ","##  ## "," ####  ","       "],
'P': ["#####  ","##  ## ","##  ## ","#####  ","##     ","##     ","##     ","       "],
'Q': [" ####  ","##  ## ","##  ## ","##  ## ","##  ## ","## ##  "," ## ## ","       "],
'R': ["#####  ","##  ## ","##  ## ","#####  ","####   ","## ##  ","##  ## ","       "],
'S': [" ####  ","##  ## ","##     "," ####  ","    ## ","##  ## "," ####  ","       "],
'T': ["###### ","  ##   ","  ##   ","  ##   ","  ##   ","  ##   ","  ##   ","       "],
'U': ["##  ## ","##  ## ","##  ## ","##  ## ","##  ## ","##  ## "," ####  ","       "],
'V': ["##  ## ","##  ## ","##  ## ","##  ## ","##  ## "," ####  ","  ##   ","       "],
'W': ["##   ##","##   ##","##   ##","## # ##","#######","### ###","##   ##","       "],
'X': ["##  ## ","##  ## "," ####  ","  ##   "," ####  ","##  ## ","##  ## ","       "],
'Y': ["##  ## ","##  ## ","##  ## "," ####  ","  ##   ","  ##   ","  ##   ","       "],
'Z': ["###### ","    ## ","   ##  ","  ##   "," ##    ","##     ","###### ","       "],
'a': ["       ","       "," ####  ","    ## "," ##### ","##  ## "," ##### ","       "],
'b': ["##     ","##     ","#####  ","##  ## ","##  ## ","##  ## ","#####  ","       "],
'c': ["       ","       "," ####  ","##     ","##     ","##  ## "," ####  ","       "],
'd': ["    ## ","    ## "," ##### ","##  ## ","##  ## ","##  ## "," ##### ","       "],
'e': ["       ","       "," ####  ","##  ## ","###### ","##     "," ####  ","       "],
'f': ["  ###  "," ##    ","#####  "," ##    "," ##    "," ##    "," ##    ","       "],
'g': ["       ","       "," ##### ","##  ## ","##  ## "," ##### ","    ## "," ####  "],
'h': ["##     ","##     ","#####  ","##  ## ","##  ## ","##  ## ","##  ## ","       "],
'i': ["  ##   ","       "," ###   ","  ##   ","  ##   ","  ##   "," ####  ","       "],
'j': ["   ##  ","       ","  ###  ","   ##  ","   ##  ","   ##  ","## ##  "," ###   "],
'k': ["##     ","##     ","## ##  ","####   ","###    ","####   ","## ##  ","       "],
'l': [" ###   ","  ##   ","  ##   ","  ##   ","  ##   ","  ##   "," ####  ","       "],
'm': ["       ","       ","## ##  ","#######","## # ##","## # ##","## # ##","       "],
'n': ["       ","       ","#####  ","##  ## ","##  ## ","##  ## ","##  ## ","       "],
'o': ["       ","       "," ####  ","##  ## ","##  ## ","##  ## "," ####  ","       "],
'p': ["       ","       ","#####  ","##  ## ","##  ## ","#####  ","##     ","##     "],
'q': ["       ","       "," ##### ","##  ## ","##  ## "," ##### ","    ## ","    ## "],
'r': ["       ","       ","## ### ","###    ","##     ","##     ","##     ","       "],
's': ["       ","       "," ##### ","##     "," ####  ","    ## ","#####  ","       "],
't': [" ##    "," ##    ","#####  "," ##    "," ##    "," ##    ","  ###  ","       "],
'u': ["       ","       ","##  ## ","##  ## ","##  ## ","##  ## "," ##### ","       "],
'v': ["       ","       ","##  ## ","##  ## ","##  ## "," ####  ","  ##   ","       "],
'w': ["       ","       ","##   ##","## # ##","## # ##","#######"," ## ## ","       "],
'x': ["       ","       ","##  ## "," ####  ","  ##   "," ####  ","##  ## ","       "],
'y': ["       ","       ","##  ## ","##  ## ","##  ## "," ##### ","    ## "," ####  "],
'z': ["       ","       ","###### ","   ##  ","  ##   "," ##    ","###### ","       "],
}


def glyph_bytes(ch: str) -> list[int]:
    rows = F.get(ch, F[' '])
    out = []
    for row in rows:
        row = row.ljust(7)
        b = 0
        for x in range(7):
            if row[x] == '#':
                b |= 1 << x
        out.append(b)
    return out


# ---------------------------------------------------------------------------
# Logotype: "APPLETINI ONE" rendered 3x from the font, then smoothed by
# filling pixels that sit in a diagonal step (softens the staircase).
# ---------------------------------------------------------------------------
def render_logo() -> list[list[int]]:
    text = "APPLETINI ONE"
    scale = 3
    wide = len(text) * 7 * scale          # 273 px
    tall = 8 * scale                      # 24 px
    grid = [[0] * wide for _ in range(tall)]
    for ci, ch in enumerate(text):
        g = F.get(ch, F[' '])
        for gy in range(8):
            for gx in range(7):
                if gx < len(g[gy]) and g[gy][gx] == '#':
                    for sy in range(scale):
                        for sx in range(scale):
                            grid[gy*scale+sy][ci*7*scale + gx*scale + sx] = 1
    # diagonal smoothing: light up a dark pixel whose NW+SE or NE+SW
    # neighbors are lit (one pass keeps corners crisp)
    snap = [row[:] for row in grid]
    for y in range(1, tall-1):
        for x in range(1, wide-1):
            if snap[y][x]:
                continue
            nw, ne = snap[y-1][x-1], snap[y-1][x+1]
            sw, se = snap[y+1][x-1], snap[y+1][x+1]
            n, s = snap[y-1][x], snap[y+1][x]
            w, e = snap[y][x-1], snap[y][x+1]
            if (nw and se and (n or w or s or e)) or \
               (ne and sw and (n or e or s or w)):
                grid[y][x] = 1
    # pack into 40-byte HGR rows, centered (offset (280-273)//2 = 3).
    # "ONE" (chars 10..12) renders orange: HGR orange needs the palette
    # bit set and lit pixels only in odd screen columns. The space at
    # char 9 leaves the palette-bit boundary byte free of white pixels.
    one_grid_x = 10 * 7 * scale
    rows = []
    xoff = (280 - wide) // 2
    one_first_byte = (one_grid_x + xoff) // 7
    for y in range(tall):
        row = [0] * 40
        for x in range(wide):
            if grid[y][x]:
                px = x + xoff
                if x >= one_grid_x and (px & 1) == 0:
                    continue
                row[px // 7] |= 1 << (px % 7)
        for b in range(one_first_byte, 40):
            row[b] |= 0x80
        rows.append(row)
    return rows


def hgr_row_addr(y: int) -> int:
    return 0x2000 + (y & 7) * 0x400 + ((y >> 3) & 7) * 0x80 + (y >> 6) * 0x28


def main() -> None:
    lines = [
        "; launcher_assets.a65 -- GENERATED by scripts/gen_hgr_assets.py.",
        "; Do not edit by hand; edit the generator and re-run.",
        "",
        "; 7x8 font, ASCII 32..127, 8 bytes per glyph, bit0 = left pixel.",
        "font7x8:",
    ]
    for code in range(32, 128):
        ch = chr(code)
        bs = glyph_bytes(ch)
        pretty = ch if 32 < code < 127 else f"chr{code}"
        lines.append("    !byte " + ", ".join(f"${b:02X}" for b in bs) +
                     f"   ; {pretty}")

    logo = render_logo()
    lines += [
        "",
        f"LOGO_ROWS = {len(logo)}",
        "logo_data:",
    ]
    for row in logo:
        lines.append("    !byte " + ", ".join(f"${b:02X}" for b in row))

    lines += ["", "; HGR line base addresses, 192 entries", "hgr_lo:"]
    addrs = [hgr_row_addr(y) for y in range(192)]
    for i in range(0, 192, 16):
        lines.append("    !byte " +
                     ", ".join(f"${a & 0xFF:02X}" for a in addrs[i:i+16]))
    lines.append("hgr_hi:")
    for i in range(0, 192, 16):
        lines.append("    !byte " +
                     ", ".join(f"${a >> 8:02X}" for a in addrs[i:i+16]))
    lines.append("")

    OUT.write_text("\n".join(lines), encoding="utf-8")
    print(f"wrote {OUT} ({len(logo)} logo rows)")


if __name__ == "__main__":
    main()
