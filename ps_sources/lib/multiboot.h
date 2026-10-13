/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef APPLETINI_MULTIBOOT_H
#define APPLETINI_MULTIBOOT_H

#include <stdint.h>

#define MB_FLASH_SIZE             0x01000000U
#define MB_GOLDEN_OFFSET          0x00000000U
#define MB_GOLDEN_TRIAL_OFFSET    0x00100000U
#define MB_GOLDEN_SIZE            0x00100000U
#define MB_APPLETINI_OFFSET       0x00200000U
#define MB_APPLETINI_CAPACITY     0x00600000U
#define MB_MULTITINI_OFFSET       0x00800000U
#define MB_MULTITINI_CAPACITY     0x007E0000U
#define MB_METADATA_A_OFFSET     0x00FE0000U
#define MB_METADATA_B_OFFSET     0x00FF0000U
#define MB_SECTOR_SIZE            0x00010000U
#define MB_MANIFEST_SIZE          32U
#define MB_METADATA_SIZE          128U
#define MB_PRODUCT_NONE           0U
#define MB_PRODUCT_APPLETINI      0x00000100U
#define MB_PRODUCT_MULTITINI      0x00000200U
#define MB_PRODUCT_MASK           0x00000300U
#define MB_FLAG_RECOVERY          0x00000001U
#define MB_FLAG_MULTIBOOT         0x00000400U
#define MB_FLAG_SLOT_VALID        0x00000001U
#define MB_FLAG_SLOT_LEGACY       0x00000002U
#define MB_APPLETINI_MARKER       "APPLETINI-MULTIBOOT-V1"
#define MB_MULTITINI_MARKER       "MULTITINI-MULTIBOOT-V1"
#define MB_RECOVERY_MARKER        "APPLETINI-GOLDEN-RECOVERY-V1"
#define MB_GOLDEN_MARKER          "APPLETINI-MULTIBOOT-GOLDEN-V1"

enum {
    MB_OK = 0, MB_E_ARGUMENT = 1, MB_E_READ = 2, MB_E_IMAGE = 3,
    MB_E_MANIFEST = 4, MB_E_CRC = 5, MB_E_MARKER = 6,
    MB_E_METADATA = 7, MB_E_UNAVAILABLE = 8, MB_E_OVERLAP = 9,
    MB_E_ERASE = 10, MB_E_PROGRAM = 11, MB_E_VERIFY = 12
};

/* All callbacks return zero on success. Reads must fill exactly len bytes.
 * Flash callbacks use absolute offsets; source callbacks use file offsets.
 * The physical driver must split programs/erases at its page/sector limits. */
typedef int (*mb_read_fn)(void *context, uint32_t offset, void *dst, uint32_t len);
typedef int (*mb_erase_fn)(void *context, uint32_t offset, uint32_t len);
typedef int (*mb_program_fn)(void *context, uint32_t offset, const void *src, uint32_t len);
typedef struct {
    void *context;
    mb_read_fn read;
    mb_erase_fn erase;
    mb_program_fn program;
    uint32_t capacity;
} mb_flash_t;
typedef struct {
    void *context;
    mb_read_fn read;
    uint32_t size;
} mb_reader_t;
typedef struct {
    uint32_t product, version, payload_size, payload_crc32;
    uint32_t image_size, image_crc32;
} mb_image_t;
typedef struct {
    uint32_t product, offset, capacity, payload_size, payload_crc32;
    uint32_t image_size, image_crc32, flags;
} mb_slot_record_t;
typedef struct {
    uint32_t sequence, default_product, request_product, sector_offset;
    mb_slot_record_t slots[2];
} mb_metadata_t;
typedef struct {
    uint32_t product, boot_offset, legacy;
    mb_image_t image;
} mb_selection_t;

uint32_t mb_product_offset(uint32_t product);
uint32_t mb_product_capacity(uint32_t product);
const char *mb_product_name(uint32_t product);
/* allow_legacy permits only manifest-v1 firmware, without assigning a product.
 * Tagged images require v2, both linked recovery/product markers, and exact
 * Bootgen payload/table bounds. The source size includes the 32-byte footer. */
int mb_image_validate(const mb_reader_t *reader, uint32_t expected_product,
                      int allow_legacy, mb_image_t *out);
int mb_metadata_load(const mb_flash_t *flash, mb_metadata_t *out);
/* out may be NULL for availability probes. */
int mb_slot_verify(const mb_flash_t *flash, uint32_t product, mb_image_t *out);
/* Fast advisory UI presence only: validates bounded Bootgen headers/partition
 * bounds, footer identity/CRC and committed eligibility. Does not read or CRC
 * the payload, prove linked capability or authorize firmware installation/boot.
 * Execution must still use mb_slot_verify/mb_select; installation and startup
 * choice still validate fully. A one-shot request delegates its full target
 * check to Golden and may use this advisory probe before committing metadata.
 * out may be NULL. image_crc32 is zero because the payload was not read. */
int mb_slot_probe(const mb_flash_t *flash, uint32_t product, mb_image_t *out);
int mb_golden_available(const mb_flash_t *flash, uint32_t *boot_offset_out);
/* Discover the linked version in the installed Golden executable, preferring
 * the primary copy and falling back to trial only on validation/read failure.
 * Manifest images require their complete payload CRC and Golden identity.
 * Historical raw B1.1 images have no payload CRC; verify their Bootgen headers
 * and loaded startup/version identity instead. Never reads a build-time version.
 * On failure, version is empty and slot_offset (when supplied) is UINT32_MAX. */
int mb_golden_installed_version(const mb_flash_t *flash, char *version,
                                uint32_t capacity, uint32_t *slot_offset);
/* NONE selects the persistent default. No eligible product returns golden
 * (product NONE and offset 0); a missing request falls back to the other slot.
 * Legacy is eligible only with old v4 metadata and a complete live v1 image. */
int mb_select(const mb_flash_t *flash, uint32_t requested, mb_selection_t *out);
int mb_set_default(const mb_flash_t *flash, uint32_t product);
int mb_set_request(const mb_flash_t *flash, uint32_t product);
/* Validate Golden live, probe committed target eligibility/headers/footer,
 * commit the one-shot request and return the checked Golden offset. Golden
 * fully validates target payload after reset and falls back if it is corrupt.
 * First metadata adoption still fully verifies images to establish records.
 * No caller-supplied token or cross-reset cache is accepted. On failure the
 * required golden_offset output is UINT32_MAX. */
int mb_prepare_request(const mb_flash_t *flash, uint32_t product,
                       uint32_t *golden_offset);
int mb_clear_request(const mb_flash_t *flash);
/* Clears target eligibility durably before any image erase. Other-slot data
 * and metadata survive. finish verifies live flash against the source record.
 * Use mb_install when possible: it validates before writes and withholds XLNX
 * until all bytes have been read back, then verifies and records the image. */
int mb_update_begin(const mb_flash_t *flash, uint32_t product);
int mb_update_finish(const mb_flash_t *flash, const mb_image_t *image);
int mb_install(const mb_flash_t *flash, const mb_reader_t *source, mb_image_t *out);

#endif
