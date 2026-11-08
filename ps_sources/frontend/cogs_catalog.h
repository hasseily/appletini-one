#ifndef COGS_CATALOG_H
#define COGS_CATALOG_H

#include <stddef.h>
#include <stdint.h>
#include "http_core.h"

#define COGS_PAGE_CAPACITY 18U
#define COGS_MAX_IMAGE_BYTES (32U * 1024U * 1024U + 64U)

typedef struct {
    char title[48];
    char bundle[48];
    char url[HTTP_URL_MAX];
    /* Raw image size, or the minimum 2IMG size when the catalog only gives blocks. */
    uint32_t bytes;
} cogs_catalog_entry_t;

typedef struct {
    uint16_t pages;
    uint8_t count;
    cogs_catalog_entry_t entries[COGS_PAGE_CAPACITY];
} cogs_catalog_page_t;

/* Foreground only. Returns 0 for a valid page, even if no entries qualify.
 * Returns -1 on invalid JSON or page structure and clears pages/count. */
int cogs_catalog_parse(const uint8_t *json, size_t length, cogs_catalog_page_t *out);

#endif
