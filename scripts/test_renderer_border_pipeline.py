#!/usr/bin/env python3
"""Replay CPU1 captures into production CPU0 border drawing, checking pixels."""

import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build" / "renderer_border_pipeline"


def main():
    BUILD.mkdir(parents=True, exist_ok=True)
    comp = (ROOT / "ps_sources/frontend/compositor.c").read_text()
    layout = (ROOT / "ps_sources/frontend/compositor_layout.c").read_text()
    effects = comp[comp.index("#define EFFECT_HISTORY_STRIDE"):
                   comp.index("/* ---------- Format badge")]
    drawing = comp[comp.index("static void fill_border_rect"):
                   comp.index("/* ---------- SuperSprite")]
    active_start = comp.index("static inline int compositor_apple_effects_active")
    effects_active = comp[active_start:comp.index("\n}", active_start) + 2]
    source = BUILD / "pipeline.c"
    source.write_text(r'''
#define main capture_harness_main
#define HARNESS_BORDER_CALLBACK
#include "harness.c"
#undef main
#include "fb16.h"
#include "display_modes.h"
#include "video_mono.h"
#include "video_blur.h"
#include "video_glow.h"
#include "video_ghosting.h"
#include "scanlines.h"
static int smartport_service_has_pending(void) { return 0; }
static void smartport_service_poll(void) {}
static uint8_t s_video_dot_bleed = APPLETINI_VIDEO_DOT_BLEED_LIGHT;
static uint8_t s_border_enabled = 1, s_border_flood, s_scanlines_mode;
static uint8_t s_video_ghosting_strength, s_video_blur_strength, s_video_glow_strength;
static uint8_t s_format_badge_enabled;
volatile uint32_t g_compositor_last_apple_slot, g_compositor_last_apple_mode;
uint32_t apple_fb_reader_display_mode(void) { return s_pub_mode; }
uint32_t apple_fb_reader_format_detail(void) { return s_pub_detail; }
uint8_t apple_fb_reader_border_color(void) { return s_pub_border; }
static void draw_format_badge(uint16_t *fb, int x, int y, int w) {
    (void)fb; (void)x; (void)y; (void)w;
}
''' + layout[layout.index("/* Only CPU0 changes output geometry."):] +
        effects + effects_active + drawing + r'''
static uint16_t output[COMP_OUT_MAX_WIDTH * COMP_OUT_MAX_HEIGHT + 2];
static unsigned checks;
static void verify_composited_border(void)
{
    for (unsigned mode = 0; mode < DISPLAY_MODE_COUNT; ++mode) {
        const display_mode_t *d = display_mode_get((uint8_t)mode);
        for (unsigned preference = 0; preference <= 2; ++preference) {
            comp_layout_set_size_multiplier((uint8_t)preference);
            comp_layout_set_output_size(d->width, d->height);
            fb16_set_size(d->width, d->height);
            const comp_viewport_t *v = s_pub_mode == APPLE_FB_DISPLAY_MODE_SHR
                ? &comp_shr_viewport : &comp_legacy_viewport;
            const unsigned sy = (s_pub_mode == APPLE_FB_DISPLAY_MODE_LEGACY)
                ? 2 * v->scale : v->scale;
            const uint16_t border = fb16_from_bgra32(apple_video_iigs_border_bgra(s_pub_border));
            for (unsigned effects = 0; effects < 2; ++effects) {
                s_video_dot_bleed = effects ? APPLETINI_VIDEO_DOT_BLEED_LIGHT : 0;
                s_video_ghosting_strength = effects ? APPLETINI_VIDEO_GHOSTING_LIGHT : 0;
                s_scanlines_mode = effects ? 3 : 0;
                s_border_flood = effects;
                for (size_t i = 0; i < sizeof output / sizeof output[0]; ++i) output[i] = 0x1357;
                effect_clear_history();
                draw_apple_subwindow(output + 1, s_pub_slot);
                if (output[0] != 0x1357 || output[(size_t)d->width*d->height + 1] != 0x1357) {
                    fprintf(stderr, "border pipeline wrote outside output\n"); exit(1);
                }
                for (int y = 0; y < d->height; ++y) {
                    for (int x = 0; x < d->width; ++x) {
                        if (x >= v->x && x < v->x + v->width &&
                            y >= v->y && y < v->y + v->height) continue;
                        const int ring = x >= v->border_x && x < v->border_x + v->border_width &&
                            y >= v->border_y && y < v->border_y + v->border_height;
                        const unsigned phase = (unsigned)(y - v->border_y) & (sy - 1U);
                        const int blank = s_scanlines_mode &&
                            ((sy == 4 && phase != 0) || (sy == 2 && phase == 1));
                        const uint16_t want = (ring || s_border_flood) ? (blank ? 0 : border) : 0x1357;
                        const uint16_t got = output[1 + y*d->width+x];
                        if (got != want) {
                            fprintf(stderr, "border pipeline mode=%u size=%u apple=%u detail=%x color=%u effects=%u at %d,%d got=%x want=%x\n",
                                mode, preference, s_pub_mode, s_pub_detail, s_pub_border, effects, x, y, got, want);
                            exit(1);
                        }
                    }
                }
                ++checks;
            }
        }
    }
}
int main(void)
{
    char *args[] = {"pipeline", "--border-only"};
    const int result = capture_harness_main(2, args);
    printf("renderer border pipeline: %u frame checks, %s\n", checks, result ? "FAIL" : "PASS");
    return result;
}
''')
    if os.name != "nt":
        raise RuntimeError("This harness needs a 32-bit host compiler; Windows MSVC x86 is configured")
    vswhere = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
    install = subprocess.check_output([str(vswhere), "-latest", "-products", "*", "-requires",
        "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"], text=True).strip()
    vcvars = Path(install) / "VC/Auxiliary/Build/vcvars32.bat"
    src = [source] + [ROOT / "ps_sources" / p for p in (
        "frontend/apple_cycle_renderer.c", "frontend/appletini_ntsc.c",
        "frontend/appletini_csbits.c", "frontend/apple2e_video_rom_data.c",
        "frontend/apple_pal_video_timing.c", "lib/fb16.c")]
    inc = [ROOT / p for p in ("scripts/host_render_harness", "scripts/host_render_harness/xil_stub",
                              "ps_sources/frontend", "ps_sources/lib")]
    command = f'call "{vcvars}" >nul 2>&1\ncl /nologo /W3 /O2 /std:c11 /D_CRT_SECURE_NO_WARNINGS /DHARNESS_REAL_PAL /FI harness_prefix.h '
    command += " ".join(f'/I"{p}"' for p in inc)
    command += f' /Fo"{BUILD}\\\\" /Fe"{BUILD / "pipeline.exe"}" ' + " ".join(f'"{p}"' for p in src)
    batch = BUILD / "compile.bat"
    batch.write_text("@echo off\n" + command + "\n")
    subprocess.run(["cmd", "/c", str(batch)], check=True)
    result = subprocess.run([str(BUILD / "pipeline.exe")], text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (BUILD / "test.log").write_text(result.stdout)
    if result.returncode:
        print(result.stdout)
        result.check_returncode()
    print(result.stdout.strip().splitlines()[-1])


if __name__ == "__main__":
    main()
