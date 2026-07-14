#ifndef BOOT_UPDATER_METADATA_H
#define BOOT_UPDATER_METADATA_H

#include <stdint.h>

#define UPDATER_METADATA_MAGIC      0x55504454U /* "UPDT" */
#define UPDATER_METADATA_VERSION    1U

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t seq;
    uint32_t flags;            /* reserved */
    uint32_t update_attempts;
    uint32_t last_result;
    uint32_t last_boot_count;
    uint32_t firmware_size;
    uint32_t firmware_crc32;
    uint32_t crc32;            /* CRC over struct with this field zeroed */
} updater_metadata_t;
int updater_metadata_validate(const updater_metadata_t *m);

#endif
