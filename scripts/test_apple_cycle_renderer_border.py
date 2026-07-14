#!/usr/bin/env python3
"""Source regressions for cycle-accurate VidHD $C034 border rendering."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RENDERER = ROOT / "ps_sources" / "frontend" / "apple_cycle_renderer.c"
NTSC_H = ROOT / "ps_sources" / "frontend" / "appletini_ntsc.h"
VIDEO_H = ROOT / "ps_sources" / "frontend" / "video_output.h"
PAL_C = ROOT / "ps_sources" / "frontend" / "apple_pal_video_timing.c"
DEMO = ROOT / "software" / "border_demo.a65"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def visible_row(line: int, frame_lines: int) -> int | None:
    if line < 208:
        return line + 16
    if frame_lines - 16 <= line < frame_lines:
        return line - (frame_lines - 16)
    return None


def test_geometry() -> None:
    ntsc = NTSC_H.read_text(encoding="utf-8")
    pal = PAL_C.read_text(encoding="utf-8")

    for text in (
        "#define ATN_BORDER_H_CYCLES             2u",
        "#define ATN_BORDER_H_PIXELS             (ATN_BORDER_H_CYCLES * 14u)",
        "#define ATN_BORDER_V_LINES              16u",
        "#define ATN_SCANNER_MAX_VERT_NTSC         262u",
        "#define ATN_SCANNER_MAX_VERT_PAL          312u",
    ):
        require(text in ntsc, f"missing geometry contract: {text}")
    require("#define PAL_RENDER_SAMPLES       ATN_ACTIVE_WIDTH" in pal,
            "PAL active decode must stay 560 pixels after the slot widens")

    for frame_lines in (262, 312):
        rows = [visible_row(line, frame_lines) for line in range(frame_lines)]
        painted = [row for row in rows if row is not None]
        require(sorted(painted) == list(range(224)),
                f"{frame_lines}-line raster must map each border row once")


def test_palette_and_latch() -> None:
    video = VIDEO_H.read_text(encoding="utf-8")
    renderer = RENDERER.read_text(encoding="utf-8")
    palette = (
        "0x000U, 0xD03U, 0x009U, 0xD2DU,\n"
        "        0x072U, 0x555U, 0x22FU, 0x6AFU,\n"
        "        0x850U, 0xF60U, 0xAAAU, 0xF98U,\n"
        "        0x0D0U, 0xFF0U, 0x4F9U, 0xFFFU"
    )

    require("#define APPLE_VIDEO_IIGS_BORDER_DEFAULT      6U" in video,
            "IIgs border must reset to medium blue")
    require(palette in video, "IIgs 16-color palette must be complete and ordered")
    require("s_vidhd_border_color  = s_border_default_color;" in renderer and
            "apple_video_settings_border_color(settings)" in renderer,
            "Apple reset must restore the configured border color")
    require("case 0xC034U:" in renderer and
            "s_vidhd_border_color = apple_video_iigs_border_color_clamp(data);" in renderer,
            "$C034 writes must update only the low-nibble latch")


def test_cycle_emission_and_handoff() -> None:
    renderer = RENDERER.read_text(encoding="utf-8")

    require("static void emit_border_cycle(uint32_t line, uint32_t cycle)" in renderer,
            "renderer needs a cycle-positioned border emitter")
    require("cycle < ATN_BORDER_H_CYCLES" in renderer and
            "ATN_SCANNER_HORZ_START - ATN_BORDER_H_CYCLES" in renderer and
            "line >= ATN_ACTIVE_HEIGHT" in renderer,
            "border emitter must separate right, left, and border-only regions")
    require("s_frame_end_pending = 1u;" in renderer and
            "line == 0u && cycle < ATN_BORDER_H_CYCLES" in renderer and
            "on_frame_end();\n        on_frame_start();" in renderer,
            "frame publish must wait until wrapped right-border cycles are stored")
    require("restore_left_border(render_line, render_cycle);" in renderer and
            "s_left_border_colors[left_index] = color;" in renderer,
            "AppleWin chroma preroll must not overwrite visible left-border pixels")
    require("apple_fb_writer_publish_frame(s_frame_display_mode," in renderer,
            "published frames must carry their sampled border color")


def test_demo_cycle_budget() -> None:
    demo = DEMO.read_text(encoding="utf-8")
    border_stores = [line.strip() for line in demo.splitlines()
                     if line.strip().startswith("sta BORDER")]

    require("!cpu 6502" in demo and "BORDER   = $C034" in demo,
            "border demo must target the unenhanced 6502 and VidHD register")
    require(len(border_stores) == 7 and
            all(line.startswith("sta BORDER,x") for line in border_stores) and
            "sta BORDER,x        ; 5; dummy read + write leaves speaker unchanged" in demo and
            "ldx #0              ; required by the paired $C034 stores" in demo,
            "every raster write must pair the //e speaker accesses")
    require(demo.count("lda #(16 - .color)  ; 2") == 3 and
            "sta BORDER,x        ; 5; opposite write lands at cycle 32" in demo and
            "half_delay:             ; JSR 6 + body 6 + RTS 6 = 18" in demo,
            "every raster path must switch to 16-color at cycle 32")
    require("jsr line_tail_delay ; 33; total: 65 cycles" in demo and
            "!align 255, 0" in demo and
            "ldx #4              ; 2" in demo and
            "line_tail_delay:        ; JSR 6 + body 21 + RTS 6 = 33" in demo,
            "normal raster line must use the fixed 65-cycle path")
    require("jsr jump_tail_delay ; 30" in demo and
            "jump_tail_delay:        ; JSR 6 + body 18 + RTS 6 = 30" in demo,
            "NTSC must enter the shared blanking tail on a 65-cycle line")
    require("jsr last_tail_delay ; 22" in demo and
            "last_tail_delay:        ; JSR 6 + body 10 + RTS 6 = 22" in demo and
            "jmp (restart_vector); 5; total: 65 cycles" in demo,
            "last scanline must include polling and looping in the 65-cycle budget")
    require("cpy #207" in demo and
            "!for .line, 1, 51" in demo and
            "!for .line, 1, 37" in demo and
            "!for .color, 0, 13" in demo,
            "demo must select exact 262-line NTSC or 312-line PAL frames")


def main() -> int:
    test_geometry()
    test_palette_and_latch()
    test_cycle_emission_and_handoff()
    test_demo_cycle_budget()
    print("apple cycle renderer border tests: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
