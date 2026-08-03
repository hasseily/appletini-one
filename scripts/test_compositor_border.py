#!/usr/bin/env python3
"""Source regressions for the legacy-video border-ring compositor path."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
LAYOUT = ROOT / "ps_sources" / "frontend" / "compositor_layout.h"
COMPOSITOR = ROOT / "ps_sources" / "frontend" / "compositor.c"
COMPOSITOR_H = ROOT / "ps_sources" / "frontend" / "compositor.h"
FRONTEND_MAIN = ROOT / "ps_sources" / "frontend" / "main.c"
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
        "#define COMP_SHR_BORDER_H_PIXELS (COMP_SUBWIN_X_OFF - COMP_BORDER_X_OFF)",
        "#define COMP_SHR_BORDER_V_PIXELS (COMP_SUBWIN_Y_OFF - COMP_BORDER_Y_OFF)",
        "#define COMP_SHR_BORDER_X_OFF    (COMP_SUBWIN_SHR_X_OFF - COMP_SHR_BORDER_H_PIXELS)",
        "#define COMP_SHR_BORDER_Y_OFF    (COMP_SUBWIN_SHR_Y_OFF - COMP_SHR_BORDER_V_PIXELS)",
    )
    for text in expected:
        require(text in layout, f"missing layout contract: {text}")

    require(344 + 1232 <= 1920 and 92 + 896 <= 1080,
            "border ring must fit the 1080p output")
    require(344 + 28 * 2 == 400 and 92 + 16 * 4 == 156,
            "active image must stay at its existing output origin")
    shr_border_x = 320 - (400 - 344)
    shr_border_y = 140 - (156 - 92)
    shr_border_w = 1280 + 2 * (400 - 344)
    shr_border_h = 800 + 2 * (156 - 92)
    require((shr_border_x, shr_border_y, shr_border_w, shr_border_h) ==
            (264, 76, 1392, 928),
            "SHR ring must retain the legacy on-screen border thickness")
    require(shr_border_x >= 0 and shr_border_y >= 0 and
            shr_border_x + shr_border_w <= 1920 and
            shr_border_y + shr_border_h <= 1080,
            "expanded SHR ring must fit the output")
    widget = (1168, 110, 360, 28)
    require(344 <= widget[0] and 92 <= widget[1] and
            widget[0] + widget[2] <= 344 + 1232 and
            widget[1] + widget[3] <= 92 + 896,
            "disk widget must fit wholly inside the legacy border")
    require(shr_border_x <= widget[0] and shr_border_y <= widget[1] and
            widget[0] + widget[2] <= shr_border_x + shr_border_w and
            widget[1] + widget[3] <= shr_border_y + shr_border_h,
            "disk widget must fit wholly inside the SHR border")


def test_frame_coherent_color() -> None:
    header = HANDOFF_H.read_text(encoding="utf-8")
    source = HANDOFF_C.read_text(encoding="utf-8")

    require("void apple_fb_writer_publish_frame(uint32_t display_mode, uint8_t border_color);" in header and
            "uint8_t apple_fb_reader_border_color(void);" in header,
            "handoff API must publish and claim a frame-coherent border color")
    require("#define PUBLISHED_BORDER_COLOR_SHIFT 10u" in source and
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
            "static void draw_solid_border_ring(uint16_t *fb," in source and
            source.count("fill_border_rect(") == 9,
            "flood and synthesized SHR ring must each use four rectangles")
    require("effect_scanline_blank(phase, vertical_scale, scanline_mode)" in source and
            "y + row - phase_origin_y" in source and
            "vertical_scale - 1U" in source,
            "border fills must follow the selected legacy or SHR scanline phase")
    require("if (s_border_flood != 0u)" in source and
            "fb16_from_bgra32(" in source and
            "apple_video_iigs_border_bgra(border_color))" in source,
            "flood must narrow the sampled frame color to 565 and honor "
            "the scanline setting")
    require("if (suppress_apple)" in tick and
            tick.index("if (suppress_apple)") < tick.index("draw_apple_subwindow(fb)"),
            "menu ownership must suppress the ring and flood with the Apple blit")


def test_shr_border_surrounds_video() -> None:
    source = COMPOSITOR.read_text(encoding="utf-8")
    frontend = FRONTEND_MAIN.read_text(encoding="utf-8")
    shr = source[
        source.index("if (display_mode == APPLE_FB_DISPLAY_MODE_SHR) {"):
        source.index("if (display_mode == APPLE_FB_DISPLAY_MODE_LEGACY_I) {")
    ]

    require("if (s_border_enabled != 0u)" in shr and
            "draw_solid_border_ring(fb," in shr and
            "COMP_SHR_BORDER_X_OFF" in shr and
            "COMP_SHR_BORDER_Y_OFF" in shr and
            "COMP_SHR_BORDER_WIDTH" in shr and
            "COMP_SHR_BORDER_HEIGHT" in shr,
            "SHR must synthesize a border around its own larger geometry")
    require(shr.index("draw_solid_border_ring(fb,") <
            shr.index("fb16_blit_2x2_scanlines(fb,"),
            "SHR video must land inside, not over, the synthesized ring")
    require("draw_border_flood(fb," in shr and
            "s_scanlines_mode,\n                                  2U);" in shr,
            "SHR flood must use the SHR ring and two-row scanline phase")
    require("COMP_SHR_BORDER_X_OFF" in frontend and
            "COMP_SHR_BORDER_Y_OFF" in frontend and
            "COMP_SHR_BORDER_WIDTH" in frontend and
            "COMP_SHR_BORDER_HEIGHT" in frontend,
            "leaving SHR must restore its complete border footprint")


def test_format_badge_stays_on_active_image() -> None:
    source = COMPOSITOR.read_text(encoding="utf-8")
    legacy = source[
        source.index("const uint32_t *src;", source.index("static int draw_apple_subwindow")):
        source.index("return 1;", source.index("const uint32_t *src;"))
    ]

    require("draw_format_badge(fb,\n"
            "                          (int)COMP_SUBWIN_X_OFF,\n"
            "                          (int)COMP_SUBWIN_Y_OFF,\n"
            "                          (int)COMP_SUBWIN_WIDTH);" in legacy and
            "draw_format_badge(fb, dst_x, dst_y, src_w * 2);" not in legacy,
            "legacy badge must stay on the active image when border state changes")


def test_border_is_below_foreground_overlays() -> None:
    header = COMPOSITOR_H.read_text(encoding="utf-8")
    source = COMPOSITOR.read_text(encoding="utf-8")
    frontend = FRONTEND_MAIN.read_text(encoding="utf-8")
    tick = source[source.index("int compositor_tick(void)"):]
    compose = frontend[
        frontend.index("static int ui_compose_frame("):
        frontend.index("/* Adapter for the compositor's typed-erased callback contract. */")
    ]

    require("COMPOSITOR_UI_PHASE_BASE" in header and
            "COMPOSITOR_UI_PHASE_OVERLAY" in header,
            "compositor UI contract must expose base and foreground phases")
    require(tick.index("COMPOSITOR_UI_PHASE_BASE") <
            tick.index("draw_apple_subwindow(fb)") <
            tick.index("COMPOSITOR_UI_PHASE_OVERLAY"),
            "Apple border/video must draw after the bezel base and before foreground UI")
    base_return = compose.index("return menu_active;")
    require(compose.index("ui_prepare_static_background(fb, show_bezel);") < base_return and
            compose.index("debug_overlay_draw(fb, &debug_snapshot);") > base_return and
            compose.index("ui_draw_storage_activity(fb, s);") > base_return and
            compose.index("screenshot_service_draw_overlay(fb);") > base_return,
            "debug, storage, and status overlays must be confined to the post-Apple phase")


def main() -> int:
    test_layout()
    test_frame_coherent_color()
    test_blit_and_flood_gating()
    test_shr_border_surrounds_video()
    test_format_badge_stays_on_active_image()
    test_border_is_below_foreground_overlays()
    print("compositor border tests: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
