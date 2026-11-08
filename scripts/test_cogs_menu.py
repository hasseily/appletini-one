#!/usr/bin/env python3
"""Exercise the CoGS menu with the real input and drawing functions."""

from pathlib import Path
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

    code = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "config_menu_internal.h"
#include "cogs_service.h"

static cogs_service_status_t status;
static cogs_catalog_page_t page;
static unsigned browse_calls, cancel_calls, download_calls, download_row;
static int browse_refused;
static uint8_t compact;
static unsigned rows, focused, bundle, progress, cogs_entry;

int cogs_service_browse(uint16_t number)
{
    ++browse_calls;
    if (browse_refused) return -1;
    status.page = number;
    status.state = COGS_LOADING;
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
    if (strstr(text, "Game ") != NULL) {
        ++rows;
        assert(y + CMUI_ROW_H <= 800);
        if (focus) {
            assert(strstr(text, "Game 17"));
            ++focused;
        }
        if (!strcmp(text, "[Bundle] Game 17 | Total Replay")) ++bundle;
    }
}
uint8_t cmui_compact_active(void) { return compact; }
uint32_t cmui_compact_row_capacity(void) { return 8U; }
void cmui_panel(uint16_t *fb, const cmui_rect_t *rect, uint32_t color)
{ (void)fb; (void)rect; (void)color; }
void cmui_text(uint16_t *fb, int x, int y, const char *text,
               uint32_t fg, uint32_t bg, int scale)
{ (void)fb; (void)x; (void)y; (void)text; (void)fg; (void)bg; (void)scale; }
void cmui_text_clipped(uint16_t *fb, int x, int y, int w, const char *text,
                       uint32_t fg, uint32_t bg, int scale)
{
    (void)w;
    cmui_text(fb, x, y, text, fg, bg, scale);
    if (!strcmp(text, "Downloading 100 / 400 bytes")) ++progress;
}
'''
    for name in ("config_menu_cogs_active", "config_menu_open_cogs", "config_menu_close_cogs",
                 "config_menu_cogs_handle_input", "config_menu_draw_cogs"):
        code += "\n" + function(menu, name)
    code += "\n" + function(tabs, "config_menu_draw_ethernet")
    code += r'''

int main(void)
{
    config_menu_t menu = {.active = 1U, .item_focus = CONFIG_ETHERNET_ITEM_COGS};
    const cmui_rect_t body = {0, 0, 1200, 800};
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
    assert(config_menu_cogs_active(&menu) && status.page == 1U && browse_calls == 2U);
    config_menu_cogs_handle_input(&menu, UI_KEY_ENTER);
    assert(!download_calls);
    status.state = COGS_READY;
    page.pages = 3U;
    page.count = COGS_PAGE_CAPACITY;
    for (unsigned index = 0; index < page.count; ++index) {
        (void)snprintf(page.entries[index].title, sizeof(page.entries[index].title),
                       "Game %u", index);
    }
    strcpy(page.entries[17].bundle, "Total Replay");
    config_menu_cogs_handle_input(&menu, UI_KEY_LEFT);
    assert(browse_calls == 2U);
    config_menu_cogs_handle_input(&menu, UI_KEY_UP);
    assert(menu.cogs_selected == 17U);
    for (compact = 0U; compact < 2U; ++compact) {
        rows = focused = bundle = 0U;
        config_menu_draw_cogs(NULL, &menu, &body);
        assert(rows == (compact ? 5U : 16U) && focused == 1U && bundle == 1U);
    }
    compact = 0U;
    config_menu_cogs_handle_input(&menu, UI_KEY_RIGHT);
    assert(browse_calls == 3U && status.page == 2U && menu.cogs_selected == 0U);
    config_menu_cogs_handle_input(&menu, UI_KEY_RIGHT);
    assert(browse_calls == 3U);
    status.state = COGS_SAVED;
    config_menu_cogs_handle_input(&menu, UI_KEY_PAGE_DOWN);
    assert(browse_calls == 4U && status.page == 3U);
    status.state = COGS_FAILED;
    config_menu_cogs_handle_input(&menu, UI_KEY_RIGHT);
    assert(browse_calls == 4U);
    config_menu_cogs_handle_input(&menu, UI_KEY_PAGE_UP);
    assert(browse_calls == 5U && status.page == 2U);
    status.state = COGS_READY;
    config_menu_cogs_handle_input(&menu, UI_KEY_UP);
    config_menu_cogs_handle_input(&menu, UI_KEY_ENTER);
    assert(download_calls == 1U && download_row == 17U);
    status.received = 100U;
    status.total = 400U;
    rows = 0U;
    config_menu_draw_cogs(NULL, &menu, &body);
    assert(progress == 1U && rows == 0U);
    config_menu_cogs_handle_input(&menu, UI_KEY_ESC);
    assert(cancel_calls == 1U && !config_menu_cogs_active(&menu) && menu.active);

    config_menu_open_cogs(&menu);
    status.state = COGS_FAILED;
    page.count = 0U;
    config_menu_cogs_handle_input(&menu, UI_KEY_ENTER);
    assert(browse_calls == 7U && status.page == 1U && download_calls == 1U);
    config_menu_cogs_handle_input(&menu, UI_KEY_BACK);
    assert(cancel_calls == 2U && !config_menu_cogs_active(&menu) && menu.active);
    puts("PASS: CoGS Ethernet action, wide/compact rows, paging, bundle, progress and modal cancel");
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
    subprocess.run([str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-DAPPLETINI_HTTP_SERVICE", "-I", str(OUT), "-I", str(FRONT),
                    "-I", str(ROOT / "scripts/fixtures/online_update_host"),
                    str(harness), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
