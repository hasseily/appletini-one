#!/usr/bin/env python3
"""Checks for the Apple cycle-capture to scanner phase bridge."""

from __future__ import annotations

from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
RENDERER_C = REPO_ROOT / "ps_sources" / "frontend" / "apple_cycle_renderer.c"
RENDERER_H = REPO_ROOT / "ps_sources" / "frontend" / "apple_cycle_renderer.h"
EGRESS_C = REPO_ROOT / "ps_sources" / "frontend" / "apple_cycle_egress.c"
EGRESS_H = REPO_ROOT / "ps_sources" / "frontend" / "apple_cycle_egress.h"
CORE1_C = REPO_ROOT / "ps_sources" / "frontend_core1" / "main.c"
CARD_REGS_H = REPO_ROOT / "ps_sources" / "frontend" / "card_control_regs.h"
APPLE_TOP = REPO_ROOT / "hdl" / "apple" / "apple_top.sv"
VTW_CORE = REPO_ROOT / "hdl" / "apple" / "vtw_core_top.sv"
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


def test_vtw_phase_lookahead_is_selective_and_ordered() -> None:
    renderer = RENDERER_C.read_text(encoding="utf-8")
    renderer_h = RENDERER_H.read_text(encoding="utf-8")
    egress = EGRESS_C.read_text(encoding="utf-8")
    egress_h = EGRESS_H.read_text(encoding="utf-8")

    mask_start = renderer.index("#define VTW_PHASE_SW_MASK")
    mask_end = renderer.index("static uint8_t  s_vtw_1mhz_active", mask_start)
    mask = renderer[mask_start:mask_end]
    for switch in (
        "ACE_SWB_80STORE_BIT",
        "ACE_SWB_TEXT_BIT",
        "ACE_SWB_MIXED_BIT",
        "ACE_SWB_PAGE2_BIT",
        "ACE_SWB_HIRES_BIT",
        "ACE_SWB_80COL_BIT",
        "ACE_SWB_DHIRES_BIT",
    ):
        require(switch in mask, f"vTW lookahead mask must include {switch}")
    require(
        "ACE_SWB_ALTCHARSET_BIT" not in mask,
        "ALTCHAR must retain its hardware-validated physical timing",
    )
    require(
        "void apple_cycle_renderer_on_next_record(uint64_t rec)" in renderer and
        "vtw_records_are_consecutive(pending, rec)" in renderer and
        "vtw_record_with_advanced_video_state(pending, rec)" in renderer,
        "vTW correction must use guarded one-cycle lookahead",
    )
    require(
        "void apple_cycle_renderer_set_vtw_1mhz(uint8_t active);" in renderer_h,
        "renderer API must gate lookahead on effective vTW 1 MHz operation",
    )
    require(
        "__attribute__((weak)) void apple_cycle_renderer_on_next_record(uint64_t rec);" in egress_h,
        "egress-only builds must be able to omit the renderer pre-record hook",
    )

    poll = egress[egress.index("void apple_cycle_egress_poll(void)"):]
    pre = poll.index("apple_cycle_renderer_on_next_record(rec)")
    shadow_main = poll.index("g_main_bank[a & 0xFFFFU] = d;")
    shadow_aux = poll.index("g_aux_bank[a & 0xFFFFU] = d;")
    require(
        pre < shadow_main and pre < shadow_aux,
        "held vTW cycle must render before next-cycle shadow writes are applied",
    )


def test_vtw_phase_gate_reuses_cpu1_status_poll() -> None:
    core = VTW_CORE.read_text(encoding="utf-8")
    top = APPLE_TOP.read_text(encoding="utf-8")
    core1 = CORE1_C.read_text(encoding="utf-8")
    regs = CARD_REGS_H.read_text(encoding="utf-8")

    require(
        "video_phase_1mhz    <= 1'b0;" in core and
        "video_phase_1mhz <=" in core and
        "core_active && bus_owned && (eff_mode == SPEED_1MHZ)" in core,
        "registered PL phase gate must follow effective vTW 1 MHz ownership",
    )
    require(
        "vtw_video_phase_1mhz" in top and
        "CARD_CTRL_REG_APPLE_RESET_STATUS" in top,
        "CPU1's existing status word must expose the PL phase gate",
    )
    require(
        "#define CARD_CTRL_APPLE_RESET_VTW_1MHZ_BIT     (1UL << 9)" in regs and
        "apple_cycle_renderer_set_vtw_1mhz(" in core1 and
        "CARD_CTRL_APPLE_RESET_VTW_1MHZ_BIT" in core1,
        "CPU1 must drive renderer lookahead from the existing status poll",
    )


def main() -> int:
    tests = [
        test_renderer_has_explicit_capture_phase_offset,
        test_raw_frame_edges_remain_unshifted,
        test_vtw_phase_lookahead_is_selective_and_ordered,
        test_vtw_phase_gate_reuses_cpu1_status_poll,
    ]
    for test in tests:
        test()
        print(f"PASS {test.__name__}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
