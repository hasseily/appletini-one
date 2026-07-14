#!/usr/bin/env python3
"""Checks for the Apple cycle-capture to scanner phase bridge."""

from __future__ import annotations

from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
RENDERER_C = REPO_ROOT / "ps_sources" / "frontend" / "apple_cycle_renderer.c"
VIDEO_OUTPUT_H = REPO_ROOT / "ps_sources" / "frontend" / "video_output.h"


class TestFailure(Exception):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise TestFailure(message)


def test_renderer_has_explicit_capture_phase_offset() -> None:
    source = RENDERER_C.read_text(encoding="utf-8")
    header = VIDEO_OUTPUT_H.read_text(encoding="utf-8")

    require(
        "#define APPLE_VIDEO_DEFAULT_CLEAN_PHASE_CYCLES 0" in header,
        "non-PAL renderer must default to the raw VBL-locked capture phase",
    )
    require(
        "#define APPLE_VIDEO_DEFAULT_PAL_PHASE_CYCLES   0" in header,
        "PAL-accurate renderer must keep the raw VBL-locked capture phase",
    )
    require(
        "apple_video_settings_clean_phase_cycles(settings)" in source and
        "s_clean_capture_phase_cycles = clean_phase;" in source,
        "renderer must consume the packed clean-mode phase setting",
    )
    require(
        "capture_to_scanner_phase(line,\n"
        "                             cycle,\n"
        "                             s_clean_capture_phase_cycles,\n"
        "                             &render_line,\n"
        "                             &render_cycle);" in source,
        "non-PAL renderer must translate raw capture coordinates with the clean phase",
    )
    require(
        "if (render_line >= visible_lines)" in source,
        "renderer vblank early-out must use scanner-phase line",
    )
    require(
        "g_nVideoClockVert = (int)render_line;" in source and
        "g_nVideoClockHorz = (int)render_cycle;" in source,
        "AppleWin scanner globals must receive scanner-phase coordinates",
    )


def test_raw_frame_edges_remain_unshifted() -> None:
    source = RENDERER_C.read_text(encoding="utf-8")

    frame_edge = (
        "if (s_prev_valid && line == 0u &&\n"
        "        (s_prev_line >= 200u || (shr_frame_marker && s_render_armed)))"
    )
    require(
        frame_edge in source,
        "frame boundary detection must stay on raw PL timestamps",
    )


def main() -> int:
    tests = [
        test_renderer_has_explicit_capture_phase_offset,
        test_raw_frame_edges_remain_unshifted,
    ]
    for test in tests:
        test()
        print(f"PASS {test.__name__}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
