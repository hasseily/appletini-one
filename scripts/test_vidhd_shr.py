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
VIDHD_CARD_SV = REPO_ROOT / "hdl" / "apple" / "vidhd_card.sv"
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
    require("0x002000-0x005FFF" in capture and
            "((a >= 24'h002000) && (a <= 24'h005FFF))" in capture,
            "capture must keep the normal main HGR window bounded to $2000-$5FFF")
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
    require("case 0xC022U:\n        s_vidhd_screen_color = data;" in source and
            "case 0xC029U:\n        s_vidhd_newvideo = data;" in source and
            "case 0xC034U:\n        s_vidhd_border_color =" in source and
            "case 0xC035U:\n        s_vidhd_shadow = data;" in source,
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
            "static uint8_t s_video7_prev_an3_addr = 0u;" in source,
            "renderer must keep AppleWin-style Video-7 RGB mode state")
    require("static void handle_video7_softswitch_record(uint64_t rec)" in source and
            "if (low == 0x5Fu && s_video7_prev_an3_addr == 0x5Eu)" in source and
            "s_video7_rgb_flags = (uint8_t)((s_video7_rgb_flags << 1) & 0x03u);" in source and
            "s_video7_rgb_flags |= sw_80col(ace_softswitch_bits(rec)) ? 0u : 1u;" in source,
            "renderer must clock !80COL on exact C05E->C05F AN3 transitions")
    require("const uint8_t video7_auto_mono =\n"
            "        ((user_mono == 0u) &&\n"
            "         (apple_video_settings_video7_auto_mono_enabled(settings) != 0u) &&\n"
            "         (s_video7_rgb_mode == 3u)) ? 1u : 0u;" in source and
            "((user_mono != 0u) || (bw_force != 0u) || (video7_auto_mono != 0u)) ? 1u : 0u" in source and
            "((bw_force != 0u) || (video7_auto_mono != 0u)) ?\n"
            "        APPLE_VIDEO_MONO_WHITE" in source,
            "Video-7 mode 3 must auto-force white mono only when bootmenu mono is off and the toggle is enabled")
    require("apple_video_settings_color_mode(settings)" in source and
            "s_render_color_mode = apple_video_settings_color_mode(settings);" in source and
            "video7_auto_mono" in source and
            "s_render_color_mode == APPLE_VIDEO_COLOR_RGB && video7_auto_mono" not in source,
            "Video-7 auto-mono must apply to every bootmenu color mode, not only RGB")
    require("s_video7_rgb_flags    = 0u;" in source and
            "s_video7_rgb_mode     = 0u;" in source and
            "s_video7_prev_an3_addr = 0u;" in source,
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
    require("const uint8_t control = g_aux_bank[0x9D00u + (uint16_t)y];" in source and
            "const uint16_t palette_base = (uint16_t)(0x9E00u + ((uint16_t)(control & 0x0Fu) * 32u));" in source,
            "SHR renderer must read line control and palette from AUX")
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
            "render_shr_cell(y, x);" in source,
            "renderer must be able to build a full SHR frame directly from AUX shadow")
    require("if (s_frame_display_mode == APPLE_FB_DISPLAY_MODE_SHR) {\n"
            "        render_shr_frame_full();\n    }" in source,
            "renderer must render the full SHR AUX-shadow frame at frame start")
    require("const int shr_frame_marker =\n        shr_active && line == 0u && cycle == 0u;" in source and
            "(s_prev_line >= 200u || (shr_frame_marker && s_render_armed))" in source,
            "renderer must treat sparse SHR markers as frame boundaries")
    require("if (shr_active) {\n        /* C029 SHR owns the frame geometry and pixel decode while active." in source and
            "s_records_in_frame++;\n        return;" in source,
            "per-record renderer dispatch must bypass legacy NTSC while SHR is active")
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
            "handoff_pack_published(s_writer_current_slot, normalized_mode, border_color)" in handoff_c and
            "s_reader_display_mode = handoff_published_mode(published);" in handoff_c,
            "published slot word must carry the display mode atomically with the slot index")

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
    require("apple_fb_slot" in read(REPO_ROOT / "ps_sources" / "frontend" / "uart_control.h") and
            "apple fb: slot=%lu mode=%s" in read(REPO_ROOT / "ps_sources" / "frontend" / "uart_control.c") and
            "snapshot->apple_fb_mode = g_compositor_last_apple_mode;" in frontend_main,
            "UART :status must expose Apple FB handoff mode so black-screen SHR can be diagnosed")
    require("static uint8_t g_output_slot_apple_mode[COMP_OUT_SLOT_COUNT];" in frontend_main and
            "static void ui_restore_apple_footprint_if_needed" in frontend_main and
            "apple_fb_reader_published_display_mode();" in frontend_main and
            "COMP_SUBWIN_SHR_X_OFF" in frontend_main and
            "COMP_SUBWIN_SHR_WIDTH" in frontend_main and
            "ui_restore_apple_footprint_if_needed(fb, show_bezel);" in frontend_main,
            "static bezel caching must restore the larger SHR footprint when returning to legacy video")


def test_core1_resets_local_video_state_on_apple_reset() -> None:
    core1 = read(REPO_ROOT / "ps_sources" / "frontend_core1" / "main.c")
    renderer_h = read(REPO_ROOT / "ps_sources" / "frontend" / "apple_cycle_renderer.h")

    require("#define APPLE_RESET_STATUS_REG       0x40000024U" in core1 and
            "static uint8_t apple_reset_seq_read(void)" in core1,
            "CPU1 must read the PL Apple reset sequence register")
    require("if (reset_seq != reset_seq_last) {\n"
            "            reset_seq_last = reset_seq;\n"
            "            apple_cycle_renderer_reset_local_video_state();\n"
            "        }" in core1,
            "CPU1 must reset renderer-local video state when Apple reset sequence changes")
    require("void apple_cycle_renderer_reset_local_video_state(void);" in renderer_h,
            "renderer reset helper must be declared for CPU1")


def test_vidhd_slot3_identity_and_slot_layout() -> None:
    top = read(APPLE_TOP_SV)
    vidhd = read(VIDHD_CARD_SV)
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

    require("apple/vidhd_card.sv" in sources,
            "VidHD card source must be included in Vivado source list")
    require("vidhd_card vidhd_card_i" in top and
            ".sss(sss)" in top and
            ".slot_assign(3'h3)" in top,
            "VidHD card must be instantiated in slot 3")
    require("apple_bus_write_arbiter #(.NUM_CLIENTS(11))" in top and
            "vidhd_ab_write" in top,
            "VidHD card bus writer must be in the Apple bus write arbiter")
    require("input  globals::SoftSwitchState sss" in vidhd and
            "wire slot_rom_hit" in vidhd and
            "sss.slot_access" in vidhd and
            "wire ab_rom_read = ab_read.serve_en && ab_read.rw && slot_rom_hit;" in vidhd,
            "VidHD slot-ROM identity must be gated by soft-switch slot access")
    require("logic sw_slotc3rom;" in globals_sv and
            "assign sss.sw_slotc3rom   = ss_slotc3rom;" in softswitch,
            "SoftSwitchState must expose SLOTC3ROM so slot-3 cards can avoid the internal //e ROM/IO personality")
    require("((slot_assign != 3'h3) || sss.sw_slotc3rom)" in vidhd,
            "VidHD slot-3 I/O and ROM identity must be disabled unless SLOTC3ROM selects the external slot")
    require("ab_write_d.assert_inh = 1'b0;" in vidhd and
            "assert_inh = ab_rom_read" not in vidhd,
            "VidHD identity must leave the //e internal slot-3 ROM uninhibited")
    require("ab_write_d               = ab_write_q;" in vidhd and
            "if (!enabled || ab_read.addr_en || ab_read.data_en)" in vidhd,
            "VidHD must hold its read response until the Apple data-drive window")
    require("16'h24EA" not in vidhd and
            "4'h0: vidhd_magic_byte = 8'h24;" in vidhd and
            "4'h1: vidhd_magic_byte = 8'hEA;" in vidhd and
            "4'h2: vidhd_magic_byte = 8'h4C;" in vidhd and
            "(ab_rom_read && (rom_idx <= 8'h02))" in vidhd,
            "VidHD slot I/O and gated slot-ROM identity must expose AppleWin's magic bytes")
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


TESTS = [
    test_record_kind_contract,
    test_capture_emits_two_records_for_vidhd_io_plus_frame,
    test_shr_capture_uses_aux_shadow_without_m2b0,
    test_renderer_tracks_vidhd_register_state,
    test_renderer_implements_video7_auto_white_mono,
    test_renderer_implements_applewin_shr_decode,
    test_bezel_lists_c029_shr_softswitch,
    test_compositor_and_handoff_are_mode_aware,
    test_core1_resets_local_video_state_on_apple_reset,
    test_vidhd_slot3_identity_and_slot_layout,
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
