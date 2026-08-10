#include "printer_service.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diskio.h"
#include "ff.h"
#include "xiltimer.h"

#include "../lib/common.h"
#include "../lib/lodepng.h"

#include "card_control_regs.h"
#include "imagewriter.h"
#include "screenshot_service.h"
#include "usb_storage_service.h"

/* A print job closes after this much FIFO silence; a dirty partial page is
 * flushed to its own PNG first. */
#define PRINTER_IDLE_TIMEOUT_TICKS ((XTime)(5ULL * (uint64_t)COUNTS_PER_SECOND))

/* FIFO bytes consumed per poll. The Apple side can sustain roughly one
 * byte per ten microseconds, so this budget drains faster than the 6502
 * can fill while never monopolizing the main loop. */
#define PRINTER_DRAIN_BUDGET 256U

#define PRINTER_WRITE_CHUNK 16384U

static FATFS g_printer_fs;
static imagewriter_t g_iw;
static uint8_t *g_canvas;
static void (*g_checkpoint)(void);

static uint8_t g_job_active;
static XTime g_last_rx_tick;
static uint32_t g_next_seq;          /* 0 = not scanned yet */
static uint32_t g_pages_saved;
static char g_last_file[PRINTER_SERVICE_PATH_LEN];

void printer_service_set_checkpoint(void (*checkpoint)(void))
{
    g_checkpoint = checkpoint;
}

static void printer_checkpoint(void)
{
    if (g_checkpoint != NULL) {
        g_checkpoint();
    }
}

uint32_t printer_service_pages_saved(void)
{
    return g_pages_saved;
}

const char *printer_service_last_file(void)
{
    return g_last_file;
}

uint8_t printer_service_job_active(void)
{
    return g_job_active;
}

static FRESULT printer_mount_sd(void)
{
    FRESULT fr;

    fr = f_mount(&g_printer_fs, "0:/", 1U);
    if (fr != FR_OK) {
        (void)disk_initialize(0);
        (void)f_mount((FATFS *)0, "0:/", 0U);
        fr = f_mount(&g_printer_fs, "0:/", 1U);
    }
    return fr;
}

static FRESULT printer_ensure_dir(void)
{
    FRESULT fr = printer_mount_sd();

    if (fr != FR_OK) {
        return fr;
    }
    fr = f_mkdir(PRINTER_SERVICE_DIR);
    return (fr == FR_EXIST) ? FR_OK : fr;
}

/* Highest existing print_NNNN number in the printouts directory, so
 * numbering continues across reboots and user renames leave no clashes. */
static uint32_t printer_scan_max_seq(void)
{
    DIR dir;
    FILINFO info;
    uint32_t max_seq = 0U;

    if (f_opendir(&dir, PRINTER_SERVICE_DIR) != FR_OK) {
        return 0U;
    }
    for (;;) {
        unsigned value = 0U;

        if (f_readdir(&dir, &info) != FR_OK || info.fname[0] == '\0') {
            break;
        }
        if ((info.fattrib & AM_DIR) != 0U) {
            continue;
        }
        if (sscanf(info.fname, "print_%4u", &value) == 1 &&
            (uint32_t)value > max_seq) {
            max_seq = (uint32_t)value;
        }
    }
    (void)f_closedir(&dir);
    return max_seq;
}

static int printer_write_exact(FIL *file, const uint8_t *data, uint32_t len)
{
    while (len != 0U) {
        const UINT chunk = (len > PRINTER_WRITE_CHUNK)
                               ? PRINTER_WRITE_CHUNK : (UINT)len;
        UINT written = 0U;
        const FRESULT fr = f_write(file, data, chunk, &written);

        if (fr != FR_OK || written != chunk) {
            return (fr == FR_OK) ? -1 : -(int)fr;
        }
        data += chunk;
        len -= (uint32_t)chunk;
        printer_checkpoint();
    }
    return 0;
}

static void printer_render_rgb(uint8_t *rgb,
                               const uint8_t *ink,
                               size_t pixels)
{
    size_t i;

    for (i = 0U; i < pixels; ++i) {
        uint8_t r = 255U;
        uint8_t g = 255U;
        uint8_t b = 255U;

        switch (ink[i] & 0x0FU) {
        case IW_INK_NONE:                                      break;
        case IW_INK_YELLOW:   r = 255U; g = 220U; b = 0U;      break;
        case IW_INK_MAGENTA:  r = 220U; g = 0U;   b = 70U;     break;
        case IW_INK_CYAN:     r = 0U;   g = 105U; b = 220U;    break;
        case IW_INK_YELLOW | IW_INK_MAGENTA:
                               r = 255U; g = 96U;  b = 0U;      break;
        case IW_INK_YELLOW | IW_INK_CYAN:
                               r = 0U;   g = 160U; b = 60U;     break;
        case IW_INK_MAGENTA | IW_INK_CYAN:
                               r = 120U; g = 40U;  b = 160U;    break;
        default:               r = 0U;   g = 0U;   b = 0U;      break;
        }
        if ((ink[i] & IW_INK_BLACK) != 0U) {
            r = 0U;
            g = 0U;
            b = 0U;
        }
        rgb[i * 3U + 0U] = r;
        rgb[i * 3U + 1U] = g;
        rgb[i * 3U + 2U] = b;
    }
}

static void printer_page_done(void *ctx,
                              const uint8_t *canvas,
                              uint32_t width,
                              uint32_t height)
{
    unsigned char *png = NULL;
    uint8_t *rgb = NULL;
    size_t png_size = 0U;
    const size_t pixels = (size_t)width * (size_t)height;
    unsigned error;
    char path[PRINTER_SERVICE_PATH_LEN];
    FIL file;
    FRESULT fr;
    uint8_t usb_storage_was_connected;
    int rc = -1;

    (void)ctx;

    if (pixels == 0U || pixels > SIZE_MAX / 3U) {
        return;
    }
    rgb = (uint8_t *)malloc(pixels * 3U);
    if (rgb == NULL) {
        return;
    }
    printer_render_rgb(rgb, canvas, pixels);
    printer_checkpoint();
    error = lodepng_encode_memory(&png, &png_size, rgb, width, height,
                                  LCT_RGB, 8U);
    free(rgb);
    printer_checkpoint();
    if (error != 0U || png == NULL) {
        free(png);
        return;
    }

    usb_storage_was_connected = usb_storage_service_disconnect();
    fr = printer_ensure_dir();
    if (fr == FR_OK) {
        if (g_next_seq == 0U) {
            g_next_seq = printer_scan_max_seq() + 1U;
        }
        (void)snprintf(path, sizeof(path), "%s/print_%04u.png",
                       PRINTER_SERVICE_DIR, (unsigned)g_next_seq);
        fr = f_open(&file, path, FA_CREATE_ALWAYS | FA_WRITE);
        if (fr != FR_OK) {
            fr = printer_mount_sd();
            if (fr == FR_OK) {
                fr = f_open(&file, path, FA_CREATE_ALWAYS | FA_WRITE);
            }
        }
        if (fr == FR_OK) {
            rc = printer_write_exact(&file, png, (uint32_t)png_size);
            if (f_close(&file) != FR_OK && rc == 0) {
                rc = -1;
            }
        }
    }
    screenshot_service_note_local_sd_write_complete();
    if (usb_storage_was_connected != 0U) {
        usb_storage_service_connect();
    }
    free(png);

    if (rc == 0) {
        g_next_seq++;
        g_pages_saved++;
        (void)snprintf(g_last_file, sizeof(g_last_file), "%s", path);
        screenshot_service_show_confirmation("PAGE PRINTED");
    }
}

void printer_service_init(void)
{
    g_canvas = NULL;
    g_checkpoint = NULL;
    g_job_active = 0U;
    g_next_seq = 0U;
    g_pages_saved = 0U;
    g_last_file[0] = '\0';
}

static int printer_ensure_interpreter(void)
{
    if (g_canvas != NULL) {
        return 0;
    }
    g_canvas = (uint8_t *)malloc((size_t)IW_PAGE_WIDTH * IW_PAGE_HEIGHT);
    if (g_canvas == NULL) {
        return -1;
    }
    if (imagewriter_init(&g_iw, g_canvas, printer_page_done, NULL) != 0) {
        free(g_canvas);
        g_canvas = NULL;
        return -1;
    }
    return 0;
}

void printer_service_poll(void)
{
    uint32_t status = REG_READ(CARD_CTRL_SSC_STATUS_REG);
    uint32_t count = status & CARD_CTRL_SSC_STATUS_COUNT_MASK;
    XTime now;

    if ((status & CARD_CTRL_SSC_STATUS_ENABLED) == 0U && count == 0U &&
        g_job_active == 0U) {
        return;
    }

    if ((status & CARD_CTRL_SSC_STATUS_OVERFLOW) != 0U) {
        /* Bytes were dropped while we were away; the page keeps whatever
         * arrived. Clear the latch so the next drop is visible too. */
        REG_WRITE(CARD_CTRL_SSC_CTRL_REG, CARD_CTRL_SSC_CTRL_OVF_CLEAR);
    }

    XTime_GetTime(&now);

    if (count != 0U) {
        uint8_t buf[PRINTER_DRAIN_BUDGET];
        uint32_t n = 0U;
        uint32_t budget = (count > PRINTER_DRAIN_BUDGET)
                              ? PRINTER_DRAIN_BUDGET : count;

        if (printer_ensure_interpreter() != 0) {
            /* No canvas memory: drop the data rather than wedge the FIFO. */
            while (budget-- > 0U) {
                REG_WRITE(CARD_CTRL_SSC_CTRL_REG, CARD_CTRL_SSC_CTRL_POP);
            }
            return;
        }

        while (n < budget) {
            const uint32_t head = REG_READ(CARD_CTRL_SSC_HEAD_REG);

            if ((head & CARD_CTRL_SSC_HEAD_VALID) == 0U) {
                break;
            }
            buf[n++] = (uint8_t)head;
            REG_WRITE(CARD_CTRL_SSC_CTRL_REG, CARD_CTRL_SSC_CTRL_POP);
        }

        if (n != 0U) {
            if (g_job_active == 0U) {
                g_job_active = 1U;
                imagewriter_reset(&g_iw);
            }
            imagewriter_feed(&g_iw, buf, n);
            g_last_rx_tick = now;
        }
        return;
    }

    if (g_job_active != 0U &&
        (int64_t)(now - g_last_rx_tick) >
            (int64_t)PRINTER_IDLE_TIMEOUT_TICKS) {
        imagewriter_flush_page(&g_iw);
        g_job_active = 0U;
    }
}
