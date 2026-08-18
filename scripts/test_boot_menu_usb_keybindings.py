#!/usr/bin/env python3
"""Source checks for boot-menu USB keybinding remapping."""

import re
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
CONFIG_MENU_C = REPO_ROOT / "ps_sources" / "frontend" / "config_menu.c"
CONFIG_MENU_H = REPO_ROOT / "ps_sources" / "frontend" / "config_menu.h"
CONFIG_MENU_MAIN_TABS_C = REPO_ROOT / "ps_sources" / "frontend" / "config_menu_main_tabs.c"
FRONTEND_MAIN_C = REPO_ROOT / "ps_sources" / "frontend" / "main.c"
BOOT_MENU_SERVICE_C = REPO_ROOT / "ps_sources" / "frontend" / "boot_menu_service.c"


class TestFailure(AssertionError):
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


def test_usb_keybinding_state_and_persistence() -> None:
    header = read(CONFIG_MENU_H)
    source = read(CONFIG_MENU_C)

    require("#define CONFIG_MENU_USB_BIND_ACTION_COUNT 14U" in header and
            "usb_hid_menu_source_t usb_menu_bindings[CONFIG_MENU_USB_BIND_ACTION_COUNT];" in header and
            "uint8_t usb_binding_capture;" in header and
            "uint8_t usb_bindings_editable;" in header,
            "config menu state must carry USB binding table, capture state, and editability")
    require("CONFIG_MENU_USB_BIND_ACTION_UP = 0" in header and
            "CONFIG_MENU_USB_BIND_ACTION_TAB_UP" in header and
            "CONFIG_MENU_USB_BIND_ACTION_TAB_DOWN" in header and
            "CONFIG_MENU_USB_BIND_ACTION_OK" in header and
            "CONFIG_MENU_USB_BIND_ACTION_BACK" in header and
            "CONFIG_MENU_USB_BIND_ACTION_SCREENSHOT_A2" in header and
            "CONFIG_MENU_USB_BIND_ACTION_SCREENSHOT_1080P" in header and
            "CONFIG_MENU_USB_BIND_ACTION_EXIT" not in header,
            "USB binding actions must cover navigation, OK, Back, screenshots, and no selectable Exit")
    require(config_version(source) >= 100 and
            '"usb.menu.bind.up"' in source and
            '"usb.menu.bind.ok"' in source and
            '"usb.menu.bind.back"' in source and
            '"usb.menu.bind.screenshot.a2"' in source and
            '"usb.menu.bind.screenshot.1080p"' in source and
            '"usb_bind_exit"' not in source and
            '"usb_bind_up"' not in source and
            "config_menu_parse_usb_binding(menu, key, value)" in source,
            "USB bindings must persist in the clean dot-notation config schema")
    require("config_menu_usb_bindings_set_defaults(menu);" in source,
            "USB bindings must initialize defaults before parsing")


def test_default_usb_mapping_preserves_existing_behavior() -> None:
    source = read(CONFIG_MENU_C)

    require("static const usb_hid_menu_source_t k_usb_binding_defaults[CONFIG_MENU_USB_BIND_ACTION_COUNT]" in source,
            "default USB binding table must be explicit")
    require("USB_HID_MENU_ACTION_ITEM_UP,\n"
            "    USB_HID_MENU_ACTION_ITEM_DOWN,\n"
            "    USB_HID_MENU_ACTION_LEFT,\n"
            "    USB_HID_MENU_ACTION_RIGHT," in source,
            "default USB bindings must preserve existing up/down/left/right behavior")
    require("USB_HID_MENU_ACTION_PREV_TAB,\n"
            "    USB_HID_MENU_ACTION_NEXT_TAB," in source,
            "default wheel bindings must preserve tab-up/tab-down behavior")
    require("USB_HID_MENU_ACTION_SELECT" in source and
            "UI_KEY_ENTER" in source and
            "UI_KEY_BACK" in source,
            "OK must translate to enter and Back must translate to the Back UI key")
    require("#define CONFIG_USB_KEY_USAGE_ESCAPE 0x29U" in source and
            "CONFIG_USB_KEY_SOURCE(CONFIG_USB_KEY_USAGE_ESCAPE)" in source,
            "Back must default to the USB keyboard Escape key")
    require("#define CONFIG_USB_KEY_USAGE_F12 0x45U" in source and
            "#define CONFIG_USB_KEY_USAGE_F13 0x68U" in source and
            "#define CONFIG_USB_KEY_USAGE_PRINTSCN 0x46U" in source and
            "CONFIG_USB_KEY_SOURCE(CONFIG_USB_KEY_USAGE_F12)" in source and
            "CONFIG_USB_KEY_SOURCE(CONFIG_USB_KEY_USAGE_PRINTSCN)" in source and
            "usage >= CONFIG_USB_KEY_USAGE_F1 &&\n        usage <= CONFIG_USB_KEY_USAGE_F12" in source and
            "usage >= CONFIG_USB_KEY_USAGE_F13 &&\n        usage <= CONFIG_USB_KEY_USAGE_F24" in source and
            "UI_KEY_NONE,\n    UI_KEY_NONE" in source,
            "screenshot bindings must default to F12/Print Screen and not translate into menu navigation keys")
    require("if (i != action && menu->usb_menu_bindings[i] == source)" in source,
            "capturing a source must clear duplicate bindings")
    require("config_menu_usb_binding_action_is_button(action) != 0U" in source and
            '"%s REQUIRES USB INPUT"' in source and
            "usb_hid_menu_source_is_keyboard(source)" in source,
            "OK/Back capture must accept keyboard/keypad sources and reject only invalid sources")


def test_boot_settings_draws_binding_editor() -> None:
    header = read(CONFIG_MENU_H)
    tabs = read(CONFIG_MENU_MAIN_TABS_C)
    source = read(CONFIG_MENU_C)

    require("CONFIG_MENU_BOOT_ONEE_ITEM 2U" in header and
            "CONFIG_MENU_BOOT_USB_BIND_RESET_ITEM 3U" in header and
            "CONFIG_MENU_BOOT_USB_BIND_FIRST_ITEM (CONFIG_MENU_BOOT_USB_BIND_RESET_ITEM + 1U)" in header and
            "CONFIG_MENU_BOOT_ITEM_COUNT" in header,
            "Boot Settings must reserve rows for the USB binding editor")
    require("case CONFIG_TAB_BOOT_SETTINGS:" in source and
            "return CONFIG_MENU_BOOT_ITEM_COUNT;" in source,
            "normal Boot Settings must include the editable USB binding rows")
    require('"USB MENU BINDINGS"' in tabs and
            "usb_binding_draw_label(action)" in tabs and
            "config_menu_usb_binding_source_text(menu->usb_menu_bindings[action])" in tabs and
            '"RESET USB BINDINGS"' in tabs and
            '"Long %s"' in tabs and
            "config_menu_usb_open_close_binding_source(menu)" in tabs,
            "Boot Settings tab must draw reset, USB binding columns, and derived long open/close Menu row")
    require('"%-*s: %s"' in tabs and
            "const uint32_t left_label_w = 5U;" in tabs and
            "const uint32_t middle_label_w = 8U;" in tabs and
            "const uint32_t right_label_w = 12U;" in tabs and
            '"UP"' in tabs and
            '"DOWN"' in tabs and
            '"BACK"' in tabs and
            '"TAB DOWN"' in tabs and
            '"PRTSCR A2"' in tabs and
            '"PRTSCR 1080P"' in tabs and
            '"PRTSCR A2"' in source and
            '"PRTSCR 1080P"' in source and
            '"SHOT A2"' not in tabs and
            '"SHOT 1080"' not in tabs,
            "USB binding rows must use fixed-width uppercase labels before the colon")
    require("static const uint8_t middle_actions[] = {\n"
            "        CONFIG_MENU_USB_BIND_ACTION_TAB_UP,\n"
            "        CONFIG_MENU_USB_BIND_ACTION_TAB_DOWN,\n"
            "        CONFIG_MENU_USB_BIND_ACTION_OK,\n"
            "        CONFIG_MENU_USB_BIND_ACTION_BACK,\n"
            "    };" in tabs and
            "static const uint8_t right_actions[] = {\n"
            "        CONFIG_MENU_USB_BIND_ACTION_SCREENSHOT_A2,\n"
            "        CONFIG_MENU_USB_BIND_ACTION_SCREENSHOT_1080P,\n"
            "        CONFIG_MENU_USB_BIND_ACTION_VTW_SPEED_TOGGLE,\n"
            "        CONFIG_MENU_USB_BIND_ACTION_VTW_SPEED_UP,\n"
            "        CONFIG_MENU_USB_BIND_ACTION_VTW_SPEED_DOWN,\n"
            "        CONFIG_MENU_USB_BIND_ACTION_VTW_SLUG_TOGGLE\n"
            "    };" in tabs and
            "((int)i + right_row_offset + spacer) * row_h" in tabs and
            "config_menu_boot_usb_binding_item_for_action(action)" in tabs,
            "visible binding order must put OK/Back in the middle and PrtScr rows below Menu")
    require("const int row_h = CMUI_ROW_H + CMUI_ROW_GAP;" in tabs and
            "const int heading_y = y + (row_h * ((onee_fixed != 0U) ? 4 : 3));" in tabs and
            "CMUI_COLOR_ACCENT" in tabs and
            "CMUI_COLOR_BORDER_SOFT" in tabs,
            "USB binding heading must leave the active ONE//e standard row clear")
    require("hgr_draw_item_dimmed(fb,\n"
            "                             x,\n"
            "                             reset_y" in tabs and
            "hgr_draw_usb_binding_item(" in tabs,
            "inactive USB binding rows must render grey")
    require("hgr_draw_usb_binding_item(\n"
            "            fb,\n"
            "            x + (column_w + column_gap) * 2,\n"
            "            binding_y,\n"
            "            column_w,\n"
            "            0U,\n"
            "            1U,\n"
            "            \"MENU\"" in tabs,
            "normal Menu open/close must render first in the right column and stay grey because it is derived from the open/close source")
    require("static const uint8_t k_boot_usb_binding_action_order[CONFIG_MENU_USB_BIND_ACTION_COUNT]" in source and
            "CONFIG_MENU_USB_BIND_ACTION_OK,\n"
            "    CONFIG_MENU_USB_BIND_ACTION_BACK,\n"
            "    CONFIG_MENU_USB_BIND_ACTION_SCREENSHOT_A2,\n"
            "    CONFIG_MENU_USB_BIND_ACTION_SCREENSHOT_1080P" in source and
            "uint32_t config_menu_boot_usb_binding_action_for_item(uint32_t item_focus)" in source and
            "uint32_t config_menu_boot_usb_binding_item_for_action(uint32_t action)" in source and
            "config_menu_boot_usb_binding_action_for_item(menu->item_focus)" in source,
            "Boot Settings focus order must follow the visible USB binding table")
    require("menu->usb_binding_capture = (uint8_t)action;" in source and
            '"PRESS USB INPUT FOR %s"' in source,
            "entering a binding row must start USB input capture")
    require('"MENU"' in tabs and
            "(uint8_t)(menu->item_focus ==" not in tabs[tabs.find('"MENU"') - 160:tabs.find('"MENU"') + 80],
            "Menu open/close row must be visible but not focusable")


def test_only_usb_menu_events_are_remapped() -> None:
    frontend_main = read(FRONTEND_MAIN_C)
    boot_menu = read(BOOT_MENU_SERVICE_C)
    config_menu = read(CONFIG_MENU_C)

    require("return config_menu_translate_usb_binding(menu, source);" in frontend_main,
            "USB menu sources must translate through the binding table")
    require("config_menu_usb_binding_capture_action(menu) !=\n"
            "        CONFIG_MENU_USB_BIND_CAPTURE_NONE" in frontend_main and
            "config_menu_capture_usb_binding(menu, event->source)" in frontend_main,
            "USB source events must feed capture mode before normal menu routing")
    require("ui_handle_input_with_config(&ui, &config_menu, boot_event.input);" in frontend_main,
            "Apple keyboard boot-menu events must remain direct and static")
    require("config_menu_translate_usb_binding" not in boot_menu and
            "usb_menu_bindings" not in boot_menu and
            "usb_binding_capture" not in boot_menu,
            "Apple keyboard mapping service must not depend on USB keybindings")
    require("case 0x1B:\n        input->key = UI_KEY_BACK;" in boot_menu and
            "case 0x7F:\n        input->key = UI_KEY_SHIFT_TAB;" in boot_menu and
            "case 0x7F:\n        input->key = UI_KEY_BACK;" not in boot_menu,
            "Apple keyboard ESC must stay Back while DEL reverse-tabs")
    require("boot_menu_service_machine_mode() != CARD_MACHINE_MODE_IIPLUS" in boot_menu and
            "case 'O':\n    case 'o':\n        input->key = UI_KEY_PAGE_UP;" in boot_menu and
            "case 'L':\n    case 'l':\n        input->key = UI_KEY_PAGE_DOWN;" in boot_menu and
            "case 'Q':\n    case 'q':\n        input->key = UI_KEY_SHIFT_TAB;" in boot_menu and
            "case 'A':\n    case 'a':\n        input->key = UI_KEY_TAB;" in boot_menu and
            "case 'I':" not in boot_menu and "case 'K':" not in boot_menu,
            "II/II+ boot menus must use O/L for items, Q for tab up, and A for tab down")
    require("case UI_KEY_SHIFT_TAB:\n        config_menu_prev_tab(menu);" in config_menu and
            "case UI_KEY_TAB:\n        config_menu_next_tab(menu);" in config_menu,
            "Q must move to the previous/up tab and A to the next/down tab")
    require("input->ascii = ascii;" in boot_menu,
            "II/II+ navigation aliases must remain typeable in text editors")


def test_binding_grid_navigation_requires_ok_for_edits() -> None:
    source = read(CONFIG_MENU_C)
    help_source = read(REPO_ROOT / "ps_sources" / "frontend" / "config_menu_help.c")
    adjust_start = source.find("static uint8_t config_menu_adjust_focused_value")
    adjust_end = source.find("static void config_menu_reload_smartport_device", adjust_start)
    adjust = source[adjust_start:adjust_end]

    require("k_boot_usb_binding_column_first[] = { 0U, 4U, 8U }" in source and
            "k_boot_usb_binding_column_row_first[] = { 0U, 0U, 1U }" in source and
            "k_boot_usb_binding_column_count[] = { 4U, 4U, 6U }" in source,
            "USB binding navigation must model the three visible columns")
    require("config_menu_boot_usb_binding_move_horizontal(menu, -1)" in source and
            "config_menu_boot_usb_binding_move_horizontal(menu, 1)" in source and
            "config_menu_boot_usb_binding_move_vertical(menu, -1)" in source and
            "config_menu_boot_usb_binding_move_vertical(menu, 1)" in source,
            "arrows must navigate the binding grid by row and column")
    require("config_menu_usb_binding_next_source" not in source and
            "config_menu_assign_usb_binding" not in adjust,
            "Left/Right must never cycle or assign a USB binding")
    require("menu->usb_binding_capture = (uint8_t)action;" in source and
            '"PRESS USB INPUT FOR %s"' in source,
            "Enter/OK must be the only binding-row action that starts capture")
    require("Up/Down move within a column, Left/Right move columns, and Enter captures" in help_source,
            "Boot Settings help must describe binding-grid navigation")


def test_usb_bindings_edit_only_from_apple_boot_menu() -> None:
    header = read(CONFIG_MENU_H)
    source = read(CONFIG_MENU_C)
    frontend_main = read(FRONTEND_MAIN_C)

    require("void config_menu_set_usb_bindings_editable(config_menu_t *menu, uint8_t editable);" in header,
            "config menu must expose an editability switch for USB bindings")
    require("if (menu->usb_bindings_editable == 0U) {\n"
            "                config_menu_set_status(menu, 1U, \"USB BINDINGS EDITABLE AT BOOT\");" in source,
            "starting capture on inactive USB binding rows must be blocked")
    require("if (menu->usb_bindings_editable == 0U) {\n"
            "        menu->usb_binding_capture = CONFIG_MENU_USB_BIND_CAPTURE_NONE;" in source,
            "capturing inactive USB binding rows must be blocked defensively")
    require("config_menu_set_usb_bindings_editable(\n"
            "        menu,\n"
            "        (uint8_t)(active != 0U && g_usb_menu_owned == 0U));" in frontend_main,
            "menu open/close must enable USB remapping only for Apple-keyboard-owned boot menus")
    require("case BOOT_MENU_EVENT_OPEN:\n"
            "                    g_usb_menu_owned = 0U;" in frontend_main and
            "case USB_HID_MENU_ACTION_OPEN:\n"
            "        if (!config_menu_is_active(menu)) {\n"
            "            g_usb_menu_owned = 1U;" in frontend_main,
            "Apple boot-menu opens and USB long-press opens must carry distinct ownership")


def test_onee_draws_fixed_controls_and_editable_bindings() -> None:
    header = read(REPO_ROOT / "ps_sources" / "frontend" / "config_menu_internal.h")
    source = read(CONFIG_MENU_C)
    tabs = read(CONFIG_MENU_MAIN_TABS_C)
    help_source = read(REPO_ROOT / "ps_sources" / "frontend" / "config_menu_help.c")

    frontend_main = read(FRONTEND_MAIN_C)
    fixed_mode = read(REPO_ROOT / "ps_sources" / "frontend" / "onee_fixed_mode.h")
    require("uint8_t config_menu_onee_fixed_bindings_active(" in header and
            "return onee_usb_fixed_mode_active(menu->onee_mode_status);" in source and
            "onee_usb_fixed_mode_active(onee_service_status())" in frontend_main and
            "CARD_CTRL_ONEE_STATUS_REQUEST_BIT" in fixed_mode and
            "CARD_CTRL_ONEE_STATUS_EFFECTIVE_BIT" in fixed_mode and
            "CARD_CTRL_ONEE_STATUS_SELECTED_BIT" not in
            fixed_mode[fixed_mode.find("onee_usb_fixed_mode_active"):],
            "the fixed guide and live routing must share REQUEST/EFFECTIVE selection")
    require('const char *heading_text = "USB MENU BINDINGS";' in tabs and
            '"RESET: Ctrl+Alt+Del"' in tabs and
            '"OPEN APPLE: Left Alt"' in tabs and
            '"CLOSED APPLE: Right Alt"' in tabs and
            '"MENU: Pause/Break + Long OK"' in tabs and
            "const int fixed_menu_y = reset_y + row_h;" in tabs and
            "fixed_menu_y,\n"
            "                             w," in tabs and
            "const int right_row_offset = (onee_fixed != 0U) ? 0 : 1;" in tabs,
            "ONE//e must show its four fixed controls as read-only")
    require("onee_fixed_binding_value" not in tabs and
            "config_menu_usb_binding_source_text(menu->usb_menu_bindings[action])" in tabs and
            tabs.count("if (menu->usb_bindings_editable != 0U)") == 4,
            "ONE//e must draw saved values and keep every binding column editable")
    require("case CONFIG_TAB_BOOT_SETTINGS:\n"
            "        return CONFIG_MENU_BOOT_ITEM_COUNT;" in source and
            "config_menu_onee_fixed_bindings_active(menu) == 0U &&\n"
            "        menu->item_focus >= CONFIG_MENU_BOOT_USB_BIND_FIRST_ITEM" not in source and
            "menu->item_focus > CONFIG_MENU_BOOT_ONEE_STANDARD_ITEM" not in source,
            "ONE//e navigation must include the full binding grid")
    require('"ONE//e USB CONTROLS ARE FIXED"' not in source and
            "menu->usb_bindings_editable = (editable != 0U) ? 1U : 0U;" in source and
            "config_menu_capture_usb_binding(menu, event->source)" in frontend_main and
            "g_usb_menu_owned == 0U || ui_onee_selected() != 0U" in frontend_main,
            "ONE//e must allow binding capture even when USB opened the menu")
    require("ONE//e fixes Reset, Open Apple, Closed Apple, and Pause/Break. Other USB bindings stay editable." in
            help_source and
            "Pause/Break and a long press of the saved OK binding both open or close the ONE//e menu." in
            help_source,
            "Boot Settings help must explain the fixed controls and both Menu routes")


def test_open_close_is_long_press_of_open_close_source() -> None:
    hid_service = read(REPO_ROOT / "ps_sources" / "frontend" / "usb_hid_service.c")
    hid_header = read(REPO_ROOT / "ps_sources" / "frontend" / "usb_hid_service.h")
    frontend_main = read(FRONTEND_MAIN_C)
    source = read(CONFIG_MENU_C)

    require("void usb_hid_service_set_menu_ok_source(usb_hid_menu_source_t source);" in hid_header and
            "void usb_hid_service_set_menu_open_close_source(usb_hid_menu_source_t source);" in hid_header,
            "USB service must expose separate OK and open/close sources")
    require("usb_hid_service_set_menu_ok_source(config_menu_usb_ok_binding_source(menu));" in frontend_main,
            "frontend must publish the configured OK source to the USB service")
    require("usb_hid_service_set_menu_open_close_source(\n"
            "        config_menu_usb_open_close_binding_source(menu));" in frontend_main,
            "frontend must publish the configured open/close source separately")
    require("static usb_hid_menu_source_t g_menu_open_close_source = USB_HID_MENU_ACTION_SELECT;" in hid_service and
            "mouse_menu_push_action((g_menu_capture == 0U) ?\n"
            "            USB_HID_MENU_ACTION_OPEN : USB_HID_MENU_ACTION_CLOSE);" in hid_service,
            "long press of the open/close source must open when closed and close when open")
    require("source == g_menu_open_close_source" in hid_service and
            "menu_start_open_close_hold(slot, source);" in hid_service and
            "menu_poll_open_close_hold(slot);" in hid_service,
            "mouse and keyboard sources must use the dedicated open/close hold tracker")
    require("mouse_menu_finish_ok_hold(slot);" in hid_service and
            "keyboard_menu_push_source(slot->ok_down_source);" in hid_service,
            "short OK release must remain separate from long open/close")
    require("usb_hid_menu_source_t config_menu_usb_ok_binding_source(const config_menu_t *menu)" in source and
            "usb_hid_menu_source_t config_menu_usb_open_close_binding_source(const config_menu_t *menu)" in source,
            "config menu must expose distinct OK and open/close source contracts")


def test_usb_keyboard_uses_binding_sources_only() -> None:
    hid_service = read(REPO_ROOT / "ps_sources" / "frontend" / "usb_hid_service.c")
    hid_header = read(REPO_ROOT / "ps_sources" / "frontend" / "usb_hid_service.h")
    frontend_main = read(FRONTEND_MAIN_C)
    source = read(CONFIG_MENU_C)

    require("USB_HID_MENU_SOURCE_KEY_BASE" in hid_header and
            "usb_hid_menu_source_from_keyboard_usage" in hid_header and
            "usb_hid_menu_source_text" in hid_header,
            "USB HID API must expose typed keyboard/keypad source IDs")
    require("hid_keyboard_action_for_key" not in hid_service and
            "keyboard_menu_push_source(next_sources[i]);" in hid_service and
            "HID_KBD_USAGE_KPD1" in hid_service and
            "HID_KBD_USAGE_F13" in hid_service,
            "USB keyboard/keypad reports must become physical sources, not hardcoded menu actions")
    require("event->action" in frontend_main and
            "config_menu_translate_usb_binding(menu, source)" in frontend_main and
            "config_menu_capture_usb_binding(menu, event->source)" in frontend_main,
            "frontend must route keyboard/keypad source events only through the USB binding table")
    require("usb_hid_menu_source_is_keyboard(source) != 0U" in source and
            "config_menu_usb_binding_source_clamp_for_action(\n"
            "    uint32_t action," in source,
            "config menu must persist and validate keyboard/keypad source IDs")


def test_screenshot_usb_shortcuts_are_global_keyboard_bindings() -> None:
    header = read(CONFIG_MENU_H)
    source = read(CONFIG_MENU_C)
    hid_header = read(REPO_ROOT / "ps_sources" / "frontend" / "usb_hid_service.h")
    hid_service = read(REPO_ROOT / "ps_sources" / "frontend" / "usb_hid_service.c")
    frontend_main = read(FRONTEND_MAIN_C)

    require("usb_hid_menu_source_t config_menu_usb_screenshot_a2_binding_source(" in header and
            "usb_hid_menu_source_t config_menu_usb_screenshot_1080p_binding_source(" in header and
            "config_menu_usb_binding_source_clamp_for_action(\n"
            "        CONFIG_MENU_USB_BIND_ACTION_SCREENSHOT_A2" in source and
            "config_menu_usb_binding_source_clamp_for_action(\n"
            "        CONFIG_MENU_USB_BIND_ACTION_SCREENSHOT_1080P" in source,
            "config menu must expose clamped screenshot binding sources")
    require("config_menu_usb_binding_action_is_screenshot(action)" in source and
            '"SCREENSHOT REQUIRES USB KEY"' in source and
            "usb_hid_menu_source_is_keyboard(source)" in source,
            "screenshot bindings must remain keyboard-only")
    require("USB_HID_MENU_ACTION_SCREENSHOT_A2" in hid_header and
            "USB_HID_MENU_ACTION_SCREENSHOT_1080P" in hid_header and
            "void usb_hid_service_set_screenshot_sources(usb_hid_menu_source_t a2_source," in hid_header,
            "USB HID service must expose screenshot shortcut actions and source configuration")
    require("static usb_hid_menu_source_t g_screenshot_a2_source = USB_HID_MENU_SOURCE_NONE;" in hid_service and
            "screenshot_push_source(next_sources[i]) != 0U" in hid_service and
            "mouse_menu_push_event(USB_HID_MENU_ACTION_SCREENSHOT_A2, source);" in hid_service and
            "mouse_menu_push_event(USB_HID_MENU_ACTION_SCREENSHOT_1080P, source);" in hid_service and
            "source == USB_HID_MENU_SOURCE_NONE ||\n"
            "            usb_hid_menu_source_is_keyboard(source) != 0U" in hid_service,
            "HID keyboard edges must emit global screenshot actions from configured keyboard-only sources")
    require("usb_hid_service_set_screenshot_sources(\n"
            "        config_menu_usb_screenshot_a2_binding_source(menu),\n"
            "        config_menu_usb_screenshot_1080p_binding_source(menu));" in frontend_main and
            "case USB_HID_MENU_ACTION_SCREENSHOT_A2:\n"
            "        ui_save_screenshot(SCREENSHOT_SERVICE_KIND_A2);" in frontend_main and
            "case USB_HID_MENU_ACTION_SCREENSHOT_1080P:\n"
            "        ui_save_screenshot(SCREENSHOT_SERVICE_KIND_1080P);" in frontend_main,
            "frontend must publish screenshot bindings and dispatch shortcut events")


TESTS = [
    test_usb_keybinding_state_and_persistence,
    test_default_usb_mapping_preserves_existing_behavior,
    test_boot_settings_draws_binding_editor,
    test_only_usb_menu_events_are_remapped,
    test_binding_grid_navigation_requires_ok_for_edits,
    test_usb_bindings_edit_only_from_apple_boot_menu,
    test_onee_draws_fixed_controls_and_editable_bindings,
    test_open_close_is_long_press_of_open_close_source,
    test_usb_keyboard_uses_binding_sources_only,
    test_screenshot_usb_shortcuts_are_global_keyboard_bindings,
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
        print(f"{len(TESTS) - len(failures)} of {len(TESTS)} USB keybinding tests passed; "
              f"{len(failures)} failed")
        return 1
    print(f"{len(TESTS)} USB keybinding tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
