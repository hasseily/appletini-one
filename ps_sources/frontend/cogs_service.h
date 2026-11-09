#ifndef COGS_SERVICE_H
#define COGS_SERVICE_H

#include "cogs_catalog.h"
#include "http_download.h"

#define COGS_SAVED_PREFIX "saved: "

enum { COGS_IDLE, COGS_LOADING, COGS_READY, COGS_DOWNLOADING, COGS_SAVED, COGS_FAILED };

typedef struct {
    uint8_t state;
    uint16_t page;
    uint32_t received, total;
    char detail[HTTP_DOWNLOAD_PATH_MAX + sizeof(COGS_SAVED_PREFIX) - 1U];
} cogs_service_status_t;

#ifdef APPLETINI_HTTP_SERVICE
/* A first browse queues the catalog scan. A completed cache lasts until reboot. */
int cogs_service_browse(uint16_t page);
int cogs_service_browse_first(void);
/* Only the foreground poll takes HTTP or SD ownership. */
int cogs_service_download(uint8_t row);
void cogs_service_cancel(void);
void cogs_service_poll(uint8_t allowed);
int cogs_service_active(void);
const cogs_catalog_page_t *cogs_service_page(void);
const cogs_service_status_t *cogs_service_status(void);
#endif

#endif
