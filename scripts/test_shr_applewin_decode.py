#!/usr/bin/env python3
"""Reference checks for IIgs/VidHD SHR decode against AppleWin.

AppleWin reference:
https://github.com/AppleWin/AppleWin/blob/master/source/VidHD.cpp
"""

from __future__ import annotations

from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
RENDERER_C = REPO_ROOT / "ps_sources" / "frontend" / "apple_cycle_renderer.c"


class TestFailure(Exception):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise TestFailure(message)


def pack_bgra(r: int, g: int, b: int) -> int:
    return 0xFF000000 | (r << 16) | (g << 8) | b


def applewin_convert_iigs(raw: int) -> int:
    """Match AppleWin's ConvertIIgs2RGB(Color) bitfield interpretation."""
    b = (raw & 0x000F) * 16
    g = ((raw >> 4) & 0x000F) * 16
    r = ((raw >> 8) & 0x000F) * 16
    return pack_bgra(r, g, b)


def make_distinct_palette() -> list[int]:
    return [
        ((i & 0x0F) << 8) | (((15 - i) & 0x0F) << 4) | ((i * 3) & 0x0F)
        for i in range(16)
    ]


def applewin_cell_320(
    a: int,
    palette: list[int],
    *,
    color_fill: bool = False,
    previous_pixel: int = 0,
) -> list[int]:
    pixels: list[int] = []
    prev = previous_pixel

    for _ in range(4):
        byte = a & 0xFF

        pixel1 = (byte >> 4) & 0x0F
        color1 = applewin_convert_iigs(palette[pixel1])
        if color_fill and pixel1 == 0:
            color1 = prev
        pixels.extend([color1, color1])

        pixel2 = byte & 0x0F
        color2 = applewin_convert_iigs(palette[pixel2])
        if color_fill and pixel2 == 0:
            color2 = color1
        pixels.extend([color2, color2])

        prev = pixels[-1]
        a >>= 8

    return pixels


def applewin_cell_640(a: int, palette: list[int]) -> list[int]:
    pixels: list[int] = []

    for _ in range(4):
        byte = a & 0xFF
        pixels.append(applewin_convert_iigs(palette[0x8 + ((byte >> 6) & 0x03)]))
        pixels.append(applewin_convert_iigs(palette[0xC + ((byte >> 4) & 0x03)]))
        pixels.append(applewin_convert_iigs(palette[0x0 + ((byte >> 2) & 0x03)]))
        pixels.append(applewin_convert_iigs(palette[0x4 + (byte & 0x03)]))
        a >>= 8

    return pixels


def write_palette(aux: bytearray, palette_base: int, palette: list[int]) -> None:
    for idx, raw in enumerate(palette):
        addr = palette_base + idx * 2
        aux[addr] = raw & 0xFF
        aux[addr + 1] = (raw >> 8) & 0xFF


def read_palette(aux: bytearray, palette_base: int) -> list[int]:
    return [aux[palette_base + i * 2] | (aux[palette_base + i * 2 + 1] << 8) for i in range(16)]


def render_cell_from_aux(aux: bytearray, y: int, x: int, *, previous_pixel: int = 0) -> list[int]:
    addr = 0x2000 + 160 * y + 4 * x
    a = aux[addr] | (aux[addr + 1] << 8) | (aux[addr + 2] << 16) | (aux[addr + 3] << 24)
    control = aux[0x9D00 + y]
    palette_base = 0x9E00 + ((control & 0x0F) * 32)
    palette = read_palette(aux, palette_base)

    if control & 0x80:
        return applewin_cell_640(a, palette)
    return applewin_cell_320(a, palette, color_fill=bool(control & 0x20), previous_pixel=previous_pixel)


def source_text() -> str:
    return RENDERER_C.read_text(encoding="utf-8")


def test_palette_conversion_matches_applewin() -> None:
    require(
        applewin_convert_iigs(0x0A5C) == 0xFFA050C0,
        "AppleWin IIgs palette conversion should be raw b/g/r nibbles scaled by 16",
    )

    src = source_text()
    require("(raw & 0x000Fu) * 16u" in src, "renderer should decode blue from bits 0..3")
    require("((raw >> 4) & 0x000Fu) * 16u" in src, "renderer should decode green from bits 4..7")
    require("((raw >> 8) & 0x000Fu) * 16u" in src, "renderer should decode red from bits 8..11")
    require("return shr_apply_c029_bw(shr_pack_bgra(r, g, b));" in src,
            "renderer should emit the decoded IIgs RGB value with the C029 B/W override applied")


def test_320_mode_cell_matches_applewin_nibble_order() -> None:
    palette = make_distinct_palette()
    a = 0xF0E11A2B
    expected_indices = [2, 2, 11, 11, 1, 1, 10, 10, 14, 14, 1, 1, 15, 15, 0, 0]
    expected = [applewin_convert_iigs(palette[i]) for i in expected_indices]

    require(applewin_cell_320(a, palette) == expected, "320 SHR should consume each byte high nibble first")
    require(any(pixel & 0x00FFFFFF for pixel in expected), "320 SHR reference vector should render visible pixels")

    src = source_text()
    require("const uint8_t pixel1 = (uint8_t)((byte >> 4) & 0x0Fu);" in src, "320 renderer should read high nibble first")
    require("const uint8_t pixel2 = (uint8_t)(byte & 0x0Fu);" in src, "320 renderer should read low nibble second")
    require("if (color_fill && pixel1 == 0u)" in src, "320 renderer should implement AppleWin color-fill pixel1 rule")
    require("if (color_fill && pixel2 == 0u) color2 = color1;" in src, "320 renderer should implement AppleWin color-fill pixel2 rule")
    require("a >>= 8;" in src, "320 renderer should advance the 4-byte SHR cell least-significant byte first")


def test_320_color_fill_matches_applewin_for_nonzero_previous_pixel() -> None:
    palette = make_distinct_palette()
    previous = 0xFF112233
    a = 0x00000001
    pixels = applewin_cell_320(a, palette, color_fill=True, previous_pixel=previous)

    require(pixels[0:2] == [previous, previous], "320 color-fill index 0 should reuse previous pixel")
    require(
        pixels[2:4] == [applewin_convert_iigs(palette[1])] * 2,
        "320 nonzero low nibble should use the selected palette color",
    )
    require(
        pixels[4:] == [applewin_convert_iigs(palette[1])] * 12,
        "320 following zero pixels should carry the last nonzero color forward",
    )

    src = source_text()
    require(
        "color1 = (dst != row0) ? *(dst - 1) : 0u;" in src,
        "local 320 row-start guard should only replace AppleWin's previous-pixel read at x=0",
    )


def test_640_mode_cell_matches_applewin_palette_order() -> None:
    palette = make_distinct_palette()
    a = 0xE41B92FF
    expected_indices: list[int] = []
    aa = a
    for _ in range(4):
        byte = aa & 0xFF
        expected_indices.extend(
            [
                0x8 + ((byte >> 6) & 0x03),
                0xC + ((byte >> 4) & 0x03),
                0x0 + ((byte >> 2) & 0x03),
                0x4 + (byte & 0x03),
            ]
        )
        aa >>= 8

    expected = [applewin_convert_iigs(palette[i]) for i in expected_indices]
    require(applewin_cell_640(a, palette) == expected, "640 SHR should use AppleWin 8/C/0/4 palette order")
    require(any(pixel & 0x00FFFFFF for pixel in expected), "640 SHR reference vector should render visible pixels")

    src = source_text()
    require("const uint8_t pixel1 = (uint8_t)((byte >> 6) & 0x03u);" in src, "640 renderer should read bits 7..6 first")
    require("shr_palette_color(palette_base, (uint8_t)(0x8u + pixel1))" in src, "640 renderer should use palette group 8 for bits 7..6")
    require("const uint8_t pixel2 = (uint8_t)((byte >> 4) & 0x03u);" in src, "640 renderer should read bits 5..4 second")
    require("shr_palette_color(palette_base, (uint8_t)(0xCu + pixel2))" in src, "640 renderer should use palette group C for bits 5..4")
    require("const uint8_t pixel3 = (uint8_t)((byte >> 2) & 0x03u);" in src, "640 renderer should read bits 3..2 third")
    require("shr_palette_color(palette_base, (uint8_t)(0x0u + pixel3))" in src, "640 renderer should use palette group 0 for bits 3..2")
    require("const uint8_t pixel4 = (uint8_t)(byte & 0x03u);" in src, "640 renderer should read bits 1..0 fourth")
    require("shr_palette_color(palette_base, (uint8_t)(0x4u + pixel4))" in src, "640 renderer should use palette group 4 for bits 1..0")


def test_shr_frame_uses_applewin_memory_map_and_line_control() -> None:
    y = 17
    x = 3
    control = 0x80 | 0x05
    palette_base = 0x9E00 + ((control & 0x0F) * 32)
    cell_addr = 0x2000 + 160 * y + 4 * x

    require(cell_addr == 0x2AAC, "SHR byte cell address should be $2000 + 160*y + 4*x")
    require(palette_base == 0x9EA0, "SHR palette base should be $9E00 + palette*32")

    src = source_text()
    require("0x2000u + 160u * y + 4u * x" in src, "renderer should use AppleWin SHR screen byte layout")
    require("const uint8_t control = g_aux_bank[0x9D00u + (uint16_t)y];" in src, "renderer should read the per-line SHR control byte")
    require("0x9E00u + ((uint16_t)(control & 0x0Fu) * 32u)" in src, "renderer should select one of 16 SHR palettes")
    require("const int is_640 = (control & 0x80u) != 0u;" in src, "renderer should use line-control bit 7 for 640 mode")
    require("const int color_fill = (control & 0x20u) != 0u;" in src, "renderer should use line-control bit 5 for color fill")
    require("static void render_shr_frame_full(void)" in src, "renderer should render SHR as a full AUX-shadow frame")
    require("for (uint32_t y = 0u; y < SHR_LOGICAL_HEIGHT; ++y)" in src, "renderer should render 200 logical SHR lines")
    require("for (uint32_t x = 0u; x < 40u; ++x)" in src, "renderer should render 40 SHR byte cells per scanline")
    require("memcpy(row1 + x * 16u, row0 + x * 16u, 16u * sizeof(uint32_t));" in src, "renderer should duplicate each 200-line SHR row vertically")


def test_reference_image_generation_from_aux_memory_is_visible() -> None:
    aux = bytearray(0x10000)
    palette = make_distinct_palette()
    y = 42
    x = 9
    control = 0x80 | 0x04
    palette_base = 0x9E00 + ((control & 0x0F) * 32)
    cell_addr = 0x2000 + 160 * y + 4 * x
    cell_bytes = [0xFF, 0x92, 0x1B, 0xE4]
    a = int.from_bytes(bytes(cell_bytes), "little")

    aux[0x9D00 + y] = control
    write_palette(aux, palette_base, palette)
    aux[cell_addr : cell_addr + 4] = bytes(cell_bytes)

    pixels = render_cell_from_aux(aux, y, x)
    require(pixels == applewin_cell_640(a, palette), "aux-memory SHR generation should match AppleWin 640-cell decode")
    require(len(pixels) == 16, "one 4-byte SHR cell should generate 16 output pixels")
    require(any(pixel & 0x00FFFFFF for pixel in pixels), "nonzero aux SHR data and palette should not render black")


TESTS = [
    test_palette_conversion_matches_applewin,
    test_320_mode_cell_matches_applewin_nibble_order,
    test_320_color_fill_matches_applewin_for_nonzero_previous_pixel,
    test_640_mode_cell_matches_applewin_palette_order,
    test_shr_frame_uses_applewin_memory_map_and_line_control,
    test_reference_image_generation_from_aux_memory_is_visible,
]


def main() -> int:
    for test in TESTS:
        try:
            test()
        except TestFailure as exc:
            print(f"FAIL {test.__name__}: {exc}")
            return 1
        print(f"PASS {test.__name__}")
    print(f"{len(TESTS)} AppleWin SHR reference checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
