#ifndef BOOT_UPDATER_CRC32_H
#define BOOT_UPDATER_CRC32_H

#include <stdint.h>
#include <stddef.h>

uint32_t crc32_init(void);
uint32_t crc32_update(uint32_t crc, const void *data, size_t len);
uint32_t crc32_finish(uint32_t crc);

#endif
