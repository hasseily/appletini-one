#ifndef BOOT_UPDATER_BOOT_CONTROL_H
#define BOOT_UPDATER_BOOT_CONTROL_H

#include <stdint.h>

void boot_control_soft_reset(void);
void boot_control_boot_qspi_image_offset(uint32_t image_offset_bytes);

#endif
