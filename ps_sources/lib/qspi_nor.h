#ifndef BOOT_UPDATER_QSPI_NOR_H
#define BOOT_UPDATER_QSPI_NOR_H

#include <stdint.h>
#include "xqspips.h"

typedef struct {
    XQspiPs qspi;
    uint8_t addr_bytes;
    uint32_t page_size;
    uint32_t sector_size;
    uint32_t capacity_bytes;
    uint8_t jedec_id[3];
    uint8_t supports_4byte_mode;
} qspi_nor_t;

int qspi_nor_init(qspi_nor_t *n, uint8_t addr_bytes, uint32_t sector_size);
uint32_t qspi_nor_capacity_bytes(const qspi_nor_t *n);
int qspi_nor_read(qspi_nor_t *n, uint32_t addr, void *dst, uint32_t len);
int qspi_nor_erase_region(qspi_nor_t *n, uint32_t addr, uint32_t len);
int qspi_nor_program(qspi_nor_t *n, uint32_t addr, const void *src, uint32_t len);

#endif
