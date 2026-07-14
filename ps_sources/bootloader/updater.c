#include "updater.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "diskio.h"
#include "ff.h"
#include "xstatus.h"

#include "../image_versions.h"
#include "../fw_update_metadata.h"
#include "../lib/crc32.h"
#include "../lib/qspi_nor.h"
#include "../lib/uart.h"

#include "updater_layout.h"
#include "golden_led.h"


#define SD_MOUNT_PATH            "0:/"
#define UPDATE_FILE_PATH         "0:/FIRMWARE.BIN"
#define UPDATE_DONE_FILE_PATH    "0:/FIRMWARE.OK"
#define IO_CHUNK_BYTES           4096U

enum {
    UPD_E_OK = 0,
    UPD_E_SD_MOUNT = 2,
    UPD_E_SD_OPEN = 3,
    UPD_E_FILE_SIZE = 4,
    UPD_E_QSPI_INIT = 5,
    UPD_E_FLASH_ERASE = 6,
    UPD_E_FLASH_PROGRAM = 7,
    UPD_E_FLASH_READBACK = 8,
    UPD_E_VERIFY_MISMATCH = 9,
    UPD_E_SD_READ = 10,
    UPD_E_SD_RENAME = 11
};

static FATFS g_fs;
static uint8_t g_file_buf[IO_CHUNK_BYTES] __attribute__((aligned(64)));
static uint8_t g_flash_buf[IO_CHUNK_BYTES] __attribute__((aligned(64)));
static uint32_t g_update_seq = 0U;

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
    if (!r) {
        return;
    }
    r->error_code = code;
    r->verified = 0;
    r->updated = 0;
    r->error_msg[0] = '\0';
    if (msg) {
        (void)snprintf(r->error_msg, sizeof(r->error_msg), "%s", msg);
    }
}

static int sd_mount(void)
{
    FRESULT fr = f_mount(&g_fs, SD_MOUNT_PATH, 1U);
    return (fr == FR_OK) ? XST_SUCCESS : XST_FAILURE;
}

static void str_copy_fixed(char *dst, uint32_t dst_len, const char *src)
{
    uint32_t i;
    if (dst == NULL || dst_len == 0U) {
        return;
    }
    for (i = 0U; i < (dst_len - 1U); ++i) {
        char c = (src != NULL) ? src[i] : '\0';
        dst[i] = c;
        if (c == '\0') {
            return;
        }
    }
    dst[dst_len - 1U] = '\0';
}

static uint8_t metadata_crc_valid(const fw_update_metadata_t *m)
{
    uint32_t crc;
    if (m == NULL) {
        return 0U;
    }
    if (m->magic != FW_UPDATE_METADATA_MAGIC ||
        m->version != FW_UPDATE_METADATA_VERSION ||
        m->length_bytes != (uint32_t)sizeof(*m)) {
        return 0U;
    }
    crc = crc32_init();
    crc = crc32_update(crc, m, sizeof(*m) - sizeof(m->crc32));
    crc = crc32_finish(crc);
    return (crc == m->crc32) ? 1U : 0U;
}

static void metadata_build_record(fw_update_metadata_t *m,
                                  const flash_layout_t *layout,
                                  const updater_result_t *r,
                                  const fw_update_metadata_t *prev_valid)
{
    uint32_t crc;

    memset(m, 0, sizeof(*m));
    m->magic = FW_UPDATE_METADATA_MAGIC;
    m->version = FW_UPDATE_METADATA_VERSION;
    m->length_bytes = (uint32_t)sizeof(*m);
    m->seq = (prev_valid != NULL) ? (prev_valid->seq + 1U) : (++g_update_seq);
    if (m->seq == 0U) {
        m->seq = 1U;
    }
    m->golden_offset = layout->golden.offset;
    m->golden_size = layout->golden.size;
    m->firmware_offset = layout->firmware.offset;
    m->firmware_size = layout->firmware.size;
    m->flags = 0U;
    str_copy_fixed(m->golden_version, FW_UPDATE_METADATA_STRLEN, APPLETINI_BOOT_IMAGE_VERSION_FULL);
    str_copy_fixed(m->layout_label, FW_UPDATE_METADATA_STRLEN, APPLETINI_FLASH_LAYOUT_LABEL);

    if (prev_valid != NULL) {
        m->flags = prev_valid->flags;
    }
    if (r != NULL && r->updated != 0 && r->verified != 0) {
        m->flags |= FW_UPDATE_METADATA_FLAG_FIRMWARE_VERIFIED;
    }

    crc = crc32_init();
    crc = crc32_update(crc, m, sizeof(*m) - sizeof(m->crc32));
    m->crc32 = crc32_finish(crc);
}

static int metadata_write_record(qspi_nor_t *nor, const flash_layout_t *layout, const fw_update_metadata_t *m)
{
    fw_update_metadata_t verify;

    log_puts("[UPD] Metadata write: enter\r\n");
    if (nor == NULL || layout == NULL || m == NULL) {
        log_puts("[UPD] Metadata write: bad args\r\n");
        return XST_FAILURE;
    }
    upd_logf("[UPD] Metadata region off=0x%08lX size=0x%08lX recsz=%lu\r\n",
             (unsigned long)layout->metadata.offset,
             (unsigned long)layout->metadata.size,
             (unsigned long)sizeof(*m));
    if (layout->metadata.size < sizeof(*m)) {
        log_puts("[UPD] Metadata write: region too small\r\n");
        return XST_FAILURE;
    }

    log_puts("[UPD] Writing metadata...\r\n");
    if (qspi_nor_erase_region(nor, layout->metadata.offset, layout->metadata.size) != XST_SUCCESS) {
        log_puts("[UPD] Metadata write: erase failed\r\n");
        return XST_FAILURE;
    }
    if (qspi_nor_program(nor, layout->metadata.offset, m, (uint32_t)sizeof(*m)) != XST_SUCCESS) {
        log_puts("[UPD] Metadata write: program failed\r\n");
        return XST_FAILURE;
    }
    if (qspi_nor_read(nor, layout->metadata.offset, &verify, (uint32_t)sizeof(verify)) != XST_SUCCESS ||
        memcmp(&verify, m, sizeof(verify)) != 0) {
        log_puts("[UPD] Metadata write: verify failed\r\n");
        return XST_FAILURE;
    }
    log_puts("[UPD] Metadata write: done\r\n");
    return XST_SUCCESS;
}

static int write_update_metadata(qspi_nor_t *nor, const flash_layout_t *layout, const updater_result_t *r)
{
    fw_update_metadata_t current;
    fw_update_metadata_t next;
    fw_update_metadata_t *prev = NULL;

    if (layout->metadata.size != 0U &&
        qspi_nor_read(nor, layout->metadata.offset, &current, (uint32_t)sizeof(current)) == XST_SUCCESS &&
        metadata_crc_valid(&current)) {
        prev = &current;
    }
    metadata_build_record(&next, layout, r, prev);
    return metadata_write_record(nor, layout, &next);
}

static int file_size_of(const char *path, uint32_t *size_out)
{
    FILINFO fi;
    FRESULT fr = f_stat(path, &fi);
    if (fr != FR_OK) {
        return XST_FAILURE;
    }
    *size_out = (uint32_t)fi.fsize;
    return XST_SUCCESS;
}

static int region_fits_capacity(uint32_t offset, uint32_t size, uint32_t capacity)
{
    if (capacity == 0U) {
        return 1;
    }
    if (offset > capacity) {
        return 0;
    }
    return size <= (capacity - offset);
}

static int program_firmware_from_sd(const flash_layout_t *layout, updater_result_t *r)
{
    FIL f;
    FRESULT fr;
    qspi_nor_t nor;
    uint32_t off = 0U;
    uint32_t crc_file = crc32_init();
    uint32_t crc_flash = crc32_init();
    unsigned int br = 0U;
    const flash_region_t fw = layout->firmware;

    /* Firmware update starting: LED solid on (covers open, QSPI init,
     * erase). The program/verify loops switch it to per-chunk toggling. */
    golden_led_on();

    fr = f_open(&f, UPDATE_FILE_PATH, FA_READ);
    if (fr != FR_OK) {
        set_err(r, UPD_E_SD_OPEN, "open failed");
        return XST_FAILURE;
    }

    if (qspi_nor_init(&nor, layout->qspi_addr_bytes, layout->erase_size_bytes) != XST_SUCCESS) {
        (void)f_close(&f);
        set_err(r, UPD_E_QSPI_INIT, "qspi init failed");
        return XST_FAILURE;
    }
    upd_logf("[UPD] Flash JEDEC %02X %02X %02X capacity=%lu addr=%u-byte\r\n",
             (unsigned int)nor.jedec_id[0],
             (unsigned int)nor.jedec_id[1],
             (unsigned int)nor.jedec_id[2],
             (unsigned long)qspi_nor_capacity_bytes(&nor),
             (unsigned int)nor.addr_bytes);
    if (!region_fits_capacity(fw.offset, r->file_size, qspi_nor_capacity_bytes(&nor))) {
        (void)f_close(&f);
        set_err(r, UPD_E_FILE_SIZE, "firmware image does not fit detected flash");
        return XST_FAILURE;
    }

    upd_logf("[UPD] Erasing firmware region @0x%08lX size=0x%08lX...\r\n",
         (unsigned long)fw.offset, (unsigned long)r->file_size);
    if (qspi_nor_erase_region(&nor, fw.offset, r->file_size) != XST_SUCCESS) {
        (void)f_close(&f);
        set_err(r, UPD_E_FLASH_ERASE, "erase failed");
        return XST_FAILURE;
    }

    log_puts("[UPD] Programming...\r\n");
    while (off < r->file_size) {
        uint32_t n = r->file_size - off;
        if (n > IO_CHUNK_BYTES) {
            n = IO_CHUNK_BYTES;
        }

        golden_led_toggle();   /* programming: toggle per chunk */

        fr = f_read(&f, g_file_buf, n, &br);
        if (fr != FR_OK || br != n) {
            (void)f_close(&f);
            set_err(r, UPD_E_SD_READ, "sd read failed");
            return XST_FAILURE;
        }

        crc_file = crc32_update(crc_file, g_file_buf, n);
        if (qspi_nor_program(&nor, fw.offset + off, g_file_buf, n) != XST_SUCCESS) {
            (void)f_close(&f);
            set_err(r, UPD_E_FLASH_PROGRAM, "program failed");
            return XST_FAILURE;
        }
        off += n;
    }
    (void)f_close(&f);
    r->file_crc32 = crc32_finish(crc_file);

    /* Programming done: LED solid on while we reopen for verify. */
    golden_led_on();

    fr = f_open(&f, UPDATE_FILE_PATH, FA_READ);
    if (fr != FR_OK) {
        set_err(r, UPD_E_SD_OPEN, "reopen failed");
        return XST_FAILURE;
    }

    log_puts("[UPD] Verifying...\r\n");
    off = 0U;
    while (off < r->file_size) {
        uint32_t n = r->file_size - off;
        if (n > IO_CHUNK_BYTES) {
            n = IO_CHUNK_BYTES;
        }

        golden_led_toggle();   /* verifying: toggle per chunk */

        fr = f_read(&f, g_file_buf, n, &br);
        if (fr != FR_OK || br != n) {
            (void)f_close(&f);
            set_err(r, UPD_E_SD_READ, "verify sd read failed");
            return XST_FAILURE;
        }

        if (qspi_nor_read(&nor, fw.offset + off, g_flash_buf, n) != XST_SUCCESS) {
            (void)f_close(&f);
            set_err(r, UPD_E_FLASH_READBACK, "flash readback failed");
            return XST_FAILURE;
        }

        crc_flash = crc32_update(crc_flash, g_flash_buf, n);
        if (memcmp(g_file_buf, g_flash_buf, n) != 0) {
            uint32_t i;
            for (i = 0U; i < n; ++i) {
                if (g_file_buf[i] != g_flash_buf[i]) {
                    upd_logf("[UPD] Verify mismatch @0x%08lX exp=0x%02X got=0x%02X\r\n",
                         (unsigned long)(fw.offset + off + i),
                         (unsigned int)g_file_buf[i],
                         (unsigned int)g_flash_buf[i]);
                    break;
                }
            }
            (void)f_close(&f);
            set_err(r, UPD_E_VERIFY_MISMATCH, "verify mismatch");
            return XST_FAILURE;
        }
        off += n;
    }
    (void)f_close(&f);
    r->flash_crc32 = crc32_finish(crc_flash);
    r->verified = 1;
    r->updated = 1;

    /* Verify done: LED solid on through metadata write + rename. */
    golden_led_on();

    if (layout->metadata.size != 0U &&
        region_fits_capacity(layout->metadata.offset,
                             layout->metadata.size,
                             qspi_nor_capacity_bytes(&nor))) {
        if (write_update_metadata(&nor, layout, r) != XST_SUCCESS) {
            set_err(r, UPD_E_FLASH_PROGRAM, "metadata write failed");
            return XST_FAILURE;
        }
    } else if (layout->metadata.size != 0U) {
        log_puts("[UPD] Metadata write skipped (metadata outside detected flash)\r\n");
    } else {
        log_puts("[UPD] Metadata write skipped (metadata region disabled)\r\n");
    }

    (void)f_unlink(UPDATE_DONE_FILE_PATH);
    fr = f_rename(UPDATE_FILE_PATH, UPDATE_DONE_FILE_PATH);
    if (fr != FR_OK) {
        set_err(r, UPD_E_SD_RENAME, "rename failed");
        return XST_FAILURE;
    }
    return XST_SUCCESS;
}

int updater_sync_boot_metadata(void)
{
    const flash_layout_t *layout = updater_layout_active();
    qspi_nor_t nor;
    fw_update_metadata_t cur;
    fw_update_metadata_t next;
    fw_update_metadata_t *prev = NULL;
    int need_write = 0;

    if (layout == NULL || layout->metadata.size == 0U) {
        return 0;
    }
    if (qspi_nor_init(&nor, layout->qspi_addr_bytes, layout->erase_size_bytes) != XST_SUCCESS) {
        return XST_FAILURE;
    }
    if (!region_fits_capacity(layout->metadata.offset,
                              layout->metadata.size,
                              qspi_nor_capacity_bytes(&nor))) {
        return 0;
    }
    if (qspi_nor_read(&nor, layout->metadata.offset, &cur, (uint32_t)sizeof(cur)) == XST_SUCCESS &&
        metadata_crc_valid(&cur)) {
        prev = &cur;
    }

    if (prev == NULL) {
        return 0;
    }
    if (strncmp(prev->golden_version, APPLETINI_BOOT_IMAGE_VERSION_FULL, FW_UPDATE_METADATA_STRLEN) != 0 ||
        prev->golden_offset != layout->golden.offset ||
        prev->golden_size != layout->golden.size ||
        prev->firmware_offset != layout->firmware.offset ||
        prev->firmware_size != layout->firmware.size ||
        strncmp(prev->layout_label, APPLETINI_FLASH_LAYOUT_LABEL, FW_UPDATE_METADATA_STRLEN) != 0) {
        need_write = 1;
    }

    if (!need_write) {
        return 0;
    }

    upd_logf("[UPD] Metadata sync: updating boot/layout metadata\r\n");
    metadata_build_record(&next, layout, NULL, prev);
    return metadata_write_record(&nor, layout, &next);
}

int updater_has_verified_firmware(void)
{
    const flash_layout_t *layout = updater_layout_active();
    qspi_nor_t nor;
    fw_update_metadata_t cur;

    if (layout == NULL || layout->metadata.size == 0U) {
        return 0;
    }
    if (qspi_nor_init(&nor, layout->qspi_addr_bytes, layout->erase_size_bytes) != XST_SUCCESS) {
        return 0;
    }
    if (!region_fits_capacity(layout->metadata.offset,
                              layout->metadata.size,
                              qspi_nor_capacity_bytes(&nor))) {
        return 0;
    }
    if (qspi_nor_read(&nor, layout->metadata.offset, &cur, (uint32_t)sizeof(cur)) != XST_SUCCESS ||
        !metadata_crc_valid(&cur)) {
        return 0;
    }
    if ((cur.flags & FW_UPDATE_METADATA_FLAG_FIRMWARE_VERIFIED) == 0U) {
        return 0;
    }
    if (cur.golden_offset != layout->golden.offset ||
        cur.golden_size != layout->golden.size ||
        cur.firmware_offset != layout->firmware.offset ||
        cur.firmware_size != layout->firmware.size ||
        strncmp(cur.layout_label, APPLETINI_FLASH_LAYOUT_LABEL, FW_UPDATE_METADATA_STRLEN) != 0) {
        return 0;
    }

    return 1;
}

int updater_run(updater_result_t *r)
{
    const flash_layout_t *layout = updater_layout_active();
    uint32_t file_size = 0U;

    if (!r) {
        return XST_FAILURE;
    }
    memset(r, 0, sizeof(*r));

    upd_logf("\r\n[UPD] boot_updater (%s)\r\n", updater_layout_name(layout));

    if (sd_mount() != XST_SUCCESS) {
        set_err(r, UPD_E_SD_MOUNT, "sd mount failed");
        return XST_FAILURE;
    }
    log_puts("[UPD] SD mounted\r\n");

    if (file_size_of(UPDATE_FILE_PATH, &file_size) != XST_SUCCESS) {
        r->found_file = 0;
        r->error_code = 0;
        r->error_msg[0] = '\0';
        return XST_SUCCESS;
    }

    r->found_file = 1;
    r->file_size = file_size;

    if (file_size == 0U || file_size > layout->firmware.size) {
        set_err(r, UPD_E_FILE_SIZE, "invalid file size for firmware region");
        return XST_FAILURE;
    }

    upd_logf("[UPD] Found FIRMWARE.BIN size=%lu bytes\r\n", (unsigned long)file_size);
    return program_firmware_from_sd(layout, r);
}
