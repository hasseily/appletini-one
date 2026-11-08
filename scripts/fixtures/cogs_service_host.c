#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "cogs_service.h"
#include "http_download.h"
#include "http_service.h"

#define STANDALONE_URL "https://disks.a2cogs.com/disks/game.po?cache=v.1"
#define SHARED_URL "https://github.com/a2-4am/4cade/releases/download/v6.1/Total.Replay.hdv"
#define STANDALONE_BYTES (280U * 512U)
#define SHARED_BYTES (65535U * 512U)

static const char mixed_catalog[] =
    "{\"pages\":2,\"count\":5,\"volumes\":["
    "{\"name\":\"Standalone Game\",\"url\":\"" STANDALONE_URL "\",\"blocks\":280,\"cat\":\"G\",\"host\":\"any\"},"
    "{\"name\":\"Shared Game\",\"url\":\"" SHARED_URL "\",\"blocks\":65535,\"cat\":\"G\",\"host\":\"any\","
      "\"in\":\"Total Replay v6.1\"},"
    "{\"name\":\"GS Game\",\"url\":\"https://example.com/gs.po\",\"blocks\":1,\"cat\":\"G\",\"host\":\"gs\"},"
    "{\"name\":\"Utility\",\"url\":\"https://example.com/util.po\",\"blocks\":1,\"cat\":\"S\",\"host\":\"any\"},"
    "{\"name\":\"Unsupported\",\"url\":\"https://example.com/odd.woz\",\"blocks\":1,\"cat\":\"G\",\"host\":\"any\"}]}";

static uint8_t owner;
static http_status_t response;
static const char *retained;
static char request_url[HTTP_URL_MAX];
static unsigned requests, http_cancels;
static uint8_t retain_requested, http_cleanup_blocked;

static http_download_status_t writer;
static uint8_t writer_active, writer_cleanup_blocked;
static unsigned writer_begins, writer_cancels;
static char writer_name[HTTP_DOWNLOAD_NAME_MAX];
static char writer_path[HTTP_DOWNLOAD_PATH_MAX];
static uint32_t writer_expected_bytes;
enum { WRITER_PROGRESS, WRITER_COMPLETE, WRITER_FAIL };
static unsigned writer_action;

int http_service_start_owned(const http_request_t *request, uint8_t who)
{
    assert(who == HTTP_OWNER_COGS && strcmp(request->method, "GET") == 0);
    assert(request->body == NULL && request->body_length == 0);
    if (request->retain) {
        assert(strstr(request->headers, "Accept: application/json\r\n") != NULL);
        assert(strstr(request->headers, "Referer:") == NULL);
    } else if (strcmp(request->url, STANDALONE_URL) == 0) {
        assert(strstr(request->headers, "Referer: https://a2cogs.com/browse/\r\n") != NULL);
    }
    ++requests;
    if (owner != HTTP_OWNER_NONE) {
        return HTTP_E_BUSY;
    }
    owner = who;
    memset(&response, 0, sizeof(response));
    response.state = HTTP_S_READ;
    retain_requested = request->retain;
    retained = NULL;
    (void)snprintf(request_url, sizeof(request_url), "%s", request->url);
    return 0;
}

uint8_t http_service_getowner(void)
{
    return owner;
}

int http_service_cancel_owned(uint8_t who)
{
    assert(who == HTTP_OWNER_COGS && owner == who);
    assert(!writer_active);
    ++http_cancels;
    if (http_cleanup_blocked) {
        return HTTP_E_IO;
    }
    owner = HTTP_OWNER_NONE;
    retained = NULL;
    return 0;
}

void http_service_status(http_status_t *out)
{
    *out = response;
}

const uint8_t *http_service_retained_owned(uint8_t who, size_t *length)
{
    assert(who == HTTP_OWNER_COGS && owner == who && retain_requested);
    *length = retained != NULL ? strlen(retained) : 0U;
    return (const uint8_t *)retained;
}

int http_download_begin_cogs(const http_status_t *source, const char *name, uint32_t expected)
{
    assert(!writer_active && owner == HTTP_OWNER_COGS && !retain_requested);
    assert(source->status == 200 && source->headers_ready);
    ++writer_begins;
    (void)snprintf(writer_name, sizeof(writer_name), "%s", name);
    writer_expected_bytes = expected;
    if (source->content_length_known && source->content_length != expected) {
        return HTTP_DL_E_LENGTH;
    }
    (void)snprintf(writer_path, sizeof(writer_path), "%s/%s", HTTP_DOWNLOAD_COGS_DIRECTORY, name);
    writer = (http_download_status_t){.state = HTTP_DL_WRITING, .total_known = 1U, .total = expected};
    writer_active = 1U;
    writer_action = WRITER_PROGRESS;
    return 0;
}

void http_download_cogs_poll(void)
{
    assert(writer_active && owner == HTTP_OWNER_COGS);
    if (writer_action == WRITER_COMPLETE) {
        writer.written = writer.total;
        writer.state = HTTP_DL_DONE;
        writer_active = 0U;
    } else if (writer_action == WRITER_FAIL) {
        writer.result = HTTP_DL_E_WRITE;
        writer.state = HTTP_DL_FAILED;
        writer_active = writer_cleanup_blocked;
    } else {
        writer.written += 512U;
    }
}

void http_download_cogs_cancel(void)
{
    assert(writer_active && owner == HTTP_OWNER_COGS);
    ++writer_cancels;
    writer.state = writer_cleanup_blocked ? HTTP_DL_FAILED : HTTP_DL_CANCELLED;
    writer_active = writer_cleanup_blocked;
}

int http_download_cogs_active(void)
{
    return writer_active;
}

void http_download_cogs_status(http_download_status_t *out)
{
    *out = writer;
}

const char *http_download_cogs_path(void)
{
    assert(writer.state == HTTP_DL_DONE && !writer_active);
    return writer_path;
}

static void begin_page(uint16_t page)
{
    unsigned previous_requests = requests;
    assert(cogs_service_browse(page) == 0);
    assert(cogs_service_status()->state == COGS_LOADING);
    assert(requests == previous_requests && owner == HTTP_OWNER_NONE);
    cogs_service_poll(1U);
    assert(requests == previous_requests + 1U && owner == HTTP_OWNER_COGS && retain_requested);
    char expected[96];
    (void)snprintf(expected, sizeof(expected),
                   "https://a2cogs.com/disks/search?q=&p=%u&cat=G&host=8", (unsigned)page);
    assert(strcmp(request_url, expected) == 0);
}

static void reply(unsigned code, uint8_t state)
{
    response.status = (uint16_t)code;
    response.state = state;
    response.headers_ready = 1U;
    cogs_service_poll(1U);
}

static void expect_idle(uint8_t state, const char *message)
{
    assert(cogs_service_status()->state == state);
    assert(strstr(cogs_service_status()->detail, message) != NULL);
    assert(!cogs_service_active() && owner == HTTP_OWNER_NONE && !writer_active);
}

static void begin_asset(uint8_t row, const char *url)
{
    unsigned previous_requests = requests;
    assert(cogs_service_download(row) == 0);
    assert(cogs_service_status()->state == COGS_DOWNLOADING);
    assert(requests == previous_requests && owner == HTTP_OWNER_NONE);
    cogs_service_poll(1U);
    assert(requests == previous_requests + 1U && owner == HTTP_OWNER_COGS && !retain_requested);
    assert(strcmp(request_url, url) == 0);
}

static void image_headers(uint32_t bytes, const char *content_type)
{
    response.content_length_known = 1U;
    response.content_length = bytes;
    (void)snprintf(response.content_type, sizeof(response.content_type), "%s", content_type);
    reply(200U, HTTP_S_READ);
}

static void check_save(uint8_t row, const char *url, const char *name, uint32_t bytes)
{
    unsigned previous_begins = writer_begins;
    begin_asset(row, url);
    assert(cogs_service_status()->total == bytes);
    image_headers(bytes, "application/octet-stream");
    assert(writer_active && writer_begins == previous_begins + 1U);
    assert(strcmp(writer_name, name) == 0 && writer_expected_bytes == bytes);
    cogs_service_poll(1U);
    assert(cogs_service_status()->received == 512U);
    writer_action = WRITER_COMPLETE;
    cogs_service_poll(1U);
    expect_idle(COGS_SAVED, name);
    assert(cogs_service_status()->received == bytes);
}

int main(void)
{
    assert(cogs_service_browse(0U) != 0 && cogs_service_browse(2U) != 0);
    assert(cogs_service_download(0U) != 0);
    begin_page(1U);
    assert(cogs_service_browse(1U) != 0 && cogs_service_download(0U) != 0);
    retained = mixed_catalog;
    reply(200U, HTTP_S_DONE);
    expect_idle(COGS_READY, "Choose a game");
    assert(cogs_service_status()->page == 1U && cogs_service_page()->count == 2U);
    assert(cogs_service_page()->pages == 2U && cogs_service_browse(3U) != 0);
    assert(cogs_service_download(2U) != 0);

    begin_page(2U);
    retained = "{\"pages\":2,\"count\":5,\"volumes\":[]}";
    reply(200U, HTTP_S_DONE);
    expect_idle(COGS_READY, "No 8-bit games");
    assert(cogs_service_status()->page == 2U && cogs_service_page()->count == 0U);
    begin_page(1U);
    retained = mixed_catalog;
    reply(200U, HTTP_S_DONE);
    expect_idle(COGS_READY, "Choose a game");
    puts("PASS 1-based 8-bit games queries, paging, and real catalog filtering");

    check_save(0U, STANDALONE_URL, "Standalone Game.po", STANDALONE_BYTES);
    check_save(1U, SHARED_URL, "Total Replay v6.1.hdv", SHARED_BYTES);
    puts("PASS standalone and shared volume names, exact byte counts, and completed save");

    unsigned previous_begins = writer_begins;
    begin_asset(0U, STANDALONE_URL);
    reply(403U, HTTP_S_READ);
    expect_idle(COGS_FAILED, "HTTP 403");
    begin_asset(0U, STANDALONE_URL);
    image_headers(STANDALONE_BYTES, "text/html; charset=utf-8");
    expect_idle(COGS_FAILED, "not a disk image");
    assert(writer_begins == previous_begins);
    begin_asset(0U, STANDALONE_URL);
    image_headers(STANDALONE_BYTES - 1U, "application/octet-stream");
    expect_idle(COGS_FAILED, "(-127)");
    assert(writer_begins == previous_begins + 1U && writer_expected_bytes == STANDALONE_BYTES);

    begin_page(2U);
    reply(504U, HTTP_S_DONE);
    expect_idle(COGS_FAILED, "HTTP 504");
    assert(cogs_service_status()->page == 1U && cogs_service_page()->count == 2U);
    begin_page(2U);
    response.result = HTTP_E_DNS;
    reply(0U, HTTP_S_FAILED);
    expect_idle(COGS_FAILED, "(-106)");
    begin_page(2U);
    retained = "{\"pages\":2,\"volumes\":[";
    reply(200U, HTTP_S_DONE);
    expect_idle(COGS_FAILED, "Invalid CoGS catalog");
    assert(cogs_service_page()->count == 2U);
    puts("PASS HTTP, image MIME/length, and malformed catalog errors");

    begin_asset(0U, STANDALONE_URL);
    image_headers(STANDALONE_BYTES, "application/octet-stream");
    writer_cleanup_blocked = 1U;
    cogs_service_cancel();
    unsigned previous_cancels = http_cancels;
    cogs_service_poll(1U);
    assert(cogs_service_active() && owner == HTTP_OWNER_COGS && http_cancels == previous_cancels);
    assert(strstr(cogs_service_status()->detail, "SD cleanup"));
    writer_cleanup_blocked = 0U;
    cogs_service_poll(1U);
    expect_idle(COGS_READY, "Download cancelled");
    assert(http_cancels == previous_cancels + 1U);

    begin_asset(0U, STANDALONE_URL);
    image_headers(STANDALONE_BYTES, "application/octet-stream");
    writer_cleanup_blocked = 1U;
    writer_action = WRITER_FAIL;
    cogs_service_poll(1U);
    assert(cogs_service_active() && owner == HTTP_OWNER_COGS);
    assert(strstr(cogs_service_status()->detail, "SD cleanup"));
    writer_cleanup_blocked = 0U;
    http_cleanup_blocked = 1U;
    cogs_service_poll(1U);
    assert(cogs_service_active() && owner == HTTP_OWNER_COGS && !writer_active);
    assert(cogs_service_status()->state == COGS_FAILED);
    http_cleanup_blocked = 0U;
    cogs_service_poll(0U);
    expect_idle(COGS_FAILED, "(-122)");
    puts("PASS cancel and failed SD cleanup retain ownership until cleanup completes");

    unsigned previous_requests = requests;
    assert(cogs_service_browse(1U) == 0);
    cogs_service_poll(0U);
    expect_idle(COGS_FAILED, "idle card and Ethernet");
    assert(requests == previous_requests);
    begin_page(1U);
    cogs_service_poll(0U);
    expect_idle(COGS_FAILED, "idle card and Ethernet");
    begin_asset(0U, STANDALONE_URL);
    image_headers(STANDALONE_BYTES, "application/octet-stream");
    unsigned previous_writer_cancels = writer_cancels;
    cogs_service_poll(0U);
    expect_idle(COGS_READY, "Download cancelled");
    assert(writer_cancels == previous_writer_cancels + 1U);

    owner = HTTP_OWNER_UPDATE;
    previous_cancels = http_cancels;
    assert(cogs_service_browse(1U) == 0);
    cogs_service_poll(1U);
    assert(!cogs_service_active() && cogs_service_status()->state == COGS_FAILED);
    assert(owner == HTTP_OWNER_UPDATE && http_cancels == previous_cancels);
    puts("PASS permission loss and foreign HTTP owner isolation");
    return 0;
}
