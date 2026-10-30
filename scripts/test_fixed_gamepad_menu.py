#!/usr/bin/env python3
"""Run fixed controller and ONE//e keyboard consumers with real binding translators."""

from pathlib import Path
import hashlib
import json
import re
import subprocess

from test_joystick_config_menu import function
from test_onee_input_service import find_native_c_compiler


ROOT = Path(__file__).resolve().parents[1]
FRONT = ROOT / "ps_sources/frontend"
OUT = ROOT / "build/fixed_gamepad_menu"


def array(source: str, name: str) -> str:
    match = re.search(r"static const [^;]*\b" + name + r"\[[^;]*?;", source, re.S)
    if not match:
        raise AssertionError(f"missing array {name}")
    return match.group(0) + "\n"


def main() -> int:
    compiler = find_native_c_compiler()
    if compiler is None:
        raise RuntimeError("A native C compiler is required")
    menu = (FRONT / "config_menu.c").read_text()
    header = (FRONT / "config_menu.h").read_text()
    uart = (FRONT / "uart_control.h").read_text()
    internal = (FRONT / "config_menu_internal.h").read_text()
    frontend = (FRONT / "main.c").read_text()
    hid = (FRONT / "usb_hid_service.c").read_text()
    code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "usb_hid_service.h"
#include "online_update_core.h"
#define HID_KBD_USAGE_ERRUNDEF 3U
#define HID_KBD_USAGE_MAX 0xE7U
'''
    code += uart[uart.index("typedef enum {"):uart.index("typedef struct {\n    uint32_t frame_count")]
    code += header[header.index("#define CONFIG_MENU_STATUS_LEN"):header.index("typedef struct {")]
    code += internal[internal.index("typedef enum {"):internal.index("} config_tab_t;") + len("} config_tab_t;")]
    code += r'''
typedef struct {
    usb_hid_menu_source_t usb_menu_bindings[CONFIG_MENU_USB_BIND_ACTION_COUNT];
    uint8_t active, browser_active, browser_target, golden_boot_update_pending;
    uint8_t usb0_sd_remote_active, ethernet_ftp_sd_remote_active, usb_binding_capture, onee_fixed_mode;
    uint8_t joystick_page_active, profile_carousel_active, profile_name_editor_active;
    uint8_t printout_action_active, printout_editor_active, printout_delete_confirm_active;
    uint8_t golden_boot_update_confirm, golden_boot_confirm_update;
    uint8_t online_update_confirm, online_update_confirm_update;
    uint8_t online_update_download_pending, online_update_install_pending;
    uint8_t about_credits_active;
    online_update_snapshot_t online_update;
    uint8_t usb_owned, onee_persist_retry_polls;
    unsigned tab, item_focus;
} config_menu_t;
typedef struct { uint32_t input_seq, key_count; ui_key_t last_key; } ui_state_t;
enum { SCREENSHOT_SERVICE_KIND_A2, SCREENSHOT_SERVICE_KIND_1080P };
static uint8_t g_usb_menu_owned;
static uint8_t g_onee_standby_available;
static ui_key_t keys[16];
static unsigned key_count, captures, screenshots, speed_actions, notices;
static unsigned opens, closes, rom_closes;
static unsigned reader_closes, preview_clears, clamp_calls, clock_refreshes, golden_refreshes;
static uint32_t g_about_credits_top,g_about_credits_rows,g_about_credits_visible;
static unsigned compact_scroll_resets;
static void cmui_compact_scroll(int delta) { assert(delta==-1000000);++compact_scroll_resets; }
static uint8_t capture_on_enter, share_stop_pending;
static usb_hid_menu_source_t captured_source;
void config_menu_set_active(config_menu_t *menu, uint8_t active);
static uint8_t config_menu_is_active(const config_menu_t *menu) { return menu->active; }
static int http_download_active(void) { return 0; }
static uint8_t config_menu_usb0_sd_remote_active(const config_menu_t *menu) { return menu->usb0_sd_remote_active; }
static uint8_t config_menu_ethernet_ftp_sd_remote_active(const config_menu_t *menu) { return menu->ethernet_ftp_sd_remote_active; }
static uint8_t config_menu_usb_binding_capture_action(const config_menu_t *menu) { return menu->usb_binding_capture; }
static uint8_t config_menu_capture_usb_binding(config_menu_t *menu, usb_hid_menu_source_t source)
{
    if (source == USB_HID_MENU_SOURCE_NONE) return 0U;
    ++captures; captured_source=source; menu->usb_binding_capture=CONFIG_MENU_USB_BIND_CAPTURE_NONE;
    return 1U;
}
static ui_input_t ui_make_input(ui_key_t key) { return (ui_input_t){key,1U,0U}; }
static void ui_handle_input_with_config(ui_state_t *state, config_menu_t *menu, ui_input_t input)
{
    assert(key_count < sizeof(keys)/sizeof(keys[0])); keys[key_count++]=input.key;
    ++state->input_seq; ++state->key_count; state->last_key=input.key;
    if (input.key==UI_KEY_BACK || input.key==UI_KEY_ESC) {
        menu->browser_active=0U;
        if (!share_stop_pending) menu->usb0_sd_remote_active=menu->ethernet_ftp_sd_remote_active=0U;
    }
    if (input.key==UI_KEY_ENTER && capture_on_enter)
        menu->usb_binding_capture=CONFIG_MENU_USB_BIND_ACTION_OK;
}
static void ui_close_config_menu_child(ui_state_t *state, config_menu_t *menu)
{ ui_handle_input_with_config(state,menu,ui_make_input(UI_KEY_ESC)); }
/* CLOSE_CONSUMER */
static void ui_set_boot_menu_visible(ui_state_t *state, config_menu_t *menu, uint8_t active)
{
    (void)state; config_menu_set_active(menu,active);
    if (active) ++opens;
    else ++closes;
}
static void ui_request_boot_menu_close(config_menu_t *menu) { (void)menu; ++rom_closes; }
static void ui_save_screenshot(unsigned kind) { (void)kind; ++screenshots; }
static void screenshot_service_show_notice(const char *notice) { (void)notice; ++notices; }
static void vtw_service_speed_toggle(void) { ++speed_actions; }
static void vtw_service_speed_step(int delta) { (void)delta; ++speed_actions; }
static void vtw_service_slug_toggle(void) { ++speed_actions; }
static const char *vtw_service_last_action_text(void) { return "speed"; }
#define CONFIG_BROWSER_TARGET_NONE 0U
static void config_menu_text_reader_close(void) { ++reader_closes; }
static void config_menu_browser_preview_clear(void) { ++preview_clears; }
static void config_menu_clamp_item(config_menu_t *menu) { assert(menu->item_focus==0U); ++clamp_calls; }
static void config_menu_refresh_golden_boot(config_menu_t *menu) { (void)menu; ++golden_refreshes; }
static void config_menu_try_read_rtc(config_menu_t *menu, uint8_t status) { (void)menu; (void)status; ++clock_refreshes; }
static void config_menu_retry_settings_if_needed(config_menu_t *menu) { (void)menu; }
static void config_menu_refresh_smartport_media_after_menu_sd(config_menu_t *menu) { (void)menu; }
static void config_menu_stop_usb0_sd_remote(config_menu_t *menu) { menu->usb0_sd_remote_active=0U; }
'''
    code = code.replace("/* CLOSE_CONSUMER */", function(frontend, "ui_config_menu_has_close_consumer"))
    for name in ("k_usb_binding_keys", "k_usb_binding_source_order", "k_usb_binding_button_source_order"):
        code += array(menu, name)
    for name in ("usb_hid_menu_source_from_keyboard_usage", "usb_hid_menu_source_is_keyboard",
                 "usb_hid_menu_source_is_gamepad"):
        code += function(hid, name)
    for name in ("config_menu_usb_binding_source_valid", "config_menu_translate_usb_fixed_action",
                 "config_menu_translate_usb_binding_explicit", "config_menu_translate_usb_binding"):
        code += function(menu, name)
    for name in ("config_menu_about_reset_scroll", "config_menu_online_update_busy", "config_menu_browser_close", "config_menu_on_tab_entered",
                 "config_menu_prev_tab", "config_menu_next_tab", "config_menu_handle_usb_fixed_tab",
                 "config_menu_focus_onee_setup", "config_menu_set_active"):
        code += function(menu, name)
    for name in ("ui_key_from_usb_menu_source", "ui_toggle_usb_gamepad_menu", "ui_handle_usb_menu_event"):
        code += function(frontend, name)
    code += r'''
static void reset(config_menu_t *menu, ui_state_t *state)
{
    memset(menu,0,sizeof(*menu)); memset(state,0,sizeof(*state)); memset(keys,0,sizeof(keys));
    menu->active=1U; menu->usb_binding_capture=CONFIG_MENU_USB_BIND_CAPTURE_NONE; g_usb_menu_owned=1U;
    key_count=captures=screenshots=speed_actions=notices=opens=closes=rom_closes=0U;
    reader_closes=preview_clears=clamp_calls=clock_refreshes=golden_refreshes=0U;
    capture_on_enter=share_stop_pending=0U; captured_source=USB_HID_MENU_SOURCE_NONE;
    g_onee_standby_available=0U;
    g_about_credits_top=g_about_credits_rows=g_about_credits_visible=compact_scroll_resets=0;
}
static void fixed(config_menu_t *menu, ui_state_t *state,
                  usb_hid_menu_action_t action, unsigned button, usb_hid_menu_action_t global)
{
    const usb_hid_menu_event_t event={.action=global,
        .source=button<32U ? USB_HID_MENU_SOURCE_GAMEPAD_BASE+button : USB_HID_MENU_SOURCE_NONE,
        .fixed_action=action};
    ui_handle_usb_menu_event(state,menu,&event);
}
static void test_fixed_navigation_and_explicit_bindings(void)
{
    config_menu_t menu; ui_state_t state;
    const usb_hid_menu_action_t actions[]={USB_HID_MENU_ACTION_ITEM_UP,USB_HID_MENU_ACTION_ITEM_DOWN,
        USB_HID_MENU_ACTION_LEFT,USB_HID_MENU_ACTION_RIGHT,USB_HID_MENU_ACTION_SELECT,
        USB_HID_MENU_ACTION_BACK,USB_HID_MENU_ACTION_PREV_TAB,USB_HID_MENU_ACTION_NEXT_TAB};
    const ui_key_t expected[]={UI_KEY_UP,UI_KEY_DOWN,UI_KEY_LEFT,UI_KEY_RIGHT,UI_KEY_ENTER,
        UI_KEY_BACK,UI_KEY_SHIFT_TAB,UI_KEY_TAB};
    for (unsigned mode=0;mode<2;++mode) for(unsigned n=0;n<8;++n) {
        reset(&menu,&state); menu.onee_fixed_mode=(uint8_t)mode;
        /* Move every old menu source to a different saved action. */
        for (unsigned a=0;a<CONFIG_MENU_USB_BIND_ACTION_COUNT;++a)
            menu.usb_menu_bindings[a]=USB_HID_MENU_ACTION_LEFT;
        fixed(&menu,&state,actions[n],32U,USB_HID_MENU_ACTION_NONE);
        if (n<6) assert(key_count==1 && keys[0]==expected[n]);
        else assert(!key_count && menu.tab==(n==6 ? CONFIG_TAB_COUNT-1U : 1U) &&
                    state.key_count==1 && state.last_key==expected[n]);
    }
    reset(&menu,&state);
    menu.usb_menu_bindings[CONFIG_MENU_USB_BIND_ACTION_LEFT]=USB_HID_MENU_ACTION_LEFT;
    fixed(&menu,&state,USB_HID_MENU_ACTION_SELECT,0U,USB_HID_MENU_ACTION_NONE);
    assert(key_count==1 && keys[0]==UI_KEY_ENTER); /* A has no old left-button alias. */
    key_count=0;
    menu.usb_menu_bindings[CONFIG_MENU_USB_BIND_ACTION_OK]=USB_HID_MENU_SOURCE_GAMEPAD_BASE;
    fixed(&menu,&state,USB_HID_MENU_ACTION_SELECT,0U,USB_HID_MENU_ACTION_NONE);
    assert(key_count==1 && keys[0]==UI_KEY_ENTER); /* Fixed and saved OK deduplicate. */
    key_count=0; menu.usb_menu_bindings[CONFIG_MENU_USB_BIND_ACTION_OK]=USB_HID_MENU_SOURCE_NONE;
    menu.usb_menu_bindings[CONFIG_MENU_USB_BIND_ACTION_RIGHT]=USB_HID_MENU_SOURCE_GAMEPAD_BASE;
    fixed(&menu,&state,USB_HID_MENU_ACTION_SELECT,0U,USB_HID_MENU_ACTION_NONE);
    assert(key_count==2 && keys[0]==UI_KEY_ENTER && keys[1]==UI_KEY_RIGHT);
    reset(&menu,&state);
    menu.usb_menu_bindings[CONFIG_MENU_USB_BIND_ACTION_TAB_UP]=USB_HID_MENU_SOURCE_GAMEPAD_BASE+4;
    fixed(&menu,&state,USB_HID_MENU_ACTION_PREV_TAB,4U,USB_HID_MENU_ACTION_NONE);
    assert(!key_count && menu.tab==CONFIG_TAB_COUNT-1U && state.key_count==1);
    reset(&menu,&state);
    menu.usb_menu_bindings[CONFIG_MENU_USB_BIND_ACTION_UP]=USB_HID_MENU_SOURCE_GAMEPAD_BASE+5;
    fixed(&menu,&state,USB_HID_MENU_ACTION_NEXT_TAB,5U,USB_HID_MENU_ACTION_NONE);
    assert(key_count==1 && keys[0]==UI_KEY_UP && menu.tab==1U && state.key_count==2);
    reset(&menu,&state);
    fixed(&menu,&state,USB_HID_MENU_ACTION_SELECT,0U,USB_HID_MENU_ACTION_SCREENSHOT_A2);
    assert(key_count==1 && keys[0]==UI_KEY_ENTER && screenshots==1);
    reset(&menu,&state);
    fixed(&menu,&state,USB_HID_MENU_ACTION_PREV_TAB,4U,USB_HID_MENU_ACTION_VTW_SPEED_UP);
    assert(!key_count && menu.tab==CONFIG_TAB_COUNT-1U && speed_actions==1);
    puts("PASS fixed navigation bypasses saved/ONEe bindings; explicit actions coexist and equal actions deduplicate");
}
static void test_learning_and_opening_capture(void)
{
    config_menu_t menu; ui_state_t state;
    const unsigned buttons[]={0U,1U,4U,5U,10U};
    const usb_hid_menu_action_t actions[]={USB_HID_MENU_ACTION_SELECT,USB_HID_MENU_ACTION_BACK,
        USB_HID_MENU_ACTION_PREV_TAB,USB_HID_MENU_ACTION_NEXT_TAB,USB_HID_MENU_ACTION_TOGGLE};
    for (unsigned n=0;n<5;++n) {
        reset(&menu,&state); menu.usb_binding_capture=CONFIG_MENU_USB_BIND_ACTION_OK;
        fixed(&menu,&state,actions[n],buttons[n],USB_HID_MENU_ACTION_SCREENSHOT_A2);
        assert(captures==1 && captured_source==USB_HID_MENU_SOURCE_GAMEPAD_BASE+buttons[n]);
        assert(!key_count && !screenshots && !opens && !closes && menu.active);
    }
    reset(&menu,&state); menu.usb_binding_capture=CONFIG_MENU_USB_BIND_ACTION_OK;
    fixed(&menu,&state,USB_HID_MENU_ACTION_ITEM_DOWN,32U,USB_HID_MENU_ACTION_NONE);
    assert(!captures && !key_count && menu.usb_binding_capture==CONFIG_MENU_USB_BIND_ACTION_OK);
    reset(&menu,&state); capture_on_enter=1U;
    fixed(&menu,&state,USB_HID_MENU_ACTION_SELECT,0U,USB_HID_MENU_ACTION_NONE);
    assert(key_count==1 && !captures && menu.usb_binding_capture==CONFIG_MENU_USB_BIND_ACTION_OK);
    puts("PASS physical-source learning consumes fixed/global side effects; opening capture does not learn its own A press");
}
static void test_toggle_children_and_safety(void)
{
    config_menu_t menu; ui_state_t state;
    reset(&menu,&state); menu.active=0U; g_usb_menu_owned=0U;
    menu.usb_menu_bindings[CONFIG_MENU_USB_BIND_ACTION_UP]=USB_HID_MENU_SOURCE_GAMEPAD_BASE+10;
    fixed(&menu,&state,USB_HID_MENU_ACTION_TOGGLE,10U,USB_HID_MENU_ACTION_SCREENSHOT_A2);
    assert(menu.active && g_usb_menu_owned && opens==1 && screenshots==1 && !key_count);
    fixed(&menu,&state,USB_HID_MENU_ACTION_TOGGLE,10U,USB_HID_MENU_ACTION_NONE);
    assert(!menu.active && !g_usb_menu_owned && closes==1 && !key_count);
    reset(&menu,&state); menu.browser_active=1U;
    fixed(&menu,&state,USB_HID_MENU_ACTION_BACK,1U,USB_HID_MENU_ACTION_NONE);
    assert(menu.active && !menu.browser_active && !closes && keys[0]==UI_KEY_BACK);
    menu.browser_active=1U; menu.joystick_page_active=1U; menu.profile_carousel_active=1U;
    menu.profile_name_editor_active=1U; menu.printout_action_active=1U;
    menu.printout_editor_active=1U; menu.printout_delete_confirm_active=1U; key_count=0;
    fixed(&menu,&state,USB_HID_MENU_ACTION_TOGGLE,10U,USB_HID_MENU_ACTION_NONE);
    assert(!menu.active && !menu.browser_active && closes==1 && !key_count);
    assert(!menu.joystick_page_active && !menu.profile_carousel_active && !menu.profile_name_editor_active);
    assert(!menu.printout_action_active && !menu.printout_editor_active && !menu.printout_delete_confirm_active);
    reset(&menu,&state); menu.browser_active=1U; g_usb_menu_owned=0U;
    fixed(&menu,&state,USB_HID_MENU_ACTION_TOGGLE,10U,USB_HID_MENU_ACTION_NONE);
    assert(rom_closes==1 && !closes && !key_count); /* Keep the Apple ROM close handshake. */
    reset(&menu,&state); menu.golden_boot_update_pending=1U;
    fixed(&menu,&state,USB_HID_MENU_ACTION_TOGGLE,10U,USB_HID_MENU_ACTION_NONE);
    assert(menu.active && !closes && !rom_closes && !key_count);
    for (unsigned transport=0;transport<2;++transport) {
        reset(&menu,&state); menu.usb0_sd_remote_active=(uint8_t)!transport; menu.ethernet_ftp_sd_remote_active=(uint8_t)transport;
        fixed(&menu,&state,USB_HID_MENU_ACTION_TOGGLE,10U,USB_HID_MENU_ACTION_NONE);
        assert(!menu.active && closes==1 && key_count==1 && keys[0]==UI_KEY_ESC);
        reset(&menu,&state); menu.usb0_sd_remote_active=(uint8_t)!transport; menu.ethernet_ftp_sd_remote_active=(uint8_t)transport;
        share_stop_pending=1U;
        fixed(&menu,&state,USB_HID_MENU_ACTION_TOGGLE,10U,USB_HID_MENU_ACTION_NONE);
        assert(menu.active && !closes && !rom_closes && key_count==1);
    }
    puts("PASS current-state Home toggles whole menus, B backs children, ROM/SD/update close safeguards stay intact");
}
static void test_fixed_tabs_leave_real_child_state(void)
{
    config_menu_t menu; ui_state_t state;
    reset(&menu,&state); menu.tab=CONFIG_TAB_USB; menu.item_focus=9U; menu.joystick_page_active=1U;
    fixed(&menu,&state,USB_HID_MENU_ACTION_PREV_TAB,4U,USB_HID_MENU_ACTION_NONE);
    assert(menu.tab==CONFIG_TAB_RAM && !menu.item_focus && !menu.joystick_page_active && !key_count);
    assert(reader_closes==1 && preview_clears==1 && clamp_calls==1);
    reset(&menu,&state); menu.tab=CONFIG_TAB_USB; menu.item_focus=7U;
    menu.browser_active=1U; menu.browser_target=3U; menu.profile_carousel_active=1U;
    menu.profile_name_editor_active=1U; menu.printout_action_active=1U;
    menu.printout_editor_active=1U; menu.printout_delete_confirm_active=1U;
    menu.golden_boot_update_confirm=1U; menu.golden_boot_confirm_update=1U;
    fixed(&menu,&state,USB_HID_MENU_ACTION_NEXT_TAB,5U,USB_HID_MENU_ACTION_NONE);
    assert(menu.tab==CONFIG_TAB_PRINTING && !menu.item_focus && !menu.browser_active && !menu.browser_target);
    assert(!menu.profile_carousel_active && !menu.profile_name_editor_active && !menu.printout_action_active);
    assert(!menu.printout_editor_active && !menu.printout_delete_confirm_active && !menu.golden_boot_update_confirm);
    assert(!menu.golden_boot_confirm_update && reader_closes==1 && preview_clears==1 && clamp_calls==1);
    reset(&menu,&state); menu.tab=CONFIG_TAB_PROFILES;
    fixed(&menu,&state,USB_HID_MENU_ACTION_PREV_TAB,4U,USB_HID_MENU_ACTION_NONE);
    assert(menu.tab==CONFIG_TAB_ABOUT && golden_refreshes==1);
    fixed(&menu,&state,USB_HID_MENU_ACTION_NEXT_TAB,5U,USB_HID_MENU_ACTION_NONE);
    assert(menu.tab==CONFIG_TAB_PROFILES);
    menu.tab=CONFIG_TAB_TRANSWARP;
    fixed(&menu,&state,USB_HID_MENU_ACTION_NEXT_TAB,5U,USB_HID_MENU_ACTION_NONE);
    assert(menu.tab==CONFIG_TAB_CLOCK && clock_refreshes==1);
    for (unsigned modal=0;modal<3;++modal) {
        reset(&menu,&state); menu.tab=CONFIG_TAB_USB; menu.browser_active=1U; menu.item_focus=9U;
        if (!modal) menu.golden_boot_update_pending=1U;
        else if (modal==1) menu.usb0_sd_remote_active=1U;
        else menu.ethernet_ftp_sd_remote_active=1U;
        fixed(&menu,&state,USB_HID_MENU_ACTION_NEXT_TAB,5U,USB_HID_MENU_ACTION_NONE);
        assert(menu.tab==CONFIG_TAB_USB && menu.browser_active && menu.item_focus==9U && !clamp_calls);
    }
    assert(!config_menu_handle_usb_fixed_tab(NULL,1));
    reset(&menu,&state); menu.active=0U;
    assert(!config_menu_handle_usb_fixed_tab(&menu,1));
    reset(&menu,&state);menu.tab=CONFIG_TAB_ABOUT;menu.about_credits_active=1;
    g_about_credits_top=7;g_about_credits_rows=90;g_about_credits_visible=8;
    fixed(&menu,&state,USB_HID_MENU_ACTION_NEXT_TAB,5U,USB_HID_MENU_ACTION_NONE);
    assert(menu.tab==CONFIG_TAB_PROFILES && !menu.about_credits_active && !key_count);
    assert(!g_about_credits_top && !g_about_credits_rows && !g_about_credits_visible && compact_scroll_resets==1);
    fixed(&menu,&state,USB_HID_MENU_ACTION_PREV_TAB,4U,USB_HID_MENU_ACTION_NONE);
    assert(menu.tab==CONFIG_TAB_ABOUT && !menu.about_credits_active && compact_scroll_resets==2 && golden_refreshes==1);
    reset(&menu,&state);menu.tab=CONFIG_TAB_ABOUT;menu.about_credits_active=1;
    fixed(&menu,&state,USB_HID_MENU_ACTION_BACK,1U,USB_HID_MENU_ACTION_NONE);
    assert(menu.active && !closes && key_count==1 && keys[0]==UI_KEY_BACK);
    fixed(&menu,&state,USB_HID_MENU_ACTION_TOGGLE,10U,USB_HID_MENU_ACTION_NONE);
    assert(!menu.active && !menu.about_credits_active && closes==1);
    puts("PASS fixed shoulders change actual tabs from joystick/browser children, preserve entry refreshes and modal safeguards");
}
static void test_fixed_keyboard_navigation(void)
{
    config_menu_t menu; ui_state_t state;
    static const struct {
        uint8_t usage;
        usb_hid_menu_action_t action;
        ui_key_t key;
    } controls[] = {
        {0x52U,USB_HID_MENU_ACTION_ITEM_UP,UI_KEY_UP},
        {0x51U,USB_HID_MENU_ACTION_ITEM_DOWN,UI_KEY_DOWN},
        {0x50U,USB_HID_MENU_ACTION_LEFT,UI_KEY_LEFT},
        {0x4FU,USB_HID_MENU_ACTION_RIGHT,UI_KEY_RIGHT},
        {0x28U,USB_HID_MENU_ACTION_SELECT,UI_KEY_ENTER},
        {0x58U,USB_HID_MENU_ACTION_SELECT,UI_KEY_ENTER},
        {0x29U,USB_HID_MENU_ACTION_BACK,UI_KEY_BACK},
        {0x2BU,USB_HID_MENU_ACTION_NEXT_TAB,UI_KEY_TAB},
        {0x2BU,USB_HID_MENU_ACTION_PREV_TAB,UI_KEY_SHIFT_TAB},
    };
    for (unsigned i=0U;i<sizeof(controls)/sizeof(controls[0]);++i) {
        const usb_hid_menu_source_t source=USB_HID_MENU_SOURCE_KEY_BASE|controls[i].usage;
        const usb_hid_menu_event_t event={.action=controls[i].action,.source=source,
            .fixed_action=USB_HID_MENU_ACTION_NONE};
        reset(&menu,&state);
        menu.usb_menu_bindings[CONFIG_MENU_USB_BIND_ACTION_RIGHT]=source;
        ui_handle_usb_menu_event(&state,&menu,&event);
        assert(state.key_count==1U && state.last_key==controls[i].key);
        if (controls[i].key==UI_KEY_TAB || controls[i].key==UI_KEY_SHIFT_TAB)
            assert(!key_count && menu.tab==(controls[i].key==UI_KEY_TAB ? 1U : CONFIG_TAB_COUNT-1U));
        else assert(key_count==1U && keys[0]==controls[i].key);
        reset(&menu,&state); menu.active=0U;
        ui_handle_usb_menu_event(&state,&menu,&event);
        assert(!state.key_count && !key_count && !opens);
        reset(&menu,&state); menu.usb_binding_capture=CONFIG_MENU_USB_BIND_ACTION_OK;
        ui_handle_usb_menu_event(&state,&menu,&event);
        assert(captures==1U && captured_source==source && !state.key_count && !key_count);
    }
    reset(&menu,&state); menu.tab=CONFIG_TAB_USB; menu.browser_active=1U; menu.item_focus=9U;
    const usb_hid_menu_event_t tab={.action=USB_HID_MENU_ACTION_NEXT_TAB,
        .source=USB_HID_MENU_SOURCE_KEY_BASE|0x2BU,.fixed_action=USB_HID_MENU_ACTION_NONE};
    ui_handle_usb_menu_event(&state,&menu,&tab);
    assert(menu.tab==CONFIG_TAB_PRINTING && !menu.browser_active && menu.item_focus==0U);
    puts("PASS fixed keyboard navigation overrides saved actions, changes real tabs and preserves capture and closed-menu guards");
}
static void test_standby_menu_entry(void)
{
    config_menu_t menu; ui_state_t state;
    const usb_hid_menu_event_t open={.action=USB_HID_MENU_ACTION_OPEN,
        .source=USB_HID_MENU_SOURCE_KEY_BASE|0xE3U,.fixed_action=USB_HID_MENU_ACTION_NONE};
    const usb_hid_menu_event_t close={.action=USB_HID_MENU_ACTION_CLOSE,
        .source=USB_HID_MENU_SOURCE_KEY_BASE|0x48U,.fixed_action=USB_HID_MENU_ACTION_NONE};
    reset(&menu,&state); menu.active=0U; menu.tab=CONFIG_TAB_VIDEO;
    g_onee_standby_available=1U;
    ui_handle_usb_menu_event(&state,&menu,&open);
    assert(menu.active && opens==1U && !key_count && menu.tab==CONFIG_TAB_BOOT_SETTINGS &&
        menu.item_focus==CONFIG_MENU_BOOT_ONEE_ITEM);
    /* Menu opening only focuses the switch; it must not synthesize Enter. */
    ui_handle_usb_menu_event(&state,&menu,&open);
    assert(opens==1U && !key_count);
    ui_handle_usb_menu_event(&state,&menu,&close);
    assert(!menu.active && closes==1U && !key_count);
    reset(&menu,&state); menu.active=0U; menu.tab=CONFIG_TAB_VIDEO; menu.item_focus=4U;
    ui_handle_usb_menu_event(&state,&menu,&open);
    assert(menu.active && menu.tab==CONFIG_TAB_VIDEO && menu.item_focus==4U);
    puts("PASS standby keys open the focused manual switch without selecting it; normal menu focus stays intact");
}
static void test_legacy_sources(void)
{
    config_menu_t menu; ui_state_t state;
    reset(&menu,&state);
    menu.usb_menu_bindings[CONFIG_MENU_USB_BIND_ACTION_DOWN]=USB_HID_MENU_SOURCE_KEY_BASE+4;
    const usb_hid_menu_event_t event={.action=USB_HID_MENU_ACTION_NONE,
        .source=USB_HID_MENU_SOURCE_KEY_BASE+4,.fixed_action=USB_HID_MENU_ACTION_NONE};
    ui_handle_usb_menu_event(&state,&menu,&event);
    assert(key_count==1 && keys[0]==UI_KEY_DOWN);
    ui_handle_usb_menu_event(&state,&menu,NULL);
    assert(key_count==1);
    puts("PASS existing remappable keyboard sources retain their path");
}
int main(void)
{
    _Static_assert(USB_HID_MENU_ACTION_VTW_SLUG_TOGGLE==15,"old action values changed");
    _Static_assert(USB_HID_MENU_ACTION_BACK==16,"Back must append to old actions");
    _Static_assert(USB_HID_MENU_ACTION_TOGGLE==17,"Toggle must append to old actions");
    test_fixed_navigation_and_explicit_bindings(); test_learning_and_opening_capture();
    test_toggle_children_and_safety(); test_fixed_tabs_leave_real_child_state();
    test_fixed_keyboard_navigation(); test_standby_menu_entry(); test_legacy_sources();
    return 0;
}
'''
    OUT.mkdir(parents=True, exist_ok=True)
    source = OUT / "test.c"
    source.write_text(code, encoding="utf-8")
    executable = OUT / "test.exe"
    command = [str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror"]
    if "mingw" in compiler.as_posix().lower():
        command.append("-static")
    command += ["-I", str(FRONT), str(source), "-o", str(executable)]
    subprocess.run(command, cwd=ROOT, check=True)
    result=subprocess.run([str(executable)], cwd=ROOT, check=True,
                          capture_output=True,text=True,encoding="utf-8")
    print(result.stdout,end="")
    (OUT/"validation.json").write_text(json.dumps({
        "hardware_test":False,"production_gamepad_dispatcher_and_close_consumer":True,
        "ui_event_handler_mocked":True,
        "coverage":"existing7groups plus actual About tab scroll resets, Back child routing and Home whole-menu close",
        "source_sha256":{name:hashlib.sha256((FRONT/name).read_bytes()).hexdigest()
                         for name in ("main.c","config_menu.c","config_menu.h","usb_hid_service.c")},
        "test_script_sha256":hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "harness_sha256":hashlib.sha256(source.read_bytes()).hexdigest(),
        "exit_code":result.returncode,"stdout":result.stdout,
    },indent=2)+"\n",encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
