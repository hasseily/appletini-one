#!/usr/bin/env python3
"""Exercise the production text reader with native FatFs/UI/heap stubs."""

from pathlib import Path
import re
import subprocess
import textwrap

from test_onee_vtw_runtime import find_native_c_compiler


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build/config_text_reader_test"


def main() -> int:
    compiler = find_native_c_compiler()
    if compiler is None:
        raise RuntimeError("A native C compiler is required for text reader tests")
    source = (ROOT / "ps_sources/frontend/config_menu_text_reader.c").read_text(
        encoding="utf-8")
    source = re.sub(r'^#include "config_menu[^\n]+\n', "", source, flags=re.MULTILINE)
    frontend = ROOT / "ps_sources/frontend"
    uart_header = (frontend / "uart_control.h").read_text(encoding="utf-8")
    menu_header = (frontend / "config_menu.h").read_text(encoding="utf-8")
    menu_source = (frontend / "config_menu.c").read_text(encoding="utf-8")
    boot_source = (frontend / "boot_menu_service.c").read_text(encoding="utf-8")
    card_header = (frontend / "card_control_regs.h").read_text(encoding="utf-8")

    def extract(text: str, pattern: str) -> str:
        match = re.search(pattern, text, flags=re.MULTILINE | re.DOTALL)
        if match is None:
            raise RuntimeError(f"Cannot extract production input code: {pattern}")
        return match.group(0)

    input_types = extract(uart_header, r"typedef enum \{\s+UI_KEY_NONE.*?} ui_input_t;")
    binding_types = extract(menu_header, r"typedef enum \{\s+CONFIG_MENU_USB_BIND_ACTION_UP.*?} config_menu_usb_bind_action_t;")
    binding_count = extract(menu_header, r"^#define CONFIG_MENU_USB_BIND_ACTION_COUNT[^\n]*")
    binding_keys = extract(menu_source, r"static const ui_key_t k_usb_binding_keys\[.*?\n};")
    boot_mapping = extract(boot_source, r"static uint8_t boot_menu_map_iiplus_key\(.*?(?=\nvoid boot_menu_service_init\()")
    machine_modes = "\n".join(extract(card_header, rf"^#define CARD_MACHINE_MODE_{name}[^\n]*")
                              for name in ("IIPLUS", "IIE"))
    input_source = "\n".join((
        "#include <stdint.h>", input_types, binding_types, binding_count,
        binding_keys, machine_modes,
        "static uint8_t machine_mode = CARD_MACHINE_MODE_IIE;\n"
        "static uint8_t boot_menu_service_machine_mode(void) { return machine_mode; }",
        boot_mapping,
    ))
    preamble = textwrap.dedent(r'''
        #include <stdint.h>
        #include <stdio.h>
        #include <stdlib.h>
        #include <string.h>
        #define CHECK(v) do { if (!(v)) { \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #v); exit(1); \
        } } while (0)
        #define CONFIG_MENU_PATH_LEN 256
        #define FB16_BUILTIN_FONT_ADVANCE_X 7
        #define CMUI_BODY_SCALE 2
        #define CMUI_TITLE_SCALE 2
        #define CMUI_SMALL_SCALE 2
        #define FB16_RGB(r,g,b) (((r) << 16) | ((g) << 8) | (b))
        #define CMUI_COLOR_PANEL 1U
        #define CMUI_COLOR_MUTED 2U
        #define CMUI_COLOR_TEXT 3U
        #define CMUI_COLOR_WARN 4U
        #define CMUI_COLOR_DOCUMENT 5U
        typedef struct { unsigned sentinel; } config_menu_t;
        typedef struct { int x, y, w, h; } cmui_rect_t;
        typedef unsigned UINT;
        typedef enum {
            FR_OK, FR_DISK_ERR, FR_NOT_READY, FR_NO_FILE,
            FR_INVALID_OBJECT, FR_NOT_ENABLED
        } FRESULT;
        #define FA_READ 1U
        typedef struct { unsigned unused; } FIL;
        static unsigned char data[256U * 1024U + 16U];
        static size_t data_size, data_pos;
        static unsigned opens, reads, closes, mounts, refreshes, file_open;
        static unsigned allocs, live_allocs, fail_alloc;
        static FRESULT first_open_result, mount_result, second_open_result;
        static FRESULT read_result, close_result;
        static char draw_lines[64][192];
        static unsigned drawn;
        static void *reader_malloc(size_t size)
        {
            void *p;
            ++allocs;
            if (fail_alloc != 0U && allocs == fail_alloc) return NULL;
            p = malloc(size);
            CHECK(p != NULL);
            ++live_allocs;
            return p;
        }
        static void reader_free(void *p)
        {
            if (p != NULL) { CHECK(live_allocs != 0U); --live_allocs; free(p); }
        }
        #define malloc reader_malloc
        #define free reader_free
        static FRESULT f_open(FIL *file, const char *path, unsigned mode)
        {
            FRESULT result;
            (void)file;
            CHECK(path != NULL && mode == FA_READ && file_open == 0U);
            result = (opens++ == 0U) ? first_open_result : second_open_result;
            if (result == FR_OK) { file_open = 1U; data_pos = 0U; }
            return result;
        }
        static FRESULT f_read(FIL *file, void *buffer, UINT request, UINT *got)
        {
            size_t count = data_size - data_pos;
            (void)file;
            CHECK(file_open != 0U);
            ++reads;
            if (read_result != FR_OK) { *got = 0U; return read_result; }
            if (count > request) count = request;
            memcpy(buffer, data + data_pos, count);
            data_pos += count;
            *got = (UINT)count;
            return FR_OK;
        }
        static FRESULT f_close(FIL *file)
        {
            (void)file;
            CHECK(file_open != 0U);
            file_open = 0U;
            ++closes;
            return close_result;
        }
        static FRESULT config_menu_mount_sd(void)
        { CHECK(file_open == 0U); ++mounts; return mount_result; }
        static void config_menu_refresh_smartport_media_after_menu_sd(config_menu_t *menu)
        { CHECK(menu != NULL && file_open == 0U); ++refreshes; }
        static void cmui_screen_rects(cmui_rect_t *nav, cmui_rect_t *body, cmui_rect_t *footer)
        {
            (void)nav; (void)footer;
            *body = (cmui_rect_t){380, 154, 1492, 812};
        }
        static void cmui_panel(uint16_t *fb, const cmui_rect_t *body, uint32_t color)
        { CHECK(fb != NULL && body != NULL && color == CMUI_COLOR_PANEL); }
        static void cmui_text_clipped(uint16_t *fb, int x, int y, int w,
                                     const char *text, uint32_t fg, uint32_t bg, int scale)
        {
            (void)fg;
            CHECK(fb != NULL && x >= 380 && x < 1872 && y >= 154 && y < 966);
            CHECK(w > 0 && x + w <= 1872 && bg == CMUI_COLOR_PANEL && scale == 2);
            CHECK(drawn < 64U);
            (void)snprintf(draw_lines[drawn++], sizeof(draw_lines[0]), "%s", text);
        }
    ''')
    tests = textwrap.dedent(r'''
        static config_menu_t menu;
        static void reset(const char *contents)
        {
            config_menu_text_reader_close();
            CHECK(live_allocs == 0U && file_open == 0U);
            data_size = strlen(contents);
            memcpy(data, contents, data_size);
            data_pos = 0U;
            opens = reads = closes = mounts = refreshes = allocs = fail_alloc = drawn = 0U;
            first_open_result = second_open_result = mount_result = FR_OK;
            read_result = close_result = FR_OK;
            menu.sentinel = 0x12345678U;
        }
        static void open_reader(void)
        {
            config_menu_text_reader_open(&menu, "README.txt", "0:/folder/README.txt");
            CHECK(config_menu_text_reader_active() && file_open == 0U);
            CHECK(menu.sentinel == 0x12345678U);
        }
        static void check_line(unsigned n, const char *expected)
        {
            char line[TEXT_READER_COLUMNS + 1U];
            CHECK(n < g_text_reader.count);
            (void)text_reader_line(g_text_reader.lines[n], line);
            CHECK(strcmp(line, expected) == 0);
        }
        static void key(ui_key_t value)
        { CHECK(config_menu_text_reader_handle_input((ui_input_t){value, 1U, 0U})); }
        static int drawn_contains(const char *text)
        {
            for (unsigned i = 0U; i < drawn; ++i)
                if (strstr(draw_lines[i], text) != NULL) return 1;
            return 0;
        }
        static void draw(void)
        {
            cmui_rect_t body;
            uint16_t fb;
            unsigned previous_reads = reads;
            cmui_screen_rects(NULL, &body, NULL);
            drawn = 0U;
            config_menu_text_reader_draw(&fb, &body);
            CHECK(reads == previous_reads && file_open == 0U);
        }
        static void test_newlines_and_tabs(void)
        {
            reset("One\r\nTwo\nThree\r\n\rFour\tX\n  indent\n");
            open_reader();
            CHECK(g_text_reader.count == 6U && g_text_reader.columns == 100U);
            CHECK(g_text_reader.page_rows == 27U);
            check_line(0U, "One"); check_line(1U, "Two"); check_line(2U, "Three");
            check_line(3U, ""); check_line(4U, "Four    X"); check_line(5U, "  indent");
            CHECK(opens == 1U && closes == 1U && mounts == 0U && refreshes == 0U);
            draw(); CHECK(drawn_contains("Lines 1-6 of 6"));
        }
        static void test_wrap(void)
        {
            char line[101];
            reset(""); memset(data, 'a', 95U);
            memcpy(data + 95U, " hello world\n", 13U); data_size = 108U;
            open_reader();
            CHECK(g_text_reader.count == 2U);
            memset(line, 'a', 95U); line[95] = '\0';
            check_line(0U, line); check_line(1U, "hello world");
            reset(""); memset(data, 'z', 205U); data_size = 205U; open_reader();
            CHECK(g_text_reader.count == 3U);
            memset(line, 'z', 100U); line[100] = '\0';
            check_line(0U, line); check_line(1U, line); check_line(2U, "zzzzz");
            reset(""); memset(data, 'x', 100U); data[100] = '\n'; data[101] = 'y';
            data_size = 102U; open_reader(); CHECK(g_text_reader.count == 2U);
            check_line(1U, "y");
            reset(""); memset(data, 'a', 50U); data[50] = ' ';
            memset(data + 51U, 'b', 49U); data[100] = ' '; data[101] = 'c';
            data_size = 102U; open_reader(); CHECK(g_text_reader.count == 2U);
            memcpy(line, data, 100U); line[100] = '\0';
            check_line(0U, line); check_line(1U, "c");
            reset("  indented text\n\ttext\n"); open_reader();
            check_line(0U, "  indented text"); check_line(1U, "        text");
        }
        static void test_encoding(void)
        {
            reset("\xEF\xBB\xBF" "Hello \xE2\x80\x9C" "text\xE2\x80\x9D"
                  " \xE2\x80\x94 \xC3\xA9 \xF0\x9F\x98\x80\n");
            open_reader(); check_line(0U, "Hello \"text\" - ? ?");
            reset(""); data[0] = 'a'; data[1] = 0; data[2] = 0x1b; data[3] = 0x7f;
            data[4] = 0xE2; data[5] = 0x82; data_size = 6U;
            open_reader(); check_line(0U, "a????");
            reset("\xFF\xFE" "A"); open_reader();
            CHECK(strstr(g_text_reader.error, "UTF-16") != NULL && live_allocs == 0U);
            draw(); CHECK(drawn_contains("UTF-16"));
            reset("\xFE\xFF" "A"); open_reader();
            CHECK(strstr(g_text_reader.error, "UTF-16") != NULL);
        }
        static void test_limits(void)
        {
            reset(""); memset(data, 'x', TEXT_READER_INPUT_LIMIT + 1U);
            data_size = TEXT_READER_INPUT_LIMIT + 1U; open_reader();
            CHECK(g_text_reader.size == TEXT_READER_INPUT_LIMIT && g_text_reader.truncated);
            CHECK(data_pos == TEXT_READER_INPUT_LIMIT + 1U && closes == 1U);
            draw(); CHECK(drawn_contains("first 256 KiB"));
            reset(""); memset(data, '\n', TEXT_READER_INPUT_LIMIT);
            data_size = TEXT_READER_INPUT_LIMIT; open_reader();
            CHECK(g_text_reader.count == TEXT_READER_INPUT_LIMIT && !g_text_reader.truncated);
            check_line(g_text_reader.count - 1U, "");
            reset(""); memset(data, '\t', TEXT_READER_INPUT_LIMIT);
            data_size = TEXT_READER_INPUT_LIMIT; open_reader();
            CHECK(g_text_reader.size == TEXT_READER_INPUT_LIMIT && g_text_reader.count > 0U);
            for (uint32_t i = 1U; i < g_text_reader.count; ++i)
                CHECK(g_text_reader.lines[i] > g_text_reader.lines[i - 1U]);
        }
        static void test_input_and_empty(void)
        {
            reset("\n"); open_reader(); CHECK(g_text_reader.count == 1U); check_line(0U, "");
            reset(""); open_reader(); CHECK(g_text_reader.count == 0U && live_allocs == 1U);
            key(UI_KEY_DOWN); key(UI_KEY_RIGHT); CHECK(g_text_reader.top == 0U);
            draw(); CHECK(drawn_contains("empty"));
            reset(""); memset(data, '\n', 100U); data_size = 100U; open_reader();
            key(UI_KEY_DOWN); CHECK(g_text_reader.top == 1U);
            key(UI_KEY_PAGE_DOWN); CHECK(g_text_reader.top == 28U);
            key(UI_KEY_RIGHT); CHECK(g_text_reader.top == 55U);
            key(UI_KEY_RIGHT); CHECK(g_text_reader.top == 73U);
            key(UI_KEY_DOWN); CHECK(g_text_reader.top == 73U);
            key(UI_KEY_PAGE_UP); CHECK(g_text_reader.top == 46U);
            key(UI_KEY_LEFT); CHECK(g_text_reader.top == 19U);
            key(UI_KEY_UP); CHECK(g_text_reader.top == 18U);
            key(UI_KEY_LEFT); CHECK(g_text_reader.top == 0U);
            CHECK(config_menu_text_reader_handle_input((ui_input_t){UI_KEY_DOWN, 0U, 0U}));
            CHECK(g_text_reader.top == 0U);
            key(UI_KEY_ENTER); key(UI_KEY_TAB); key(UI_KEY_MENU);
            CHECK(config_menu_text_reader_active() && g_text_reader.top == 0U);
            key(UI_KEY_ESC); CHECK(!config_menu_text_reader_active() && live_allocs == 0U);
            CHECK(!config_menu_text_reader_handle_input((ui_input_t){UI_KEY_DOWN, 1U, 0U}));
            open_reader(); key(UI_KEY_BACK); CHECK(!config_menu_text_reader_active());
        }
        static void boot_key(uint8_t raw, unsigned top, uint8_t ascii)
        {
            ui_input_t input;
            CHECK(boot_menu_map_key(raw, &input));
            CHECK(input.pressed == 1U && input.ascii == ascii);
            CHECK(config_menu_text_reader_handle_input(input));
            if (g_text_reader.top != top) {
                fprintf(stderr, "Boot key $%02X: expected line %u, got %lu\n",
                        raw, top, (unsigned long)g_text_reader.top);
                exit(1);
            }
        }
        static void test_boot_keyboard_navigation(void)
        {
            reset(""); memset(data, '\n', 100U); data_size = 100U; open_reader();
            machine_mode = CARD_MACHINE_MODE_IIE;
            key(UI_KEY_PAGE_DOWN); CHECK(g_text_reader.top == 27U);
            boot_key(0x0BU, 26U, 0U); boot_key(0x0AU, 27U, 0U);
            boot_key(0x8BU, 26U, 0U); boot_key(0x8AU, 27U, 0U);
            boot_key(0x08U, 0U, 0U); boot_key(0x15U, 27U, 0U);
            key(UI_KEY_PAGE_DOWN); CHECK(g_text_reader.top == 54U);
            key(UI_KEY_PAGE_UP); CHECK(g_text_reader.top == 27U);
            machine_mode = CARD_MACHINE_MODE_IIPLUS;
            boot_key('O', 26U, 'O'); boot_key('L', 27U, 'L');
            boot_key('o', 26U, 'o'); boot_key('l', 27U, 'l');
            machine_mode = CARD_MACHINE_MODE_IIE;
            boot_key('O', 27U, 'O'); boot_key('L', 27U, 'L');
        }
        static void test_usb_navigation_bindings(void)
        {
            reset(""); memset(data, '\n', 100U); data_size = 100U; open_reader();
            key(UI_KEY_PAGE_DOWN); CHECK(g_text_reader.top == 27U);
            key(k_usb_binding_keys[CONFIG_MENU_USB_BIND_ACTION_UP]);
            CHECK(g_text_reader.top == 26U);
            key(k_usb_binding_keys[CONFIG_MENU_USB_BIND_ACTION_DOWN]);
            CHECK(g_text_reader.top == 27U);
            key(k_usb_binding_keys[CONFIG_MENU_USB_BIND_ACTION_LEFT]);
            CHECK(g_text_reader.top == 0U);
            key(k_usb_binding_keys[CONFIG_MENU_USB_BIND_ACTION_RIGHT]);
            CHECK(g_text_reader.top == 27U);
        }
        static void test_filesystem_errors(void)
        {
            reset("abc"); first_open_result = FR_NO_FILE; open_reader();
            CHECK(opens == 1U && mounts == 0U && closes == 0U && live_allocs == 0U);
            CHECK(strstr(g_text_reader.error, "Cannot open") != NULL);
            reset("abc"); first_open_result = FR_NOT_ENABLED; open_reader();
            CHECK(opens == 2U && mounts == 1U && refreshes == 1U && closes == 1U);
            check_line(0U, "abc");
            reset("abc"); first_open_result = FR_NOT_READY; mount_result = FR_DISK_ERR;
            open_reader(); CHECK(opens == 1U && refreshes == 1U && closes == 0U && live_allocs == 0U);
            reset("abc"); first_open_result = FR_INVALID_OBJECT; second_open_result = FR_NO_FILE;
            open_reader(); CHECK(opens == 2U && refreshes == 1U && closes == 0U && live_allocs == 0U);
            reset("abc"); read_result = FR_DISK_ERR; open_reader();
            CHECK(closes == 1U && refreshes == 0U && live_allocs == 0U);
            CHECK(strstr(g_text_reader.error, "Cannot read") != NULL);
            reset("abc"); first_open_result = FR_NOT_READY; read_result = FR_DISK_ERR;
            open_reader(); CHECK(closes == 1U && refreshes == 1U && live_allocs == 0U);
            reset("abc"); close_result = FR_DISK_ERR; open_reader();
            CHECK(strstr(g_text_reader.error, "Cannot read") != NULL && live_allocs == 0U);
        }
        static void test_oom_and_cleanup(void)
        {
            reset("abc"); fail_alloc = 1U; open_reader();
            CHECK(opens == 0U && live_allocs == 0U && strstr(g_text_reader.error, "memory"));
            reset("abc"); fail_alloc = 2U; open_reader();
            CHECK(closes == 1U && live_allocs == 0U && strstr(g_text_reader.error, "memory"));
            reset("abc"); open_reader(); CHECK(live_allocs == 2U);
            config_menu_text_reader_open(NULL, "x", "x");
            CHECK(!config_menu_text_reader_active() && live_allocs == 0U);
            config_menu_text_reader_open(&menu, "x", NULL);
            CHECK(!config_menu_text_reader_active() && live_allocs == 0U);
            reset("abc"); open_reader();
            config_menu_text_reader_open(&menu, "Second\r\nname\t.txt", "0:/second.txt");
            CHECK(live_allocs == 2U && strcmp(g_text_reader.name, "Second name .txt") == 0);
            check_line(0U, "abc");
            config_menu_text_reader_close(); config_menu_text_reader_close();
            CHECK(live_allocs == 0U);
        }
        int main(void)
        {
            test_newlines_and_tabs(); test_wrap(); test_encoding(); test_limits();
            test_input_and_empty(); test_filesystem_errors(); test_oom_and_cleanup();
            test_boot_keyboard_navigation(); test_usb_navigation_bindings();
            config_menu_text_reader_close(); CHECK(live_allocs == 0U && file_open == 0U);
            puts("PASS: 9 text-reader behavior groups (formatting, bounds, input, Apple/USB mappings, SD lifecycle, errors)");
            return 0;
        }
    ''')
    BUILD.mkdir(parents=True, exist_ok=True)
    c_path = BUILD / "text_reader_test.c"
    executable = BUILD / "text_reader_test.exe"
    c_path.write_text(preamble + "\n" + input_source + "\n" + source + "\n" + tests,
                      encoding="utf-8")
    subprocess.run([str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror", "-static",
                    str(c_path), "-o", str(executable)], check=True, cwd=ROOT)
    subprocess.run([str(executable)], check=True, cwd=ROOT)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
