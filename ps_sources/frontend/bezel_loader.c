#include "bezel_loader.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "ff.h"
#include "diskio.h"

#define LODEPNG_NO_COMPILE_CPP
#define LODEPNG_NO_COMPILE_ENCODER
#define LODEPNG_NO_COMPILE_DISK

#include "../lib/lodepng.h"

static FATFS g_bezel_fs;
static int g_bezel_fs_mounted = 0;

static FRESULT mount_sd_retry(FATFS *fs, const char *path, DSTATUS *status_before, DSTATUS *status_after_init)
{
    FRESULT fr = f_mount(fs, path, 1U);

    if (fr == FR_OK) {
        if (status_before != NULL) {
            *status_before = disk_status(0);
        }
        if (status_after_init != NULL) {
            *status_after_init = disk_status(0);
        }
        return fr;
    }

    if (status_before != NULL) {
        *status_before = disk_status(0);
    }
    if (status_after_init != NULL) {
        *status_after_init = disk_initialize(0);
    } else {
        (void)disk_initialize(0);
    }

    (void)f_mount(0, path, 0U);
    return f_mount(fs, path, 1U);
}

static void set_error(char *errbuf, size_t errbuf_size, const char *fmt, ...)
{
    va_list ap;

    if (errbuf == NULL || errbuf_size == 0U) {
        return;
    }

    va_start(ap, fmt);
    vsnprintf(errbuf, errbuf_size, fmt, ap);
    va_end(ap);
    errbuf[errbuf_size - 1U] = '\0';
}

static int ensure_sd_mounted(char *errbuf, size_t errbuf_size)
{
    FRESULT fr;
    DSTATUS status_before = 0U;
    DSTATUS status_after_init = 0U;

    if (g_bezel_fs_mounted != 0) {
        return 0;
    }

    fr = mount_sd_retry(&g_bezel_fs, "0:/", &status_before, &status_after_init);
    if (fr != FR_OK) {
        set_error(errbuf,
                  errbuf_size,
                  "SD mount failed: FRESULT=%u status=0x%02X init=0x%02X",
                  (unsigned)fr,
                  (unsigned)status_before,
                  (unsigned)status_after_init);
        return -1;
    }

    g_bezel_fs_mounted = 1;
    return 0;
}

static int read_file_from_sd(const char *path,
                             unsigned char **out_data,
                             size_t *out_size,
                             char *errbuf,
                             size_t errbuf_size)
{
    FIL file;
    FRESULT fr;
    unsigned char *data = NULL;
    size_t offset = 0U;
    FSIZE_t file_size = 0U;

    if (ensure_sd_mounted(errbuf, errbuf_size) != 0) {
        return -1;
    }

    fr = f_open(&file, path, FA_READ);
    if (fr != FR_OK) {
        set_error(errbuf, errbuf_size, "Open failed for %s: FRESULT=%u", path, (unsigned)fr);
        return -1;
    }

    file_size = f_size(&file);
    if (file_size == 0U) {
        (void)f_close(&file);
        set_error(errbuf, errbuf_size, "PNG file is empty: %s", path);
        return -1;
    }
    if (file_size > (FSIZE_t)SIZE_MAX) {
        (void)f_close(&file);
        set_error(errbuf, errbuf_size, "PNG file is too large: %s", path);
        return -1;
    }

    data = (unsigned char *)malloc((size_t)file_size);
    if (data == NULL) {
        (void)f_close(&file);
        set_error(errbuf, errbuf_size, "Out of memory reading %s", path);
        return -1;
    }

    while (offset < (size_t)file_size) {
        const size_t remaining = (size_t)file_size - offset;
        const UINT chunk = (remaining > (size_t)UINT_MAX) ? UINT_MAX : (UINT)remaining;
        UINT bytes_read = 0U;

        fr = f_read(&file, data + offset, chunk, &bytes_read);
        if (fr != FR_OK) {
            free(data);
            (void)f_close(&file);
            set_error(errbuf, errbuf_size, "Read failed for %s: FRESULT=%u", path, (unsigned)fr);
            return -1;
        }
        if (bytes_read == 0U) {
            free(data);
            (void)f_close(&file);
            set_error(errbuf, errbuf_size, "Unexpected EOF while reading %s", path);
            return -1;
        }

        offset += (size_t)bytes_read;
    }

    (void)f_close(&file);
    *out_data = data;
    *out_size = offset;
    return 0;
}

int bezel_loader_decode_png_rgb565(const unsigned char *png_data,
                                   size_t png_size,
                                   const char *label,
                                   uint16_t **out_pixels,
                                   unsigned *out_w,
                                   unsigned *out_h,
                                   char *errbuf,
                                   size_t errbuf_size)
{
    unsigned char *rgba = NULL;
    uint16_t *px = NULL;
    unsigned w = 0U;
    unsigned h = 0U;
    unsigned error;
    size_t pixel_count;
    size_t i;

    if (png_data == NULL || png_size == 0U || out_pixels == NULL || out_w == NULL || out_h == NULL) {
        set_error(errbuf, errbuf_size, "Invalid bezel loader arguments");
        return -1;
    }
    if (label == NULL) {
        label = "bezel PNG";
    }

    *out_pixels = NULL;
    *out_w = 0U;
    *out_h = 0U;

    error = lodepng_decode32(&rgba, &w, &h, png_data, png_size);
    if (error != 0U) {
        set_error(errbuf, errbuf_size, "PNG decode failed for %s: %s",
                  label, lodepng_error_text(error));
        return -1;
    }

    if (w != 1920U || h == 0U || h > 1080U) {
        free(rgba);
        set_error(errbuf, errbuf_size,
                  "PNG must be 1920 wide and <=1080 high, got %ux%u for %s",
                  w, h, label);
        return -1;
    }

    pixel_count = (size_t)w * (size_t)h;
    px = (uint16_t *)malloc(pixel_count * sizeof(uint16_t));
    if (px == NULL) {
        free(rgba);
        set_error(errbuf, errbuf_size, "Out of memory converting bezel PNG");
        return -1;
    }

    /* Alpha-premultiply against black, then pack RGB565 -- the bezel is
     * stored at the exact depth the output surface and DVI pins carry,
     * so drawing it is a straight row memcpy. */
    for (i = 0U; i < pixel_count; ++i) {
        const unsigned char a = rgba[(i * 4U) + 3U];
        const unsigned char r = (unsigned char)((rgba[(i * 4U) + 0U] * a + 127U) / 255U);
        const unsigned char g = (unsigned char)((rgba[(i * 4U) + 1U] * a + 127U) / 255U);
        const unsigned char b = (unsigned char)((rgba[(i * 4U) + 2U] * a + 127U) / 255U);

        px[i] = (uint16_t)((((uint16_t)r & 0xF8U) << 8) |
                           (((uint16_t)g & 0xFCU) << 3) |
                           (((uint16_t)b) >> 3));
    }

    free(rgba);
    *out_pixels = px;
    *out_w = w;
    *out_h = h;
    set_error(errbuf, errbuf_size, "OK");
    return 0;
}

int bezel_loader_load_png_rgb565(const char *path,
                                 uint16_t **out_pixels,
                                 unsigned *out_w,
                                 unsigned *out_h,
                                 char *errbuf,
                                 size_t errbuf_size)
{
    unsigned char *png_data = NULL;
    size_t png_size = 0U;
    int rc;

    if (path == NULL) {
        set_error(errbuf, errbuf_size, "Invalid bezel loader path");
        return -1;
    }

    if (read_file_from_sd(path, &png_data, &png_size, errbuf, errbuf_size) != 0) {
        return -1;
    }

    rc = bezel_loader_decode_png_rgb565(png_data,
                                        png_size,
                                        path,
                                        out_pixels,
                                        out_w,
                                        out_h,
                                        errbuf,
                                        errbuf_size);
    free(png_data);
    return rc;
}

void bezel_loader_free_rgb565(uint16_t *pixels)
{
    free(pixels);
}
