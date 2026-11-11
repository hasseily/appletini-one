#!/usr/bin/env python3
"""Execute native IIgs capture-state and renderer helpers on the host."""

import sys
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory

from test_joystick_config_menu import function
from test_onee_input_service import find_native_c_compiler

FRONT = Path(__file__).resolve().parents[1] / "ps_sources/frontend"


def main():
    renderer = (FRONT / "apple_cycle_renderer.c").read_text(encoding="utf-8")
    egress = (FRONT / "apple_cycle_egress.c").read_text(encoding="utf-8")
    helpers = renderer.replace("static inline ", "static ")
    code = r"""
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "apple_cycle_egress.h"
#include "video_output.h"
#define ATN_SCANNER_MAX_VERT_PAL 312u
#define ATN_SCANNER_MAX_VERT_NTSC 262u
static uint8_t s_vidhd_newvideo, s_gs_active, s_gs_newvideo, s_gs_screen_color;
static uint8_t s_gs_langsel, s_gs_wait_state, s_vidhd_border_color;
static uint16_t s_gs_recovery_records;
#define GS_GAP_QUEUE_BOUND 4160u
static uint8_t s_border_default_color = 6;
static uint8_t s_shr_cache_invalidate, s_gs_previous_shr_cell;
static uint32_t s_current_sw, s_scanner_frame_lines;
static uint32_t *g_pVideoAddress;
static uint32_t g_aAppleWinBaseColors[16], s_aw_base_packed[16];
static uint32_t aw_pack_bgra(uint32_t c) { return 0xFF000000u | c; }
static unsigned resyncs;
static void apply_video_settings_if_changed(void) {}
static void gs_raster_state(uint64_t rec) { (void)rec; }
static void shr_mode_switch_resync(void) { ++resyncs; }
static uint32_t shr_palette_color(uint16_t base, uint8_t index)
{
    return 0xFF000000u | ((uint32_t)base << 8) | index;
}
"""
    palette_enum = renderer.index("enum {\n    AW_HGR_BLACK")
    code += renderer[palette_enum:renderer.index("};", palette_enum) + 2] + "\n"
    for name in (
        "vidhd_shr_enabled", "vidhd_bw_forced", "shr_render_cell_320",
        "shr_render_cell_640", "gs_set_video_register", "handle_gs_state_record",
        "aw_emit_duochrome_bits", "aw_bgr", "aw_palette_color", "aw_base_color",
    ):
        code += function(helpers, name)
    code += r"""
int main(void)
{
    const uint64_t snapshot =
        ((uint64_t)ACE_RECORD_KIND_GS_STATE << ACE_BIT_RECORD_KIND_LO) |
        (1ULL << ACE_BIT_FRAME_EN) | (0x6AULL << ACE_BIT_GS_C022_LO) |
        (0xA0ULL << ACE_BIT_GS_C029_LO) | (0x10ULL << ACE_BIT_GS_C02B_LO) |
        (1ULL << ACE_BIT_GS_PHASE_LOCKED) | (1ULL << ACE_BIT_GS_FRAME_SELECTED) |
        (7ULL << ACE_BIT_GS_BORDER_LO) | (3ULL << ACE_BIT_SW_DHIRES);
    uint32_t pixels[32] = {0};
    handle_gs_state_record(snapshot);
    assert(s_gs_active && s_gs_screen_color == 0x6A && s_gs_newvideo == 0xA0);
    assert(s_vidhd_border_color == 7 && s_current_sw == 3);
    assert(s_scanner_frame_lines == 312 && vidhd_shr_enabled() && !vidhd_bw_forced());
    assert(aw_palette_color(AW_HGR_BLUE) == 0xFF2222FFu);
    assert(aw_palette_color(AW_HGR_ORANGE) == 0xFFFF6600u);
    assert(aw_palette_color(AW_HGR_GREEN) == 0xFF00DD00u);
    assert(aw_palette_color(AW_HGR_VIOLET) == 0xFFDD22DDu);
    for (unsigned i = 0; i < 16; ++i)
        assert(aw_base_color((uint8_t)i) == apple_video_iigs_border_bgra((uint8_t)i));
    g_pVideoAddress = pixels;
    aw_emit_duochrome_bits(0x01, 4, 1);
    assert(pixels[0] == apple_video_iigs_border_bgra(6) && pixels[1] == pixels[0]);
    assert(pixels[2] == apple_video_iigs_border_bgra(10) && pixels[3] == pixels[2]);
    gs_set_video_register(0xC022, 0xA6);
    g_pVideoAddress = pixels;
    aw_emit_duochrome_bits(0x02, 2, 0);
    assert(pixels[0] == apple_video_iigs_border_bgra(6));
    assert(pixels[1] == apple_video_iigs_border_bgra(10));
    shr_render_cell_320(pixels, 0, 0x02010000, 2, 1);
    assert(pixels[0] == shr_palette_color(2, 0));
    assert(pixels[10] == shr_palette_color(2, 1));
    assert(pixels[12] == shr_palette_color(2, 1));
    assert(pixels[14] == shr_palette_color(2, 2));
    shr_render_cell_640(pixels, 1, 0x0000001B, 3);
    assert(pixels[16] == shr_palette_color(3, 8));
    assert(pixels[17] == shr_palette_color(3, 13));
    assert(pixels[18] == shr_palette_color(3, 2));
    assert(pixels[19] == shr_palette_color(3, 7));
    gs_set_video_register(0xC029, 0x40);
    assert(!vidhd_shr_enabled());
    s_gs_wait_state = 2;
    handle_gs_state_record(snapshot & ~(1ULL << (ACE_BIT_GS_C02B_LO + 4)));
    assert(s_gs_wait_state == 1 && s_scanner_frame_lines == 262);
    s_gs_recovery_records = GS_GAP_QUEUE_BOUND;
    handle_gs_state_record(snapshot);
    assert(!s_gs_wait_state && s_scanner_frame_lines == 312);
    handle_gs_state_record(snapshot & ~(1ULL << ACE_BIT_FRAME_EN));
    assert(!s_gs_active && !vidhd_shr_enabled());
    s_vidhd_newvideo = 0xE0;
    assert(vidhd_shr_enabled() && vidhd_bw_forced());
    assert(resyncs == 2);
    puts("Native state, SHR selection, color fill, 640 palettes, 50/60 Hz, recovery");
    return 0;
}
"""
    compiler = find_native_c_compiler()
    if compiler is None:
        raise RuntimeError("A native C compiler is required")
    raster = (Path(__file__).parent / "fixtures/iigs_video_raster_harness.c").read_text(encoding="utf-8")
    renderer_names = (
        "vidhd_shr_enabled", "vidhd_bw_forced", "shr_scanline_addr", "shr_pack_bgra",
        "shr_apply_c029_bw", "shr_read_palette_color", "shr_palette_color",
        "shr_render_cell_320", "shr_render_cell_640",
        "gs_set_video_register", "handle_gs_state_record", "gs_render_mono_cell", "gs_raster_cell",
        "gs_latch_scanline", "gs_raster_visible_cycle", "gs_raster_begin_frame", "gs_raster_drop", "gs_raster_time",
        "gs_raster_state", "apple_cycle_renderer_before_record", "phase_record_is_frame",
        "apple_cycle_renderer_needs_phase_lookahead", "dispatch_gs_record",
        "apple_cycle_renderer_note_aux_ctrl_write", "gs_raster_gap",
    )
    start = egress.index("            if (apple_cycle_renderer_before_record)")
    end = egress.index("            g_records_processed++;", start)
    raster = raster.replace("/* RENDERER */", "\n".join(function(helpers, n) for n in renderer_names))
    raster = raster.replace("/* EGRESS */", egress[start:end])
    with TemporaryDirectory() as tmp:
        for name, text in (("state", code), ("raster", raster)):
            source = Path(tmp) / f"{name}.c"
            binary = Path(tmp) / f"{name}.exe"
            source.write_text(text, encoding="utf-8")
            # The egress weak hooks and their fixture definitions share a TU.
            extra = ["-Wno-address"] if name == "raster" else []
            benchmark = ["--benchmark"] if name == "raster" and "--benchmark" in sys.argv else []
            if benchmark:
                extra.append("-O2")
            subprocess.run([str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror", *extra,
                            "-I", str(FRONT), str(source), "-o", str(binary)], check=True)
            subprocess.run([str(binary), *benchmark], check=True)


if __name__ == "__main__":
    main()

