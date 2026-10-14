#ifndef BOOT_UPDATER_CRC32_H
#define BOOT_UPDATER_CRC32_H

#include <stdint.h>
#include <stddef.h>

/* CRC-32/ISO-HDLC: reflected 0xEDB88320 polynomial, all-ones initial state
 * and final XOR. update accepts an unfinished state and preserves it for a
 * zero-length chunk; data may be NULL only when len is zero. */
uint32_t crc32_init(void);
uint32_t crc32_update(uint32_t crc, const void *data, size_t len);
uint32_t crc32_finish(uint32_t crc);

#endif
