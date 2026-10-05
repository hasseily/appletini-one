#include "screenshot_service.h"

#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diskio.h"
#include "ff.h"
#include "xiltimer.h"

#include "../lib/crc32.h"
#include "../lib/fb16.h"

#include "apple_fb_handoff.h"
#include "compositor.h"
#include "compositor_layout.h"
#include "display_modes.h"
#include "scanlines.h"
#include "usb_storage_service.h"

#define SCREENSHOT_DIR "0:/screenshots"
#define SCREENSHOT_OVERLAY_TICKS ((XTime)(3ULL * (uint64_t)COUNTS_PER_SECOND))
#define PNG_ADLER_MOD 65521U
#define PNG_ADLER_NMAX 5552U
#define SCREENSHOT_ROWS_PER_POLL 2U
#define SCREENSHOT_NAME_ATTEMPTS 64U

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
static uint8_t g_png_row[1U + (COMP_OUT_MAX_WIDTH * 4U)];

typedef struct {
    uint32_t crc, adler_a, adler_b;
} screenshot_png_t;

typedef enum { SHOT_IDLE, SHOT_OPEN, SHOT_ROWS, SHOT_FINISH, SHOT_RENAME } shot_state_t;
static struct {
    shot_state_t state;
    void *pixels;
    screenshot_surface_t surface;
    uint32_t width, height, next_row;
    FSIZE_t offset;
    FIL file;
    uint8_t file_open, owns_part, storage_session, usb_was_connected;
    uint8_t result_pending;
    uint32_t attempts;
    char timestamp[32], suffix[16];
    char temporary[SCREENSHOT_SERVICE_PATH_LEN];
    char final[SCREENSHOT_SERVICE_PATH_LEN - 5U]; /* Room for ".part". */
    rtc_pcf8563_time_t rtc;
    screenshot_png_t png;
    screenshot_service_result_t result;
} g_shot;
static uint32_t g_shot_serial;

/* Two transient on-screen overlays, each a self-contained instance:
 * BOTTOM = screenshot confirmations, TOP = TransWarp speed notices, so
 * the two never overlap. Same fade-and-restore machinery for both. */
typedef enum {
    OVERLAY_BOTTOM = 0,   /* screenshots */
    OVERLAY_TOP    = 1,   /* TransWarp speed notices */
    OVERLAY_COUNT
} overlay_slot_e;

typedef struct {
    char    text[40];
    XTime   until;
    uint8_t active;
    uint8_t drawn_slots;
    uint8_t restore_slots;
    uint8_t anchor_top;   /* 1 = top of screen, 0 = bottom */
    screenshot_service_rect_t rect;
    /* Footprint pending restore. Distinct from `rect` so replacing a
     * still-visible box (a different-width speed notice) restores the OLD
     * box's area, not the new one's -- otherwise the old box's
     * non-overlapping pixels orphan on screen. */
    screenshot_service_rect_t restore_rect;
} overlay_t;

static overlay_t g_overlays[OVERLAY_COUNT];

void screenshot_service_clear_overlays(void)
{
    memset(g_overlays, 0, sizeof(g_overlays));
    g_overlays[OVERLAY_TOP].anchor_top = 1U;
}
static uint8_t g_scanlines_mode;
static DWORD g_fattime_override;
static uint8_t g_fattime_override_active;
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

static screenshot_service_rect_t overlay_rect_for_text(const char *text,
                                                       uint8_t anchor_top)
{
    const int scale = FB16_WIDTH < 1200 ? 2 : 3;
    screenshot_service_rect_t rect;
    const int text_w = (int)strlen(text) * FB16_BUILTIN_FONT_ADVANCE_X * scale;
    const int text_h = FB16_BUILTIN_FONT_HEIGHT * scale;

    rect.w = text_w + 36;
    rect.h = text_h + 24;
    rect.x = ((int)COMP_OUT_WIDTH - rect.w) / 2;
    rect.y = (anchor_top != 0U)
                 ? 32
                 : (int)COMP_OUT_HEIGHT - rect.h - 32;
    return rect;
}

static uint8_t output_slot_mask_for_fb(const uint16_t *fb)
{
    const uint8_t slot = comp_out_addr_to_slot((uint32_t)(uintptr_t)fb);

    return (slot < COMP_OUT_SLOT_COUNT) ? (uint8_t)(1U << slot) : 0U;
}

static void overlay_expire(overlay_t *ov)
{
    if (ov->active == 0U) {
        return;
    }

    ov->active = 0U;
    ov->restore_rect = ov->rect;
    ov->restore_slots |= ov->drawn_slots;
    ov->drawn_slots = 0U;
    ov->text[0] = '\0';
    if (ov->restore_slots != 0U) {
        compositor_request_full_refresh();
    }
}

static void overlay_show(overlay_t *ov, const char *text)
{
    XTime now = 0U;
    const uint8_t was_active = ov->active;

    if (text == NULL) {
        text = "";
    }

    /* Replacing a still-visible box: queue its CURRENT footprint for
     * restore (with its own rect) before overwriting, so the old box is
     * cleaned even when the new one is a different size or position. */
    if (was_active != 0U) {
        ov->restore_rect = ov->rect;
        ov->restore_slots |= ov->drawn_slots;
    }

    (void)snprintf(ov->text, sizeof(ov->text), "%s", text);
    XTime_GetTime(&now);
    ov->until = now + SCREENSHOT_OVERLAY_TICKS;
    ov->rect = overlay_rect_for_text(ov->text, ov->anchor_top);
    ov->active = 1U;
    ov->drawn_slots = 0U;
    if (was_active != 0U) {
        compositor_request_full_refresh();
    }
}

void screenshot_service_show_notice(const char *text)
{
    overlay_show(&g_overlays[OVERLAY_TOP], text);
}

void screenshot_service_show_confirmation(const char *text)
{
    overlay_show(&g_overlays[OVERLAY_BOTTOM], text);
}

void screenshot_service_init(void)
{
    g_scanlines_mode = APPLETINI_SCANLINES_OFF;
    memset(g_overlays, 0, sizeof(g_overlays));
    g_overlays[OVERLAY_TOP].anchor_top = 1U;
    g_fattime_override = 0U;
    g_fattime_override_active = 0U;
    g_sd_write_hook = NULL;
    g_sd_write_hook_ctx = NULL;
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

/* Shared by every service that writes the SD card locally (screenshots,
 * printouts) so the USB0 mass-storage host cache gets invalidated. */
void screenshot_service_note_local_sd_write_complete(void)
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

static int png_begin(FIL *file, screenshot_png_t *png,
                     uint32_t width, uint32_t height)
{
    static const uint8_t signature[8] = {
        0x89U, 'P', 'N', 'G', 0x0DU, 0x0AU, 0x1AU, 0x0AU
    };
    uint8_t ihdr[13];
    uint32_t crc;
    uint8_t zlib_header[2] = {0x78U, 0x01U};
    const uint32_t row_len = 1U + (width * 4U);
    const uint32_t idat_len = 2U + (height * (5U + row_len)) + 4U;

    if (width == 0U || height == 0U ||
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

    png->crc = crc;
    png->adler_a = 1U;
    png->adler_b = 0U;
    return 0;
}

static int png_row(FIL *file, screenshot_png_t *png,
                   const screenshot_surface_t *surface,
                   uint32_t y, uint32_t width, uint32_t height)
{
    uint8_t block_header[5];
    const uint16_t row_len = (uint16_t)(1U + width * 4U);

    fill_png_row(g_png_row, surface, y, width);
    block_header[0] = (uint8_t)((y + 1U == height) ? 0x01U : 0x00U);
    store_le16(&block_header[1], row_len);
    store_le16(&block_header[3], (uint16_t)~row_len);
    if (png_chunk_data(file, &png->crc, block_header, sizeof(block_header)) != 0 ||
        png_chunk_data(file, &png->crc, g_png_row, row_len) != 0) {
        return -1;
    }
    adler32_update(&png->adler_a, &png->adler_b, g_png_row, row_len);
    return 0;
}

static int png_end(FIL *file, screenshot_png_t *png)
{
    uint32_t crc;
    {
        uint8_t adler[4];
        store_be32(adler, (png->adler_b << 16U) | png->adler_a);
        if (png_chunk_data(file, &png->crc, adler, sizeof(adler)) != 0 ||
            png_chunk_end(file, png->crc) != 0) {
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
    /* Reuse the volume: remounting invalidates other services' open files. */
    FRESULT fr = f_mkdir(SCREENSHOT_DIR);
    if (fr == FR_NOT_ENABLED) {
        fr = mount_sd();
        if (fr == FR_OK) {
            fr = f_mkdir(SCREENSHOT_DIR);
        }
    }
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

static void next_final_path(void)
{
    (void)snprintf(g_shot.final, sizeof(g_shot.final),
                   SCREENSHOT_DIR "/%s-%s-%08lx.png", g_shot.timestamp,
                   g_shot.suffix, (unsigned long)g_shot_serial++);
}

static FRESULT shot_close(void)
{
    FRESULT fr = FR_OK;
    if (g_shot.file_open != 0U) {
        fr = f_close(&g_shot.file);
        if (fr == FR_OK) {
            g_shot.file_open = 0U;
        }
    }
    return fr;
}

static void shot_complete(int rc, const char *message)
{
    FRESULT cleanup = shot_close();
    if (cleanup == FR_OK && g_shot.owns_part != 0U) {
        cleanup = f_unlink(g_shot.temporary);
        if (cleanup == FR_OK || cleanup == FR_NO_FILE) {
            cleanup = FR_OK;
            g_shot.owns_part = 0U;
        }
    }
    if (cleanup != FR_OK) {
        result_set(&g_shot.result, -(int)cleanup, g_shot.temporary,
                   "%s; PART CLEANUP FAILED=%u", message, (unsigned)cleanup);
    } else {
        result_set(&g_shot.result, rc, rc == 0 ? g_shot.final : NULL,
                   "%s", message);
    }
    fat_timestamp_override_end();
    if (g_shot.storage_session != 0U) {
        screenshot_service_note_local_sd_write_complete();
        if (g_shot.usb_was_connected != 0U) {
            usb_storage_service_connect();
        }
    }
    free(g_shot.pixels);
    g_shot.pixels = NULL;
    g_shot.state = SHOT_IDLE;
    g_shot.result_pending = 1U;
    overlay_show(&g_overlays[OVERLAY_BOTTOM], g_shot.result.rc == 0 ?
                 "SCREENSHOT SAVED" : g_shot.result.message);
    compositor_request_full_refresh();
}

/* Other CPU0 services can remount FatFS between polls. Reopen each batch and
 * check its length rather than retaining an invalid FIL across a remount. */
static FRESULT shot_resume_file(void)
{
    FRESULT fr = f_open(&g_shot.file, g_shot.temporary, FA_WRITE);
    if (fr != FR_OK) {
        return fr;
    }
    g_shot.file_open = 1U;
    if (f_size(&g_shot.file) != g_shot.offset) {
        return FR_INVALID_OBJECT;
    }
    fr = f_lseek(&g_shot.file, g_shot.offset);
    return (fr == FR_OK && f_tell(&g_shot.file) != g_shot.offset) ?
            FR_DISK_ERR : fr;
}

static void shot_poll(void)
{
    FRESULT fr;
    int rc = 0;
    if (g_shot.state == SHOT_IDLE) {
        return;
    }
    fat_timestamp_override_begin(&g_shot.rtc);
    if (g_shot.state == SHOT_OPEN) {
        g_shot.usb_was_connected = usb_storage_service_disconnect();
        g_shot.storage_session = 1U;
        fr = ensure_screenshot_dir();
        if (fr != FR_OK) {
            shot_complete(-(int)fr, "SD DIRECTORY FAILED");
            return;
        }
        for (g_shot.attempts = 0U;
             g_shot.attempts < SCREENSHOT_NAME_ATTEMPTS; ++g_shot.attempts) {
            next_final_path();
            (void)snprintf(g_shot.temporary, sizeof(g_shot.temporary),
                           "%s.part", g_shot.final);
            fr = f_open(&g_shot.file, g_shot.temporary, FA_CREATE_NEW | FA_WRITE);
            if (fr != FR_EXIST) {
                break;
            }
        }
        if (fr != FR_OK) {
            shot_complete(-(int)fr, "TEMP FILE CREATE FAILED");
            return;
        }
        g_shot.file_open = 1U;
        g_shot.owns_part = 1U;
        rc = png_begin(&g_shot.file, &g_shot.png, g_shot.width, g_shot.height);
        g_shot.state = SHOT_ROWS;
        g_shot.attempts = 0U;
    } else if (g_shot.state == SHOT_RENAME) {
        /* FatFS rename never replaces an existing file. A completed .part
         * remains private until a successful close and non-clobber rename. */
        fr = f_rename(g_shot.temporary, g_shot.final);
        if (fr == FR_EXIST && ++g_shot.attempts < SCREENSHOT_NAME_ATTEMPTS) {
            next_final_path();
            fat_timestamp_override_end();
            return;
        }
        if (fr == FR_OK) {
            g_shot.owns_part = 0U;
        }
        shot_complete(fr == FR_OK ? 0 : -(int)fr,
                      fr == FR_OK ? "OK" : "PNG PUBLISH FAILED");
        return;
    } else {
        fr = shot_resume_file();
        if (fr != FR_OK) {
            shot_complete(-(int)fr, "TEMP FILE RESUME FAILED");
            return;
        }
        if (g_shot.state == SHOT_ROWS) {
            for (uint32_t n = 0U; n < SCREENSHOT_ROWS_PER_POLL &&
                 g_shot.next_row < g_shot.height; ++n) {
                rc = png_row(&g_shot.file, &g_shot.png, &g_shot.surface,
                             g_shot.next_row, g_shot.width, g_shot.height);
                if (rc != 0) {
                    break;
                }
                ++g_shot.next_row;
            }
            if (g_shot.next_row == g_shot.height) {
                g_shot.state = SHOT_FINISH;
            }
        } else {
            rc = png_end(&g_shot.file, &g_shot.png);
            g_shot.state = SHOT_RENAME;
        }
    }
    g_shot.offset = f_tell(&g_shot.file);
    fr = shot_close();
    if (rc != 0 || fr != FR_OK) {
        shot_complete(rc != 0 ? rc : -(int)fr,
                      rc != 0 ? "PNG WRITE FAILED" : "PNG CLOSE FAILED");
        return;
    }
    fat_timestamp_override_end();
}

int screenshot_service_request(screenshot_service_kind_t kind,
                               const rtc_pcf8563_time_t *rtc,
                               screenshot_service_result_t *result)
{
    screenshot_surface_t surface = {0};
    uint32_t width, height;
    uint32_t native_width, native_height;
    size_t pixel_bytes;
    void *pixels;

    if (g_shot.state != SHOT_IDLE || g_shot.result_pending != 0U) {
        result_set(result, -1, NULL, "SCREENSHOT BUSY");
        return -1;
    }
    if (kind != SCREENSHOT_SERVICE_KIND_A2 && kind != SCREENSHOT_SERVICE_KIND_1080P) {
        result_set(result, -1, NULL, "INVALID SCREENSHOT KIND");
        return -1;
    }
    if (kind == SCREENSHOT_SERVICE_KIND_A2) {
        uint8_t slot = apple_fb_reader_claim();
        uint32_t mode = apple_fb_reader_display_mode();
        const uint32_t video_settings = apple_fb_video_settings_get();

        if (slot == APPLE_FB_NO_SLOT || slot >= COMP_APPLE_SLOT_COUNT) {
            slot = (uint8_t)g_compositor_last_apple_slot;
            mode = g_compositor_last_apple_mode;
        }
        if (slot == APPLE_FB_NO_SLOT || slot >= COMP_APPLE_SLOT_COUNT) {
            result_set(result, -1, NULL, "NO APPLE FRAME");
            return -1;
        }

        /* Keep the F1.2.2 Apple capture: raw renderer colors, its border
         * selection and fixed 2x/4x (SHR 2x/2x) size, independent of output
         * layout, compositor effects and the FPGA pixel mask. */
        surface.base = (const uint32_t *)(uintptr_t)comp_apple_slot_addr[slot];
        surface.scale_x = 2U;
        surface.scanlines_mode = g_scanlines_mode;
        if (mode == APPLE_FB_DISPLAY_MODE_SHR) {
            surface.stride_pixels = COMP_APPLE_SHR_ROW_PIXELS;
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
        pixel_bytes = sizeof(uint32_t);
    } else {
        uint8_t slot = 0xFFU;
        surface.base565 = compositor_latched_framebuffer(&slot);
        surface.stride_pixels = COMP_OUT_WIDTH;
        surface.scale_x = 1U;
        surface.scale_y = 1U;
        width = COMP_OUT_WIDTH;
        height = COMP_OUT_HEIGHT;
        if (surface.base565 == NULL || slot >= COMP_OUT_SLOT_COUNT ||
            width == 0U || height == 0U || width > COMP_OUT_MAX_WIDTH ||
            height > COMP_OUT_MAX_HEIGHT) {
            result_set(result, -1, NULL, "NO OUTPUT FRAME");
            return -1;
        }
        pixel_bytes = sizeof(uint16_t);
    }
    native_width = width / surface.scale_x;
    native_height = height / surface.scale_y;
    pixels = malloc((size_t)native_width * native_height * pixel_bytes);
    if (pixels == NULL) {
        result_set(result, -1, NULL, "SCREENSHOT OUT OF MEMORY");
        return -1;
    }
    /* CPU0 does not yield during this copy. The claimed Apple slot is
     * protected from CPU1; CPU0 owns output writes. SD polling retains only
     * this private snapshot, never either framebuffer ring. */
    for (uint32_t row = 0; row < native_height; ++row) {
        const size_t source_offset =
            ((size_t)row * surface.stride_pixels + surface.x_offset) * pixel_bytes;
        const void *source = surface.base565 != NULL ?
            (const void *)surface.base565 : (const void *)surface.base;
        memcpy((uint8_t *)pixels + (size_t)row * native_width * pixel_bytes,
               (const uint8_t *)source + source_offset,
               (size_t)native_width * pixel_bytes);
    }
    memset(&g_shot, 0, sizeof(g_shot));
    g_shot.pixels = pixels;
    g_shot.width = width;
    g_shot.height = height;
    surface.base = kind == SCREENSHOT_SERVICE_KIND_A2 ? pixels : NULL;
    surface.base565 = kind == SCREENSHOT_SERVICE_KIND_1080P ? pixels : NULL;
    surface.stride_pixels = native_width;
    surface.x_offset = 0U;
    g_shot.surface = surface;
    if (rtc != NULL) { g_shot.rtc = *rtc; }
    make_timestamp(g_shot.timestamp, sizeof(g_shot.timestamp), rtc);
    (void)snprintf(g_shot.suffix, sizeof(g_shot.suffix), "%s",
                   kind == SCREENSHOT_SERVICE_KIND_A2 ? "a2" :
                   compositor_output_mode() == DISPLAY_MODE_DEFAULT ? "1080p" :
                   display_mode_get(compositor_output_mode())->name);
    g_shot.state = SHOT_OPEN;
    result_set(result, 0, NULL, "SCREENSHOT QUEUED");
    return 0;
}

uint8_t screenshot_service_busy(void)
{
    return g_shot.state != SHOT_IDLE ? 1U : 0U;
}

uint8_t screenshot_service_take_result(screenshot_service_result_t *result)
{
    if (g_shot.result_pending == 0U) { return 0U; }
    if (result != NULL) { *result = g_shot.result; }
    g_shot.result_pending = 0U;
    return 1U;
}

int screenshot_service_cancel(void)
{
    if (g_shot.state == SHOT_IDLE) { return 0; }
    fat_timestamp_override_begin(&g_shot.rtc);
    shot_complete(-1, "SCREENSHOT CANCELLED");
    return g_shot.owns_part != 0U ? g_shot.result.rc : 0;
}

int screenshot_service_save(screenshot_service_kind_t kind,
                            const rtc_pcf8563_time_t *rtc,
                            screenshot_service_result_t *result)
{
    screenshot_service_result_t completed;
    const int rc = screenshot_service_request(kind, rtc, result);
    if (rc != 0) { return rc; }
    while (screenshot_service_busy() != 0U) { screenshot_service_poll(); }
    (void)screenshot_service_take_result(&completed);
    if (result != NULL) { *result = completed; }
    return completed.rc;
}

void screenshot_service_poll(void)
{
    XTime now = 0U;
    uint8_t any_active = 0U;

    shot_poll();
    for (uint32_t i = 0U; i < OVERLAY_COUNT; ++i) {
        if (g_overlays[i].active != 0U) {
            any_active = 1U;
        }
    }
    if (any_active == 0U) {
        return;
    }

    XTime_GetTime(&now);
    for (uint32_t i = 0U; i < OVERLAY_COUNT; ++i) {
        if (g_overlays[i].active != 0U &&
            (int64_t)(g_overlays[i].until - now) <= 0) {
            overlay_expire(&g_overlays[i]);
        }
    }
}

/* The frame restore path clears at most one overlay per call so the
 * caller can restore each rect independently; it is invoked once per
 * overlay until all are consumed (main.c loops on the nonzero return). */
uint8_t screenshot_service_restore_rect_for_frame(uint16_t *fb,
                                                  screenshot_service_rect_t *rect)
{
    const uint8_t slot_mask = output_slot_mask_for_fb(fb);

    if (slot_mask == 0U) {
        return 0U;
    }
    for (uint32_t i = 0U; i < OVERLAY_COUNT; ++i) {
        overlay_t *ov = &g_overlays[i];

        if ((ov->restore_slots & slot_mask) == 0U) {
            continue;
        }
        if (rect != NULL) {
            *rect = ov->restore_rect;
        }
        ov->restore_slots = (uint8_t)(ov->restore_slots & (uint8_t)~slot_mask);
        return 1U;
    }
    return 0U;
}

static void overlay_draw_one(uint16_t *fb, overlay_t *ov, uint8_t slot_mask)
{
    const int scale = FB16_WIDTH < 1200 ? 2 : 3;

    if (ov->active == 0U || ov->text[0] == '\0') {
        return;
    }

    compositor_pixel_mask_exclude(ov->rect.x, ov->rect.y, ov->rect.w, ov->rect.h);
    fb16_fill_rect(fb, ov->rect.x, ov->rect.y, ov->rect.w, ov->rect.h,
                   FB16_COLOR_BLACK);
    fb16_rect(fb, ov->rect.x, ov->rect.y, ov->rect.w, ov->rect.h,
              FB16_COLOR_GREEN);
    fb16_string_scaled(fb,
                       ov->rect.x + 18,
                       ov->rect.y + 12,
                       ov->text,
                       FB16_COLOR_WHITE,
                       FB16_COLOR_BLACK,
                       scale);
    if (slot_mask != 0U) {
        ov->drawn_slots = (uint8_t)(ov->drawn_slots | slot_mask);
    }
}

void screenshot_service_draw_overlay(uint16_t *fb)
{
    const uint8_t slot_mask = output_slot_mask_for_fb(fb);

    if (fb == NULL) {
        return;
    }
    for (uint32_t i = 0U; i < OVERLAY_COUNT; ++i) {
        overlay_draw_one(fb, &g_overlays[i], slot_mask);
    }
}
