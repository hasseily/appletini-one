/* SPDX-License-Identifier: GPL-3.0-only */
#include "multiboot_platform.h"
#include "qspi_nor.h"
#include "xstatus.h"

#include <string.h>

static qspi_nor_t nor;
static mb_flash_t flash;
static int opened;

static int flash_read(void *context, uint32_t offset, void *destination, uint32_t length)
{
    return qspi_nor_read((qspi_nor_t *)context, offset, destination, length) == XST_SUCCESS ? 0 : -1;
}

static int flash_erase(void *context, uint32_t offset, uint32_t length)
{
    return qspi_nor_erase_region((qspi_nor_t *)context, offset, length) == XST_SUCCESS ? 0 : -1;
}

static int flash_program(void *context, uint32_t offset, const void *source, uint32_t length)
{
    return qspi_nor_program((qspi_nor_t *)context, offset, source, length) == XST_SUCCESS ? 0 : -1;
}

int mb_platform_open(void)
{
    if (opened) return MB_OK;
    if (qspi_nor_init(&nor, 3U, MB_SECTOR_SIZE) != XST_SUCCESS ||
        qspi_nor_capacity_bytes(&nor) < MB_FLASH_SIZE) return MB_E_READ;
    flash.context = &nor;
    flash.read = flash_read;
    flash.erase = flash_erase;
    flash.program = flash_program;
    flash.capacity = MB_FLASH_SIZE;
    opened = 1;
    return MB_OK;
}

const mb_flash_t *mb_platform_flash(void)
{
    return mb_platform_open() == MB_OK ? &flash : NULL;
}

int mb_platform_status(uint32_t current_product, mb_platform_status_t *status)
{
    mb_metadata_t metadata;
    mb_selection_t selection;
    uint32_t golden_offset;
    if (status == NULL) return MB_E_ARGUMENT;
    memset(status, 0, sizeof(*status));
    if (mb_platform_open() != MB_OK) return MB_E_READ;
    status->golden_available = mb_golden_available(&flash, &golden_offset) == MB_OK;
    status->appletini_present = mb_slot_verify(&flash, MB_PRODUCT_APPLETINI, NULL) == MB_OK;
    status->multitini_present = mb_slot_verify(&flash, MB_PRODUCT_MULTITINI, NULL) == MB_OK;
    if (mb_metadata_load(&flash, &metadata) == MB_OK) {
        status->default_product = metadata.default_product;
        status->request_product = metadata.request_product;
    }
    if (mb_select(&flash, MB_PRODUCT_NONE, &selection) == MB_OK) {
        status->legacy_present = selection.legacy != 0U;
        if (status->default_product == MB_PRODUCT_NONE && selection.product != MB_PRODUCT_NONE)
            status->default_product = selection.product;
    }
    /* Before migration a verified legacy image has no product identity.
     * The running application's identity may label its startup fallback,
     * but never advertises an untagged image as an installed peer. */
    if (status->default_product == MB_PRODUCT_NONE && status->legacy_present)
        status->default_product = current_product;
    return MB_OK;
}

int mb_platform_set_default(uint32_t product)
{
    return mb_platform_open() == MB_OK ? mb_set_default(&flash, product) : MB_E_READ;
}

int mb_platform_request(uint32_t product)
{
    return mb_platform_open() == MB_OK ? mb_set_request(&flash, product) : MB_E_READ;
}

int mb_platform_clear_request(void)
{
    return mb_platform_open() == MB_OK ? mb_clear_request(&flash) : MB_E_READ;
}

int mb_platform_golden_offset(uint32_t *offset)
{
    return mb_platform_open() == MB_OK ? mb_golden_available(&flash, offset) : MB_E_READ;
}
