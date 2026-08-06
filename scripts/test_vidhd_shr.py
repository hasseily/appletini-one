#!/usr/bin/env python3
"""Source-level regression tests for VidHD/SHR support.

These tests run without Vitis or hardware:

    python scripts/test_vidhd_shr.py
"""

from __future__ import annotations

from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
CAPTURE_PKG_SV = REPO_ROOT / "hdl" / "apple" / "apple_cycle_capture_pkg.sv"
CAPTURE_SV = REPO_ROOT / "hdl" / "apple" / "apple_cycle_capture.sv"
APPLE_TOP_SV = REPO_ROOT / "hdl" / "apple" / "apple_top.sv"
SOFT_SWITCH_MANAGER_SV = REPO_ROOT / "hdl" / "apple" / "soft_switch_manager.sv"
HDL_SOURCES = REPO_ROOT / "hdl" / "hdl_sources.txt"
EGRESS_H = REPO_ROOT / "ps_sources" / "frontend" / "apple_cycle_egress.h"
EGRESS_C = REPO_ROOT / "ps_sources" / "frontend" / "apple_cycle_egress.c"
RENDERER_C = REPO_ROOT / "ps_sources" / "frontend" / "apple_cycle_renderer.c"
HANDOFF_H = REPO_ROOT / "ps_sources" / "frontend" / "apple_fb_handoff.h"
HANDOFF_C = REPO_ROOT / "ps_sources" / "frontend" / "apple_fb_handoff.c"
COMPOSITOR_LAYOUT_H = REPO_ROOT / "ps_sources" / "frontend" / "compositor_layout.h"
COMPOSITOR_C = REPO_ROOT / "ps_sources" / "frontend" / "compositor.c"
FB16_H = REPO_ROOT / "ps_sources" / "lib" / "fb16.h"
FB16_C = REPO_ROOT / "ps_sources" / "lib" / "fb16.c"
FRONTEND_MAIN_C = REPO_ROOT / "ps_sources" / "frontend" / "main.c"
DEBUG_OVERLAY_C = REPO_ROOT / "ps_sources" / "frontend" / "debug_overlay.c"
CARD_CONTROL_REGS_H = REPO_ROOT / "ps_sources" / "frontend" / "card_control_regs.h"
CONFIG_MENU_C = REPO_ROOT / "ps_sources" / "frontend" / "config_menu.c"
CONFIG_MENU_INTERNAL_H = REPO_ROOT / "ps_sources" / "frontend" / "config_menu_internal.h"
CONFIG_MENU_PHASOR_C = REPO_ROOT / "ps_sources" / "frontend" / "config_menu_phasor.c"
IMAGE_VERSIONS_H = REPO_ROOT / "ps_sources" / "image_versions.h"
VIDEO_OUTPUT_TEST = REPO_ROOT / "scripts" / "test_video_output_config_menu.py"


class TestFailure(Exception):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise TestFailure(message)


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def test_record_kind_contract() -> None:
    pkg = read(CAPTURE_PKG_SV)
    header = read(EGRESS_H)

    require("RECORD_KIND_LEGACY         = 3'b000" in pkg and
            "RECORD_KIND_IO_WRITE       = 3'b001" in pkg and
            "RECORD_KIND_SOFTSW_ACCESS  = 3'b010" in pkg,
            "SV record package must reserve distinct I/O-write and soft-switch access record kinds")
    require("logic [2:0]  record_kind;" in pkg,
            "top three AppleCycleRecord bits must be named record_kind")
    require("function automatic AppleCycleRecord pack_io_write_record" in pkg,
            "SV package must provide the canonical I/O-write packer")
    require("{RECORD_KIND_IO_WRITE, apple_addr, data, line_in_frame, cycle_in_line, 21'd0}" in pkg,
            "I/O-write record layout must remain 3+16+8+9+7+21 bits")
    require("function automatic AppleCycleRecord pack_softswitch_access_record" in pkg and
            "pack_softswitch_access_record.record_kind     = RECORD_KIND_SOFTSW_ACCESS;" in pkg and
            "pack_softswitch_access_record.addr_decode     = {8'd0, apple_addr};" in pkg,
            "SV package must provide a soft-switch access record carrying address and soft-switch bits")

    require("#define ACE_RECORD_KIND_LEGACY          0U" in header and
            "#define ACE_RECORD_KIND_IO_WRITE        1U" in header and
            "#define ACE_RECORD_KIND_SOFTSW_ACCESS   2U" in header,
            "C mirror must define the same record kinds")
    require("#define ACE_BIT_RECORD_KIND_LO    61" in header and
            "#define ACE_BIT_IO_ADDR_LO        45" in header and
            "#define ACE_BIT_IO_DATA_LO        37" in header,
            "C mirror must expose I/O-write bit positions")
    require("static inline uint32_t ace_record_kind(uint64_t r)" in header and
            "static inline uint16_t ace_io_addr(uint64_t r)" in header and
            "static inline uint8_t ace_io_data(uint64_t r)" in header,
            "C mirror must expose kind and I/O-write accessors")
    require("static inline uint16_t ace_softswitch_access_addr(uint64_t r)" in header,
            "C mirror must expose the C0xx address from soft-switch access records")


def test_capture_emits_two_records_for_vidhd_io_plus_frame() -> None:
    source = read(CAPTURE_SV)

    require("is_vidhd_register_write" in source and
            "16'hC022" in source and "16'hC029" in source and
            "16'hC034" in source and "16'hC035" in source,
            "capture must recognize the VidHD/IIgs C0xx register writes")
    require("is_video7_an3_access" in source and
            "16'hC05E" in source and "16'hC05F" in source,
            "capture must recognize Video-7 AN3/DHIRES accesses")
    require("assign video7_softswitch_access =\n"
            "        ab_read.data_en &&\n"
            "        is_video7_an3_access(cap_addr);" in source,
            "Video-7 AN3 capture must trigger on reads and writes")
    require("wire [15:0] cap_addr = ab_read.addr;" in source,
            "capture must decode the authoritative PHI0-high address sample")
    require("io_record_din = pack_io_write_record" in source,
            "capture must use the canonical I/O-write record packer")
    require("io_record_din = pack_softswitch_access_record" in source and
            "current_softswitch_bits" in source,
            "capture must emit soft-switch records with same-cycle soft-switch state")
    require("pending_record_valid" in source and
            "pending_record_q     <= apple_record_din;" in source,
            "capture must queue the frame/memory record when an I/O write also occurs")
    require("if (pending_record_valid)" in source and
            "else if (io_push_request)" in source and
            "else\n            record_din = apple_record_din;" in source,
            "capture arbitration must emit pending, then I/O, then normal Apple records")
    require("logic shr_capture_active_q;" in source and
            "wire c029_write_shr_active = (ab_read.data[7:6] == 2'b11);" in source,
            "capture must track the C029 fake-SHR enable bit pattern")
    require("wire shr_frame_marker =" in source and
            "(line_in_frame == 9'd0)" in source and
            "(cycle_in_line == 7'd0)" in source,
            "capture must emit sparse frame markers while fake-SHR is active")
    require("wire capture_frame_en =" in source and
            "(!shr_capture_active_next || shr_frame_marker)" in source and
            "assign apple_push_request = ab_read.data_en && (rule1_valid || capture_frame_en);" in source,
            "fake-SHR must keep memory writes but suppress per-cycle frame records")


def test_shr_capture_uses_aux_shadow_without_m2b0() -> None:
    source = read(SOFT_SWITCH_MANAGER_SV)
    capture = read(CAPTURE_SV)

    require("ab_read.m2b0" not in source,
            "M2B0 must not steer real Apple memory on the Apple //e fake-SHR path")
    require("ss_shr" not in source,
            "soft-switch manager must not gate real bank steering on C029 SHR")
    require("captured AUX $2000-$9FFF shadow" in source,
            "soft-switch manager comment must document the simple AUX-shadow SHR source")
    require("ab_read.m2b0" not in capture and "shr_m2b0_shadow_write" not in capture,
            "capture must not synthesize IIgs M2B0 SHR writes for the Apple //e path")
    require("0x002000-0x009FFF" in capture and
            "((a >= 24'h002000) && (a <= 24'h009FFF))" in capture,
            "capture must mirror main $2000-$9FFF so SHR interlace "
            "can keep their second field in the main bank")
    require("0x012000-0x019FFF" in capture and
            "((a >= 24'h012000) && (a <= 24'h019FFF))" in capture,
            "capture must keep the full AUX $2000-$9FFF SHR window")
    require("0x010400-0x010BFF" in capture and
            "((a >= 24'h010400) && (a <= 24'h010BFF))" in capture and
            "24'h0107FF" not in capture,
            "capture must include AUX text page 2 for DLORES/TEXT80 PAGE2")
    require("in_video_range(cap_addr_decode)" in capture and
            "apple_record_din.addr_decode    = cap_addr_decode;" in capture and
            "cap_addr_decode    = sss.addr_decode_late" in capture,
            "capture records must use the observation decode of the "
            "authoritative address sample")


def test_renderer_tracks_vidhd_register_state() -> None:
    source = read(RENDERER_C)

    require("static void handle_vidhd_io_record(uint64_t rec)" in source,
            "renderer must handle ordered VidHD I/O records")
    require("case 0xC022U:" in source and
            "s_vidhd_screen_color = data;" in source and
            "case 0xC029U:\n        s_vidhd_newvideo = data;" in source and
            "case 0xC034U:" in source and
            "s_vidhd_border_color = color;" in source and
            "case 0xC035U:" in source and
            "s_vidhd_shadow = data;" in source,
            "renderer must track C022, C029, C034, and C035 state")
    require("if (ace_record_kind(rec) == ACE_RECORD_KIND_IO_WRITE) {\n"
            "        handle_vidhd_io_record(rec);\n"
            "        return;\n"
            "    }" in source,
            "renderer dispatch must consume ordered VidHD I/O records")


def test_renderer_implements_video7_auto_white_mono() -> None:
    source = read(RENDERER_C)

    require("static uint8_t s_video7_rgb_flags = 0u;" in source and
            "static uint8_t s_video7_rgb_mode = 0u;" in source and
            "static uint8_t s_video7_an3_sequence = 0u;" in source,
            "renderer must keep the Video-7 sequence and two-bit mode state")
    handler_start = source.find(
        "static void handle_video7_softswitch_record(uint64_t rec)")
    handler_end = source.find("\n}\n", handler_start)
    require(handler_start >= 0 and handler_end > handler_start,
            "renderer must keep the Video-7 soft-switch handler")
    handler = source[handler_start:handler_end]
    require("if ((low & 0xFEu) != 0x5Eu) {\n        return;\n    }" in handler,
            "non-AN3 accesses (the interleaved 80COL writes) must not disturb "
            "the Video-7 sequence")
    require("if (sw_mixed(sw)) {\n        s_video7_an3_sequence = 0u;" in handler,
            "an AN3 access with MIXED on must abort the select sequence")
    require("if (low == 0x5Eu) {" in handler and
            "s_video7_an3_sequence = 1u;" in handler and
            "if (s_video7_an3_sequence == 5u)" in handler,
            "the sequence must start and commit on $C05E -- DHGR enabled at "
            "both ends, matching A2Desktop's five-toggle select")
    require("if (s_video7_an3_sequence == 2u) {" in handler and
            "sw_80col(sw) ? 0u : 0x02u" in handler and
            "sw_80col(sw) ? 0u : 0x01u" in handler,
            "each $C05F rising edge must clock !80COL, first into bit 1 and "
            "second into bit 0 (AppleWin RGBMonitor latch F2,F1)")
    require("const uint8_t video7_mono =\n"
            "        ((shr_now == 0u) &&\n"
            "         (apple_video_settings_video7_auto_mono_enabled(settings) != 0u) &&\n"
            "         (s_video7_rgb_mode == 3u)) ? 1u : 0u;" in source and
            "const uint8_t video7_auto_mono =\n"
            "        ((user_mono == 0u) && (video7_mono != 0u)) ? 1u : 0u;" in source and
            "((user_mono != 0u) || (bw_force != 0u) || (video7_auto_mono != 0u)) ? 1u : 0u" in source and
            "((bw_force != 0u) || (video7_auto_mono != 0u)) ?\n"
            "        APPLE_VIDEO_MONO_WHITE" in source,
            "Video-7 mode 3 must auto-force white mono only when bootmenu mono is off, "
            "the toggle is enabled, and C029 SHR is not active")
    require("const uint8_t video7_mix =\n"
            "        ((shr_now == 0u) &&\n"
            "         (apple_video_settings_dhgr_col140m_enabled(settings) != 0u) &&\n"
            "         (s_video7_rgb_mode == 2u)) ? 1u : 0u;" in source and
            "s_render_dhgr_col140m_enable = video7_mix;" in source,
            "Video-7 mode 2 must enable MIX only when its UI switch is on and SHR is off")
    require("apple_video_settings_color_mode(settings)" in source and
            "s_render_color_mode = apple_video_settings_color_mode(settings);" in source and
            "video7_auto_mono" in source and
            "s_render_color_mode == APPLE_VIDEO_COLOR_RGB && video7_auto_mono" not in source,
            "Video-7 auto-mono must apply to every bootmenu color mode, not only RGB")
    require("s_video7_rgb_flags    = 0u;" in source and
            "s_video7_rgb_mode     = 0u;" in source and
            "s_video7_an3_sequence = 0u;" in source,
            "Apple reset must clear Video-7 RGB state")
    require("if (ace_record_kind(rec) == ACE_RECORD_KIND_SOFTSW_ACCESS) {\n"
            "        handle_video7_softswitch_record(rec);\n"
            "        return;\n"
            "    }" in source,
            "per-record dispatch must consume Video-7 soft-switch records")


def test_renderer_implements_applewin_shr_decode() -> None:
    source = read(RENDERER_C)

    require("#define SHR_WIDTH  640u" in source and
            "#define SHR_HEIGHT 400u" in source and
            "#define SHR_LOGICAL_HEIGHT 200u" in source,
            "renderer must publish AppleWin-style 640x400 SHR frames")
    require("static inline int vidhd_shr_enabled(void)" in source and
            "(s_vidhd_newvideo & 0xC0u) == 0xC0u" in source,
            "SHR must be enabled only when C029 bits 7 and 6 are both set")
    require("static inline int vidhd_bw_forced(void)" in source and
            "(s_vidhd_newvideo & 0x20u) != 0u" in source,
            "C029 bit 5 must force black-and-white output")
    require("static inline uint16_t shr_scanline_addr" in source and
            "0x2000u + 160u * y + 4u * x" in source,
            "SHR scanner address must use 160 bytes per line and four bytes per cycle")
    require("const uint8_t control = s_f_bank[0x9D00u + (uint16_t)y];" in source and
            "(uint16_t)(0x9E00u + ((uint16_t)(control & 0x0Fu) * 32u));"
            in source and
            "shr_eval_field_modes(g_aux_bank);" in source,
            "SHR line control/palette must come from the field bank, "
            "with AUX as the control plane")
    require("static void shr_render_cell_320" in source and
            "if (color_fill && pixel1 == 0u)" in source and
            "color1 = (dst != row0) ? *(dst - 1) : 0u;" in source and
            "if (color_fill && pixel2 == 0u) color2 = color1;" in source,
            "320-mode color fill must match AppleWin's previous-pixel behavior")
    require("static void shr_render_cell_640" in source and
            "shr_palette_color(palette_base, (uint8_t)(0x8u + pixel1))" in source and
            "shr_palette_color(palette_base, (uint8_t)(0xCu + pixel2))" in source and
            "shr_palette_color(palette_base, (uint8_t)(0x0u + pixel3))" in source and
            "shr_palette_color(palette_base, (uint8_t)(0x4u + pixel4))" in source,
            "640-mode palette group mapping must follow IIgs/AppleWin")
    require("return shr_apply_c029_bw(shr_pack_bgra(r, g, b));" in source,
            "SHR palette colors must honor the C029 bit-5 B/W override")
    require("const uint8_t effective_mono =\n"
            "        ((user_mono != 0u) || (bw_force != 0u) || (video7_auto_mono != 0u)) ? 1u : 0u;" in source and
            "const uint8_t mono_color = ((bw_force != 0u) || (video7_auto_mono != 0u)) ?" in source and
            "APPLE_VIDEO_MONO_WHITE : apple_video_settings_mono_color(settings)" in source,
            "C029 bit-5 and Video-7 mode 3 must force monochrome for legacy modes")
    require("static void render_shr_frame_full(void)" in source and
            "for (uint32_t y = 0u; y < SHR_LOGICAL_HEIGHT; ++y)" in source and
            "for (uint32_t x = 0u; x < 40u; ++x)" in source and
            "render_shr_line_to(out, y);" in source and
            "memcpy(out + SHR_WIDTH, out, SHR_WIDTH * sizeof(uint32_t));" in source,
            "renderer must be able to build a full SHR frame directly from AUX shadow")
    require("if (s_frame_display_mode == APPLE_FB_DISPLAY_MODE_SHR) {" in source and
            "if (rebuild != 0u) {" in source and
            "generation != s_shr_settle_generation" in source and
            "render_shr_frame_full();" in source and
            "publish_current_frame();" in source,
            "renderer must settle, render, and publish a changed SHR shadow "
            "at a frame start")
    require("const int shr_frame_marker =\n        shr_active && line == 0u && cycle == 0u;" in source and
            "(s_prev_line >= 200u || (shr_frame_marker && s_render_armed))" in source,
            "renderer must treat sparse SHR markers as frame boundaries")
    require("if (shr_active ||\n"
            "        s_frame_display_mode == APPLE_FB_DISPLAY_MODE_LEGACY_I ||\n"
            "        s_legacy_flip_q != 0u) {" in source and
            "s_records_in_frame++;\n        return;" in source,
            "per-record renderer dispatch must bypass legacy NTSC while SHR, "
            "weave, or flip merge is active")
    require("if (was_shr != vidhd_shr_enabled()) {" in source and
            "s_render_armed = 0;" in source and
            "s_frame_end_pending = 0u;" in source and
            "s_pending_line0_mask = 0u;" in source and
            "apple_pal_video_resync();" in source and
            "g_resync_pending = 1u;" in source,
            "SHR transitions must abandon mixed-geometry frames and resync PAL capture")
    require("const uint8_t synthesized =" in source and
            "s_frame_display_mode == APPLE_FB_DISPLAY_MODE_SHR ||" in source and
            "s_frame_display_mode == APPLE_FB_DISPLAY_MODE_LEGACY_I ||" in source and
            "synthesized ? 0u : apple_pal_video_end_frame();" in source and
            "} else if (s_frame_display_mode == APPLE_FB_DISPLAY_MODE_LEGACY_I) {\n"
            "        if (legacy_shadow_is_settled() != 0u) {\n"
            "            render_legacy_weave_frame_full();\n"
            "            publish_current_frame();\n"
            "        }\n"
            "    } else if (s_legacy_flip_q != 0u) {\n"
            "        if (legacy_shadow_is_settled() != 0u) {\n"
            "            render_legacy_flip_merge_frame_full();\n"
            "            publish_current_frame();\n"
            "        }\n"
            "    } else {\n"
            "        s_legacy_settle_armed = 0u;\n"
            "        apple_pal_video_begin_frame();\n"
            "    }" in source,
            "full shadow modes must publish at frame start and never publish "
            "an untouched writer slot at frame end")
    require("void apple_cycle_renderer_reset_local_video_state(void)" in source and
            "const uint32_t text_sw = SW_BIT(TEXT);" in source and
            "s_current_sw          = text_sw;" in source and
            "s_vidhd_newvideo      = 0u;" in source,
            "Apple reset must return local renderer soft-switch state to TEXT/C051 and clear fake-SHR")


def test_bezel_lists_c029_shr_softswitch() -> None:
    source = read(DEBUG_OVERLAY_C)

    require("static void draw_vidhd_shr_state" in source and
            "s->apple_mode == APPLE_FB_DISPLAY_MODE_SHR" in source,
            "soft-switch window must derive C029/SHR state from the published Apple FB mode")
    require("0xC029U" in source and '"%04lX SHR %u"' in source and
            "draw_vidhd_shr_state(fb, x, y, w, 8U, s);" in source,
            "soft-switch window must include a C029 SHR row")




def test_compositor_and_handoff_are_mode_aware() -> None:
    handoff_h = read(HANDOFF_H)
    handoff_c = read(HANDOFF_C)
    layout = read(COMPOSITOR_LAYOUT_H)
    compositor = read(COMPOSITOR_C)
    frontend_main = read(FRONTEND_MAIN_C)
    fb16_h = read(FB16_H)
    fb16_c = read(FB16_C)

    require("#define APPLE_FB_DISPLAY_MODE_LEGACY 0U" in handoff_h and
            "#define APPLE_FB_DISPLAY_MODE_SHR    1U" in handoff_h,
            "handoff must publish the renderer's frame geometry mode")
    require("void apple_fb_writer_publish_mode(uint32_t display_mode);" in handoff_h and
            "uint32_t apple_fb_reader_display_mode(void);" in handoff_h,
            "handoff API must carry mode metadata with the slot")
    require("uint32_t apple_fb_reader_published_display_mode(void);" in handoff_h and
            "return handoff_published_mode(published);" in handoff_c,
            "UI must be able to peek the next published Apple mode before the compositor blits it")
    require("#define HANDOFF_DISPLAY_MODE_ADDR   0xFFFF1010U" in handoff_c,
            "display mode metadata must live in the shared OCM handoff block")
    require("#define PUBLISHED_DISPLAY_MODE_SHIFT 8u" in handoff_c and
            "static inline uint32_t handoff_pack_published" in handoff_c and
            "handoff_pack_published(s_writer_current_slot, normalized_mode," in handoff_c and
            "s_reader_display_mode = handoff_published_mode(published);" in handoff_c,
            "published slot word must carry the display mode atomically with the slot index")
    require("APPLE_FB_FORMAT_SHR4_320" in handoff_h and
            "void apple_fb_writer_publish_frame_detail" in handoff_h and
            "uint32_t apple_fb_reader_format_detail(void);" in handoff_h and
            "#define PUBLISHED_FORMAT_DETAIL_SHIFT 14u" in handoff_c and
            "s_reader_format_detail = handoff_published_format_detail(published);" in handoff_c,
            "published slot word must carry exact format detail with the frame")

    require("#define COMP_APPLE_SHR_WIDTH        640u" in layout and
            "#define COMP_APPLE_SHR_HEIGHT       400u" in layout and
            "#define COMP_APPLE_SLOT_BYTES       0x00100000u" in layout,
            "Apple FB slots must be large enough for SHR")
    require("0x3F300000u" in read(REPO_ROOT / "ps_sources" / "frontend" / "compositor_layout.c") and
            "0x3F400000u" in read(REPO_ROOT / "ps_sources" / "frontend" / "compositor_layout.c") and
            "0x3F500000u" in read(REPO_ROOT / "ps_sources" / "frontend" / "compositor_layout.c"),
            "Apple FB slots must be spaced one megabyte apart")

    require("fb16_blit_2x2_scanlines" in fb16_h and "void fb16_blit_2x2_scanlines" in fb16_c,
            "fb16 must provide a 2x2 blitter for 640x400 SHR output")
    require("const uint32_t display_mode = apple_fb_reader_display_mode();" in compositor and
            "if (display_mode == APPLE_FB_DISPLAY_MODE_SHR)" in compositor and
            "fb16_blit_2x2_scanlines" in compositor,
            "compositor must choose the SHR blit path from published mode metadata")
    require("g_compositor_last_apple_slot" in compositor and
            "g_compositor_last_apple_mode" in compositor and
            "const uint32_t display_mode = apple_fb_reader_display_mode();" in compositor,
            "compositor must expose the last claimed Apple slot and mode for hardware diagnostics")
    badge = compositor[
        compositor.index("static const char *format_badge_video7_tag"):
        compositor.index("static void draw_format_badge")
    ]
    require("apple_fb_reader_format_detail()" in badge and
            "ACE_MAIN_BANK_ADDR" not in badge and "ACE_AUX_BANK_ADDR" not in badge and
            '"SHR4 %s %s%s"' in badge and '"SHR-3200 320"' in badge,
            "badge must use frame-coherent CPU1 metadata and name SHR subformats")
    require("format_badge_video7_tag" in badge and
            'return " Video-7 mono";' in badge and
            'return " Video-7 MIX";' in badge and
            "base == APPLE_FB_FORMAT_DHGR" in badge,
            "badge must tag Video-7 mono on legacy modes and MIX only on DHGR")
    require("apple_fb_slot" in read(REPO_ROOT / "ps_sources" / "frontend" / "uart_control.h") and
            "apple fb: slot=%lu mode=%s" in read(REPO_ROOT / "ps_sources" / "frontend" / "uart_control.c") and
            "snapshot->apple_fb_mode = g_compositor_last_apple_mode;" in frontend_main,
            "UART :status must expose Apple FB handoff mode so black-screen SHR can be diagnosed")
    require("static uint8_t g_output_slot_apple_mode[COMP_OUT_SLOT_COUNT];" in frontend_main and
            "static void ui_restore_apple_footprint_if_needed" in frontend_main and
            "apple_fb_reader_published_display_mode();" in frontend_main and
            "COMP_SHR_BORDER_X_OFF" in frontend_main and
            "COMP_SHR_BORDER_Y_OFF" in frontend_main and
            "COMP_SHR_BORDER_WIDTH" in frontend_main and
            "COMP_SHR_BORDER_HEIGHT" in frontend_main and
            "ui_restore_apple_footprint_if_needed(fb, show_bezel);" in frontend_main,
            "static bezel caching must restore the complete SHR border footprint when returning to legacy video")


def test_shr_generation_cache() -> None:
    renderer = read(RENDERER_C)
    egress = read(EGRESS_C) + read(EGRESS_H)
    layout = read(COMPOSITOR_LAYOUT_H)

    require("volatile uint32_t g_video_shadow_generation = 1U;" in egress and
            "extern volatile uint32_t g_video_shadow_generation;" in egress and
            "(uint16_t)(a & 0xFFFFU) >= 0x2000U" in egress and
            "(uint16_t)(a & 0xFFFFU) <= 0x9FFFU" in egress and
            "g_video_shadow_generation++;" in egress,
            "egress must advance the video generation after each relevant shadow write")
    require("static uint8_t  s_shr_cache_valid = 0u;" in renderer and
            "static uint8_t  s_shr_cache_invalidate = 1u;" in renderer and
            "static uint32_t s_shr_cache_generation = 0u;" in renderer and
            "static uint8_t  s_shr_settle_armed = 0u;" in renderer and
            "static uint32_t s_shr_settle_generation = 0u;" in renderer,
            "renderer must track cache validity, non-memory changes, and generation")
    require("generation != s_shr_cache_generation" in renderer and
            "generation != s_shr_settle_generation" in renderer and
            "s_shr_settle_generation = generation;" in renderer and
            "render_shr_frame_full();" in renderer and
            "publish_current_frame();" in renderer and
            "s_shr_cache_generation = g_video_shadow_generation;" in renderer and
            "g_acr_shr_cache_rebuilds++;" in renderer and
            "g_acr_shr_frames_skipped++;" in renderer,
            "changed SHR frames must settle before one full rebuild while "
            "static frames skip work")
    require("static uint8_t legacy_shadow_is_settled(void)" in renderer and
            "generation != s_legacy_settle_generation" in renderer and
            "s_frame_format_detail != s_legacy_settle_detail" in renderer and
            "s_video_settings_seen != s_legacy_settle_settings" in renderer,
            "legacy paged modes must wait for a stable data and format snapshot")
    require("#define ACE_RING_BASE           0x3F200000U" in egress and
            "#define ACE_RING_SIZE_LOG2      20U" in egress and
            "#define ACE_POLL_RECORD_CAP   131072U" in egress and
            "#define ACE_MMU_CONTROL_SECTION 0x3F000000U" in egress and
            "Xil_SetTlbAttributes(ACE_MMU_CONTROL_SECTION, NORM_NONCACHE);" in egress and
            "Xil_SetTlbAttributes(ACE_MMU_RING_SECTION, NORM_NONCACHE);" in egress and
            "#error \"Apple-cycle egress ring overlaps SDD or video shadow storage\"" in egress and
            "#error \"Apple-cycle egress ring overlaps Apple framebuffer slot 0\"" in egress and
            "0x3F200000  apple-cycle egress ring (1 MB; ends at 0x3F300000)" in layout,
            "the capture ring must hold one long SHR decode without "
            "overlapping SDD storage or framebuffer slots")
    require("s_shr_cache_invalidate = 1u;" in
            renderer[renderer.index("static void apply_video_rom_if_changed"):
                     renderer.index("static void apply_video_settings_if_changed")] and
            "s_shr_cache_invalidate = 1u;" in
            renderer[renderer.index("static void apply_video_settings_if_changed"):
                     renderer.index("void apple_cycle_renderer_reset_local_video_state")] and
            "s_shr_cache_valid     = 0u;" in
            renderer[renderer.index("void apple_cycle_renderer_reset_local_video_state"):
                     renderer.index("static void handle_video7_softswitch_record")] and
            "s_shr_cache_valid = 0u;" in
            renderer[renderer.index("static void shr_mode_switch_resync"):
                     renderer.index("volatile uint32_t g_acr_shr_mode_resyncs")],
            "ROM, settings, reset, and mode resync must invalidate cached SHR")
    require("const uint8_t frame_ready =\n"
            "        synthesized ? 0u : apple_pal_video_end_frame();" in renderer,
            "frame end must not publish a stale synthesized writer slot")
    marker = renderer[
        renderer.index("if (shr_frame_marker) {"):
        renderer.index("} else {", renderer.index("if (shr_frame_marker) {"))
    ]
    require("on_frame_end();" in marker and "on_frame_start();" in marker,
            "each SHR marker must close the frame and run the cache decision")


def test_core1_resets_local_video_state_on_apple_reset() -> None:
    core1 = read(REPO_ROOT / "ps_sources" / "frontend_core1" / "main.c")
    renderer_h = read(REPO_ROOT / "ps_sources" / "frontend" / "apple_cycle_renderer.h")
    card_regs = read(REPO_ROOT / "ps_sources" / "frontend" / "card_control_regs.h")

    require('#include "../frontend/card_control_regs.h"' in core1 and
            "static uint32_t apple_reset_status_read(void)" in core1 and
            "REG_READ(CARD_CTRL_APPLE_RESET_STATUS_REG)" in core1 and
            "#define CARD_CTRL_APPLE_RESET_SEQ_MASK" in card_regs,
            "CPU1 must read the PL Apple reset sequence register")
    require("if (reset_seq != reset_seq_last) {\n"
            "            reset_seq_last = reset_seq;\n"
            "            apple_cycle_renderer_reset_local_video_state();\n"
            "        }" in core1,
            "CPU1 must reset renderer-local video state when Apple reset sequence changes")
    require("void apple_cycle_renderer_reset_local_video_state(void);" in renderer_h,
            "renderer reset helper must be declared for CPU1")


def test_no_vidhd_identity_and_slot_layout() -> None:
    top = read(APPLE_TOP_SV)
    globals_sv = read(REPO_ROOT / "hdl" / "globals.sv")
    softswitch = read(REPO_ROOT / "hdl" / "apple" / "soft_switch_manager.sv")
    mouse = read(REPO_ROOT / "hdl" / "apple" / "mouse_card.sv")
    smartport = read(REPO_ROOT / "hdl" / "apple" / "smartport_card.sv")
    disk2 = read(REPO_ROOT / "hdl" / "apple" / "disk2_card.sv")
    sources = read(HDL_SOURCES)
    frontend_main = read(FRONTEND_MAIN_C)
    card_control_regs = read(CARD_CONTROL_REGS_H)
    config_menu = read(CONFIG_MENU_C)
    config_menu_internal = read(CONFIG_MENU_INTERNAL_H)
    config_menu_device_tabs = read(REPO_ROOT / "ps_sources" / "frontend" / "config_menu_device_tabs.c")
    config_menu_phasor = read(CONFIG_MENU_PHASOR_C)

    # Product decision (July 2026): the Appletini must NEVER identify
    # itself as a VidHD. Slot 3 carries no card identity at all -- the
    # //e internal 80-column firmware owns slot-3 space like a stock
    # machine, and the passive $C022/$C029/$C034/$C035 register shadow
    # in the capture path is observation only.
    require("apple/vidhd_card.sv" not in sources,
            "VidHD identity card must not be in the Vivado source list")
    require("vidhd_card" not in top,
            "no VidHD card may be instantiated")
    # 11 clients = the 10 post-VidHD-removal clients plus the virtual
    # TransWarp bus master; the VidHD client itself must stay gone.
    require("apple_bus_write_arbiter #(.NUM_CLIENTS(11))" in top and
            "vidhd_ab_write" not in top,
            "bus write arbiter must not include a VidHD client")
    require("logic sw_slotc3rom;" in globals_sv and
            "assign sss.sw_slotc3rom   = ss_slotc3rom;" in softswitch,
            "SoftSwitchState must expose SLOTC3ROM so slot-3 cards can avoid the internal //e ROM/IO personality")
    require("sss.slot_access &&" in mouse and
            "((slot_assign != 3'h3) || sss.sw_slotc3rom)" in mouse and
            "apple_bus_enabled = configured &&" in smartport and
            "((slot_assign != 3'h3) || sss.sw_slotc3rom)" in smartport and
            "apple_bus_active = enabled &&" in disk2 and
            "((slot_assign != 3'h3) || sss.sw_slotc3rom)" in disk2,
            "ROM-bearing virtual cards must use slot_access and the slot-3 external-ROM gate")
    require("CARD_CTRL_SLOT_ENABLE_RESET      = 32'h0000_0016" in top and
            "wire card_slot1_enable = card_slot_enable_mask_q[1];" in top and
            "wire card_slot2_enable = card_slot_enable_mask_q[2];" in top and
            "wire card_slot4_enable = card_slot_enable_mask_q[4];" in top,
            "PL default slot mask must enable Ethernet slot 1, mouse slot 2 and Phasor slot 4")
    require("localparam logic [2:0] MB1_SLOT_ASSIGN = 3'h4;" in top and
            ".slot_assign(MB1_SLOT_ASSIGN)" in top,
            "Phasor must be controlled as slot 4")
    require("mouse_card mouse_card_i" in top and
            ".ab_read(gate_ab(ab_read, card_slot2_enable))" in top and
            ".slot_assign(3'h2)" in top,
            "MouseCard must be controlled as slot 2")
    require('"mouse_card_slot2.mem"' in mouse and
            "apple/mouse_card_slot2.mem" in sources,
            "MouseCard slot ROM image must be patched and loaded for slot 2")
    require("#define CARD_CTRL_SLOT_MOUSE       2U" in card_control_regs and
            "#define CARD_CTRL_SLOT_MOCKINGBOARD 4U" in card_control_regs and
            "#include \"card_control_regs.h\"" in frontend_main,
            "firmware slot-control constants must match the PL slot layout")
    require("#define MOUSE_CONTROL_SLOT 2U" in config_menu and
            "#define MOCKINGBOARD_CONTROL_SLOT 4U" in config_menu_internal and
            '"Phasor"' in config_menu and
            "phasor.slot4.enabled=%s\\n" in config_menu_phasor and
            "phasor.pan.%u=%u\\n" in config_menu_phasor and
            "for (uint32_t channel = 0U; channel < MOCKINGBOARD_CHANNEL_COUNT; ++channel)" in config_menu_phasor and
            "mouse.slot2.enabled=%s\\n" in config_menu and
            "mockingboard_slot4=%u\\n" not in config_menu_phasor and
            "Enable in Slot 2" in config_menu_device_tabs and
            "Enable in Slot 4" in config_menu_phasor and
            'strcmp(key, "mouse.slot2.enabled") == 0' in config_menu and
            'strcmp(key, "phasor.slot4.enabled") == 0' in config_menu_phasor and
            'strcmp(key, "mockingboard_slot5") == 0' not in config_menu_phasor,
            "boot menu controls and saved config keys must describe the slot layout with clean dot notation")

    require("ui_draw_apple_view_border(fb);" not in frontend_main,
            "frontend border rendering must come from the compositor")


def test_shr4_extended_modes() -> None:
    """SHR4 (SDD-compatible): magic detection, per-pixel dispatch, and
    demosaic filter fidelity."""
    src = RENDERER_C.read_text(encoding="utf-8")
    hdr = (REPO_ROOT / "ps_sources" / "frontend" /
           "apple_cycle_renderer.h").read_text(encoding="utf-8")

    # Magic bytes 'SHR4' in high ASCII at aux $9DFC, re-evaluated per frame.
    require("SHR_MAGIC_ADDR  0x9DFCu" in src and
            "0xD3u" in src and "0xC8u" in src and
            "0xD2u" in src and "0xB4u" in src,
            "SHR4 magic must be the high-ASCII 'SHR4' string at $9DFC")
    require("s_shr4_frame_active = (uint8_t)shr4_magic_present(bank);" in src
            and "shr_eval_field_modes(g_aux_bank);" in src,
            "the magic must be re-evaluated every frame (mode-exit hygiene)")

    # Per-pixel dispatch on the palette second byte's high nibble.
    for token in ("case 1u:", "shr4_rggb_pixel", "case 2u:",
                  "shr4_pal256_color", "case 3u:", "shr4_r4g4b4_pixel"):
        require(token in src, f"SHR4 dispatch must include {token}")

    # PAL256 uses the flat 512-byte palette area as one 256-color table.
    require("0x9E00u + ((uint16_t)idx * 2u)" in src,
            "PAL256 must index the flat palette at aux $9E00")

    # Demosaic weight tables: each must sum to 16 (doubled Malvar gain 8)
    # so a flat field passes through unchanged.
    import re
    for name in ("k_rggb_g", "k_rggb_xg", "k_rggb_xgx", "k_rggb_rb"):
        m = re.search(name + r"\[13\]\s*=\s*\{([^}]+)\}", src)
        require(m is not None, f"missing filter table {name}")
        weights = [int(t) for t in re.findall(r"-?\d+", m.group(1))]
        require(len(weights) == 13, f"{name} must have 13 weights")
        require(sum(weights) == 16,
                f"{name} weights sum to {sum(weights)}, expected 16")

    # Normalization divisor matches the doubled weights: max_sample * 16
    # (240 for 4-bit 320-mode samples, 48 for 2-bit 640-mode samples).
    require("/ (max_sample * 16)" in src,
            "filter must normalize by max_sample * 16")
    require("is640 ? 3 : 15" in src,
            "640-mode RGGB must use 2-bit samples (max 3)")

    # Diagnostics exported.
    require("g_acr_shr4_frames" in hdr,
            "SHR4 frame counter must be visible in the header")



def test_shr3200_mode() -> None:
    """SHR-3200: magic, reversed palette index, either-bank palettes."""
    src = RENDERER_C.read_text(encoding="utf-8")
    require("0xB3u" in src and "0xB2u" in src and "0xB0u" in src,
            "3200 magic must be the high-ASCII '3200' string")
    require("15u - (idx & 0x0Fu)" in src,
            "3200 palette index must be reversed (0 = last entry)")
    require("s_shr3200_bank ? g_aux_bank" in src,
            "3200 palettes must come from either bank per ctrl byte 1")
    require("pal_start < (uint16_t)(0xFFFFu - 200u * 32u)" in src,
            "3200 palette pointer must be bounds-checked")
    require("if (!s_shr4_frame_active && shr3200_magic_present(bank))" in src,
            "SHR4 must take priority over 3200")


def test_shr_interlace() -> None:
    """SHR interlace: per-field evaluation, geometry, PL window."""
    src = RENDERER_C.read_text(encoding="utf-8")
    top = APPLE_TOP_SV.read_text(encoding="utf-8")
    engine = (REPO_ROOT / "hdl" / "apple" /
              "vtw_bus_engine.sv").read_text(encoding="utf-8")
    core = (REPO_ROOT / "hdl" / "apple" /
            "vtw_core_top.sv").read_text(encoding="utf-8")

    # Paged type 1 = interlace weave; type 2 = page flip, which below
    # 120 Hz output MERGES both fields 50/50 per frame (the old
    # frame-alternation approach was too flickery and stays banned).
    require("if (paged == 1u) {" in src and
            "s_shr_interlace_mode = 1u;" in src and
            "} else if (paged == 2u) {" in src and
            "flip_merge = 1u;" in src and
            "bgra_avg(s_shr_merge_row_a[x]," in src and
            "s_shr_flip_parity" not in src,
            "renderer must weave interlace and merge page flips, never alternate")
    # Interlace: odd rows come from the main field, each field re-evaluated.
    require("render_shr_line_to(\n"
            "                    &g_atn_framebuffer[(y * 2u + field) * SHR_WIDTH], y);" in src and
            "shr_eval_field_modes(field ? g_main_bank : g_aux_bank);" in src,
            "interlace must render both fields with per-field mode state")
    # CPU1 -> PL handshake, write-on-change.
    require("Xil_Out32(CARD_CTRL_VIDEO_POST_WIDE_REG" in src and
            "if (want == s_post_wide_last) return;" in src,
            "renderer must widen the vTW window via CARD_CTRL 0x35 on change")
    # PL plumbing end to end.
    require("8'h35: post_main_wide_q <= as_common.wdata[0];" in top,
            "apple_top must decode the 0x35 widen bit")
    require("wide_main" in engine and
            "post_main_wide" in core,
            "the widened window must reach the posting classifier")
    require("logic shr_post_main_wide_q;" in core and
            "wire post_main_wide_eff = post_main_wide | shr_post_main_wide_q;" in core and
            "cycle_addr_q == 16'h9DF8" in core and
            "xl_is_write && xl_is_aux" in core and
            "cycle_wdata_q == 8'd1 || cycle_wdata_q == 8'd2" in core and
            "post_main_wide_eff" in core,
            "vTW must widen from its private aux $9DF8 write before CPU1 sees the post")


def test_shr4_640_mode() -> None:
    """SHR4 640-mode dispatch: quadrant palette remap + 2-bit RGGB."""
    src = RENDERER_C.read_text(encoding="utf-8")
    require("k_shr640_quad[4] = { 8u, 12u, 0u, 4u };" in src,
            "640-mode pixels must use the standard palette quadrants "
            "8-11, 12-15, 0-3, 4-7 left to right")
    require("(byte >> (6u - 2u * lp)) & 0x03u" in src,
            "640-mode pixels are 2 bits each, leftmost in the high bits")
    require("(bank[a] >> (6 - 2 * (px & 3))) & 0x03u" in src,
            "640 RGGB samples extract the raw 2-bit value at 640 res")
    require("shr4_rggb_pixel_lookup(px, (int32_t)s_f_out_row, 1)" in src,
            "640 RGGB must demosaic in 640-space with 2-bit samples")
    require("shr4_render_cell_640(row, y, x, palette_base);" in src,
            "render_shr_line must dispatch 640-mode SCBs to the SHR4 "
            "640 cell when SHR4 is active")


def test_shr_mode_self_heal() -> None:
    """A dropped C029 record must not freeze the renderer: CPU1 polls
    the PL's authoritative SHR state each batch and repairs."""
    src = RENDERER_C.read_text(encoding="utf-8")
    cap = read(CAPTURE_SV)
    top = read(APPLE_TOP_SV)
    core1 = (REPO_ROOT / "ps_sources" / "frontend_core1" /
             "main.c").read_text(encoding="utf-8")
    regs = read(CARD_CONTROL_REGS_H)
    require("output logic                            shr_capture_active" in cap
            and "assign shr_capture_active = shr_capture_active_q;" in cap,
            "capture must export its authoritative fake-SHR state")
    require("21'h000000, shr_capture_active_w," in top,
            "apple_top must expose the SHR state in the reset-status word")
    require("CARD_CTRL_APPLE_RESET_SHR_ACTIVE_BIT   (1UL << 10)" in regs,
            "regs header must define the SHR-active status bit")
    require("void apple_cycle_renderer_sync_shr_mode(uint8_t pl_shr_active)"
            in src and "g_acr_shr_mode_resyncs++;" in src,
            "renderer must repair local C029 state from the PL and count it")
    require("apple_cycle_renderer_sync_shr_mode(" in core1 and
            "CARD_CTRL_APPLE_RESET_SHR_ACTIVE_BIT" in core1,
            "CPU1 must feed the PL SHR state to the renderer each batch")
    main0 = (REPO_ROOT / "ps_sources" / "frontend" /
             "main.c").read_text(encoding="utf-8")
    require("uart_control_dma_bus_write(0xC029U, 0x01U);" in main0,
            "CPU0 must clear fake-SHR with a real C029 bus write on "
            "Apple reset")
    require("s_shr_sync_hold" in src and
            "s_shr_sync_hold = 1u;" in src,
            "the renderer must hold off SHR re-adoption after reset "
            "until the PL clear lands")


def test_page_flip_removed() -> None:
    """Neither legacy video nor SHR may alternate frame pages."""
    src = RENDERER_C.read_text(encoding="utf-8")
    top = APPLE_TOP_SV.read_text(encoding="utf-8")
    core1 = (REPO_ROOT / "ps_sources" / "frontend_core1" /
             "main.c").read_text(encoding="utf-8")
    menu = (REPO_ROOT / "ps_sources" / "frontend" /
            "config_menu.c").read_text(encoding="utf-8")
    menu_h = (REPO_ROOT / "ps_sources" / "frontend" /
              "config_menu.h").read_text(encoding="utf-8")
    menu_internal = (REPO_ROOT / "ps_sources" / "frontend" /
                     "config_menu_internal.h").read_text(encoding="utf-8")
    menu_tabs = (REPO_ROOT / "ps_sources" / "frontend" /
                 "config_menu_main_tabs.c").read_text(encoding="utf-8")
    regs = read(CARD_CONTROL_REGS_H)

    combined = "\n".join((src, top, core1, menu, menu_h, menu_internal,
                          menu_tabs, regs))
    for obsolete in ("legacy_paging", "legacy_page_flip",
                     "LEGACY_PAGING", "LEGACY_FLIP",
                     "video.legacy.flip", "s_legacy_flip_parity",
                     "s_shr_flip_parity"):
        require(obsolete not in combined,
                f"obsolete page-flip path remains: {obsolete}")
    require("CONFIG_VIDEO_ITEM_COUNT        16U" in menu_internal,
            "Video menu must contain the current 16 controls")


TESTS = [
    test_record_kind_contract,
    test_capture_emits_two_records_for_vidhd_io_plus_frame,
    test_shr_capture_uses_aux_shadow_without_m2b0,
    test_renderer_tracks_vidhd_register_state,
    test_renderer_implements_video7_auto_white_mono,
    test_renderer_implements_applewin_shr_decode,
    test_bezel_lists_c029_shr_softswitch,
    test_compositor_and_handoff_are_mode_aware,
    test_shr_generation_cache,
    test_core1_resets_local_video_state_on_apple_reset,
    test_no_vidhd_identity_and_slot_layout,
    test_shr4_extended_modes,
    test_shr3200_mode,
    test_shr_interlace,
    test_shr4_640_mode,
    test_shr_mode_self_heal,
    test_page_flip_removed,
]


def main() -> int:
    failures = []
    for test in TESTS:
        try:
            test()
        except TestFailure as exc:
            failures.append((test.__name__, str(exc)))
            print(f"FAIL {test.__name__}: {exc}")
        else:
            print(f"PASS {test.__name__}")
    if failures:
        print(f"{len(TESTS) - len(failures)} of {len(TESTS)} VidHD/SHR tests passed; "
              f"{len(failures)} failed")
        return 1
    print(f"{len(TESTS)} VidHD/SHR tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
