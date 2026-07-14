#!/usr/bin/env python3
"""Source-level regression tests for frontend storage activity UI."""

from __future__ import annotations

from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
FRONTEND_MAIN_C = REPO_ROOT / "ps_sources" / "frontend" / "main.c"
CONFIG_MENU_C = REPO_ROOT / "ps_sources" / "frontend" / "config_menu.c"
CONFIG_MENU_H = REPO_ROOT / "ps_sources" / "frontend" / "config_menu.h"
SMARTPORT_SERVICE_C = REPO_ROOT / "ps_sources" / "frontend" / "smartport_service.c"
SMARTPORT_SERVICE_H = REPO_ROOT / "ps_sources" / "frontend" / "smartport_service.h"


class TestFailure(Exception):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise TestFailure(message)


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def test_storage_activity_tracks_disk2_without_hiding_smartport() -> None:
    source = read(FRONTEND_MAIN_C)

    require("CARD_CTRL_SLOT_DISK2" in source,
            "frontend must name the Disk II slot bit")
    require("control_get_slot_enabled(NULL, CARD_CTRL_SLOT_DISK2) != 0U &&\n"
            "        disk2_service_get_activity(&disk2_activity) == 0" in source,
            "Disk II activity polling must remain gated by Slot 6")
    require("smartport_service_get_activity(&smartport_activity) == 0" in source,
            "SmartPort activity must still be polled for the storage overlay")
    require("if (disk2_valid == 0U && smartport_valid == 0U) {\n"
            "        memset(&g_storage_activity, 0, sizeof(g_storage_activity));" in source,
            "storage activity state should reset only when no storage source is available")


def test_disk_activity_visibility_is_configurable() -> None:
    source = read(FRONTEND_MAIN_C)
    config_c = read(CONFIG_MENU_C)
    config_h = read(CONFIG_MENU_H)

    require("const uint8_t show_disk_activity =\n"
            "        (menu == NULL || menu->disk2_activity_visible != 0U) ? 1U : 0U;" in source,
            "frontend compositor must read Disk II activity visibility from the config menu")
    require("uint8_t config_menu_storage_activity_page_visible(const config_menu_t *menu);" in config_h and
            "uint8_t config_menu_storage_activity_page_visible(const config_menu_t *menu)" in config_c and
            "menu->tab == CONFIG_TAB_SMARTPORT ||\n"
            "            menu->tab == CONFIG_TAB_DISK2" in config_c and
            "menu->browser_active != 0U" in config_c,
            "storage activity overlay must be explicitly visible on SmartPort and Disk II pages")
    require("const uint8_t draw_disk_activity_after_menu =\n"
            "        (show_disk_activity != 0U &&\n"
            "         config_menu_storage_activity_page_visible(menu) != 0U) ? 1U : 0U;" in source and
            "if (show_disk_activity == 0U) {" in source and
            "} else if (draw_disk_activity_after_menu == 0U) {\n"
            "        ui_draw_storage_activity(fb, s);\n"
            "    }" in source and
            "if (draw_disk_activity_after_menu != 0U) {\n"
            "        ui_draw_storage_activity(fb, s);\n"
            "    }" in source,
            "storage activity overlay must draw after the SmartPort/Disk II menu pages")
    require("if (show_disk_activity == 0U) {" in source and
            "ui_restore_static_rect(fb,\n"
            "                               UI_DISK_ACTIVITY_X," in source and
            "memset(&g_storage_activity, 0, sizeof(g_storage_activity));" in source,
            "disabled storage activity overlay must not poll or draw stale activity")


def test_storage_activity_draws_disk2_or_smartport_label() -> None:
    source = read(FRONTEND_MAIN_C)

    require('"DISK II D%u"' in source and '"SMARTPORT SP%u"' in source,
            "storage activity overlay must label Disk II and SmartPort sources")
    require("UI_STORAGE_SOURCE_DISK2" in source and
            "UI_STORAGE_SOURCE_SMARTPORT" in source,
            "storage activity overlay must remember the most recent active source")
    require("uint8_t disk2_last_unit;" in source and
            "uint8_t smartport_last_unit;" in source and
            "unit = (g_storage_activity.smartport_last_unit <\n"
            "                SMARTPORT_SERVICE_DEVICE_COUNT) ?" in source and
            "unit = (g_storage_activity.disk2_last_unit < DISK2_DRIVE_COUNT) ?" in source,
            "storage activity overlay must keep the last active device while idle")
    require("if (smartport_valid == 0U) {\n"
            "            source = UI_STORAGE_SOURCE_DISK2;\n"
            "        }" in source and
            "(status_active == 0U && read_active == 0U && write_active == 0U)) {\n"
            "            source = UI_STORAGE_SOURCE_DISK2;" not in source,
            "SmartPort idle state must not fall back to Disk II drive 1")
    require("drive_active = (disk2_activity.motor_on != 0U ||\n"
            "                        disk2_activity.spinning != 0U) ? 1U : 0U;" in source,
            "Disk II square should still represent motor/spinning state")
    require("drive_active = (status_active != 0U || read_active != 0U ||\n"
            "                        write_active != 0U) ? 1U : 0U;" in source,
            "SmartPort square should represent recent command activity")
    require("title_color = (present != 0U && drive_active != 0U) ?\n"
            "            FB16_COLOR_WHITE : FB16_COLOR_DARK_GRAY;" in source and
            "title_color = (disk2_activity.enabled != 0U && present != 0U &&\n"
            "                       (drive_active != 0U || read_active != 0U ||\n"
            "                        write_active != 0U)) ?\n"
            "            FB16_COLOR_WHITE : FB16_COLOR_DARK_GRAY;" in source,
            "idle storage activity overlays must dim the retained device label")
    require("fb16_fill_rect(fb, x + 328, y + 9, 10, 10, drive_color);" in source and
            "fb16_rect(fb, x + 328, y + 9, 10, 10, FB16_COLOR_DARK_GRAY);" in source,
            "storage activity square must draw active and inactive states")


def test_smartport_service_exposes_activity_counts() -> None:
    header = read(SMARTPORT_SERVICE_H)
    source = read(SMARTPORT_SERVICE_C)

    require("typedef struct {\n"
            "    uint8_t present_mask;\n"
            "    uint8_t device;\n"
            "    uint8_t read_only;\n"
            "    uint32_t status_count;\n"
            "    uint32_t read_count;\n"
            "    uint32_t write_count;\n"
            "} smartport_activity_t;" in header,
            "SmartPort service must expose an activity snapshot type")
    require("int smartport_service_get_activity(smartport_activity_t *out);" in header,
            "SmartPort service must expose an activity snapshot API")
    require("static void smartport_note_activity(uint8_t command," in source and
            "g_activity_status_count++;" in source and
            "g_activity_read_count++;" in source and
            "g_activity_write_count++;" in source and
            "smartport_note_activity(activity_cmd, dev, result);" in source,
            "SmartPort command execution must count successful status/read/write activity")
    require("out->present_mask = smartport_present_mask();" in source and
            "out->read_only = ((out->present_mask & (uint8_t)(1U << device)) != 0U &&" in source,
            "SmartPort activity snapshots must include media presence and lock state")


TESTS = [
    test_storage_activity_tracks_disk2_without_hiding_smartport,
    test_disk_activity_visibility_is_configurable,
    test_storage_activity_draws_disk2_or_smartport_label,
    test_smartport_service_exposes_activity_counts,
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
        print(f"{len(TESTS) - len(failures)} of {len(TESTS)} frontend storage activity tests passed; "
              f"{len(failures)} failed")
        return 1
    print(f"{len(TESTS)} frontend storage activity tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
