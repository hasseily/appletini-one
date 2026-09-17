/* A read-only text panel over the disk browser. The file is closed before
 * viewing starts so menu SD remounts cannot leave another live FatFs handle. */
#include "config_menu_text_reader.h"
#include "config_menu_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEXT_READER_INPUT_LIMIT (256U * 1024U)
#define TEXT_READER_COLUMNS 100U
#define TEXT_READER_PADDING 20
#define TEXT_READER_HEADER_H 100
#define TEXT_READER_FOOTER_H 64
#define TEXT_READER_LINE_H 24

typedef struct {
    char *text;
    uint32_t *lines;
    uint32_t size;
    uint32_t count;
    uint32_t top;
    uint32_t columns;
    uint32_t page_rows;
    uint8_t active;
    uint8_t truncated;
    char name[CONFIG_MENU_PATH_LEN];
    char error[96];
} text_reader_t;

static text_reader_t g_text_reader;

void config_menu_text_reader_close(void)
{
    free(g_text_reader.lines);
    free(g_text_reader.text);
    memset(&g_text_reader, 0, sizeof(g_text_reader));
}

uint8_t config_menu_text_reader_active(void)
{
    return g_text_reader.active;
}

/* The menu font is ASCII. Consume a whole valid UTF-8 character so one
 * unsupported character produces one marker, not one per encoding byte. */
static uint32_t text_reader_character(const unsigned char *src,
                                      uint32_t size,
                                      uint32_t *position)
{
    const uint32_t start = *position;
    const uint32_t first = src[start];
    uint32_t code;
    uint32_t length;
    uint32_t minimum;

    *position = start + 1U;
    if (first < 0x80U) {
        return first;
    }
    if (first >= 0xC2U && first <= 0xDFU) {
        code = first & 0x1FU;
        length = 2U;
        minimum = 0x80U;
    } else if (first >= 0xE0U && first <= 0xEFU) {
        code = first & 0x0FU;
        length = 3U;
        minimum = 0x800U;
    } else if (first >= 0xF0U && first <= 0xF4U) {
        code = first & 0x07U;
        length = 4U;
        minimum = 0x10000U;
    } else {
        return '?';
    }
    if (length > size - start) {
        /* A capped read can end in a UTF-8 character. */
        while (*position < size && (src[*position] & 0xC0U) == 0x80U) {
            ++*position;
        }
        return '?';
    }
    for (uint32_t i = 1U; i < length; ++i) {
        if ((src[start + i] & 0xC0U) != 0x80U) {
            return '?';
        }
        code = (code << 6U) | (src[start + i] & 0x3FU);
    }
    *position = start + length;
    if (code < minimum || code > 0x10FFFFU ||
        (code >= 0xD800U && code <= 0xDFFFU)) {
        return '?';
    }
    return code;
}

static uint32_t text_reader_normalize(char *text, uint32_t size)
{
    uint32_t read_pos = 0U;
    uint32_t write_pos = 0U;
    const unsigned char *bytes = (const unsigned char *)text;

    if (size >= 3U && bytes[0] == 0xEFU && bytes[1] == 0xBBU &&
        bytes[2] == 0xBFU) {
        read_pos = 3U;
    }
    while (read_pos < size) {
        uint32_t code = text_reader_character(bytes, size, &read_pos);

        if (code == '\r') {
            if (read_pos < size && bytes[read_pos] == '\n') {
                ++read_pos;
            }
            code = '\n';
        } else if (code == 0xA0U) {
            code = ' ';
        } else if (code == 0x2018U || code == 0x2019U) {
            code = '\'';
        } else if (code == 0x201CU || code == 0x201DU) {
            code = '"';
        } else if (code == 0x2013U || code == 0x2014U) {
            code = '-';
        }
        if (code != '\n' && code != '\t' &&
            (code < 0x20U || code > 0x7EU)) {
            code = '?';
        }
        text[write_pos++] = (char)code;
    }
    text[write_pos] = '\0';
    return write_pos;
}

/* Return the next visual line's source offset. Tabs stay in the cache and
 * expand here, keeping even a file full of tabs within the input limit. */
static uint32_t text_reader_line(uint32_t start, char *out)
{
    uint32_t pos = start;
    uint32_t column = 0U;
    uint32_t break_pos = 0U;
    uint32_t break_column = 0U;
    uint8_t in_space = 0U;
    uint8_t seen_text = 0U;
    uint8_t wrapped = 0U;

    while (pos < g_text_reader.size) {
        const char ch = g_text_reader.text[pos];
        uint32_t width;

        if (ch == '\n') {
            ++pos;
            break;
        }
        if (column == g_text_reader.columns) {
            if (ch == ' ' || ch == '\t') {
                /* A word ending exactly at the edge already fits. Drop
                 * its separator instead of moving it to the next row. */
                while (pos < g_text_reader.size &&
                       (g_text_reader.text[pos] == ' ' ||
                        g_text_reader.text[pos] == '\t')) {
                    ++pos;
                }
                if (pos < g_text_reader.size && g_text_reader.text[pos] == '\n') {
                    ++pos;
                }
                break;
            }
            wrapped = 1U;
            break;
        }
        width = (ch == '\t') ? (8U - (column % 8U)) : 1U;
        if (width > g_text_reader.columns - column) {
            width = g_text_reader.columns - column;
        }
        if (ch == ' ' || ch == '\t') {
            if (seen_text != 0U) {
                if (in_space == 0U) {
                    break_column = column;
                }
                break_pos = pos + 1U;
            }
            in_space = 1U;
        } else {
            seen_text = 1U;
            in_space = 0U;
        }
        if (out != NULL) {
            memset(out + column, (ch == '\t') ? ' ' : ch, width);
        }
        column += width;
        ++pos;
    }
    if (wrapped != 0U && break_pos != 0U) {
        pos = break_pos;
        column = break_column;
        while (pos < g_text_reader.size &&
               (g_text_reader.text[pos] == ' ' || g_text_reader.text[pos] == '\t')) {
            ++pos;
        }
        /* Do not create a blank row when wrapping drops only end spaces. */
        if (pos < g_text_reader.size && g_text_reader.text[pos] == '\n') {
            ++pos;
        }
    }
    if (out != NULL) {
        out[column] = '\0';
    }
    return pos;
}

static void text_reader_error(const char *message)
{
    free(g_text_reader.lines);
    free(g_text_reader.text);
    g_text_reader.lines = NULL;
    g_text_reader.text = NULL;
    g_text_reader.size = 0U;
    g_text_reader.count = 0U;
    (void)snprintf(g_text_reader.error, sizeof(g_text_reader.error), "%s", message);
}

static void text_reader_file_error(const char *operation, FRESULT result)
{
    char message[96];

    (void)snprintf(message, sizeof(message), "%s (SD error %u).",
                   operation, (unsigned)result);
    text_reader_error(message);
}

void config_menu_text_reader_open(config_menu_t *menu,
                                  const char *name,
                                  const char *path)
{
    FIL file;
    FRESULT result;
    FRESULT close_result;
    uint32_t total = 0U;
    uint8_t remounted = 0U;
    cmui_rect_t body;

    config_menu_text_reader_close();
    if (menu == NULL || path == NULL) {
        return;
    }
    g_text_reader.active = 1U;
    (void)snprintf(g_text_reader.name, sizeof(g_text_reader.name), "%s",
                   (name != NULL) ? name : "Text file");
    (void)text_reader_normalize(g_text_reader.name,
                                 (uint32_t)strlen(g_text_reader.name));
    for (char *ch = g_text_reader.name; *ch != '\0'; ++ch) {
        if (*ch == '\n' || *ch == '\t') {
            *ch = ' ';
        }
    }
    cmui_screen_rects(NULL, &body, NULL);
    g_text_reader.columns = (uint32_t)((body.w - (2 * TEXT_READER_PADDING)) /
                                      (FB16_BUILTIN_FONT_ADVANCE_X * CMUI_BODY_SCALE));
    if (g_text_reader.columns == 0U || g_text_reader.columns > TEXT_READER_COLUMNS) {
        g_text_reader.columns = TEXT_READER_COLUMNS;
    }
    g_text_reader.page_rows = (uint32_t)((body.h - TEXT_READER_HEADER_H -
                                        TEXT_READER_FOOTER_H) / TEXT_READER_LINE_H);
    if (g_text_reader.page_rows == 0U) {
        g_text_reader.page_rows = 1U;
    }
    g_text_reader.text = malloc(TEXT_READER_INPUT_LIMIT + 1U);
    if (g_text_reader.text == NULL) {
        text_reader_error("Not enough memory to open this text file.");
        return;
    }
    result = f_open(&file, path, FA_READ);
    if (result == FR_NOT_ENABLED || result == FR_NOT_READY ||
        result == FR_INVALID_OBJECT) {
        /* A remount invalidates SmartPort handles even if opening fails. */
        remounted = 1U;
        result = config_menu_mount_sd();
        if (result == FR_OK) {
            result = f_open(&file, path, FA_READ);
        }
    }
    if (result != FR_OK) {
        if (remounted != 0U) {
            config_menu_refresh_smartport_media_after_menu_sd(menu);
        }
        text_reader_file_error("Cannot open text file", result);
        return;
    }
    while (total < TEXT_READER_INPUT_LIMIT) {
        UINT got = 0U;
        UINT request = (UINT)(TEXT_READER_INPUT_LIMIT - total);

        if (request > 4096U) {
            request = 4096U;
        }
        result = f_read(&file, g_text_reader.text + total, request, &got);
        total += (uint32_t)got;
        if (result != FR_OK || got < request) {
            break;
        }
    }
    if (result == FR_OK && total == TEXT_READER_INPUT_LIMIT) {
        unsigned char extra;
        UINT got = 0U;

        result = f_read(&file, &extra, 1U, &got);
        g_text_reader.truncated = (got != 0U) ? 1U : 0U;
    }
    close_result = f_close(&file);
    if (remounted != 0U) {
        config_menu_refresh_smartport_media_after_menu_sd(menu);
    }
    if (result != FR_OK || close_result != FR_OK) {
        text_reader_file_error("Cannot read text file",
                               (result != FR_OK) ? result : close_result);
        return;
    }
    if (total >= 2U &&
        (((unsigned char)g_text_reader.text[0] == 0xFFU &&
          (unsigned char)g_text_reader.text[1] == 0xFEU) ||
         ((unsigned char)g_text_reader.text[0] == 0xFEU &&
          (unsigned char)g_text_reader.text[1] == 0xFFU))) {
        text_reader_error("UTF-16 text is not supported. Save this file as UTF-8 or ASCII.");
        return;
    }
    g_text_reader.size = text_reader_normalize(g_text_reader.text, total);
    for (uint32_t pos = 0U; pos < g_text_reader.size;
         pos = text_reader_line(pos, NULL)) {
        ++g_text_reader.count;
    }
    if (g_text_reader.count != 0U) {
        uint32_t pos = 0U;

        g_text_reader.lines = malloc((size_t)g_text_reader.count * sizeof(uint32_t));
        if (g_text_reader.lines == NULL) {
            text_reader_error("Not enough memory to lay out this text file.");
            return;
        }
        for (uint32_t line = 0U; line < g_text_reader.count; ++line) {
            g_text_reader.lines[line] = pos;
            pos = text_reader_line(pos, NULL);
        }
    }
}

uint8_t config_menu_text_reader_handle_input(ui_input_t input)
{
    uint32_t step;
    uint32_t last_top;

    if (g_text_reader.active == 0U) {
        return 0U;
    }
    if (input.pressed == 0U) {
        return 1U;
    }
    if (input.key == UI_KEY_ESC || input.key == UI_KEY_BACK) {
        config_menu_text_reader_close();
        return 1U;
    }
    last_top = (g_text_reader.count > g_text_reader.page_rows) ?
        g_text_reader.count - g_text_reader.page_rows : 0U;
    step = (input.key == UI_KEY_PAGE_UP || input.key == UI_KEY_PAGE_DOWN ||
            input.key == UI_KEY_LEFT || input.key == UI_KEY_RIGHT) ?
        g_text_reader.page_rows : 1U;
    switch (input.key) {
    case UI_KEY_UP:
    case UI_KEY_LEFT:
    case UI_KEY_PAGE_UP:
        g_text_reader.top = (g_text_reader.top > step) ? g_text_reader.top - step : 0U;
        break;
    case UI_KEY_DOWN:
    case UI_KEY_RIGHT:
    case UI_KEY_PAGE_DOWN:
        g_text_reader.top = (step < last_top - g_text_reader.top) ?
            g_text_reader.top + step : last_top;
        break;
    default:
        break;
    }
    return 1U;
}

void config_menu_text_reader_draw(uint16_t *fb, const cmui_rect_t *body)
{
    char line[TEXT_READER_COLUMNS + 1U];
    char position[64];
    uint32_t last;
    int text_x;
    int text_w;
    int footer_y;

    if (g_text_reader.active == 0U || fb == NULL || body == NULL) {
        return;
    }
    text_x = body->x + TEXT_READER_PADDING;
    text_w = body->w - (2 * TEXT_READER_PADDING);
    footer_y = body->y + body->h - TEXT_READER_FOOTER_H;
    cmui_panel(fb, body, CMUI_COLOR_PANEL);
    cmui_text_clipped(fb, text_x, body->y + 18, text_w,
                       g_text_reader.name, CMUI_COLOR_DOCUMENT,
                       CMUI_COLOR_PANEL, CMUI_TITLE_SCALE);
    last = g_text_reader.top + g_text_reader.page_rows;
    if (last > g_text_reader.count) {
        last = g_text_reader.count;
    }
    (void)snprintf(position, sizeof(position), "Lines %lu-%lu of %lu",
                   (unsigned long)((g_text_reader.count != 0U) ? g_text_reader.top + 1U : 0U),
                   (unsigned long)last, (unsigned long)g_text_reader.count);
    cmui_text_clipped(fb, text_x, body->y + 54, text_w, position,
                       CMUI_COLOR_MUTED, CMUI_COLOR_PANEL, CMUI_SMALL_SCALE);
    if (g_text_reader.error[0] != '\0' || g_text_reader.size == 0U) {
        cmui_text_clipped(fb, text_x, body->y + TEXT_READER_HEADER_H, text_w,
                           (g_text_reader.error[0] != '\0') ? g_text_reader.error :
                               "This text file is empty.",
                           CMUI_COLOR_MUTED, CMUI_COLOR_PANEL, CMUI_BODY_SCALE);
    } else {
        for (uint32_t index = g_text_reader.top; index < last; ++index) {
            (void)text_reader_line(g_text_reader.lines[index], line);
            cmui_text_clipped(fb, text_x,
                               body->y + TEXT_READER_HEADER_H +
                                   (int)(index - g_text_reader.top) * TEXT_READER_LINE_H,
                               text_w, line, CMUI_COLOR_TEXT,
                               CMUI_COLOR_PANEL, CMUI_BODY_SCALE);
        }
    }
    if (g_text_reader.truncated != 0U) {
        cmui_text_clipped(fb, text_x, footer_y, text_w,
                           "File truncated: showing the first 256 KiB.",
                           CMUI_COLOR_WARN, CMUI_COLOR_PANEL, CMUI_SMALL_SCALE);
    }
    cmui_text_clipped(fb, text_x, footer_y + 30, text_w,
                       "Up/Down: scroll   Left/Right or PgUp/PgDn: page   Esc: back",
                       CMUI_COLOR_MUTED, CMUI_COLOR_PANEL, CMUI_SMALL_SCALE);
}
