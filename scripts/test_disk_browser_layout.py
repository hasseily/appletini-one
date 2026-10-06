#!/usr/bin/env python3
"""Render the real file browser at each output size and check its scrolling window."""

from pathlib import Path
import re
import subprocess

from test_disk_browser import extract_function
from test_onee_vtw_runtime import find_native_c_compiler


ROOT = Path(__file__).resolve().parents[1]
FRONTEND = ROOT / "ps_sources/frontend"
BUILD = ROOT / "build/disk_browser_layout"


def main() -> int:
    compiler = find_native_c_compiler()
    if compiler is None:
        raise RuntimeError("A native C compiler is required for disk-browser layout tests")
    source = (FRONTEND / "config_menu.c").read_text(encoding="utf-8")
    header = (FRONTEND / "config_menu.h").read_text(encoding="utf-8")
    declarations = []
    for name in ("CONFIG_MENU_PATH_LEN", "CONFIG_BROWSER_MAX_ENTRIES",
                 "CONFIG_BROWSER_HEADER_H", "CONFIG_BROWSER_BOTTOM_PAD"):
        match = re.search(rf"^#define {name}\s+\S+", source + "\n" + header, re.MULTILINE)
        if match is None:
            raise RuntimeError(f"Missing production define {name}")
        declarations.append(match.group(0))
    for kind, name in (("enum", "config_browser_target_t"),
                       ("enum", "config_browser_entry_type_t"),
                       ("struct", "config_browser_entry_t")):
        declarations.append(re.search(rf"typedef {kind} \{{[^{{}}]*\}} {name};", source).group(0))

    code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config_menu_ui.h"
#include "display_modes.h"
static unsigned drawn, focused, previewed, frames, first_index, last_index;
static int previous_y;
static unsigned expected_selected;
static void record_string(uint16_t *fb, int x, int y, const char *text,
                          uint16_t fg, uint16_t bg, int scale)
{
    const char *entry = strstr(text, "Entry");
    assert(x >= 0 && y >= 0);
    assert(x + (int)strlen(text) * FB16_BUILTIN_FONT_ADVANCE_X * scale <= FB16_WIDTH);
    assert(y + FB16_BUILTIN_FONT_HEIGHT * scale <= FB16_HEIGHT);
    if (entry != NULL) {
        unsigned index;
        assert(sscanf(entry, "Entry%u", &index) == 1);
        if (drawn == 0U) first_index = index;
        else {
            assert(index == last_index + 1U);
            assert(y > previous_y);
        }
        last_index = index;
        previous_y = y;
        ++drawn;
        if (bg == CMUI_COLOR_ROW_ACTIVE) {
            assert(index == expected_selected);
            ++focused;
        }
    }
    fb16_string_scaled(fb, x, y, text, fg, bg, scale);
}
#define fb16_string_scaled record_string
#include "config_menu_ui.c"
#undef fb16_string_scaled
typedef uint64_t FSIZE_t;
typedef int FRESULT;
#define FR_OK 0
#define FR_INVALID_OBJECT 1
#define FR_NO_FILE 2
#define HGR_WHITE CMUI_COLOR_TEXT
#define HGR_ORANGE CMUI_COLOR_WARN
#define HGR_GREEN CMUI_COLOR_SUCCESS
'''
    code += "\n".join(declarations) + r'''
typedef struct {
    uint8_t browser_active, browser_target;
    uint16_t browser_selected, browser_top, browser_count;
    char browser_dir[CONFIG_MENU_PATH_LEN];
} config_menu_t;
static config_browser_entry_t g_browser_entries[CONFIG_BROWSER_MAX_ENTRIES];
static uint8_t config_menu_browser_entry_is_disabled(const config_menu_t *menu,
                                                    const config_browser_entry_t *entry)
{ (void)menu; return entry->type == CONFIG_BROWSER_ENTRY_FILTERED; }
static void config_menu_browser_preview_prepare(config_menu_t *menu) { (void)menu; }
static void config_menu_draw_browser_preview(uint16_t *fb, const config_menu_t *menu,
                                              int x, int y, int w, int h)
{ (void)fb; (void)menu; assert(x >= 0 && y >= 0 && x+w <= FB16_WIDTH && y+h <= FB16_HEIGHT); ++previewed; }
'''
    names = (
        "config_menu_browser_is_disk2_target", "config_menu_browser_is_smartport_target",
        "config_menu_browser_target_smartport_device", "config_menu_browser_title",
        "config_menu_browser_get_entry", "config_menu_browser_visible_rows",
        "config_menu_browser_first_row", "config_menu_browser_keep_selection_visible",
        "config_menu_browser_move", "hgr_draw_lock_icon", "hgr_draw_item_with_lock_ex",
        "config_menu_draw_browser",
    )
    functions = [extract_function(source, name) for name in names]
    code += "\n".join(function[:function.index("{")].rstrip() + ";" for function in functions)
    code += "\n" + "\n".join(functions)
    code += r'''
static void fixture(config_menu_t *menu, unsigned target, unsigned count)
{
    memset(menu, 0, sizeof(*menu));
    menu->browser_active = 1;
    menu->browser_target = target;
    menu->browser_count = count;
    strcpy(menu->browser_dir, "0:/Calibration");
    for (unsigned i = 0; i < count; ++i) {
        config_browser_entry_t *entry = &g_browser_entries[i];
        memset(entry, 0, sizeof(*entry));
        snprintf(entry->name, sizeof(entry->name), "Entry%03u.po", i);
        entry->type = i % 5U == 0U ? CONFIG_BROWSER_ENTRY_TEXT : CONFIG_BROWSER_ENTRY_FILE;
        entry->read_only = i % 3U == 0U;
    }
}
static void draw_check(uint16_t *fb, const config_menu_t *menu, unsigned capacity)
{
    const unsigned compact = FB16_WIDTH < 1680 || FB16_HEIGHT < 1000;
    const unsigned expected = menu->browser_count < capacity ? menu->browser_count : capacity;
    cmui_rect_t body;
    static const char * const tabs[] = {"Files"};
    drawn = focused = previewed = 0;
    previous_y = -1;
    expected_selected = menu->browser_selected;
    cmui_clear(fb);
    assert(config_menu_browser_visible_rows(menu) == capacity);
    if (compact) {
        cmui_compact_begin();
        config_menu_draw_browser(fb, menu, 0, 0, 1480);
        assert(s_compact_count == expected + 3U);
        cmui_compact_finish(fb, "Files", tabs, 1, 0, "Choose a file", 0, 0, 0);
    } else {
        cmui_screen_rects(NULL, &body, NULL);
        config_menu_draw_browser(fb, menu, body.x, body.y - 4, body.w);
    }
    assert(drawn == expected);
    assert(focused == (menu->browser_count != 0U));
    if (drawn) {
        assert(first_index <= menu->browser_selected && last_index >= menu->browser_selected);
        assert(last_index < menu->browser_count);
        if (menu->browser_count <= capacity) assert(first_index == 0U);
        if (menu->browser_selected + 1U == menu->browser_count) assert(last_index + 1U == menu->browser_count);
    }
    assert(previewed == (!compact && (menu->browser_target == CONFIG_BROWSER_TARGET_PROFILE_IMAGE ||
                                     menu->browser_target == CONFIG_BROWSER_TARGET_PRINTOUT)));
    ++frames;
}
static void save_preview(const uint16_t *fb, unsigned width, unsigned height)
{
    char path[80];
    snprintf(path, sizeof(path), "browser_%ux%u.ppm", width, height);
    FILE *file = fopen(path, "wb");
    assert(file);
    fprintf(file, "P6\n%u %u\n255\n", width, height);
    for (unsigned i = 0; i < width * height; ++i) {
        const uint16_t p = fb[i];
        const unsigned char rgb[3] = {(unsigned char)(((p >> 11) & 31U) * 255U / 31U),
            (unsigned char)(((p >> 5) & 63U) * 255U / 63U), (unsigned char)((p & 31U) * 255U / 31U)};
        fwrite(rgb, sizeof(rgb), 1, file);
    }
    fclose(file);
}
int main(void)
{
    /* Explicit viewport expectations catch the former fixed 17-row limit.
     * Include every shipping output and the smaller compact fallback sizes. */
    static const unsigned sizes[][3] = {
        {1024,768,24}, {1200,800,25}, {1280,1024,34}, {1680,1050,16},
        {1920,1080,17}, {1360,768,24}, {640,400,16}, {640,480,21}, {1280,720,36}
    };
    static const uint8_t targets[] = {CONFIG_BROWSER_TARGET_DISK2_D1,
        CONFIG_BROWSER_TARGET_PROFILE_IMAGE, CONFIG_BROWSER_TARGET_PRINTOUT};
    const size_t pixels = FB16_MAX_WIDTH * FB16_MAX_HEIGHT;
    uint16_t *allocation = malloc((pixels + 128U) * sizeof(*allocation));
    assert(allocation);
    uint16_t *fb = allocation + 64U;
    for (unsigned mode = 0; mode < DISPLAY_MODE_COUNT; ++mode) {
        const display_mode_t *output = display_mode_get(mode);
        assert(output->width == sizes[mode][0] && output->height == sizes[mode][1]);
    }
    for (unsigned mode = 0; mode < sizeof(sizes)/sizeof(sizes[0]); ++mode) {
        const unsigned width = sizes[mode][0], height = sizes[mode][1], capacity = sizes[mode][2];
        const size_t used = width * height;
        assert(fb16_set_size(width, height) != 0);
        for (unsigned i = 0; i < 64; ++i) allocation[i] = fb[used+i] = 0xA55A;
        for (unsigned target = 0; target < sizeof(targets)/sizeof(targets[0]); ++target) {
            config_menu_t menu;
            fixture(&menu, targets[target], 96);
            const unsigned selected[] = {0,1,capacity-1,capacity,47,94,95};
            for (unsigned i = 0; i < sizeof(selected)/sizeof(selected[0]); ++i) {
                menu.browser_selected = selected[i];
                menu.browser_top = 0;
                config_menu_browser_keep_selection_visible(&menu);
                draw_check(fb, &menu, capacity);
            }
            config_menu_browser_move(&menu, 1);
            assert(menu.browser_selected == 0 && menu.browser_top == 0);
            draw_check(fb, &menu, capacity);
            config_menu_browser_move(&menu, -1);
            assert(menu.browser_selected == 95 && menu.browser_top == 96-capacity);
            draw_check(fb, &menu, capacity);
            /* Every entry remains reachable in both directions, including
             * when a stale top would otherwise leave the lower area blank. */
            for (unsigned n = 0; n < 192; ++n) {
                config_menu_browser_move(&menu, n < 96 ? 1 : -1);
                assert(menu.browser_top <= menu.browser_selected);
                assert(menu.browser_selected < menu.browser_top + capacity);
                assert(menu.browser_top <= menu.browser_count - capacity);
            }
            menu.browser_top = menu.browser_selected = 95;
            draw_check(fb, &menu, capacity);
            /* Draw directly after a resize; no key press must be required. */
            assert(fb16_set_size(640,400) != 0);
            config_menu_browser_keep_selection_visible(&menu);
            assert(fb16_set_size(width,height) != 0);
            draw_check(fb, &menu, capacity);
            if (target == 0U) save_preview(fb, width, height);
            for (unsigned count = 0; count <= capacity + 1U; ++count) {
                fixture(&menu, targets[target], count);
                menu.browser_selected = count ? count - 1U : 0U;
                menu.browser_top = menu.browser_selected;
                draw_check(fb, &menu, capacity);
            }
        }
        for (unsigned i = 0; i < 64; ++i) assert(allocation[i] == 0xA55A && fb[used+i] == 0xA55A);
        printf("PASS: %ux%u, %u file rows, all targets, short lists, wrap and resize\n", width,height,capacity);
    }
    free(allocation);
    printf("DISK BROWSER LAYOUT PASS: %u rendered frames; text bounds and framebuffer guards\n", frames);
    return 0;
}
'''
    BUILD.mkdir(parents=True, exist_ok=True)
    harness = BUILD / "test.c"
    executable = BUILD / "test.exe"
    harness.write_text(code, encoding="utf-8")
    subprocess.run([str(compiler), "-std=c11", "-O1", "-funsigned-char", "-Wall", "-Wextra",
                    "-Werror", "-static", "-I", str(FRONTEND), str(harness),
                    str(FRONTEND / "config_menu_logo_png.c"),
                    str(ROOT / "ps_sources/lib/fb16.c"), str(ROOT / "ps_sources/lib/lodepng.c"),
                    "-o", str(executable)], cwd=ROOT, check=True)
    subprocess.run([str(executable)], cwd=BUILD, check=True, timeout=60)
    try:
        from PIL import Image
        for preview in BUILD.glob("*.ppm"):
            with Image.open(preview) as image:
                image.save(preview.with_suffix(".png"))
    except ImportError:
        pass
    print(f"Previews: {BUILD}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
