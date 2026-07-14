#!/usr/bin/env python3
"""Source-level regression tests for config-menu profiles.

These tests run without Vitis or hardware:

    python scripts/test_config_profiles.py
"""

from __future__ import annotations

from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
FRONTEND = REPO_ROOT / "ps_sources" / "frontend"
CONFIG_MENU_C = FRONTEND / "config_menu.c"
CONFIG_MENU_H = FRONTEND / "config_menu.h"
CONFIG_MENU_HELP_C = FRONTEND / "config_menu_help.c"
CONFIG_MENU_INTERNAL_H = FRONTEND / "config_menu_internal.h"
CONFIG_MENU_PROFILES_C = FRONTEND / "config_menu_profiles.c"
CONFIG_MENU_UI_C = FRONTEND / "config_menu_ui.c"
CONFIG_MENU_UI_H = FRONTEND / "config_menu_ui.h"
FRONTEND_MAIN_C = FRONTEND / "main.c"
IMAGE_VERSIONS_H = REPO_ROOT / "ps_sources" / "image_versions.h"
PROFILE_MANAGER_C = FRONTEND / "profile_manager.c"
PROFILE_MANAGER_H = FRONTEND / "profile_manager.h"
VITIS_SCRIPT = REPO_ROOT / "scripts" / "create_vitis_workspace.py"


class TestFailure(Exception):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise TestFailure(message)


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def test_profile_filesystem_contract() -> None:
    header = read(PROFILE_MANAGER_H)
    source = read(PROFILE_MANAGER_C)

    require('#define PROFILE_MANAGER_ROOT "0:/profiles"' in header,
            "profiles must be rooted at 0:/profiles")
    require('#define PROFILE_MANAGER_CFG_NAME "appletini_cfg.txt"' in header,
            "profile config filename must be appletini_cfg.txt")
    require('#define PROFILE_MANAGER_THUMB_NAME "thumb.png"' in header,
            "profile thumbnail filename must be thumb.png")
    require("#define PROFILE_MANAGER_THUMB_W 560U" in header and
            "#define PROFILE_MANAGER_THUMB_H 384U" in header,
            "normalized profile thumbnails must be 560x384")
    require("FRESULT profile_manager_ensure_root(void)" in header and
            "f_mkdir(PROFILE_MANAGER_ROOT)" in source,
            "profile manager must create the profile root on demand")
    require("uint8_t profile_manager_is_profile_dir(const char *dir)" in source and
            "profile_manager_cfg_path(dir, cfg_path" in source and
            "f_stat(cfg_path, &info)" in source,
            "profile directories must be identified by appletini_cfg.txt")
    require("FRESULT profile_manager_create_profile(const char *parent_dir," in header and
            "profile_manager_profile_name_valid(name)" in source and
            "f_mkdir(path)" in source,
            "Save As must create the user-named profile directory")
    require("FRESULT profile_manager_rename_profile(const char *profile_dir," in header and
            "f_rename(profile_dir, path)" in source,
            "profile manager must support user-requested profile rename")


def test_autosave_remains_working_config() -> None:
    source = read(CONFIG_MENU_C)

    require('#define APPLETINI_CFG_PATH "0:/appletini_cfg.txt"' in source,
            "working autosave config must remain at 0:/appletini_cfg.txt")
    require("void config_menu_save_settings(config_menu_t *menu)\n{\n"
            "    (void)config_menu_save_settings_to_path(menu,\n"
            "                                            APPLETINI_CFG_PATH," in source,
            "normal autosave wrapper must write APPLETINI_CFG_PATH")
    require("config_menu_save_settings_to_path(menu, cfg_path, NULL)" in source,
            "profile save must use the same serializer with an explicit profile path")
    require("config_menu_save_settings_to_path(menu, APPLETINI_CFG_PATH, NULL)" in source,
            "profile load must autosave the resulting working config")
    require("config_menu_save_settings_to_path(menu, menu->profile_source_dir" not in source,
            "normal autosave must not write back to the selected profile")


def test_clean_config_schema_contract() -> None:
    source = read(CONFIG_MENU_C)

    require("#define APPLETINI_CFG_VERSION 103U" in source,
            "Border settings must use config version 103")
    require("config_menu_parse_config_line(line, &value)" in source and
            "hash = strchr(line, '#')" in source and
            "config_menu_ascii_lower_in_place(key)" in source,
            "config parser must support inline comments and case-insensitive dot keys")
    require('"appletini.config.version=%u\\n"' in source and
            'strcmp(key, "appletini.config.version") == 0' in source,
            "config version must use appletini-prefixed dot notation")
    require('"boot.menu.seconds=%s\\n"' in source and
            'strcmp(key, "boot.menu.seconds") == 0' in source and
            '"boot.menu.seconds") == 0' in source,
            "boot settings must use boot-prefixed dot notation")
    require('"0:/appletini.cfg"' not in source,
            "config source must use appletini_cfg.txt exclusively")
    require('"ram.enabled=%s\\n"' in source and
            'strcmp(key, "ram.enabled") == 0' in source and
            'menu->ramworks_enabled = menu->ram_enabled;' in source and
            '"ram.ramworks.enabled"' not in source,
            "RAM config must persist one RAM key and derive RamWorks from it")
    require('"ethernet.config.enabled=%s\\n"' in source and
            '"ethernet.address.mode=%s\\n"' in source and
            '"ethernet.mac=%s\\n"' in source and
            '"ethernet.ip=%s\\n"' in source and
            '"ethernet.subnet=%s\\n"' in source and
            '"ethernet.gateway=%s\\n"' in source,
            "Ethernet network config must persist in clean dot notation")
    require('strcmp(key, "ethernet.config.enabled") == 0' in source and
            'strcmp(key, "ethernet.address.mode") == 0' in source and
            'strcmp(key, "ethernet.mac") == 0' in source and
            'strcmp(key, "ethernet.ip") == 0' in source and
            'strcmp(key, "ethernet.subnet") == 0' in source and
            'strcmp(key, "ethernet.gateway") == 0' in source,
            "Ethernet network config parser must read the new dot keys")


def test_profile_load_overrides_current_without_updating_source_profile() -> None:
    source = read(CONFIG_MENU_C)
    profiles = read(CONFIG_MENU_PROFILES_C)

    require("uint8_t config_menu_load_profile_settings(config_menu_t *menu,\n"
            "                                          const char *profile_dir)" in source,
            "profile load helper must exist")
    require("config_menu_read_settings_from_path(menu, cfg_path, 1U, NULL)" in source,
            "profile load must reset current settings before parsing profile config")
    require("config_menu_copy_text(menu->profile_source_dir," in source,
            "selected profile path must be remembered for explicit saves")
    require("config_menu_apply_runtime(menu);" in source and
            "config_menu_apply_bezel(menu);" in source and
            "config_menu_apply_video_rom(menu);" in source,
            "profile load must apply runtime settings and startup assets")
    require("config_menu_load_profile_settings(menu, entry->path)" in profiles,
            "carousel profile selection must call the profile load helper")


def test_profile_bezel_changes_update_active_profile() -> None:
    source = read(CONFIG_MENU_C)

    require("static void config_menu_save_active_profile_if_selected(config_menu_t *menu)" in source and
            "config_menu_save_profile_settings(menu, menu->profile_source_dir)" in source,
            "menu must have a helper to update the selected profile")
    require("config_menu_copy_text(menu->bezel_path,\n"
            "                              sizeof(menu->bezel_path),\n"
            "                              path);\n"
            "        config_menu_save_settings(menu);\n"
            "        config_menu_apply_bezel(menu);\n"
            "        config_menu_save_active_profile_if_selected(menu);" in source,
            "choosing a bezel PNG must update the active profile config")
    require("menu->bezel_path[0] = '\\0';\n"
            "        config_menu_save_settings(menu);\n"
            "        config_menu_apply_bezel(menu);\n"
            "        config_menu_save_active_profile_if_selected(menu);" in source,
            "choosing auto bezel must update the active profile config")
    require("menu->show_bezel = (menu->show_bezel != 0U) ? 0U : 1U;\n"
            "        config_menu_save_settings(menu);\n"
            "        config_menu_save_active_profile_if_selected(menu);" in source,
            "left/right Show bezel changes must update the active profile config")
    require("menu->show_bezel = (menu->show_bezel != 0U) ? 0U : 1U;\n"
            "            config_menu_apply_runtime(menu);\n"
            "            config_menu_save_settings(menu);\n"
            "            config_menu_save_active_profile_if_selected(menu);" in source,
            "enter Show bezel changes must update the active profile config")


def test_subfolders_and_carousel_ui() -> None:
    header = read(CONFIG_MENU_H)
    source = read(CONFIG_MENU_C)
    profiles = read(CONFIG_MENU_PROFILES_C)
    manager = read(PROFILE_MANAGER_C)

    require("CONFIG_TAB_PROFILES" in source and '"Profiles"' in source,
            "Profiles tab must be registered in the config menu")
    require("#define CONFIG_MENU_PROFILE_ITEM_COUNT 5U" in header,
            "Profiles tab must expose choose/save/save-as/rename/image actions")
    require("CONFIG_PROFILE_UI_PARENT" in profiles and
            "CONFIG_PROFILE_UI_FOLDER" in profiles and
            "CONFIG_PROFILE_UI_PROFILE" in profiles,
            "carousel entries must distinguish parent, folders, and profiles")
    require("profile_manager_list_dir(menu->profile_dir" in profiles,
            "carousel must list the current profiles subfolder")
    require("PROFILE_MANAGER_ENTRY_PROFILE" in manager and
            "PROFILE_MANAGER_ENTRY_FOLDER" in manager,
            "profile manager must return both folder and profile entry types")
    require("profile_draw_carousel" in profiles and
            "profile_move(menu, 1)" in profiles and
            "profile_draw_card" in profiles,
            "profile selection must be a carousel, not the generic file list")
    require("menu->profile_carousel_active" in header + source + profiles,
            "menu state must track active carousel mode")
    require('profiles_copy_text(kind, sizeof(kind), "PROFILE")' not in profiles,
            "focused profile cards must not draw the green PROFILE type label")
    require("fb16_rect(fb, fb_x - 8, fb_y - 12, fb_w + 16, panel_h, FB16_COLOR_WHITE)" not in profiles,
            "carousel must not draw an outer white rectangle")
    require("fb16_rect(fb, x - 2, y - 2, w + 4, h + 4, CMUI_COLOR_ACCENT)" in profiles,
            "focused carousel thumbnails must have a one-pixel gap inside the accent border")


def test_profile_carousel_consumes_tab_and_esc_locally() -> None:
    profiles = read(CONFIG_MENU_PROFILES_C)
    start = profiles.find("if (menu == NULL || menu->profile_carousel_active == 0U ||")
    end = profiles.find("\n\nvoid config_menu_profiles_save_to_profile", start)

    require(start >= 0 and end > start, "profile carousel input block must be present")
    block = profiles[start:end]
    require("case UI_KEY_TAB:\n"
            "    case UI_KEY_SHIFT_TAB:\n"
            "        return 1U;" in block,
            "profile carousel must consume Tab/Del instead of tab-switching")
    require("case UI_KEY_PAGE_DOWN:\n"
            "    case UI_KEY_DOWN:\n"
            "    case UI_KEY_RIGHT:\n"
            "        profile_move(menu, 1);" in block and
            "case UI_KEY_PAGE_UP:\n"
            "    case UI_KEY_UP:\n"
            "    case UI_KEY_LEFT:\n"
            "        profile_move(menu, -1);" in block,
            "profile carousel must still navigate with arrows")
    require("case UI_KEY_ESC:\n"
            "        menu->profile_carousel_active = 0U;\n"
            "        return 1U;" in block and
            "config_menu_set_active(menu, 0U)" not in block,
            "profile carousel ESC must close only the carousel")


def test_profile_naming_and_virtual_keyboard() -> None:
    header = read(CONFIG_MENU_H)
    source = read(CONFIG_MENU_C)
    profiles = read(CONFIG_MENU_PROFILES_C)
    manager = read(PROFILE_MANAGER_C)
    boot_menu = read(FRONTEND / "boot_menu_service.c")

    require("uint8_t profile_name_editor_active;" in header and
            "char profile_name_editor_text[CONFIG_MENU_PATH_LEN];" in header,
            "menu state must carry profile name editor state")
    require("profile_name_editor_start(menu,\n"
            "                              CONFIG_PROFILE_NAME_MODE_SAVE_AS" in profiles and
            "profile_manager_create_profile(menu->profile_dir" in profiles,
            "Save As must open the name editor and create the chosen profile name")
    require("void config_menu_profiles_rename(config_menu_t *menu)" in profiles and
            "profile_manager_rename_profile(menu->profile_name_editor_target_dir" in profiles,
            "Rename must open the name editor and rename the selected profile")
    require('"Rename profile"' in profiles and
            "config_menu_profiles_rename(menu)" in source,
            "Profiles tab must expose and activate Rename profile")
    require("menu->profile_name_editor_virtual =\n"
            "        (menu->usb_bindings_editable == 0U) ? 1U : 0U;" in profiles and
            "static const char * const k_profile_vk_keys[CONFIG_PROFILE_VK_KEY_COUNT]" in profiles and
            "profile_vk_select(menu)" in profiles,
            "USB-owned menus must use the on-screen virtual keyboard")
    require("#define CONFIG_PROFILE_VK_KEY_W 108" in profiles and
            "#define CONFIG_PROFILE_VK_KEY_H 54" in profiles and
            "#define CONFIG_PROFILE_VK_KEY_SCALE 2" in profiles and
            "const int grid_w =" in profiles and
            "fb16_string_scaled(fb,\n"
            "                           key_x + ((key_w - text_w) / 2),\n"
            "                           key_y + ((key_h - text_h) / 2)" in profiles,
            "virtual keyboard labels must be larger and centered in wider keys")
    require("input.ascii != 0U" in profiles and
            "input->ascii = ascii;" in boot_menu,
            "Apple-keyboard-owned menus must accept typed profile names")
    require('"Save to current profile"' in profiles and
            '"Save to profile"' not in profiles,
            "Profiles tab must use the explicit current-profile save label")
    require("profile_manager_create_unique_profile" not in header + profiles + manager,
            "profile creation must not fall back to auto-generated New Profile names")


def test_profile_image_picker_and_normalization() -> None:
    source = read(CONFIG_MENU_C)
    profiles = read(CONFIG_MENU_PROFILES_C)
    manager = read(PROFILE_MANAGER_C)

    require("CONFIG_BROWSER_TARGET_PROFILE_IMAGE" in source,
            "generic PNG browser must have a profile-image target")
    require("return config_menu_has_png_ext(info->fname);" in source,
            "profile-image browser must accept PNG files")
    require("config_menu_profiles_set_image_from_png(menu, path)" in source,
            "selected PNG must be routed to profile image normalization")
    require("config_browser_preview_cache_t" in source and
            "static config_browser_preview_cache_t g_browser_preview_cache;" in source,
            "profile-image picker must keep a thumbnail preview cache")
    require("profile_manager_load_thumb_bgra32(entry.path,\n"
            "                                          &g_browser_preview_cache.pixels" in source and
            "config_menu_browser_preview_prepare(menu);" in source,
            "profile-image picker must load a preview for the highlighted PNG")
    require("config_menu_draw_browser_preview(fb," in source and
            "CONFIG_BROWSER_PROFILE_PREVIEW_W" in source and
            "config_menu_blit_scaled_bgra" in source,
            "profile-image picker must draw a scaled preview panel")
    require("CONFIG_BROWSER_PROFILE_IMAGE_VISIBLE_ROWS 11U" in source and
            "config_menu_browser_visible_rows(menu)" in source,
            "profile-image picker list height must stay above the footer")
    require("CONFIG_BROWSER_PROFILE_PREVIEW_H" in source and
            "PROFILE_MANAGER_THUMB_H" in source and
            "PROFILE_MANAGER_THUMB_W" in source,
            "profile-image picker preview height must be sized to the thumbnail aspect")
    require("config_menu_refresh_smartport_media_after_menu_sd(menu);" in source and
            "profile_manager_load_thumb_bgra32(entry.path" in source,
            "preview SD reads must refresh SmartPort media handles")
    require("profile_manager_normalize_thumb_png(path,\n"
            "                                            menu->profile_source_dir" in profiles,
            "Set image must write into the selected profile directory")
    require("profile_manager_thumb_path(profile_dir, thumb_path" in manager and
            "f_open(&file, thumb_path, FA_CREATE_ALWAYS | FA_WRITE)" in manager,
            "normalization must overwrite <profile>/thumb.png")
    require("width != PROFILE_MANAGER_THUMB_W" in manager and
            "height != PROFILE_MANAGER_THUMB_H" in manager,
            "PNG writer must enforce the normalized thumbnail dimensions")
    require("profile_make_normalized_rgba" in manager and
            "scaled_w" in manager and "scaled_h" in manager,
            "normalizer must resize the selected PNG before writing thumb.png")


def test_vitis_build_registration() -> None:
    vitis = read(VITIS_SCRIPT)
    logo_generator = read(REPO_ROOT / "scripts" / "generate_config_menu_logo_png.py")

    require('"../../../ps_sources/frontend/config_menu_profiles.c"' in vitis,
            "config menu profiles source must be registered in Vitis")
    require('"../../../ps_sources/frontend/config_menu_logo_png.c"' in vitis and
            "generate_config_menu_logo_png_sources" in vitis,
            "embedded config menu logo source must be generated and registered in Vitis")
    require("width > 640 or height > 96" in logo_generator,
            "config menu logo generator must accept the wider header logo asset")
    require('"../../../ps_sources/frontend/config_menu_ui.c"' in vitis,
            "modern config menu UI source must be registered in Vitis")
    require('"../../../ps_sources/frontend/profile_manager.c"' in vitis,
            "profile manager source must be registered in Vitis")
    require('"../../../ps_sources/frontend/uthernet2_control.c"' in vitis,
            "Uthernet II control source must be registered in Vitis")


def test_modern_config_menu_ui_contract() -> None:
    source = read(CONFIG_MENU_C)
    internal = read(CONFIG_MENU_INTERNAL_H)
    ui_h = read(CONFIG_MENU_UI_H)
    ui_c = read(CONFIG_MENU_UI_C)
    help_source = read(CONFIG_MENU_HELP_C)
    image_versions = read(IMAGE_VERSIONS_H)

    require('#include "config_menu_ui.h"' in internal,
            "config menu internals must include the modern UI layer")
    page_draw = source[
        source.index("static void config_menu_draw_page"):
        source.index("void config_menu_draw(uint16_t *fb")
    ]

    require("#define CMUI_MARGIN_X 48" in ui_h and
            "#define CMUI_MARGIN_Y 38" in ui_h and
            "#define CMUI_NAV_W 300" in ui_h and
            "#define CMUI_HEADER_H 116" in ui_h and
            "#define CMUI_ROW_H 34" in ui_h and
            "#define CMUI_BODY_SCALE 2" in ui_h and
            "#define CMUI_VALUE_LABEL_W 360" in ui_h and
            "#define CMUI_SLIDER_LABEL_W 320" in ui_h,
            "modern UI must lock compact 1080p margins, navigation width, rows, labels, and 16px body text scale")
    require("#define CMUI_COLOR_ROW_DISABLED" in ui_h and
            "#define CMUI_COLOR_DISABLED_EDGE" in ui_h,
            "disabled rows must use a distinct background and edge color, not only a nearby gray text shade")
    require("void cmui_screen_rects" in ui_c and
            "void cmui_header" in ui_c and
            "void cmui_footer" in ui_c and
            "void cmui_slider" in ui_c and
            "void cmui_nav_item" in ui_c and
            "void cmui_help_panel" in ui_c,
            "modern UI layer must centralize screen geometry, header/footer chrome, help panels, and reusable controls")
    require("cmui_screen_rects(&nav, &body, &footer);" in source and
            "cmui_clear(fb);" in source and
            "cmui_header(fb,\n"
            "                \"Appletini\",\n"
            "                APPLETINI_FIRMWARE_IMAGE_VERSION_FULL,\n"
            "                usb_owned);" in source and
            "cmui_footer(fb, &footer, menu->status, menu->status_warning, usb_owned);" in source and
            "cmui_nav_item(fb, &row" in source and
            "if (menu->tab != CONFIG_TAB_ABOUT) {\n"
            "        config_menu_draw_help(fb, menu, &help);\n"
            "    }" in source,
            "config menu draw path must use the modern shell, header, footer, tab rows, and lower help panel")
    require('#include "config_menu_logo_png.h"' in ui_c and
            "lodepng_decode32" in ui_c and
            "cmui_logo_draw(fb, logo_x, logo_y)" in ui_c and
            '"../../../ps_sources/frontend/config_menu_logo_png.c"' in read(VITIS_SCRIPT),
            "header must draw the embedded firmware logo from a generated PNG source")
    require('"Configuration"' not in source and
            '"Configuration"' not in ui_c and
            '"Boot and configuration"' not in source,
            "header must not draw the redundant Configuration subtitle")
    require("k_tab_labels[menu->tab]" not in page_draw and
            "content_y" not in page_draw and
            "fb16_fill_rect(fb, x, y + 38" not in page_draw,
            "selected tab pages must draw content directly without a repeated page title")
    require("CONFIG_TAB_ABOUT" in source and
            '"About"' in source and
            "static void config_menu_draw_about" in source and
            '#include "../image_versions.h"' in source and
            "APPLETINI_BOOT_IMAGE_VERSION_FULL" in source and
            "APPLETINI_FIRMWARE_IMAGE_VERSION_FULL" in source and
            '"Versions"' in source and
            source.find('"Versions"') < source.find('"Contributors"') and
            "content_h = (menu->tab == CONFIG_TAB_ABOUT) ?\n"
            "        h : (h - help_h - CONFIG_MENU_HELP_GAP);" in source and
            "static void config_menu_draw_about_bottom_text" in source and
            "config_menu_help_resolve(CONFIG_TAB_ABOUT, 0U)" in source and
            "config_menu_draw_about_bottom_text(fb, x, y, w, h);" in source and
            '"Contributors"' in source and
            "Third-party codebases" in source and
            '"CherryUSB - embedded USB stack"' in source and
            '"Xilinx/AMD Vitis standalone BSP and drivers"' in source,
            "menu must expose an About tab with versions, all credits, and bottom-page help text")
    require('#define APPLETINI_BOOT_IMAGE_VERSION_FULL' in image_versions and
            '#define APPLETINI_FIRMWARE_IMAGE_VERSION_SHORT  "F0.9.0"' in image_versions and
            '#define APPLETINI_FIRMWARE_IMAGE_VERSION_FULL   "Firmware F0.9.0"' in image_versions,
            "About/version UI must use the shared image-version definitions")
    require("#define CONFIG_MENU_HELP_H 210" in source and
            "cmui_help_panel(fb, rect, \"Help\"" in source and
            '"Profiles store complete menu configurations' in help_source and
            '"Boot settings control how long the menu prompt appears' in help_source and
            '"Appletini serves 64K auxiliary memory' in help_source and
            '"Change this only from BOOT mode' in help_source,
            "each tab must have centrally managed lower help copy")
    require('"BOOT MENU"' in ui_c and
            '"ACTIVE"' in ui_c and
            '"* ACTIVE"' not in ui_c and
            '"USB device"' in ui_c and
            '"Apple keyboard"' in ui_c and
            "const char *version" in ui_h and
            "const int version_w = (version != NULL)" in ui_c and
            "badge_y + badge_h + 36" in ui_c,
            "header must expose owner mode and draw the firmware version below it")
    require('"Tab/Del"' in ui_c and '"<>"' in ui_c and '"Enter"' in ui_c and '"Esc"' in ui_c and
            '"USB", "ACTIVE"' in ui_c and
            '"Navigate", "USB device"' in ui_c,
            "footer must draw compact Apple-keyboard hints and USB ownership state")
def test_usb_owned_menu_disables_ram_checkbox() -> None:
    header = read(CONFIG_MENU_H)
    source = read(CONFIG_MENU_C)
    device_tabs = read(REPO_ROOT / "ps_sources" / "frontend" / "config_menu_device_tabs.c")
    internal = read(REPO_ROOT / "ps_sources" / "frontend" / "config_menu_internal.h")
    ui_c = read(REPO_ROOT / "ps_sources" / "frontend" / "config_menu_ui.c")
    frontend_main = read(FRONTEND_MAIN_C)

    require("uint8_t usb_owned;" in header and
            "void config_menu_set_usb_owned(config_menu_t *menu, uint8_t usb_owned);" in header,
            "config menu state must track USB-owned menu sessions")
    require("void config_menu_set_usb_owned(config_menu_t *menu, uint8_t usb_owned)" in source and
            "menu->usb_owned = (usb_owned != 0U) ? 1U : 0U;" in source and
            "menu->usb_binding_capture = CONFIG_MENU_USB_BIND_CAPTURE_NONE;" in source,
            "USB ownership setter must publish ownership and cancel capture state")
    require("config_menu_set_usb_owned(\n"
            "        menu,\n"
            "        (uint8_t)(active != 0U && g_usb_menu_owned != 0U));" in frontend_main and
            "config_menu_set_usb_owned(\n"
            "        menu,\n"
            "        (uint8_t)(config_menu_is_active(menu) && g_usb_menu_owned != 0U));" in frontend_main,
            "frontend must synchronize config menu ownership from the USB-opened menu state")
    require("if (menu->usb_owned != 0U) {\n"
            "            config_menu_set_status(menu, 1U,\n"
            "                \"RAM CAN ONLY CHANGE FROM BOOT MENU\");\n"
            "            break;\n"
            "        }" in source,
            "RAM toggle must reject USB-owned config menu sessions")
    require("void hgr_draw_check_item_dimmed" in internal and
            "void hgr_draw_check_item_dimmed" in source and
            "cmui_check_row_ex" in ui_c,
            "menu must have a disabled checkbox drawing path")
    require("if (menu->usb_owned != 0U) {\n"
            "        hgr_draw_check_item_dimmed(fb, x, y, w, focused,\n"
            "                                   menu->ram_enabled,\n"
            "                                   \"Provide 64K aux + 8MB RamWorks\");" in device_tabs and
            "boot_menu_service_aux_card_present() != 0U" in device_tabs,
            "RAM tab must visually disable the RAM checkbox for USB-owned menus")


def main() -> int:
    tests = [
        test_profile_filesystem_contract,
        test_autosave_remains_working_config,
        test_clean_config_schema_contract,
        test_profile_load_overrides_current_without_updating_source_profile,
        test_profile_bezel_changes_update_active_profile,
        test_subfolders_and_carousel_ui,
        test_profile_carousel_consumes_tab_and_esc_locally,
        test_profile_naming_and_virtual_keyboard,
        test_profile_image_picker_and_normalization,
        test_vitis_build_registration,
        test_modern_config_menu_ui_contract,
        test_usb_owned_menu_disables_ram_checkbox,
    ]
    for test in tests:
        test()
    print(f"{len(tests)} config profile tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
