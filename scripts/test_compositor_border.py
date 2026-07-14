#!/usr/bin/env python3
"""Source regressions for the legacy-video border-ring compositor path."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
LAYOUT = ROOT / "ps_sources" / "frontend" / "compositor_layout.h"
COMPOSITOR = ROOT / "ps_sources" / "frontend" / "compositor.c"
HANDOFF_C = ROOT / "ps_sources" / "frontend" / "apple_fb_handoff.c"
HANDOFF_H = ROOT / "ps_sources" / "frontend" / "apple_fb_handoff.h"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def test_layout() -> None:
    layout = LAYOUT.read_text(encoding="utf-8")

    expected = (
        "#define COMP_APPLE_WIDTH               560u",
        "#define COMP_APPLE_HEIGHT              192u",
        "#define COMP_APPLE_BORDER_H_CYCLES     2u",
        "#define COMP_APPLE_BORDER_V_LINES      16u",
        "#define COMP_BORDER_X_OFF    344u",
        "#define COMP_BORDER_Y_OFF    92u",
        "#define COMP_BORDER_WIDTH    1232u",
        "#define COMP_BORDER_HEIGHT   896u",
        "#define COMP_SUBWIN_X_OFF    400u",
        "#define COMP_SUBWIN_Y_OFF    156u",
    )
    for text in expected:
        require(text in layout, f"missing layout contract: {text}")

    require(344 + 1232 <= 1920 and 92 + 896 <= 1080,
            "border ring must fit the 1080p output")
    require(344 + 28 * 2 == 400 and 92 + 16 * 4 == 156,
            "active image must stay at its existing output origin")


def test_frame_coherent_color() -> None:
    header = HANDOFF_H.read_text(encoding="utf-8")
    source = HANDOFF_C.read_text(encoding="utf-8")

    require("void apple_fb_writer_publish_frame(uint32_t display_mode, uint8_t border_color);" in header and
            "uint8_t apple_fb_reader_border_color(void);" in header,
            "handoff API must publish and claim a frame-coherent border color")
    require("#define PUBLISHED_BORDER_COLOR_SHIFT 9u" in source and
            "handoff_published_border_color(published)" in source and
            "s_reader_border_color = handoff_published_border_color(published);" in source,
            "border nibble must share the atomic published-frame word")


def test_blit_and_flood_gating() -> None:
    source = COMPOSITOR.read_text(encoding="utf-8")
    tick = source[source.index("int compositor_tick(void)"):]

    require("src_w = (int)COMP_APPLE_VISIBLE_WIDTH;" in source and
            "src_h = (int)COMP_APPLE_VISIBLE_HEIGHT;" in source and
            "COMP_APPLE_ACTIVE_Y * COMP_APPLE_ROW_PIXELS" in source,
            "border on/off paths must select ring or active source geometry")
    require("static void draw_border_flood(uint16_t *fb," in source and
            source.count("fill_border_flood_rect(") == 5,
            "outer flood must use four rectangles around the ring")
    require("effect_scanline_blank(phase, 4U, scanline_mode)" in source and
            "y + row - (int)COMP_BORDER_Y_OFF" in source and
            "color, scanline_mode" in source,
            "outer flood must continue the ring's scanline phase")
    require("if (s_border_flood != 0u)" in source and
            "fb16_from_bgra32(" in source and
            "apple_video_iigs_border_bgra(border_color))" in source,
            "flood must narrow the sampled frame color to 565 and honor "
            "the scanline setting")
    require("if (suppress_apple)" in tick and
            tick.index("if (suppress_apple)") < tick.index("draw_apple_subwindow(fb)"),
            "menu ownership must suppress the ring and flood with the Apple blit")


def main() -> int:
    test_layout()
    test_frame_coherent_color()
    test_blit_and_flood_gating()
    print("compositor border tests: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
