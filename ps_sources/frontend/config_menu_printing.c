/* Printing tab: virtual SSC enable plus the printout browser's file
 * actions. The rename editor and delete confirm draw over the jailed
 * printout browser (config_menu.c routes their input here first). The
 * editor mirrors the profile name editor: raw ASCII when a USB keyboard
 * drives the menu, an on-screen keyboard otherwise. */

#include "config_menu_internal.h"

#include <stdio.h>
#include <string.h>

#include "printer_service.h"

#define CONFIG_PRINTOUT_NAME_MAX 48U
#define CONFIG_PRINTOUT_VK_COLS 10U
#define CONFIG_PRINTOUT_VK_KEY_COUNT 44U
#define CONFIG_PRINTOUT_VK_DELETE 40U
#define CONFIG_PRINTOUT_VK_OK 41U
#define CONFIG_PRINTOUT_VK_CANCEL 42U
#define CONFIG_PRINTOUT_VK_CLEAR 43U
#define CONFIG_PRINTOUT_VK_PANEL_W 1210
#define CONFIG_PRINTOUT_VK_PANEL_H 470
#define CONFIG_PRINTOUT_VK_KEY_W 108
#define CONFIG_PRINTOUT_VK_KEY_H 54
#define CONFIG_PRINTOUT_VK_KEY_GAP 8
#define CONFIG_PRINTOUT_VK_KEY_SCALE 2

static const char * const k_printout_vk_keys[CONFIG_PRINTOUT_VK_KEY_COUNT] = {
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J",
    "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T",
    "U", "V", "W", "X", "Y", "Z", "0", "1", "2", "3",
    "4", "5", "6", "7", "8", "9", "SPACE", "-", "_", ".",
    "DEL", "OK", "CANCEL", "CLEAR"
};

void config_menu_printing_toggle_ssc(config_menu_t *menu)
{
    if (menu == NULL) {
        return;
    }
    menu->ssc_slot1_enabled = menu->ssc_slot1_enabled ? 0U : 1U;
    if (menu->platform.set_ssc_enabled != NULL) {
        menu->platform.set_ssc_enabled(menu->platform.ctx,
                                       menu->ssc_slot1_enabled);
    }
    config_menu_save_settings(menu);
    config_menu_set_status(menu,
                           menu->ssc_slot1_enabled ? 0U : 1U,
                           menu->ssc_slot1_enabled ?
                               "SUPER SERIAL CARD ENABLED IN SLOT 1" :
                               "SUPER SERIAL CARD DISABLED");
}

void config_menu_draw_printing(uint16_t *fb,
                               const config_menu_t *menu,
                               int x,
                               int y,
                               int w)
{
    const int row_h = CMUI_ROW_H + CMUI_ROW_GAP;
    char line[CONFIG_MENU_STATUS_LEN];

    if (menu == NULL) {
        return;
    }

    hgr_draw_check_item(fb, x, y, w,
                        (uint8_t)(menu->item_focus == CONFIG_PRINTING_ITEM_ENABLE),
                        menu->ssc_slot1_enabled,
                        "Enable Super Serial Card (Slot 1, printer)");
    hgr_draw_item(fb, x, y + row_h, w,
                  (uint8_t)(menu->item_focus == CONFIG_PRINTING_ITEM_BROWSE),
                  "Browse Printouts...",
                  HGR_WHITE);

    (void)snprintf(line, sizeof(line),
                   "Pages saved this session: %u",
                   (unsigned)printer_service_pages_saved());
    cmui_caption(fb, x, y + (3 * row_h), w, line);
    if (printer_service_last_file()[0] != '\0') {
        (void)snprintf(line, sizeof(line),
                       "Last printout: %s", printer_service_last_file());
        cmui_caption(fb, x, y + (3 * row_h) + 26, w, line);
    }
    cmui_caption(fb, x, y + (3 * row_h) + 52, w,
                 "Print from the Apple with PR#1. Pages render as PNG "
                 "files in 0:/printouts.");
}

/* ---------- Printout file actions ---------- */

static uint8_t printout_name_char_allowed(uint8_t ch)
{
    if (ch < 0x20U || ch > 0x7EU) {
        return 0U;
    }
    switch (ch) {
    case '"': case '*': case '/': case ':': case '<':
    case '>': case '?': case '\\': case '|':
        return 0U;
    default:
        return 1U;
    }
}

static void printout_editor_append(config_menu_t *menu, uint8_t ch)
{
    size_t len = strlen(menu->printout_editor_text);

    if (printout_name_char_allowed(ch) == 0U ||
        len + 1U >= CONFIG_PRINTOUT_NAME_MAX) {
        return;
    }
    menu->printout_editor_text[len] = (char)ch;
    menu->printout_editor_text[len + 1U] = '\0';
}

static void printout_editor_delete(config_menu_t *menu)
{
    size_t len = strlen(menu->printout_editor_text);

    if (len > 0U) {
        menu->printout_editor_text[len - 1U] = '\0';
    }
}

static void printout_editor_close(config_menu_t *menu)
{
    menu->printout_editor_active = 0U;
    menu->printout_editor_text[0] = '\0';
    menu->printout_action_name[0] = '\0';
    menu->printout_action_path[0] = '\0';
}

void config_menu_printing_start_rename(config_menu_t *menu,
                                       const char *name,
                                       const char *path)
{
    size_t len;

    if (menu == NULL || name == NULL || path == NULL) {
        return;
    }
    (void)snprintf(menu->printout_action_name,
                   sizeof(menu->printout_action_name), "%s", name);
    (void)snprintf(menu->printout_action_path,
                   sizeof(menu->printout_action_path), "%s", path);
    /* Prefill without the .png suffix; the commit puts it back. */
    (void)snprintf(menu->printout_editor_text,
                   sizeof(menu->printout_editor_text), "%s", name);
    len = strlen(menu->printout_editor_text);
    if (len > 4U &&
        strcasecmp(&menu->printout_editor_text[len - 4U], ".png") == 0) {
        menu->printout_editor_text[len - 4U] = '\0';
    }
    if (len >= CONFIG_PRINTOUT_NAME_MAX) {
        menu->printout_editor_text[CONFIG_PRINTOUT_NAME_MAX - 1U] = '\0';
    }
    menu->printout_delete_confirm_active = 0U;
    menu->printout_editor_virtual =
        (menu->usb_bindings_editable == 0U) ? 1U : 0U;
    menu->printout_editor_vk_index = 0U;
    menu->printout_editor_active = 1U;
}

void config_menu_printing_start_delete(config_menu_t *menu,
                                       const char *name,
                                       const char *path)
{
    if (menu == NULL || name == NULL || path == NULL) {
        return;
    }
    (void)snprintf(menu->printout_action_name,
                   sizeof(menu->printout_action_name), "%s", name);
    (void)snprintf(menu->printout_action_path,
                   sizeof(menu->printout_action_path), "%s", path);
    menu->printout_editor_active = 0U;
    menu->printout_delete_confirm_active = 1U;
}

static void printout_status_fresult(config_menu_t *menu,
                                    const char *prefix,
                                    FRESULT fr)
{
    char text[CONFIG_MENU_STATUS_LEN];

    (void)snprintf(text, sizeof(text), "%s FR=%u", prefix, (unsigned)fr);
    config_menu_set_status(menu, 1U, text);
}

static void printout_delete_commit(config_menu_t *menu)
{
    FRESULT fr;
    char text[CONFIG_MENU_STATUS_LEN];

    menu->printout_delete_confirm_active = 0U;
    fr = config_menu_mount_sd();
    if (fr == FR_OK) {
        fr = f_unlink(menu->printout_action_path);
    }
    if (fr != FR_OK) {
        printout_status_fresult(menu, "PRINTOUT DELETE FAILED", fr);
    } else {
        (void)snprintf(text, sizeof(text), "DELETED %.60s",
                       menu->printout_action_name);
        config_menu_set_status(menu, 0U, text);
    }
    config_menu_browser_reload_after_fileop(menu);
    menu->printout_action_name[0] = '\0';
    menu->printout_action_path[0] = '\0';
}

static void printout_rename_commit(config_menu_t *menu)
{
    char new_name[CONFIG_MENU_PATH_LEN];
    char new_path[CONFIG_MENU_PATH_LEN];
    char dir[CONFIG_MENU_PATH_LEN];
    const char *base;
    size_t len;
    size_t dir_len;
    FILINFO info;
    FRESULT fr;
    char text[CONFIG_MENU_STATUS_LEN];

    /* Trim trailing spaces/dots, then restore the .png suffix. */
    (void)snprintf(new_name, sizeof(new_name), "%s",
                   menu->printout_editor_text);
    len = strlen(new_name);
    while (len > 0U && (new_name[len - 1U] == ' ' || new_name[len - 1U] == '.')) {
        new_name[--len] = '\0';
    }
    if (len == 0U) {
        config_menu_set_status(menu, 1U, "PRINTOUT NAME IS EMPTY");
        return;
    }
    if (len + 4U >= sizeof(new_name)) {
        config_menu_set_status(menu, 1U, "PRINTOUT NAME TOO LONG");
        return;
    }
    (void)snprintf(&new_name[len], sizeof(new_name) - len, ".png");

    /* Directory of the original file. */
    base = config_menu_basename(menu->printout_action_path);
    dir_len = (size_t)(base - menu->printout_action_path);
    if (dir_len > 0U && dir_len < sizeof(dir)) {
        memcpy(dir, menu->printout_action_path, dir_len);
        while (dir_len > 1U && dir[dir_len - 1U] == '/') {
            dir_len--;
        }
        dir[dir_len] = '\0';
    } else {
        (void)snprintf(dir, sizeof(dir), "%s", PRINTER_SERVICE_DIR);
    }
    if ((size_t)snprintf(new_path, sizeof(new_path), "%s/%s", dir, new_name) >=
        sizeof(new_path)) {
        config_menu_set_status(menu, 1U, "PRINTOUT NAME TOO LONG");
        return;
    }

    if (strcasecmp(new_path, menu->printout_action_path) == 0) {
        printout_editor_close(menu);
        return;
    }

    fr = config_menu_mount_sd();
    if (fr == FR_OK) {
        fr = f_stat(new_path, &info);
        if (fr == FR_OK) {
            config_menu_set_status(menu, 1U, "PRINTOUT NAME EXISTS");
            return;
        }
        if (fr != FR_NO_FILE) {
            printout_status_fresult(menu, "PRINTOUT RENAME FAILED", fr);
            return;
        }
        fr = f_rename(menu->printout_action_path, new_path);
    }
    if (fr != FR_OK) {
        printout_status_fresult(menu, "PRINTOUT RENAME FAILED", fr);
        return;
    }
    (void)snprintf(text, sizeof(text), "RENAMED TO %.60s", new_name);
    config_menu_set_status(menu, 0U, text);
    printout_editor_close(menu);
    config_menu_browser_reload_after_fileop(menu);
}

static void printout_vk_move(config_menu_t *menu, int8_t dx, int8_t dy)
{
    uint8_t row = (uint8_t)(menu->printout_editor_vk_index /
                            CONFIG_PRINTOUT_VK_COLS);
    uint8_t col = (uint8_t)(menu->printout_editor_vk_index %
                            CONFIG_PRINTOUT_VK_COLS);
    uint8_t next;

    if (dx < 0) {
        col = (col == 0U) ? (CONFIG_PRINTOUT_VK_COLS - 1U) : (uint8_t)(col - 1U);
    } else if (dx > 0) {
        col = (uint8_t)((col + 1U) % CONFIG_PRINTOUT_VK_COLS);
    }
    if (dy < 0) {
        row = (row == 0U) ?
            (uint8_t)((CONFIG_PRINTOUT_VK_KEY_COUNT - 1U) /
                      CONFIG_PRINTOUT_VK_COLS) :
            (uint8_t)(row - 1U);
    } else if (dy > 0) {
        row = (uint8_t)(row + 1U);
        if ((uint32_t)row * CONFIG_PRINTOUT_VK_COLS >=
            CONFIG_PRINTOUT_VK_KEY_COUNT) {
            row = 0U;
        }
    }
    next = (uint8_t)((uint32_t)row * CONFIG_PRINTOUT_VK_COLS + col);
    while (next >= CONFIG_PRINTOUT_VK_KEY_COUNT) {
        next = (next >= CONFIG_PRINTOUT_VK_COLS) ?
            (uint8_t)(next - CONFIG_PRINTOUT_VK_COLS) : 0U;
    }
    menu->printout_editor_vk_index = next;
}

static void printout_vk_select(config_menu_t *menu)
{
    const uint8_t index = menu->printout_editor_vk_index;

    if (index < 26U) {
        printout_editor_append(menu, (uint8_t)('A' + index));
    } else if (index < 36U) {
        printout_editor_append(menu, (uint8_t)('0' + (index - 26U)));
    } else if (index == 36U) {
        printout_editor_append(menu, ' ');
    } else if (index == 37U) {
        printout_editor_append(menu, '-');
    } else if (index == 38U) {
        printout_editor_append(menu, '_');
    } else if (index == 39U) {
        printout_editor_append(menu, '.');
    } else if (index == CONFIG_PRINTOUT_VK_DELETE) {
        printout_editor_delete(menu);
    } else if (index == CONFIG_PRINTOUT_VK_OK) {
        printout_rename_commit(menu);
    } else if (index == CONFIG_PRINTOUT_VK_CANCEL) {
        printout_editor_close(menu);
    } else if (index == CONFIG_PRINTOUT_VK_CLEAR) {
        menu->printout_editor_text[0] = '\0';
    }
}

uint8_t config_menu_printing_handle_input(config_menu_t *menu,
                                          ui_input_t input)
{
    if (menu == NULL || input.pressed == 0U) {
        return 0U;
    }

    if (menu->printout_delete_confirm_active != 0U) {
        switch (input.key) {
        case UI_KEY_ENTER:
            printout_delete_commit(menu);
            return 1U;
        case UI_KEY_BACK:
        case UI_KEY_ESC:
            menu->printout_delete_confirm_active = 0U;
            menu->printout_action_name[0] = '\0';
            menu->printout_action_path[0] = '\0';
            return 1U;
        default:
            return 1U;
        }
    }

    if (menu->printout_editor_active == 0U) {
        return 0U;
    }

    if (menu->printout_editor_virtual != 0U) {
        switch (input.key) {
        case UI_KEY_LEFT:
            printout_vk_move(menu, -1, 0);
            return 1U;
        case UI_KEY_RIGHT:
            printout_vk_move(menu, 1, 0);
            return 1U;
        case UI_KEY_UP:
        case UI_KEY_PAGE_UP:
            printout_vk_move(menu, 0, -1);
            return 1U;
        case UI_KEY_DOWN:
        case UI_KEY_PAGE_DOWN:
            printout_vk_move(menu, 0, 1);
            return 1U;
        case UI_KEY_ENTER:
            printout_vk_select(menu);
            return 1U;
        case UI_KEY_BACK:
            if (menu->printout_editor_text[0] != '\0') {
                printout_editor_delete(menu);
            } else {
                printout_editor_close(menu);
            }
            return 1U;
        case UI_KEY_ESC:
            printout_editor_close(menu);
            return 1U;
        default:
            return 1U;
        }
    }

    if (input.ascii != 0U) {
        printout_editor_append(menu, input.ascii);
        return 1U;
    }

    switch (input.key) {
    case UI_KEY_ENTER:
        printout_rename_commit(menu);
        return 1U;
    case UI_KEY_BACK:
    case UI_KEY_LEFT:
        if (menu->printout_editor_text[0] != '\0') {
            printout_editor_delete(menu);
        } else {
            printout_editor_close(menu);
        }
        return 1U;
    case UI_KEY_ESC:
        printout_editor_close(menu);
        return 1U;
    default:
        return 1U;
    }
}

static void printout_fit_text(char *dst, size_t dst_len,
                              const char *src, size_t max_chars)
{
    size_t len = strlen(src);

    if (len > max_chars && max_chars < dst_len) {
        (void)snprintf(dst, dst_len, "%.*s...",
                       (int)(max_chars - 3U), src);
        return;
    }
    (void)snprintf(dst, dst_len, "%s", src);
}

static void printout_draw_delete_confirm(uint16_t *fb,
                                         const config_menu_t *menu,
                                         int x,
                                         int y,
                                         int w)
{
    const int panel_w = 760;
    const int panel_h = 190;
    const int panel_x = x + ((w - panel_w) / 2);
    const int panel_y = y + 120;
    char line[96];

    fb16_fill_rect(fb, panel_x, panel_y, panel_w, panel_h, CMUI_COLOR_PANEL);
    fb16_rect(fb, panel_x, panel_y, panel_w, panel_h, CMUI_COLOR_WARN);
    cmui_text(fb, panel_x + 20, panel_y + 18, "DELETE PRINTOUT?",
              CMUI_COLOR_WARN, CMUI_COLOR_PANEL, CMUI_BODY_SCALE);
    printout_fit_text(line, sizeof(line), menu->printout_action_name, 44U);
    cmui_text_clipped(fb, panel_x + 20, panel_y + 70, panel_w - 40,
                      line, CMUI_COLOR_TEXT, CMUI_COLOR_PANEL,
                      CMUI_BODY_SCALE);
    cmui_text(fb, panel_x + 20, panel_y + 130,
              "ENTER = Delete    ESC = Cancel",
              CMUI_COLOR_MUTED, CMUI_COLOR_PANEL, CMUI_BODY_SCALE);
}

static void printout_draw_editor(uint16_t *fb,
                                 const config_menu_t *menu,
                                 int x,
                                 int y,
                                 int w)
{
    const int panel_w = (menu->printout_editor_virtual != 0U) ?
        CONFIG_PRINTOUT_VK_PANEL_W : 760;
    const int panel_h = (menu->printout_editor_virtual != 0U) ?
        CONFIG_PRINTOUT_VK_PANEL_H : 180;
    const int panel_x = x + ((w - panel_w) / 2);
    const int panel_y = y + 10;
    char display[80];

    fb16_fill_rect(fb, panel_x, panel_y, panel_w, panel_h, CMUI_COLOR_PANEL);
    fb16_rect(fb, panel_x, panel_y, panel_w, panel_h, CMUI_COLOR_BORDER);
    cmui_text(fb, panel_x + 20, panel_y + 18, "RENAME PRINTOUT",
              CMUI_COLOR_WARN, CMUI_COLOR_PANEL, CMUI_BODY_SCALE);

    printout_fit_text(display, sizeof(display),
                      (menu->printout_editor_text[0] != '\0') ?
                          menu->printout_editor_text : "[name]",
                      42U);
    fb16_fill_rect(fb, panel_x + 20, panel_y + 58, panel_w - 40, 44,
                   CMUI_COLOR_ROW);
    fb16_rect(fb, panel_x + 20, panel_y + 58, panel_w - 40, 44,
              CMUI_COLOR_ACCENT);
    cmui_text_clipped(fb, panel_x + 34, panel_y + 70, panel_w - 68,
                      display,
                      (menu->printout_editor_text[0] != '\0') ?
                          CMUI_COLOR_TEXT : CMUI_COLOR_DIM,
                      CMUI_COLOR_ROW,
                      CMUI_BODY_SCALE);

    if (menu->printout_editor_virtual == 0U) {
        return;
    }

    for (uint32_t i = 0U; i < CONFIG_PRINTOUT_VK_KEY_COUNT; ++i) {
        const uint32_t row = i / CONFIG_PRINTOUT_VK_COLS;
        const uint32_t col = i % CONFIG_PRINTOUT_VK_COLS;
        const int key_w = CONFIG_PRINTOUT_VK_KEY_W;
        const int key_h = CONFIG_PRINTOUT_VK_KEY_H;
        const int key_gap = CONFIG_PRINTOUT_VK_KEY_GAP;
        const int key_scale = CONFIG_PRINTOUT_VK_KEY_SCALE;
        const int grid_w =
            ((int)CONFIG_PRINTOUT_VK_COLS * key_w) +
            (((int)CONFIG_PRINTOUT_VK_COLS - 1) * key_gap);
        const int key_x = panel_x + ((panel_w - grid_w) / 2) +
            (int)col * (key_w + key_gap);
        const int key_y = panel_y + 126 + (int)row * (key_h + key_gap);
        const uint8_t focused =
            (uint8_t)(i == menu->printout_editor_vk_index);
        const uint32_t bg = (focused != 0U) ?
            CMUI_COLOR_ROW_ACTIVE : CMUI_COLOR_ROW;
        const uint32_t fg = (focused != 0U) ?
            CMUI_COLOR_ACCENT : CMUI_COLOR_TEXT;
        const char *label = k_printout_vk_keys[i];
        const int text_w =
            (int)strlen(label) * FB16_BUILTIN_FONT_ADVANCE_X * key_scale;
        const int text_h = FB16_BUILTIN_FONT_HEIGHT * key_scale;

        fb16_fill_rect(fb, key_x, key_y, key_w, key_h, bg);
        fb16_rect(fb, key_x, key_y, key_w, key_h,
                  focused ? CMUI_COLOR_ACCENT : CMUI_COLOR_BORDER_SOFT);
        fb16_string_scaled(fb,
                           key_x + ((key_w - text_w) / 2),
                           key_y + ((key_h - text_h) / 2),
                           label,
                           fg,
                           bg,
                           key_scale);
    }
}

void config_menu_printing_draw_overlays(uint16_t *fb,
                                        const config_menu_t *menu,
                                        int x,
                                        int y,
                                        int w)
{
    if (menu == NULL) {
        return;
    }
    if (menu->printout_delete_confirm_active != 0U) {
        printout_draw_delete_confirm(fb, menu, x, y, w);
    } else if (menu->printout_editor_active != 0U) {
        printout_draw_editor(fb, menu, x, y, w);
    }
}
