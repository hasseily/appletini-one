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
    uint8_t quad_read_enabled;
    uint8_t quad_probe_failed;
    uint8_t controller_ready;
    uint32_t input_clock_hz;
    uint64_t read_bytes;
    uint64_t read_transactions;
    uint32_t read_fallback_count;
} qspi_nor_t;

typedef struct {
    uint64_t bytes;          /* Successfully returned public-read bytes. */
    uint64_t transactions;  /* Read command attempts, including qualification. */
    uint32_t fallback_count;
    uint32_t chunk_bytes;
    uint32_t clock_hz;
    uint8_t quad_enabled;
    uint8_t probe_failed;
    uint8_t jedec_id[3];
} qspi_nor_read_stats_t;

int qspi_nor_init(qspi_nor_t *n, uint8_t addr_bytes, uint32_t sector_size);
uint32_t qspi_nor_capacity_bytes(const qspi_nor_t *n);
void qspi_nor_get_read_stats(const qspi_nor_t *n, qspi_nor_read_stats_t *stats);
const char *qspi_nor_read_mode(const qspi_nor_t *n);
/* CPU0-only, cooperative and non-reentrant: reads use shared aligned scratch
 * buffers. Quad output is qualified without changing any flash status bits. */
int qspi_nor_read(qspi_nor_t *n, uint32_t addr, void *dst, uint32_t len);
int qspi_nor_erase_region(qspi_nor_t *n, uint32_t addr, uint32_t len);
int qspi_nor_program(qspi_nor_t *n, uint32_t addr, const void *src, uint32_t len);

#endif
