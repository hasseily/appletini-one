#!/usr/bin/env python3
"""Exercise production background helpers through Apple mode and size changes."""

import os
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build" / "display_mode_transition_test"


def function(source, name):
    match = re.search(r"^(?:static\s+)?[\w *]+\b" + name + r"\s*\([^;]*?\)\s*\{",
                      source, re.MULTILINE)
    if not match:
        raise AssertionError(f"missing function: {name}")
    depth = 0
    for end in range(source.index("{", match.start()), len(source)):
        depth += (source[end] == "{") - (source[end] == "}")
        if depth == 0:
            return source[match.start():end + 1]
    raise AssertionError(f"unterminated function: {name}")


def check_claim_order(compositor):
    source = re.sub(r"/\*.*?\*/|//[^\n]*", "", compositor, flags=re.DOTALL)
    tick = function(source, "compositor_tick")
    draw = function(source, "draw_apple_subwindow")
    claims = list(re.finditer(r"\bapple_fb_reader_claim\s*\(", tick))
    assert len(claims) == 1, "each compositor pass must claim the Apple frame once"
    base = tick.index("COMPOSITOR_UI_PHASE_BASE")
    callback = tick.rfind("s_ui_draw(", 0, base)
    assert callback >= 0 and claims[0].start() < callback, "claim must precede BASE callback"
    assert "apple_fb_reader_claim" not in draw, "Apple blit must use the frame claimed before BASE"
    assignment = re.search(r"\b(\w+)\s*=\s*apple_fb_reader_claim\s*\(\s*\)", tick)
    assert assignment and re.search(
        r"draw_apple_subwindow\s*\(\s*fb\s*,\s*" + assignment[1] + r"\s*\)", tick
    ), "Apple blit must receive the same slot used for BASE metadata"
    print("PASS one Apple frame claim before BASE, shared with the Apple blit")


def check_size_integration(frontend):
    source = re.sub(r"/\*.*?\*/|//[^\n]*", "", frontend, flags=re.DOTALL)
    main = function(source, "main")
    apply = list(re.finditer(r"ui_apply_size_multiplier\(&config_menu\)", main))
    assert len(apply) == 2, "apply size preference at boot and in the main loop"
    ordered = re.findall(
        r"ui_apply_output_mode\(&config_menu,\s*[01]U\);\s*"
        r"ui_apply_size_multiplier\(&config_menu\);", main
    )
    assert len(ordered) == 2, "apply size preference after output dimensions are updated"
    assert main.find("compositor_tick()", apply[-1].end()) >= 0, "apply size before composing"
    thunk = function(source, "ui_compose_thunk")
    assert "compositor_full_refresh_active()" in thunk and \
        thunk.index("ui_invalidate_static_backgrounds()") < thunk.index("ui_compose_frame("), \
        "forced refresh must invalidate slot backgrounds before painting"
    print("PASS size integration at boot/main loop and forced-refresh background fallback")


HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fb16.h"
#include "compositor_layout.h"
#include "display_modes.h"
#include "apple_fb_handoff.h"

/* Use the production viewport calculation, with native test allocations in
 * place of fixed DDR addresses for main.c's per-slot bookkeeping. */
#define comp_out_addr_to_slot unused_ddr_addr_to_slot
#include "compositor_layout.c"
#undef comp_out_addr_to_slot

#define GUARD 64U
#define MAX_PIXELS ((size_t)FB16_MAX_WIDTH * FB16_MAX_HEIGHT)
#define GUARD_VALUE 0xE739U
#define OLD_APPLE 0xF81FU
#define NEW_APPLE 0x07E0U
#define OLD_FLOOD 0xA55AU
#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "FAIL mode %u bezel %u border %u slot %u line %d: %s\n", \
            test_mode, test_bezel, test_border, test_slot, __LINE__, #cond); exit(1); \
} } while (0)

static unsigned test_mode, test_bezel, test_border, test_slot;
static uint16_t *storage[COMP_OUT_SLOT_COUNT], *frames[COMP_OUT_SLOT_COUNT];
static uint16_t *g_bezel_565;
static unsigned g_bezel_width, g_bezel_height;
static uint32_t g_bezel_generation = 7U;
static uint32_t g_output_slot_bg_generation[COMP_OUT_SLOT_COUNT];
static uint8_t g_output_slot_bg_show_bezel[COMP_OUT_SLOT_COUNT];
static uint8_t g_output_slot_apple_mode[COMP_OUT_SLOT_COUNT];
static uint8_t g_output_slot_debug_dirty[COMP_OUT_SLOT_COUNT];
static uint32_t claimed_mode, published_mode;
typedef struct { uint8_t size_multiplier; } config_menu_t;
static uint8_t g_storage_activity[64], s_force_full_refresh;
static unsigned effect_history_clears;
static uint8_t config_menu_size_multiplier(const config_menu_t *menu)
{
    return menu->size_multiplier;
}
static void effect_clear_history(void) { ++effect_history_clears; }

/* PRODUCTION_SIZE_API */

uint8_t comp_out_addr_to_slot(uint32_t address)
{
    for (uint8_t i = 0; i < COMP_OUT_SLOT_COUNT; ++i)
        if ((uint32_t)(uintptr_t)frames[i] == address) return i;
    return 0xFFU;
}

uint32_t apple_fb_reader_display_mode(void) { return claimed_mode; }
uint32_t apple_fb_reader_published_display_mode(void) { return published_mode; }

/* PRODUCTION_HELPERS */

static size_t pixels(void) { return (size_t)FB16_WIDTH * FB16_HEIGHT; }

static uint16_t bezel_pixel(int x, int y)
{
    /* Distinct rows and columns catch wrong pitch and partial restores. */
    return (uint16_t)(((unsigned)x * 13U + (unsigned)y * 211U) & 0x7FFFU);
}

static uint16_t background_pixel(int x, int y)
{
    if (test_bezel == 0U) return FB16_COLOR_BLACK;
    if (test_bezel == 3U || (unsigned)y >= g_bezel_height) return UI_BEZEL_BG_COLOR;
    return bezel_pixel(x, y);
}

static void check_guards(void)
{
    for (unsigned slot = 0; slot < COMP_OUT_SLOT_COUNT; ++slot) {
        for (size_t i = 0; i < GUARD; ++i) CHECK(storage[slot][i] == GUARD_VALUE);
        for (size_t i = GUARD + pixels(); i < MAX_PIXELS + 2U * GUARD; ++i)
            CHECK(storage[slot][i] == GUARD_VALUE);
    }
}

static void check_background(uint16_t *fb)
{
    for (int y = 0; y < FB16_HEIGHT; ++y)
        for (int x = 0; x < FB16_WIDTH; ++x)
            CHECK(fb[(size_t)y * FB16_WIDTH + x] == background_pixel(x, y));
}

static void fill(uint16_t *fb, uint16_t color)
{
    for (size_t i = 0; i < pixels(); ++i) fb[i] = color;
}

static comp_viewport_t viewport(uint32_t mode)
{
    return mode == APPLE_FB_DISPLAY_MODE_SHR ? comp_shr_viewport : comp_legacy_viewport;
}

static int in_picture(int x, int y, comp_viewport_t v)
{
    if (test_border)
        return x >= v.border_x && x < v.border_x + v.border_width &&
               y >= v.border_y && y < v.border_y + v.border_height;
    return x >= v.x && x < v.x + v.width && y >= v.y && y < v.y + v.height;
}

static void paint_picture(uint16_t *fb, uint32_t mode, uint16_t color)
{
    const comp_viewport_t v = viewport(mode);
    fb16_fill_rect(fb, test_border ? v.border_x : v.x, test_border ? v.border_y : v.y,
                       test_border ? v.border_width : v.width,
                       test_border ? v.border_height : v.height, color);
}

static void check_picture(uint16_t *fb, uint32_t mode)
{
    const comp_viewport_t v = viewport(mode);
    for (int y = 0; y < FB16_HEIGHT; ++y)
        for (int x = 0; x < FB16_WIDTH; ++x) {
            const uint16_t want = in_picture(x, y, v) ? NEW_APPLE : background_pixel(x, y);
            CHECK(fb[(size_t)y * FB16_WIDTH + x] == want);
        }
}

static void setup(void)
{
    g_bezel_width = (unsigned)FB16_WIDTH - (test_bezel == 3U ? 1U : 0U);
    g_bezel_height = test_bezel == 2U ? (unsigned)FB16_HEIGHT / 3U : (unsigned)FB16_HEIGHT;
    for (unsigned y = 0; y < g_bezel_height; ++y)
        for (unsigned x = 0; x < g_bezel_width; ++x)
            g_bezel_565[(size_t)y * g_bezel_width + x] = bezel_pixel((int)x, (int)y);
    ui_invalidate_static_backgrounds();
    for (unsigned slot = 0; slot < COMP_OUT_SLOT_COUNT; ++slot) {
        for (size_t i = 0; i < MAX_PIXELS + 2U * GUARD; ++i)
            storage[slot][i] = GUARD_VALUE;
        ui_prepare_static_background(frames[slot], test_bezel != 0U);
        check_background(frames[slot]);
    }
    check_guards();
}

static void exercise_transition(uint32_t prior_mode, uint32_t next_mode)
{
    /* Give every slot the prior frame before reusing each independently.
     * Fill beyond its Apple rectangle too: border flood can paint that area. */
    for (unsigned slot = 0; slot < COMP_OUT_SLOT_COUNT; ++slot) {
        claimed_mode = prior_mode;
        published_mode = prior_mode;
        ui_restore_apple_footprint_if_needed(frames[slot], test_bezel != 0U);
        fill(frames[slot], OLD_FLOOD);
        paint_picture(frames[slot], prior_mode, OLD_APPLE);
    }
    for (test_slot = 0; test_slot < COMP_OUT_SLOT_COUNT; ++test_slot) {
        /* Renderer publication can differ from the already claimed frame.
         * Restoration must follow the exact frame the blit will consume. */
        claimed_mode = next_mode;
        published_mode = prior_mode;
        ui_prepare_static_background(frames[test_slot], test_bezel != 0U);
        ui_restore_apple_footprint_if_needed(frames[test_slot], test_bezel != 0U);
        check_background(frames[test_slot]);
        paint_picture(frames[test_slot], next_mode, NEW_APPLE);
        check_picture(frames[test_slot], next_mode);
        CHECK(g_output_slot_apple_mode[test_slot] ==
              (next_mode == APPLE_FB_DISPLAY_MODE_SHR ? APPLE_FB_DISPLAY_MODE_SHR : APPLE_FB_DISPLAY_MODE_LEGACY));
        for (unsigned other = 0; other < COMP_OUT_SLOT_COUNT; ++other) {
            if (other < test_slot) check_picture(frames[other], next_mode);
            if (other > test_slot) {
                CHECK(frames[other][0] ==
                      (in_picture(0, 0, viewport(prior_mode)) ? OLD_APPLE : OLD_FLOOD));
                CHECK(g_output_slot_apple_mode[other] ==
                      (prior_mode == APPLE_FB_DISPLAY_MODE_SHR ? APPLE_FB_DISPLAY_MODE_SHR : APPLE_FB_DISPLAY_MODE_LEGACY));
            }
        }
        check_guards();
    }
}

static void exercise_unchanged(void)
{
    for (test_slot = 0; test_slot < COMP_OUT_SLOT_COUNT; ++test_slot) {
        const uint32_t modes[] = {APPLE_FB_DISPLAY_MODE_LEGACY,
                                 APPLE_FB_DISPLAY_MODE_LEGACY_I,
                                 APPLE_FB_DISPLAY_MODE_LEGACY};
        claimed_mode = APPLE_FB_DISPLAY_MODE_LEGACY;
        ui_restore_apple_footprint_if_needed(frames[test_slot], test_bezel != 0U);
        fill(frames[test_slot], OLD_FLOOD);
        for (unsigned i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
            claimed_mode = modes[i];
            published_mode = APPLE_FB_DISPLAY_MODE_SHR;
            ui_prepare_static_background(frames[test_slot], test_bezel != 0U);
            ui_restore_apple_footprint_if_needed(frames[test_slot], test_bezel != 0U);
            for (size_t p = 0; p < pixels(); ++p) CHECK(frames[test_slot][p] == OLD_FLOOD);
            CHECK(g_output_slot_apple_mode[test_slot] == APPLE_FB_DISPLAY_MODE_LEGACY);
        }
    }
    check_guards();
}

static void check_size_geometry(uint8_t requested)
{
    const int legacy_x = FB16_WIDTH / 560, legacy_y = FB16_HEIGHT / 384;
    const int shr_x = FB16_WIDTH / 640, shr_y = FB16_HEIGHT / 400;
    int legacy = legacy_x < legacy_y ? legacy_x : legacy_y;
    int shr = shr_x < shr_y ? shr_x : shr_y;
    if (requested != 0U && requested < legacy) legacy = requested;
    if (requested != 0U && requested < shr) shr = requested;
    CHECK(comp_legacy_viewport.scale == legacy && comp_shr_viewport.scale == shr);
    CHECK(comp_legacy_viewport.width == 560 * legacy && comp_legacy_viewport.height == 384 * legacy);
    CHECK(comp_shr_viewport.width == 640 * shr && comp_shr_viewport.height == 400 * shr);
    CHECK(comp_legacy_viewport.x == (FB16_WIDTH - 560 * legacy) / 2);
    CHECK(comp_legacy_viewport.y == (FB16_HEIGHT - 384 * legacy) / 2);
    CHECK(comp_shr_viewport.x == (FB16_WIDTH - 640 * shr) / 2);
    CHECK(comp_shr_viewport.y == (FB16_HEIGHT - 400 * shr) / 2);
    CHECK(compositor_size_multiplier() == requested);
}

static void exercise_size_changes(uint32_t apple_mode)
{
    /* Keep the Apple display mode fixed: its per-slot mode tag cannot
     * identify these geometry changes. The real main callback must mark
     * every background dirty before the next use of each output buffer. */
    static const uint8_t sequence[] = {0U, 1U, 2U, 1U, 0U};
    config_menu_t menu = {0U};
    ui_apply_size_multiplier(&menu);
    setup();
    claimed_mode = published_mode = apple_mode;
    for (unsigned step = 1; step < sizeof(sequence); ++step) {
        const comp_viewport_t old_view = viewport(apple_mode);
        for (unsigned slot = 0; slot < COMP_OUT_SLOT_COUNT; ++slot) {
            ui_prepare_static_background(frames[slot], test_bezel != 0U);
            ui_restore_apple_footprint_if_needed(frames[slot], test_bezel != 0U);
            fill(frames[slot], OLD_FLOOD);
            paint_picture(frames[slot], apple_mode, OLD_APPLE);
            g_output_slot_debug_dirty[slot] = 1U;
        }
        /* An unchanged menu preference must not clear history or force a
         * full-frame repaint on every normal compositor pass. */
        s_force_full_refresh = 0U;
        const unsigned old_clears = effect_history_clears;
        ui_apply_size_multiplier(&menu);
        CHECK(s_force_full_refresh == 0U && effect_history_clears == old_clears);
        for (unsigned slot = 0; slot < COMP_OUT_SLOT_COUNT; ++slot)
            CHECK(g_output_slot_bg_generation[slot] != 0U);

        memset(g_storage_activity, 0xA5, sizeof(g_storage_activity));
        menu.size_multiplier = sequence[step];
        ui_apply_size_multiplier(&menu);
        CHECK(menu.size_multiplier == sequence[step]);
        CHECK(s_force_full_refresh != 0U && effect_history_clears == old_clears + 1U);
        check_size_geometry(sequence[step]);
        for (size_t i = 0; i < sizeof(g_storage_activity); ++i)
            CHECK(g_storage_activity[i] == 0U);
        for (unsigned slot = 0; slot < COMP_OUT_SLOT_COUNT; ++slot) {
            CHECK(g_output_slot_bg_generation[slot] == 0U);
            CHECK(g_output_slot_debug_dirty[slot] == 0U);
        }
        for (test_slot = 0; test_slot < COMP_OUT_SLOT_COUNT; ++test_slot) {
            ui_prepare_static_background(frames[test_slot], test_bezel != 0U);
            ui_restore_apple_footprint_if_needed(frames[test_slot], test_bezel != 0U);
            check_background(frames[test_slot]);
            paint_picture(frames[test_slot], apple_mode, NEW_APPLE);
            check_picture(frames[test_slot], apple_mode);
            CHECK(g_output_slot_bg_generation[test_slot] != 0U);
            for (unsigned other = 0; other < COMP_OUT_SLOT_COUNT; ++other) {
                if (other < test_slot) check_picture(frames[other], apple_mode);
                if (other > test_slot) {
                    CHECK(g_output_slot_bg_generation[other] == 0U);
                    CHECK(frames[other][0] ==
                          (in_picture(0, 0, old_view) ? OLD_APPLE : OLD_FLOOD));
                }
            }
            check_guards();
        }
        /* SHR at 1200x800 and 1360x768 clamps 2x to 1x without changing
         * the stored preference; switching back to legacy still gets 2x. */
        CHECK(compositor_size_multiplier() == menu.size_multiplier);
    }
}

int main(void)
{
    for (unsigned slot = 0; slot < COMP_OUT_SLOT_COUNT; ++slot) {
        storage[slot] = malloc((MAX_PIXELS + 2U * GUARD) * sizeof(uint16_t));
        CHECK(storage[slot] != NULL);
        frames[slot] = storage[slot] + GUARD;
    }
    g_bezel_565 = malloc(MAX_PIXELS * sizeof(uint16_t));
    CHECK(g_bezel_565 != NULL);
    for (test_mode = 0; test_mode < DISPLAY_MODE_COUNT; ++test_mode) {
        const display_mode_t *mode = display_mode_get((uint8_t)test_mode);
        comp_layout_set_size_multiplier(0U);
        CHECK(comp_layout_set_output_size(mode->width, mode->height));
        CHECK(fb16_set_size(mode->width, mode->height));
        if ((mode->width == 1200U && mode->height == 800U) ||
            (mode->width == 1360U && mode->height == 768U)) {
            CHECK(comp_legacy_viewport.scale == 2U && comp_shr_viewport.scale == 1U);
            CHECK(comp_legacy_viewport.width > comp_shr_viewport.width);
            CHECK(comp_legacy_viewport.height > comp_shr_viewport.height);
        }
        for (test_bezel = 0; test_bezel < 4; ++test_bezel) {
            for (test_border = 0; test_border < 2; ++test_border) {
                setup();
                exercise_transition(APPLE_FB_DISPLAY_MODE_LEGACY, APPLE_FB_DISPLAY_MODE_SHR);
                exercise_transition(APPLE_FB_DISPLAY_MODE_SHR, APPLE_FB_DISPLAY_MODE_LEGACY);
                exercise_transition(APPLE_FB_DISPLAY_MODE_LEGACY_I, APPLE_FB_DISPLAY_MODE_SHR);
                exercise_transition(APPLE_FB_DISPLAY_MODE_SHR, APPLE_FB_DISPLAY_MODE_LEGACY_I);
                exercise_unchanged();
                exercise_size_changes(APPLE_FB_DISPLAY_MODE_LEGACY);
                exercise_size_changes(APPLE_FB_DISPLAY_MODE_SHR);
                exercise_size_changes(APPLE_FB_DISPLAY_MODE_LEGACY_I);
            }
        }
        printf("PASS %s: all three slots, legacy/SHR both ways, bezel/border variants, frame coherence and guards\n", mode->name);
        printf("PASS %s: same-mode Max/1x/2x shrink/grow, independent fit and per-slot background/overlay cleanup\n", mode->name);
    }
    ui_restore_apple_footprint_if_needed(NULL, 0U);
    for (unsigned slot = 0; slot < COMP_OUT_SLOT_COUNT; ++slot) free(storage[slot]);
    free(g_bezel_565);
    return 0;
}
'''


def main():
    frontend = (ROOT / "ps_sources/frontend/main.c").read_text(encoding="utf-8")
    compositor = (ROOT / "ps_sources/frontend/compositor.c").read_text(encoding="utf-8")
    check_claim_order(compositor)
    check_size_integration(frontend)
    helpers = "\n\n".join(function(frontend, name) for name in (
        "ui_bezel_matches_output", "ui_draw_bezel_image", "ui_draw_bezel",
        "ui_invalidate_static_backgrounds", "ui_restore_static_rect",
        "ui_prepare_static_background", "ui_restore_apple_footprint_if_needed",
        "ui_apply_size_multiplier",
    ))
    size_api = "\n\n".join(function(compositor, name) for name in (
        "compositor_set_size_multiplier", "compositor_size_multiplier",
    ))
    color = re.search(r"^#define UI_BEZEL_BG_COLOR[^\n]+", frontend, re.MULTILINE)
    assert color, "missing production bezel background color"
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
    source = BUILD / "display_mode_transition.c"
    source.write_text(HARNESS.replace("/* PRODUCTION_SIZE_API */", size_api).replace(
                          "/* PRODUCTION_HELPERS */", color[0] + "\n" + helpers),
                      encoding="utf-8")
    binary = BUILD / ("display_mode_transition.exe" if os.name == "nt" else "display_mode_transition")
    env = os.environ.copy()
    env["PATH"] = str(Path(compiler).resolve().parent) + os.pathsep + env["PATH"]
    subprocess.run([
        compiler, "-std=c11", "-O2", "-funsigned-char", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
        "-I" + str(ROOT / "ps_sources/frontend"), "-I" + str(ROOT / "ps_sources/lib"),
        str(source), str(ROOT / "ps_sources/lib/fb16.c"), "-o", str(binary),
    ], check=True, env=env)
    subprocess.run([str(binary)], check=True, env=env)


if __name__ == "__main__":
    main()
