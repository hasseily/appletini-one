#include "config_menu_internal.h"

#include <stdio.h>
#include <string.h>

#include "profile_manager.h"

#define CONFIG_PROFILE_MAX_ENTRIES 64U
#define CONFIG_PROFILE_THUMB_CACHE_COUNT 3U
#define CONFIG_PROFILE_NAME_MODE_SAVE_AS 1U
#define CONFIG_PROFILE_NAME_MODE_RENAME 2U
#define CONFIG_PROFILE_NAME_MAX 48U
#define CONFIG_PROFILE_VK_COLS 10U
#define CONFIG_PROFILE_VK_KEY_COUNT 44U
#define CONFIG_PROFILE_VK_DELETE 40U
#define CONFIG_PROFILE_VK_OK 41U
#define CONFIG_PROFILE_VK_CANCEL 42U
#define CONFIG_PROFILE_VK_CLEAR 43U
#define CONFIG_PROFILE_VK_PANEL_W 1210
#define CONFIG_PROFILE_VK_PANEL_H 470
#define CONFIG_PROFILE_VK_KEY_W 108
#define CONFIG_PROFILE_VK_KEY_H 54
#define CONFIG_PROFILE_VK_KEY_GAP 8
#define CONFIG_PROFILE_VK_KEY_SCALE 2

typedef enum {
    CONFIG_PROFILE_UI_PARENT = 0,
    CONFIG_PROFILE_UI_EMPTY,
    CONFIG_PROFILE_UI_FOLDER,
    CONFIG_PROFILE_UI_PROFILE
} config_profile_ui_type_t;

typedef struct {
    config_profile_ui_type_t type;
    char name[CONFIG_MENU_PATH_LEN];
    char path[CONFIG_MENU_PATH_LEN];
    char cfg_path[CONFIG_MENU_PATH_LEN];
    char thumb_path[CONFIG_MENU_PATH_LEN];
} config_profile_ui_entry_t;

typedef struct {
    uint8_t loaded;
    uint8_t valid;
    char path[CONFIG_MENU_PATH_LEN];
    uint32_t *pixels;
    unsigned w;
    unsigned h;
} config_profile_thumb_cache_t;

static config_profile_ui_entry_t g_profile_entries[CONFIG_PROFILE_MAX_ENTRIES];
static config_profile_thumb_cache_t g_profile_thumb_cache[CONFIG_PROFILE_THUMB_CACHE_COUNT];
static const char * const k_profile_vk_keys[CONFIG_PROFILE_VK_KEY_COUNT] = {
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J",
    "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T",
    "U", "V", "W", "X", "Y", "Z", "0", "1", "2", "3",
    "4", "5", "6", "7", "8", "9", "SPACE", "-", "_", ".",
    "DEL", "OK", "CANCEL", "CLEAR"
};

static void profiles_copy_text(char *dst, size_t dst_len, const char *src)
{
    if (dst == NULL || dst_len == 0U) {
        return;
    }
    if (src == NULL) {
        src = "";
    }
    (void)snprintf(dst, dst_len, "%s", src);
}

static void profile_thumb_cache_clear_slot(uint32_t slot)
{
    config_profile_thumb_cache_t *cache;

    if (slot >= CONFIG_PROFILE_THUMB_CACHE_COUNT) {
        return;
    }

    cache = &g_profile_thumb_cache[slot];
    if (cache->pixels != NULL) {
        profile_manager_free_bgra32(cache->pixels);
    }
    memset(cache, 0, sizeof(*cache));
}

static void profile_thumb_cache_clear_all(void)
{
    for (uint32_t i = 0U; i < CONFIG_PROFILE_THUMB_CACHE_COUNT; ++i) {
        profile_thumb_cache_clear_slot(i);
    }
}

static config_profile_ui_entry_t *profile_entry_at(const config_menu_t *menu,
                                                   uint16_t index)
{
    if (menu == NULL || index >= menu->profile_count ||
        index >= CONFIG_PROFILE_MAX_ENTRIES) {
        return NULL;
    }
    return &g_profile_entries[index];
}

static void profile_add_entry(config_menu_t *menu,
                              config_profile_ui_type_t type,
                              const char *name,
                              const char *path,
                              const char *cfg_path,
                              const char *thumb_path)
{
    config_profile_ui_entry_t *entry;

    if (menu == NULL || menu->profile_count >= CONFIG_PROFILE_MAX_ENTRIES) {
        return;
    }

    entry = &g_profile_entries[menu->profile_count++];
    memset(entry, 0, sizeof(*entry));
    entry->type = type;
    profiles_copy_text(entry->name, sizeof(entry->name), name);
    profiles_copy_text(entry->path, sizeof(entry->path), path);
    profiles_copy_text(entry->cfg_path, sizeof(entry->cfg_path), cfg_path);
    profiles_copy_text(entry->thumb_path, sizeof(entry->thumb_path), thumb_path);
}

static void profile_refresh_carousel(config_menu_t *menu)
{
    profile_manager_entry_t entries[CONFIG_PROFILE_MAX_ENTRIES];
    uint16_t child_count = 0U;
    FRESULT fr;

    if (menu == NULL) {
        return;
    }
    if (menu->profile_dir[0] == '\0') {
        profiles_copy_text(menu->profile_dir,
                           sizeof(menu->profile_dir),
                           PROFILE_MANAGER_ROOT);
    }

    menu->profile_count = 0U;
    profile_thumb_cache_clear_all();

    fr = profile_manager_ensure_root();
    if (fr != FR_OK) {
        config_menu_set_status(menu, 1U, "PROFILE ROOT UNAVAILABLE");
        return;
    }

    if (profile_manager_is_root(menu->profile_dir) == 0U) {
        char parent[CONFIG_MENU_PATH_LEN];

        (void)profile_manager_parent_path(menu->profile_dir, parent, sizeof(parent));
        profile_add_entry(menu, CONFIG_PROFILE_UI_PARENT, "[..]", parent, "", "");
    }

    fr = profile_manager_list_dir(menu->profile_dir,
                                  entries,
                                  CONFIG_PROFILE_MAX_ENTRIES,
                                  &child_count);
    if (fr != FR_OK) {
        config_menu_set_status(menu, 1U, "PROFILE FOLDER READ FAILED");
        return;
    }

    for (uint16_t i = 0U;
         i < child_count && menu->profile_count < CONFIG_PROFILE_MAX_ENTRIES;
         ++i) {
        profile_add_entry(
            menu,
            (entries[i].type == PROFILE_MANAGER_ENTRY_PROFILE) ?
                CONFIG_PROFILE_UI_PROFILE : CONFIG_PROFILE_UI_FOLDER,
            entries[i].name,
            entries[i].path,
            entries[i].cfg_path,
            entries[i].thumb_path);
    }

    if (menu->profile_count == 0U) {
        profile_add_entry(menu, CONFIG_PROFILE_UI_EMPTY, "[NO PROFILES]", "", "", "");
    }
    if (menu->profile_selected >= menu->profile_count) {
        menu->profile_selected = (menu->profile_count == 0U) ?
            0U : (uint16_t)(menu->profile_count - 1U);
    }
    config_menu_refresh_smartport_media_after_menu_sd(menu);
}

void config_menu_profiles_init(config_menu_t *menu)
{
    if (menu == NULL) {
        return;
    }

    menu->profile_carousel_active = 0U;
    menu->profile_selected = 0U;
    menu->profile_count = 0U;
    profiles_copy_text(menu->profile_dir,
                           sizeof(menu->profile_dir),
                           PROFILE_MANAGER_ROOT);
    menu->profile_source_dir[0] = '\0';
    menu->profile_name_editor_active = 0U;
    menu->profile_name_editor_mode = 0U;
    menu->profile_name_editor_virtual = 0U;
    menu->profile_name_editor_vk_index = 0U;
    menu->profile_name_editor_text[0] = '\0';
    menu->profile_name_editor_target_dir[0] = '\0';
}

void config_menu_profiles_open_carousel(config_menu_t *menu)
{
    if (menu == NULL) {
        return;
    }

    if (menu->profile_dir[0] == '\0') {
        profiles_copy_text(menu->profile_dir,
                           sizeof(menu->profile_dir),
                           PROFILE_MANAGER_ROOT);
    }
    menu->profile_carousel_active = 1U;
    profile_refresh_carousel(menu);
}

static void profile_move(config_menu_t *menu, int8_t delta)
{
    if (menu == NULL || menu->profile_count == 0U) {
        return;
    }
    if (delta < 0) {
        menu->profile_selected = (menu->profile_selected == 0U) ?
            (uint16_t)(menu->profile_count - 1U) :
            (uint16_t)(menu->profile_selected - 1U);
    } else {
        menu->profile_selected =
            (uint16_t)((menu->profile_selected + 1U) % menu->profile_count);
    }
}

static void profile_select(config_menu_t *menu)
{
    config_profile_ui_entry_t *entry;

    if (menu == NULL) {
        return;
    }
    entry = profile_entry_at(menu, menu->profile_selected);
    if (entry == NULL) {
        return;
    }

    if (entry->type == CONFIG_PROFILE_UI_EMPTY) {
        config_menu_set_status(menu, 1U, "NO PROFILES IN THIS FOLDER");
    } else if (entry->type == CONFIG_PROFILE_UI_PARENT ||
               entry->type == CONFIG_PROFILE_UI_FOLDER) {
        profiles_copy_text(menu->profile_dir, sizeof(menu->profile_dir), entry->path);
        menu->profile_selected = 0U;
        profile_refresh_carousel(menu);
    } else if (entry->type == CONFIG_PROFILE_UI_PROFILE) {
        if (config_menu_load_profile_settings(menu, entry->path) != 0U) {
            menu->profile_carousel_active = 0U;
        }
    }
}

static void profile_name_trim_copy(char *dst, size_t dst_len, const char *src)
{
    const char *start;
    size_t len;

    if (dst == NULL || dst_len == 0U) {
        return;
    }
    if (src == NULL) {
        src = "";
    }
    start = src;
    while (*start == ' ') {
        ++start;
    }
    len = strlen(start);
    while (len > 0U && start[len - 1U] == ' ') {
        --len;
    }
    if (len >= dst_len) {
        len = dst_len - 1U;
    }
    memcpy(dst, start, len);
    dst[len] = '\0';
}

static uint8_t profile_name_char_allowed(uint8_t ascii)
{
    switch (ascii) {
    case '"':
    case '*':
    case '/':
    case ':':
    case '<':
    case '>':
    case '?':
    case '\\':
    case '|':
        return 0U;
    default:
        break;
    }
    return (ascii >= 0x20U && ascii <= 0x7EU) ? 1U : 0U;
}

static void profile_name_editor_start(config_menu_t *menu,
                                      uint8_t mode,
                                      const char *initial_name,
                                      const char *target_dir)
{
    if (menu == NULL) {
        return;
    }

    menu->profile_carousel_active = 0U;
    menu->profile_name_editor_active = 1U;
    menu->profile_name_editor_mode = mode;
    menu->profile_name_editor_virtual =
        (menu->usb_bindings_editable == 0U) ? 1U : 0U;
    menu->profile_name_editor_vk_index = 0U;
    profile_name_trim_copy(menu->profile_name_editor_text,
                           sizeof(menu->profile_name_editor_text),
                           initial_name);
    profiles_copy_text(menu->profile_name_editor_target_dir,
                       sizeof(menu->profile_name_editor_target_dir),
                       target_dir);
    config_menu_set_status(menu,
                           0U,
                           (mode == CONFIG_PROFILE_NAME_MODE_RENAME) ?
                               "RENAME PROFILE" : "SAVE AS PROFILE");
}

static void profile_name_editor_cancel(config_menu_t *menu)
{
    if (menu == NULL) {
        return;
    }
    menu->profile_name_editor_active = 0U;
    menu->profile_name_editor_mode = 0U;
    menu->profile_name_editor_text[0] = '\0';
    menu->profile_name_editor_target_dir[0] = '\0';
    config_menu_set_status(menu, 0U, "PROFILE NAME CANCELLED");
}

static void profile_name_editor_delete(config_menu_t *menu)
{
    size_t len;

    if (menu == NULL) {
        return;
    }
    len = strlen(menu->profile_name_editor_text);
    if (len > 0U) {
        menu->profile_name_editor_text[len - 1U] = '\0';
    }
}

static void profile_name_editor_append(config_menu_t *menu, uint8_t ascii)
{
    size_t len;

    if (menu == NULL || profile_name_char_allowed(ascii) == 0U) {
        return;
    }
    len = strlen(menu->profile_name_editor_text);
    if (len >= (CONFIG_PROFILE_NAME_MAX - 1U) ||
        len >= (sizeof(menu->profile_name_editor_text) - 1U)) {
        config_menu_set_status(menu, 1U, "PROFILE NAME TOO LONG");
        return;
    }
    menu->profile_name_editor_text[len] = (char)ascii;
    menu->profile_name_editor_text[len + 1U] = '\0';
}

static void profile_name_editor_status_fresult(config_menu_t *menu,
                                               const char *prefix,
                                               FRESULT fr)
{
    char text[CONFIG_MENU_STATUS_LEN];

    if (fr == FR_EXIST) {
        config_menu_set_status(menu, 1U, "PROFILE NAME EXISTS");
        return;
    }
    if (fr == FR_INVALID_NAME) {
        config_menu_set_status(menu, 1U, "INVALID PROFILE NAME");
        return;
    }
    (void)snprintf(text,
                   sizeof(text),
                   "%.70s FR=%u",
                   (prefix != NULL) ? prefix : "PROFILE ERROR",
                   (unsigned)fr);
    config_menu_set_status(menu, 1U, text);
}

static void profile_name_editor_commit(config_menu_t *menu)
{
    char name[CONFIG_MENU_PATH_LEN];
    char new_dir[CONFIG_MENU_PATH_LEN];
    FRESULT fr;

    if (menu == NULL || menu->profile_name_editor_active == 0U) {
        return;
    }

    profile_name_trim_copy(name, sizeof(name), menu->profile_name_editor_text);
    if (profile_manager_profile_name_valid(name) == 0U) {
        config_menu_set_status(menu, 1U, "INVALID PROFILE NAME");
        return;
    }

    if (menu->profile_name_editor_mode == CONFIG_PROFILE_NAME_MODE_RENAME) {
        if (menu->profile_name_editor_target_dir[0] == '\0') {
            config_menu_set_status(menu, 1U, "SELECT A PROFILE FIRST");
            return;
        }
        fr = profile_manager_rename_profile(menu->profile_name_editor_target_dir,
                                            name,
                                            new_dir,
                                            sizeof(new_dir));
        if (fr != FR_OK) {
            profile_name_editor_status_fresult(menu, "PROFILE RENAME FAILED", fr);
            return;
        }
        profiles_copy_text(menu->profile_source_dir,
                           sizeof(menu->profile_source_dir),
                           new_dir);
        profiles_copy_text(menu->profile_name_editor_target_dir,
                           sizeof(menu->profile_name_editor_target_dir),
                           new_dir);
        config_menu_set_status(menu, 0U, "PROFILE RENAMED");
    } else {
        if (menu->profile_dir[0] == '\0') {
            profiles_copy_text(menu->profile_dir,
                               sizeof(menu->profile_dir),
                               PROFILE_MANAGER_ROOT);
        }
        fr = profile_manager_create_profile(menu->profile_dir,
                                            name,
                                            new_dir,
                                            sizeof(new_dir));
        if (fr != FR_OK) {
            profile_name_editor_status_fresult(menu, "PROFILE CREATE FAILED", fr);
            return;
        }
        if (config_menu_save_profile_settings(menu, new_dir) == 0U) {
            return;
        }
        profiles_copy_text(menu->profile_source_dir,
                           sizeof(menu->profile_source_dir),
                           new_dir);
        config_menu_set_status(menu, 0U, "PROFILE SAVED");
    }

    menu->profile_name_editor_active = 0U;
    menu->profile_name_editor_mode = 0U;
    menu->profile_name_editor_text[0] = '\0';
    menu->profile_name_editor_target_dir[0] = '\0';
    profile_refresh_carousel(menu);
}

static void profile_vk_move(config_menu_t *menu, int8_t dx, int8_t dy)
{
    uint8_t row;
    uint8_t col;
    uint8_t next;

    if (menu == NULL) {
        return;
    }

    row = (uint8_t)(menu->profile_name_editor_vk_index / CONFIG_PROFILE_VK_COLS);
    col = (uint8_t)(menu->profile_name_editor_vk_index % CONFIG_PROFILE_VK_COLS);
    if (dx < 0) {
        col = (col == 0U) ? (CONFIG_PROFILE_VK_COLS - 1U) : (uint8_t)(col - 1U);
    } else if (dx > 0) {
        col = (uint8_t)((col + 1U) % CONFIG_PROFILE_VK_COLS);
    }
    if (dy < 0) {
        row = (row == 0U) ?
            (uint8_t)((CONFIG_PROFILE_VK_KEY_COUNT - 1U) / CONFIG_PROFILE_VK_COLS) :
            (uint8_t)(row - 1U);
    } else if (dy > 0) {
        row = (uint8_t)(row + 1U);
        if ((uint32_t)row * CONFIG_PROFILE_VK_COLS >= CONFIG_PROFILE_VK_KEY_COUNT) {
            row = 0U;
        }
    }
    next = (uint8_t)((uint32_t)row * CONFIG_PROFILE_VK_COLS + col);
    while (next >= CONFIG_PROFILE_VK_KEY_COUNT) {
        next = (next >= CONFIG_PROFILE_VK_COLS) ?
            (uint8_t)(next - CONFIG_PROFILE_VK_COLS) : 0U;
    }
    menu->profile_name_editor_vk_index = next;
}

static void profile_vk_select(config_menu_t *menu)
{
    const uint8_t index = (menu != NULL) ? menu->profile_name_editor_vk_index : 0U;

    if (menu == NULL) {
        return;
    }
    if (index < 26U) {
        profile_name_editor_append(menu, (uint8_t)('A' + index));
    } else if (index < 36U) {
        profile_name_editor_append(menu, (uint8_t)('0' + (index - 26U)));
    } else if (index == 36U) {
        profile_name_editor_append(menu, ' ');
    } else if (index == 37U) {
        profile_name_editor_append(menu, '-');
    } else if (index == 38U) {
        profile_name_editor_append(menu, '_');
    } else if (index == 39U) {
        profile_name_editor_append(menu, '.');
    } else if (index == CONFIG_PROFILE_VK_DELETE) {
        profile_name_editor_delete(menu);
    } else if (index == CONFIG_PROFILE_VK_OK) {
        profile_name_editor_commit(menu);
    } else if (index == CONFIG_PROFILE_VK_CANCEL) {
        profile_name_editor_cancel(menu);
    } else if (index == CONFIG_PROFILE_VK_CLEAR) {
        menu->profile_name_editor_text[0] = '\0';
    }
}

static uint8_t profile_name_editor_handle_input(config_menu_t *menu,
                                                ui_input_t input)
{
    if (menu == NULL || menu->profile_name_editor_active == 0U ||
        input.pressed == 0U) {
        return 0U;
    }

    if (menu->profile_name_editor_virtual != 0U) {
        switch (input.key) {
        case UI_KEY_LEFT:
            profile_vk_move(menu, -1, 0);
            return 1U;
        case UI_KEY_RIGHT:
            profile_vk_move(menu, 1, 0);
            return 1U;
        case UI_KEY_UP:
        case UI_KEY_PAGE_UP:
            profile_vk_move(menu, 0, -1);
            return 1U;
        case UI_KEY_DOWN:
        case UI_KEY_PAGE_DOWN:
            profile_vk_move(menu, 0, 1);
            return 1U;
        case UI_KEY_ENTER:
            profile_vk_select(menu);
            return 1U;
        case UI_KEY_BACK:
            if (menu->profile_name_editor_text[0] != '\0') {
                profile_name_editor_delete(menu);
            } else {
                profile_name_editor_cancel(menu);
            }
            return 1U;
        case UI_KEY_ESC:
            profile_name_editor_cancel(menu);
            return 1U;
        default:
            return 1U;
        }
    }

    if (input.ascii != 0U) {
        profile_name_editor_append(menu, input.ascii);
        return 1U;
    }

    switch (input.key) {
    case UI_KEY_ENTER:
        profile_name_editor_commit(menu);
        return 1U;
    case UI_KEY_BACK:
    case UI_KEY_LEFT:
        if (menu->profile_name_editor_text[0] != '\0') {
            profile_name_editor_delete(menu);
        } else {
            profile_name_editor_cancel(menu);
        }
        return 1U;
    case UI_KEY_ESC:
        profile_name_editor_cancel(menu);
        return 1U;
    default:
        return 1U;
    }
}

uint8_t config_menu_profiles_handle_input(config_menu_t *menu, ui_input_t input)
{
    if (profile_name_editor_handle_input(menu, input) != 0U) {
        return 1U;
    }

    if (menu == NULL || menu->profile_carousel_active == 0U ||
        input.pressed == 0U) {
        return 0U;
    }

    switch (input.key) {
    case UI_KEY_TAB:
    case UI_KEY_SHIFT_TAB:
        return 1U;
    case UI_KEY_PAGE_DOWN:
    case UI_KEY_DOWN:
    case UI_KEY_RIGHT:
        profile_move(menu, 1);
        return 1U;
    case UI_KEY_PAGE_UP:
    case UI_KEY_UP:
    case UI_KEY_LEFT:
        profile_move(menu, -1);
        return 1U;
    case UI_KEY_ENTER:
        profile_select(menu);
        return 1U;
    case UI_KEY_BACK:
        if (profile_manager_is_root(menu->profile_dir) == 0U) {
            char parent[CONFIG_MENU_PATH_LEN];

            (void)profile_manager_parent_path(menu->profile_dir, parent, sizeof(parent));
            profiles_copy_text(menu->profile_dir, sizeof(menu->profile_dir), parent);
            menu->profile_selected = 0U;
            profile_refresh_carousel(menu);
        } else {
            menu->profile_carousel_active = 0U;
        }
        return 1U;
    case UI_KEY_ESC:
        menu->profile_carousel_active = 0U;
        return 1U;
    default:
        return 1U;
    }
}

void config_menu_profiles_save_to_profile(config_menu_t *menu)
{
    if (menu == NULL) {
        return;
    }
    if (menu->profile_source_dir[0] == '\0') {
        config_menu_set_status(menu, 1U, "SELECT A PROFILE FIRST");
        return;
    }

    if (config_menu_save_profile_settings(menu, menu->profile_source_dir) != 0U) {
        char text[CONFIG_MENU_STATUS_LEN];

        (void)snprintf(text,
                       sizeof(text),
                       "SAVED PROFILE %.72s",
                       config_menu_basename(menu->profile_source_dir));
        config_menu_set_status(menu, 0U, text);
    }
}

void config_menu_profiles_save_as(config_menu_t *menu)
{
    if (menu == NULL) {
        return;
    }
    if (menu->profile_dir[0] == '\0') {
        profiles_copy_text(menu->profile_dir,
                           sizeof(menu->profile_dir),
                           PROFILE_MANAGER_ROOT);
    }

    profile_name_editor_start(menu,
                              CONFIG_PROFILE_NAME_MODE_SAVE_AS,
                              "",
                              menu->profile_dir);
}

void config_menu_profiles_rename(config_menu_t *menu)
{
    if (menu == NULL) {
        return;
    }
    if (menu->profile_source_dir[0] == '\0') {
        config_menu_set_status(menu, 1U, "SELECT A PROFILE FIRST");
        return;
    }
    profile_name_editor_start(menu,
                              CONFIG_PROFILE_NAME_MODE_RENAME,
                              config_menu_basename(menu->profile_source_dir),
                              menu->profile_source_dir);
}

void config_menu_profiles_set_image_from_png(config_menu_t *menu, const char *path)
{
    char err[CONFIG_MENU_STATUS_LEN];
    char text[CONFIG_MENU_STATUS_LEN];

    if (menu == NULL || path == NULL) {
        return;
    }
    if (menu->profile_source_dir[0] == '\0') {
        config_menu_set_status(menu, 1U, "SELECT A PROFILE FIRST");
        return;
    }

    err[0] = '\0';
    if (profile_manager_normalize_thumb_png(path,
                                            menu->profile_source_dir,
                                            err,
                                            sizeof(err)) == 0) {
        profile_thumb_cache_clear_all();
        (void)snprintf(text,
                       sizeof(text),
                       "PROFILE IMAGE: %.76s",
                       config_menu_basename(path));
        config_menu_set_status(menu, 0U, text);
    } else {
        (void)snprintf(text,
                       sizeof(text),
                       "PROFILE IMAGE FAILED: %.70s",
                       err);
        config_menu_set_status(menu, 1U, text);
    }
    config_menu_refresh_smartport_media_after_menu_sd(menu);
}

static void profile_fit_text(char *dst, size_t dst_len, const char *src, uint32_t max_chars)
{
    size_t len;

    if (dst == NULL || dst_len == 0U) {
        return;
    }
    if (src == NULL) {
        src = "";
    }
    if (max_chars + 1U > dst_len) {
        max_chars = (uint32_t)(dst_len - 1U);
    }
    len = strlen(src);
    if (len <= max_chars) {
        profiles_copy_text(dst, dst_len, src);
        return;
    }
    if (max_chars <= 3U) {
        dst[0] = '\0';
        return;
    }
    memcpy(dst, src, max_chars - 3U);
    dst[max_chars - 3U] = '.';
    dst[max_chars - 2U] = '.';
    dst[max_chars - 1U] = '.';
    dst[max_chars] = '\0';
}

static void profile_blit_scaled_bgra(uint16_t *fb,
                                     int dst_x,
                                     int dst_y,
                                     int dst_w,
                                     int dst_h,
                                     const uint32_t *src,
                                     unsigned src_w,
                                     unsigned src_h)
{
    if (fb == NULL || src == NULL || dst_w <= 0 || dst_h <= 0 ||
        src_w == 0U || src_h == 0U) {
        return;
    }

    for (int y = 0; y < dst_h; ++y) {
        const int out_y = dst_y + y;
        const uint32_t sy = (uint32_t)(((uint64_t)(uint32_t)y * src_h) /
                                      (uint32_t)dst_h);

        if (out_y < 0 || out_y >= FB16_HEIGHT) {
            continue;
        }
        for (int x = 0; x < dst_w; ++x) {
            const int out_x = dst_x + x;
            const uint32_t sx = (uint32_t)(((uint64_t)(uint32_t)x * src_w) /
                                          (uint32_t)dst_w);

            if (out_x < 0 || out_x >= FB16_WIDTH) {
                continue;
            }
            fb[((size_t)out_y * FB16_WIDTH) + (size_t)out_x] =
                fb16_from_bgra32(src[((size_t)sy * src_w) + sx]);
        }
    }
}

static const uint32_t *profile_cache_thumb(const config_profile_ui_entry_t *entry,
                                           uint32_t cache_slot,
                                           unsigned *out_w,
                                           unsigned *out_h)
{
    config_profile_thumb_cache_t *cache;
    char err[80];

    if (out_w != NULL) {
        *out_w = 0U;
    }
    if (out_h != NULL) {
        *out_h = 0U;
    }
    if (entry == NULL || entry->type != CONFIG_PROFILE_UI_PROFILE ||
        entry->thumb_path[0] == '\0' ||
        cache_slot >= CONFIG_PROFILE_THUMB_CACHE_COUNT) {
        return NULL;
    }

    cache = &g_profile_thumb_cache[cache_slot];
    if (cache->loaded != 0U && strcmp(cache->path, entry->thumb_path) == 0) {
        if (cache->valid != 0U) {
            if (out_w != NULL) {
                *out_w = cache->w;
            }
            if (out_h != NULL) {
                *out_h = cache->h;
            }
            return cache->pixels;
        }
        return NULL;
    }

    profile_thumb_cache_clear_slot(cache_slot);
    cache->loaded = 1U;
    profiles_copy_text(cache->path, sizeof(cache->path), entry->thumb_path);
    err[0] = '\0';
    if (profile_manager_load_thumb_bgra32(entry->thumb_path,
                                          &cache->pixels,
                                          &cache->w,
                                          &cache->h,
                                          err,
                                          sizeof(err)) == 0) {
        cache->valid = 1U;
        if (out_w != NULL) {
            *out_w = cache->w;
        }
        if (out_h != NULL) {
            *out_h = cache->h;
        }
        return cache->pixels;
    }

    cache->valid = 0U;
    return NULL;
}

static void profile_draw_placeholder(uint16_t *fb,
                                     int x,
                                     int y,
                                     int w,
                                     int h,
                                     config_profile_ui_type_t type,
                                     uint8_t focused)
{
    const uint32_t bg = (type == CONFIG_PROFILE_UI_FOLDER) ?
        CMUI_COLOR_PANEL_2 : CMUI_COLOR_ROW;
    const uint32_t accent = (type == CONFIG_PROFILE_UI_FOLDER) ?
        CMUI_COLOR_ACCENT : CMUI_COLOR_ACCENT_2;
    const uint32_t dim = focused ? CMUI_COLOR_BORDER : CMUI_COLOR_BORDER_SOFT;

    fb16_fill_rect(fb, x, y, w, h, bg);
    for (int row = 0; row < h; row += 12) {
        fb16_hline(fb, x, y + row, w, dim);
    }

    if (type == CONFIG_PROFILE_UI_FOLDER || type == CONFIG_PROFILE_UI_PARENT) {
        const int folder_w = (w * 52) / 100;
        const int folder_h = (h * 34) / 100;
        const int fx = x + ((w - folder_w) / 2);
        const int fy = y + ((h - folder_h) / 2);

        fb16_fill_rect(fb, fx, fy + (folder_h / 5), folder_w, folder_h, accent);
        fb16_fill_rect(fb, fx + (folder_w / 12), fy, folder_w / 3, folder_h / 4, accent);
        fb16_rect(fb, fx, fy + (folder_h / 5), folder_w, folder_h,
                  CMUI_COLOR_TEXT);
    } else {
        const char *label = (type == CONFIG_PROFILE_UI_EMPTY) ? "EMPTY" : "APPLETINI";
        const int scale = focused ? 4 : 2;
        const int text_w = (int)strlen(label) * FB16_BUILTIN_FONT_ADVANCE_X * scale;

        fb16_string_scaled(fb,
                           x + ((w - text_w) / 2),
                           y + ((h - FB16_BUILTIN_FONT_HEIGHT * scale) / 2),
                           label,
                           accent,
                           bg,
                           scale);
    }
}

static void profile_draw_card(uint16_t *fb,
                              const config_profile_ui_entry_t *entry,
                              int x,
                              int y,
                              int w,
                              int h,
                              uint8_t focused,
                              uint32_t cache_slot)
{
    uint32_t title_color = focused ? CMUI_COLOR_ACCENT : CMUI_COLOR_TEXT;
    unsigned thumb_w = 0U;
    unsigned thumb_h = 0U;
    const uint32_t *thumb = profile_cache_thumb(entry, cache_slot, &thumb_w, &thumb_h);
    char title[80];
    char kind[24];
    int text_scale = focused ? 2 : 1;
    int max_chars;

    if (entry == NULL) {
        return;
    }

    if (focused == 0U) {
        fb16_fill_rect(fb, x - 8, y - 8, w + 16, h + 50, CMUI_COLOR_PANEL);
        fb16_rect(fb, x - 8, y - 8, w + 16, h + 50, CMUI_COLOR_BORDER_SOFT);
    }
    if (thumb != NULL) {
        profile_blit_scaled_bgra(fb, x, y, w, h, thumb, thumb_w, thumb_h);
    } else {
        profile_draw_placeholder(fb, x, y, w, h, entry->type, focused);
    }
    if (focused != 0U) {
        fb16_rect(fb, x - 2, y - 2, w + 4, h + 4, CMUI_COLOR_ACCENT);
    } else {
        fb16_rect(fb, x, y, w, h, CMUI_COLOR_BORDER_SOFT);
    }

    max_chars = w / (FB16_BUILTIN_FONT_ADVANCE_X * text_scale);
    profile_fit_text(title, sizeof(title), entry->name, (uint32_t)max_chars);
    if (entry->type == CONFIG_PROFILE_UI_FOLDER) {
        profiles_copy_text(kind, sizeof(kind), "FOLDER");
    } else if (entry->type == CONFIG_PROFILE_UI_PARENT) {
        profiles_copy_text(kind, sizeof(kind), "BACK");
    } else {
        profiles_copy_text(kind, sizeof(kind), "");
    }

    fb16_string_scaled(fb,
                       x,
                       y + h + 10,
                       title,
                       title_color,
                       CMUI_COLOR_BG,
                       text_scale);
    if (kind[0] != '\0') {
        fb16_string_scaled(fb,
                           x,
                           y + h + 12 + (FB16_BUILTIN_FONT_HEIGHT * text_scale),
                           kind,
                           focused ? CMUI_COLOR_ACCENT : CMUI_COLOR_DIM,
                           CMUI_COLOR_BG,
                           text_scale);
    }
}

static void profile_draw_carousel(uint16_t *fb,
                                  const config_menu_t *menu,
                                  int x,
                                  int y,
                                  int w)
{
    const int fb_x = HGR_X + (x * HGR_SCALE);
    const int fb_y = HGR_Y + ((y - 2) * HGR_SCALE);
    const int fb_w = w * HGR_SCALE;
    const int panel_h = 620;
    const int center_w = 560;
    const int center_h = 384;
    const int side_w = 260;
    const int side_h = 178;
    const int center_x = fb_x + ((fb_w - center_w) / 2);
    const int center_y = fb_y + 78;
    const int side_y = center_y + 82;
    const int left_x = fb_x + 42;
    const int right_x = fb_x + fb_w - side_w - 42;
    char line[CONFIG_MENU_STATUS_LEN];

    if (menu == NULL || menu->profile_count == 0U) {
        return;
    }

    fb16_fill_rect(fb, fb_x - 20, fb_y - 18, fb_w + 40, panel_h,
                   CMUI_COLOR_PANEL);
    fb16_rect(fb, fb_x - 20, fb_y - 18, fb_w + 40, panel_h,
              CMUI_COLOR_BORDER);
    cmui_text(fb,
              fb_x + 16,
              fb_y + 10,
              "Profile Carousel",
              CMUI_COLOR_WARN,
              CMUI_COLOR_PANEL,
              CMUI_BODY_SCALE);
    cmui_text_clipped(fb,
                      fb_x + 16,
                      fb_y + 46,
                      fb_w - 180,
                      menu->profile_dir,
                      CMUI_COLOR_MUTED,
                      CMUI_COLOR_PANEL,
                      CMUI_SMALL_SCALE);
    (void)snprintf(line,
                   sizeof(line),
                   "%u/%u",
                   (unsigned)(menu->profile_selected + 1U),
                   (unsigned)menu->profile_count);
    cmui_text(fb,
              fb_x + fb_w - 110,
              fb_y + 10,
              line,
              CMUI_COLOR_TEXT,
              CMUI_COLOR_PANEL,
              CMUI_BODY_SCALE);

    if (menu->profile_count > 1U) {
        const uint16_t prev = (menu->profile_selected == 0U) ?
            (uint16_t)(menu->profile_count - 1U) :
            (uint16_t)(menu->profile_selected - 1U);
        const uint16_t next =
            (uint16_t)((menu->profile_selected + 1U) % menu->profile_count);

        profile_draw_card(fb, profile_entry_at(menu, prev),
                          left_x, side_y, side_w, side_h, 0U, 0U);
        profile_draw_card(fb, profile_entry_at(menu, next),
                          right_x, side_y, side_w, side_h, 0U, 1U);
    }
    profile_draw_card(fb,
                      profile_entry_at(menu, menu->profile_selected),
                      center_x,
                      center_y,
                      center_w,
                      center_h,
                      1U,
                      2U);

}

static void profile_draw_name_editor(uint16_t *fb,
                                     const config_menu_t *menu,
                                     int x,
                                     int y,
                                     int w)
{
    const int fb_x = HGR_X + (x * HGR_SCALE);
    const int fb_y = HGR_Y + ((y + 10) * HGR_SCALE);
    const int fb_w = w * HGR_SCALE;
    const int panel_w = (menu != NULL && menu->profile_name_editor_virtual != 0U) ?
        CONFIG_PROFILE_VK_PANEL_W : 760;
    const int panel_h = (menu != NULL && menu->profile_name_editor_virtual != 0U) ?
        CONFIG_PROFILE_VK_PANEL_H : 180;
    const int panel_x = fb_x + ((fb_w - panel_w) / 2);
    const int panel_y = fb_y;
    const char *title =
        (menu != NULL && menu->profile_name_editor_mode == CONFIG_PROFILE_NAME_MODE_RENAME) ?
        "RENAME PROFILE" : "SAVE AS PROFILE";
    char display[80];

    if (menu == NULL || menu->profile_name_editor_active == 0U) {
        return;
    }

    fb16_fill_rect(fb, panel_x, panel_y, panel_w, panel_h, CMUI_COLOR_PANEL);
    fb16_rect(fb, panel_x, panel_y, panel_w, panel_h, CMUI_COLOR_BORDER);
    cmui_text(fb,
              panel_x + 20,
              panel_y + 18,
              title,
              CMUI_COLOR_WARN,
              CMUI_COLOR_PANEL,
              CMUI_BODY_SCALE);

    profile_fit_text(display,
                     sizeof(display),
                     (menu->profile_name_editor_text[0] != '\0') ?
                         menu->profile_name_editor_text : "[name]",
                     42U);
    fb16_fill_rect(fb,
                   panel_x + 20,
                   panel_y + 58,
                   panel_w - 40,
                   44,
                   CMUI_COLOR_ROW);
    fb16_rect(fb, panel_x + 20, panel_y + 58, panel_w - 40, 44,
              CMUI_COLOR_ACCENT);
    cmui_text_clipped(fb,
                      panel_x + 34,
                      panel_y + 70,
                      panel_w - 68,
                      display,
                      (menu->profile_name_editor_text[0] != '\0') ?
                          CMUI_COLOR_TEXT : CMUI_COLOR_DIM,
                      CMUI_COLOR_ROW,
                      CMUI_BODY_SCALE);

    if (menu->profile_name_editor_virtual == 0U) {
        return;
    }

    for (uint32_t i = 0U; i < CONFIG_PROFILE_VK_KEY_COUNT; ++i) {
        const uint32_t row = i / CONFIG_PROFILE_VK_COLS;
        const uint32_t col = i % CONFIG_PROFILE_VK_COLS;
        const int key_w = CONFIG_PROFILE_VK_KEY_W;
        const int key_h = CONFIG_PROFILE_VK_KEY_H;
        const int key_gap = CONFIG_PROFILE_VK_KEY_GAP;
        const int key_scale = CONFIG_PROFILE_VK_KEY_SCALE;
        const int grid_w =
            ((int)CONFIG_PROFILE_VK_COLS * key_w) +
            (((int)CONFIG_PROFILE_VK_COLS - 1) * key_gap);
        const int key_x = panel_x + ((panel_w - grid_w) / 2) +
            (int)col * (key_w + key_gap);
        const int key_y = panel_y + 126 + (int)row * (key_h + key_gap);
        const uint8_t focused = (uint8_t)(i == menu->profile_name_editor_vk_index);
        const uint32_t bg = (focused != 0U) ?
            CMUI_COLOR_ROW_ACTIVE : CMUI_COLOR_ROW;
        const uint32_t fg = (focused != 0U) ? CMUI_COLOR_ACCENT : CMUI_COLOR_TEXT;
        const char *label = k_profile_vk_keys[i];
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

void config_menu_profiles_draw(uint16_t *fb,
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

    (void)snprintf(line,
                   sizeof(line),
                   "Current: %s",
                   (menu->profile_source_dir[0] != '\0') ?
                       config_menu_basename(menu->profile_source_dir) :
                       "[working config]");
    cmui_caption(fb, x + 18, y + 10, w - 36, line);

    hgr_draw_item(fb,
                  x,
                  y + row_h,
                  w,
                  (uint8_t)(menu->item_focus == 0U),
                  "Choose profile",
                  HGR_WHITE);
    hgr_draw_item(fb,
                  x,
                  y + (row_h * 2),
                  w,
                  (uint8_t)(menu->item_focus == 1U),
                  "Save to current profile",
                  (menu->profile_source_dir[0] != '\0') ? HGR_WHITE : HGR_DIMMED);
    hgr_draw_item(fb,
                  x,
                  y + (row_h * 3),
                  w,
                  (uint8_t)(menu->item_focus == 2U),
                  "Save As",
                  HGR_WHITE);
    hgr_draw_item(fb,
                  x,
                  y + (row_h * 4),
                  w,
                  (uint8_t)(menu->item_focus == 3U),
                  "Rename profile",
                  (menu->profile_source_dir[0] != '\0') ? HGR_WHITE : HGR_DIMMED);
    hgr_draw_item(fb,
                  x,
                  y + (row_h * 5),
                  w,
                  (uint8_t)(menu->item_focus == 4U),
                  "Set image",
                  (menu->profile_source_dir[0] != '\0') ? HGR_WHITE : HGR_DIMMED);

    if (menu->profile_source_dir[0] != '\0') {
        (void)snprintf(line,
                       sizeof(line),
                       "Profile path: %.72s",
                       menu->profile_source_dir);
        cmui_caption(fb, x + 18, y + (row_h * 7) + 12, w - 36, line);
    } else {
        cmui_caption(fb,
                     x + 18,
                     y + (row_h * 7) + 12,
                     w - 36,
                     "Choose a profile to enable Save, Rename, and Set image");
    }

    if (menu->profile_carousel_active != 0U) {
        profile_draw_carousel(fb, menu, x, y, w);
    }
    if (menu->profile_name_editor_active != 0U) {
        profile_draw_name_editor(fb, menu, x, y, w);
    }
}
