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
#define UPDATE_FILE_PATH      "0:/FIRMWARE.BIN"
#define UPDATE_DONE_FILE_PATH "0:/FIRMWARE.OK"

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
    r->error_code = code;
    r->verified = 0;
    r->updated = 0;
    (void)snprintf(r->error_msg, sizeof(r->error_msg), "%s", msg);
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

int updater_run(updater_result_t *r)
{
    FIL file;
    FRESULT fr;
    FSIZE_t size;
    mb_reader_t source;
    mb_image_t installed;
    const mb_flash_t *flash;
    int rc;

    if (r == NULL) return XST_FAILURE;
    memset(r, 0, sizeof(*r));
    upd_logf("\r\n[UPD] boot_updater (%s)\r\n", updater_layout_name(updater_layout_active()));
    if (f_mount(&g_fs, SD_MOUNT_PATH, 1U) != FR_OK) {
        set_err(r, 2, "sd mount failed");
        return XST_FAILURE;
    }
    fr = f_open(&file, UPDATE_FILE_PATH, FA_READ);
    if (fr == FR_NO_FILE || fr == FR_NO_PATH) return XST_SUCCESS;
    if (fr != FR_OK) {
        set_err(r, 3, "cannot open update file");
        return XST_FAILURE;
    }
    r->found_file = 1;
    size = f_size(&file);
    if (size <= MB_MANIFEST_SIZE || size > (FSIZE_t)MB_MULTITINI_CAPACITY || size > (FSIZE_t)UINT32_MAX) {
        (void)f_close(&file);
        set_err(r, 4, "invalid tagged firmware size");
        return XST_FAILURE;
    }
    r->file_size = (uint32_t)size;
    flash = mb_platform_flash();
    if (flash == NULL) {
        (void)f_close(&file);
        set_err(r, 5, "qspi initialization failed");
        return XST_FAILURE;
    }
    source.context = &file;
    source.read = firmware_file_read;
    source.size = r->file_size;
    golden_led_on();
    /* The product tag selects A or B, regardless of the SD filename. v1 is
     * rejected here: an old generic footer cannot identify either product. */
    rc = mb_install(flash, &source, &installed);
    fr = f_close(&file);
    if (rc != MB_OK) {
        upd_logf("[UPD] Tagged firmware install rejected code=%d\r\n", rc);
        set_err(r, 20 + rc, "multiboot validation or installation failed");
        return XST_FAILURE;
    }
    r->file_crc32 = installed.image_crc32;
    r->flash_crc32 = installed.image_crc32;
    r->verified = 1;
    r->updated = 1;
    upd_logf("[UPD] Verified %s @0x%08lX size=%lu\r\n",
             mb_product_name(installed.product),
             (unsigned long)mb_product_offset(installed.product),
             (unsigned long)installed.image_size);
    if (fr != FR_OK) log_puts("[UPD] Verified image; source close failed\r\n");
    golden_led_on();
    (void)f_unlink(UPDATE_DONE_FILE_PATH);
    if (f_rename(UPDATE_FILE_PATH, UPDATE_DONE_FILE_PATH) != FR_OK) {
        /* Installed bytes and metadata are already verified. Retrying the
         * file on a later boot is safe; do not hide the successful install. */
        log_puts("[UPD] Verified image; could not rename FIRMWARE.BIN\r\n");
    }
    return XST_SUCCESS;
}