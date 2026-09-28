#!/usr/bin/env python3
"""Compile production scaling and compare clipped output pixels and guards."""

import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build" / "display_scaling_test"


def main():
    compiler = os.environ.get("CC") or shutil.which("gcc")
    if not compiler:
        for candidate in (
            "C:/msys64/ucrt64/bin/gcc.exe",
            "E:/AMDDesignTools/2025.2/tps/mingw/10.0.0/win64.o/nt/bin/gcc.exe",
        ):
            if Path(candidate).exists():
                compiler = candidate
                break
    if not compiler:
        raise RuntimeError("Set CC to a native GCC compiler")
    BUILD.mkdir(parents=True, exist_ok=True)
    compositor = (ROOT / "ps_sources/frontend/compositor.c").read_text(encoding="utf-8")
    effects = compositor[compositor.index("#define EFFECT_HISTORY_STRIDE"):
                         compositor.index("/* ---------- Format badge")]
    borders = compositor[compositor.index("static void fill_border_rect"):
                         compositor.index("static int draw_apple_subwindow")]
    apple_drawing = compositor[compositor.index("static int draw_apple_subwindow"):
                               compositor.index("/* ---------- SuperSprite")]
    size_api = compositor[compositor.index("void compositor_set_size_multiplier"):
                          compositor.index("void compositor_init")]
    overlay = (ROOT / "ps_sources/frontend/linear_text_overlay.c").read_text(encoding="utf-8")
    overlay_palette = overlay[overlay.index("#define RGB565_CONST"):
                              overlay.index("static linear_text_overlay_config_t")]
    overlay_drawing = overlay[overlay.index("static const uint8_t *glyph_for"):]
    source = BUILD / "display_scaling.c"
    source.write_text(r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fb16.h"
#include "compositor_layout.h"
#include "display_modes.h"
#include "video_mono.h"
#include "video_blur.h"
#include "video_glow.h"
#include "video_ghosting.h"
#include "scanlines.h"
#include "linear_text_overlay.h"
#include "linear_text_overlay_font.h"
#include "apple_fb_handoff.h"
static int smartport_service_has_pending(void) { return 0; }
static void smartport_service_poll(void) {}
static uint8_t s_video_dot_bleed = APPLETINI_VIDEO_DOT_BLEED_LIGHT;
static uint8_t s_force_full_refresh;
''' + effects + size_api + borders + r'''
static uint32_t test_apple[COMP_APPLE_SLOT_BYTES / sizeof(uint32_t)];
static uintptr_t test_apple_slots[COMP_APPLE_SLOT_COUNT];
#define comp_apple_slot_addr test_apple_slots
static uint32_t test_display_mode, test_format_detail;
static uint8_t s_border_enabled, s_border_flood, s_scanlines_mode;
static uint8_t s_video_ghosting_strength, s_video_blur_strength, s_video_glow_strength;
static uint8_t s_format_badge_enabled;
volatile uint32_t g_compositor_last_apple_slot, g_compositor_last_apple_mode;
uint32_t apple_fb_reader_display_mode(void) { return test_display_mode; }
uint32_t apple_fb_reader_format_detail(void) { return test_format_detail; }
uint8_t apple_fb_reader_border_color(void) { return 12; }
static int compositor_apple_effects_active(void) { return s_video_ghosting_strength != 0; }
static void draw_format_badge(uint16_t *fb, int x, int y, int w) {
    (void)fb; (void)x; (void)y; (void)w;
}
''' + apple_drawing + overlay_palette + r'''
static linear_text_overlay_config_t s_active;
static uint8_t s_visible = 1;
static const uint8_t overlay_cells[] = {'A', 0x1E, 'B', 0x42, 'C', 0x3F, 'D', 0x65};
static void process_frame_request(void) {}
static uint8_t blink_phase_on(void) { return 1; }
static const volatile uint8_t *shadow_slot(uint8_t slot) { (void)slot; return overlay_cells; }
#define REG_READ(reg) ((void)(reg), 0U)
''' + overlay_drawing + r'''
#define GUARD 128
#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); exit(1); } } while (0)
static uint16_t *storage, *fb;
static uint32_t src[648 * 400];
static uint16_t overlay_reference[1280 * 800];

static void reset_output(void) {
    for (size_t i = 0; i < (size_t)FB16_MAX_WIDTH * FB16_MAX_HEIGHT + 2 * GUARD; ++i)
        storage[i] = 0x1357;
}
static void check_guards(void) {
    for (int i = 0; i < GUARD; ++i) CHECK(storage[i] == 0x1357);
    for (size_t i = GUARD + (size_t)FB16_WIDTH * FB16_HEIGHT;
         i < GUARD + (size_t)FB16_MAX_WIDTH * FB16_MAX_HEIGHT + GUARD; ++i)
        CHECK(storage[i] == 0x1357);
}
static int blank(unsigned phase, unsigned scale, unsigned scan) {
    if (scale == 4) return scan != 0 && phase >= 4 - scan;
    return scale == 2 && scan >= 2 && phase == 1;
}
static void check_blit(const comp_viewport_t *v, int shr, int woven,
                        unsigned scan, int effects_on) {
    const int sx = v->scale, sy = (shr || woven) ? sx : 2 * sx;
    const int w = shr ? 640 : woven ? 560 : 616;
    const int h = shr ? 400 : woven ? 384 : 224;
    const int stride = w + 8;
    const int ox = (shr || woven) ? v->x : v->border_x;
    const int oy = (shr || woven) ? v->y : v->border_y;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < stride; ++x)
            src[y * stride + x] = 0xFF000000U |
                ((uint32_t)((x * 17) & 255) << 16) |
                ((uint32_t)((y * 13) & 255) << 8) | ((x + y) & 255);
    reset_output();
    if (effects_on) {
        effect_clear_history();
        s_mono_width = 0;
        blit_apple_effects_scaled(fb, ox, oy, src, w, h, stride, sx, sy,
                                   scan, APPLETINI_VIDEO_GHOSTING_LIGHT, 0, 0);
    } else {
        blit_apple_scaled_serviced(fb, ox, oy, src, w, h, stride, sx, sy, scan);
    }
    check_guards();
    for (int y = 0; y < FB16_HEIGHT; ++y) {
        for (int x = 0; x < FB16_WIDTH; ++x) {
            uint16_t want = 0x1357;
            if (x >= ox && x < ox + w * sx && y >= oy && y < oy + h * sy) {
                want = blank((y - oy) % sy, sy, scan) ? 0 :
                    fb16_from_bgra32(src[((y - oy) / sy) * stride + (x - ox) / sx]);
            }
            CHECK(fb[y * FB16_WIDTH + x] == want);
        }
    }
}
static void check_shr_border(const comp_viewport_t *v, unsigned scan) {
    reset_output();
    draw_solid_border_ring(fb, v->border_x, v->border_y,
                             v->border_width, v->border_height,
                             v->x, v->y, v->width, v->height,
                             0xFFFF, scan, v->scale);
    draw_border_flood(fb, v->border_x, v->border_y,
                       v->border_width, v->border_height,
                       0xFFFF, scan, v->scale);
    check_guards();
    for (int y = 0; y < FB16_HEIGHT; ++y) {
        for (int x = 0; x < FB16_WIDTH; ++x) {
            const int active = x >= v->x && x < v->x + v->width &&
                y >= v->y && y < v->y + v->height;
            const unsigned phase = (unsigned)(y - v->border_y) & (v->scale - 1U);
            const uint16_t want = active ? 0x1357 : blank(phase, v->scale, scan) ? 0 : 0xFFFF;
            CHECK(fb[y * FB16_WIDTH + x] == want);
        }
    }
}
static void check_overlay(int shr) {
    const int width = FB16_WIDTH, height = FB16_HEIGHT;
    const uint8_t preference = comp_layout_size_multiplier();
    const int canvas_w = shr ? 1280 : 1120;
    const int canvas_h = shr ? 800 : 768;
    /* Reference pixels always use the original 2x logical canvas. */
    comp_layout_set_size_multiplier(0);
    CHECK(comp_layout_set_output_size(1920, 1080));
    CHECK(fb16_set_size(1920, 1080));
    comp_viewport_t v = shr ? comp_shr_viewport : comp_legacy_viewport;
    memset(&s_active, 0, sizeof(s_active));
    s_active.config = LTO_CONFIG_CP437 | LTO_CONFIG_FONT_8X16;
    s_active.cols = s_active.rows = 2;
    s_active.origin_x = 5;
    s_active.origin_y = 7;
    s_active.scale = 0x12;
    reset_output();
    linear_text_overlay_draw(fb, shr);
    check_guards();
    for (int y = 0; y < canvas_h; ++y)
        memcpy(overlay_reference + y * canvas_w,
               fb + (y + v.y) * FB16_WIDTH + v.x, canvas_w * sizeof(*fb));
    comp_layout_set_size_multiplier(preference);
    CHECK(comp_layout_set_output_size(width, height));
    CHECK(fb16_set_size(width, height));
    v = shr ? comp_shr_viewport : comp_legacy_viewport;
    reset_output();
    linear_text_overlay_draw(fb, shr);
    check_guards();
    for (int y = 0; y < FB16_HEIGHT; ++y) {
        for (int x = 0; x < FB16_WIDTH; ++x) {
            uint16_t want = 0x1357;
            if (x >= v.x && x < v.x + v.width && y >= v.y && y < v.y + v.height)
                want = overlay_reference[((y - v.y) * 2 / v.scale) * canvas_w +
                                          (x - v.x) * 2 / v.scale];
            CHECK(fb[y * FB16_WIDTH + x] == want);
        }
    }
}
static void check_small_effects(void) {
    uint16_t reference[5][18];
    for (int i = 0; i < 45; ++i)
        src[i] = 0xFF000000U | ((i * 87159U) & 0xFFFFFFU);
    for (unsigned mono = 0; mono < 2; ++mono) {
        s_mono_x = 1;
        s_mono_y = 0;
        s_mono_width = mono ? 7 : 0;
        s_mono_height = 5;
        s_mono_channel_shift = video_mono_channel_shift(3);
        video_mono_build_tint(s_mono_tint, 3);
        for (unsigned blur = 0; blur < 4; ++blur) {
            for (unsigned glow = 0; glow < 4; ++glow) {
                reset_output();
                effect_clear_history();
                blit_apple_effects_scaled(fb, 0, 0, src, 9, 5, 9, 2, 2, 0,
                                           APPLETINI_VIDEO_GHOSTING_LIGHT, blur, glow);
                for (int y = 0; y < 5; ++y)
                    memcpy(reference[y], fb + 2 * y * FB16_WIDTH, sizeof(reference[y]));
                reset_output();
                effect_clear_history();
                blit_apple_effects_scaled(fb, -3, -1, src, 9, 5, 9, 1, 1, 3,
                                           APPLETINI_VIDEO_GHOSTING_LIGHT, blur, glow);
                check_guards();
                for (int y = 0; y < 7; ++y) {
                    for (int x = 0; x < 12; ++x) {
                        uint16_t want = 0x1357;
                        if (y < 4 && x < 6) {
                            const uint16_t a = reference[y + 1][2 * (x + 3)];
                            const uint16_t b = reference[y + 1][2 * (x + 3) + 1];
                            const unsigned r = (((a >> 11) & 31) + ((b >> 11) & 31)) / 2;
                            const unsigned g = (((a >> 5) & 63) + ((b >> 5) & 63)) / 2;
                            const unsigned blue = ((a & 31) + (b & 31)) / 2;
                            want = (r << 11) | (g << 5) | blue;
                        }
                        CHECK(fb[y * FB16_WIDTH + x] == want);
                    }
                }
            }
        }
    }
    s_mono_width = 0;
}
static void check_claimed_border_modes(void) {
    const comp_viewport_t *v = &comp_legacy_viewport;
    const uint32_t active = 0xFF224466U;
    const uint32_t raster = 0xFFF02244U;
    const uint16_t active565 = fb16_from_bgra32(active);
    const uint16_t raster565 = fb16_from_bgra32(raster);
    const uint16_t solid565 = fb16_from_bgra32(apple_video_iigs_border_bgra(12));
    test_apple_slots[0] = (uintptr_t)test_apple;
    for (unsigned presentation = 0; presentation < 3; ++presentation) {
        const int woven = presentation == 1;
        const int synthetic = presentation != 0;
        const unsigned sy = woven ? v->scale : 2U * v->scale;
        test_display_mode = woven ? APPLE_FB_DISPLAY_MODE_LEGACY_I : APPLE_FB_DISPLAY_MODE_LEGACY;
        test_format_detail = APPLE_FB_FORMAT_DETAIL(APPLE_FB_FORMAT_HGR, 0, presentation);
        for (size_t i = 0; i < sizeof(test_apple) / sizeof(test_apple[0]); ++i)
            test_apple[i] = synthetic ? 0 : raster;
        const int source_y = woven ? 0 : COMP_APPLE_ACTIVE_Y;
        for (int y = 0; y < (woven ? 384 : 192); ++y)
            for (int x = 0; x < 560; ++x)
                test_apple[(source_y + y) * COMP_APPLE_ROW_PIXELS + COMP_APPLE_ACTIVE_X + x] = active;
        for (unsigned borders_on = 0; borders_on < 3; ++borders_on) {
            s_border_enabled = borders_on != 0;
            s_border_flood = borders_on == 2;
            for (unsigned scan = 0; scan < 4; scan += 3) {
                s_scanlines_mode = scan;
                for (unsigned fx = 0; fx < 2; ++fx) {
                    s_video_ghosting_strength = fx;
                    effect_clear_history();
                    reset_output();
                    CHECK(draw_apple_subwindow(fb, 0));
                    check_guards();
                    for (int y = 0; y < FB16_HEIGHT; ++y) {
                        for (int x = 0; x < FB16_WIDTH; ++x) {
                            uint16_t want = 0x1357;
                            const int in_active = x >= v->x && x < v->x + v->width &&
                                y >= v->y && y < v->y + v->height;
                            const int in_ring = x >= v->border_x && x < v->border_x + v->border_width &&
                                y >= v->border_y && y < v->border_y + v->border_height;
                            const unsigned phase = (unsigned)(y - v->border_y) & (sy - 1U);
                            if (in_active) want = blank(phase, sy, scan) ? 0 : active565;
                            else if (s_border_enabled && in_ring)
                                want = blank(phase, sy, scan) ? 0 : synthetic ? solid565 : raster565;
                            else if (s_border_enabled && s_border_flood)
                                want = blank(phase, sy, scan) ? 0 : solid565;
                            CHECK(fb[y * FB16_WIDTH + x] == want);
                        }
                    }
                }
            }
        }
    }
    s_video_ghosting_strength = 0;
}
int main(void) {
    /* Explicit catalog expectations catch ID or mode-list drift. */
    static const int expected[][4] = {
        {1024, 768, 1, 1}, {1200, 800, 2, 1}, {1280, 1024, 2, 2},
        {1680, 1050, 2, 2}, {1920, 1080, 2, 2}, {1360, 768, 2, 1}
    };
    CHECK(DISPLAY_MODE_COUNT == sizeof(expected) / sizeof(expected[0]));
    storage = malloc(((size_t)FB16_MAX_WIDTH * FB16_MAX_HEIGHT + 2 * GUARD) * sizeof(*fb));
    CHECK(storage != NULL);
    fb = storage + GUARD;
    CHECK(compositor_size_multiplier() == 0);
    for (unsigned preference = 0; preference <= 2; ++preference) {
    s_force_full_refresh = 0;
    s_effect_history[0] = 0xFF123456;
    compositor_set_size_multiplier(preference);
    CHECK(compositor_size_multiplier() == preference);
    if (preference != 0) {
        CHECK(s_force_full_refresh == 1);
        CHECK(s_effect_history[0] == 0);
    }
    for (unsigned mode = 0; mode < DISPLAY_MODE_COUNT; ++mode) {
        const display_mode_t *d = display_mode_get(mode);
        const int legacy_scale = preference == 1 ? 1 : expected[mode][2];
        const int shr_scale = preference == 1 ? 1 : expected[mode][3];
        CHECK(d->width == expected[mode][0] && d->height == expected[mode][1]);
        CHECK(comp_layout_set_output_size(d->width, d->height));
        CHECK(fb16_set_size(d->width, d->height));
        CHECK(comp_layout_size_multiplier() == preference);
        CHECK(comp_legacy_viewport.scale == legacy_scale);
        CHECK(comp_shr_viewport.scale == shr_scale);
        CHECK(comp_legacy_viewport.x == (d->width - 560 * legacy_scale) / 2);
        CHECK(comp_legacy_viewport.y == (d->height - 384 * legacy_scale) / 2);
        CHECK(comp_shr_viewport.x == (d->width - 640 * shr_scale) / 2);
        CHECK(comp_shr_viewport.y == (d->height - 400 * shr_scale) / 2);
        CHECK(comp_legacy_viewport.border_width == 616 * legacy_scale);
        CHECK(comp_legacy_viewport.border_height == 448 * legacy_scale);
        CHECK(comp_shr_viewport.border_width == 696 * shr_scale);
        CHECK(comp_shr_viewport.border_height == 464 * shr_scale);
        reset_output();
        fb16_clear(fb, 0xABCD);
        check_guards();
        for (int i = 0; i < FB16_WIDTH * FB16_HEIGHT; ++i) CHECK(fb[i] == 0xABCD);
        for (unsigned scan = 0; scan < 4; ++scan) {
            for (int fx = 0; fx < 2; ++fx) {
                check_blit(&comp_legacy_viewport, 0, 0, scan, fx);
                check_blit(&comp_legacy_viewport, 0, 1, scan, fx);
                check_blit(&comp_shr_viewport, 1, 0, scan, fx);
            }
            check_shr_border(&comp_shr_viewport, scan);
        }
        check_overlay(0);
        check_overlay(1);
        check_claimed_border_modes();
        printf("PASS %s preference %s: centered scale, borders, scanlines, effects, text overlay and guards\n",
               d->name, preference == 0 ? "Max" : preference == 1 ? "1x" : "2x");
    }
    }
    compositor_set_size_multiplier(255);
    CHECK(compositor_size_multiplier() == 0);
    CHECK(comp_layout_set_output_size(1920, 1080));
    CHECK(fb16_set_size(1920, 1080));
    CHECK(!comp_layout_set_output_size(1920, 1200));
    CHECK(!fb16_set_size(1920, 1200));
    CHECK(FB16_WIDTH == 1920 && FB16_HEIGHT == 1080);
    CHECK(comp_legacy_viewport.border_x == 344 && comp_legacy_viewport.border_y == 92);
    CHECK(comp_shr_viewport.border_x == 264 && comp_shr_viewport.border_y == 76);
    check_small_effects();
    puts("PASS clipped 1x RGB/mono output with every blur/glow combination");
    free(storage);
    return 0;
}
''', encoding="utf-8")
    binary = BUILD / ("display_scaling.exe" if os.name == "nt" else "display_scaling")
    env = os.environ.copy()
    env["PATH"] = str(Path(compiler).resolve().parent) + os.pathsep + env["PATH"]
    subprocess.run([compiler, "-std=c11", "-O2", "-funsigned-char", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-function", "-Wno-unused-variable",
                    "-I" + str(ROOT / "ps_sources/frontend"),
                    "-I" + str(ROOT / "ps_sources/lib"), str(source),
                    str(ROOT / "ps_sources/lib/fb16.c"),
                    str(ROOT / "ps_sources/frontend/compositor_layout.c"),
                    str(ROOT / "ps_sources/frontend/linear_text_overlay_font.c"),
                    "-o", str(binary)], check=True, env=env)
    subprocess.run([str(binary)], check=True, env=env)


if __name__ == "__main__":
    main()
