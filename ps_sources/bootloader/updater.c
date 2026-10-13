#include "updater.h"

#include <limits.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "ff.h"
#include "xstatus.h"
#include "../lib/multiboot_platform.h"
#include "../lib/uart.h"
#include "updater_layout.h"
#include "golden_led.h"

#define SD_MOUNT_PATH         "0:/"

typedef struct {
    const char *source;
    const char *done;
} update_file_t;

/* Filenames identify candidates only. The verified contents select the slot. */
static const update_file_t g_update_files[] = {
    { "0:/FIRMWARE.BIN", "0:/FIRMWARE.OK" },
    { "0:/FIRMWARE-APPLETINI.BIN", "0:/FIRMWARE-APPLETINI.OK" },
    { "0:/FIRMWARE-MULTITINI.BIN", "0:/FIRMWARE-MULTITINI.OK" }
};

static FATFS g_fs;

static void log_puts(const char *s)
{
    uart_puts(UART0_BASE, s);
    uart_puts(UART1_BASE, s);
}

static void upd_logf(const char *fmt, ...)
{
    char line[220];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    log_puts(line);
}

static void set_err(updater_result_t *r, int code, const char *msg)
{
    if (r->error_code != 0) return;
    r->error_code = code;
    (void)snprintf(r->error_msg, sizeof(r->error_msg), "%s", msg);
}

static void file_err(updater_result_t *r, int code,
                     const char *path, const char *msg)
{
    char detail[sizeof(r->error_msg)];
    upd_logf("[UPD] %s: %s (code=%d)\r\n", path, msg, code);
    (void)snprintf(detail, sizeof(detail), "%s: %s", path, msg);
    set_err(r, code, detail);
}

/* Random access is required: preflight validates the complete source before
 * any flash erase, and installation rereads it while withholding XLNX. */
static int firmware_file_read(void *context, uint32_t offset,
                              void *dst, uint32_t len)
{
    FIL *file = (FIL *)context;
    UINT count = 0U;
    golden_led_toggle();
    if (f_lseek(file, (FSIZE_t)offset) != FR_OK ||
        f_read(file, dst, len, &count) != FR_OK || count != len) {
        return -1;
    }
    return 0;
}

int updater_select_firmware(uint32_t *boot_offset)
{
    const mb_flash_t *flash = mb_platform_flash();
    mb_selection_t selected;
    mb_metadata_t metadata;
    if (boot_offset == NULL || flash == NULL ||
        mb_select(flash, MB_PRODUCT_NONE, &selected) != MB_OK ||
        (selected.product == MB_PRODUCT_NONE && selected.legacy == 0U)) {
        return XST_FAILURE;
    }
    /* Consume a one-shot choice only after choosing a live verified image.
     * A failed metadata clear must never turn a valid fallback into a reset
     * loop. The persistent startup default is independent of this request. */
    if (mb_metadata_load(flash, &metadata) == MB_OK && metadata.request_product != MB_PRODUCT_NONE &&
        mb_clear_request(flash) != MB_OK) {
        log_puts("[GOLDEN] Could not clear one-shot request; verified selection retained\r\n");
    }
    *boot_offset = selected.boot_offset;
    upd_logf("[GOLDEN] Selected %s%s @0x%08lX\r\n",
             selected.legacy ? "legacy firmware" : mb_product_name(selected.product),
             selected.legacy ? " (product unknown)" : "",
             (unsigned long)selected.boot_offset);
    return XST_SUCCESS;
}

int updater_has_verified_firmware(void)
{
    const mb_flash_t *flash = mb_platform_flash();
    mb_selection_t selected;
    return flash != NULL && mb_select(flash, MB_PRODUCT_NONE, &selected) == MB_OK &&
           (selected.product != MB_PRODUCT_NONE || selected.legacy != 0U);
}

int updater_sync_boot_metadata(void)
{
    /* Preserve v4 single-slot metadata until a tagged image or owner choice
     * migrates it transactionally. Boot eligibility always checks live bytes. */
    return XST_SUCCESS;
}

static void install_update_file(updater_result_t *r, const update_file_t *entry)
{
    FIL file;
    FRESULT fr;
    FSIZE_t size;
    mb_reader_t source;
    mb_image_t installed;
    const mb_flash_t *flash;
    int rc;

    fr = f_open(&file, entry->source, FA_READ);
    if (fr == FR_NO_FILE || fr == FR_NO_PATH) return;
    r->found_file = 1;
    r->files_found++;
    if (fr != FR_OK) {
        file_err(r, 3, entry->source, "cannot open update file");
        r->files_failed++;
        return;
    }
    upd_logf("[UPD] Processing %s\r\n", entry->source);
    size = f_size(&file);
    if (size <= MB_MANIFEST_SIZE || size > (FSIZE_t)MB_MULTITINI_CAPACITY || size > (FSIZE_t)UINT32_MAX) {
        file_err(r, 4, entry->source, "invalid tagged firmware size");
        if (f_close(&file) != FR_OK) {
            file_err(r, 6, entry->source, "source close failed; update file retained");
        }
        r->files_failed++;
        return;
    }
    flash = mb_platform_flash();
    if (flash == NULL) {
        file_err(r, 5, entry->source, "qspi initialization failed");
        if (f_close(&file) != FR_OK) {
            file_err(r, 6, entry->source, "source close failed; update file retained");
        }
        r->files_failed++;
        return;
    }
    source.context = &file;
    source.read = firmware_file_read;
    source.size = (uint32_t)size;
    golden_led_on();
    /* The product tag selects A or B, regardless of the SD filename. v1 is
     * rejected here: an old generic footer cannot identify either product. */
    rc = mb_install(flash, &source, &installed);
    fr = f_close(&file);
    if (rc != MB_OK) {
        file_err(r, 20 + rc, entry->source, "multiboot validation or installation failed");
        if (fr != FR_OK) {
            file_err(r, 6, entry->source, "source close failed; update file retained");
        }
        r->files_failed++;
        return;
    }
    r->files_updated++;
    r->file_size = (uint32_t)size;
    r->file_crc32 = installed.image_crc32;
    r->flash_crc32 = installed.image_crc32;
    r->verified = 1;
    r->updated = 1;
    upd_logf("[UPD] Verified %s @0x%08lX size=%lu\r\n",
             mb_product_name(installed.product),
             (unsigned long)mb_product_offset(installed.product),
             (unsigned long)installed.image_size);
    golden_led_on();
    if (fr != FR_OK) {
        file_err(r, 6, entry->source, "verified image; source close failed; update file retained");
        r->files_failed++;
        return;
    }
    fr = f_unlink(entry->done);
    if (fr != FR_OK && fr != FR_NO_FILE && fr != FR_NO_PATH) {
        file_err(r, 7, entry->source, "verified image; cannot remove previous .OK file; update file retained");
        r->rename_failures++;
        r->files_failed++;
        return;
    }
    if (f_rename(entry->source, entry->done) != FR_OK) {
        /* Installed bytes and metadata are already verified. Retrying the
         * file on a later boot is safe; do not hide the successful install. */
        file_err(r, 8, entry->source, "verified image; cannot rename update file; update file retained");
        r->rename_failures++;
        r->files_failed++;
        return;
    }
    r->files_renamed++;
    upd_logf("[UPD] Renamed %s to %s\r\n", entry->source, entry->done);
}

int updater_run(updater_result_t *r)
{
    unsigned i;

    if (r == NULL) return XST_FAILURE;
    memset(r, 0, sizeof(*r));
    upd_logf("\r\n[UPD] boot_updater (%s)\r\n", updater_layout_name(updater_layout_active()));
    if (f_mount(&g_fs, SD_MOUNT_PATH, 1U) != FR_OK) {
        set_err(r, 2, "sd mount failed");
        return XST_FAILURE;
    }
    /* Finish every candidate before Golden selects an image to boot. A bad
     * or inaccessible file must not prevent a later valid image installing. */
    for (i = 0U; i < sizeof(g_update_files) / sizeof(g_update_files[0]); ++i) {
        install_update_file(r, &g_update_files[i]);
    }
    upd_logf("[UPD] Batch found=%lu installed=%lu failed=%lu renamed=%lu rename_failures=%lu\r\n",
             (unsigned long)r->files_found, (unsigned long)r->files_updated,
             (unsigned long)r->files_failed, (unsigned long)r->files_renamed,
             (unsigned long)r->rename_failures);
    return r->error_code == 0 ? XST_SUCCESS : XST_FAILURE;
}
