#!/usr/bin/env python3
"""Source-level regression tests for config-menu profiles.

These tests run without Vitis or hardware:

    python scripts/test_config_profiles.py
"""

from __future__ import annotations

from pathlib import Path
import re


REPO_ROOT = Path(__file__).resolve().parents[1]
FRONTEND = REPO_ROOT / "ps_sources" / "frontend"
CONFIG_MENU_C = FRONTEND / "config_menu.c"
CONFIG_MENU_H = FRONTEND / "config_menu.h"
CONFIG_MENU_HELP_C = FRONTEND / "config_menu_help.c"
CONFIG_MENU_INTERNAL_H = FRONTEND / "config_menu_internal.h"
CONFIG_MENU_DEVICE_TABS_C = FRONTEND / "config_menu_device_tabs.c"
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

    require("#define APPLETINI_CFG_VERSION 115U" in source,
            "the global ONE//e persistence key must advance the config schema")
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


def test_profiles_contextual_help_for_every_action() -> None:
    profiles = read(CONFIG_MENU_PROFILES_C)
    help_source = read(CONFIG_MENU_HELP_C)
    expected = [
        (0, "Choose profile", "profiles_choose"),
        (1, "Save to current profile", "profiles_save_current"),
        (2, "Save As", "profiles_save_as"),
        (3, "Rename profile", "profiles_rename"),
        (4, "Set image", "profiles_set_image"),
    ]

    for item, label, block in expected:
        require(f"menu->item_focus == {item}U" in profiles and
                f'"{label}"' in profiles,
                f"Profiles item {item} must retain its expected label/focus mapping")
        require(f"HELP({block}," in help_source and
                f"OVERRIDE({item}U, {block})" in help_source,
                f"Profiles item {item} must have its own contextual help block")

    require("TAB_WITH_OVERRIDES(CONFIG_TAB_PROFILES, profiles, profiles_overrides)" in help_source,
            "Profiles tab must resolve focused-item help instead of always showing its default block")


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
    require("CONFIG_BROWSER_PROFILE_IMAGE_VISIBLE_ROWS 17U" in source and
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
    boot_version = re.search(
        r'^\s*#define\s+APPLETINI_BOOT_IMAGE_VERSION_FULL\s+"([^"]+)"',
        image_versions,
        re.MULTILINE)
    firmware_short = re.search(
        r'^\s*#define\s+APPLETINI_FIRMWARE_IMAGE_VERSION_SHORT\s+"([^"]+)"',
        image_versions,
        re.MULTILINE)
    firmware_full = re.search(
        r'^\s*#define\s+APPLETINI_FIRMWARE_IMAGE_VERSION_FULL\s+"([^"]+)"',
        image_versions,
        re.MULTILINE)

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
            "cmui_footer(fb,\n"
            "                &footer,\n"
            "                menu->status,\n"
            "                menu->status_warning,\n"
            "                usb_owned," in source and
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
    require(boot_version is not None and
            firmware_short is not None and
            firmware_full is not None and
            firmware_full.group(1) == f"Firmware {firmware_short.group(1)}",
            "About/version UI must use the shared image-version definitions")
    require("#define CONFIG_MENU_HELP_H 210" in source and
            "cmui_help_panel(fb, rect, \"Help\"" in source and
            '"Profiles store complete menu configurations' in help_source and
            '"The wait sets how long the \'A\' prompt shows before boot.' in help_source and
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
    require('"Tab/Del", "Up/Down"' in ui_c and
            '"Q/A", "O/L"' in ui_c and
            '"<>"' in ui_c and '"Enter"' in ui_c and '"Esc"' in ui_c and
            '"USB", "ACTIVE"' in ui_c and
            '"Navigate", "USB device"' in ui_c,
            "footer must draw machine-specific Apple-keyboard hints and USB ownership state")
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


def test_transwarp_slot_slowdown_rows() -> None:
    header = read(CONFIG_MENU_H)
    source = read(CONFIG_MENU_C)
    internal = read(CONFIG_MENU_INTERNAL_H)
    device_tabs = read(CONFIG_MENU_DEVICE_TABS_C)
    help_source = read(CONFIG_MENU_HELP_C)

    require("#define CONFIG_TRANSWARP_ITEM_IGNORE_C074  2U" in internal and
            "#define CONFIG_TRANSWARP_ITEM_DISABLE_D2   3U" in internal and
            "#define CONFIG_TRANSWARP_ITEM_FLOATBUS     4U" in internal and
            "#define CONFIG_TRANSWARP_ITEM_PADDLE       5U" in internal and
            "#define CONFIG_TRANSWARP_ITEM_SLUG         6U" in internal and
            "#define CONFIG_TRANSWARP_ITEM_SLOT_FIRST   7U" in internal and
            "#define CONFIG_TRANSWARP_SLOT_COUNT        7U" in internal and
            "#define CONFIG_TRANSWARP_ITEM_COUNT" in internal,
            "TransWarp focus order must match the compact slowdown layout")
    require('"Ignore $C074 Speed Switch"' in device_tabs and
            '"Disable DiskII Acceleration"' in device_tabs and
            "y + (2 * row_h), option_w" in device_tabs and
            "option_x2, y + (2 * row_h), option_w2" in device_tabs,
            "TransWarp compatibility checkboxes must share one row")
    require('strcmp(key, "vtw.c074.ignore") == 0' in source and
            'strcmp(key, "vtw.disk2.acceleration.disabled") == 0' in source and
            '"vtw.c074.ignore=%s\\n"' in source and
            '"vtw.disk2.acceleration.disabled=%s\\n"' in source,
            "TransWarp compatibility checkboxes must persist")
    require('"Slow Floating bus ($C019,$C030-$C05F)"' in device_tabs and
            '"Slow Paddles/joystick ($C064-$C070)"' in device_tabs and
            "y + (3 * row_h), option_w" in device_tabs and
            "option_x2, y + (3 * row_h), option_w2" in device_tabs and
            '"Enable 0.05 MHz slug debug key"' in device_tabs and
            "y + (4 * row_h), w" in device_tabs,
            "floating-bus and paddle slowdown must share the row above slug")
    require('"Slow down physical slots:"' in device_tabs and
            "for (uint8_t slot = 1U; slot <= CONFIG_TRANSWARP_SLOT_COUNT; ++slot)" in
            device_tabs and
            'snprintf(slot_label, sizeof(slot_label), "%u"' in device_tabs and
            "fb, slot_x, y + (6 * row_h), slot_w" in device_tabs,
            "TransWarp tab must show slots 1-7 as checkbox cells under one heading")
    require('"Slowdown window:", win_val' in device_tabs and
            "hgr_draw_value_item(fb, x, y + (7 * row_h), w" in device_tabs,
            "slowdown window must follow the compact physical-slot row")
    adjust_start = source.find("static uint8_t config_menu_adjust_focused_value")
    adjust_end = source.find("static int config_menu_reload_smartport_device", adjust_start)
    activate_start = source.find("static void config_menu_activate_item")
    activate_end = source.find("uint8_t config_menu_handle_input", activate_start)
    require(adjust_start >= 0 and adjust_end > adjust_start and
            activate_start >= 0 and activate_end > activate_start,
            "config menu adjustment and activation handlers must be present")
    adjust = source[adjust_start:adjust_end]
    activate = source[activate_start:activate_end]
    require("static uint8_t config_menu_vtw_toggle_focused_slot" in source and
            "menu->item_focus -\n"
            "                     CONFIG_TRANSWARP_ITEM_SLOT_FIRST + 1U" in source and
            "config_menu_vtw_toggle_focused_slot(menu)" not in adjust and
            "(void)config_menu_vtw_toggle_focused_slot(menu);" in activate,
            "only OK may toggle the focused TransWarp slot checkbox")
    require("vtw_slowdown_slot_cursor" not in header + source + device_tabs,
            "TransWarp slowdown UI must not retain the hidden slot selector")
    require("OVERRIDE(2, transwarp_ignore_c074)" in help_source and
            "OVERRIDE(3, transwarp_disable_disk2_accel)" in help_source and
            "OVERRIDE(4, transwarp_slowdown_floatbus)" in help_source and
            "OVERRIDE(5, transwarp_slowdown_paddle)" in help_source and
            "OVERRIDE(6, transwarp_slug)" in help_source and
            "OVERRIDE(14, transwarp_slowdown_window)" in help_source,
            "TransWarp non-slot slowdown rows must retain contextual help")
    for item in range(7, 14):
        require(f"OVERRIDE({item}, transwarp_slowdown_slots)" in help_source,
                f"TransWarp slot slowdown row {item} must retain contextual help")
    require('"Slow Floating bus ($C019,$C030-$C05F)"' in device_tabs and
            "CONFIG_TRANSWARP_ITEM_SPEAKER" not in internal + source + device_tabs and
            "CONFIG_TRANSWARP_ITEM_VIDEO" not in internal + source + device_tabs,
            "Speaker and Video rows must be replaced by one floating-bus row")
    require("config_menu_migrate_vtw_slowdown_mask" in source and
            "VTW_SLOW_LEGACY_VIDEO_BIT" in source and
            "menu->vtw_slowdown_mask |= VTW_SLOW_FLOATBUS_BIT;" in source and
            "menu->vtw_slowdown_mask &= VTW_SLOW_VALID_MASK;" in source,
            "config 104 Speaker/Video selections must migrate to Floating-bus I/O")


def test_checkbox_rows_ignore_left_right() -> None:
    source = read(CONFIG_MENU_C)
    adjust_start = source.find("static uint8_t config_menu_adjust_focused_value")
    adjust_end = source.find("static int config_menu_reload_smartport_device", adjust_start)

    require(adjust_start >= 0 and adjust_end > adjust_start,
            "config menu focused-value adjustment handler must be present")
    adjust = source[adjust_start:adjust_end]
    forbidden_checkbox_mutations = (
        "config_menu_vtw_toggle_focused_slot(menu)",
        "menu->vtw_ignore_c074 =",
        "menu->vtw_disable_disk2_accel =",
        "menu->border_enabled =",
        "menu->show_bezel =",
        "menu->show_debugging =",
        "menu->disk2_activity_visible =",
        "config_menu_ethernet_toggle_slot(menu)",
        "config_menu_ethernet_toggle_saved_config(menu)",
    )
    for mutation in forbidden_checkbox_mutations:
        require(mutation not in adjust,
                f"left/right adjustment must not modify checkbox state: {mutation}")


def main() -> int:
    tests = [
        test_profile_filesystem_contract,
        test_autosave_remains_working_config,
        test_clean_config_schema_contract,
        test_profile_load_overrides_current_without_updating_source_profile,
        test_profile_bezel_changes_update_active_profile,
        test_subfolders_and_carousel_ui,
        test_profiles_contextual_help_for_every_action,
        test_profile_carousel_consumes_tab_and_esc_locally,
        test_profile_naming_and_virtual_keyboard,
        test_profile_image_picker_and_normalization,
        test_vitis_build_registration,
        test_modern_config_menu_ui_contract,
        test_usb_owned_menu_disables_ram_checkbox,
        test_transwarp_slot_slowdown_rows,
        test_checkbox_rows_ignore_left_right,
    ]
    for test in tests:
        test()
    print(f"{len(tests)} config profile tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
