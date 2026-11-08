#ifdef APPLETINI_HTTP_SERVICE
#include "cogs_service.h"

#include "http_download.h"
#include "http_service.h"

#include <stdio.h>
#include <string.h>

enum { P_IDLE, P_PAGE_START, P_PAGE_READ, P_IMAGE_START, P_IMAGE_READ,
       P_IMAGE_WRITE, P_HTTP_CLEANUP };

static cogs_catalog_page_t catalog;
static cogs_catalog_page_t incoming;
static cogs_service_status_t status;
static uint16_t requested_page;
static uint8_t selected_row;
static uint8_t phase;
static uint8_t cancel_requested;

int cogs_service_active(void)
{
    return phase != P_IDLE;
}

const cogs_catalog_page_t *cogs_service_page(void)
{
    return &catalog;
}

const cogs_service_status_t *cogs_service_status(void)
{
    return &status;
}

static void set_status(uint8_t state, const char *detail)
{
    status.state = state;
    (void)snprintf(status.detail, sizeof(status.detail), "%s", detail);
}

int cogs_service_browse(uint16_t page)
{
    if (cogs_service_active() || page == 0U ||
        (page != 1U && page > catalog.pages)) {
        return -1;
    }
    requested_page = page;
    cancel_requested = 0U;
    status.received = status.total = 0U;
    phase = P_PAGE_START;
    set_status(COGS_LOADING, "Loading 8-bit games");
    return 0;
}

int cogs_service_download(uint8_t row)
{
    if (cogs_service_active() || row >= catalog.count) {
        return -1;
    }
    selected_row = row;
    cancel_requested = 0U;
    status.received = 0U;
    status.total = catalog.entries[row].bytes;
    phase = P_IMAGE_START;
    set_status(COGS_DOWNLOADING, "Connecting to the disk image host");
    return 0;
}

void cogs_service_cancel(void)
{
    cancel_requested = 1U;
}

static void release_http(void)
{
    phase = P_IDLE;
    if (http_service_getowner() == HTTP_OWNER_COGS &&
        http_service_cancel_owned(HTTP_OWNER_COGS) != 0 &&
        http_service_getowner() == HTTP_OWNER_COGS) {
        phase = P_HTTP_CLEANUP;
    }
}

static void finish(uint8_t state, const char *detail)
{
    set_status(state, detail);
    release_http();
}

static void transport_error(const char *what, unsigned code, int result)
{
    status.state = COGS_FAILED;
    if (code != 0U && (code < 200U || code >= 300U)) {
        (void)snprintf(status.detail, sizeof(status.detail), "%s HTTP %u", what, code);
    } else {
        (void)snprintf(status.detail, sizeof(status.detail), "%s failed (%d)", what, result);
    }
    release_http();
}

static void start_http(const char *url, uint8_t retain, uint8_t next)
{
    static const char catalog_headers[] = "Accept: application/json\r\n";
    static const char image_headers[] = "Referer: https://a2cogs.com/browse/\r\n";
    const http_request_t request = {url, "GET", retain ? catalog_headers : image_headers,
                                    NULL, 0U, retain};
    const int result = http_service_start_owned(&request, HTTP_OWNER_COGS);
    if (result != 0) {
        transport_error("CoGS connection", 0U, result);
        return;
    }
    phase = next;
}

static void read_catalog(void)
{
    size_t length = 0U;
    const uint8_t *data = http_service_retained_owned(HTTP_OWNER_COGS, &length);
    if (data == NULL || cogs_catalog_parse(data, length, &incoming) != 0 ||
        (incoming.pages != 0U && requested_page > incoming.pages)) {
        finish(COGS_FAILED, "Invalid CoGS catalog page");
        return;
    }
    catalog = incoming;
    status.page = requested_page;
    finish(COGS_READY, catalog.count == 0U ? "No 8-bit games on this page" : "Choose a game to download");
}

static void begin_image(const http_status_t *response)
{
    const cogs_catalog_entry_t *entry = &catalog.entries[selected_row];
    http_url_t url;
    char name[HTTP_DOWNLOAD_NAME_MAX];
    if (http_url_parse(entry->url, &url) != 0) {
        finish(COGS_FAILED, "Invalid disk image URL");
        return;
    }
    const char *end = url.path + strcspn(url.path, "?");
    const char *extension = end;
    while (extension > url.path && extension[-1] != '/' && extension[-1] != '.') {
        --extension;
    }
    if (extension > url.path && extension[-1] == '.') {
        --extension;
    }
    const size_t extension_length = *extension == '.' ? (size_t)(end - extension) : 0U;
    if (extension_length == 0U || extension_length > 4U ||
        strncmp(response->content_type, "text/html", 9U) == 0 ||
        strncmp(response->content_type, "application/json", 16U) == 0) {
        finish(COGS_FAILED, "Response is not a disk image");
        return;
    }
    const char *title = entry->bundle[0] != '\0' ? entry->bundle : entry->title;
    (void)snprintf(name, sizeof(name), "%s%.*s", title, (int)extension_length, extension);
    const int result = http_download_begin_cogs(response, name, entry->bytes);
    if (result != 0) {
        transport_error("CoGS download", 0U, result);
        return;
    }
    phase = P_IMAGE_WRITE;
    set_status(COGS_DOWNLOADING, "Saving to _software/CoGS");
}

static void poll_image_writer(uint8_t allowed)
{
    if (!allowed || cancel_requested) {
        http_download_cogs_cancel();
    } else {
        http_download_cogs_poll();
    }
    http_download_status_t download;
    http_download_cogs_status(&download);
    status.received = download.written;
    if (http_download_cogs_active()) {
        if (download.state != HTTP_DL_WRITING) {
            set_status(COGS_DOWNLOADING, "Waiting for SD cleanup");
        }
        return;
    }
    if (download.state == HTTP_DL_DONE) {
        const char *path = http_download_cogs_path();
        const char *name = strrchr(path, '/');
        status.state = COGS_SAVED;
        (void)snprintf(status.detail, sizeof(status.detail), "Saved %.85s", name != NULL ? name + 1 : path);
        release_http();
    } else if (cancel_requested || download.state == HTTP_DL_CANCELLED) {
        finish(COGS_READY, "Download cancelled");
    } else {
        transport_error("CoGS SD download", 0U, download.result);
    }
}

void cogs_service_poll(uint8_t allowed)
{
    if (phase == P_IDLE) {
        return;
    }
    if (phase == P_HTTP_CLEANUP) {
        release_http();
        return;
    }
    if (phase == P_IMAGE_WRITE) {
        poll_image_writer(allowed);
        return;
    }
    if (cancel_requested) {
        finish(catalog.count != 0U ? COGS_READY : COGS_IDLE, "CoGS request cancelled");
        return;
    }
    if (!allowed) {
        finish(COGS_FAILED, "CoGS needs an idle card and Ethernet with an IP address");
        return;
    }
    if (phase == P_PAGE_START) {
        char url[96];
        (void)snprintf(url, sizeof(url),
                       "https://a2cogs.com/disks/search?q=&p=%u&cat=G&host=8", requested_page);
        start_http(url, 1U, P_PAGE_READ);
        return;
    }
    if (phase == P_IMAGE_START) {
        start_http(catalog.entries[selected_row].url, 0U, P_IMAGE_READ);
        return;
    }
    if (http_service_getowner() != HTTP_OWNER_COGS) {
        transport_error("CoGS connection", 0U, HTTP_E_BUSY);
        return;
    }

    http_status_t response;
    http_service_status(&response);
    if (response.state == HTTP_S_FAILED ||
        (response.headers_ready && response.status >= 400U) ||
        (response.state == HTTP_S_DONE && response.status != 200U)) {
        transport_error("CoGS server", response.status, response.result);
    } else if (phase == P_PAGE_READ && response.state == HTTP_S_DONE) {
        read_catalog();
    } else if (phase == P_IMAGE_READ && response.headers_ready && response.status == 200U) {
        begin_image(&response);
    }
}
#endif
