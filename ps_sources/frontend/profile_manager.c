#include "profile_manager.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diskio.h"
#include "ff.h"

#include "../lib/crc32.h"
#include "../lib/lodepng.h"

#define PROFILE_PNG_ADLER_MOD 65521U
#define PROFILE_PNG_ADLER_NMAX 5552U

static FATFS g_profile_fs;
static uint8_t g_profile_png_row[1U + (PROFILE_MANAGER_THUMB_W * 4U)];

static void profile_set_error(char *errbuf, size_t errbuf_size, const char *fmt, ...)
{
    va_list ap;

    if (errbuf == NULL || errbuf_size == 0U) {
        return;
    }

    va_start(ap, fmt);
    (void)vsnprintf(errbuf, errbuf_size, fmt, ap);
    va_end(ap);
    errbuf[errbuf_size - 1U] = '\0';
}

FRESULT profile_manager_mount(void)
{
    FRESULT fr = f_mount(&g_profile_fs, "0:/", 1U);

    if (fr == FR_OK) {
        return fr;
    }

    (void)disk_initialize(0);
    (void)f_mount((FATFS *)0, "0:/", 0U);
    return f_mount(&g_profile_fs, "0:/", 1U);
}

FRESULT profile_manager_ensure_root(void)
{
    FRESULT fr = profile_manager_mount();

    if (fr != FR_OK) {
        return fr;
    }

    fr = f_mkdir(PROFILE_MANAGER_ROOT);
    return (fr == FR_EXIST) ? FR_OK : fr;
}

static char profile_lower_char(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return (char)(c - 'A' + 'a');
    }
    if (c == '\\') {
        return '/';
    }
    return c;
}

static uint8_t profile_path_ieq_trimmed(const char *a, const char *b)
{
    size_t alen;
    size_t blen;

    if (a == NULL || b == NULL) {
        return 0U;
    }

    alen = strlen(a);
    blen = strlen(b);
    while (alen > 3U && (a[alen - 1U] == '/' || a[alen - 1U] == '\\')) {
        --alen;
    }
    while (blen > 3U && (b[blen - 1U] == '/' || b[blen - 1U] == '\\')) {
        --blen;
    }
    if (alen != blen) {
        return 0U;
    }
    for (size_t i = 0U; i < alen; ++i) {
        if (profile_lower_char(a[i]) != profile_lower_char(b[i])) {
            return 0U;
        }
    }
    return 1U;
}

uint8_t profile_manager_is_root(const char *path)
{
    return profile_path_ieq_trimmed(path, PROFILE_MANAGER_ROOT);
}

uint8_t profile_manager_join_path(const char *dir,
                                  const char *name,
                                  char *out,
                                  size_t out_len)
{
    size_t dir_len;
    int len;

    if (out == NULL || out_len == 0U || name == NULL || name[0] == '\0') {
        return 0U;
    }
    if (dir == NULL || dir[0] == '\0') {
        dir = PROFILE_MANAGER_ROOT;
    }

    dir_len = strlen(dir);
    if (dir_len == 0U || dir[dir_len - 1U] == '/' || dir[dir_len - 1U] == '\\') {
        len = snprintf(out, out_len, "%s%s", dir, name);
    } else {
        len = snprintf(out, out_len, "%s/%s", dir, name);
    }

    return (len > 0 && len < (int)out_len) ? 1U : 0U;
}

uint8_t profile_manager_parent_path(const char *path, char *out, size_t out_len)
{
    char tmp[PROFILE_MANAGER_PATH_LEN];
    size_t len;

    if (out == NULL || out_len == 0U) {
        return 0U;
    }
    if (path == NULL || path[0] == '\0' || profile_manager_is_root(path) != 0U) {
        (void)snprintf(out, out_len, "%s", PROFILE_MANAGER_ROOT);
        return 1U;
    }

    (void)snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    while (len > 0U && (tmp[len - 1U] == '/' || tmp[len - 1U] == '\\')) {
        tmp[--len] = '\0';
    }
    while (len > 0U && tmp[len - 1U] != '/' && tmp[len - 1U] != '\\') {
        --len;
    }
    if (len == 0U) {
        (void)snprintf(out, out_len, "%s", PROFILE_MANAGER_ROOT);
        return 1U;
    }
    tmp[len - 1U] = '\0';
    if (profile_path_ieq_trimmed(tmp, "0:") != 0U ||
        profile_path_ieq_trimmed(tmp, "0:/") != 0U ||
        strlen(tmp) < strlen(PROFILE_MANAGER_ROOT)) {
        (void)snprintf(out, out_len, "%s", PROFILE_MANAGER_ROOT);
    } else {
        (void)snprintf(out, out_len, "%s", tmp);
    }
    return 1U;
}

uint8_t profile_manager_cfg_path(const char *profile_dir, char *out, size_t out_len)
{
    return profile_manager_join_path(profile_dir, PROFILE_MANAGER_CFG_NAME, out, out_len);
}

uint8_t profile_manager_thumb_path(const char *profile_dir, char *out, size_t out_len)
{
    return profile_manager_join_path(profile_dir, PROFILE_MANAGER_THUMB_NAME, out, out_len);
}

uint8_t profile_manager_is_profile_dir(const char *dir)
{
    FILINFO info;
    char cfg_path[PROFILE_MANAGER_PATH_LEN];

    if (dir == NULL || dir[0] == '\0' ||
        profile_manager_cfg_path(dir, cfg_path, sizeof(cfg_path)) == 0U) {
        return 0U;
    }

    if (f_stat(cfg_path, &info) != FR_OK) {
        return 0U;
    }
    return ((info.fattrib & AM_DIR) == 0U) ? 1U : 0U;
}

static int profile_entry_compare(const profile_manager_entry_t *a,
                                 const profile_manager_entry_t *b)
{
    const char *an;
    const char *bn;

    if (a == NULL || b == NULL) {
        return 0;
    }
    if (a->type == PROFILE_MANAGER_ENTRY_FOLDER &&
        b->type != PROFILE_MANAGER_ENTRY_FOLDER) {
        return -1;
    }
    if (a->type != PROFILE_MANAGER_ENTRY_FOLDER &&
        b->type == PROFILE_MANAGER_ENTRY_FOLDER) {
        return 1;
    }

    an = a->name;
    bn = b->name;
    while (*an != '\0' && *bn != '\0') {
        const char ac = profile_lower_char(*an);
        const char bc = profile_lower_char(*bn);
        if (ac != bc) {
            return (ac < bc) ? -1 : 1;
        }
        ++an;
        ++bn;
    }
    if (*an == '\0' && *bn == '\0') {
        return 0;
    }
    return (*an == '\0') ? -1 : 1;
}

static void profile_sort_entries(profile_manager_entry_t *entries, uint16_t count)
{
    for (uint16_t i = 1U; i < count; ++i) {
        profile_manager_entry_t item = entries[i];
        uint16_t j = i;

        while (j > 0U && profile_entry_compare(&item, &entries[j - 1U]) < 0) {
            entries[j] = entries[j - 1U];
            --j;
        }
        entries[j] = item;
    }
}

FRESULT profile_manager_list_dir(const char *dir,
                                 profile_manager_entry_t *entries,
                                 uint16_t max_entries,
                                 uint16_t *out_count)
{
    DIR fat_dir;
    FILINFO info;
    FRESULT fr;
    uint16_t count = 0U;

    if (entries == NULL || out_count == NULL || max_entries == 0U) {
        return FR_INVALID_PARAMETER;
    }
    *out_count = 0U;

    fr = profile_manager_ensure_root();
    if (fr != FR_OK) {
        return fr;
    }
    if (dir == NULL || dir[0] == '\0') {
        dir = PROFILE_MANAGER_ROOT;
    }

    fr = f_opendir(&fat_dir, dir);
    if (fr != FR_OK) {
        return fr;
    }

    for (;;) {
        fr = f_readdir(&fat_dir, &info);
        if (fr != FR_OK || info.fname[0] == '\0') {
            break;
        }
        if (strcmp(info.fname, ".") == 0 || strcmp(info.fname, "..") == 0 ||
            (info.fattrib & AM_DIR) == 0U) {
            continue;
        }
        if (count < max_entries) {
            profile_manager_entry_t *entry = &entries[count];
            char child_path[PROFILE_MANAGER_PATH_LEN];

            if (profile_manager_join_path(dir,
                                          info.fname,
                                          child_path,
                                          sizeof(child_path)) == 0U) {
                continue;
            }

            memset(entry, 0, sizeof(*entry));
            (void)snprintf(entry->name, sizeof(entry->name), "%.127s", info.fname);
            (void)snprintf(entry->path, sizeof(entry->path), "%s", child_path);
            entry->type = (profile_manager_is_profile_dir(child_path) != 0U) ?
                PROFILE_MANAGER_ENTRY_PROFILE : PROFILE_MANAGER_ENTRY_FOLDER;
            (void)profile_manager_cfg_path(child_path,
                                           entry->cfg_path,
                                           sizeof(entry->cfg_path));
            (void)profile_manager_thumb_path(child_path,
                                             entry->thumb_path,
                                             sizeof(entry->thumb_path));
            ++count;
        }
    }
    (void)f_closedir(&fat_dir);
    if (fr != FR_OK) {
        return fr;
    }

    profile_sort_entries(entries, count);
    *out_count = count;
    return FR_OK;
}

static uint8_t profile_name_char_valid(char c)
{
    switch (c) {
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
    return ((unsigned char)c >= 0x20U && (unsigned char)c != 0x7FU) ? 1U : 0U;
}

uint8_t profile_manager_profile_name_valid(const char *name)
{
    size_t len;

    if (name == NULL || name[0] == '\0') {
        return 0U;
    }
    len = strlen(name);
    if (len == 0U || len >= PROFILE_MANAGER_PATH_LEN) {
        return 0U;
    }
    if ((len == 1U && name[0] == '.') ||
        (len == 2U && name[0] == '.' && name[1] == '.')) {
        return 0U;
    }
    if (name[0] == ' ' || name[0] == '.' ||
        name[len - 1U] == ' ' || name[len - 1U] == '.') {
        return 0U;
    }
    for (size_t i = 0U; i < len; ++i) {
        if (profile_name_char_valid(name[i]) == 0U) {
            return 0U;
        }
    }
    return 1U;
}

FRESULT profile_manager_create_profile(const char *parent_dir,
                                       const char *name,
                                       char *out_dir,
                                       size_t out_dir_len)
{
    char path[PROFILE_MANAGER_PATH_LEN];
    FRESULT fr;

    if (out_dir == NULL || out_dir_len == 0U) {
        return FR_INVALID_PARAMETER;
    }
    out_dir[0] = '\0';
    if (parent_dir == NULL || parent_dir[0] == '\0') {
        parent_dir = PROFILE_MANAGER_ROOT;
    }
    if (profile_manager_profile_name_valid(name) == 0U ||
        profile_manager_join_path(parent_dir, name, path, sizeof(path)) == 0U) {
        return FR_INVALID_NAME;
    }

    fr = profile_manager_ensure_root();
    if (fr != FR_OK) {
        return fr;
    }
    fr = f_mkdir(path);
    if (fr == FR_OK) {
        (void)snprintf(out_dir, out_dir_len, "%s", path);
    }
    return fr;
}

FRESULT profile_manager_rename_profile(const char *profile_dir,
                                       const char *name,
                                       char *out_dir,
                                       size_t out_dir_len)
{
    char parent[PROFILE_MANAGER_PATH_LEN];
    char path[PROFILE_MANAGER_PATH_LEN];
    FILINFO info;
    FRESULT fr;

    if (out_dir == NULL || out_dir_len == 0U) {
        return FR_INVALID_PARAMETER;
    }
    out_dir[0] = '\0';
    if (profile_dir == NULL || profile_dir[0] == '\0' ||
        profile_manager_is_root(profile_dir) != 0U ||
        profile_manager_profile_name_valid(name) == 0U) {
        return FR_INVALID_NAME;
    }
    fr = profile_manager_ensure_root();
    if (fr != FR_OK) {
        return fr;
    }
    if (profile_manager_is_profile_dir(profile_dir) == 0U) {
        return FR_INVALID_NAME;
    }
    if (profile_manager_parent_path(profile_dir, parent, sizeof(parent)) == 0U ||
        profile_manager_join_path(parent, name, path, sizeof(path)) == 0U) {
        return FR_INVALID_NAME;
    }
    if (profile_path_ieq_trimmed(profile_dir, path) != 0U) {
        (void)snprintf(out_dir, out_dir_len, "%s", profile_dir);
        return FR_OK;
    }

    fr = f_stat(path, &info);
    if (fr == FR_OK) {
        return FR_EXIST;
    }
    if (fr != FR_NO_FILE) {
        return fr;
    }
    fr = f_rename(profile_dir, path);
    if (fr == FR_OK) {
        (void)snprintf(out_dir, out_dir_len, "%s", path);
    }
    return fr;
}

static int profile_read_file(const char *path,
                             unsigned char **out_data,
                             size_t *out_size,
                             char *errbuf,
                             size_t errbuf_size)
{
    FIL file;
    FRESULT fr;
    FSIZE_t file_size;
    unsigned char *data;
    size_t offset = 0U;

    if (out_data == NULL || out_size == NULL || path == NULL || path[0] == '\0') {
        profile_set_error(errbuf, errbuf_size, "Invalid PNG path");
        return -1;
    }

    *out_data = NULL;
    *out_size = 0U;
    fr = profile_manager_mount();
    if (fr != FR_OK) {
        profile_set_error(errbuf, errbuf_size, "SD mount failed: FRESULT=%u", (unsigned)fr);
        return -1;
    }

    fr = f_open(&file, path, FA_READ);
    if (fr != FR_OK) {
        profile_set_error(errbuf, errbuf_size, "Open failed for %s: FRESULT=%u", path, (unsigned)fr);
        return -1;
    }

    file_size = f_size(&file);
    if (file_size == 0U) {
        (void)f_close(&file);
        profile_set_error(errbuf, errbuf_size, "PNG file is empty: %s", path);
        return -1;
    }

    data = (unsigned char *)malloc((size_t)file_size);
    if (data == NULL) {
        (void)f_close(&file);
        profile_set_error(errbuf, errbuf_size, "Out of memory reading %s", path);
        return -1;
    }

    while (offset < (size_t)file_size) {
        const size_t remaining = (size_t)file_size - offset;
        const UINT chunk = (remaining > (size_t)UINT_MAX) ? UINT_MAX : (UINT)remaining;
        UINT got = 0U;

        fr = f_read(&file, data + offset, chunk, &got);
        if (fr != FR_OK || got == 0U) {
            free(data);
            (void)f_close(&file);
            profile_set_error(errbuf, errbuf_size, "Read failed for %s: FRESULT=%u", path, (unsigned)fr);
            return -1;
        }
        offset += (size_t)got;
    }

    (void)f_close(&file);
    *out_data = data;
    *out_size = offset;
    return 0;
}

static int profile_decode_png_bgra32(const unsigned char *png_data,
                                     size_t png_size,
                                     const char *label,
                                     uint32_t **out_pixels,
                                     unsigned *out_w,
                                     unsigned *out_h,
                                     char *errbuf,
                                     size_t errbuf_size)
{
    unsigned char *rgba = NULL;
    uint32_t *bgra = NULL;
    unsigned w = 0U;
    unsigned h = 0U;
    unsigned error;
    size_t pixel_count;

    if (png_data == NULL || png_size == 0U ||
        out_pixels == NULL || out_w == NULL || out_h == NULL) {
        profile_set_error(errbuf, errbuf_size, "Invalid PNG decode arguments");
        return -1;
    }
    if (label == NULL) {
        label = "profile PNG";
    }

    *out_pixels = NULL;
    *out_w = 0U;
    *out_h = 0U;

    error = lodepng_decode32(&rgba, &w, &h, png_data, png_size);
    if (error != 0U) {
        profile_set_error(errbuf, errbuf_size, "PNG decode failed for %s: %s",
                          label, lodepng_error_text(error));
        return -1;
    }
    if (w == 0U || h == 0U) {
        free(rgba);
        profile_set_error(errbuf, errbuf_size, "PNG has invalid dimensions: %s", label);
        return -1;
    }

    pixel_count = (size_t)w * (size_t)h;
    bgra = (uint32_t *)malloc(pixel_count * sizeof(uint32_t));
    if (bgra == NULL) {
        free(rgba);
        profile_set_error(errbuf, errbuf_size, "Out of memory converting %s", label);
        return -1;
    }

    for (size_t i = 0U; i < pixel_count; ++i) {
        const unsigned char a = rgba[(i * 4U) + 3U];
        const unsigned char r = (unsigned char)((rgba[(i * 4U) + 0U] * a + 127U) / 255U);
        const unsigned char g = (unsigned char)((rgba[(i * 4U) + 1U] * a + 127U) / 255U);
        const unsigned char b = (unsigned char)((rgba[(i * 4U) + 2U] * a + 127U) / 255U);

        bgra[i] = ((uint32_t)b) |
                  (((uint32_t)g) << 8U) |
                  (((uint32_t)r) << 16U) |
                  (0xFFUL << 24U);
    }

    free(rgba);
    *out_pixels = bgra;
    *out_w = w;
    *out_h = h;
    profile_set_error(errbuf, errbuf_size, "OK");
    return 0;
}

int profile_manager_load_thumb_bgra32(const char *thumb_path,
                                      uint32_t **out_pixels,
                                      unsigned *out_w,
                                      unsigned *out_h,
                                      char *errbuf,
                                      size_t errbuf_size)
{
    unsigned char *png_data = NULL;
    size_t png_size = 0U;
    int rc;

    if (profile_read_file(thumb_path, &png_data, &png_size, errbuf, errbuf_size) != 0) {
        return -1;
    }
    rc = profile_decode_png_bgra32(png_data,
                                   png_size,
                                   thumb_path,
                                   out_pixels,
                                   out_w,
                                   out_h,
                                   errbuf,
                                   errbuf_size);
    free(png_data);
    return rc;
}

static void profile_store_be32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value >> 24U);
    dst[1] = (uint8_t)(value >> 16U);
    dst[2] = (uint8_t)(value >> 8U);
    dst[3] = (uint8_t)value;
}

static void profile_store_le16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8U);
}

static int profile_write_exact(FIL *file, const void *data, uint32_t len)
{
    const uint8_t *src = (const uint8_t *)data;

    while (len != 0U) {
        const UINT chunk = (len > (uint32_t)UINT_MAX) ? UINT_MAX : (UINT)len;
        UINT written = 0U;
        const FRESULT fr = f_write(file, src, chunk, &written);

        if (fr != FR_OK || written != chunk) {
            return (fr == FR_OK) ? -1 : -(int)fr;
        }
        src += chunk;
        len -= (uint32_t)chunk;
    }

    return 0;
}

static int profile_png_chunk_begin(FIL *file,
                                   const char type[4],
                                   uint32_t length,
                                   uint32_t *crc)
{
    uint8_t header[8];

    profile_store_be32(header, length);
    header[4] = (uint8_t)type[0];
    header[5] = (uint8_t)type[1];
    header[6] = (uint8_t)type[2];
    header[7] = (uint8_t)type[3];

    if (profile_write_exact(file, header, sizeof(header)) != 0) {
        return -1;
    }

    *crc = crc32_update(crc32_init(), type, 4U);
    return 0;
}

static int profile_png_chunk_data(FIL *file,
                                  uint32_t *crc,
                                  const void *data,
                                  uint32_t len)
{
    if (profile_write_exact(file, data, len) != 0) {
        return -1;
    }
    *crc = crc32_update(*crc, data, len);
    return 0;
}

static int profile_png_chunk_end(FIL *file, uint32_t crc)
{
    uint8_t out[4];

    profile_store_be32(out, crc32_finish(crc));
    return profile_write_exact(file, out, sizeof(out));
}

static void profile_adler32_update(uint32_t *a_io,
                                   uint32_t *b_io,
                                   const uint8_t *data,
                                   uint32_t len)
{
    uint32_t a = *a_io;
    uint32_t b = *b_io;

    while (len != 0U) {
        uint32_t chunk = (len > PROFILE_PNG_ADLER_NMAX) ? PROFILE_PNG_ADLER_NMAX : len;

        len -= chunk;
        while (chunk != 0U) {
            a += *data++;
            b += a;
            --chunk;
        }
        a %= PROFILE_PNG_ADLER_MOD;
        b %= PROFILE_PNG_ADLER_MOD;
    }

    *a_io = a;
    *b_io = b;
}

static int profile_write_rgba_png(FIL *file,
                                  const uint8_t *rgba,
                                  uint32_t width,
                                  uint32_t height)
{
    static const uint8_t signature[8] = {
        0x89U, 'P', 'N', 'G', 0x0DU, 0x0AU, 0x1AU, 0x0AU
    };
    uint8_t ihdr[13];
    uint32_t crc;
    uint32_t adler_a = 1U;
    uint32_t adler_b = 0U;
    uint8_t zlib_header[2] = {0x78U, 0x01U};
    const uint32_t row_len = 1U + (width * 4U);
    const uint32_t idat_len = 2U + (height * (5U + row_len)) + 4U;

    if (file == NULL || rgba == NULL ||
        width != PROFILE_MANAGER_THUMB_W ||
        height != PROFILE_MANAGER_THUMB_H ||
        row_len > (uint32_t)sizeof(g_profile_png_row)) {
        return -1;
    }

    if (profile_write_exact(file, signature, sizeof(signature)) != 0) {
        return -1;
    }

    memset(ihdr, 0, sizeof(ihdr));
    profile_store_be32(&ihdr[0], width);
    profile_store_be32(&ihdr[4], height);
    ihdr[8] = 8U;
    ihdr[9] = 6U;

    if (profile_png_chunk_begin(file, "IHDR", sizeof(ihdr), &crc) != 0 ||
        profile_png_chunk_data(file, &crc, ihdr, sizeof(ihdr)) != 0 ||
        profile_png_chunk_end(file, crc) != 0 ||
        profile_png_chunk_begin(file, "IDAT", idat_len, &crc) != 0 ||
        profile_png_chunk_data(file, &crc, zlib_header, sizeof(zlib_header)) != 0) {
        return -1;
    }

    for (uint32_t y = 0U; y < height; ++y) {
        uint8_t block_header[5];
        const uint16_t len16 = (uint16_t)row_len;

        g_profile_png_row[0] = 0U;
        memcpy(&g_profile_png_row[1U], &rgba[(size_t)y * (size_t)width * 4U], width * 4U);
        block_header[0] = (uint8_t)((y + 1U == height) ? 0x01U : 0x00U);
        profile_store_le16(&block_header[1], len16);
        profile_store_le16(&block_header[3], (uint16_t)~len16);

        if (profile_png_chunk_data(file, &crc, block_header, sizeof(block_header)) != 0 ||
            profile_png_chunk_data(file, &crc, g_profile_png_row, row_len) != 0) {
            return -1;
        }
        profile_adler32_update(&adler_a, &adler_b, g_profile_png_row, row_len);
    }

    {
        uint8_t adler[4];
        profile_store_be32(adler, (adler_b << 16U) | adler_a);
        if (profile_png_chunk_data(file, &crc, adler, sizeof(adler)) != 0 ||
            profile_png_chunk_end(file, crc) != 0) {
            return -1;
        }
    }

    if (profile_png_chunk_begin(file, "IEND", 0U, &crc) != 0 ||
        profile_png_chunk_end(file, crc) != 0) {
        return -1;
    }

    return 0;
}

static uint8_t *profile_make_normalized_rgba(const unsigned char *src_rgba,
                                             unsigned src_w,
                                             unsigned src_h,
                                             char *errbuf,
                                             size_t errbuf_size)
{
    const uint32_t dst_w = PROFILE_MANAGER_THUMB_W;
    const uint32_t dst_h = PROFILE_MANAGER_THUMB_H;
    uint32_t scaled_w;
    uint32_t scaled_h;
    uint32_t x0;
    uint32_t y0;
    uint8_t *dst;

    if (src_rgba == NULL || src_w == 0U || src_h == 0U) {
        profile_set_error(errbuf, errbuf_size, "PNG has invalid dimensions");
        return NULL;
    }

    dst = (uint8_t *)malloc((size_t)dst_w * (size_t)dst_h * 4U);
    if (dst == NULL) {
        profile_set_error(errbuf, errbuf_size, "Out of memory resizing profile image");
        return NULL;
    }
    for (uint32_t i = 0U; i < dst_w * dst_h; ++i) {
        dst[(i * 4U) + 0U] = 0U;
        dst[(i * 4U) + 1U] = 0U;
        dst[(i * 4U) + 2U] = 0U;
        dst[(i * 4U) + 3U] = 0xFFU;
    }

    if (((uint64_t)dst_w * (uint64_t)src_h) <=
        ((uint64_t)dst_h * (uint64_t)src_w)) {
        scaled_w = dst_w;
        scaled_h = (uint32_t)(((uint64_t)src_h * (uint64_t)dst_w) / (uint64_t)src_w);
    } else {
        scaled_h = dst_h;
        scaled_w = (uint32_t)(((uint64_t)src_w * (uint64_t)dst_h) / (uint64_t)src_h);
    }
    if (scaled_w == 0U) {
        scaled_w = 1U;
    }
    if (scaled_h == 0U) {
        scaled_h = 1U;
    }
    if (scaled_w > dst_w) {
        scaled_w = dst_w;
    }
    if (scaled_h > dst_h) {
        scaled_h = dst_h;
    }

    x0 = (dst_w - scaled_w) / 2U;
    y0 = (dst_h - scaled_h) / 2U;

    for (uint32_t dy = 0U; dy < scaled_h; ++dy) {
        const uint32_t sy = (uint32_t)(((uint64_t)dy * (uint64_t)src_h) /
                                      (uint64_t)scaled_h);
        for (uint32_t dx = 0U; dx < scaled_w; ++dx) {
            const uint32_t sx = (uint32_t)(((uint64_t)dx * (uint64_t)src_w) /
                                          (uint64_t)scaled_w);
            const unsigned char *src =
                &src_rgba[((size_t)sy * (size_t)src_w + (size_t)sx) * 4U];
            uint8_t *pixel =
                &dst[((size_t)(y0 + dy) * (size_t)dst_w + (size_t)(x0 + dx)) * 4U];
            const unsigned char a = src[3];

            pixel[0] = (uint8_t)((src[0] * a + 127U) / 255U);
            pixel[1] = (uint8_t)((src[1] * a + 127U) / 255U);
            pixel[2] = (uint8_t)((src[2] * a + 127U) / 255U);
            pixel[3] = 0xFFU;
        }
    }

    return dst;
}

int profile_manager_normalize_thumb_png(const char *src_png_path,
                                        const char *profile_dir,
                                        char *errbuf,
                                        size_t errbuf_size)
{
    unsigned char *png_data = NULL;
    size_t png_size = 0U;
    unsigned char *src_rgba = NULL;
    uint8_t *dst_rgba = NULL;
    unsigned src_w = 0U;
    unsigned src_h = 0U;
    unsigned error;
    char thumb_path[PROFILE_MANAGER_PATH_LEN];
    FIL file;
    FRESULT fr;
    int rc = -1;

    if (src_png_path == NULL || src_png_path[0] == '\0' ||
        profile_dir == NULL || profile_dir[0] == '\0') {
        profile_set_error(errbuf, errbuf_size, "Invalid profile image arguments");
        return -1;
    }

    if (profile_manager_thumb_path(profile_dir, thumb_path, sizeof(thumb_path)) == 0U) {
        profile_set_error(errbuf, errbuf_size, "Profile image path is too long");
        return -1;
    }
    if (profile_read_file(src_png_path, &png_data, &png_size, errbuf, errbuf_size) != 0) {
        return -1;
    }

    error = lodepng_decode32(&src_rgba, &src_w, &src_h, png_data, png_size);
    free(png_data);
    if (error != 0U) {
        profile_set_error(errbuf, errbuf_size, "PNG decode failed for %s: %s",
                          src_png_path, lodepng_error_text(error));
        return -1;
    }

    dst_rgba = profile_make_normalized_rgba(src_rgba,
                                            src_w,
                                            src_h,
                                            errbuf,
                                            errbuf_size);
    free(src_rgba);
    if (dst_rgba == NULL) {
        return -1;
    }

    fr = profile_manager_mount();
    if (fr != FR_OK) {
        profile_set_error(errbuf, errbuf_size, "SD mount failed: FRESULT=%u", (unsigned)fr);
        free(dst_rgba);
        return -1;
    }
    fr = f_open(&file, thumb_path, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr != FR_OK) {
        profile_set_error(errbuf, errbuf_size, "Open failed for %s: FRESULT=%u",
                          thumb_path, (unsigned)fr);
        free(dst_rgba);
        return -1;
    }

    if (profile_write_rgba_png(&file,
                               dst_rgba,
                               PROFILE_MANAGER_THUMB_W,
                               PROFILE_MANAGER_THUMB_H) == 0) {
        profile_set_error(errbuf, errbuf_size, "OK");
        rc = 0;
    } else {
        profile_set_error(errbuf, errbuf_size, "Write failed for %s", thumb_path);
    }

    (void)f_close(&file);
    free(dst_rgba);
    return rc;
}

void profile_manager_free_bgra32(uint32_t *pixels)
{
    free(pixels);
}
