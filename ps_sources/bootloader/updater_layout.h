#ifndef BOOT_UPDATER_LAYOUT_H
#define BOOT_UPDATER_LAYOUT_H

#include <stdint.h>

typedef struct {
    uint32_t offset;
    uint32_t size;
} flash_region_t;

typedef struct {
    uint8_t qspi_addr_bytes; /* 3 for <=16MiB addressing, 4 for >16MiB */
    uint32_t erase_size_bytes; /* usually 64 KiB */
    flash_region_t golden;
    flash_region_t firmware;
    flash_region_t metadata; /* update and verified-boot metadata */
} flash_layout_t;

/* 32 KiB unit used by the Zynq Multiboot register. */
#define ZYNQ_MULTIBOOT_GRANULARITY_BYTES 0x8000U

const flash_layout_t *updater_layout_active(void);
const char *updater_layout_name(const flash_layout_t *layout);
uint32_t updater_layout_firmware_offset(const flash_layout_t *layout);

#endif
