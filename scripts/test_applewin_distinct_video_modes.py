#!/usr/bin/env python3
"""AppleWin regression checks for distinct Idealized vs RGB rendering paths."""

from __future__ import annotations

from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
RENDERER_C = REPO_ROOT / "ps_sources" / "frontend" / "apple_cycle_renderer.c"
IMAGE_VERSIONS_H = REPO_ROOT / "ps_sources" / "image_versions.h"

BASE = [
    "#000000", "#930B7C", "#1F35D3", "#BB36FF",
    "#00760C", "#7E7E7E", "#07A8E1", "#9DACFF",
    "#624C00", "#F9561D", "#7E7E7E", "#FF81EC",
    "#43C800", "#DCCD16", "#5DF785", "#FFFFFF",
]

AW_HGR_BLACK = 0
AW_HGR_WHITE = 1
AW_HGR_BLUE = 2
AW_HGR_ORANGE = 3
AW_HGR_GREEN = 4
AW_HGR_VIOLET = 5
AW_HGR_GREY1 = 6
AW_BLACK = 12

HIRES_TO_PAL = [AW_HGR_VIOLET, AW_HGR_BLUE, AW_HGR_GREEN,
                AW_HGR_ORANGE, AW_HGR_BLACK, AW_HGR_WHITE]
DOUBLE_HIRES_BASE = [0, 2, 4, 6, 8, 10, 12, 14, 1, 3, 5, 7, 9, 11, 13, 15]


class TestFailure(Exception):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise TestFailure(message)


def palette(index: int) -> str:
    if index >= AW_BLACK:
        return BASE[index - AW_BLACK]
    return {
        AW_HGR_BLACK: BASE[0],
        AW_HGR_WHITE: BASE[15],
        AW_HGR_BLUE: BASE[6],
        AW_HGR_ORANGE: BASE[9],
        AW_HGR_GREEN: BASE[12],
        AW_HGR_VIOLET: BASE[3],
        AW_HGR_GREY1: "#808080",
    }[index]


def hgr_idealized(prev_byte: int, this_byte: int, next_byte: int, x: int) -> list[str]:
    source = [AW_HGR_BLACK] * 32
    column = ((prev_byte & 0xE0) >> 3) | (next_byte & 0x03)
    prev_high = 1 if column >= 16 else 0
    pixels = [0] * 11
    pixels[0] = column & 4
    pixels[1] = column & 8
    pixels[9] = column & 1
    pixels[10] = column & 2
    mask = 1
    for i in range(2, 9):
        pixels[i] = 1 if (this_byte & mask) else 0
        mask <<= 1

    curr_high = (this_byte >> 7) & 1
    if curr_high:
        if pixels[1]:
            if pixels[2] or pixels[0]:
                source[0] = source[16] = AW_HGR_WHITE
            else:
                source[0] = AW_HGR_BLACK if not prev_high else AW_HGR_ORANGE
                source[16] = AW_HGR_BLUE
        elif pixels[0] and pixels[2]:
            source[0] = AW_HGR_BLUE
            source[16] = AW_HGR_ORANGE

    out_x = curr_high
    for odd in range(2):
        if odd:
            out_x = 16 + curr_high
        for i in range(2, 9):
            color = 4
            if pixels[i]:
                color = 5
                if not (pixels[i - 1] or pixels[i + 1]):
                    color = ((odd ^ (i & 1)) << 1) | curr_high
            elif pixels[i - 1] and pixels[i + 1]:
                color = ((odd ^ (not (i & 1))) << 1) | curr_high
            source[out_x] = source[out_x + 1] = HIRES_TO_PAL[color]
            out_x += 2

    start = (x & 1) * 16
    return [palette(source[start + i]) for i in range(14)]


def hgr_rgb(prev_byte: int, byte2: int, byte3: int, byte4: int, x: int) -> list[str]:
    dword = ((prev_byte & 0x7F) |
             ((byte2 & 0x7F) << 7) |
             ((byte3 & 0x7F) << 14) |
             ((byte4 & 0x7F) << 21))
    colors = []
    tmp = dword >> 7
    offset = bool(byte2 & 0x80)
    for i in range(14):
        if i == 7:
            offset = bool(byte3 & 0x80)
        color = tmp & 0x3
        colors.append(palette(1 + color if offset else 6 - color))
        if i & 1:
            tmp >>= 2

    bw = [palette(AW_HGR_BLACK), palette(AW_HGR_WHITE)]
    if x & 1:
        dword >>= 7
        start = 7
    else:
        start = 0

    out = []
    for i in range(start, start + 7):
        if ((dword & 0x01C0) == 0x0140) or ((dword & 0x01C0) == 0x0080):
            color = colors[i]
        else:
            color = bw[1 if (dword & 0x0080) else 0]
        out.extend([color, color])
        dword >>= 1
    return out


def dhires_source_base(column: int, byteval: int, x: int) -> str:
    color = [0] * 10
    pattern = byteval | (column << 8)
    for pixel in range(1, 15):
        if not (pattern & (1 << pixel)):
            continue
        pixelcolor = 1 << ((pixel - 3) & 3)
        if 5 <= pixel < 15 and (pattern & (0x7 << (pixel - 4))):
            color[pixel - 5] |= pixelcolor
        if 4 <= pixel < 14 and (pattern & (0xF << (pixel - 4))):
            color[pixel - 4] |= pixelcolor
        if 3 <= pixel < 13:
            color[pixel - 3] |= pixelcolor
        if 2 <= pixel < 12 and (pattern & (0xF << (pixel + 1))):
            color[pixel - 2] |= pixelcolor
        if 1 <= pixel < 11 and (pattern & (0x7 << (pixel + 2))):
            color[pixel - 1] |= pixelcolor
    return BASE[DOUBLE_HIRES_BASE[color[x] & 0x0F]]


def dhires_idealized(prev_main: int, aux: int, main: int, next_aux: int, x: int) -> list[str]:
    xpixel = x * 14
    dword = ((prev_main & 0x70) |
             ((aux & 0x7F) << 7) |
             ((main & 0x7F) << 14) |
             ((next_aux & 0x07) << 21))
    out = []
    for pixel in (0, 7):
        color = (xpixel + pixel) & 3
        value = dword >> (4 + pixel - color)
        byteval = value & 0xFF
        column = (value >> 8) & 0xFF
        out.extend(dhires_source_base(column, byteval, color + i) for i in range(7))
    return out


def dhires_rgb(aux0: int, main0: int, aux1: int, main1: int, x: int) -> list[str]:
    dword = ((aux0 & 0x7F) |
             ((main0 & 0x7F) << 7) |
             ((aux1 & 0x7F) << 14) |
             ((main1 & 0x7F) << 21))
    colors = []
    for _ in range(7):
        bits = dword & 0x0F
        colors.append(BASE[((bits & 7) << 1) | ((bits & 8) >> 3)])
        dword >>= 4
    cells = [0,0,0,0, 1,1,1,1, 2,2,2,2, 3,3] if (x & 1) == 0 else \
            [3,3, 4,4,4,4, 5,5,5,5, 6,6,6,6]
    return [colors[cell] for cell in cells]


def test_source_wires_distinct_paths() -> None:
    source = RENDERER_C.read_text(encoding="utf-8")
    for token in (
        "step_hgr_idealized", "step_dhgr_idealized",
        "step_hgr_rgb", "step_dhgr_rgb",
        "step_lores_crisp", "step_dlores_crisp",
        "render_applewin_crisp_color",
        "!render_applewin_crisp_color()",
        "aw_rol_nib(auxval >> 4)",
        "s_render_color_mode == APPLE_VIDEO_COLOR_RGB",
    ):
        require(token in source, f"renderer is missing AppleWin distinct path token: {token}")


def test_dhgr_idealized_uses_precomputed_lookup() -> None:
    source = RENDERER_C.read_text(encoding="utf-8")

    for token in (
        "AW_DHIRES_LOOKUP_WIDTH = 10",
        "s_aw_dhires_lookup[AW_DHIRES_LOOKUP_BYTES]",
        "aw_init_crisp_lookup_tables();",
        "aw_build_dhires_lookup_row",
        "const uint8_t *src = &s_aw_dhires_lookup[offs]",
    ):
        require(token in source, f"DHGR Idealized is missing precomputed lookup token: {token}")


def test_applewin_samples_are_distinct() -> None:
    hgr_i = hgr_idealized(0x00, 0x81, 0x00, 0)
    hgr_r = hgr_rgb(0x00, 0x81, 0x00, 0x00, 0)
    dhgr_i = dhires_idealized(0x20, 0x12, 0x34, 0x05, 8)
    dhgr_r = dhires_rgb(0x12, 0x34, 0x05, 0x66, 8)

    require(hgr_i != hgr_r, "AppleWin HGR Idealized and RGB samples must differ")
    require(dhgr_i != dhgr_r, "AppleWin DHGR Idealized and RGB samples must differ")

    print("AppleWin HGR Idealized:", " ".join(hgr_i))
    print("AppleWin HGR RGB      :", " ".join(hgr_r))
    print("AppleWin DHGR Idealized:", " ".join(dhgr_i))
    print("AppleWin DHGR RGB      :", " ".join(dhgr_r))


def main() -> int:
    tests = [
        test_source_wires_distinct_paths,
        test_dhgr_idealized_uses_precomputed_lookup,
        test_applewin_samples_are_distinct,
    ]
    for test in tests:
        test()
        print(f"PASS {test.__name__}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
