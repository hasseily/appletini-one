#ifndef BOOT_UPDATER_UPDATER_H
#define BOOT_UPDATER_UPDATER_H

#include <stdint.h>

typedef struct {
    int found_file;
    int updated;
    int verified;
    /* Batch totals. A verified install can also fail source close or rename,
     * so files_updated and files_failed need not sum to files_found. */
    uint32_t files_found;
    uint32_t files_updated;
    uint32_t files_failed;
    uint32_t files_renamed;
    uint32_t rename_failures;
    /* Details of the last verified install; later errors do not clear them. */
    uint32_t file_size;
    uint32_t file_crc32;
    uint32_t flash_crc32;
    int error_code;
    char error_msg[96];
} updater_result_t;

/* Process all three recognized filenames once, in order. Any file error is
 * reported after the batch; successful installs remain eligible to boot. */
int updater_run(updater_result_t *result);
int updater_sync_boot_metadata(void);
int updater_has_verified_firmware(void);
/* No available verified image returns failure. */
int updater_select_firmware(uint32_t *boot_offset);

#endif
