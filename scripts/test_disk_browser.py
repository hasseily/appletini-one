#!/usr/bin/env python3
"""Run the production disk-browser logic natively with an in-memory FatFs list.

Only filesystem I/O, preview work, reader calls and mount actions are stubbed. The tests use
the real entry types, filters, sorting, duplicate checks and navigation code.
Generated C and its executable stay in build/disk_browser_test.
"""

from pathlib import Path
import re
import subprocess
import textwrap

from test_onee_vtw_runtime import find_native_c_compiler


ROOT = Path(__file__).resolve().parents[1]
FRONTEND = ROOT / "ps_sources/frontend"
BUILD = ROOT / "build/disk_browser_test"


def extract_function(source: str, name: str) -> str:
    match = re.search(
        rf"^(?:static )?[^\n;{{}}]+\b{re.escape(name)}\([^;{{}}]*\)\s*\n"
        rf"\{{.*?^\}}", source, re.MULTILINE | re.DOTALL)
    if match is None:
        raise RuntimeError(f"Cannot extract production function {name}")
    return match.group(0)


def check_reader_integration(source: str) -> None:
    """Guard the reader's modal input, draw order and lifetime in the full menu."""
    handler = extract_function(source, "config_menu_handle_input")
    dispatch = re.search(
        r"if \(config_menu_is_active\(menu\) &&\s*"
        r"config_menu_text_reader_handle_input\(input\) != 0U\) \{\s*return 1U;\s*\}",
        handler)
    if dispatch is None:
        raise RuntimeError("Reader must consume input and return before the menu handles it")
    for later in ("if (input.key == UI_KEY_MENU)", "if (menu->browser_active != 0U)"):
        if later not in handler or dispatch.end() >= handler.index(later):
            raise RuntimeError("Reader input must precede menu toggle and browser navigation")
    closer = extract_function(source, "config_menu_browser_close")
    if "config_menu_text_reader_close();" not in closer:
        raise RuntimeError("Closing the browser must close its reader")
    active = extract_function(source, "config_menu_set_active")
    deactivate = re.search(r"\} else \{(?P<body>.*?)^    \}", active, re.MULTILINE | re.DOTALL)
    if deactivate is None or "config_menu_browser_close(menu);" not in deactivate["body"]:
        raise RuntimeError("Deactivating the menu must close its browser and reader")
    draw = extract_function(source, "config_menu_draw")
    browser_draw = draw.find("config_menu_draw_browser(")
    reader_draw = draw.find("config_menu_text_reader_draw(")
    if browser_draw < 0 or reader_draw <= browser_draw:
        raise RuntimeError("Draw the reader after the browser so its panel stays visible")
    workspace_builder = (ROOT / "scripts/create_vitis_workspace.py").read_text(encoding="utf-8")
    if '"../../../ps_sources/frontend/config_menu_text_reader.c"' not in workspace_builder:
        raise RuntimeError("Register the reader source in the production Vitis build")
    print("DISK BROWSER READER INTEGRATION PASS: 5 guards", flush=True)


def main() -> int:
    compiler = find_native_c_compiler()
    if compiler is None:
        raise RuntimeError("A native C compiler is required for disk-browser tests")
    source = (FRONTEND / "config_menu.c").read_text(encoding="utf-8")
    check_reader_integration(source)
    header = (FRONTEND / "config_menu.h").read_text(encoding="utf-8")
    defines = []
    for name in (
        "CONFIG_MENU_PATH_LEN", "CONFIG_BROWSER_MAX_ENTRIES",
        "CONFIG_BROWSER_VISIBLE_ROWS", "CONFIG_BROWSER_PROFILE_IMAGE_VISIBLE_ROWS",
        "CONFIG_BROWSER_CAT_SMARTPORT", "CONFIG_BROWSER_CAT_DISK2",
        "CONFIG_BROWSER_CAT_BEZEL", "CONFIG_BROWSER_CAT_ROM",
        "CONFIG_BROWSER_CAT_PROFILE", "CONFIG_BROWSER_CAT_PRINTOUT",
        "CONFIG_BROWSER_CAT_COUNT", "CONFIG_DISK2_STANDARD_TRACK_BYTES",
        "CONFIG_DISK2_STANDARD_TRACKS", "CONFIG_DISK2_MAX_TRACKS",
        "CONFIG_DISK2_NIB_TRACK_BYTES", "CONFIG_DISK2_2MG_HEADER_BYTES",
    ):
        match = re.search(rf"^#define {name}\s+\S+", source + "\n" + header, re.MULTILINE)
        if match is None:
            raise RuntimeError(f"Missing production define {name}")
        defines.append(match.group(0))
    types = []
    for kind, name in (("enum", "config_browser_target_t"),
                       ("enum", "config_browser_entry_type_t"),
                       ("struct", "config_browser_entry_t")):
        match = re.search(rf"typedef {kind} \{{[^{{}}]*\}} {name};", source)
        if match is None:
            raise RuntimeError(f"Missing production type {name}")
        types.append(match.group(0))
    names = (
        "config_menu_ascii_lower", "config_menu_str_ieq", "config_menu_path_eq_char",
        "config_menu_path_ieq", "config_menu_smartport_path_in_use",
        "config_menu_disk2_path_in_use", "config_menu_has_smartport_ext",
        "config_menu_has_disk2_ext", "config_menu_has_png_ext", "config_menu_has_txt_ext",
        "config_menu_is_video_rom_file", "config_menu_copy_text",
        "config_menu_path_is_root", "config_menu_join_path", "config_menu_parent_path",
        "config_menu_browser_is_disk2_target", "config_menu_browser_is_smartport_target",
        "config_menu_browser_is_disk_target",
        "config_menu_browser_is_bezel_target", "config_menu_browser_category",
        "config_menu_dir_accessible", "config_menu_browser_remember_dir",
        "config_menu_browser_target_smartport_device",
        "config_menu_browser_entry_is_duplicate_image", "config_menu_browser_entry_is_disabled",
        "config_menu_browser_empty_text", "config_menu_browser_accepts",
        "config_menu_browser_add_entry", "config_menu_browser_compare",
        "config_menu_browser_sort", "config_menu_browser_refresh",
        "config_menu_browser_get_entry", "config_menu_browser_selected_file",
        "config_menu_browser_visible_rows", "config_menu_browser_keep_selection_visible",
        "config_menu_browser_select_first_image", "config_menu_browser_close",
        "config_menu_browser_set_dir", "config_menu_browser_default_dir",
        "config_menu_open_browser", "config_menu_browser_select",
        "config_menu_browser_parent", "config_menu_browser_move",
    )
    functions = [extract_function(source, name) for name in names]
    prototypes = [function[:function.index("{")].rstrip() + ";" for function in functions]
    preamble = textwrap.dedent(r'''
        #include <assert.h>
        #include <stdint.h>
        #include <stdio.h>
        #include <stdlib.h>
        #include <string.h>
        typedef uint64_t FSIZE_t;
        typedef int FRESULT;
        #define FR_OK 0
        #define FR_INVALID_OBJECT 1
        #define FR_NO_FILE 2
        #define AM_DIR 0x10U
        #define AM_RDO 0x01U
        #define SMARTPORT_DEVICE_COUNT 8U
        #define PRINTER_SERVICE_DIR "0:/PRINT"
        #define CHECK(value, message) do { if (!(value)) { \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, message); exit(1); \
        } } while (0)
    ''') + "\n".join(defines + types) + textwrap.dedent(r'''
        typedef struct {
            uint8_t browser_active, browser_target;
            uint16_t browser_selected, browser_top, browser_count;
            char browser_dir[CONFIG_MENU_PATH_LEN];
            char browser_last_dir[CONFIG_BROWSER_CAT_COUNT][CONFIG_MENU_PATH_LEN];
            char smartport_disk_paths[8][CONFIG_MENU_PATH_LEN];
            uint8_t smartport_slots[8];
            char disk2_disk_paths[2][CONFIG_MENU_PATH_LEN];
            char bezel_path[CONFIG_MENU_PATH_LEN];
            char video_rom_path[CONFIG_MENU_PATH_LEN];
        } config_menu_t;
        typedef struct {
            char fname[CONFIG_MENU_PATH_LEN];
            FSIZE_t fsize;
            uint8_t fattrib;
        } FILINFO;
        typedef struct { unsigned index; } DIR;
        static config_browser_entry_t g_browser_entries[CONFIG_BROWSER_MAX_ENTRIES];
        static FILINFO fixture[CONFIG_BROWSER_MAX_ENTRIES + 32U];
        static unsigned fixture_count, applied, emptied, previewed, errors;
        static unsigned reader_opened, reader_closed;
        static config_menu_t *reader_menu;
        static char reader_name[CONFIG_MENU_PATH_LEN], reader_path[CONFIG_MENU_PATH_LEN];
        static uint8_t apply_ok = 1U;
        static FRESULT open_result = FR_OK;
        static FRESULT f_opendir(DIR *dir, const char *path)
        { (void)path; dir->index = 0U; return open_result; }
        static FRESULT f_readdir(DIR *dir, FILINFO *info)
        {
            memset(info, 0, sizeof(*info));
            if (dir->index < fixture_count) *info = fixture[dir->index++];
            return FR_OK;
        }
        static FRESULT f_closedir(DIR *dir) { (void)dir; return FR_OK; }
        static FRESULT config_menu_mount_sd(void) { return open_result; }
        static void config_menu_browser_preview_clear(void) { }
        static void config_menu_browser_preview_prepare(config_menu_t *menu)
        { (void)menu; ++previewed; }
        static void config_menu_refresh_smartport_media_after_menu_sd(config_menu_t *menu)
        { (void)menu; }
        static void config_menu_set_sd_error(config_menu_t *menu, const char *text, FRESULT fr)
        { (void)menu; (void)text; (void)fr; ++errors; }
        static uint8_t config_menu_browser_apply_file(config_menu_t *menu, const char *path)
        { (void)menu; (void)path; ++applied; return apply_ok; }
        static void config_menu_browser_apply_empty(config_menu_t *menu)
        { (void)menu; ++emptied; }
        static void config_menu_text_reader_open(config_menu_t *menu, const char *name, const char *path)
        {
            ++reader_opened; reader_menu = menu;
            snprintf(reader_name, sizeof(reader_name), "%s", name);
            snprintf(reader_path, sizeof(reader_path), "%s", path);
        }
        static void config_menu_text_reader_close(void) { ++reader_closed; }
    ''')
    tests = textwrap.dedent(r'''
        static void reset_fixture(config_menu_t *menu, uint8_t target)
        {
            memset(menu, 0, sizeof(*menu));
            memset(g_browser_entries, 0, sizeof(g_browser_entries));
            memset(fixture, 0, sizeof(fixture));
            fixture_count = applied = emptied = previewed = errors = 0U;
            reader_opened = reader_closed = 0U; reader_menu = NULL;
            reader_name[0] = reader_path[0] = '\0';
            apply_ok = 1U; open_result = FR_OK;
            menu->browser_active = 1U; menu->browser_target = target;
        }
        static void add_file(const char *name, FSIZE_t size, uint8_t attributes)
        {
            FILINFO *info;
            CHECK(fixture_count < sizeof(fixture)/sizeof(fixture[0]), "fixture capacity");
            info = &fixture[fixture_count++];
            snprintf(info->fname, sizeof(info->fname), "%s", name);
            info->fsize = size; info->fattrib = attributes;
        }
        static uint16_t find_entry(const config_menu_t *menu, const char *name)
        {
            for (uint16_t i = 0; i < menu->browser_count; ++i)
                if (strcmp(g_browser_entries[i].name, name) == 0) return i;
            return UINT16_MAX;
        }
        static config_browser_entry_t *selected(config_menu_t *menu)
        {
            CHECK(menu->browser_selected < menu->browser_count, "selected row must exist");
            return &g_browser_entries[menu->browser_selected];
        }
        static void check_visible(const config_menu_t *menu)
        {
            CHECK(menu->browser_top <= menu->browser_selected &&
                  menu->browser_selected < menu->browser_top + config_menu_browser_visible_rows(menu),
                  "selected row must stay visible");
        }
        static void test_smartport_initial_and_disabled_rows(void)
        {
            config_menu_t menu;
            reset_fixture(&menu, CONFIG_BROWSER_TARGET_SMARTPORT_1);
            add_file("z.po", 1U, 0U); add_file("Readme.bin", 4096U, 0U);
            add_file("a.hdv", 512U, AM_RDO); add_file("Folder", 0U, AM_DIR);
            add_file(".", 0U, AM_DIR); add_file("..", 0U, AM_DIR);
            config_menu_browser_set_dir(&menu, "0:/disks");
            CHECK(menu.browser_count == 6U, "show rejected files but omit dot entries");
            CHECK(strcmp(selected(&menu)->name, "a.hdv") == 0, "first sorted image, not directory/control");
            CHECK(selected(&menu)->read_only && !config_menu_browser_entry_is_disabled(&menu, selected(&menu)),
                  "read-only images remain selectable");
            CHECK(g_browser_entries[find_entry(&menu, "Readme.bin")].type == CONFIG_BROWSER_ENTRY_FILTERED,
                  "rejected file remains visible with filtered type");
            CHECK(g_browser_entries[2].type == CONFIG_BROWSER_ENTRY_DIR, "directories sort before file rows");
            config_menu_browser_move(&menu, 1);
            CHECK(strcmp(selected(&menu)->name, "z.po") == 0, "down skips filtered row between images");
            config_menu_browser_move(&menu, 1);
            CHECK(menu.browser_selected == 0U, "down wraps to parent");
            config_menu_browser_move(&menu, -1);
            CHECK(strcmp(selected(&menu)->name, "z.po") == 0, "up wraps to final enabled row");
            menu.browser_selected = find_entry(&menu, "Readme.bin");
            CHECK(config_menu_browser_entry_is_disabled(&menu, selected(&menu)), "filtered row disabled");
            config_menu_browser_select(&menu);
            CHECK(applied == 0U && menu.browser_active, "direct activation cannot mount a filtered row");
            CHECK(!config_menu_browser_selected_file(&menu, NULL, 0U, NULL, 0U), "filtered row is not an actionable file");
            config_menu_browser_move(&menu, -1);
            CHECK(strcmp(selected(&menu)->name, "a.hdv") == 0, "up leaves disabled row for nearest image");
            check_visible(&menu);
            apply_ok = 0U; config_menu_browser_select(&menu);
            CHECK(applied == 1U && menu.browser_active, "failed valid mount keeps browser open");
            apply_ok = 1U; config_menu_browser_select(&menu);
            CHECK(applied == 2U && !menu.browser_active, "successful valid mount closes browser");
        }
        static void test_disk2_filter_geometry(void)
        {
            config_menu_t menu;
            reset_fixture(&menu, CONFIG_BROWSER_TARGET_DISK2_D1);
            add_file("small.po", 1U, 0U); add_file("large.hdv", 33554432U, 0U);
            add_file("broken.dsk", 143000U, AM_RDO); add_file("z.woz", 12U, 0U);
            add_file("a.DSK", 143360U, AM_RDO); add_file("bad.woz", 11U, 0U);
            config_menu_open_browser(&menu, CONFIG_BROWSER_TARGET_DISK2_D1);
            CHECK(strcmp(selected(&menu)->name, "a.DSK") == 0, "open picker also chooses first image");
            CHECK(menu.browser_count == 8U, "slot 6 retains all six file rows");
            CHECK(g_browser_entries[find_entry(&menu, "small.po")].type == CONFIG_BROWSER_ENTRY_FILTERED &&
                  g_browser_entries[find_entry(&menu, "broken.dsk")].type == CONFIG_BROWSER_ENTRY_FILTERED &&
                  g_browser_entries[find_entry(&menu, "large.hdv")].type == CONFIG_BROWSER_ENTRY_FILTERED &&
                  g_browser_entries[find_entry(&menu, "bad.woz")].type == CONFIG_BROWSER_ENTRY_FILTERED,
                  "extension and geometry rejection become disabled rows");
            CHECK(g_browser_entries[find_entry(&menu, "broken.dsk")].read_only,
                  "filtered files retain their read-only attribute for the lock column");
            config_menu_browser_move(&menu, 1);
            CHECK(strcmp(selected(&menu)->name, "z.woz") == 0, "slot 6 navigation skips all incompatible images");
            config_menu_browser_move(&menu, 1);
            CHECK(selected(&menu)->type == CONFIG_BROWSER_ENTRY_CLOSE, "root close remains reachable");
        }
        static void test_duplicates(void)
        {
            config_menu_t menu;
            const uint8_t targets[] = {CONFIG_BROWSER_TARGET_SMARTPORT_1,
                CONFIG_BROWSER_TARGET_SMARTPORT_8, CONFIG_BROWSER_TARGET_DISK2_D1,
                CONFIG_BROWSER_TARGET_DISK2_D2};
            for (unsigned t = 0; t < sizeof(targets)/sizeof(targets[0]); ++t) {
                reset_fixture(&menu, targets[t]);
                add_file("a.po", 143360U, 0U); add_file("b.po", 143360U, 0U);
                if (t < 2U) {
                    const unsigned other = t == 0U ? 7U : 0U;
                    menu.smartport_slots[other] = 1U;
                    strcpy(menu.smartport_disk_paths[other], "0:\\DISKS\\A.PO");
                } else {
                    strcpy(menu.disk2_disk_paths[t == 2U ? 1U : 0U], "0:\\DISKS\\A.PO");
                }
                config_menu_browser_set_dir(&menu, "0:/disks");
                CHECK(strcmp(selected(&menu)->name, "b.po") == 0, "initial focus skips another mounted image");
                menu.browser_selected = find_entry(&menu, "a.po");
                CHECK(config_menu_browser_entry_is_disabled(&menu, selected(&menu)), "duplicate paths are case/slash insensitive");
                CHECK(!config_menu_browser_selected_file(&menu, NULL, 0U, NULL, 0U),
                      "duplicate rows cannot become actionable files");
                config_menu_browser_select(&menu);
                CHECK(applied == 0U && menu.browser_active, "duplicate activation never reaches mount callback");
                config_menu_browser_move(&menu, 1);
                CHECK(strcmp(selected(&menu)->name, "b.po") == 0, "navigation skips duplicates");
            }
            reset_fixture(&menu, CONFIG_BROWSER_TARGET_SMARTPORT_1);
            menu.smartport_slots[0] = 1U; strcpy(menu.smartport_disk_paths[0], "0:/disks/a.po");
            strcpy(menu.smartport_disk_paths[1], "0:/disks/a.po"); /* Disabled peer is not mounted. */
            add_file("a.po", 1U, 0U);
            config_menu_browser_set_dir(&menu, "0:/disks");
            CHECK(selected(&menu)->type == CONFIG_BROWSER_ENTRY_FILE &&
                  !config_menu_browser_entry_is_disabled(&menu, selected(&menu)), "own image and disabled SmartPort peer allowed");
            reset_fixture(&menu, CONFIG_BROWSER_TARGET_DISK2_D1);
            strcpy(menu.disk2_disk_paths[0], "0:/disks/a.po"); add_file("a.po", 143360U, 0U);
            config_menu_browser_set_dir(&menu, "0:/disks");
            CHECK(selected(&menu)->type == CONFIG_BROWSER_ENTRY_FILE, "own Disk II image stays selectable");
        }
        static void test_fallback_directory_and_scroll(void)
        {
            config_menu_t menu;
            char name[40];
            reset_fixture(&menu, CONFIG_BROWSER_TARGET_DISK2_D1);
            add_file("notes.bin", 100U, 0U); add_file("Child", 0U, AM_DIR);
            config_menu_browser_set_dir(&menu, "0:/disks");
            CHECK(menu.browser_selected == 0U && selected(&menu)->type == CONFIG_BROWSER_ENTRY_PARENT,
                  "no compatible image falls back to parent");
            config_menu_browser_move(&menu, -1);
            CHECK(selected(&menu)->type == CONFIG_BROWSER_ENTRY_DIR, "disabled tail skipped on upward wrap");
            fixture_count = 0U; add_file("disk.woz", 12U, 0U);
            config_menu_browser_select(&menu);
            CHECK(strcmp(menu.browser_dir, "0:/disks/Child") == 0 && strcmp(selected(&menu)->name, "disk.woz") == 0,
                  "entering a child directory selects its first image");
            config_menu_browser_parent(&menu);
            CHECK(strcmp(menu.browser_dir, "0:/disks") == 0 && selected(&menu)->type == CONFIG_BROWSER_ENTRY_FILE,
                  "parent navigation applies the same first-image policy");
            fixture_count = 0U; config_menu_browser_set_dir(&menu, "0:/");
            CHECK(menu.browser_count == 2U && menu.browser_selected == 0U && selected(&menu)->type == CONFIG_BROWSER_ENTRY_CLOSE,
                  "empty root falls back to close with empty option retained");
            config_menu_browser_move(&menu, 1); config_menu_browser_select(&menu);
            CHECK(emptied == 1U && !menu.browser_active, "empty-drive action remains reachable");
            reset_fixture(&menu, CONFIG_BROWSER_TARGET_SMARTPORT_1);
            for (unsigned i = 0; i < CONFIG_BROWSER_VISIBLE_ROWS + 3U; ++i) {
                snprintf(name, sizeof(name), "folder%02u", i); add_file(name, 0U, AM_DIR);
            }
            add_file("image.po", 1U, 0U); config_menu_browser_set_dir(&menu, "0:/");
            CHECK(selected(&menu)->type == CONFIG_BROWSER_ENTRY_FILE && menu.browser_top > 0U,
                  "initial image below many directories scrolls into view");
            check_visible(&menu); config_menu_browser_move(&menu, 1); check_visible(&menu);
            CHECK(menu.browser_selected == 0U && menu.browser_top == 0U, "wrap updates scroll to first row");
        }
        static void test_capacity_and_empty_movement(void)
        {
            config_menu_t menu;
            char name[40];
            reset_fixture(&menu, CONFIG_BROWSER_TARGET_SMARTPORT_1);
            for (unsigned i = 0; i < CONFIG_BROWSER_MAX_ENTRIES + 4U; ++i) {
                snprintf(name, sizeof(name), "filtered%03u.bin", i); add_file(name, 1U, 0U);
            }
            add_file("late.po", 1U, 0U); add_file("late_directory", 0U, AM_DIR);
            config_menu_browser_set_dir(&menu, "0:/");
            CHECK(menu.browser_count == CONFIG_BROWSER_MAX_ENTRIES, "entry list stays bounded");
            CHECK(find_entry(&menu, "late.po") != UINT16_MAX && find_entry(&menu, "late_directory") != UINT16_MAX,
                  "late accepted image and directory displace filtered rows when full");
            CHECK(strcmp(selected(&menu)->name, "late.po") == 0, "filtered capacity cannot hide the only image");
            reset_fixture(&menu, CONFIG_BROWSER_TARGET_SMARTPORT_1);
            for (unsigned i = 0; i < CONFIG_BROWSER_MAX_ENTRIES - 2U; ++i) {
                snprintf(name, sizeof(name), "image%03u.po", i); add_file(name, 1U, 0U);
            }
            add_file("late.bin", 1U, 0U); config_menu_browser_set_dir(&menu, "0:/");
            CHECK(menu.browser_count == CONFIG_BROWSER_MAX_ENTRIES && find_entry(&menu, "late.bin") == UINT16_MAX,
                  "filtered arrivals never displace accepted rows");
            for (uint16_t i = 0U; i < menu.browser_count; ++i)
                CHECK(g_browser_entries[i].type != CONFIG_BROWSER_ENTRY_FILTERED, "all accepted rows retained");
            menu.browser_count = 3U; menu.browser_selected = 0U;
            for (unsigned i = 0U; i < 3U; ++i) g_browser_entries[i].type = CONFIG_BROWSER_ENTRY_FILTERED;
            config_menu_browser_move(&menu, 1); config_menu_browser_move(&menu, -1);
            CHECK(menu.browser_selected < menu.browser_count, "all-disabled list cannot loop or go out of bounds");
            menu.browser_count = 0U; menu.browser_selected = 0U;
            config_menu_browser_move(&menu, 1); config_menu_browser_move(&menu, -1);
            CHECK(menu.browser_selected == 0U, "zero-row navigation is inert");
        }
        static void test_text_entries_and_reader(void)
        {
            config_menu_t menu;
            char name[40], saved_dir[CONFIG_MENU_PATH_LEN];
            char action_name[CONFIG_MENU_PATH_LEN], action_path[CONFIG_MENU_PATH_LEN];
            const uint8_t targets[] = {CONFIG_BROWSER_TARGET_SMARTPORT_1,
                CONFIG_BROWSER_TARGET_SMARTPORT_8, CONFIG_BROWSER_TARGET_DISK2_D1,
                CONFIG_BROWSER_TARGET_DISK2_D2};
            for (unsigned t = 0U; t < sizeof(targets)/sizeof(targets[0]); ++t) {
                reset_fixture(&menu, targets[t]);
                add_file("00-readme.tXt", 1U, AM_RDO);
                add_file("middle.po", 143360U, 0U);
                add_file("zz-last.TXT", 0U, 0U);
                add_file("ignore.txt.bak", 1U, 0U);
                add_file("ignoretxt", 1U, 0U);
                for (unsigned i = 0U; i < CONFIG_BROWSER_VISIBLE_ROWS + 3U; ++i) {
                    snprintf(name, sizeof(name), "folder%02u", i); add_file(name, 0U, AM_DIR);
                }
                config_menu_browser_set_dir(&menu, "0:/documents");
                CHECK(g_browser_entries[find_entry(&menu, "00-readme.tXt")].type == CONFIG_BROWSER_ENTRY_TEXT &&
                      g_browser_entries[find_entry(&menu, "zz-last.TXT")].type == CONFIG_BROWSER_ENTRY_TEXT,
                      "all disk targets recognize case-insensitive TXT independent of size or read-only flag");
                CHECK(g_browser_entries[find_entry(&menu, "ignore.txt.bak")].type == CONFIG_BROWSER_ENTRY_FILTERED &&
                      g_browser_entries[find_entry(&menu, "ignoretxt")].type == CONFIG_BROWSER_ENTRY_FILTERED,
                      "only the final .txt extension opens the reader");
                CHECK(strcmp(selected(&menu)->name, "middle.po") == 0 && menu.browser_top > 0U,
                      "initial focus still chooses image after earlier TXT and scrolls into view");
                config_menu_browser_move(&menu, -1);
                CHECK(strcmp(selected(&menu)->name, "00-readme.tXt") == 0 &&
                      !config_menu_browser_entry_is_disabled(&menu, selected(&menu)),
                      "read-only TXT is selectable by normal navigation");
                check_visible(&menu);
                strcpy(action_name, "unchanged-name"); strcpy(action_path, "unchanged-path");
                CHECK(!config_menu_browser_selected_file(&menu, action_name, sizeof(action_name), action_path, sizeof(action_path)) &&
                      strcmp(action_name, "unchanged-name") == 0 && strcmp(action_path, "unchanged-path") == 0,
                      "TXT remains excluded from image-action selected_file API");
                const uint16_t saved_selected = menu.browser_selected;
                const uint16_t saved_top = menu.browser_top;
                const uint16_t saved_count = menu.browser_count;
                const unsigned saved_closed = reader_closed;
                strcpy(saved_dir, menu.browser_dir);
                config_menu_browser_select(&menu);
                CHECK(reader_opened == 1U && reader_menu == &menu &&
                      strcmp(reader_name, "00-readme.tXt") == 0 &&
                      strcmp(reader_path, "0:/documents/00-readme.tXt") == 0,
                      "TXT activation reaches reader with exact selected name and path");
                CHECK(applied == 0U && emptied == 0U && reader_closed == saved_closed,
                      "TXT activation neither mounts media nor closes browser/reader");
                CHECK(menu.browser_active && menu.browser_target == targets[t] &&
                      menu.browser_selected == saved_selected && menu.browser_top == saved_top &&
                      menu.browser_count == saved_count && strcmp(menu.browser_dir, saved_dir) == 0,
                      "opening TXT retains browser directory, selection and scroll position");
                config_menu_browser_close(&menu);
                CHECK(!menu.browser_active && reader_closed == saved_closed + 1U,
                      "closing browser closes its reader");

                reset_fixture(&menu, targets[t]);
                add_file("readme.txt", 100U, 0U); add_file("Child", 0U, AM_DIR);
                config_menu_browser_set_dir(&menu, "0:/documents");
                CHECK(menu.browser_selected == 0U && selected(&menu)->type == CONFIG_BROWSER_ENTRY_PARENT,
                      "TXT without an image does not override parent fallback");
                config_menu_browser_move(&menu, -1);
                CHECK(selected(&menu)->type == CONFIG_BROWSER_ENTRY_TEXT,
                      "TXT remains reachable when no image is present");
            }
        }
        static void test_text_capacity_priority(void)
        {
            config_menu_t menu;
            char name[40];
            reset_fixture(&menu, CONFIG_BROWSER_TARGET_SMARTPORT_1);
            for (unsigned i = 0U; i < CONFIG_BROWSER_MAX_ENTRIES - 2U; ++i) {
                snprintf(name, sizeof(name), "readme%03u.txt", i); add_file(name, 1U, 0U);
            }
            add_file("late.po", 1U, 0U); add_file("late_directory", 0U, AM_DIR);
            config_menu_browser_set_dir(&menu, "0:/");
            CHECK(menu.browser_count == CONFIG_BROWSER_MAX_ENTRIES &&
                  find_entry(&menu, "late.po") != UINT16_MAX && find_entry(&menu, "late_directory") != UINT16_MAX,
                  "late image and directory displace TXT when the browser is full");
            CHECK(strcmp(selected(&menu)->name, "late.po") == 0,
                  "TXT capacity cannot hide the only image or take initial focus");

            reset_fixture(&menu, CONFIG_BROWSER_TARGET_DISK2_D2);
            add_file("keep.po", 143360U, 0U); add_file("keep_directory", 0U, AM_DIR);
            for (unsigned i = 0U; i < CONFIG_BROWSER_MAX_ENTRIES - 4U; ++i) {
                snprintf(name, sizeof(name), "filtered%03u.bin", i); add_file(name, 1U, 0U);
            }
            add_file("late.txt", 1U, AM_RDO); config_menu_browser_set_dir(&menu, "0:/");
            CHECK(menu.browser_count == CONFIG_BROWSER_MAX_ENTRIES &&
                  find_entry(&menu, "late.txt") != UINT16_MAX && find_entry(&menu, "keep.po") != UINT16_MAX &&
                  find_entry(&menu, "keep_directory") != UINT16_MAX,
                  "late TXT displaces filtered content while retaining image and directory");

            reset_fixture(&menu, CONFIG_BROWSER_TARGET_SMARTPORT_8);
            for (unsigned i = 0U; i < CONFIG_BROWSER_MAX_ENTRIES - 2U; ++i) {
                snprintf(name, sizeof(name), "image%03u.po", i); add_file(name, 1U, 0U);
            }
            add_file("late.txt", 1U, 0U); config_menu_browser_set_dir(&menu, "0:/");
            CHECK(menu.browser_count == CONFIG_BROWSER_MAX_ENTRIES && find_entry(&menu, "late.txt") == UINT16_MAX,
                  "late TXT cannot displace an image in a full browser");
            for (unsigned i = 0U; i < CONFIG_BROWSER_MAX_ENTRIES - 2U; ++i) {
                snprintf(name, sizeof(name), "image%03u.po", i);
                CHECK(find_entry(&menu, name) != UINT16_MAX, "all images survive a late TXT arrival");
            }
        }
        static void test_other_browser_behavior(void)
        {
            config_menu_t menu;
            const uint8_t targets[] = {CONFIG_BROWSER_TARGET_BEZEL, CONFIG_BROWSER_TARGET_PROFILE_IMAGE,
                CONFIG_BROWSER_TARGET_PRINTOUT, CONFIG_BROWSER_TARGET_VIDEO_ROM};
            for (unsigned t = 0U; t < sizeof(targets)/sizeof(targets[0]); ++t) {
                reset_fixture(&menu, targets[t]);
                add_file("preview.png", 4096U, 0U); add_file("ignored.txt", 13U, 0U);
                config_menu_open_browser(&menu, targets[t]);
                CHECK(menu.browser_selected == 0U, "non-disk browsers retain control-row initial focus");
                CHECK(find_entry(&menu, "ignored.txt") == UINT16_MAX, "non-disk filters still hide unsupported files");
                CHECK(find_entry(&menu, "preview.png") != UINT16_MAX, "non-disk accepted file retained");
                config_menu_browser_set_dir(&menu, targets[t] == CONFIG_BROWSER_TARGET_PRINTOUT ? PRINTER_SERVICE_DIR : "0:/sub");
                CHECK(menu.browser_selected == 0U, "non-disk set_dir retains initial focus policy");
                if (targets[t] == CONFIG_BROWSER_TARGET_PRINTOUT) {
                    CHECK(menu.browser_count == 2U && selected(&menu)->type == CONFIG_BROWSER_ENTRY_CLOSE,
                          "printout root retains close, no empty row");
                    config_menu_browser_parent(&menu);
                    CHECK(strcmp(menu.browser_dir, PRINTER_SERVICE_DIR) == 0, "printout parent remains jailed");
                }
                menu.browser_selected = find_entry(&menu, "preview.png");
                CHECK(config_menu_browser_selected_file(&menu, NULL, 0U, NULL, 0U), "accepted non-disk file actions retained");
                CHECK(previewed != 0U, "browser operations still refresh preview");
            }
            reset_fixture(&menu, CONFIG_BROWSER_TARGET_DISK2_D1); open_result = FR_NO_FILE;
            config_menu_open_browser(&menu, CONFIG_BROWSER_TARGET_DISK2_D1);
            CHECK(!menu.browser_active && errors == 1U, "failed open retains close/error behavior");
        }
        int main(void)
        {
            test_smartport_initial_and_disabled_rows();
            test_disk2_filter_geometry();
            test_duplicates();
            test_fallback_directory_and_scroll();
            test_capacity_and_empty_movement();
            test_text_entries_and_reader();
            test_text_capacity_priority();
            test_other_browser_behavior();
            puts("DISK BROWSER NATIVE PASS: 8 behavior groups");
            return 0;
        }
    ''')
    BUILD.mkdir(parents=True, exist_ok=True)
    harness = BUILD / "disk_browser.c"
    executable = BUILD / "disk_browser.exe"
    harness.write_text(preamble + "\n".join(prototypes + functions) + tests, encoding="utf-8")
    subprocess.run([str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror", "-static",
                    str(harness), "-o", str(executable)], cwd=ROOT, check=True)
    subprocess.run([str(executable)], cwd=ROOT, check=True, timeout=15)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
