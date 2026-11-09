#!/usr/bin/env python3
"""Exercise the CoGS menu with the real input and drawing functions."""

from pathlib import Path
import re
import subprocess

from test_onee_input_service import find_native_c_compiler
from test_usb_gamepad_input import extract_function

ROOT = Path(__file__).resolve().parents[1]
FRONT = ROOT / "ps_sources/frontend"
OUT = ROOT / "build/cogs_menu_test"


def function(source: str, name: str) -> str:
    return extract_function(source, name)[1]


def main() -> int:
    menu = (FRONT / "config_menu.c").read_text(encoding="utf-8")
    tabs = (FRONT / "config_menu_device_tabs.c").read_text(encoding="utf-8")
    activate = function(menu, "config_menu_activate_item")
    assert "case CONFIG_ETHERNET_ITEM_COGS:\n            config_menu_open_cogs(menu);" in activate
    assert "CONFIG_ETHERNET_ITEM_COUNT : 1U" in function(menu, "config_menu_tab_item_count")
    handler = function(menu, "config_menu_handle_input")
    assert handler.index("config_menu_cogs_handle_input") < handler.index("config_menu_toggle(menu)")
    assert "config_menu_close_cogs(menu)" in function(menu, "config_menu_set_active")
    assert "config_menu_cogs_active(menu)" in function(menu, "config_menu_handle_usb_fixed_tab")
    draw = function(menu, "config_menu_draw")
    assert "config_menu_draw_cogs(fb, menu, &logical_body);" in draw
    assert "config_menu_draw_cogs(fb, menu, &body);" in draw
    assert "menu->tab, footer_status, footer_warning," in draw
    assert "&footer,\n                footer_status,\n                footer_warning," in draw
    hint_start = draw.index("    if (config_menu_cogs_active(menu) != 0U)")
    hint_end = draw.index("\n#endif", hint_start)
    compact_tabs = re.search(r"static const char \* const k_compact_tab_labels\[.*?\};",
                             menu, re.DOTALL).group()

    code = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "config_menu_internal.h"
#include "cogs_service.h"
#include "display_modes.h"

static cogs_service_status_t status;
static cogs_catalog_page_t page;
static unsigned browse_calls, open_calls, cancel_calls, download_calls, download_row;
static int browse_refused;
static uint8_t cache_ready;
static unsigned rows, focused, progress, cogs_entry;
static unsigned saved_calls, page_calls, hint_calls;
static unsigned expected_selected;
static int previous_bottom, last_game_bottom, compact_footer_divider;
typedef struct { int left, top, right, bottom; } text_bounds_t;
static text_bounds_t status_bounds, hint_bounds;
static const char *expected_hint, *expected_footer_status;
static size_t hint_offset;
static char expected_page[32];
static const char saved_status[] =
    "saved: 0:/_software/CoGS/_Game with a remarkably long title and spaces ok-9999.dsk";
static uint16_t framebuffer[FB16_MAX_WIDTH * FB16_MAX_HEIGHT];

static int overlap(text_bounds_t a, text_bounds_t b)
{
    return a.left < b.right && b.left < a.right &&
           a.top < b.bottom && b.top < a.bottom;
}

static void record_text(uint16_t *fb, int x, int y, const char *text,
                        uint16_t fg, uint16_t bg, int scale)
{
    const text_bounds_t bounds = {x, y,
        x + (int)strlen(text) * FB16_BUILTIN_FONT_ADVANCE_X * scale,
        y + FB16_BUILTIN_FONT_HEIGHT * scale};
    assert(x >= 0 && bounds.right <= FB16_WIDTH);
    assert(y >= 0 && bounds.bottom <= FB16_HEIGHT);
    if (!strcmp(text, "CoGS games") || !strcmp(text, "Catalog ready")) {
        assert(y >= previous_bottom);
        previous_bottom = bounds.bottom;
    } else if (!strncmp(text, "Game ", 5)) {
        assert(rows < page.count && !strcmp(text, page.entries[rows].title));
        assert(y >= previous_bottom);
        previous_bottom = last_game_bottom = bounds.bottom;
        if (bg == CMUI_COLOR_ROW_ACTIVE) {
            assert(rows == expected_selected);
            ++focused;
        }
        ++rows;
    } else if (!strcmp(text, "Downloading 100 / 400 bytes")) {
        ++progress;
    } else if (!strncmp(text, "saved: ", 7)) {
        assert(!strcmp(text, saved_status));
        assert(fg == CMUI_COLOR_SUCCESS && bg == CMUI_COLOR_BG);
        assert(y >= last_game_bottom);
        assert(scale == (FB16_WIDTH <= 1024 ? 1 : CMUI_SMALL_SCALE));
        status_bounds = bounds;
        ++saved_calls;
    } else if (expected_footer_status && !strcmp(text, expected_footer_status)) {
        assert(y >= last_game_bottom);
        status_bounds = bounds;
    } else if (expected_hint && expected_hint[hint_offset] &&
               !strncmp(text, expected_hint + hint_offset, strlen(text))) {
        const char *remainder = expected_hint + hint_offset + strlen(text);
        assert(!*remainder || *remainder == ' ');
        assert(y >= last_game_bottom);
        assert(!overlap(bounds, status_bounds));
        if (hint_calls == 0U) {
            hint_bounds = bounds;
            if (!strncmp(text, "Page ", 5)) {
                assert(!strncmp(text, expected_page, strlen(expected_page)));
                ++page_calls;
            }
        } else {
            assert(y >= hint_bounds.bottom);
            hint_bounds.bottom = bounds.bottom;
        }
        hint_offset = (size_t)(remainder - expected_hint) + (*remainder != '\0');
        ++hint_calls;
    } else {
        assert(strncmp(text, "Page ", 5));
    }
    fb16_string_scaled(fb, x, y, text, fg, bg, scale);
}

static void record_hline(uint16_t *fb, int x, int y, int w, uint16_t color)
{
    if (color == CMUI_COLOR_BORDER_SOFT) compact_footer_divider = y;
    fb16_hline(fb, x, y, w, color);
}

#define fb16_string_scaled record_text
#define fb16_hline record_hline
#include "config_menu_ui.c"
#undef fb16_string_scaled
#undef fb16_hline

int cogs_service_browse(uint16_t number)
{
    ++browse_calls;
    assert(cache_ready && number >= 1U && number <= page.pages);
    status.page = number;
    status.state = COGS_READY;
    return 0;
}
int cogs_service_browse_first(void)
{
    ++open_calls;
    if (browse_refused) return -1;
    status.page = cache_ready ? 1U : 0U;
    status.state = cache_ready ? COGS_READY : COGS_LOADING;
    return 0;
}
int cogs_service_download(uint8_t row)
{
    ++download_calls;
    download_row = row;
    status.state = COGS_DOWNLOADING;
    return 0;
}
void cogs_service_cancel(void) { ++cancel_calls; }
const cogs_service_status_t *cogs_service_status(void) { return &status; }
const cogs_catalog_page_t *cogs_service_page(void) { return &page; }

void config_menu_set_status(config_menu_t *menu, uint8_t warning, const char *text)
{
    menu->status_warning = warning;
    (void)snprintf(menu->status, sizeof(menu->status), "%s", text);
}
void config_menu_format_ipv4(const uint8_t addr[UTHERNET2_IPV4_LEN], char *out, size_t len)
{ (void)addr; (void)len; out[0] = '\0'; }
void config_menu_format_mac(const uint8_t addr[UTHERNET2_MAC_LEN], char *out, size_t len)
{ (void)addr; (void)len; out[0] = '\0'; }
uint8_t config_menu_ethernet_selected_index(const config_menu_t *menu, uint8_t width)
{ (void)menu; (void)width; return 0U; }
const char *config_menu_ethernet_address_mode_text(uint8_t mode)
{ (void)mode; return ""; }
void hgr_draw_check_item(uint16_t *fb, int x, int y, int w, uint8_t focus,
                         uint8_t checked, const char *text)
{ (void)fb; (void)x; (void)y; (void)w; (void)focus; (void)checked; (void)text; }
void hgr_draw_value_item(uint16_t *fb, int x, int y, int w, uint8_t focus,
                         const char *label, const char *value)
{ (void)fb; (void)x; (void)y; (void)w; (void)focus; (void)label; (void)value; }
void hgr_draw_item(uint16_t *fb, int x, int y, int w, uint8_t focus,
                   const char *text, uint32_t color)
{
    (void)fb; (void)x; (void)w; (void)color;
    if (!strcmp(text, "CoGS games")) {
        assert(focus && y + CMUI_ROW_H <= 722);
        ++cogs_entry;
    }
}
'''
    for name in ("config_menu_cogs_active", "config_menu_open_cogs", "config_menu_close_cogs",
                 "config_menu_cogs_handle_input", "config_menu_draw_cogs"):
        code += "\n" + function(menu, name)
    code += "\n" + function(tabs, "config_menu_draw_ethernet")
    code += "\n" + compact_tabs + "\n"
    code += f'''
static const char *cogs_footer(const config_menu_t *menu, uint8_t usb_owned,
                               const char **text, uint8_t *warning, char hint_storage[128])
{{
    const char *input_hint = NULL;
    const char *footer_status = menu->status;
    uint8_t footer_warning = menu->status_warning;
    char cogs_input_hint[128];
{draw[hint_start:hint_end]}
    if (text != NULL) *text = footer_status;
    if (warning != NULL) *warning = footer_warning;
    if (input_hint == cogs_input_hint) {{
        strcpy(hint_storage, cogs_input_hint);
        return hint_storage;
    }}
    return input_hint;
}}
'''
    code += r'''

static const char *cogs_input_hint(const config_menu_t *menu, uint8_t usb_owned)
{
    static char hint_storage[128];
    return cogs_footer(menu, usb_owned, NULL, NULL, hint_storage);
}

static void draw_catalog(const config_menu_t *menu, uint8_t usb_owned)
{
    const char *footer_status;
    uint8_t footer_warning;
    char hint_storage[128];
    const char *hint = cogs_footer(menu, usb_owned, &footer_status, &footer_warning,
                                   hint_storage);
    expected_hint = hint;
    expected_footer_status = footer_status;
    (void)snprintf(expected_page, sizeof(expected_page), "Page %u of %u | ",
                   (unsigned)status.page, (unsigned)page.pages);
    hint_offset = 0U;
    rows = focused = saved_calls = page_calls = hint_calls = 0U;
    previous_bottom = last_game_bottom = 0;
    compact_footer_divider = -1;
    status_bounds = hint_bounds = (text_bounds_t){0};
    expected_selected = menu->cogs_selected;
    if (FB16_WIDTH < 1680 || FB16_HEIGHT < 1000) {
        const cmui_rect_t body = {0, 0, 1480, 812};
        cmui_compact_begin();
        config_menu_draw_cogs(framebuffer, menu, &body);
        cmui_compact_finish(framebuffer, "Ethernet", k_compact_tab_labels, CONFIG_TAB_COUNT,
                            CONFIG_TAB_ETHERNET, footer_status, footer_warning,
                            usb_owned, 0U, hint);
        assert(cmui_compact_scroll_offset() == 0U);
        assert(compact_footer_divider >= 0);
        assert(hint_bounds.top > compact_footer_divider);
    } else {
        cmui_rect_t body, footer;
        cmui_screen_rects(NULL, &body, &footer);
        cmui_clear(framebuffer);
        config_menu_draw_cogs(framebuffer, menu, &body);
        assert(last_game_bottom <= body.y + body.h);
        cmui_footer(framebuffer, &footer, footer_status, footer_warning,
                    usb_owned, 0U, hint);
        assert(hint_bounds.top >= footer.y);
    }
    assert(hint_calls != 0U && expected_hint[hint_offset] == '\0');
    assert(!overlap(status_bounds, hint_bounds));
    expected_hint = expected_footer_status = NULL;
}

static void check_full_page(const config_menu_t *menu, int width, int height)
{
    assert(fb16_set_size(width, height));
    const cogs_service_status_t ready = status;
    for (uint8_t badge = 0; badge < 2U; ++badge) {
        cmui_set_update_badge(badge);
        for (uint8_t usb_owned = 0; usb_owned < 2U; ++usb_owned) {
            draw_catalog(menu, usb_owned);
            assert(rows == COGS_PAGE_CAPACITY && focused == 1U);
            assert(saved_calls == 0U && page_calls == 1U);
            status.state = COGS_SAVED;
            strcpy(status.detail, saved_status);
            draw_catalog(menu, usb_owned);
            assert(rows == COGS_PAGE_CAPACITY && focused == 1U);
            assert(saved_calls == 1U && page_calls == 1U);
            status = ready;
        }
    }
}

int main(void)
{
    config_menu_t menu = {.active = 1U, .item_focus = CONFIG_ETHERNET_ITEM_COGS};
    assert(strlen(saved_status) == 82U);
    assert(COGS_PAGE_CAPACITY == 20U);
    config_menu_draw_ethernet(NULL, &menu, 380, 154, 1252);
    assert(!cogs_entry);
    menu.ethernet_slot1_enabled = 1U;
    config_menu_draw_ethernet(NULL, &menu, 380, 154, 1252);
    assert(cogs_entry == 1U);

    browse_refused = 1;
    config_menu_open_cogs(&menu);
    assert(!config_menu_cogs_active(&menu) && menu.status_warning);
    browse_refused = 0;
    config_menu_open_cogs(&menu);
    assert(config_menu_cogs_active(&menu) && status.state == COGS_LOADING);
    assert(open_calls == 2U && browse_calls == 0U);
    assert(!strcmp(cogs_input_hint(&menu, 0U), "Esc: Cancel"));
    draw_catalog(&menu, 0U);
    assert(rows == 0U && page_calls == 0U);
    config_menu_cogs_handle_input(&menu, UI_KEY_ENTER);
    assert(!download_calls && open_calls == 2U);
    status.state = COGS_FAILED;
    status.page = 2U;
    assert(!strcmp(cogs_input_hint(&menu, 0U), "Enter: Retry | Esc: Back"));
    assert(!strcmp(cogs_input_hint(&menu, 1U), "A: Retry | B: Back"));
    config_menu_cogs_handle_input(&menu, UI_KEY_ENTER);
    assert(open_calls == 3U && browse_calls == 0U && download_calls == 0U);
    assert(status.state == COGS_LOADING);
    cache_ready = 1U;
    status.state = COGS_READY;
    status.page = 1U;
    strcpy(status.detail, "Catalog ready");
    page.pages = 3U;
    page.count = COGS_PAGE_CAPACITY;
    for (unsigned index = 0; index < page.count; ++index) {
        (void)snprintf(page.entries[index].title, sizeof(page.entries[index].title),
                       "Game %02u with a title that takes up a whole row", index);
    }
    config_menu_cogs_handle_input(&menu, UI_KEY_LEFT);
    assert(browse_calls == 0U);
    config_menu_cogs_handle_input(&menu, UI_KEY_UP);
    assert(menu.cogs_selected == 19U);
    config_menu_set_status(&menu, 1U, "Earlier warning");
    page.pages = 13U;
    status.page = 12U;
    check_full_page(&menu, 640, 400);
    check_full_page(&menu, 1680, 1000);
    for (uint8_t mode = 0; mode < DISPLAY_MODE_COUNT; ++mode) {
        const display_mode_t *output = display_mode_get(mode);
        check_full_page(&menu, output->width, output->height);
    }
    page.pages = 3U;
    status.page = 1U;
    config_menu_cogs_handle_input(&menu, UI_KEY_RIGHT);
    assert(browse_calls == 1U && status.page == 2U && menu.cogs_selected == 0U);
    assert(status.state == COGS_READY);
    draw_catalog(&menu, 0U);
    assert(page_calls == 1U && rows == COGS_PAGE_CAPACITY);
    config_menu_cogs_handle_input(&menu, UI_KEY_RIGHT);
    assert(browse_calls == 2U && status.page == 3U && status.state == COGS_READY);
    draw_catalog(&menu, 0U);
    assert(page_calls == 1U && rows == COGS_PAGE_CAPACITY);
    status.state = COGS_SAVED;
    config_menu_cogs_handle_input(&menu, UI_KEY_PAGE_UP);
    assert(browse_calls == 3U && status.page == 2U);
    draw_catalog(&menu, 0U);
    assert(page_calls == 1U && rows == COGS_PAGE_CAPACITY);
    status.state = COGS_FAILED;
    config_menu_cogs_handle_input(&menu, UI_KEY_PAGE_DOWN);
    assert(browse_calls == 4U && status.page == 3U);
    config_menu_cogs_handle_input(&menu, UI_KEY_RIGHT);
    assert(browse_calls == 4U);
    config_menu_cogs_handle_input(&menu, UI_KEY_LEFT);
    assert(browse_calls == 5U && status.page == 2U);
    config_menu_cogs_handle_input(&menu, UI_KEY_UP);
    config_menu_cogs_handle_input(&menu, UI_KEY_ENTER);
    assert(download_calls == 1U && download_row == 19U);
    status.received = 100U;
    status.total = 400U;
    draw_catalog(&menu, 0U);
    assert(progress == 1U && rows == 0U && page_calls == 1U);
    config_menu_cogs_handle_input(&menu, UI_KEY_ESC);
    assert(cancel_calls == 1U && !config_menu_cogs_active(&menu) && menu.active);

    config_menu_open_cogs(&menu);
    assert(open_calls == 4U && browse_calls == 5U);
    assert(status.state == COGS_READY && status.page == 1U && menu.cogs_selected == 0U);
    page.pages = 1U;
    assert(!strncmp(cogs_input_hint(&menu, 0U), "Page 1 of 1 | ", 14U));
    assert(strstr(cogs_input_hint(&menu, 0U), "Enter: Download"));
    assert(strstr(cogs_input_hint(&menu, 1U), "A: Download"));
    assert(!strstr(cogs_input_hint(&menu, 0U), "Left/Right"));
    assert(!strstr(cogs_input_hint(&menu, 1U), "Left/Right"));
    config_menu_cogs_handle_input(&menu, UI_KEY_RIGHT);
    assert(browse_calls == 5U);
    page.count = 0U;
    page.pages = status.page = 0U;
    assert(!strcmp(cogs_input_hint(&menu, 0U), "Esc: Back"));
    assert(!strcmp(cogs_input_hint(&menu, 1U), "B: Back"));
    config_menu_cogs_handle_input(&menu, UI_KEY_ENTER);
    assert(open_calls == 4U && browse_calls == 5U && download_calls == 1U);
    config_menu_cogs_handle_input(&menu, UI_KEY_BACK);
    assert(cancel_calls == 2U && !config_menu_cogs_active(&menu) && menu.active);
    puts("PASS: CoGS cached keyboard paging, footer label and saved path, all 20 rows, retry, progress and cancel");
    return 0;
}
'''
    compiler = find_native_c_compiler()
    if compiler is None:
        raise RuntimeError("A native C compiler is required for the CoGS menu test")
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "xiicps.h").write_text("#pragma once\ntypedef struct {int unused;} XIicPs;\n",
                                  encoding="utf-8")
    harness = OUT / "cogs_menu.c"
    harness.write_text(code, encoding="utf-8")
    executable = OUT / "cogs_menu.exe"
    subprocess.run([str(compiler), "-std=c11", "-O1", "-static", "-funsigned-char",
                    "-Wall", "-Wextra", "-Werror",
                    "-DAPPLETINI_HTTP_SERVICE", "-I", str(OUT), "-I", str(FRONT),
                    "-I", str(ROOT / "scripts/fixtures/online_update_host"),
                    str(harness), str(FRONT / "config_menu_logo_png.c"),
                    str(ROOT / "ps_sources/lib/fb16.c"), str(ROOT / "ps_sources/lib/lodepng.c"),
                    "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
