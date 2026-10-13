/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef APPLETINI_MULTIBOOT_PLATFORM_H
#define APPLETINI_MULTIBOOT_PLATFORM_H

#include "multiboot.h"

typedef struct {
    int golden_available;
    int appletini_present;
    int multitini_present;
    int legacy_present;
    uint32_t default_product;
    uint32_t request_product;
} mb_platform_status_t;

int mb_platform_open(void);
const mb_flash_t *mb_platform_flash(void);
int mb_platform_status(uint32_t current_product, mb_platform_status_t *status);
/* Display only: bounded slot probes and cached verified Golden identity.
 * This is not authorization to boot, change startup choice or write flash. */
int mb_platform_status_ui(uint32_t current_product, mb_platform_status_t *status);
int mb_platform_golden_version_cached(char *version, uint32_t capacity);
int mb_platform_set_default(uint32_t product);
int mb_platform_request(uint32_t product);
int mb_platform_clear_request(void);
int mb_platform_golden_offset(uint32_t *offset);

#endif
