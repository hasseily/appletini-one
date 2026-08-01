#include "xil_mmu.h"
#include "disk2_service.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ff.h"
#include "xil_cache.h"

#include "../lib/common.h"
#include "../lib/crc32.h"
#include <stdio.h>
#include "../lib/uart.h"

#define DISK2_BASE 0x40060000U
#define DISK2_REG(idx) (DISK2_BASE + ((idx) * 4U))

#define DISK2_REG_STAGING_BASE 0x02U
#define DISK2_REG_TRACK_INFO 0x06U
#define DISK2_REG_TRACK_LENGTH 0x07U
#define DISK2_REG_TRACK_INDEX 0x08U
#define DISK2_REG_TRACK_DATA 0x09U
#define DISK2_REG_STREAM_POS 0x0AU
#define DISK2_REG_STREAM_READS 0x0BU
#define DISK2_REG_WRITE_INFO 0x0CU
#define DISK2_REG_WRITE_COUNT 0x0DU
#define DISK2_REG_TRACK_BIT_COUNT 0x0EU
#define DISK2_REG_TRACK_BIT_OFFSET 0x0FU
#define DISK2_REG_TRACK_BIT_TIMING 0x15U
#define DISK2_REG_TRACK_SEAM 0x16U
#define DISK2_REG_D1_INFO    0x10U
#define DISK2_REG_D2_INFO    0x18U
#define DISK2_REG_WOZ_ALIAS_RANGE 0x20U

#define DISK2_STANDARD_TRACKS 35U
#define DISK2_STANDARD_MAX_TRACKS 40U
#define DISK2_SECTOR_BYTES 256U
#define DISK2_DSK_SECTORS_PER_TRACK 16U
#define DISK2_NIB_TRACK_BYTES 6656U
#define DISK2_STANDARD_MAX_IMAGE_BYTES (DISK2_STANDARD_MAX_TRACKS * DISK2_NIB_TRACK_BYTES)
#define DISK2_TRACK_STREAM_BYTES 8192U
#define DISK2_IO_CHUNK_BYTES 512U
#define DISK2_WOZ_HEADER_SIZE 12U
#define DISK2_WOZ_CHUNK_HEADER_SIZE 8U
#define DISK2_WOZ_INFO_CHUNK_SIZE 60U
#define DISK2_WOZ_INFO_DISK_TYPE_OFFSET 1U
#define DISK2_WOZ_INFO_WRITE_PROTECT_OFFSET 2U
#define DISK2_WOZ_INFO_BOOT_SECTOR_FORMAT_OFFSET 38U
#define DISK2_WOZ_INFO_OPTIMAL_BIT_TIMING_OFFSET 39U
#define DISK2_WOZ_OPTIMAL_BIT_TIMING_5_25 32U
#define DISK2_WOZ_DISK_TYPE_5_25 1U
#define DISK2_WOZ_BOOT_SECTOR_13 2U
#define DISK2_WOZ1_TRACK_BYTES 6656U
#define DISK2_WOZ1_TRK_OFFSET 6646U
#define DISK2_WOZ_EMPTY_TRACK_BYTES 6400U
#define DISK2_WOZ_BLOCK_BYTES 512U
#define DISK2_WOZ_MAX_TRACKS 40U
#define DISK2_WOZ_TMAP_EMPTY 0xFFU
#define DISK2_WOZ_TRKV2_BYTES 8U
#define DISK2_WOZ_TRKV2_TABLE_BYTES (DISK2_WOZ_TMAP_SIZE * DISK2_WOZ_TRKV2_BYTES)
#define DISK2_APPLEWIN_RAND_MAX 32767U
#define DISK2_APPLEWIN_RAND_3_10 9830U
#define DISK2_NO_LOADED_TRACK 0xFFU
#define DISK2_NO_PHYSICAL_TRACK 0xFFU
#define DISK2_LOAD_STALE_REQUEST (-100)
#define DISK2_WOZ_SCAN_SEAM_BITS 256U

#define DISK2_2MG_HEADER_SIZE 64U
#define DISK2_2MG_MAGIC 0x474D4932U
#define DISK2_2MG_FORMAT_DOS33 0U
#define DISK2_2MG_FORMAT_PRODOS 1U
#define DISK2_2MG_FORMAT_NIB 2U
#define DISK2_2MG_FLAG_VOLUME_VALID 0x00000100U
#define DISK2_2MG_FLAG_LOCKED 0x80000000U
#define DISK2_DEFAULT_VOLUME_NUMBER 0xFEU

#define DISK2_TRACK_INFO_LOADED_BIT 0x00000001U
#define DISK2_TRACK_INFO_MATCH_BIT  0x00000002U
#define DISK2_TRACK_INFO_RAW_BITS_BIT 0x00000004U
#define DISK2_TRACK_INFO_NO_TRACK_BIT 0x00000008U
#define DISK2_TRACK_INFO_CUR_DRIVE_SHIFT 16U
#define DISK2_TRACK_INFO_CUR_QTRACK_SHIFT 8U
#define DISK2_TRACK_INFO_LOAD_DRIVE_SHIFT 20U
#define DISK2_TRACK_INFO_LOAD_QTRACK_SHIFT 8U
#define DISK2_TRACK_INFO_LOADED_QTRACK_SHIFT 24U
#define DISK2_WRITE_INFO_DIRTY_BIT 0x00000001U
#define DISK2_WRITE_INFO_BUSY_BIT  0x00000002U
#define DISK2_WRITE_INFO_QTRACK_SHIFT 8U
#define DISK2_WRITE_INFO_DRIVE_SHIFT 16U

#define DISK2_WOZ_STRUCTURE_REJECT (-22)

static uint32_t g_uart_base = 0U;
static char g_disk2_paths[DISK2_DRIVE_COUNT][DISK2_IMAGE_PATH_MAX];
static disk2_image_info_t g_disk2_info[DISK2_DRIVE_COUNT];
static uint8_t g_track_buf[DISK2_TRACK_STREAM_BYTES] __attribute__((aligned(64)));
static uint8_t g_scan_buf[DISK2_TRACK_STREAM_BYTES];
static uint8_t g_woz_decode_buf[DISK2_TRACK_STREAM_BYTES];
static uint8_t g_sector_track_buf[DISK2_DSK_SECTORS_PER_TRACK * DISK2_SECTOR_BYTES];
static uint8_t g_file_io_buf[DISK2_IO_CHUNK_BYTES] __attribute__((aligned(64)));
static uint8_t g_standard_image_buf[DISK2_DRIVE_COUNT][DISK2_STANDARD_MAX_IMAGE_BYTES] __attribute__((aligned(64)));
static uint32_t g_standard_image_size[DISK2_DRIVE_COUNT];
static uint8_t g_standard_image_cached[DISK2_DRIVE_COUNT];
static uint8_t g_loaded_drive = DISK2_NO_LOADED_TRACK;
static uint8_t g_loaded_qtrack = DISK2_NO_LOADED_TRACK;
static uint32_t g_loaded_track_length = 0U;
static uint32_t g_loaded_track_bit_count = 0U;
static uint8_t g_drive_rotation_valid[DISK2_DRIVE_COUNT];
static uint8_t g_drive_rotation_qtrack[DISK2_DRIVE_COUNT];
static uint8_t g_drive_rotation_raw_bits[DISK2_DRIVE_COUNT];
static uint32_t g_drive_rotation_bit_count[DISK2_DRIVE_COUNT];
static uint32_t g_load_fail_count = 0U;
static uint8_t g_disk2_enabled = 1U;
static uint8_t g_woz_write_enable[DISK2_DRIVE_COUNT];
static uint8_t g_woz_image_write_protected[DISK2_DRIVE_COUNT];
static uint32_t g_woz_flush_log_count = 0U;

typedef struct {
    uint32_t length;
    uint32_t bit_count;
    uint32_t bit_offset;
    uint32_t bit_timing;
    uint32_t seam_start;
    uint32_t seam_run;
    uint8_t raw_bits;
} disk2_track_stream_t;

static const uint8_t gcr_6and2[64] = {
    0x96U, 0x97U, 0x9AU, 0x9BU, 0x9DU, 0x9EU, 0x9FU, 0xA6U,
    0xA7U, 0xABU, 0xACU, 0xADU, 0xAEU, 0xAFU, 0xB2U, 0xB3U,
    0xB4U, 0xB5U, 0xB6U, 0xB7U, 0xB9U, 0xBAU, 0xBBU, 0xBCU,
    0xBDU, 0xBEU, 0xBFU, 0xCBU, 0xCDU, 0xCEU, 0xCFU, 0xD3U,
    0xD6U, 0xD7U, 0xD9U, 0xDAU, 0xDBU, 0xDCU, 0xDDU, 0xDEU,
    0xDFU, 0xE5U, 0xE6U, 0xE7U, 0xE9U, 0xEAU, 0xEBU, 0xECU,
    0xEDU, 0xEEU, 0xEFU, 0xF2U, 0xF3U, 0xF4U, 0xF5U, 0xF6U,
    0xF7U, 0xF9U, 0xFAU, 0xFBU, 0xFCU, 0xFDU, 0xFEU, 0xFFU
};

static uint8_t decode44_pair(const uint8_t *buf, uint32_t len, uint32_t pos);
static uint8_t woz_raw_bit_at(const uint8_t *buf, uint32_t bit_offset);
static int flush_dirty_track(uint8_t drive, uint8_t qtrack);

static uint8_t drive_reg_base(uint8_t drive)
{
    return (drive == 0U) ? DISK2_REG_D1_INFO : DISK2_REG_D2_INFO;
}

static void publish_drive(uint8_t drive)
{
    uint8_t base;
    const disk2_image_info_t *info;
    uint32_t info_word;

    if (drive >= DISK2_DRIVE_COUNT) {
        return;
    }

    base = drive_reg_base(drive);
    info = &g_disk2_info[drive];
    info_word = ((uint32_t)info->present) |
                ((uint32_t)info->read_only << 1) |
                ((uint32_t)info->format << 4);

    /* Staging region: PL reads it over AXI, so keep it out of the
     * CPU caches entirely. */
    Xil_SetTlbAttributes(DISK2_DDR_STAGING_BASE, NORM_NONCACHE);
    REG_WRITE(DISK2_REG(DISK2_REG_STAGING_BASE), DISK2_STAGING_BASE_OFFSET);
    REG_WRITE(DISK2_REG(base + 0U), info_word);
    REG_WRITE(DISK2_REG(base + 1U), info->file_size);
    REG_WRITE(DISK2_REG(base + 2U), info->track_count);
    REG_WRITE(DISK2_REG(base + 3U), 0U);
    REG_WRITE(DISK2_REG(base + 4U), 0U);
}

static uint32_t disk2_reg_read(uint8_t reg)
{
    return REG_READ(DISK2_REG(reg));
}

static void disk2_reg_write(uint8_t reg, uint32_t value)
{
    REG_WRITE(DISK2_REG(reg), value);
}

static void ack_dirty_track(uint8_t drive, uint8_t qtrack)
{
    disk2_reg_write(DISK2_REG_WRITE_INFO,
                    DISK2_WRITE_INFO_DIRTY_BIT |
                    ((uint32_t)qtrack << DISK2_WRITE_INFO_QTRACK_SHIFT) |
                    ((uint32_t)drive << DISK2_WRITE_INFO_DRIVE_SHIFT));
}

static uint8_t write_info_drive(uint32_t write_info)
{
    return (uint8_t)((write_info >> DISK2_WRITE_INFO_DRIVE_SHIFT) & 0x01U);
}

static uint8_t write_info_qtrack(uint32_t write_info)
{
    return (uint8_t)((write_info >> DISK2_WRITE_INFO_QTRACK_SHIFT) & 0xFFU);
}

static uint8_t write_info_has_pending(uint32_t write_info)
{
    return ((write_info & (DISK2_WRITE_INFO_DIRTY_BIT |
                           DISK2_WRITE_INFO_BUSY_BIT)) != 0U) ? 1U : 0U;
}

static int stage_track_to_ddr(uint32_t length)
{
    if (length == 0U || length > DISK2_TRACK_STREAM_BYTES) {
        return -1;
    }

    /* The non-cacheable DDR staging region is coherent with the PL bridge. */
    memcpy((void *)(uintptr_t)DISK2_DDR_STAGING_BASE,
           g_track_buf, DISK2_TRACK_STREAM_BYTES);
    return 0;
}

/* Compare the PL-visible DDR staging region with the source track buffer. */
int disk2_service_verify_staged(uint32_t *first_mismatch,
                                uint32_t *mismatch_count)
{
    static uint8_t verify_buf[DISK2_IO_CHUNK_BYTES]
        __attribute__((aligned(64)));
    uint32_t pos;
    uint32_t bad = 0U;
    uint32_t first = 0xFFFFFFFFU;

    if (g_loaded_qtrack == DISK2_NO_LOADED_TRACK) {
        return -1;
    }

    for (pos = 0U; pos < DISK2_TRACK_STREAM_BYTES;
         pos += DISK2_IO_CHUNK_BYTES) {
        uint32_t i;
        memcpy(verify_buf,
               (const void *)(uintptr_t)(DISK2_DDR_STAGING_BASE + pos),
               DISK2_IO_CHUNK_BYTES);
        for (i = 0U; i < DISK2_IO_CHUNK_BYTES; ++i) {
            if (verify_buf[i] != g_track_buf[pos + i]) {
                if (first == 0xFFFFFFFFU) {
                    first = pos + i;
                }
                bad++;
            }
        }
    }

    if (first_mismatch != NULL) {
        *first_mismatch = first;
    }
    if (mismatch_count != NULL) {
        *mismatch_count = bad;
    }
    return (bad == 0U) ? 0 : 1;
}

static uint8_t ascii_lower(uint8_t c)
{
    if (c >= (uint8_t)'A' && c <= (uint8_t)'Z') {
        return (uint8_t)(c + ((uint8_t)'a' - (uint8_t)'A'));
    }
    return c;
}

static uint8_t str_ieq(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return 0U;
    }
    while (*a != '\0' && *b != '\0') {
        if (ascii_lower((uint8_t)*a) != ascii_lower((uint8_t)*b)) {
            return 0U;
        }
        ++a;
        ++b;
    }
    return (*a == '\0' && *b == '\0') ? 1U : 0U;
}

static uint8_t path_ieq(const char *a, const char *b)
{
    uint8_t ac;
    uint8_t bc;

    if (a == NULL || b == NULL) {
        return 0U;
    }
    while (*a != '\0' && *b != '\0') {
        ac = (uint8_t)*a++;
        bc = (uint8_t)*b++;
        if (ac == (uint8_t)'\\') {
            ac = (uint8_t)'/';
        }
        if (bc == (uint8_t)'\\') {
            bc = (uint8_t)'/';
        }
        if (ascii_lower(ac) != ascii_lower(bc)) {
            return 0U;
        }
    }
    return (*a == '\0' && *b == '\0') ? 1U : 0U;
}

static const char *path_ext(const char *path)
{
    const char *dot = NULL;

    if (path == NULL) {
        return "";
    }
    while (*path != '\0') {
        if (*path == '.') {
            dot = path;
        }
        ++path;
    }
    return (dot != NULL) ? dot : "";
}

static uint32_t le32_load(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint16_t le16_load(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void le16_store(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void le32_store(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static int file_read_exact_at(FIL *file, uint32_t offset, void *buf, UINT len)
{
    FRESULT fr;
    UINT got = 0U;

    fr = f_lseek(file, offset);
    if (fr == FR_OK) {
        fr = f_read(file, buf, len, &got);
    }
    if (fr != FR_OK) {
        return -(int)fr;
    }
    return (got == len) ? 0 : -2;
}

static int file_write_exact_at(FIL *file, uint32_t offset, const void *buf, UINT len)
{
    FRESULT fr;
    UINT wrote = 0U;

    fr = f_lseek(file, offset);
    if (fr == FR_OK) {
        fr = f_write(file, buf, len, &wrote);
    }
    if (fr != FR_OK) {
        return -(int)fr;
    }
    return (wrote == len) ? 0 : -2;
}

static int read_drive_bytes(uint8_t drive, uint32_t offset, void *buf, uint32_t len)
{
    FIL file;
    FRESULT fr;
    int rc;

    if (drive >= DISK2_DRIVE_COUNT || buf == NULL) {
        return -1;
    }
    fr = f_open(&file, g_disk2_paths[drive], FA_READ);
    if (fr != FR_OK) {
        return -(int)fr;
    }
    rc = file_read_exact_at(&file, offset, buf, (UINT)len);
    (void)f_close(&file);
    return rc;
}

static int write_drive_bytes(uint8_t drive, uint32_t offset, const void *buf, uint32_t len)
{
    FIL file;
    FRESULT fr;
    int rc;

    if (drive >= DISK2_DRIVE_COUNT || buf == NULL) {
        return -1;
    }
    fr = f_open(&file, g_disk2_paths[drive], FA_READ | FA_WRITE);
    if (fr != FR_OK) {
        return -(int)fr;
    }
    rc = file_write_exact_at(&file, offset, buf, (UINT)len);
    if (rc == 0) {
        FRESULT sync_fr = f_sync(&file);
        if (sync_fr != FR_OK) {
            rc = -(int)sync_fr;
        }
    }
    (void)f_close(&file);
    return rc;
}

static int load_standard_image_cache(uint8_t drive,
                                     FIL *file,
                                     uint32_t data_offset,
                                     uint32_t data_size)
{
    FRESULT fr;
    uint32_t offset = 0U;

    if (drive >= DISK2_DRIVE_COUNT || file == NULL) {
        return -1;
    }
    g_standard_image_cached[drive] = 0U;
    g_standard_image_size[drive] = 0U;

    if (data_size > DISK2_STANDARD_MAX_IMAGE_BYTES) {
        return -2;
    }

    fr = f_lseek(file, data_offset);
    if (fr != FR_OK) {
        return -(int)fr;
    }

    while (offset < data_size) {
        UINT chunk;
        UINT got = 0U;
        uint32_t remaining = data_size - offset;

        chunk = (remaining > DISK2_IO_CHUNK_BYTES) ?
            (UINT)DISK2_IO_CHUNK_BYTES : (UINT)remaining;
        fr = f_read(file, &g_standard_image_buf[drive][offset], chunk, &got);
        if (fr != FR_OK) {
            return -(int)fr;
        }
        if (got != chunk) {
            return -2;
        }
        offset += (uint32_t)chunk;
    }

    g_standard_image_size[drive] = data_size;
    g_standard_image_cached[drive] = 1U;
    return 0;
}

static int standard_image_cache_bounds(uint8_t drive, uint32_t offset, uint32_t len)
{
    if (drive >= DISK2_DRIVE_COUNT) {
        return -1;
    }
    if (g_standard_image_cached[drive] == 0U) {
        return -1;
    }
    if (offset > g_standard_image_size[drive] ||
        len > g_standard_image_size[drive] - offset) {
        return -2;
    }
    return 0;
}

static int read_standard_image_bytes(uint8_t drive, uint32_t offset, void *buf, uint32_t len)
{
    int rc;

    if (buf == NULL) {
        return -1;
    }
    rc = standard_image_cache_bounds(drive, offset, len);
    if (rc != 0) {
        return rc;
    }
    memcpy(buf, &g_standard_image_buf[drive][offset], len);
    return 0;
}

static int write_standard_image_bytes(uint8_t drive, uint32_t offset, const void *buf, uint32_t len)
{
    int rc;

    if (buf == NULL) {
        return -1;
    }
    if (drive >= DISK2_DRIVE_COUNT) {
        return -1;
    }
    if (g_disk2_info[drive].read_only != 0U) {
        return -1;
    }
    rc = standard_image_cache_bounds(drive, offset, len);
    if (rc != 0) {
        return rc;
    }
    rc = write_drive_bytes(drive,
                           g_disk2_info[drive].image_data_offset + offset,
                           buf,
                           len);
    if (rc == 0) {
        memcpy(&g_standard_image_buf[drive][offset], buf, len);
    }
    return rc;
}

static int sector_track_offset(uint8_t drive, uint8_t track, uint32_t *offset_out)
{
    const disk2_image_info_t *info;
    uint32_t offset;

    if (drive >= DISK2_DRIVE_COUNT || offset_out == NULL) {
        return -1;
    }
    info = &g_disk2_info[drive];
    if (info->present == 0U ||
        (info->format != DISK2_IMAGE_DSK &&
         info->format != DISK2_IMAGE_DO &&
         info->format != DISK2_IMAGE_PO)) {
        return -1;
    }
    if ((uint32_t)track >= info->track_count) {
        return -2;
    }
    offset = (uint32_t)track * DISK2_DSK_SECTORS_PER_TRACK * DISK2_SECTOR_BYTES;
    if (offset + (DISK2_DSK_SECTORS_PER_TRACK * DISK2_SECTOR_BYTES) >
        info->image_data_size) {
        return -2;
    }
    *offset_out = offset;
    return 0;
}

static int nib_track_offset(uint8_t drive, uint8_t track, uint32_t *offset_out)
{
    const disk2_image_info_t *info;
    uint32_t offset;

    if (drive >= DISK2_DRIVE_COUNT || offset_out == NULL) {
        return -1;
    }
    info = &g_disk2_info[drive];
    if (info->present == 0U || info->format != DISK2_IMAGE_NIB) {
        return -1;
    }
    if ((uint32_t)track >= info->track_count) {
        return -2;
    }
    offset = (uint32_t)track * DISK2_NIB_TRACK_BYTES;
    if (offset + DISK2_NIB_TRACK_BYTES > info->image_data_size) {
        return -2;
    }
    *offset_out = offset;
    return 0;
}

static disk2_image_format_t format_from_path(const char *path)
{
    const char *ext = path_ext(path);

    if (str_ieq(ext, ".woz") != 0U) {
        return DISK2_IMAGE_WOZ;
    }
    if (str_ieq(ext, ".nib") != 0U) {
        return DISK2_IMAGE_NIB;
    }
    if (str_ieq(ext, ".dsk") != 0U) {
        return DISK2_IMAGE_DSK;
    }
    if (str_ieq(ext, ".do") != 0U) {
        return DISK2_IMAGE_DO;
    }
    if (str_ieq(ext, ".po") != 0U) {
        return DISK2_IMAGE_PO;
    }
    if (str_ieq(ext, ".2mg") != 0U || str_ieq(ext, ".2img") != 0U) {
        return DISK2_IMAGE_2MG;
    }
    return DISK2_IMAGE_NONE;
}

static void copy_path(char *dst, size_t dst_len, const char *src)
{
    size_t i;

    if (dst == NULL || dst_len == 0U) {
        return;
    }
    if (src == NULL) {
        src = "";
    }
    for (i = 0U; i + 1U < dst_len && src[i] != '\0'; ++i) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

/* Match AppleWin's 5.25-inch DO/PO size acceptance exactly. In particular,
 * a few historically distributed 140K images have short or extra tails and
 * are intentionally accepted even though their size is not a multiple of 4K. */
static uint8_t sector_image_size_valid(uint32_t size, uint32_t *tracks_out)
{
    uint8_t valid;

    if (size > DISK2_STANDARD_MAX_TRACKS *
               DISK2_DSK_SECTORS_PER_TRACK * DISK2_SECTOR_BYTES) {
        return 0U;
    }
    if (size >= (DISK2_STANDARD_TRACKS + 1U) *
                DISK2_DSK_SECTORS_PER_TRACK * DISK2_SECTOR_BYTES) {
        valid = (((size - DISK2_STANDARD_TRACKS *
                         DISK2_DSK_SECTORS_PER_TRACK * DISK2_SECTOR_BYTES) %
                  (DISK2_DSK_SECTORS_PER_TRACK * DISK2_SECTOR_BYTES)) == 0U) ?
            1U : 0U;
    } else {
        valid = (((size >= 143105U) && (size <= 143364U)) ||
                 size == 143403U || size == 143488U) ? 1U : 0U;
    }
    if (valid != 0U && tracks_out != NULL) {
        *tracks_out = size /
            (DISK2_DSK_SECTORS_PER_TRACK * DISK2_SECTOR_BYTES);
    }
    return valid;
}

static uint8_t nib_image_size_valid(uint32_t size, uint32_t *tracks_out)
{
    if (size < DISK2_STANDARD_TRACKS * DISK2_NIB_TRACK_BYTES ||
        size > DISK2_STANDARD_MAX_TRACKS * DISK2_NIB_TRACK_BYTES ||
        (size % DISK2_NIB_TRACK_BYTES) != 0U) {
        return 0U;
    }
    if (tracks_out != NULL) {
        *tracks_out = size / DISK2_NIB_TRACK_BYTES;
    }
    return 1U;
}

/* A 13-sector NIB uses D5 AA B5 address prologues. Do not reject an odd
 * protected track merely because B5 appears in data: only identify the image
 * as 13-sector when it has B5 address fields and no 16-sector address fields. */
static uint8_t cached_nib_address_valid(const uint8_t *track,
                                        uint32_t pos,
                                        uint8_t sector_count)
{
    uint8_t encoded[11];
    uint8_t volume;
    uint8_t track_number;
    uint8_t sector;
    uint8_t checksum;

    for (uint32_t i = 0U; i < sizeof(encoded); ++i) {
        encoded[i] = track[(pos + 3U + i) % DISK2_NIB_TRACK_BYTES];
    }
    for (uint32_t i = 0U; i < 8U; ++i) {
        if ((encoded[i] & 0xAAU) != 0xAAU) {
            return 0U;
        }
    }
    if (encoded[8] != 0xDEU ||
        encoded[9] != 0xAAU ||
        encoded[10] != 0xEBU) {
        return 0U;
    }
    volume = decode44_pair(encoded, sizeof(encoded), 0U);
    track_number = decode44_pair(encoded, sizeof(encoded), 2U);
    sector = decode44_pair(encoded, sizeof(encoded), 4U);
    checksum = decode44_pair(encoded, sizeof(encoded), 6U);
    return (sector < sector_count &&
            (uint8_t)(volume ^ track_number ^ sector) == checksum) ? 1U : 0U;
}

static uint8_t cached_nib_is_13_sector(uint8_t drive, uint32_t track_count)
{
    uint32_t addr13 = 0U;
    uint32_t addr16 = 0U;

    if (drive >= DISK2_DRIVE_COUNT ||
        g_standard_image_cached[drive] == 0U) {
        return 0U;
    }
    for (uint32_t track = 0U; track < track_count; ++track) {
        const uint8_t *data = &g_standard_image_buf[drive][track *
                                                           DISK2_NIB_TRACK_BYTES];
        for (uint32_t pos = 0U; pos < DISK2_NIB_TRACK_BYTES; ++pos) {
            uint32_t p1 = (pos + 1U) % DISK2_NIB_TRACK_BYTES;
            uint32_t p2 = (pos + 2U) % DISK2_NIB_TRACK_BYTES;

            if (data[pos] == 0xD5U && data[p1] == 0xAAU) {
                if (data[p2] == 0x96U &&
                    cached_nib_address_valid(data, pos, 16U) != 0U) {
                    ++addr16;
                } else if (data[p2] == 0xB5U &&
                           cached_nib_address_valid(data, pos, 13U) != 0U) {
                    ++addr13;
                }
            }
        }
    }
    return (addr13 != 0U && addr16 == 0U) ? 1U : 0U;
}

static int woz_calculate_crc(FIL *file, uint32_t file_size, uint32_t *crc_out)
{
    uint32_t crc;
    uint32_t pos;
    int rc;

    if (file == NULL || crc_out == NULL ||
        file_size < DISK2_WOZ_HEADER_SIZE) {
        return -1;
    }
    crc = crc32_init();
    for (pos = DISK2_WOZ_HEADER_SIZE; pos < file_size; ) {
        uint32_t chunk = file_size - pos;

        if (chunk > sizeof(g_file_io_buf)) {
            chunk = sizeof(g_file_io_buf);
        }
        rc = file_read_exact_at(file, pos, g_file_io_buf, (UINT)chunk);
        if (rc != 0) {
            return rc;
        }
        crc = crc32_update(crc, g_file_io_buf, chunk);
        pos += chunk;
    }
    *crc_out = crc32_finish(crc);
    return 0;
}

static int probe_2mg(FIL *file, disk2_image_info_t *info)
{
    uint8_t hdr[DISK2_2MG_HEADER_SIZE];
    uint32_t format;
    uint32_t flags;
    uint32_t blocks;
    uint32_t data_offset;
    uint32_t data_size;
    uint32_t tracks = 0U;
    int rc;

    if (file == NULL || info == NULL || info->file_size < sizeof(hdr)) {
        return -2;
    }
    rc = file_read_exact_at(file, 0U, hdr, sizeof(hdr));
    if (rc != 0) {
        return rc;
    }
    if (le32_load(&hdr[0]) != DISK2_2MG_MAGIC ||
        le16_load(&hdr[8]) != DISK2_2MG_HEADER_SIZE ||
        le16_load(&hdr[10]) > 1U) {
        return -2;
    }

    format = le32_load(&hdr[12]);
    flags = le32_load(&hdr[16]);
    blocks = le32_load(&hdr[20]);
    data_offset = le32_load(&hdr[24]);
    data_size = le32_load(&hdr[28]);
    if (data_offset == 0U) {
        data_offset = DISK2_2MG_HEADER_SIZE;
    }
    if (format == DISK2_2MG_FORMAT_PRODOS && data_size == 0U) {
        if (blocks > (0xFFFFFFFFU / 512U)) {
            return -2;
        }
        data_size = blocks * 512U;
    }
    if (data_offset < DISK2_2MG_HEADER_SIZE ||
        data_offset > info->file_size ||
        data_size > info->file_size - data_offset) {
        return -2;
    }

    switch (format) {
    case DISK2_2MG_FORMAT_DOS33:
        if (sector_image_size_valid(data_size, &tracks) == 0U) {
            return -2;
        }
        info->format = DISK2_IMAGE_DO;
        break;
    case DISK2_2MG_FORMAT_PRODOS:
        if (sector_image_size_valid(data_size, &tracks) == 0U) {
            return -2;
        }
        info->format = DISK2_IMAGE_PO;
        break;
    case DISK2_2MG_FORMAT_NIB:
        /* AppleWin only accepts the standard 35-track NIB payload in 2IMG. */
        if (data_size != DISK2_STANDARD_TRACKS * DISK2_NIB_TRACK_BYTES) {
            return -2;
        }
        tracks = DISK2_STANDARD_TRACKS;
        info->format = DISK2_IMAGE_NIB;
        break;
    default:
        return -2;
    }

    info->container_2mg = 1U;
    /* AppleWin only applies the optional 2IMG volume number to DOS-order
     * sector images. ProDOS-order sector images retain the normal $FE
     * address-field volume. */
    info->volume_number =
        (format == DISK2_2MG_FORMAT_DOS33 &&
         (flags & DISK2_2MG_FLAG_VOLUME_VALID) != 0U) ?
            (uint8_t)flags : DISK2_DEFAULT_VOLUME_NUMBER;
    info->read_only = (flags & DISK2_2MG_FLAG_LOCKED) ? 1U : 0U;
    info->image_data_offset = data_offset;
    info->image_data_size = data_size;
    info->track_count = tracks;
    info->logical_blocks = data_size / 512U;
    return 0;
}

static void clear_drive_state(uint8_t drive)
{
    if (drive >= DISK2_DRIVE_COUNT) {
        return;
    }
    g_woz_write_enable[drive] = 0U;
    g_woz_image_write_protected[drive] = 0U;
    g_standard_image_cached[drive] = 0U;
    g_standard_image_size[drive] = 0U;
    g_drive_rotation_valid[drive] = 0U;
    g_drive_rotation_qtrack[drive] = 0U;
    g_drive_rotation_raw_bits[drive] = 0U;
    g_drive_rotation_bit_count[drive] = 0U;
    memset(&g_disk2_info[drive], 0, sizeof(g_disk2_info[drive]));
    if (g_loaded_drive == drive) {
        g_loaded_drive = DISK2_NO_LOADED_TRACK;
        g_loaded_qtrack = DISK2_NO_LOADED_TRACK;
        g_loaded_track_length = 0U;
        g_loaded_track_bit_count = 0U;
        disk2_reg_write(DISK2_REG_TRACK_INFO, 0U);
        disk2_reg_write(DISK2_REG_TRACK_BIT_COUNT, 0U);
    }
    publish_drive(drive);
}

static int probe_woz(FIL *file, disk2_image_info_t *info)
{
    FRESULT fr;
    UINT got = 0U;
    uint8_t hdr[DISK2_WOZ_HEADER_SIZE];
    uint32_t pos = DISK2_WOZ_HEADER_SIZE;
    uint8_t info_version = 0U;
    uint8_t have_info = 0U;
    uint8_t have_tmap = 0U;
    uint8_t have_trks = 0U;
    uint32_t expected_crc;
    uint32_t actual_crc;

    memset(info->woz_tmap, DISK2_WOZ_TMAP_EMPTY, sizeof(info->woz_tmap));
    info->woz_version = 0U;
    info->woz_info_offset = 0U;
    info->woz_tmap_offset = 0U;
    info->woz_trks_offset = 0U;
    info->woz_trks_size = 0U;
    info->woz_optimal_bit_timing = DISK2_WOZ_OPTIMAL_BIT_TIMING_5_25;

    fr = f_lseek(file, 0U);
    if (fr != FR_OK) {
        return -(int)fr;
    }
    fr = f_read(file, hdr, sizeof(hdr), &got);
    if (fr != FR_OK) {
        return -(int)fr;
    }
    if (got != sizeof(hdr) ||
        (memcmp(hdr, "WOZ1", 4U) != 0 && memcmp(hdr, "WOZ2", 4U) != 0) ||
        hdr[4] != 0xFFU || hdr[5] != 0x0AU ||
        hdr[6] != 0x0DU || hdr[7] != 0x0AU) {
        return -2;
    }
    expected_crc = le32_load(&hdr[8]);
    if (expected_crc != 0U) {
        int crc_rc = woz_calculate_crc(file, info->file_size, &actual_crc);

        if (crc_rc != 0) {
            return crc_rc;
        }
        if (actual_crc != expected_crc) {
            return -2;
        }
    }
    info->woz_version = (uint8_t)(hdr[3] - (uint8_t)'0');

    while (pos + DISK2_WOZ_CHUNK_HEADER_SIZE <= info->file_size) {
        uint8_t chunk[DISK2_WOZ_CHUNK_HEADER_SIZE];
        uint32_t chunk_size;
        uint32_t next_pos;

        fr = f_lseek(file, pos);
        if (fr != FR_OK) {
            return -(int)fr;
        }
        fr = f_read(file, chunk, sizeof(chunk), &got);
        if (fr != FR_OK) {
            return -(int)fr;
        }
        if (got != sizeof(chunk)) {
            break;
        }

        chunk_size = le32_load(&chunk[4]);
        if (memcmp(chunk, "INFO", 4U) == 0) {
            uint8_t info_chunk[DISK2_WOZ_INFO_CHUNK_SIZE];

            if (have_info != 0U || chunk_size < DISK2_WOZ_INFO_CHUNK_SIZE) {
                return -2;
            }
            have_info = 1U;
            info->woz_info_offset = pos + DISK2_WOZ_CHUNK_HEADER_SIZE;
            fr = f_lseek(file, info->woz_info_offset);
            if (fr != FR_OK) {
                return -(int)fr;
            }
            fr = f_read(file, info_chunk, sizeof(info_chunk), &got);
            if (fr != FR_OK) {
                return -(int)fr;
            }
            if (got != sizeof(info_chunk) ||
                info_chunk[DISK2_WOZ_INFO_DISK_TYPE_OFFSET] != DISK2_WOZ_DISK_TYPE_5_25) {
                return -2;
            }
            info_version = info_chunk[0];
            if (info_version >= 2U &&
                info_chunk[DISK2_WOZ_INFO_BOOT_SECTOR_FORMAT_OFFSET] ==
                    DISK2_WOZ_BOOT_SECTOR_13) {
                return -2;
            }
            info->read_only =
                (info_chunk[DISK2_WOZ_INFO_WRITE_PROTECT_OFFSET] != 0U) ? 1U : 0U;
            if (info_version >= 2U &&
                info_chunk[DISK2_WOZ_INFO_OPTIMAL_BIT_TIMING_OFFSET] != 0U) {
                info->woz_optimal_bit_timing =
                    info_chunk[DISK2_WOZ_INFO_OPTIMAL_BIT_TIMING_OFFSET];
            }
        } else if (memcmp(chunk, "TMAP", 4U) == 0) {
            if (have_tmap != 0U) {
                return -2;
            }
            have_tmap = 1U;
            info->woz_tmap_offset = pos + DISK2_WOZ_CHUNK_HEADER_SIZE;
            if (chunk_size >= DISK2_WOZ_TMAP_SIZE) {
                fr = f_lseek(file, info->woz_tmap_offset);
                if (fr != FR_OK) {
                    return -(int)fr;
                }
                fr = f_read(file, info->woz_tmap, sizeof(info->woz_tmap), &got);
                if (fr != FR_OK) {
                    return -(int)fr;
                }
                if (got != sizeof(info->woz_tmap)) {
                    return -2;
                }
            } else {
                return -2;
            }
        } else if (memcmp(chunk, "TRKS", 4U) == 0) {
            if (have_trks != 0U) {
                return -2;
            }
            have_trks = 1U;
            info->woz_trks_offset = pos + DISK2_WOZ_CHUNK_HEADER_SIZE;
            info->woz_trks_size = chunk_size;
        }

        next_pos = pos + DISK2_WOZ_CHUNK_HEADER_SIZE + chunk_size;
        if (next_pos < pos || next_pos > info->file_size) {
            return -2;
        }
        pos = next_pos;
    }

    if (have_info == 0U || have_tmap == 0U || have_trks == 0U) {
        return -2;
    }
    if (info->woz_version == 1U) {
        if (info->woz_trks_size % DISK2_WOZ1_TRACK_BYTES != 0U) {
            return -2;
        }
    } else if (info->woz_version == 2U) {
        if (info->woz_trks_size < DISK2_WOZ_TRKV2_TABLE_BYTES) {
            return -2;
        }
    } else {
        return -2;
    }

    info->track_count = DISK2_WOZ_MAX_TRACKS;
    info->logical_blocks = info->file_size / 512U;
    return 0;
}

static int probe_file(uint8_t drive)
{
    FIL file;
    FRESULT fr;
    const char *path;
    disk2_image_info_t info;
    disk2_image_format_t format;
    uint32_t tracks = 0U;

    if (drive >= DISK2_DRIVE_COUNT) {
        return -1;
    }

    path = g_disk2_paths[drive];
    clear_drive_state(drive);
    if (path[0] == '\0') {
        return 0;
    }

    format = format_from_path(path);
    if (format == DISK2_IMAGE_NONE) {
        return -2;
    }

    fr = f_open(&file, path, FA_READ);
    if (fr != FR_OK) {
        return -(int)fr;
    }

    memset(&info, 0, sizeof(info));
    info.present = 1U;
    info.read_only = 0U;
    info.volume_number = DISK2_DEFAULT_VOLUME_NUMBER;
    info.format = format;
    info.file_size = (uint32_t)f_size(&file);
    info.image_data_offset = 0U;
    info.image_data_size = info.file_size;

    if (format == DISK2_IMAGE_WOZ) {
        int rc = probe_woz(&file, &info);
        (void)f_close(&file);
        if (rc != 0) {
            clear_drive_state(drive);
            return rc;
        }
        g_woz_image_write_protected[drive] = info.read_only;
        if (info.read_only == 0U) {
            fr = f_open(&file, path, FA_READ | FA_WRITE);
            if (fr == FR_OK) {
                g_woz_write_enable[drive] = 1U;
                (void)f_close(&file);
            } else {
                info.read_only = 1U;
            }
        }
    } else {
        int rc;

        if (format == DISK2_IMAGE_2MG) {
            rc = probe_2mg(&file, &info);
        } else if (format == DISK2_IMAGE_NIB) {
            rc = (nib_image_size_valid(info.image_data_size, &tracks) != 0U) ?
                0 : -2;
            info.track_count = tracks;
            info.logical_blocks = info.image_data_size / 512U;
        } else if (format == DISK2_IMAGE_DSK ||
                   format == DISK2_IMAGE_DO ||
                   format == DISK2_IMAGE_PO) {
            rc = (sector_image_size_valid(info.image_data_size, &tracks) != 0U) ?
                0 : -2;
            info.track_count = tracks;
            info.logical_blocks = info.image_data_size / 512U;
        } else {
            rc = -2;
        }
        if (rc != 0) {
            (void)f_close(&file);
            clear_drive_state(drive);
            return rc;
        }
        rc = load_standard_image_cache(drive,
                                       &file,
                                       info.image_data_offset,
                                       info.image_data_size);
        (void)f_close(&file);
        if (rc != 0) {
            clear_drive_state(drive);
            return rc;
        }
        if (info.format == DISK2_IMAGE_NIB &&
            cached_nib_is_13_sector(drive, info.track_count) != 0U) {
            clear_drive_state(drive);
            return -2;
        }
        if (info.read_only == 0U) {
            fr = f_open(&file, path, FA_READ | FA_WRITE);
            if (fr == FR_OK) {
                info.read_only = 0U;
                (void)f_close(&file);
            } else {
                info.read_only = 1U;
            }
        }
    }

    g_disk2_info[drive] = info;
    publish_drive(drive);
    g_load_fail_count = 0U;

    if (g_uart_base != 0U) {
        uart_puts(g_uart_base, "Disk II D");
        uart_putdec(g_uart_base, (uint32_t)drive + 1U);
        uart_puts(g_uart_base, ": ");
        uart_puts(g_uart_base, disk2_service_format_name(info.format));
        uart_puts(g_uart_base, " image selected\r\n");
    }
    return 0;
}

static uint8_t requested_drive(uint32_t track_info)
{
    return (uint8_t)((track_info >> DISK2_TRACK_INFO_CUR_DRIVE_SHIFT) & 0x01U);
}

static uint8_t requested_qtrack(uint32_t track_info)
{
    return (uint8_t)((track_info >> DISK2_TRACK_INFO_CUR_QTRACK_SHIFT) & 0xFFU);
}

static uint8_t loaded_drive(uint32_t track_info)
{
    return (uint8_t)((track_info >> DISK2_TRACK_INFO_LOAD_DRIVE_SHIFT) & 0x01U);
}

static uint8_t loaded_qtrack(uint32_t track_info)
{
    return (uint8_t)((track_info >> DISK2_TRACK_INFO_LOADED_QTRACK_SHIFT) & 0xFFU);
}

static uint8_t track_request_matches(uint8_t drive, uint8_t qtrack)
{
    uint32_t track_info = disk2_reg_read(DISK2_REG_TRACK_INFO);

    return (requested_drive(track_info) == drive &&
            requested_qtrack(track_info) == qtrack &&
            (track_info & DISK2_TRACK_INFO_MATCH_BIT) == 0U) ? 1U : 0U;
}

static uint8_t qtrack_to_track(uint8_t qtrack, uint32_t track_count)
{
    uint32_t track = ((uint32_t)qtrack) / 4U;

    if (track_count == 0U || track >= track_count) {
        return DISK2_NO_PHYSICAL_TRACK;
    }
    return (uint8_t)track;
}

static uint32_t woz_track_switch_bit_offset(uint32_t old_bit_offset,
                                            uint32_t old_bit_count,
                                            uint32_t new_bit_count)
{
    uint64_t scaled;

    if (new_bit_count == 0U) {
        return 0U;
    }
    if (old_bit_count == 0U) {
        old_bit_count = 8U;
    }

    scaled = ((uint64_t)old_bit_offset * (uint64_t)new_bit_count) /
             (uint64_t)old_bit_count;
    scaled += 7U;
    if (scaled >= new_bit_count) {
        return 0U;
    }
    return (uint32_t)scaled;
}

static void nib_put(uint8_t *buf, uint32_t *pos, uint8_t value)
{
    if (*pos < DISK2_NIB_TRACK_BYTES) {
        buf[*pos] = value;
        *pos += 1U;
    }
}

static void nib_put_repeat(uint8_t *buf, uint32_t *pos, uint8_t value, uint32_t count)
{
    for (uint32_t i = 0U; i < count; ++i) {
        nib_put(buf, pos, value);
    }
}

static void nib_put_44(uint8_t *buf, uint32_t *pos, uint8_t value)
{
    nib_put(buf, pos, (uint8_t)((value >> 1) | 0xAAU));
    nib_put(buf, pos, (uint8_t)(value | 0xAAU));
}

static void encode_6and2_sector(uint8_t *encoded, const uint8_t *sector)
{
    uint8_t raw[343];
    uint8_t offset = 0xACU;
    uint32_t out = 0U;
    uint8_t saved;

    while (offset != 0x02U) {
        uint8_t value = 0U;

        value = (uint8_t)((value << 2) |
                          ((sector[offset] & 0x01U) << 1) |
                          ((sector[offset] & 0x02U) >> 1));
        offset = (uint8_t)(offset - 0x56U);
        value = (uint8_t)((value << 2) |
                          ((sector[offset] & 0x01U) << 1) |
                          ((sector[offset] & 0x02U) >> 1));
        offset = (uint8_t)(offset - 0x56U);
        value = (uint8_t)((value << 2) |
                          ((sector[offset] & 0x01U) << 1) |
                          ((sector[offset] & 0x02U) >> 1));
        offset = (uint8_t)(offset - 0x53U);
        raw[out++] = (uint8_t)(value << 2);
    }
    raw[out - 2U] &= 0x3FU;
    raw[out - 1U] &= 0x3FU;

    for (uint32_t i = 0U; i < DISK2_SECTOR_BYTES; ++i) {
        raw[out++] = sector[i];
    }

    saved = 0U;
    for (uint32_t i = 0U; i < 342U; ++i) {
        encoded[i] = gcr_6and2[(saved ^ raw[i]) >> 2];
        saved = raw[i];
    }
    encoded[342] = gcr_6and2[saved >> 2];
}

static uint8_t sector_image_is_prodos_order(disk2_image_format_t format)
{
    return (format == DISK2_IMAGE_PO) ? 1U : 0U;
}

static uint8_t sector_image_file_sector(uint8_t physical_sector, uint8_t is_prodos)
{
    if (physical_sector == 15U) {
        return 15U;
    }
    return (uint8_t)(((uint32_t)physical_sector * (is_prodos ? 8U : 7U)) % 15U);
}

static int read_sector_physical_track(uint8_t drive, uint8_t track, uint8_t *buf)
{
    uint32_t offset;
    int rc = sector_track_offset(drive, track, &offset);

    if (rc != 0) {
        return rc;
    }
    return read_standard_image_bytes(drive, offset, buf,
                                     DISK2_DSK_SECTORS_PER_TRACK * DISK2_SECTOR_BYTES);
}

static int write_sector_physical_track(uint8_t drive, uint8_t track, const uint8_t *buf)
{
    uint32_t offset;
    int rc;

    if (g_disk2_info[drive].read_only != 0U) {
        return -1;
    }
    rc = sector_track_offset(drive, track, &offset);
    if (rc != 0) {
        return rc;
    }
    return write_standard_image_bytes(drive, offset, buf,
                                      DISK2_DSK_SECTORS_PER_TRACK * DISK2_SECTOR_BYTES);
}

static uint32_t nibblize_sector_track(uint8_t *nib,
                                      const uint8_t *sector_track,
                                      uint8_t track,
                                      uint8_t is_prodos,
                                      uint8_t volume)
{
    uint32_t pos = 0U;
    uint8_t encoded[343];

    memset(nib, 0xFF, DISK2_NIB_TRACK_BYTES);
    nib_put_repeat(nib, &pos, 0xFFU, 48U);
    for (uint8_t physical_sector = 0U;
         physical_sector < DISK2_DSK_SECTORS_PER_TRACK;
         ++physical_sector) {
        uint8_t file_sector = sector_image_file_sector(physical_sector, is_prodos);
        uint8_t checksum = (uint8_t)(volume ^ track ^ physical_sector);

        nib_put(nib, &pos, 0xD5U);
        nib_put(nib, &pos, 0xAAU);
        nib_put(nib, &pos, 0x96U);
        nib_put_44(nib, &pos, volume);
        nib_put_44(nib, &pos, track);
        nib_put_44(nib, &pos, physical_sector);
        nib_put_44(nib, &pos, checksum);
        nib_put(nib, &pos, 0xDEU);
        nib_put(nib, &pos, 0xAAU);
        nib_put(nib, &pos, 0xEBU);

        nib_put_repeat(nib, &pos, 0xFFU, 6U);
        nib_put(nib, &pos, 0xD5U);
        nib_put(nib, &pos, 0xAAU);
        nib_put(nib, &pos, 0xADU);

        encode_6and2_sector(encoded, &sector_track[(uint32_t)file_sector * DISK2_SECTOR_BYTES]);
        for (uint32_t i = 0U; i < sizeof(encoded); ++i) {
            nib_put(nib, &pos, encoded[i]);
        }

        nib_put(nib, &pos, 0xDEU);
        nib_put(nib, &pos, 0xAAU);
        nib_put(nib, &pos, 0xEBU);
        nib_put_repeat(nib, &pos, 0xFFU, 27U);
    }
    return pos;
}

static int gcr_decode_6and2(uint8_t value, uint8_t *decoded_out)
{
    static uint8_t table_ready = 0U;
    static uint8_t table[0x80];
    uint8_t decoded;

    if (decoded_out == NULL || value < 0x80U) {
        return -1;
    }

    if (table_ready == 0U) {
        memset(table, 0xFF, sizeof(table));
        for (uint32_t i = 0U; i < 64U; ++i) {
            table[gcr_6and2[i] - 0x80U] = (uint8_t)(i << 2);
        }
        table_ready = 1U;
    }

    decoded = table[value - 0x80U];
    if (decoded == 0xFFU) {
        return -1;
    }
    *decoded_out = decoded;
    return 0;
}

static int decode_6and2_sector(uint8_t *sector, const uint8_t *encoded)
{
    uint8_t raw[343];
    uint8_t saved = 0U;
    uint8_t offset = 0xACU;
    uint32_t low_index = 0U;

    if (sector == NULL || encoded == NULL) {
        return -1;
    }

    for (uint32_t i = 0U; i < sizeof(raw); ++i) {
        if (gcr_decode_6and2(encoded[i], &raw[i]) != 0) {
            return -1;
        }
    }

    for (uint32_t i = 0U; i < 342U; ++i) {
        uint8_t value = (uint8_t)(saved ^ raw[i]);
        raw[i] = value;
        saved = value;
    }
    if (raw[342] != saved) {
        return -1;
    }

    while (offset != 0x02U) {
        const uint8_t low_bits = raw[low_index++];

        if (offset >= 0xACU) {
            sector[offset] = (uint8_t)((raw[0x56U + offset] & 0xFCU) |
                                       ((low_bits & 0x80U) >> 7) |
                                       ((low_bits & 0x40U) >> 5));
        }

        offset = (uint8_t)(offset - 0x56U);
        sector[offset] = (uint8_t)((raw[0x56U + offset] & 0xFCU) |
                                   ((low_bits & 0x20U) >> 5) |
                                   ((low_bits & 0x10U) >> 3));

        offset = (uint8_t)(offset - 0x56U);
        sector[offset] = (uint8_t)((raw[0x56U + offset] & 0xFCU) |
                                   ((low_bits & 0x08U) >> 3) |
                                   ((low_bits & 0x04U) >> 1));

        offset = (uint8_t)(offset - 0x53U);
    }
    return 0;
}

static int denibblize_sector_track(const uint8_t *nib,
                                   uint32_t len,
                                   uint8_t *sector_track,
                                   uint8_t is_prodos,
                                   uint8_t expected_track)
{
    uint32_t offset = 0U;
    int sector = -1;
    uint16_t sectors_seen = 0U;
    uint8_t encoded[384];
    uint8_t decoded_sector[DISK2_SECTOR_BYTES];

    if (nib == NULL || sector_track == NULL || len < 512U) {
        return -1;
    }
    /* AppleWin writes a freshly zeroed 4K track and fills only sectors that
     * have a valid address/data pair. This is important for protected disks
     * whose formatter intentionally leaves sectors absent. */
    memset(sector_track, 0,
           DISK2_DSK_SECTORS_PER_TRACK * DISK2_SECTOR_BYTES);

    /*
     * Inspect every byte position instead of assuming that the first 33
     * occurrences of $D5 are the 32 address/data prologues. A real Disk II
     * write splice may leave harmless $D5 bytes in a gap. Counting those as
     * fields made a fully valid track stop at bitmap $7FFF and reject sector
     * 15. Two passes preserve circular address/data pairing if a field spans
     * the staging-buffer seam; normally all 16 sectors finish in pass one.
     */
    for (uint32_t scanned = 0U;
         scanned < (len * 2U) && sectors_seen != 0xFFFFU;
         ++scanned) {
        uint32_t next1 = offset + 1U;
        uint32_t next2;

        if (next1 >= len) {
            next1 = 0U;
        }
        next2 = next1 + 1U;
        if (next2 >= len) {
            next2 = 0U;
        }

        if (nib[offset] == 0xD5U &&
            nib[next1] == 0xAAU &&
            (nib[next2] == 0x96U || nib[next2] == 0xADU)) {
            uint32_t temp_offset = next2 + 1U;

            if (temp_offset >= len) {
                temp_offset = 0U;
            }

            for (uint32_t i = 0U; i < sizeof(encoded); ++i) {
                encoded[i] = nib[temp_offset++];
                if (temp_offset >= len) {
                    temp_offset = 0U;
                }
            }

            if (nib[next2] == 0x96U) {
                uint8_t volume = decode44_pair(encoded, sizeof(encoded), 0U);
                uint8_t track = decode44_pair(encoded, sizeof(encoded), 2U);
                uint8_t address_sector =
                    decode44_pair(encoded, sizeof(encoded), 4U);
                uint8_t checksum = decode44_pair(encoded, sizeof(encoded), 6U);
                uint8_t address_valid = 1U;

                for (uint32_t i = 0U; i < 8U; ++i) {
                    if ((encoded[i] & 0xAAU) != 0xAAU) {
                        address_valid = 0U;
                    }
                }
                if (track != expected_track ||
                    address_sector >= DISK2_DSK_SECTORS_PER_TRACK ||
                    (uint8_t)(volume ^ track ^ address_sector) != checksum ||
                    encoded[8] != 0xDEU ||
                    encoded[9] != 0xAAU ||
                    encoded[10] != 0xEBU) {
                    address_valid = 0U;
                }
                /*
                 * An invalid prologue-shaped sequence may be splice noise in
                 * the gap between a valid address and its data field. Do not
                 * let it erase the pending valid address; the next genuine
                 * address will replace it naturally.
                 */
                if (address_valid != 0U) {
                    sector = (int)address_sector;
                }
            } else {
                if (sector >= 0 &&
                    sector < (int)DISK2_DSK_SECTORS_PER_TRACK &&
                    encoded[343] == 0xDEU &&
                    encoded[344] == 0xAAU &&
                    encoded[345] == 0xEBU &&
                    decode_6and2_sector(decoded_sector, encoded) == 0) {
                    uint8_t file_sector =
                        sector_image_file_sector((uint8_t)sector, is_prodos);
                    memcpy(&sector_track[(uint32_t)file_sector * DISK2_SECTOR_BYTES],
                           decoded_sector,
                           sizeof(decoded_sector));
                    sectors_seen |= (uint16_t)(1U << (uint8_t)sector);
                    sector = -1;
                }
            }
        }

        offset = next1;
    }

    return 0;
}

static void count_prologue(const uint8_t *buf,
                           uint32_t len,
                           uint8_t b0,
                           uint8_t b1,
                           uint8_t b2,
                           uint32_t *count_out,
                           uint32_t *first_out)
{
    uint32_t count = 0U;
    uint32_t first = 0xFFFFFFFFU;

    if (buf == NULL || len < 3U) {
        *count_out = 0U;
        *first_out = first;
        return;
    }

    for (uint32_t i = 0U; i < len; ++i) {
        if (buf[i] == b0 &&
            buf[(i + 1U) % len] == b1 &&
            buf[(i + 2U) % len] == b2) {
            if (count == 0U) {
                first = i;
            }
            ++count;
        }
    }

    *count_out = count;
    *first_out = first;
}

static uint8_t decode44_pair(const uint8_t *buf, uint32_t len, uint32_t pos)
{
    uint8_t high;
    uint8_t low;

    high = buf[pos % len];
    low = buf[(pos + 1U) % len];
    return (uint8_t)((((uint32_t)high << 1) | 1U) & low);
}

static void analyze_nib_track(const uint8_t *buf,
                              uint32_t len,
                              uint8_t drive,
                              uint8_t track,
                              uint8_t qtrack,
                              disk2_track_scan_t *out)
{
    memset(out, 0, sizeof(*out));
    out->loaded = 1U;
    out->drive = (uint8_t)(drive + 1U);
    out->track = track;
    out->qtrack = qtrack;
    out->length = len;

    count_prologue(buf, len, 0xD5U, 0xAAU, 0x96U,
                   &out->addr16_count, &out->first_addr16);
    count_prologue(buf, len, 0xD5U, 0xAAU, 0xB5U,
                   &out->addr13_count, &out->first_addr13);
    count_prologue(buf, len, 0xD5U, 0xAAU, 0xADU,
                   &out->data_count, &out->first_data);

    if (out->first_addr16 != 0xFFFFFFFFU) {
        uint32_t p = out->first_addr16 + 3U;
        out->first_addr16_valid = 1U;
        out->first_addr16_volume = decode44_pair(buf, len, p);
        out->first_addr16_track = decode44_pair(buf, len, p + 2U);
        out->first_addr16_sector = decode44_pair(buf, len, p + 4U);
        out->first_addr16_checksum = decode44_pair(buf, len, p + 6U);
        out->first_addr16_checksum_ok =
            ((out->first_addr16_volume ^
              out->first_addr16_track ^
              out->first_addr16_sector) == out->first_addr16_checksum) ? 1U : 0U;
    }
}

static uint32_t woz_lss_decode_track(const uint8_t *raw,
                                     uint32_t bit_count,
                                     uint8_t *out,
                                     uint32_t out_capacity)
{
    uint8_t head_window = 0U;
    uint8_t shift = 0U;
    uint8_t latch_delay = 0U;
    uint32_t bit_cells;
    uint32_t out_len = 0U;

    if (raw == NULL || out == NULL || bit_count == 0U || out_capacity == 0U) {
        return 0U;
    }

    if (bit_count > (UINT32_MAX - DISK2_WOZ_SCAN_SEAM_BITS)) {
        bit_cells = bit_count;
    } else {
        bit_cells = bit_count + DISK2_WOZ_SCAN_SEAM_BITS;
    }

    for (uint32_t bit_pos = 0U; bit_pos < bit_cells; ++bit_pos) {
        uint8_t output_bit;

        head_window = (uint8_t)(((uint32_t)head_window << 1) |
                                woz_raw_bit_at(raw, bit_pos % bit_count));
        head_window &= 0x0FU;
        output_bit = (head_window != 0U) ?
            (uint8_t)((head_window >> 1) & 1U) : 0U;
        shift = (uint8_t)(((uint32_t)shift << 1) | output_bit);

        if (latch_delay != 0U) {
            latch_delay = (latch_delay > 4U) ? (uint8_t)(latch_delay - 4U) : 0U;
            if (shift == 0U) {
                latch_delay = (uint8_t)(latch_delay + 4U);
            }
        }

        if (latch_delay == 0U && (shift & 0x80U) != 0U) {
            if (out_len >= out_capacity) {
                break;
            }
            out[out_len++] = shift;
            latch_delay = 7U;
            shift = 0U;
        }
    }

    return out_len;
}

static void analyze_woz_track(const uint8_t *raw,
                              uint32_t bit_count,
                              uint8_t drive,
                              uint8_t track,
                              uint8_t qtrack,
                              disk2_track_scan_t *out)
{
    uint32_t decoded_len;

    decoded_len = woz_lss_decode_track(raw,
                                       bit_count,
                                       g_woz_decode_buf,
                                       sizeof(g_woz_decode_buf));
    analyze_nib_track(g_woz_decode_buf, decoded_len, drive, track, qtrack, out);
}

static int read_nib_physical_track(uint8_t drive, uint8_t track, uint8_t *buf)
{
    uint32_t offset;
    int rc = nib_track_offset(drive, track, &offset);

    if (rc != 0) {
        return rc;
    }
    return read_standard_image_bytes(drive, offset, buf, DISK2_NIB_TRACK_BYTES);
}

static int write_nib_physical_track(uint8_t drive, uint8_t track, const uint8_t *buf)
{
    uint32_t offset;
    int rc;

    if (g_disk2_info[drive].read_only != 0U) {
        return -1;
    }
    rc = nib_track_offset(drive, track, &offset);
    if (rc != 0) {
        return rc;
    }
    return write_standard_image_bytes(drive, offset, buf, DISK2_NIB_TRACK_BYTES);
}

static uint32_t fill_woz_empty_track(uint8_t *buf)
{
    uint32_t state = 1U;
    uint32_t i;

    for (i = 0U; i < DISK2_WOZ_EMPTY_TRACK_BYTES; ++i) {
        uint8_t value = 0U;
        uint8_t bit;

        for (bit = 0U; bit < 8U; ++bit) {
            state = (state * 214013U) + 2531011U;
            if (((state >> 16) & DISK2_APPLEWIN_RAND_MAX) < DISK2_APPLEWIN_RAND_3_10) {
                value |= (uint8_t)(1U << bit);
            }
        }
        buf[i] = value;
    }
    return DISK2_WOZ_EMPTY_TRACK_BYTES;
}

static uint8_t woz_raw_bit_at(const uint8_t *buf, uint32_t bit_offset)
{
    return (uint8_t)((buf[bit_offset >> 3] & (0x80U >> (bit_offset & 7U))) ? 1U : 0U);
}

static void woz_find_track_seam(const uint8_t *buf,
                                uint32_t bit_count,
                                uint32_t *start_out,
                                uint32_t *run_out)
{
    uint32_t bit_offset = 0U;
    uint8_t shift_reg = 0U;
    uint32_t zero_count = 0U;
    int32_t start_bit_offset = -1;
    int32_t nibble_start_bit_offset = -1;
    int32_t sync_ff_start_bit_offset = -1;
    uint32_t sync_ff_run_length = 0U;
    int32_t longest_sync_ff_start_bit_offset = -1;
    uint32_t longest_sync_ff_run_length = 0U;

    if (start_out == NULL || run_out == NULL) {
        return;
    }

    *start_out = 0U;
    *run_out = 0U;
    if (buf == NULL || bit_count == 0U) {
        return;
    }

    for (;;) {
        uint8_t output_bit = woz_raw_bit_at(buf, bit_offset);

        bit_offset++;
        if (bit_offset == bit_count) {
            bit_offset = 0U;
        }

        if ((start_bit_offset < 0 && bit_offset == 0U) ||
            (start_bit_offset >= 0 && bit_offset == (uint32_t)start_bit_offset)) {
            break;
        }

        if ((shift_reg & 0x80U) != 0U) {
            if (output_bit == 0U) {
                zero_count++;
                continue;
            }

            if (shift_reg == 0xFFU && zero_count == 2U) {
                if (sync_ff_start_bit_offset < 0) {
                    sync_ff_start_bit_offset = nibble_start_bit_offset;
                }
                sync_ff_run_length++;
            }

            if ((shift_reg != 0xFFU || zero_count != 2U) &&
                sync_ff_start_bit_offset >= 0) {
                if (start_bit_offset < 0) {
                    start_bit_offset = nibble_start_bit_offset;
                }
                if (longest_sync_ff_run_length < sync_ff_run_length) {
                    longest_sync_ff_start_bit_offset = sync_ff_start_bit_offset;
                    longest_sync_ff_run_length = sync_ff_run_length;
                }
                sync_ff_start_bit_offset = -1;
                sync_ff_run_length = 0U;
            }

            shift_reg = 0U;
            zero_count = 0U;
        }

        shift_reg = (uint8_t)((shift_reg << 1) | output_bit);
        if (shift_reg == 0x01U) {
            nibble_start_bit_offset =
                (bit_offset == 0U) ? (int32_t)(bit_count - 1U) : (int32_t)(bit_offset - 1U);
        }
    }

    if (longest_sync_ff_run_length != 0U &&
        longest_sync_ff_start_bit_offset >= 0) {
        *start_out = (uint32_t)longest_sync_ff_start_bit_offset;
        *run_out = longest_sync_ff_run_length;
    }
}

static uint8_t woz_qtrack_to_tmap_index(uint8_t qtrack)
{
    uint32_t index = (uint32_t)qtrack;

    if (index >= DISK2_WOZ_TMAP_SIZE) {
        index = DISK2_WOZ_TMAP_SIZE - 1U;
    }
    return (uint8_t)index;
}

static uint8_t woz_qtracks_share_image_track(const disk2_image_info_t *info,
                                             uint8_t left_qtrack,
                                             uint8_t right_qtrack)
{
    uint8_t left_index;
    uint8_t right_index;

    if (info == NULL || info->format != DISK2_IMAGE_WOZ) {
        return 0U;
    }

    left_index = woz_qtrack_to_tmap_index(left_qtrack);
    right_index = woz_qtrack_to_tmap_index(right_qtrack);
    return (info->woz_tmap[left_index] == info->woz_tmap[right_index]) ? 1U : 0U;
}

static uint8_t dirty_track_is_current(uint32_t track_info,
                                      uint8_t dirty_drive,
                                      uint8_t dirty_qtrack)
{
    const disk2_image_info_t *info;
    uint8_t current_qtrack;

    if (dirty_drive != requested_drive(track_info) ||
        (track_info & DISK2_TRACK_INFO_MATCH_BIT) == 0U) {
        return 0U;
    }

    current_qtrack = requested_qtrack(track_info);
    if (dirty_qtrack == current_qtrack) {
        return 1U;
    }

    info = &g_disk2_info[dirty_drive];
    return woz_qtracks_share_image_track(info, dirty_qtrack, current_qtrack);
}

static void publish_woz_alias_range(const disk2_image_info_t *info, uint8_t qtrack)
{
    uint8_t lo = qtrack;
    uint8_t hi = qtrack;
    uint8_t tmap_index;
    uint8_t trk_index;

    if (info != NULL && info->format == DISK2_IMAGE_WOZ) {
        tmap_index = woz_qtrack_to_tmap_index(qtrack);
        trk_index = info->woz_tmap[tmap_index];
        if (trk_index != DISK2_WOZ_TMAP_EMPTY) {
            uint8_t i;

            for (i = 0U; i < DISK2_WOZ_TMAP_SIZE; ++i) {
                if (info->woz_tmap[i] == trk_index) {
                    if (i < lo) {
                        lo = i;
                    }
                    if (i > hi) {
                        hi = i;
                    }
                }
            }
        }
    }

    disk2_reg_write(DISK2_REG_WOZ_ALIAS_RANGE,
                    ((uint32_t)hi << 8) | (uint32_t)lo);
}

static int woz_find_next_track_index(const disk2_image_info_t *info, uint8_t *index_out)
{
    uint8_t i;
    uint8_t next = 0U;

    if (info == NULL || index_out == NULL) {
        return -1;
    }

    for (i = 0U; i < DISK2_WOZ_TMAP_SIZE; ++i) {
        uint8_t mapped = info->woz_tmap[i];

        if (mapped != DISK2_WOZ_TMAP_EMPTY) {
            if (mapped >= DISK2_WOZ_TMAP_SIZE - 1U) {
                return -2;
            }
            if (mapped >= next) {
                next = (uint8_t)(mapped + 1U);
            }
        }
    }

    *index_out = next;
    return 0;
}

static int woz_write_header_crc(FIL *file, uint32_t file_size)
{
    uint32_t crc;
    uint8_t crc_bytes[4];
    int rc;

    rc = woz_calculate_crc(file, file_size, &crc);
    if (rc != 0) {
        return rc;
    }
    le32_store(crc_bytes, crc);
    return file_write_exact_at(file, 8U, crc_bytes, sizeof(crc_bytes));
}

static int woz_update_tmap(FIL *file,
                           disk2_image_info_t *info,
                           uint8_t tmap_index,
                           uint8_t trk_index)
{
    if (file == NULL || info == NULL || tmap_index >= DISK2_WOZ_TMAP_SIZE) {
        return -1;
    }

    info->woz_tmap[tmap_index] = trk_index;
    if (tmap_index > 0U) {
        info->woz_tmap[tmap_index - 1U] = trk_index;
    }
    if (tmap_index + 1U < DISK2_WOZ_TMAP_SIZE) {
        info->woz_tmap[tmap_index + 1U] = trk_index;
    }

    return file_write_exact_at(file,
                               info->woz_tmap_offset,
                               info->woz_tmap,
                               sizeof(info->woz_tmap));
}

static int write_woz1_track(FIL *file,
                            disk2_image_info_t *info,
                            uint8_t tmap_index,
                            uint8_t trk_index,
                            uint8_t existing,
                            const uint8_t *buf,
                            uint32_t length,
                            uint32_t *file_size_io)
{
    uint32_t offset = info->woz_trks_offset +
                      ((uint32_t)trk_index * DISK2_WOZ1_TRACK_BYTES);
    uint16_t bit_count;
    uint16_t bytes_used;
    int rc;

    if (length > DISK2_WOZ1_TRK_OFFSET) {
        return -2;
    }

    memset(g_scan_buf, 0, DISK2_WOZ1_TRACK_BYTES);
    if (existing != 0U) {
        if (offset + DISK2_WOZ1_TRACK_BYTES > *file_size_io ||
            (uint32_t)(trk_index + 1U) * DISK2_WOZ1_TRACK_BYTES >
            info->woz_trks_size) {
            return -2;
        }
        rc = file_read_exact_at(file, offset, g_scan_buf, DISK2_WOZ1_TRACK_BYTES);
        if (rc != 0) {
            return rc;
        }
        bytes_used = le16_load(&g_scan_buf[DISK2_WOZ1_TRK_OFFSET]);
        bit_count = le16_load(&g_scan_buf[DISK2_WOZ1_TRK_OFFSET + 2U]);
        if (bytes_used != length || bit_count == 0U) {
            return -2;
        }
    } else {
        if (offset != *file_size_io ||
            info->woz_trks_offset + info->woz_trks_size != *file_size_io) {
            return -2;
        }
        bit_count = (uint16_t)(length * 8U);
        rc = woz_update_tmap(file, info, tmap_index, trk_index);
        if (rc != 0) {
            return rc;
        }
        le32_store(g_file_io_buf, info->woz_trks_size + DISK2_WOZ1_TRACK_BYTES);
        rc = file_write_exact_at(file,
                                 info->woz_trks_offset - 4U,
                                 g_file_io_buf,
                                 4U);
        if (rc != 0) {
            return rc;
        }
        info->woz_trks_size += DISK2_WOZ1_TRACK_BYTES;
        *file_size_io += DISK2_WOZ1_TRACK_BYTES;
    }

    memcpy(g_scan_buf, buf, length);
    le16_store(&g_scan_buf[DISK2_WOZ1_TRK_OFFSET], (uint16_t)length);
    le16_store(&g_scan_buf[DISK2_WOZ1_TRK_OFFSET + 2U], bit_count);
    return file_write_exact_at(file, offset, g_scan_buf, DISK2_WOZ1_TRACK_BYTES);
}

static int write_woz2_track(FIL *file,
                            disk2_image_info_t *info,
                            uint8_t tmap_index,
                            uint8_t trk_index,
                            uint8_t existing,
                            const uint8_t *buf,
                            uint32_t length,
                            uint32_t *file_size_io)
{
    uint8_t trk[DISK2_WOZ_TRKV2_BYTES];
    uint32_t trk_offset = info->woz_trks_offset +
                          ((uint32_t)trk_index * DISK2_WOZ_TRKV2_BYTES);
    uint32_t bit_count;
    uint16_t start_block;
    uint16_t block_count;
    uint32_t data_offset;
    uint32_t write_length;
    int rc;

    if (trk_offset + DISK2_WOZ_TRKV2_BYTES >
        info->woz_trks_offset + info->woz_trks_size) {
        return -2;
    }

    if (existing != 0U) {
        rc = file_read_exact_at(file, trk_offset, trk, sizeof(trk));
        if (rc != 0) {
            return rc;
        }
        start_block = le16_load(&trk[0]);
        block_count = le16_load(&trk[2]);
        bit_count = le32_load(&trk[4]);
        if (start_block == 0U || block_count == 0U ||
            length > (uint32_t)block_count * DISK2_WOZ_BLOCK_BYTES) {
            return -2;
        }
        if (bit_count == 0U || ((bit_count + 7U) / 8U) != length) {
            return -2;
        }
        write_length = length;
    } else {
        uint32_t padded_length =
            (length + DISK2_WOZ_BLOCK_BYTES - 1U) &
            ~(DISK2_WOZ_BLOCK_BYTES - 1U);
        uint32_t aligned_file_size =
            (*file_size_io + DISK2_WOZ_BLOCK_BYTES - 1U) &
            ~(DISK2_WOZ_BLOCK_BYTES - 1U);
        uint32_t pad_pos;

        if (info->woz_trks_offset + info->woz_trks_size != *file_size_io ||
            padded_length > 0xFFFFU * DISK2_WOZ_BLOCK_BYTES ||
            aligned_file_size / DISK2_WOZ_BLOCK_BYTES > 0xFFFFU) {
            return -2;
        }

        memset(g_file_io_buf, 0, sizeof(g_file_io_buf));
        for (pad_pos = *file_size_io; pad_pos < aligned_file_size; ) {
            uint32_t chunk = aligned_file_size - pad_pos;

            if (chunk > sizeof(g_file_io_buf)) {
                chunk = sizeof(g_file_io_buf);
            }
            rc = file_write_exact_at(file, pad_pos, g_file_io_buf, (UINT)chunk);
            if (rc != 0) {
                return rc;
            }
            pad_pos += chunk;
        }

        start_block = (uint16_t)(aligned_file_size / DISK2_WOZ_BLOCK_BYTES);
        block_count = (uint16_t)(padded_length / DISK2_WOZ_BLOCK_BYTES);
        bit_count = length * 8U;
        write_length = padded_length;
        le16_store(&trk[0], start_block);
        le16_store(&trk[2], block_count);
        le32_store(&trk[4], bit_count);

        rc = file_write_exact_at(file, trk_offset, trk, sizeof(trk));
        if (rc != 0) {
            return rc;
        }
        rc = woz_update_tmap(file, info, tmap_index, trk_index);
        if (rc != 0) {
            return rc;
        }
        le32_store(g_file_io_buf,
                   info->woz_trks_size +
                   (aligned_file_size - *file_size_io) +
                   padded_length);
        rc = file_write_exact_at(file,
                                 info->woz_trks_offset - 4U,
                                 g_file_io_buf,
                                 4U);
        if (rc != 0) {
            return rc;
        }
        info->woz_trks_size += (aligned_file_size - *file_size_io) + padded_length;
        *file_size_io = aligned_file_size + padded_length;
    }

    data_offset = (uint32_t)start_block * DISK2_WOZ_BLOCK_BYTES;
    if (data_offset + length > *file_size_io && existing != 0U) {
        return -2;
    }
    if (write_length > length) {
        uint32_t pad_pos;

        memcpy(g_scan_buf, buf, length);
        memset(&g_scan_buf[length], 0, write_length - length);
        for (pad_pos = 0U; pad_pos < write_length; ) {
            uint32_t chunk = write_length - pad_pos;

            if (chunk > DISK2_IO_CHUNK_BYTES) {
                chunk = DISK2_IO_CHUNK_BYTES;
            }
            rc = file_write_exact_at(file,
                                     data_offset + pad_pos,
                                     &g_scan_buf[pad_pos],
                                     (UINT)chunk);
            if (rc != 0) {
                return rc;
            }
            pad_pos += chunk;
        }
        rc = 0;
    } else {
        rc = file_write_exact_at(file, data_offset, buf, (UINT)length);
    }
    if (rc == 0 && existing != 0U) {
        rc = file_write_exact_at(file, trk_offset, trk, sizeof(trk));
    }
    return rc;
}

static int write_woz_physical_qtrack(uint8_t drive,
                                     uint8_t qtrack,
                                     const uint8_t *buf,
                                     uint32_t length)
{
    FIL file;
    FRESULT fr;
    disk2_image_info_t *info;
    uint8_t tmap_index;
    uint8_t trk_index;
    uint8_t existing;
    uint32_t file_size;
    int rc;

    if (drive >= DISK2_DRIVE_COUNT || buf == NULL || length == 0U ||
        length > DISK2_TRACK_STREAM_BYTES) {
        return -1;
    }

    info = &g_disk2_info[drive];
    if (info->present == 0U || info->format != DISK2_IMAGE_WOZ ||
        info->read_only != 0U) {
        return -1;
    }

    tmap_index = woz_qtrack_to_tmap_index(qtrack);
    trk_index = info->woz_tmap[tmap_index];
    existing = (trk_index != DISK2_WOZ_TMAP_EMPTY) ? 1U : 0U;
    if (existing == 0U) {
        rc = woz_find_next_track_index(info, &trk_index);
        if (rc != 0) {
            return rc;
        }
    }
    if (trk_index >= DISK2_WOZ_TMAP_SIZE) {
        return -2;
    }

    fr = f_open(&file, g_disk2_paths[drive], FA_READ | FA_WRITE);
    if (fr != FR_OK) {
        return -(int)fr;
    }
    file_size = (uint32_t)f_size(&file);

    if (info->woz_version == 1U) {
        rc = write_woz1_track(&file, info, tmap_index, trk_index, existing,
                              buf, length, &file_size);
    } else if (info->woz_version == 2U) {
        rc = write_woz2_track(&file, info, tmap_index, trk_index, existing,
                              buf, length, &file_size);
    } else {
        rc = -2;
    }

    if (rc == 0) {
        file_size = (uint32_t)f_size(&file);
        rc = woz_write_header_crc(&file, file_size);
    }
    if (rc == 0) {
        FRESULT sync_fr = f_sync(&file);
        if (sync_fr != FR_OK) {
            rc = -(int)sync_fr;
        }
    }
    (void)f_close(&file);

    if (rc == 0) {
        info->file_size = file_size;
        info->logical_blocks = file_size / 512U;
        publish_drive(drive);
    }
    return rc;
}

static int read_woz1_track(uint8_t drive,
                           const disk2_image_info_t *info,
                           uint8_t trk_index,
                           uint8_t *buf,
                           uint32_t *length_out,
                           uint32_t *bit_count_out)
{
    uint32_t offset = info->woz_trks_offset +
                      ((uint32_t)trk_index * DISK2_WOZ1_TRACK_BYTES);
    uint32_t length;
    uint32_t bit_count;
    int rc;

    if ((uint32_t)(trk_index + 1U) * DISK2_WOZ1_TRACK_BYTES > info->woz_trks_size ||
        offset + DISK2_WOZ1_TRACK_BYTES > info->file_size) {
        return -2;
    }
    rc = read_drive_bytes(drive, offset, g_scan_buf, DISK2_WOZ1_TRACK_BYTES);
    if (rc != 0) {
        return rc;
    }

    length = le16_load(&g_scan_buf[DISK2_WOZ1_TRK_OFFSET]);
    if (length == 0U || length > DISK2_WOZ1_TRK_OFFSET) {
        length = DISK2_WOZ1_TRK_OFFSET;
    }
    bit_count = le16_load(&g_scan_buf[DISK2_WOZ1_TRK_OFFSET + 2U]);
    if (bit_count == 0U || bit_count > (length * 8U)) {
        bit_count = length * 8U;
    }
    if (length > DISK2_TRACK_STREAM_BYTES) {
        return -2;
    }
    memcpy(buf, g_scan_buf, length);
    *length_out = length;
    *bit_count_out = bit_count;
    return 0;
}

static int read_woz2_track(uint8_t drive,
                           const disk2_image_info_t *info,
                           uint8_t trk_index,
                           uint8_t *buf,
                           uint32_t *length_out,
                           uint32_t *bit_count_out)
{
    uint8_t trk[DISK2_WOZ_TRKV2_BYTES];
    uint32_t descriptor_offset;
    uint32_t data_offset;
    uint32_t length;
    uint32_t bit_count;
    int rc;

    descriptor_offset = info->woz_trks_offset +
                        ((uint32_t)trk_index * DISK2_WOZ_TRKV2_BYTES);
    if (((uint32_t)trk_index * DISK2_WOZ_TRKV2_BYTES) + DISK2_WOZ_TRKV2_BYTES >
        info->woz_trks_size) {
        return -2;
    }
    rc = read_drive_bytes(drive, descriptor_offset, trk, sizeof(trk));
    if (rc != 0) {
        return rc;
    }

    bit_count = le32_load(&trk[4]);
    length = (bit_count + 7U) / 8U;
    if (le16_load(&trk[0]) == 0U || le16_load(&trk[2]) == 0U ||
        bit_count == 0U || length == 0U) {
        *length_out = fill_woz_empty_track(buf);
        *bit_count_out = *length_out * 8U;
        return 0;
    }
    if (length > (uint32_t)le16_load(&trk[2]) * DISK2_WOZ_BLOCK_BYTES ||
        length > DISK2_TRACK_STREAM_BYTES) {
        return -2;
    }

    data_offset = (uint32_t)le16_load(&trk[0]) * DISK2_WOZ_BLOCK_BYTES;
    if (data_offset + length > info->file_size) {
        return -2;
    }
    rc = read_drive_bytes(drive, data_offset, g_scan_buf, length);
    if (rc != 0) {
        return rc;
    }
    memcpy(buf, g_scan_buf, length);
    *length_out = length;
    *bit_count_out = bit_count;
    return 0;
}

static int read_woz_qtrack_as_stream(uint8_t drive,
                                     uint8_t qtrack,
                                     uint8_t *buf,
                                     uint32_t *length_out,
                                     uint32_t *bit_count_out)
{
    uint8_t tmap_index;
    uint8_t trk_index;
    const disk2_image_info_t *info;

    if (drive >= DISK2_DRIVE_COUNT || buf == NULL || length_out == NULL ||
        bit_count_out == NULL) {
        return -1;
    }

    info = &g_disk2_info[drive];
    if (info->present == 0U || info->format != DISK2_IMAGE_WOZ) {
        return -1;
    }

    tmap_index = woz_qtrack_to_tmap_index(qtrack);
    trk_index = info->woz_tmap[tmap_index];
    if (trk_index == DISK2_WOZ_TMAP_EMPTY) {
        *length_out = fill_woz_empty_track(buf);
        *bit_count_out = *length_out * 8U;
        return 0;
    }
    if (trk_index >= DISK2_WOZ_TMAP_SIZE) {
        return -2;
    }

    if (info->woz_version == 1U) {
        return read_woz1_track(drive, info, trk_index, buf, length_out, bit_count_out);
    }
    if (info->woz_version == 2U) {
        return read_woz2_track(drive, info, trk_index, buf, length_out, bit_count_out);
    }
    return -2;
}

static int prepare_standard_track_stream(uint8_t drive,
                                         uint8_t track,
                                         uint8_t *buf,
                                         disk2_track_stream_t *stream)
{
    const disk2_image_info_t *info;
    int rc;

    if (drive >= DISK2_DRIVE_COUNT || buf == NULL || stream == NULL) {
        return -1;
    }

    memset(stream, 0, sizeof(*stream));
    stream->bit_timing = DISK2_WOZ_OPTIMAL_BIT_TIMING_5_25;

    info = &g_disk2_info[drive];
    if (info->format == DISK2_IMAGE_NIB) {
        rc = read_nib_physical_track(drive, track, buf);
        if (rc == 0) {
            stream->length = DISK2_NIB_TRACK_BYTES;
            stream->bit_count = stream->length * 8U;
        }
        return rc;
    }
    if (info->format == DISK2_IMAGE_DSK ||
        info->format == DISK2_IMAGE_DO ||
        info->format == DISK2_IMAGE_PO) {
        rc = read_sector_physical_track(drive, track, g_sector_track_buf);
        if (rc != 0) {
            return rc;
        }
        stream->length = nibblize_sector_track(buf,
                                               g_sector_track_buf,
                                               track,
                                               sector_image_is_prodos_order(info->format),
                                               info->volume_number);
        stream->bit_count = stream->length * 8U;
        return 0;
    }
    return -1;
}

static int prepare_woz_track_stream(uint8_t drive,
                                    uint8_t qtrack,
                                    uint8_t *buf,
                                    disk2_track_stream_t *stream)
{
    const disk2_image_info_t *info;
    int rc;

    if (drive >= DISK2_DRIVE_COUNT || buf == NULL || stream == NULL) {
        return -1;
    }

    memset(stream, 0, sizeof(*stream));
    info = &g_disk2_info[drive];
    if (info->format != DISK2_IMAGE_WOZ) {
        return -1;
    }

    rc = read_woz_qtrack_as_stream(drive, qtrack, buf,
                                   &stream->length,
                                   &stream->bit_count);
    if (rc != 0) {
        return rc;
    }

    /* load_track captures the live offset while the LSS is frozen, then
       scales it into this track's bit space. */
    stream->bit_timing = info->woz_optimal_bit_timing;
    stream->raw_bits = 1U;
    if (woz_qtrack_to_tmap_index(qtrack) >= 132U) {
        woz_find_track_seam(buf,
                            stream->bit_count,
                            &stream->seam_start,
                            &stream->seam_run);
    }
    return 0;
}

static int prepare_track_stream(uint8_t drive,
                                uint8_t qtrack,
                                uint8_t *buf,
                                disk2_track_stream_t *stream)
{
    const disk2_image_info_t *info;

    if (drive >= DISK2_DRIVE_COUNT) {
        return -1;
    }

    info = &g_disk2_info[drive];
    if (info->format == DISK2_IMAGE_WOZ) {
        return prepare_woz_track_stream(drive, qtrack, buf, stream);
    }
    return prepare_standard_track_stream(drive,
                                         qtrack_to_track(qtrack, info->track_count),
                                         buf,
                                         stream);
}

static int load_track(uint8_t drive, uint8_t qtrack)
{
    disk2_track_stream_t stream;
    uint32_t track_info;
    uint32_t commit_word;
    uint32_t restored_stream_pos = 0U;
    const disk2_image_info_t *info;
    int rc;

    if (drive >= DISK2_DRIVE_COUNT) {
        return -1;
    }

    info = &g_disk2_info[drive];
    if (info->present == 0U) {
        return -1;
    }

    track_info = disk2_reg_read(DISK2_REG_TRACK_INFO);
    if (info->format == DISK2_IMAGE_WOZ &&
        (track_info & DISK2_TRACK_INFO_LOADED_BIT) != 0U &&
        loaded_drive(track_info) == drive &&
        woz_qtracks_share_image_track(info, loaded_qtrack(track_info), qtrack) != 0U) {
        uint32_t bit_count;
        uint32_t bit_offset;

        if (track_request_matches(drive, qtrack) == 0U) {
            return DISK2_LOAD_STALE_REQUEST;
        }
        if (write_info_has_pending(disk2_reg_read(DISK2_REG_WRITE_INFO)) != 0U) {
            return DISK2_LOAD_STALE_REQUEST;
        }

        bit_count = disk2_reg_read(DISK2_REG_TRACK_BIT_COUNT);
        bit_offset = disk2_reg_read(DISK2_REG_TRACK_BIT_OFFSET);
        if (bit_count != 0U) {
            bit_offset += 7U;
            if (bit_offset >= bit_count) {
                bit_offset = 0U;
            }
            disk2_reg_write(DISK2_REG_TRACK_BIT_OFFSET, bit_offset);
        }

        commit_word = DISK2_TRACK_INFO_LOADED_BIT |
                      DISK2_TRACK_INFO_RAW_BITS_BIT |
                      ((uint32_t)qtrack << DISK2_TRACK_INFO_LOAD_QTRACK_SHIFT) |
                      ((uint32_t)drive << DISK2_TRACK_INFO_LOAD_DRIVE_SHIFT);
        publish_woz_alias_range(info, qtrack);
        disk2_reg_write(DISK2_REG_TRACK_INFO, commit_word);
        g_loaded_drive = drive;
        g_loaded_qtrack = qtrack;
        g_loaded_track_length = disk2_reg_read(DISK2_REG_TRACK_LENGTH);
        g_loaded_track_bit_count = bit_count;
        g_drive_rotation_valid[drive] = 1U;
        g_drive_rotation_qtrack[drive] = qtrack;
        g_drive_rotation_raw_bits[drive] = 1U;
        g_drive_rotation_bit_count[drive] = bit_count;
        g_load_fail_count = 0U;
        return 0;
    }

    if (info->format != DISK2_IMAGE_WOZ &&
        qtrack_to_track(qtrack, info->track_count) ==
            DISK2_NO_PHYSICAL_TRACK) {
        if (track_request_matches(drive, qtrack) == 0U ||
            write_info_has_pending(disk2_reg_read(DISK2_REG_WRITE_INFO)) != 0U) {
            return DISK2_LOAD_STALE_REQUEST;
        }
        disk2_reg_write(DISK2_REG_TRACK_INFO, 0U);
        disk2_reg_write(DISK2_REG_TRACK_LENGTH, 0U);
        disk2_reg_write(DISK2_REG_TRACK_BIT_COUNT, 0U);
        commit_word = DISK2_TRACK_INFO_LOADED_BIT |
                      DISK2_TRACK_INFO_NO_TRACK_BIT |
                      ((uint32_t)qtrack << DISK2_TRACK_INFO_LOAD_QTRACK_SHIFT) |
                      ((uint32_t)drive << DISK2_TRACK_INFO_LOAD_DRIVE_SHIFT);
        disk2_reg_write(DISK2_REG_TRACK_INFO, commit_word);
        g_loaded_drive = drive;
        g_loaded_qtrack = qtrack;
        g_loaded_track_length = 0U;
        g_loaded_track_bit_count = 0U;
        g_load_fail_count = 0U;
        return 0;
    }

    memset(g_track_buf, 0xFF, sizeof(g_track_buf));
    rc = prepare_track_stream(drive, qtrack, g_track_buf, &stream);
    if (rc != 0) {
        return rc;
    }

    if (track_request_matches(drive, qtrack) == 0U) {
        return DISK2_LOAD_STALE_REQUEST;
    }

    /* A write may land while the replacement track is being read from SD.
       Defer staging until the resident dirty track has been flushed; its
       dirty metadata must never be paired with the replacement bytes. */
    if (write_info_has_pending(disk2_reg_read(DISK2_REG_WRITE_INFO)) != 0U) {
        return DISK2_LOAD_STALE_REQUEST;
    }

    /* Freeze the LSS before taking the selected drive's saved position. The
       PL keeps independent rotation registers for both drives even though
       only one track image occupies DDR at a time. */
    disk2_reg_write(DISK2_REG_TRACK_INFO, 0U);
    if (info->format == DISK2_IMAGE_WOZ) {
        uint32_t old_bit_offset = disk2_reg_read(DISK2_REG_TRACK_BIT_OFFSET);

        if (g_drive_rotation_valid[drive] != 0U &&
            g_drive_rotation_raw_bits[drive] != 0U &&
            g_drive_rotation_qtrack[drive] == qtrack) {
            stream.bit_offset = (stream.bit_count != 0U) ?
                (old_bit_offset % stream.bit_count) : 0U;
        } else {
            stream.bit_offset = woz_track_switch_bit_offset(
                old_bit_offset,
                g_drive_rotation_bit_count[drive],
                stream.bit_count);
        }
    } else if (g_drive_rotation_valid[drive] != 0U &&
               g_drive_rotation_raw_bits[drive] == 0U &&
               g_drive_rotation_qtrack[drive] == qtrack &&
               stream.length != 0U) {
        /* AppleWin keeps m_byte per drive, but resets it when that drive
         * actually loads a different standard track. Returning to the same
         * resident qtrack after selecting the other drive restores it. */
        restored_stream_pos =
            disk2_reg_read(DISK2_REG_STREAM_POS) % stream.length;
    }

    rc = stage_track_to_ddr(stream.length);
    if (rc != 0) {
        return rc;
    }

    if (track_request_matches(drive, qtrack) == 0U) {
        return DISK2_LOAD_STALE_REQUEST;
    }

    disk2_reg_write(DISK2_REG_TRACK_LENGTH, stream.length);
    disk2_reg_write(DISK2_REG_TRACK_BIT_COUNT, stream.bit_count);
    disk2_reg_write(DISK2_REG_TRACK_BIT_OFFSET, stream.bit_offset);
    disk2_reg_write(DISK2_REG_TRACK_BIT_TIMING, stream.bit_timing);
    disk2_reg_write(DISK2_REG_TRACK_SEAM,
                    ((stream.seam_run & 0xFFFFU) << 16) |
                    (stream.seam_start & 0xFFFFU));
    disk2_reg_write(DISK2_REG_TRACK_INDEX, 0U);
    disk2_reg_write(DISK2_REG_STREAM_POS, restored_stream_pos);
    if (stream.raw_bits != 0U) {
        publish_woz_alias_range(info, qtrack);
    } else {
        publish_woz_alias_range(NULL, qtrack);
    }

    commit_word = DISK2_TRACK_INFO_LOADED_BIT |
                  (stream.raw_bits ? DISK2_TRACK_INFO_RAW_BITS_BIT : 0U) |
                  ((uint32_t)qtrack << DISK2_TRACK_INFO_LOAD_QTRACK_SHIFT) |
                  ((uint32_t)drive << DISK2_TRACK_INFO_LOAD_DRIVE_SHIFT);
    disk2_reg_write(DISK2_REG_TRACK_INFO, commit_word);

    g_loaded_drive = drive;
    g_loaded_qtrack = qtrack;
    g_loaded_track_length = stream.length;
    g_loaded_track_bit_count = stream.bit_count;
    g_drive_rotation_valid[drive] = 1U;
    g_drive_rotation_qtrack[drive] = qtrack;
    g_drive_rotation_raw_bits[drive] = stream.raw_bits;
    g_drive_rotation_bit_count[drive] = stream.bit_count;
    g_load_fail_count = 0U;
    return 0;
}

static int read_loaded_track_ddr(uint8_t *buf, uint32_t length)
{
    if (buf == NULL) {
        return -1;
    }

    if (length > DISK2_TRACK_STREAM_BYTES) {
        length = DISK2_TRACK_STREAM_BYTES;
    }
    Xil_DCacheInvalidateRange((UINTPTR)buf, DISK2_TRACK_STREAM_BYTES);
    memcpy(buf, (const void *)(uintptr_t)DISK2_DDR_STAGING_BASE, length);
    return 0;
}

static uint32_t buffer_crc32(const uint8_t *buf, uint32_t length)
{
    return crc32_finish(crc32_update(crc32_init(), buf, length));
}

static uint32_t first_buffer_diff(const uint8_t *left,
                                  uint32_t left_len,
                                  const uint8_t *right,
                                  uint32_t right_len)
{
    uint32_t min_len = (left_len < right_len) ? left_len : right_len;
    uint32_t i;

    for (i = 0U; i < min_len; ++i) {
        if (left[i] != right[i]) {
            return i;
        }
    }
    return (left_len == right_len) ? 0xFFFFFFFFU : min_len;
}

static void log_woz_flush(uint8_t drive,
                          uint8_t qtrack,
                          uint32_t length,
                          uint32_t bit_count,
                          uint32_t snap_write_count,
                          uint32_t post_write_count,
                          uint32_t post_write_info,
                          int rc)
{
    if (g_uart_base == 0U || g_woz_write_enable[drive] == 0U) {
        return;
    }

    ++g_woz_flush_log_count;
    uart_puts(g_uart_base, "Disk II WOZ flush #");
    uart_putdec(g_uart_base, g_woz_flush_log_count);
    uart_puts(g_uart_base, " d");
    uart_putdec(g_uart_base, (uint32_t)drive + 1U);
    uart_puts(g_uart_base, " q");
    uart_putdec(g_uart_base, qtrack);
    uart_puts(g_uart_base, " len=");
    uart_putdec(g_uart_base, length);
    uart_puts(g_uart_base, " bits=");
    uart_putdec(g_uart_base, bit_count);
    uart_puts(g_uart_base, " wc=");
    uart_putdec(g_uart_base, snap_write_count);
    uart_puts(g_uart_base, "/");
    uart_putdec(g_uart_base, post_write_count);
    uart_puts(g_uart_base, " wi=0x");
    uart_puthex(g_uart_base, post_write_info);
    uart_puts(g_uart_base, " ti=0x");
    uart_puthex(g_uart_base, disk2_reg_read(DISK2_REG_TRACK_INFO));
    uart_puts(g_uart_base, " rc=");
    if (rc < 0) {
        uart_puts(g_uart_base, "-");
        uart_putdec(g_uart_base, (uint32_t)(-rc));
    } else {
        uart_putdec(g_uart_base, (uint32_t)rc);
    }
    uart_puts(g_uart_base, "\r\n");
}

static void log_woz_verify(uint8_t drive,
                           uint8_t qtrack,
                           uint32_t write_len,
                           uint32_t write_bits,
                           uint32_t verify_len,
                           uint32_t verify_bits,
                           uint32_t diff,
                           uint32_t write_crc,
                           uint32_t verify_crc,
                           int verify_rc)
{
    if (g_uart_base == 0U || g_woz_write_enable[drive] == 0U) {
        return;
    }

    uart_puts(g_uart_base, "Disk II WOZ verify d");
    uart_putdec(g_uart_base, (uint32_t)drive + 1U);
    uart_puts(g_uart_base, " q");
    uart_putdec(g_uart_base, qtrack);
    uart_puts(g_uart_base, " rc=");
    if (verify_rc < 0) {
        uart_puts(g_uart_base, "-");
        uart_putdec(g_uart_base, (uint32_t)(-verify_rc));
    } else {
        uart_putdec(g_uart_base, (uint32_t)verify_rc);
    }
    uart_puts(g_uart_base, " len=");
    uart_putdec(g_uart_base, write_len);
    uart_puts(g_uart_base, "/");
    uart_putdec(g_uart_base, verify_len);
    uart_puts(g_uart_base, " bits=");
    uart_putdec(g_uart_base, write_bits);
    uart_puts(g_uart_base, "/");
    uart_putdec(g_uart_base, verify_bits);
    uart_puts(g_uart_base, " crc=0x");
    uart_puthex(g_uart_base, write_crc);
    uart_puts(g_uart_base, "/0x");
    uart_puthex(g_uart_base, verify_crc);
    uart_puts(g_uart_base, " diff=");
    if (diff == 0xFFFFFFFFU) {
        uart_puts(g_uart_base, "none");
    } else {
        uart_putdec(g_uart_base, diff);
    }
    uart_puts(g_uart_base, "\r\n");
}

static void log_woz_structure(uint8_t drive,
                              uint8_t qtrack,
                              const disk2_track_scan_t *before,
                              const disk2_track_scan_t *after,
                              int rc)
{
    if (g_uart_base == 0U || g_woz_write_enable[drive] == 0U ||
        before == NULL || after == NULL) {
        return;
    }

    uart_puts(g_uart_base, "Disk II WOZ structure d");
    uart_putdec(g_uart_base, (uint32_t)drive + 1U);
    uart_puts(g_uart_base, " q");
    uart_putdec(g_uart_base, qtrack);
    uart_puts(g_uart_base, " old addr=");
    uart_putdec(g_uart_base, before->addr16_count);
    uart_puts(g_uart_base, " data=");
    uart_putdec(g_uart_base, before->data_count);
    uart_puts(g_uart_base, " new addr=");
    uart_putdec(g_uart_base, after->addr16_count);
    uart_puts(g_uart_base, " data=");
    uart_putdec(g_uart_base, after->data_count);
    uart_puts(g_uart_base, " rc=");
    if (rc < 0) {
        uart_puts(g_uart_base, "-");
        uart_putdec(g_uart_base, (uint32_t)(-rc));
    } else {
        uart_putdec(g_uart_base, (uint32_t)rc);
    }
    uart_puts(g_uart_base, "\r\n");
}

static void disable_woz_write_after_reject(uint8_t drive, uint8_t qtrack)
{
    if (drive >= DISK2_DRIVE_COUNT) {
        return;
    }
    if (g_disk2_info[drive].format == DISK2_IMAGE_WOZ) {
        g_woz_write_enable[drive] = 0U;
        g_disk2_info[drive].read_only = 1U;
        publish_drive(drive);
    }
    if (g_uart_base != 0U) {
        uart_puts(g_uart_base, "Disk II WOZ write disabled d");
        uart_putdec(g_uart_base, (uint32_t)drive + 1U);
        uart_puts(g_uart_base, " q");
        uart_putdec(g_uart_base, qtrack);
        uart_puts(g_uart_base, " after structural reject\r\n");
    }
}

static int flush_dirty_track(uint8_t drive, uint8_t qtrack)
{
    uint8_t track;
    uint32_t length = disk2_reg_read(DISK2_REG_TRACK_LENGTH);
    uint32_t bit_count;
    uint32_t write_crc = 0U;
    uint32_t verify_crc = 0U;
    uint32_t verify_length = 0U;
    uint32_t verify_bit_count = 0U;
    uint32_t verify_diff = 0xFFFFFFFFU;
    uint32_t snap_write_count;
    uint32_t post_write_info;
    uint32_t post_write_count;
    const disk2_image_info_t *info;
    int rc;
    int verify_rc = -1;
    disk2_track_scan_t woz_before_scan;
    disk2_track_scan_t woz_after_scan;

    if (drive >= DISK2_DRIVE_COUNT || length == 0U) {
        return -1;
    }

    info = &g_disk2_info[drive];
    if (info->present == 0U || info->read_only != 0U) {
        return -1;
    }

    track = qtrack_to_track(qtrack, info->track_count);
    if (info->format != DISK2_IMAGE_WOZ &&
        track == DISK2_NO_PHYSICAL_TRACK) {
        /* There is no backing track beyond the end of a standard image. */
        return 0;
    }
    bit_count = disk2_reg_read(DISK2_REG_TRACK_BIT_COUNT);

    /* Snapshot stream_write_count before copying the DDR staging region.
       Recheck busy + write_count afterward: if either has advanced, a new
       PL bit-write landed during our window and the bytes we just captured
       are already stale. Aborting leaves dirty=1 set so the next poll
       cycle will retry with current staging contents instead of writing
       a stale snapshot to disk. */
    snap_write_count = disk2_reg_read(DISK2_REG_WRITE_COUNT);
    if (read_loaded_track_ddr(g_track_buf, length) != 0) {
        return -1;
    }
    post_write_info = disk2_reg_read(DISK2_REG_WRITE_INFO);
    post_write_count = disk2_reg_read(DISK2_REG_WRITE_COUNT);
    if ((post_write_info & DISK2_WRITE_INFO_BUSY_BIT) != 0U ||
        post_write_count != snap_write_count) {
        return -1;
    }
    if (info->format == DISK2_IMAGE_WOZ && g_woz_write_enable[drive] != 0U) {
        write_crc = buffer_crc32(g_track_buf, length);
        verify_rc = read_woz_qtrack_as_stream(drive, qtrack, g_scan_buf,
                                              &verify_length,
                                              &verify_bit_count);
        if (verify_rc == 0) {
            analyze_woz_track(g_scan_buf, verify_bit_count, drive, track,
                              qtrack, &woz_before_scan);
            analyze_woz_track(g_track_buf, bit_count, drive, track, qtrack,
                              &woz_after_scan);
            if ((woz_before_scan.addr16_count != 0U &&
                 woz_after_scan.addr16_count < woz_before_scan.addr16_count) ||
                (woz_before_scan.data_count != 0U &&
                 woz_after_scan.data_count < woz_before_scan.data_count)) {
                log_woz_structure(drive, qtrack, &woz_before_scan,
                                  &woz_after_scan,
                                  DISK2_WOZ_STRUCTURE_REJECT);
                return DISK2_WOZ_STRUCTURE_REJECT;
            }
            log_woz_structure(drive, qtrack, &woz_before_scan,
                              &woz_after_scan, 0);
        }
    }

    if (info->format == DISK2_IMAGE_NIB) {
        if (length < DISK2_NIB_TRACK_BYTES) {
            return -2;
        }
        rc = write_nib_physical_track(drive, track, g_track_buf);
    } else if (info->format == DISK2_IMAGE_WOZ) {
        rc = write_woz_physical_qtrack(drive, qtrack, g_track_buf, length);
    } else if (info->format == DISK2_IMAGE_DSK ||
               info->format == DISK2_IMAGE_DO ||
               info->format == DISK2_IMAGE_PO) {
        rc = denibblize_sector_track(g_track_buf, length, g_sector_track_buf,
                                     sector_image_is_prodos_order(info->format),
                                     track);
        if (rc != 0) {
            return rc;
        }
        rc = write_sector_physical_track(drive, track, g_sector_track_buf);
    } else {
        rc = -1;
    }

    if (info->format == DISK2_IMAGE_WOZ) {
        if (rc == 0 && g_woz_write_enable[drive] != 0U) {
            verify_rc = read_woz_qtrack_as_stream(drive, qtrack, g_scan_buf,
                                                  &verify_length,
                                                  &verify_bit_count);
            if (verify_rc == 0) {
                verify_crc = buffer_crc32(g_scan_buf, verify_length);
                verify_diff = first_buffer_diff(g_track_buf, length,
                                                g_scan_buf, verify_length);
            }
            log_woz_verify(drive, qtrack, length, bit_count,
                           verify_length, verify_bit_count, verify_diff,
                           write_crc, verify_crc, verify_rc);
        }
        log_woz_flush(drive, qtrack, length, bit_count, snap_write_count,
                      post_write_count, post_write_info, rc);
    }

    if (rc == 0 && g_loaded_drive == drive && g_loaded_qtrack == qtrack) {
        g_loaded_track_length = length;
        g_loaded_track_bit_count = disk2_reg_read(DISK2_REG_TRACK_BIT_COUNT);
    }
    return rc;
}

static int flush_drive_before_media_change(uint8_t drive)
{
    /* The PL write FIFO normally drains in only a few polls. Bound the wait
     * so a broken memory port cannot hang the configuration UI forever. */
    for (uint32_t attempt = 0U; attempt < 65536U; ++attempt) {
        uint32_t write_info = disk2_reg_read(DISK2_REG_WRITE_INFO);
        uint8_t dirty_drive;
        uint8_t dirty_qtrack;
        int rc;

        if ((write_info & DISK2_WRITE_INFO_BUSY_BIT) != 0U) {
            continue;
        }
        if ((write_info & DISK2_WRITE_INFO_DIRTY_BIT) == 0U) {
            return 0;
        }
        dirty_drive = write_info_drive(write_info);
        if (dirty_drive != drive) {
            return 0;
        }
        dirty_qtrack = write_info_qtrack(write_info);
        rc = flush_dirty_track(dirty_drive, dirty_qtrack);
        if (rc == DISK2_WOZ_STRUCTURE_REJECT) {
            ack_dirty_track(dirty_drive, dirty_qtrack);
            disable_woz_write_after_reject(dirty_drive, dirty_qtrack);
            return 0;
        }
        if (rc != 0) {
            return rc;
        }
        ack_dirty_track(dirty_drive, dirty_qtrack);
    }
    return -4;
}

int disk2_service_init(uint32_t uart_base)
{
    g_uart_base = uart_base;
    clear_drive_state(0U);
    clear_drive_state(1U);
    g_disk2_paths[0][0] = '\0';
    g_disk2_paths[1][0] = '\0';
    disk2_reg_write(DISK2_REG_TRACK_INFO, 0U);
    disk2_reg_write(DISK2_REG_TRACK_LENGTH, 0U);
    disk2_reg_write(DISK2_REG_TRACK_BIT_COUNT, 0U);
    disk2_reg_write(DISK2_REG_TRACK_BIT_OFFSET, 0U);
    disk2_reg_write(DISK2_REG_TRACK_SEAM, 0U);
    disk2_reg_write(DISK2_REG_TRACK_INDEX, 0U);
    disk2_reg_write(DISK2_REG_STREAM_POS, 0U);
    return 0;
}

void disk2_service_set_enabled(uint8_t enabled)
{
    g_disk2_enabled = (enabled != 0U) ? 1U : 0U;
    if (g_disk2_enabled == 0U) {
        g_load_fail_count = 0U;
    }
}

/* Log genuine dirty-flush failures at a bounded rate. Motor-on deferrals are
 * expected and remain quiet. */
static uint32_t g_flush_fail_count;

int disk2_service_flush_dirty_now(void)
{
    const uint32_t write_info = disk2_reg_read(DISK2_REG_WRITE_INFO);
    uint8_t drive;
    uint8_t qtrack;
    int rc;

    if ((write_info & DISK2_WRITE_INFO_DIRTY_BIT) == 0U) {
        return 0;
    }
    if ((write_info & DISK2_WRITE_INFO_BUSY_BIT) != 0U) {
        return -1;
    }
    drive = write_info_drive(write_info);
    qtrack = write_info_qtrack(write_info);
    rc = flush_dirty_track(drive, qtrack);
    if (rc == DISK2_WOZ_STRUCTURE_REJECT) {
        ack_dirty_track(drive, qtrack);
        disable_woz_write_after_reject(drive, qtrack);
        return 0;
    }
    if (rc != 0) {
        return rc;
    }
    ack_dirty_track(drive, qtrack);
    return 0;
}

void disk2_service_poll(void)
{
    uint32_t track_info = disk2_reg_read(DISK2_REG_TRACK_INFO);
    uint32_t write_info = disk2_reg_read(DISK2_REG_WRITE_INFO);
    uint32_t status;
    uint8_t drive = requested_drive(track_info);
    uint8_t qtrack = requested_qtrack(track_info);
    int rc;

    if ((write_info & DISK2_WRITE_INFO_DIRTY_BIT) != 0U) {
        uint8_t dirty_drive =
            (uint8_t)((write_info >> DISK2_WRITE_INFO_DRIVE_SHIFT) & 0x01U);
        uint8_t dirty_qtrack =
            (uint8_t)((write_info >> DISK2_WRITE_INFO_QTRACK_SHIFT) & 0xFFU);
        uint8_t dirty_is_current =
            dirty_track_is_current(track_info, dirty_drive, dirty_qtrack);

        if ((write_info & DISK2_WRITE_INFO_BUSY_BIT) != 0U) {
            return;
        }

        status = disk2_reg_read(0U);
        if (dirty_is_current != 0U && ((status >> 11) & 1U) != 0U) {
            return;
        }
        rc = flush_dirty_track(dirty_drive, dirty_qtrack);
        if (rc == DISK2_WOZ_STRUCTURE_REJECT) {
            ack_dirty_track(dirty_drive, dirty_qtrack);
            disable_woz_write_after_reject(dirty_drive, dirty_qtrack);
            return;
        }
        if (rc != 0) {
            if (g_flush_fail_count == 0U ||
                (g_flush_fail_count % 256U) == 0U) {
                char msg[96];
                (void)snprintf(msg, sizeof(msg),
                    "disk2: dirty flush FAILED rc=%d d%u qtrack=%u "
                    "(fail #%lu)\r\n",
                    rc, (unsigned)(dirty_drive + 1U), (unsigned)dirty_qtrack,
                    (unsigned long)(g_flush_fail_count + 1U));
                uart_puts(g_uart_base, msg);
            }
            g_flush_fail_count++;
            return;
        }
        g_flush_fail_count = 0U;
        ack_dirty_track(dirty_drive, dirty_qtrack);
    }

    /* A pending dirty track is still flushed above when Slot 6 is disabled.
     * New track-load requests are not meaningful until the card is enabled. */
    if (g_disk2_enabled == 0U) {
        g_load_fail_count = 0U;
        return;
    }
    if ((track_info & DISK2_TRACK_INFO_MATCH_BIT) != 0U) {
        return;
    }
    if (drive >= DISK2_DRIVE_COUNT) {
        return;
    }
    /* An unmatched request for an empty drive is normal. Leave it pending so
     * the PL continues to present no media, but do not report it as an I/O
     * failure on every retry. */
    if (g_disk2_info[drive].present == 0U) {
        g_load_fail_count = 0U;
        return;
    }
    rc = load_track(drive, qtrack);
    if (rc == DISK2_LOAD_STALE_REQUEST) {
        return;
    }
    if (rc != 0) {
        /* Track-load failures are retryable because the 6502 keeps the
         * request pending. Report them at a bounded rate while later polls
         * recover from transient SD I/O failures. */
        if (g_load_fail_count == 0U ||
            (g_load_fail_count % 64U) == 0U) {
            char msg[96];
            (void)snprintf(msg, sizeof(msg),
                "disk2: track load FAILED rc=%d d%u qtrack=%u (fail #%lu)\r\n",
                rc, (unsigned)(drive + 1U), (unsigned)qtrack,
                (unsigned long)(g_load_fail_count + 1U));
            uart_puts(g_uart_base, msg);
        }
        g_load_fail_count++;
    } else if (g_load_fail_count != 0U) {
        char msg[80];
        (void)snprintf(msg, sizeof(msg),
            "disk2: track load recovered after %lu failure(s)\r\n",
            (unsigned long)g_load_fail_count);
        uart_puts(g_uart_base, msg);
        g_load_fail_count = 0U;
    }
}

int disk2_service_set_image_path(uint8_t drive, const char *path)
{
    char new_path[DISK2_IMAGE_PATH_MAX];
    uint8_t other;
    int rc;

    if (drive >= DISK2_DRIVE_COUNT) {
        return -1;
    }
    copy_path(new_path, sizeof(new_path), path);
    other = (uint8_t)(drive ^ 1U);
    if (new_path[0] != '\0' &&
        g_disk2_info[other].present != 0U &&
        path_ieq(new_path, g_disk2_paths[other]) != 0U) {
        /* Never open one writable image through two independent caches. */
        return -3;
    }
    rc = flush_drive_before_media_change(drive);
    if (rc != 0) {
        return rc;
    }
    copy_path(g_disk2_paths[drive], sizeof(g_disk2_paths[drive]), new_path);
    return probe_file(drive);
}

const char *disk2_service_get_image_path(uint8_t drive)
{
    if (drive >= DISK2_DRIVE_COUNT) {
        return "";
    }
    return g_disk2_paths[drive];
}

int disk2_service_reset_media(uint8_t drive)
{
    int rc;

    if (drive >= DISK2_DRIVE_COUNT) {
        return -1;
    }
    rc = flush_drive_before_media_change(drive);
    if (rc != 0) {
        return rc;
    }
    return probe_file(drive);
}

int disk2_service_get_image_info(uint8_t drive, disk2_image_info_t *out)
{
    if (drive >= DISK2_DRIVE_COUNT || out == NULL) {
        return -1;
    }
    *out = g_disk2_info[drive];
    return 0;
}

int disk2_service_set_woz_write_enable(uint8_t drive, uint8_t enable)
{
    FIL file;
    FRESULT fr;
    uint32_t write_info;
    uint8_t dirty_drive;
    uint8_t dirty_qtrack;
    int rc;

    if (drive >= DISK2_DRIVE_COUNT) {
        return -1;
    }
    if (g_disk2_info[drive].present == 0U ||
        g_disk2_info[drive].format != DISK2_IMAGE_WOZ) {
        return -2;
    }

    write_info = disk2_reg_read(DISK2_REG_WRITE_INFO);
    dirty_drive = write_info_drive(write_info);
    dirty_qtrack = write_info_qtrack(write_info);

    if (enable == 0U) {
        if ((write_info & DISK2_WRITE_INFO_DIRTY_BIT) != 0U &&
            dirty_drive == drive) {
            if ((write_info & DISK2_WRITE_INFO_BUSY_BIT) != 0U) {
                return -4;
            }
            rc = flush_dirty_track(dirty_drive, dirty_qtrack);
            if (rc == DISK2_WOZ_STRUCTURE_REJECT) {
                ack_dirty_track(dirty_drive, dirty_qtrack);
                disable_woz_write_after_reject(dirty_drive, dirty_qtrack);
                return 0;
            }
            if (rc != 0) {
                return -5;
            }
            ack_dirty_track(dirty_drive, dirty_qtrack);
        }
        g_woz_write_enable[drive] = 0U;
        g_disk2_info[drive].read_only = 1U;
        publish_drive(drive);
        return 0;
    }

    if (g_woz_image_write_protected[drive] != 0U) {
        return -3;
    }
    if ((write_info & DISK2_WRITE_INFO_DIRTY_BIT) != 0U &&
        dirty_drive == drive &&
        g_woz_write_enable[drive] == 0U &&
        g_disk2_info[drive].read_only != 0U &&
        (write_info & DISK2_WRITE_INFO_BUSY_BIT) == 0U) {
        ack_dirty_track(dirty_drive, dirty_qtrack);
        write_info = 0U;
    }
    if ((write_info & DISK2_WRITE_INFO_DIRTY_BIT) != 0U) {
        return -4;
    }

    fr = f_open(&file, g_disk2_paths[drive], FA_READ | FA_WRITE);
    if (fr != FR_OK) {
        return -(int)fr;
    }
    (void)f_close(&file);

    g_woz_write_enable[drive] = 1U;
    g_disk2_info[drive].read_only = 0U;
    publish_drive(drive);
    return 0;
}

uint8_t disk2_service_get_woz_write_enable(uint8_t drive)
{
    if (drive >= DISK2_DRIVE_COUNT) {
        return 0U;
    }
    return g_woz_write_enable[drive];
}

int disk2_service_scan_loaded_track(disk2_track_scan_t *out)
{
    if (out == NULL) {
        return -1;
    }

    memset(out, 0, sizeof(*out));
    if (g_loaded_drive >= DISK2_DRIVE_COUNT ||
        g_loaded_qtrack == DISK2_NO_LOADED_TRACK ||
        g_loaded_track_length == 0U) {
        return -1;
    }

    analyze_nib_track(
        g_track_buf,
        g_loaded_track_length,
        g_loaded_drive,
        (g_disk2_info[g_loaded_drive].format == DISK2_IMAGE_WOZ) ?
            (uint8_t)(g_loaded_qtrack / 4U) :
            qtrack_to_track(g_loaded_qtrack, g_disk2_info[g_loaded_drive].track_count),
        g_loaded_qtrack,
        out);
    return 0;
}

int disk2_service_scan_file_track(uint8_t drive, uint8_t track, disk2_track_scan_t *out)
{
    disk2_track_stream_t stream;
    int rc;

    if (out == NULL) {
        return -1;
    }
    rc = prepare_track_stream(drive, (uint8_t)(track * 4U), g_scan_buf, &stream);
    if (rc != 0) {
        memset(out, 0, sizeof(*out));
        return rc;
    }

    analyze_nib_track(g_scan_buf, stream.length, drive, track,
                      (uint8_t)(track * 4U), out);
    return 0;
}

int disk2_service_wozscan_loaded_track(disk2_track_scan_t *out)
{
    uint32_t track_info;
    uint32_t length;
    uint32_t bit_count;
    uint8_t drive;
    uint8_t qtrack;

    if (out == NULL) {
        return -1;
    }

    memset(out, 0, sizeof(*out));
    track_info = disk2_reg_read(DISK2_REG_TRACK_INFO);
    if ((track_info & DISK2_TRACK_INFO_LOADED_BIT) == 0U ||
        (track_info & DISK2_TRACK_INFO_RAW_BITS_BIT) == 0U) {
        return -1;
    }

    drive = loaded_drive(track_info);
    qtrack = loaded_qtrack(track_info);
    if (drive >= DISK2_DRIVE_COUNT ||
        g_disk2_info[drive].format != DISK2_IMAGE_WOZ) {
        return -1;
    }

    length = disk2_reg_read(DISK2_REG_TRACK_LENGTH);
    bit_count = disk2_reg_read(DISK2_REG_TRACK_BIT_COUNT);
    if (length == 0U || bit_count == 0U ||
        length > DISK2_TRACK_STREAM_BYTES ||
        bit_count > (length * 8U)) {
        return -1;
    }

    if (read_loaded_track_ddr(g_scan_buf, length) != 0) {
        return -1;
    }

    analyze_woz_track(
        g_scan_buf,
        bit_count,
        drive,
        (uint8_t)(qtrack / 4U),
        qtrack,
        out);
    return 0;
}

int disk2_service_wozscan_file_track(uint8_t drive, uint8_t track, disk2_track_scan_t *out)
{
    disk2_track_stream_t stream;
    int rc;

    if (out == NULL || drive >= DISK2_DRIVE_COUNT ||
        g_disk2_info[drive].format != DISK2_IMAGE_WOZ) {
        return -1;
    }

    rc = prepare_woz_track_stream(drive, (uint8_t)(track * 4U), g_scan_buf, &stream);
    if (rc != 0) {
        memset(out, 0, sizeof(*out));
        return rc;
    }

    analyze_woz_track(g_scan_buf,
                      stream.bit_count,
                      drive,
                      track,
                      (uint8_t)(track * 4U),
                      out);
    return 0;
}

int disk2_service_get_activity(disk2_activity_t *out)
{
    uint32_t status;
    uint32_t write_info;

    if (out == NULL) {
        return -1;
    }

    status = disk2_reg_read(0U);
    write_info = disk2_reg_read(DISK2_REG_WRITE_INFO);

    out->present_mask = (uint8_t)(status & 0x03U);
    out->enabled = (uint8_t)((status >> 2U) & 0x01U);
    out->motor_on = (uint8_t)((status >> 3U) & 0x01U);
    out->drive = (uint8_t)((status >> 4U) & 0x01U);
    out->phase = (uint8_t)((status >> 5U) & 0x0FU);
    out->spinning = (uint8_t)((status >> 11U) & 0x01U);
    out->write_busy = (uint8_t)((write_info >> 1U) & 0x01U);
    out->write_dirty = (uint8_t)(write_info & 0x01U);
    out->write_qtrack = (uint8_t)((write_info >> DISK2_WRITE_INFO_QTRACK_SHIFT) & 0xFFU);
    out->write_drive = (uint8_t)((write_info >> DISK2_WRITE_INFO_DRIVE_SHIFT) & 0x01U);
    out->io_count = disk2_reg_read(0x05U);
    out->read_count = disk2_reg_read(DISK2_REG_STREAM_READS);
    out->write_count = disk2_reg_read(DISK2_REG_WRITE_COUNT);
    return 0;
}

const char *disk2_service_format_name(disk2_image_format_t format)
{
    switch (format) {
    case DISK2_IMAGE_WOZ:
        return "WOZ";
    case DISK2_IMAGE_NIB:
        return "NIB";
    case DISK2_IMAGE_DSK:
        return "DSK";
    case DISK2_IMAGE_DO:
        return "DO";
    case DISK2_IMAGE_PO:
        return "PO";
    case DISK2_IMAGE_2MG:
        return "2MG";
    case DISK2_IMAGE_NONE:
    default:
        return "none";
    }
}
