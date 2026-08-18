#!/usr/bin/env python3
"""Source-level regression tests for frontend storage activity UI."""

from __future__ import annotations

from pathlib import Path
import shutil
import subprocess
import tempfile
import textwrap


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


def test_storage_activity_uses_effective_disk2_state() -> None:
    source = read(FRONTEND_MAIN_C)
    draw = source[
        source.index("static void ui_draw_storage_activity("):
        source.index("static void ui_restore_debug_overlay_regions(")
    ]
    snapshot = source[
        source.index("static void ui_collect_debug_overlay_snapshot("):
        source.index("/* Two-phase compositor callback.")
    ]

    require("if (disk2_service_get_activity(&disk2_activity) == 0)" in draw and
            "disk2_valid = (disk2_activity.enabled != 0U) ? 1U : 0U;" in draw and
            "control_get_slot_enabled" not in draw,
            "storage UI must poll Disk II without a saved Slot 6 gate and use its effective enable bit")
    require("if (disk2_service_get_activity(&snapshot->disk2_activity) == 0)" in snapshot and
            "snapshot->disk2_valid =\n"
            "            (snapshot->disk2_activity.enabled != 0U) ? 1U : 0U;" in snapshot and
            "control_get_slot_enabled" not in snapshot,
            "debug snapshot must report the effective Disk II enable state during ONE//e")
    require("smartport_service_get_activity(&smartport_activity) == 0" in draw,
            "SmartPort activity must still be polled for the storage overlay")
    require("if (disk2_valid == 0U && smartport_valid == 0U) {\n"
            "        memset(&g_storage_activity, 0, sizeof(g_storage_activity));" in draw,
            "storage activity state should reset only when no storage source is available")


def test_disk_activity_visibility_is_configurable() -> None:
    source = read(FRONTEND_MAIN_C)
    config_c = read(CONFIG_MENU_C)
    config_h = read(CONFIG_MENU_H)
    compose = source[
        source.index("static int ui_compose_frame("):
        source.index("/* Adapter for the compositor's typed-erased callback contract. */")
    ]
    base_return = compose.index("return menu_active;")
    first_activity_draw = compose.index("ui_draw_storage_activity(fb, s);")
    menu_draw = compose.index("config_menu_draw(fb, menu, g_usb_menu_owned);")
    second_activity_draw = compose.index(
        "ui_draw_storage_activity(fb, s);", first_activity_draw + 1)

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
            base_return < first_activity_draw < menu_draw < second_activity_draw and
            "if (draw_disk_activity_after_menu != 0U) {\n"
            "        ui_draw_storage_activity(fb, s);\n"
            "    }" in source,
            "storage activity must be a foreground overlay and remain above "
            "the SmartPort/Disk II menu pages where requested")
    require("if (show_disk_activity == 0U) {" in source and
            "if (menu->border_enabled == 0U) {" in source and
            "ui_restore_static_rect(fb," in source and
            "UI_DISK_ACTIVITY_X," in source and
            "memset(&g_storage_activity, 0, sizeof(g_storage_activity));" in source,
            "disabled storage activity must restore the static background only when no border can repaint it")


def test_storage_activity_draws_disk2_or_smartport_label() -> None:
    source = read(FRONTEND_MAIN_C)
    selector = source[
        source.index("static uint8_t ui_storage_choose_source("):
        source.index("static void ui_draw_disk_lock_icon(")
    ]
    draw = source[
        source.index("static void ui_draw_storage_activity("):
        source.index("static void ui_restore_debug_overlay_regions(")
    ]

    require('"DISK II D%u"' in source and '"SMARTPORT SP%u"' in source,
            "storage activity overlay must label Disk II and SmartPort sources")
    require("UI_STORAGE_SOURCE_DISK2" in source and
            "UI_STORAGE_SOURCE_SMARTPORT" in source,
            "storage activity overlay must remember the most recent active source")
    require("uint8_t disk2_last_unit;" in source and
            "uint8_t smartport_last_unit;" in source and
            "uint8_t smartport_status_unit;" in source and
            "g_storage_activity.smartport_status_unit :\n"
            "                g_storage_activity.smartport_last_unit" in draw and
            "unit = (g_storage_activity.disk2_last_unit < DISK2_DRIVE_COUNT) ?" in source,
            "storage activity overlay must keep data and STATUS units separate")
    require("g_storage_activity.smartport_status_unit =\n"
            "                (smartport_activity.device" in draw and
            "g_storage_activity.smartport_last_unit =\n"
            "                (smartport_activity.device" in draw,
            "SmartPort STATUS must not replace the last data-transfer unit")
    sp_data = selector.index("smartport_data_active != 0U")
    disk2_activity = selector.index("disk2_active != 0U")
    sp_status = selector.index("smartport_status_active != 0U")
    retained = selector.index("retained_source == UI_STORAGE_SOURCE_SMARTPORT")
    require(sp_data < disk2_activity < sp_status < retained,
            "source priority must be SmartPort data, Disk II work, STATUS, then retained source")
    require("source = ui_storage_choose_source(disk2_valid," in draw,
            "storage overlay must use the tested source selector")
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


def _extract_c_function(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[start:pos + 1]
    raise TestFailure(f"could not extract {signature}")


def _find_native_c_compiler() -> Path | None:
    for name in ("gcc", "cc", "clang"):
        found = shutil.which(name)
        if found:
            return Path(found)
    xilinx = Path("C:/Xilinx")
    if xilinx.exists():
        matches = sorted(
            xilinx.glob("*/tps/mingw/*/win64.o/nt/bin/x86_64-w64-mingw32-gcc.exe"),
            reverse=True,
        )
        if matches:
            return matches[0]
    return None


def run_native_source_selection_test() -> bool:
    compiler = _find_native_c_compiler()
    if compiler is None:
        print("SKIP native_source_selection: no host C compiler")
        return True

    selector = _extract_c_function(
        read(FRONTEND_MAIN_C), "static uint8_t ui_storage_choose_source(")
    harness_source = textwrap.dedent(f"""
        #include <stdint.h>
        #define UI_STORAGE_SOURCE_DISK2 0U
        #define UI_STORAGE_SOURCE_SMARTPORT 1U

        {selector}

        #define CHECK(expected, ...) do {{ \\
            if (ui_storage_choose_source(__VA_ARGS__) != (expected)) return __LINE__; \\
        }} while (0)

        int main(void)
        {{
            /* SmartPort reads and writes remain visible even with a spinning floppy. */
            CHECK(UI_STORAGE_SOURCE_SMARTPORT, 1U, 1U, 1U, 1U, 1U,
                  UI_STORAGE_SOURCE_DISK2);
            /* A Disk II motor/read/write beats a SmartPort STATUS-only probe. */
            CHECK(UI_STORAGE_SOURCE_DISK2, 1U, 1U, 1U, 0U, 1U,
                  UI_STORAGE_SOURCE_SMARTPORT);
            CHECK(UI_STORAGE_SOURCE_SMARTPORT, 1U, 1U, 0U, 0U, 1U,
                  UI_STORAGE_SOURCE_DISK2);
            /* STATUS expiry returns to the last source that did real work. */
            CHECK(UI_STORAGE_SOURCE_DISK2, 1U, 1U, 0U, 0U, 0U,
                  UI_STORAGE_SOURCE_DISK2);
            CHECK(UI_STORAGE_SOURCE_SMARTPORT, 1U, 1U, 0U, 0U, 0U,
                  UI_STORAGE_SOURCE_SMARTPORT);
            /* If a retained service vanishes, use the service that remains. */
            CHECK(UI_STORAGE_SOURCE_DISK2, 1U, 0U, 0U, 0U, 0U,
                  UI_STORAGE_SOURCE_SMARTPORT);
            /* A disabled effective Disk II is invalid and cannot beat SmartPort. */
            CHECK(UI_STORAGE_SOURCE_SMARTPORT, 0U, 1U, 0U, 0U, 0U,
                  UI_STORAGE_SOURCE_DISK2);
            return 0;
        }}
    """)

    with tempfile.TemporaryDirectory(prefix="onee-storage-") as temp_dir:
        temp = Path(temp_dir)
        harness = temp / "storage_source_test.c"
        executable = temp / "storage_source_test.exe"
        harness.write_text(harness_source, encoding="utf-8")
        build = subprocess.run(
            [str(compiler), "-std=c99", "-Wall", "-Wextra", "-Werror",
             str(harness), "-o", str(executable)],
            capture_output=True,
            text=True,
        )
        if build.returncode != 0:
            print("FAIL native_source_selection: host compilation failed")
            print(build.stdout)
            print(build.stderr)
            return False
        run = subprocess.run([str(executable)], capture_output=True, text=True)
        if run.returncode != 0:
            print(f"FAIL native_source_selection: case at harness line {run.returncode}")
            return False
    print("PASS native_source_selection")
    return True


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
    test_storage_activity_uses_effective_disk2_state,
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
    native_ok = run_native_source_selection_test()
    if failures or not native_ok:
        print(f"{len(TESTS) - len(failures)} of {len(TESTS)} frontend storage activity tests passed; "
              f"{len(failures)} source checks failed")
        return 1
    print(f"{len(TESTS)} frontend storage activity source checks and native selection test passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
