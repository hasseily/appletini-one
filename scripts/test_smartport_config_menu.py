#!/usr/bin/env python3
"""Source-level regression tests for the SmartPort boot menu wiring.

These tests run without Vitis or hardware:

    python scripts/test_smartport_config_menu.py
"""

from __future__ import annotations

import re
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
CONFIG_MENU_C = REPO_ROOT / "ps_sources" / "frontend" / "config_menu.c"
CONFIG_MENU_H = REPO_ROOT / "ps_sources" / "frontend" / "config_menu.h"
CONFIG_MENU_INTERNAL_H = REPO_ROOT / "ps_sources" / "frontend" / "config_menu_internal.h"
CONFIG_MENU_DEVICE_TABS_C = REPO_ROOT / "ps_sources" / "frontend" / "config_menu_device_tabs.c"
CONFIG_MENU_HELP_C = REPO_ROOT / "ps_sources" / "frontend" / "config_menu_help.c"
CONFIG_MENU_MAIN_TABS_C = REPO_ROOT / "ps_sources" / "frontend" / "config_menu_main_tabs.c"
FRONTEND_MAIN_C = REPO_ROOT / "ps_sources" / "frontend" / "main.c"
COMPOSITOR_C = REPO_ROOT / "ps_sources" / "frontend" / "compositor.c"
COMPOSITOR_H = REPO_ROOT / "ps_sources" / "frontend" / "compositor.h"
DEBUG_OVERLAY_C = REPO_ROOT / "ps_sources" / "frontend" / "debug_overlay.c"
DEBUG_OVERLAY_H = REPO_ROOT / "ps_sources" / "frontend" / "debug_overlay.h"
BOOT_MENU_SERVICE_C = REPO_ROOT / "ps_sources" / "frontend" / "boot_menu_service.c"
SMARTPORT_C = REPO_ROOT / "ps_sources" / "frontend" / "smartport_service.c"
SMARTPORT_H = REPO_ROOT / "ps_sources" / "frontend" / "smartport_service.h"
UART_CONTROL_C = REPO_ROOT / "ps_sources" / "frontend" / "uart_control.c"
UART_CONTROL_H = REPO_ROOT / "ps_sources" / "frontend" / "uart_control.h"
VITIS_SCRIPT = REPO_ROOT / "scripts" / "create_vitis_workspace.py"


class TestFailure(Exception):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise TestFailure(message)


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def config_version(source: str) -> int:
    match = re.search(r"#define APPLETINI_CFG_VERSION\s+(\d+)U", source)
    require(match is not None, "config version define must be present")
    return int(match.group(1))


def test_menu_stores_eight_smartport_paths() -> None:
    header = read(CONFIG_MENU_H)
    source = read(CONFIG_MENU_C)

    require("int (*set_smartport_image_path)(void *ctx, uint8_t device, const char *path);" in header,
            "menu platform callback must pass SmartPort device number")
    require("char smartport_disk_paths[8][CONFIG_MENU_PATH_LEN];" in header,
            "menu state must store one image path per SmartPort device")
    require('"smartport.disk.%u.path=%s\\n"' in source and
            '"smartport.disk.%u.enabled=%s\\n"' in source and
            "for (uint32_t device = 0U; device < SMARTPORT_DEVICE_COUNT; ++device)" in source,
            "saved config must include one clean SmartPort disk path and enabled key per device")


def test_tab_switch_resets_item_focus() -> None:
    source = read(CONFIG_MENU_C)
    next_start = source.find("static void config_menu_next_tab")
    prev_start = source.find("static void config_menu_prev_tab")
    next_end = prev_start
    prev_end = source.find("static void config_menu_next_item")

    require(next_start >= 0 and prev_start > next_start and prev_end > prev_start,
            "tab navigation helpers must be present")
    require("menu->item_focus = 0U;" in source[next_start:next_end] and
            "menu->item_focus = 0U;" in source[prev_start:prev_end],
            "moving between tabs must put the cursor on the top row")


def test_browser_selection_consumes_tab_and_esc_locally() -> None:
    source = read(CONFIG_MENU_C)
    start = source.find("if (menu->browser_active != 0U) {")
    end = source.find("\n\n    switch (input.key) {", start)

    require(start >= 0 and end > start, "browser input block must be present")
    block = source[start:end]
    require("case UI_KEY_TAB:\n"
            "        case UI_KEY_SHIFT_TAB:\n"
            "            return 1U;" in block,
            "browser selection list must consume Tab/Del instead of tab-switching")
    require("case UI_KEY_PAGE_DOWN:\n"
            "        case UI_KEY_DOWN:\n"
            "            config_menu_browser_move(menu, 1);" in block and
            "case UI_KEY_PAGE_UP:\n"
            "        case UI_KEY_UP:\n"
            "            config_menu_browser_move(menu, -1);" in block,
            "browser selection list must still navigate with up/down")
    require("case UI_KEY_ESC:\n"
            "            config_menu_browser_close(menu);\n"
            "            return 1U;" in block and
            "config_menu_set_active(menu, 0U)" not in block,
            "browser selection list ESC must close only the browser")


def test_menu_applies_all_smartport_paths() -> None:
    source = read(CONFIG_MENU_C)
    frontend_main = read(FRONTEND_MAIN_C)

    require("for (uint8_t device = 1U; device <= SMARTPORT_DEVICE_COUNT; ++device)" in source,
            "runtime apply must iterate SmartPort devices 1..8")
    require("menu->platform.set_smartport_image_path(menu->platform.ctx,\n"
            "                                                      device,\n"
            "                                                      \"\")" in source,
            "runtime apply must clear stale SmartPort paths before publishing enabled devices")
    require("if (menu->smartport_slots[index] != 0U &&\n"
            "            config_menu_smartport_path_used_before(\n"
            "                menu,\n"
            "                device,\n"
            "                menu->smartport_disk_paths[index]) != 0U) {\n"
            "            menu->smartport_slots[index] = 0U;\n"
            "            menu->smartport_disk_paths[index][0] = '\\0';\n"
            "        }" in source,
            "runtime apply must clear later duplicate SmartPort paths from menu state")
    require("path = (menu->smartport_slots[index] != 0U) ?\n"
            "            menu->smartport_disk_paths[index] : \"\";" in source,
            "runtime apply must disable SmartPort devices without selected images")
    require("menu->platform.set_smartport_image_path(menu->platform.ctx,\n"
            "                                                      device,\n"
            "                                                      path)" in source,
            "runtime apply must publish each SmartPort path to the backend")
    require("static int menu_platform_set_smartport_image(void *ctx, uint8_t device, const char *path)" in frontend_main,
            "frontend platform callback must accept SmartPort device number")
    require("return smartport_service_set_image_path(device, path);" in frontend_main,
            "frontend platform callback must forward one-based SmartPort device number")


def test_browser_targets_each_smartport_device() -> None:
    source = read(CONFIG_MENU_C)
    tabs = read(CONFIG_MENU_DEVICE_TABS_C)

    require("CONFIG_BROWSER_TARGET_SMARTPORT_1" in source and
            "CONFIG_BROWSER_TARGET_SMARTPORT_8" in source,
            "browser target enum must represent SP1 through SP8")
    require("static uint8_t config_menu_browser_is_smartport_target(uint8_t target)" in source,
            "browser helper must recognize SmartPort targets")
    require("static uint8_t config_menu_browser_target_smartport_device(uint8_t target)" in source,
            "browser helper must convert target to SmartPort device number")
    require("case CONFIG_TAB_SMARTPORT:\n        return SMARTPORT_DEVICE_COUNT + 3U; /* overlay + N slots + ram disk + SuperSprite */" in source,
            "SmartPort tab must expose the overlay toggle, SuperSprite, SP1-SP8, and RAM32")
    require('"Show drive activity overlay"' in tabs and
            "menu->disk2_activity_visible" in tabs and
            '"SuperSprite VDP + PSG (Slot 7, disables SmartPort)"' in tabs and
            "y + ((int)i + 1) * row_h" in tabs and
            "menu->item_focus == (i + 1U)" in tabs and
            "menu->sp_ramdisk_enabled" in tabs and
            "SMARTPORT_DEVICE_COUNT + 1U" in tabs and
            "SMARTPORT_DEVICE_COUNT + 3) * row_h" in tabs and
            '"RAM32: 32MB volatile ram disk"' in tabs,
            "SmartPort tab must draw the shared overlay toggle, SuperSprite, SP1-SP8, and RAM32")
    require("if (menu->item_focus == 0U) {\n"
            "            if (menu->supersprite_enabled != 0U) {" in source and
            "if (menu->item_focus == SMARTPORT_DEVICE_COUNT + 2U) {\n"
            "            menu->supersprite_enabled = menu->supersprite_enabled ? 0U : 1U;" in source and
            "if (menu->supersprite_enabled != 0U) {\n"
            "            config_menu_set_status(menu, 1U, \"SUPERSPRITE DISABLES SMARTPORT\");\n"
            "            break;" in source and
            "if (menu->item_focus == SMARTPORT_DEVICE_COUNT + 1U) {\n"
            "            menu->sp_ramdisk_enabled =" in source and
            "RAM32 32MB RAM DISK ON (VOLATILE)" in source and
            "slot = menu->item_focus - 1U;" in source and
            "config_menu_open_browser(menu,\n"
            "                                     (uint8_t)(CONFIG_BROWSER_TARGET_SMARTPORT_1 + slot));" in source,
            "SmartPort tab ENTER must toggle item 0/SuperSprite/RAM32 and open the browser for focused SP devices")
    require("menu->tab == CONFIG_TAB_SMARTPORT && menu->item_focus == 0U" in source and
            "menu->supersprite_enabled != 0U" in source and
            "SUPERSPRITE DISABLES SMARTPORT" in source,
            "SmartPort tab left/right must toggle the shared drive activity overlay unless SuperSprite owns slot 7")
def test_browser_splits_po_images_by_size() -> None:
    source = read(CONFIG_MENU_C)
    help_source = read(CONFIG_MENU_HELP_C)
    service = read(SMARTPORT_C)

    require("#define CONFIG_DISK2_PO_IMAGE_BYTES 143360U" in source,
            "Disk II .po filter must use the 140K ProDOS image size")
    require("#define CONFIG_SMARTPORT_140K_PO_IMAGE_BYTES 143360U" in source,
            "SmartPort .po filter must include the 140K ProDOS image size")
    require("#define CONFIG_SMARTPORT_800K_PO_IMAGE_BYTES 819200U" in source,
            "SmartPort .po filter must use the 800K ProDOS image size")
    require("HELP(smartport_ram32," in help_source and
            "volatile 32MB SmartPort block device" in help_source and
            "OVERRIDE(SMARTPORT_DEVICE_COUNT + 1U, smartport_ram32)" in help_source,
            "SmartPort help must include a RAM32-specific explanation")
    require("static uint8_t config_menu_has_smartport_ext(const char *name, FSIZE_t size)" in source,
            "SmartPort extension helper must receive the file size")
    require("static uint8_t config_menu_has_disk2_ext(const char *name, FSIZE_t size)" in source,
            "Disk II extension helper must receive the file size")
    require("(config_menu_str_ieq(dot, \".po\") != 0U &&\n"
            "             (size == (FSIZE_t)CONFIG_SMARTPORT_140K_PO_IMAGE_BYTES ||\n"
            "              size == (FSIZE_t)CONFIG_SMARTPORT_800K_PO_IMAGE_BYTES))" in source,
            "SmartPort browser must accept 140K and 800K .po images")
    require("(config_menu_str_ieq(dot, \".po\") != 0U &&\n"
            "             size == (FSIZE_t)CONFIG_DISK2_PO_IMAGE_BYTES)" in source,
            "Disk II browser must accept only 140K .po images")
    require("return config_menu_has_smartport_ext(info->fname, info->fsize);" in source,
            "SmartPort browser must pass FatFs file size to the extension helper")
    require("return config_menu_has_disk2_ext(info->fname, info->fsize);" in source,
            "Disk II browser must pass FatFs file size to the extension helper")
    require("Supported: HDV, 2MG, 2IMG, 140K PO, and 800K PO." in help_source,
            "SmartPort help must document both supported PO sizes")
    require("SP_DISK2_PRODOS_IMAGE_BYTES" not in service and
            "size == (FSIZE_t)SP_DISK2_PRODOS_IMAGE_BYTES" not in service,
            "SmartPort service must not reject 140K .po images at load time")


def test_smartport_service_public_api_is_one_based() -> None:
    header = read(SMARTPORT_H)
    source = read(SMARTPORT_C)
    frontend_main = read(FRONTEND_MAIN_C)

    require("Device numbers are SmartPort units 1..8" in header,
            "service header must document one-based public device numbers")
    require("static int service_device_to_index(uint8_t device, uint8_t *index_out)" in source,
            "service must translate one-based public device numbers privately")
    require("if (device == 0U || device > SP_MAX_DEVICES)" in source,
            "service API must reject public device 0")
    require("*index_out = (uint8_t)(device - 1U);" in source,
            "service API must map public device N to private index N-1")
    require("smartport_service_get_image_path(1U)" in frontend_main,
            "startup print should refer to SP1 using one-based API")
    require("smartport_service_read_block(1U, block_num, buffer, count, actual_out)" in frontend_main,
            "UART debug read should target SP1 using one-based API")


def test_draws_selected_path_for_every_smartport_device() -> None:
    source = read(CONFIG_MENU_DEVICE_TABS_C)

    require("config_menu_basename(menu->smartport_disk_paths[i])" in source,
            "SmartPort page must draw each device's selected image basename")
    require('"ENTER OPENS SELECTED SP BROWSER; [EMPTY] CLEARS DEVICE"' not in source,
            "SmartPort page must not carry tab-local key mapping helper text")


def test_menu_uses_only_main_key_mapping_help() -> None:
    ui_source = read(REPO_ROOT / "ps_sources" / "frontend" / "config_menu_ui.c")
    require('"BOOT MENU"' in ui_source and
            '"Apple keyboard"' in ui_source,
            "Apple-keyboard ownership must remain visible in the header")
    require('"ACTIVE"' in ui_source and
            '"* ACTIVE"' not in ui_source and
            '"USB device"' in ui_source,
            "USB ownership and active state must be visible in the header")
    require('"Tab/Del"' in ui_source and
            '"Navigate"' in ui_source and
            '"Esc"' in ui_source,
            "footer must draw compact key-style controls")


def test_clock_fields_support_left_right_adjustment() -> None:
    source = read(CONFIG_MENU_C)

    require("static uint8_t config_menu_adjust_clock_field(config_menu_t *menu, int8_t delta)" in source and
            "menu->item_focus < 2U || menu->item_focus > 7U" in source and
            "config_menu_adjust_u16_wrap(menu->clock_time.year" in source and
            "config_menu_adjust_u8_wrap(menu->clock_time.month" in source and
            "config_menu_adjust_u8_wrap(menu->clock_time.day" in source and
            "config_menu_adjust_u8_wrap(menu->clock_time.hour" in source and
            "config_menu_adjust_u8_wrap(menu->clock_time.min" in source and
            "config_menu_adjust_u8_wrap(menu->clock_time.sec" in source,
            "clock date/time fields must share a wraparound left/right adjustment helper")
    require("menu->tab == CONFIG_TAB_CLOCK &&\n"
            "        config_menu_adjust_clock_field(menu, delta) != 0U" in source,
            "left/right input must adjust focused clock fields")
    require(source.count("(void)config_menu_adjust_clock_field(menu, 1);") >= 6,
            "Enter must keep incrementing every editable clock field through the same helper")


def test_menu_rejects_duplicate_smartport_paths() -> None:
    source = read(CONFIG_MENU_C)

    require("static uint8_t config_menu_path_ieq(const char *a, const char *b)" in source,
            "menu duplicate checks must compare paths case-insensitively")
    require("static uint8_t config_menu_smartport_path_in_use(const config_menu_t *menu,\n"
            "                                                 uint8_t device,\n"
            "                                                 const char *path)" in source,
            "menu must check whether another SP unit already uses a selected path")
    require("static uint8_t config_menu_smartport_path_used_before(const config_menu_t *menu,\n"
            "                                                      uint8_t device,\n"
            "                                                      const char *path)" in source,
            "runtime apply must keep the first configured duplicate and suppress later ones")
    require("if (config_menu_smartport_path_in_use(menu, device, path) != 0U) {\n"
            "            (void)snprintf(text,\n"
            "                           sizeof(text),\n"
            "                           \"SMARTPORT SP%u DUPLICATE IMAGE\"," in source,
            "browser selection must reject duplicate SmartPort image paths before saving")
    require("config_menu_set_status(menu, 1U, text);\n"
            "            return;\n"
            "        }\n"
            "        config_menu_copy_text(menu->smartport_disk_paths[device - 1U]" in source,
            "duplicate rejection must happen before mutating the selected SP path")


def test_browser_dims_duplicate_smartport_images() -> None:
    source = read(CONFIG_MENU_C)
    internal = read(CONFIG_MENU_INTERNAL_H)

    require("#define HGR_DIMMED CMUI_COLOR_DIM" in internal,
            "browser must have a dimmed color for already-mounted SmartPort images")
    require("static uint8_t config_menu_browser_entry_is_duplicate_smartport_image(\n"
            "    const config_menu_t *menu,\n"
            "    const config_browser_entry_t *entry)" in source,
            "browser draw path must identify duplicate SmartPort image entries")
    require("entry->type != CONFIG_BROWSER_ENTRY_FILE ||\n"
            "        config_menu_browser_is_smartport_target(menu->browser_target) == 0U" in source,
            "duplicate dimming must only apply to SmartPort file entries")
    require("dimmed = config_menu_browser_entry_is_duplicate_smartport_image(menu, &entry);" in source,
            "browser row drawing must compute duplicate-image dim state")
    require("hgr_draw_item_with_lock_ex(\n"
            "            fb,\n"
            "            x,\n"
            "            row_y + ((int)row * row_h)," in source and
            "            focused,\n"
            "            line,\n"
            "            color,\n" in source and
            "            (uint8_t)(config_menu_browser_is_disk2_target(menu->browser_target) != 0U &&\n"
            "                      entry.type == CONFIG_BROWSER_ENTRY_FILE),\n"
            "            entry.read_only,\n"
            "            dimmed);" in source,
            "browser row drawing must pass duplicate-image dim state into item rendering")
    require("(dimmed != 0U) ? CMUI_COLOR_DIM" in source and
            "CMUI_COLOR_ROW_DISABLED" in source,
            "dimmed browser rows must use gray text even when focused")


def test_settings_menu_controls_debug_overlay_and_bezel() -> None:
    header = read(CONFIG_MENU_H)
    source = read(CONFIG_MENU_C)
    main_tabs = read(CONFIG_MENU_MAIN_TABS_C)
    frontend_main = read(FRONTEND_MAIN_C)
    compositor = read(COMPOSITOR_C)
    compositor_h = read(COMPOSITOR_H)
    debug_overlay = read(DEBUG_OVERLAY_C)
    debug_overlay_h = read(DEBUG_OVERLAY_H)
    uart_control = read(UART_CONTROL_C)
    uart_control_h = read(UART_CONTROL_H)
    vitis_script = read(VITIS_SCRIPT)
    boot_draw = main_tabs[
        main_tabs.index("void config_menu_draw_boot_settings"):
        main_tabs.index("void config_menu_draw_video")
    ]
    video_draw = main_tabs[
        main_tabs.index("void config_menu_draw_video"):
        main_tabs.index("void config_menu_draw_clock")
    ]

    require("int (*set_bezel_path)(void *ctx, const char *path);" in header,
            "menu platform must expose a runtime bezel loader callback")
    require("uint32_t display_page;" not in header and
            "uint8_t show_debugging;" in header and
            "uint8_t show_bezel;" in header and
            "char bezel_path[CONFIG_MENU_PATH_LEN];" in header,
            "menu config must persist overlay visibility and selected bezel path without page state")

    require(config_version(source) >= 100,
            "config version must bump when adding persisted settings keys")
    require('"display_page=%u\\n"' not in source and
            '"video.debug.overlay=%s\\n"' in source and
            '"video.bezel.visible=%s\\n"' in source and
            '"video.bezel.path=%s\\n"' in source,
            "saved config must include debug visibility, bezel visibility, and bezel path without display_page")
    require('strcmp(key, "display_page") == 0' not in source and
            'strcmp(key, "video.debug.overlay") == 0' in source and
            'strcmp(key, "video.bezel.visible") == 0' in source and
            'strcmp(key, "video.bezel.path") == 0' in source,
            "config loader must parse debug visibility, bezel visibility, and bezel path without display_page")
    require("case CONFIG_TAB_BOOT_SETTINGS:\n        return CONFIG_MENU_BOOT_ITEM_COUNT;" in source,
            "boot settings tab must expose boot controls and USB menu binding rows")
    require('"Boot menu"' in boot_draw and '"Boot device"' in boot_draw and
            '"USB MENU BINDINGS"' in boot_draw and
            '"Show debugging"' not in boot_draw and
            '"Show bezel"' not in boot_draw and
            '"Bezel"' not in boot_draw and
            '"Display page"' not in boot_draw,
            "boot settings tab must not draw video/debug/bezel rows")
    require('"Show debugging"' in video_draw and '"Show bezel"' in video_draw and
            '"Bezel"' in video_draw and '"Display page"' not in video_draw,
            "video tab must draw debug visibility and bezel rows")
    require('"Auto 0:/bezel.png, 0:/bezels/bezel.png"' in source and
            "return menu->bezel_path;" in source,
            "video Bezel row must show the auto lookup paths or the full custom path")

    require("menu->show_debugging = (menu->show_debugging != 0U) ? 0U : 1U;" in source,
            "video Show debugging row must toggle the debug overlay flag")
    require("menu->show_bezel = (menu->show_bezel != 0U) ? 0U : 1U;" in source,
            "video Show bezel row must toggle the bezel image flag")
    require("CONFIG_BROWSER_TARGET_BEZEL" in source and
            "static uint8_t config_menu_browser_is_bezel_target(uint8_t target)" in source,
            "file browser must have a bezel target")
    require("return \"Bezel PNG\";" in source and
            "return \"[AUTO BEZEL]\";" in source,
            "bezel browser must label the picker and auto fallback row")
    require("return config_menu_has_png_ext(info->fname);" in source,
            "bezel browser must filter to PNG files")
    require("config_menu_open_browser(menu, CONFIG_BROWSER_TARGET_BEZEL);" in source,
            "video Bezel row must open the bezel picker")
    require("config_menu_parent_path(menu->bezel_path,\n"
            "                                menu->browser_dir,\n"
            "                                sizeof(menu->browser_dir));" in source,
            "bezel picker should reopen beside the currently selected custom bezel")
    require("menu->bezel_path[0] = '\\0';\n"
            "        config_menu_save_settings(menu);\n"
            "        config_menu_apply_bezel(menu);" in source,
            "bezel browser empty row must restore auto bezel mode")

    require("menu_platform.set_bezel_path = menu_platform_set_bezel_path;" in frontend_main,
            "frontend menu platform must wire the bezel loader")
    require("static int ui_apply_bezel_path(const char *path)" in frontend_main and
            "if (path != NULL && path[0] != '\\0')" in frontend_main and
            '#define UI_BEZEL_SD_PATH "0:/bezel.png"' in frontend_main and
            '#define UI_BEZEL_SD_FALLBACK_PATH "0:/bezels/bezel.png"' in frontend_main and
            "static int ui_load_auto_bezel(const char *skip_path)" in frontend_main and
            "UI_BEZEL_SD_PATH,\n        UI_BEZEL_SD_FALLBACK_PATH" in frontend_main and
            "ui_load_auto_bezel(path)" in frontend_main and
            "ui_load_auto_bezel(NULL)" in frontend_main and
            "return ui_load_embedded_bezel();" in frontend_main,
            "frontend bezel loader must support custom, SD auto paths, then embedded fallback")
    require("ui_init_bezel();" not in frontend_main,
            "boot must preserve the configured bezel selection")
    require("menu == NULL || menu->show_bezel != 0U" in frontend_main and
            "fb16_clear(fb, FB16_COLOR_BLACK);" in frontend_main,
            "frontend must allow the boot menu to disable bezel drawing")
    require("static void ui_prepare_static_background(uint16_t *fb, uint8_t show_bezel)" in frontend_main and
            "g_output_slot_bg_generation[slot] != generation" in frontend_main and
            "ui_restore_debug_overlay_regions(fb, show_bezel);" in frontend_main and
            "ui_mark_slot_dynamic(fb);" in frontend_main,
            "frontend must cache static bezel backgrounds per output slot and restore dynamic regions")
    require("UI_DEBUG_OVERLAY_REFRESH_FRAMES" not in frontend_main and
            "g_output_slot_debug_generation" not in frontend_main and
            "ui_debug_overlay_refresh_due" not in frontend_main,
            "debug overlay must not use slot-throttled caching that can show stale values")
    require("static uint8_t g_output_slot_debug_dirty[COMP_OUT_SLOT_COUNT];" in frontend_main and
            "ui_debug_overlay_slot_dirty(fb)" in frontend_main and
            "ui_note_debug_overlay_drawn(fb);" in frontend_main and
            "ui_note_debug_overlay_cleared(fb);" in frontend_main and
            "if (show_debugging != 0U ||\n        ui_debug_overlay_slot_dirty(fb) != 0U)" in frontend_main,
            "debug overlay restore must be skipped when debug is off and the slot is clean")
    require("#include \"debug_overlay.h\"" in frontend_main and
            "debug_overlay_snapshot_t debug_snapshot;" in frontend_main and
            "ui_collect_debug_overlay_snapshot(&debug_snapshot, s, menu, show_bezel);" in frontend_main and
            "debug_overlay_draw(fb, &debug_snapshot);" in frontend_main,
            "frontend must collect and draw the standalone debug overlay")
    require("typedef struct {\n    int x;\n    int y;\n    int w;\n    int h;\n} debug_overlay_rect_t;" in debug_overlay_h and
            "debug_overlay_region_count" in debug_overlay_h and
            "debug_overlay_draw" in debug_overlay_h,
            "debug overlay header must expose its restore regions and draw entry point")
    require("draw_header" in debug_overlay and
            "draw_system" in debug_overlay and
            "draw_input" in debug_overlay and
            "draw_storage" in debug_overlay and
            "draw_video" in debug_overlay and
            "draw_softswitches" in debug_overlay and
            "draw_performance" in debug_overlay and
            "OWNER %s" in debug_overlay and
            "HUD_TOP_Y       (FB16_HEIGHT - HUD_TOP_H - 8)" in debug_overlay and
            "FPS 1080p %lu.%02lu  Apple area %lu.%02lu" in debug_overlay and
            '"USB device"' in debug_overlay and
            '"Apple keyboard"' in debug_overlay,
            "debug overlay must draw compact bottom system/input/storage/video/soft-switch/pipeline HUD panels")
    require("HUD_SOFT_X      HUD_RIGHT_X" in debug_overlay and
            '"Soft Switches"' in debug_overlay and
            '"80STORE"' in debug_overlay and
            '"RAMRD"' in debug_overlay and
            '"RAMWRT"' in debug_overlay and
            '"ALTZP"' in debug_overlay and
            '"TEXT"' in debug_overlay and
            '"MIXED"' in debug_overlay and
            '"PAGE2"' in debug_overlay and
            '"HIRES"' in debug_overlay and
            '"ALTCHAR"' in debug_overlay and
            '"80COL"' in debug_overlay and
            '"DHIRES"' in debug_overlay and
            '"LCBNK2"' in debug_overlay and
            '"LCRD"' in debug_overlay and
            '"LCWRT"' in debug_overlay and
            "0xC029U" in debug_overlay and
            '"%04lX SHR %u"' in debug_overlay and
            "CARD_CTRL_SOFTSW_RAMWORKS_BANK_MASK" in debug_overlay,
            "debug overlay must split tracked soft switches into their own live window")
    require("static uint32_t g_apple_fps_x100 = 0U;" in frontend_main and
            "uint32_t apple_blits = g_compositor_apple_frames_drawn;" in frontend_main and
            "uint32_t renderer_publish_seq = apple_fb_reader_publish_seq();" in frontend_main and
            "snapshot->apple_fps_x100 = g_apple_fps_x100;" in frontend_main and
            "snapshot->renderer_fps_x100 = g_renderer_fps_x100;" in frontend_main and
            "uint32_t apple_fps_x100;" in debug_overlay_h and
            "uint32_t compositor_fps_x100;" in debug_overlay_h and
            "uint32_t renderer_fps_x100;" in debug_overlay_h,
            "debug overlay must report separate HDMI, compositor, renderer, and Apple-blit FPS")
    require("#include \"xiltimer.h\"" in compositor and
            "extern volatile uint32_t g_compositor_last_ui_us;" in compositor_h and
            "extern volatile uint32_t g_compositor_last_apple_us;" in compositor_h and
            "extern volatile uint32_t g_compositor_last_sync_us;" in compositor_h and
            "extern volatile uint32_t g_compositor_last_total_us;" in compositor_h and
            "XTime_GetTime(&ui_start);" in compositor and
            "apple_drawn = draw_apple_subwindow(fb);" in compositor and
            "XTime_GetTime(&sync_start);" in compositor and
            "g_compositor_last_total_us = ticks_to_us(total_end - total_start);" in compositor,
            "compositor must time UI draw, Apple blit, sync/publish, and total compose work")
    require("uint32_t compositor_ui_us;" in debug_overlay_h and
            "uint32_t compositor_apple_us;" in debug_overlay_h and
            "uint32_t compositor_sync_us;" in debug_overlay_h and
            "uint32_t compositor_total_us;" in debug_overlay_h and
            "snapshot->compositor_ui_us = g_compositor_last_ui_us;" in frontend_main and
            "snapshot->compositor_apple_us = g_compositor_last_apple_us;" in frontend_main and
            "snapshot->compositor_sync_us = g_compositor_last_sync_us;" in frontend_main and
            "snapshot->compositor_total_us = g_compositor_last_total_us;" in frontend_main and
            "snapshot->renderer_publish_seq = g_renderer_publish_seq;" in frontend_main and
            "\"FPS comp %lu.%02lu render %lu.%02lu\"" in debug_overlay and
            "\"FPS blit %lu.%02lu hdmi %lu.%02lu\"" in debug_overlay and
            "\"Frames pub %lu seq %lu\"" in debug_overlay and
            "\"Comp UI %luus Apple %luus\"" in debug_overlay and
            "\"Sync %luus Total %luus\"" in debug_overlay and
            "\"Apple drawn %lu suppress %lu\"" in debug_overlay,
            "debug overlay must show compositor timing instrumentation")
    require("ui_update_fps(&ui);" in frontend_main and
            "ui_update_fps(s);" not in frontend_main,
            "FPS accounting must run from the vblank loop without enabling debug overlay")
    require("uint32_t fps_x100;" in uart_control_h and
            "uint32_t apple_fps_x100;" in uart_control_h and
            "uint8_t apple_video_50hz;" in uart_control_h and
            "snapshot->fps_x100 = g_fps_x100;" in frontend_main and
            "snapshot->apple_fps_x100 = g_apple_fps_x100;" in frontend_main and
            "snapshot->apple_video_50hz = ui_apple_video_50hz(g_config_menu_state);" in frontend_main and
            "apple_fps=%lu.%02lu hdmi_fps=%lu.%02lu" in uart_control,
            "UART status must expose non-debug Apple-area FPS and PAL/NTSC target")
    require("../../../ps_sources/frontend/debug_overlay.c" in vitis_script,
            "Vitis frontend build must compile the standalone debug overlay source")
TESTS = [
    test_menu_stores_eight_smartport_paths,
    test_tab_switch_resets_item_focus,
    test_browser_selection_consumes_tab_and_esc_locally,
    test_menu_applies_all_smartport_paths,
    test_browser_targets_each_smartport_device,
    test_browser_splits_po_images_by_size,
    test_smartport_service_public_api_is_one_based,
    test_draws_selected_path_for_every_smartport_device,
    test_menu_uses_only_main_key_mapping_help,
    test_clock_fields_support_left_right_adjustment,
    test_menu_rejects_duplicate_smartport_paths,
    test_browser_dims_duplicate_smartport_images,
    test_settings_menu_controls_debug_overlay_and_bezel,
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
        print(f"{len(TESTS) - len(failures)} of {len(TESTS)} SmartPort menu tests passed; "
              f"{len(failures)} failed")
        return 1
    print(f"{len(TESTS)} SmartPort menu tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
