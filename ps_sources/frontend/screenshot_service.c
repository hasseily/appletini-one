#include "screenshot_service.h"

#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "diskio.h"
#include "ff.h"
#include "xiltimer.h"

#include "../lib/crc32.h"
#include "../lib/fb16.h"

#include "apple_fb_handoff.h"
#include "compositor.h"
#include "compositor_layout.h"
#include "scanlines.h"
#include "usb_storage_service.h"

#define SCREENSHOT_DIR "0:/screenshots"
#define SCREENSHOT_OVERLAY_TICKS ((XTime)(3ULL * (uint64_t)COUNTS_PER_SECOND))
#define PNG_ADLER_MOD 65521U
#define PNG_ADLER_NMAX 5552U

typedef struct {
    const uint32_t *base;      /* BGRA32 surfaces (Apple frame ring) */
    const uint16_t *base565;   /* RGB565 surfaces (output ring); wins
                                * over `base` when non-NULL */
    uint32_t stride_pixels;
    uint32_t x_offset;
    uint8_t scale_x;
    uint8_t scale_y;
    uint8_t scanlines_mode;
} screenshot_surface_t;

static FATFS g_screenshot_fs;
static uint8_t g_png_row[1U + (COMP_OUT_WIDTH * 4U)];
static char g_overlay_text[32];
static XTime g_overlay_until;
static uint8_t g_overlay_active;
static uint8_t g_overlay_drawn_slots;
static uint8_t g_overlay_restore_slots;
static uint8_t g_scanlines_mode;
static DWORD g_fattime_override;
static uint8_t g_fattime_override_active;
static screenshot_service_rect_t g_overlay_rect;
static screenshot_service_sd_write_hook_t g_sd_write_hook;
static void *g_sd_write_hook_ctx;

static void result_set(screenshot_service_result_t *result,
                       int rc,
                       const char *path,
                       const char *fmt,
                       ...)
{
    va_list ap;

    if (result == NULL) {
        return;
    }

    result->rc = rc;
    if (path != NULL) {
        (void)snprintf(result->path, sizeof(result->path), "%s", path);
    } else {
        result->path[0] = '\0';
    }

    if (fmt == NULL) {
        result->message[0] = '\0';
        return;
    }

    va_start(ap, fmt);
    (void)vsnprintf(result->message, sizeof(result->message), fmt, ap);
    va_end(ap);
    result->message[sizeof(result->message) - 1U] = '\0';
}

static screenshot_service_rect_t overlay_rect_for_text(const char *text)
{
    const int scale = 3;
    screenshot_service_rect_t rect;
    const int text_w = (int)strlen(text) * FB16_BUILTIN_FONT_ADVANCE_X * scale;
    const int text_h = FB16_BUILTIN_FONT_HEIGHT * scale;

    rect.w = text_w + 36;
    rect.h = text_h + 24;
    rect.x = ((int)COMP_OUT_WIDTH - rect.w) / 2;
    rect.y = (int)COMP_OUT_HEIGHT - rect.h - 32;
    return rect;
}

static uint8_t output_slot_mask_for_fb(const uint16_t *fb)
{
    const uint8_t slot = comp_out_addr_to_slot((uint32_t)(uintptr_t)fb);

    return (slot < COMP_OUT_SLOT_COUNT) ? (uint8_t)(1U << slot) : 0U;
}

static void overlay_expire(void)
{
    if (g_overlay_active == 0U) {
        return;
    }

    g_overlay_active = 0U;
    g_overlay_restore_slots |= g_overlay_drawn_slots;
    g_overlay_drawn_slots = 0U;
    g_overlay_text[0] = '\0';
    if (g_overlay_restore_slots != 0U) {
        compositor_request_full_refresh();
    }
}

static void overlay_show(const char *text)
{
    XTime now = 0U;

    if (text == NULL) {
        text = "";
    }

    (void)snprintf(g_overlay_text, sizeof(g_overlay_text), "%s", text);
    XTime_GetTime(&now);
    g_overlay_until = now + SCREENSHOT_OVERLAY_TICKS;
    g_overlay_rect = overlay_rect_for_text(g_overlay_text);
    g_overlay_active = 1U;
    g_overlay_drawn_slots = 0U;
    g_overlay_restore_slots = 0U;
}

void screenshot_service_init(void)
{
    g_scanlines_mode = APPLETINI_SCANLINES_OFF;
    g_overlay_text[0] = '\0';
    g_overlay_until = 0U;
    g_overlay_active = 0U;
    g_overlay_drawn_slots = 0U;
    g_overlay_restore_slots = 0U;
    g_fattime_override = 0U;
    g_fattime_override_active = 0U;
    g_sd_write_hook = NULL;
    g_sd_write_hook_ctx = NULL;
    memset(&g_overlay_rect, 0, sizeof(g_overlay_rect));
}

void screenshot_service_set_scanlines(uint8_t mode)
{
    g_scanlines_mode = appletini_scanlines_clamp(mode);
}

void screenshot_service_set_sd_write_hook(screenshot_service_sd_write_hook_t hook,
                                          void *ctx)
{
    g_sd_write_hook = hook;
    g_sd_write_hook_ctx = ctx;
}

static void screenshot_service_note_local_sd_write_complete(void)
{
    if (g_sd_write_hook != NULL) {
        g_sd_write_hook(g_sd_write_hook_ctx);
    }
}

static void store_be32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value >> 24U);
    dst[1] = (uint8_t)(value >> 16U);
    dst[2] = (uint8_t)(value >> 8U);
    dst[3] = (uint8_t)value;
}

static void store_le16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8U);
}

static int write_exact(FIL *file, const void *data, uint32_t len)
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

static int png_chunk_begin(FIL *file,
                           const char type[4],
                           uint32_t length,
                           uint32_t *crc)
{
    uint8_t header[8];

    store_be32(header, length);
    header[4] = (uint8_t)type[0];
    header[5] = (uint8_t)type[1];
    header[6] = (uint8_t)type[2];
    header[7] = (uint8_t)type[3];

    if (write_exact(file, header, sizeof(header)) != 0) {
        return -1;
    }

    *crc = crc32_update(crc32_init(), type, 4U);
    return 0;
}

static int png_chunk_data(FIL *file,
                          uint32_t *crc,
                          const void *data,
                          uint32_t len)
{
    if (write_exact(file, data, len) != 0) {
        return -1;
    }
    *crc = crc32_update(*crc, data, len);
    return 0;
}

static int png_chunk_end(FIL *file, uint32_t crc)
{
    uint8_t out[4];

    store_be32(out, crc32_finish(crc));
    return write_exact(file, out, sizeof(out));
}

static void adler32_update(uint32_t *a_io,
                           uint32_t *b_io,
                           const uint8_t *data,
                           uint32_t len)
{
    uint32_t a = *a_io;
    uint32_t b = *b_io;

    while (len != 0U) {
        uint32_t chunk = (len > PNG_ADLER_NMAX) ? PNG_ADLER_NMAX : len;

        len -= chunk;
        while (chunk != 0U) {
            a += *data++;
            b += a;
            chunk--;
        }
        a %= PNG_ADLER_MOD;
        b %= PNG_ADLER_MOD;
    }

    *a_io = a;
    *b_io = b;
}

static uint8_t surface_scanline_blank(const screenshot_surface_t *surface, uint32_t y)
{
    uint8_t mode;
    uint32_t phase;

    if (surface == NULL || surface->scale_y <= 1U) {
        return 0U;
    }

    mode = appletini_scanlines_clamp(surface->scanlines_mode);
    phase = y % surface->scale_y;
    if (mode == APPLETINI_SCANLINES_OFF || phase == 0U) {
        return 0U;
    }

    if (surface->scale_y == 2U) {
        return (mode >= APPLETINI_SCANLINES_MEDIUM) ? 1U : 0U;
    }
    if (surface->scale_y == 4U) {
        return (phase >= (4U - (uint32_t)mode)) ? 1U : 0U;
    }

    return 0U;
}

static void fill_png_row(uint8_t *row,
                         const screenshot_surface_t *surface,
                         uint32_t y,
                         uint32_t width)
{
    const uint32_t row_off =
        ((y / surface->scale_y) * surface->stride_pixels) +
        surface->x_offset;
    const uint32_t *src = (surface->base565 == NULL)
        ? surface->base + row_off : NULL;
    const uint16_t *src565 = (surface->base565 != NULL)
        ? surface->base565 + row_off : NULL;
    const uint8_t blank = surface_scanline_blank(surface, y);

    row[0] = 0U;
    for (uint32_t x = 0U; x < width; ++x) {
        const uint32_t bgra =
            (blank != 0U) ? 0U
            : (src565 != NULL)
                ? fb16_to_bgra32(src565[x / surface->scale_x])
                : src[x / surface->scale_x];
        uint8_t *dst = &row[1U + (x * 4U)];

        dst[0] = (uint8_t)(bgra >> 16U);
        dst[1] = (uint8_t)(bgra >> 8U);
        dst[2] = (uint8_t)bgra;
        dst[3] = 0xFFU;
    }
}

static int write_png_rgba(FIL *file,
                          const screenshot_surface_t *surface,
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

    if (surface == NULL ||
        (surface->base == NULL && surface->base565 == NULL) ||
        width == 0U || height == 0U ||
        surface->scale_x == 0U || surface->scale_y == 0U ||
        row_len > (uint32_t)sizeof(g_png_row)) {
        return -1;
    }

    if (write_exact(file, signature, sizeof(signature)) != 0) {
        return -1;
    }

    memset(ihdr, 0, sizeof(ihdr));
    store_be32(&ihdr[0], width);
    store_be32(&ihdr[4], height);
    ihdr[8] = 8U;  /* bit depth */
    ihdr[9] = 6U;  /* RGBA */

    if (png_chunk_begin(file, "IHDR", sizeof(ihdr), &crc) != 0 ||
        png_chunk_data(file, &crc, ihdr, sizeof(ihdr)) != 0 ||
        png_chunk_end(file, crc) != 0) {
        return -1;
    }

    if (png_chunk_begin(file, "IDAT", idat_len, &crc) != 0 ||
        png_chunk_data(file, &crc, zlib_header, sizeof(zlib_header)) != 0) {
        return -1;
    }

    for (uint32_t y = 0U; y < height; ++y) {
        uint8_t block_header[5];
        const uint16_t len16 = (uint16_t)row_len;

        fill_png_row(g_png_row, surface, y, width);
        block_header[0] = (uint8_t)((y + 1U == height) ? 0x01U : 0x00U);
        store_le16(&block_header[1], len16);
        store_le16(&block_header[3], (uint16_t)~len16);

        if (png_chunk_data(file, &crc, block_header, sizeof(block_header)) != 0 ||
            png_chunk_data(file, &crc, g_png_row, row_len) != 0) {
            return -1;
        }
        adler32_update(&adler_a, &adler_b, g_png_row, row_len);
    }

    {
        uint8_t adler[4];
        store_be32(adler, (adler_b << 16U) | adler_a);
        if (png_chunk_data(file, &crc, adler, sizeof(adler)) != 0 ||
            png_chunk_end(file, crc) != 0) {
            return -1;
        }
    }

    if (png_chunk_begin(file, "IEND", 0U, &crc) != 0 ||
        png_chunk_end(file, crc) != 0) {
        return -1;
    }

    return 0;
}

static FRESULT mount_sd(void)
{
    FRESULT fr;

    fr = f_mount(&g_screenshot_fs, "0:/", 1U);
    if (fr != FR_OK) {
        (void)disk_initialize(0);
        (void)f_mount((FATFS *)0, "0:/", 0U);
        fr = f_mount(&g_screenshot_fs, "0:/", 1U);
    }

    return fr;
}

static FRESULT ensure_screenshot_dir(void)
{
    FRESULT fr = mount_sd();

    if (fr != FR_OK) {
        return fr;
    }

    fr = f_mkdir(SCREENSHOT_DIR);
    return (fr == FR_EXIST) ? FR_OK : fr;
}

static uint8_t rtc_is_timestamp_valid(const rtc_pcf8563_time_t *rtc)
{
    return (rtc != NULL &&
            rtc->valid != 0U &&
            rtc->month >= 1U && rtc->month <= 12U &&
            rtc->day >= 1U && rtc->day <= 31U &&
            rtc->hour <= 23U &&
            rtc->min <= 59U &&
            rtc->sec <= 59U) ? 1U : 0U;
}

static void make_timestamp(char *out, size_t out_size, const rtc_pcf8563_time_t *rtc)
{
    if (rtc_is_timestamp_valid(rtc) != 0U) {
        (void)snprintf(out,
                       out_size,
                       "%04u%02u%02u-%02u%02u%02u",
                       (unsigned)rtc->year,
                       (unsigned)rtc->month,
                       (unsigned)rtc->day,
                       (unsigned)rtc->hour,
                       (unsigned)rtc->min,
                       (unsigned)rtc->sec);
    } else {
        XTime now = 0U;
        uint64_t seconds = 0ULL;

        XTime_GetTime(&now);
        if (COUNTS_PER_SECOND != 0U) {
            seconds = ((uint64_t)now / (uint64_t)COUNTS_PER_SECOND);
        }
        (void)snprintf(out, out_size, "uptime-%010llu",
                       (unsigned long long)seconds);
    }
}

static uint16_t fat_date_from_rtc(const rtc_pcf8563_time_t *rtc)
{
    uint16_t year = rtc->year;

    if (year < 1980U) {
        year = 1980U;
    } else if (year > 2107U) {
        year = 2107U;
    }

    return (uint16_t)(((uint16_t)(year - 1980U) << 9U) |
                      ((uint16_t)rtc->month << 5U) |
                      (uint16_t)rtc->day);
}

static uint16_t fat_time_from_rtc(const rtc_pcf8563_time_t *rtc)
{
    return (uint16_t)(((uint16_t)rtc->hour << 11U) |
                      ((uint16_t)rtc->min << 5U) |
                      (uint16_t)(rtc->sec / 2U));
}

static DWORD fat_datetime_from_rtc(const rtc_pcf8563_time_t *rtc)
{
    return ((DWORD)fat_date_from_rtc(rtc) << 16U) |
           (DWORD)fat_time_from_rtc(rtc);
}

static DWORD fat_datetime_fallback(void)
{
    return ((DWORD)(2010U - 1980U) << 25U) |
           ((DWORD)1U << 21U) |
           ((DWORD)1U << 16U);
}

/* Last good RTC reading, refreshed by the main loop's sensor poll. */
static DWORD g_fattime_cached;
static uint8_t g_fattime_cached_valid;

void screenshot_service_update_fattime_from_rtc(const rtc_pcf8563_time_t *rtc)
{
    if (rtc == NULL || rtc_is_timestamp_valid(rtc) == 0U) {
        return;
    }
    g_fattime_cached = fat_datetime_from_rtc(rtc);
    g_fattime_cached_valid = 1U;
}

DWORD appletini_fatfs_get_fattime(void)
{
    if (g_fattime_override_active != 0U) {
        return g_fattime_override;
    }
    if (g_fattime_cached_valid != 0U) {
        return g_fattime_cached;
    }

    return fat_datetime_fallback();
}

static void fat_timestamp_override_begin(const rtc_pcf8563_time_t *rtc)
{
    if (rtc_is_timestamp_valid(rtc) == 0U) {
        g_fattime_override_active = 0U;
        return;
    }

    g_fattime_override = fat_datetime_from_rtc(rtc);
    g_fattime_override_active = 1U;
}

static void fat_timestamp_override_end(void)
{
    g_fattime_override_active = 0U;
}

static FRESULT open_screenshot_file(FIL *file,
                                    const char *timestamp,
                                    const char *suffix,
                                    char *path,
                                    size_t path_size)
{
    FRESULT fr;

    if (file == NULL || timestamp == NULL || suffix == NULL ||
        path == NULL || path_size == 0U) {
        return FR_INVALID_OBJECT;
    }

    (void)snprintf(path,
                   path_size,
                   SCREENSHOT_DIR "/%s-%s.png",
                   timestamp,
                   suffix);

    fr = ensure_screenshot_dir();
    if (fr != FR_OK) {
        return fr;
    }

    fr = f_open(file, path, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr == FR_OK) {
        return fr;
    }

    fr = mount_sd();
    if (fr != FR_OK) {
        return fr;
    }

    (void)ensure_screenshot_dir();
    return f_open(file, path, FA_CREATE_ALWAYS | FA_WRITE);
}

static int save_surface_png(const screenshot_surface_t *surface,
                            uint32_t width,
                            uint32_t height,
                            const char *timestamp,
                            const char *suffix,
                            const rtc_pcf8563_time_t *rtc,
                            screenshot_service_result_t *result)
{
    FIL file;
    FRESULT fr;
    char path[SCREENSHOT_SERVICE_PATH_LEN];
    uint8_t usb_storage_was_connected;
    int rc;

    usb_storage_was_connected = usb_storage_service_disconnect();
    fat_timestamp_override_begin(rtc);
    fr = open_screenshot_file(&file, timestamp, suffix, path, sizeof(path));
    if (fr != FR_OK) {
        fat_timestamp_override_end();
        screenshot_service_note_local_sd_write_complete();
        if (usb_storage_was_connected != 0U) {
            usb_storage_service_connect();
        }
        result_set(result, -(int)fr, path, "OPEN FAILED FRESULT=%u", (unsigned)fr);
        return -(int)fr;
    }

    rc = write_png_rgba(&file, surface, width, height);
    fr = f_close(&file);
    fat_timestamp_override_end();
    screenshot_service_note_local_sd_write_complete();
    if (usb_storage_was_connected != 0U) {
        usb_storage_service_connect();
    }
    if (rc != 0) {
        result_set(result, rc, path, "PNG WRITE FAILED");
        return rc;
    }
    if (fr != FR_OK) {
        result_set(result, -(int)fr, path, "CLOSE FAILED FRESULT=%u", (unsigned)fr);
        return -(int)fr;
    }

    result_set(result, 0, path, "OK");
    return 0;
}

static int save_a2_png(const char *timestamp,
                       const rtc_pcf8563_time_t *rtc,
                       screenshot_service_result_t *result)
{
    uint8_t slot = apple_fb_reader_claim();
    uint32_t mode = apple_fb_reader_display_mode();
    const uint32_t video_settings = apple_fb_video_settings_get();
    screenshot_surface_t surface;
    uint32_t width;
    uint32_t height;

    if (slot == APPLE_FB_NO_SLOT || slot >= COMP_APPLE_SLOT_COUNT) {
        slot = (uint8_t)g_compositor_last_apple_slot;
        mode = g_compositor_last_apple_mode;
    }

    if (slot == APPLE_FB_NO_SLOT || slot >= COMP_APPLE_SLOT_COUNT) {
        result_set(result, -1, NULL, "NO APPLE FRAME");
        return -1;
    }

    surface.base = (const uint32_t *)(uintptr_t)comp_apple_slot_addr[slot];
    surface.base565 = NULL;
    surface.scale_x = 2U;
    surface.scanlines_mode = g_scanlines_mode;
    if (mode == APPLE_FB_DISPLAY_MODE_SHR) {
        surface.stride_pixels = COMP_APPLE_SHR_ROW_PIXELS;
        surface.x_offset = 0U;
        surface.scale_y = 2U;
        width = COMP_APPLE_SHR_WIDTH * 2U;
        height = COMP_APPLE_SHR_HEIGHT * 2U;
    } else {
        surface.stride_pixels = COMP_APPLE_ROW_PIXELS;
        surface.scale_y = 4U;
        if (apple_video_settings_border_enabled(video_settings) != 0U) {
            surface.x_offset = COMP_APPLE_LEFT_BORDER_PIXELS;
            width = COMP_APPLE_VISIBLE_WIDTH * 2U;
            height = COMP_APPLE_VISIBLE_HEIGHT * 4U;
        } else {
            surface.base += COMP_APPLE_ACTIVE_Y * COMP_APPLE_ROW_PIXELS;
            surface.x_offset = COMP_APPLE_ACTIVE_X;
            width = COMP_APPLE_WIDTH * 2U;
            height = COMP_APPLE_HEIGHT * 4U;
        }
    }

    return save_surface_png(&surface, width, height, timestamp, "a2", rtc, result);
}

static int save_1080p_png(const char *timestamp,
                          const rtc_pcf8563_time_t *rtc,
                          screenshot_service_result_t *result)
{
    uint8_t slot = 0xFFU;
    screenshot_surface_t surface;

    surface.base = NULL;
    surface.base565 = compositor_latched_framebuffer(&slot);
    surface.stride_pixels = COMP_OUT_WIDTH;
    surface.x_offset = 0U;
    surface.scale_x = 1U;
    surface.scale_y = 1U;
    surface.scanlines_mode = APPLETINI_SCANLINES_OFF;

    if (surface.base565 == NULL || slot >= COMP_OUT_SLOT_COUNT) {
        result_set(result, -1, NULL, "NO OUTPUT FRAME");
        return -1;
    }

    return save_surface_png(&surface,
                            COMP_OUT_WIDTH,
                            COMP_OUT_HEIGHT,
                            timestamp,
                            "1080p",
                            rtc,
                            result);
}

int screenshot_service_save(screenshot_service_kind_t kind,
                            const rtc_pcf8563_time_t *rtc,
                            screenshot_service_result_t *result)
{
    char timestamp[32];
    int rc;

    if (result != NULL) {
        memset(result, 0, sizeof(*result));
    }

    make_timestamp(timestamp, sizeof(timestamp), rtc);
    compositor_set_paused(1U);
    if (kind == SCREENSHOT_SERVICE_KIND_A2) {
        rc = save_a2_png(timestamp, rtc, result);
    } else if (kind == SCREENSHOT_SERVICE_KIND_1080P) {
        rc = save_1080p_png(timestamp, rtc, result);
    } else {
        result_set(result, -1, NULL, "INVALID SCREENSHOT KIND");
        rc = -1;
    }
    compositor_set_paused(0U);

    if (rc == 0) {
        overlay_show("SCREENSHOT SAVED");
    } else if (result != NULL && result->message[0] != '\0') {
        overlay_show(result->message);
    } else {
        overlay_show("SCREENSHOT FAILED");
    }
    compositor_request_full_refresh();
    return rc;
}

void screenshot_service_poll(void)
{
    XTime now = 0U;

    if (g_overlay_active == 0U) {
        return;
    }

    XTime_GetTime(&now);
    if ((int64_t)(g_overlay_until - now) <= 0) {
        overlay_expire();
    }
}

uint8_t screenshot_service_restore_rect_for_frame(uint16_t *fb,
                                                  screenshot_service_rect_t *rect)
{
    const uint8_t slot_mask = output_slot_mask_for_fb(fb);

    if (slot_mask == 0U || (g_overlay_restore_slots & slot_mask) == 0U) {
        return 0U;
    }

    if (rect != NULL) {
        *rect = g_overlay_rect;
    }
    g_overlay_restore_slots = (uint8_t)(g_overlay_restore_slots & (uint8_t)~slot_mask);
    return 1U;
}

void screenshot_service_draw_overlay(uint16_t *fb)
{
    const char *text = g_overlay_text;
    const uint8_t slot_mask = output_slot_mask_for_fb(fb);
    const int scale = 3;
    const int x = g_overlay_rect.x;
    const int y = g_overlay_rect.y;
    const int box_w = g_overlay_rect.w;
    const int box_h = g_overlay_rect.h;

    if (fb == NULL || g_overlay_active == 0U || text[0] == '\0') {
        return;
    }

    fb16_fill_rect(fb, x, y, box_w, box_h, FB16_COLOR_BLACK);
    fb16_rect(fb, x, y, box_w, box_h, FB16_COLOR_GREEN);
    fb16_string_scaled(fb,
                       x + 18,
                       y + 12,
                       text,
                       FB16_COLOR_WHITE,
                       FB16_COLOR_BLACK,
                       scale);
    if (slot_mask != 0U) {
        g_overlay_drawn_slots = (uint8_t)(g_overlay_drawn_slots | slot_mask);
    }
}
