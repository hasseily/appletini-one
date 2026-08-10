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


def test_per_switch_phase_lookahead_is_selective_and_ordered() -> None:
    renderer = RENDERER_C.read_text(encoding="utf-8")
    renderer_h = RENDERER_H.read_text(encoding="utf-8")
    egress = EGRESS_C.read_text(encoding="utf-8")
    egress_h = EGRESS_H.read_text(encoding="utf-8")

    vtw_start = renderer.index("#define VTW_PHASE_SW_MASK")
    tuning_start = renderer.index("#define PHASE_DELAY_1", vtw_start)
    mask_end = renderer.index("static uint8_t  s_vtw_1mhz_active", tuning_start)
    vtw_mask = renderer[vtw_start:tuning_start]
    tuning = renderer[tuning_start:mask_end]
    for switch in (
        "ACE_SWB_80STORE_BIT",
        "ACE_SWB_TEXT_BIT",
        "ACE_SWB_MIXED_BIT",
        "ACE_SWB_PAGE2_BIT",
        "ACE_SWB_HIRES_BIT",
        "ACE_SWB_80COL_BIT",
        "ACE_SWB_DHIRES_BIT",
    ):
        require(switch in vtw_mask, f"vTW normalization must include {switch}")
    require(
        "ACE_SWB_ALTCHARSET_BIT" not in vtw_mask,
        "ALTCHAR must bypass the vTW-only normalization",
    )
    require(
        "#define PHASE_C050_TEXT       PHASE_NATIVE" in tuning and
        "#define PHASE_C052_MIXED      PHASE_DELAY_1" in tuning and
        "#define PHASE_C00D_80COL      PHASE_NATIVE" in tuning and
        "#define PHASE_C05E_DHIRES     PHASE_NATIVE" in tuning and
        "#define PHASE_C00F_ALTCHARSET PHASE_NATIVE" in tuning,
        "only C052 MIXED must retain a one-cycle scanner delay",
    )
    require(
        "#define PHASE_SWITCH_MASK_AT(value)" in tuning and
        "PHASE_SWITCH_MASK_AT(PHASE_DELAY_1)" in tuning and
        "PHASE_SWITCH_MASK_AT(PHASE_ADVANCE_1)" in tuning and
        "PHASE_SWITCH_MASK_AT(PHASE_ADVANCE_2)" in tuning,
        "phase masks must derive from the per-switch tuning table",
    )
    require(
        "phase_records_are_consecutive(s_phase_prev_record, rec)" in renderer and
        "phase_replace_bits(corrected, baseline1" in renderer and
        "phase_replace_bits(corrected, raw2" in renderer,
        "phase correction must use guarded prior, next, and next+2 states",
    )
    require(
        "void apple_cycle_renderer_set_vtw_1mhz(uint8_t active);" in renderer_h,
        "renderer API must gate vTW normalization on effective 1 MHz operation",
    )
    require(
        "apple_cycle_renderer_on_record_lookahead" in renderer_h and
        "apple_cycle_renderer_on_record_lookahead" in egress_h,
        "egress must pass two future switch snapshots to the renderer",
    )

    poll = egress[egress.index("void apple_cycle_egress_poll(void)"):]
    fill = poll.index("s_lookahead_batch[batch_count++] = ACE_RING_SLOT(batch_scan)")
    peek = poll.index("lookahead_ready = egress_find_phase_lookahead(")
    shadow_main = poll.index("g_main_bank[a & 0xFFFFU] = d;")
    shadow_aux = poll.index("g_aux_bank[a & 0xFFFFU] = d;")
    dispatch = poll.index("apple_cycle_renderer_on_record_lookahead(rec, next1, next2)")
    require(
        fill < peek < shadow_main and peek < shadow_aux and
        shadow_main < dispatch and shadow_aux < dispatch,
        "egress must cache records, peek switches, apply current memory, then dispatch",
    )
    require(
        "Future records never update the video shadow here." in egress and
        "egress_find_phase_lookahead(const uint64_t *records" in egress,
        "lookahead must document that future pixel data stays hidden",
    )
    helper = egress[egress.index("static int egress_find_phase_lookahead"):
                    egress.index("/* --- Poll", egress.index(
                        "static int egress_find_phase_lookahead"))]
    require(
        "ACE_RING_SLOT" not in helper,
        "per-cycle phase lookahead must never reread non-cacheable DDR",
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
        test_per_switch_phase_lookahead_is_selective_and_ordered,
        test_vtw_phase_gate_reuses_cpu1_status_poll,
    ]
    for test in tests:
        test()
        print(f"PASS {test.__name__}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
