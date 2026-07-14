#ifndef BOOT_UPDATER_UPDATER_H
#define BOOT_UPDATER_UPDATER_H

#include <stdint.h>

typedef struct {
    int found_file;
    int updated;
    int verified;
    uint32_t file_size;
    uint32_t file_crc32;
    uint32_t flash_crc32;
    int error_code;
    char error_msg[96];
} updater_result_t;

int updater_run(updater_result_t *result);
int updater_sync_boot_metadata(void);
int updater_has_verified_firmware(void);

#endif
