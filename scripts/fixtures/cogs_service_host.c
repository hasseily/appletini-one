#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "cogs_service.h"
#include "http_download.h"
#include "http_service.h"

#define STANDALONE_URL "https://disks.a2cogs.com/disks/game.po?cache=v.1"
#define REPLAY_URL "https://github.com/a2-4am/4cade/releases/download/v6.1/Total.Replay.hdv"
#define REPLAY_TITLE "Total Replay v6.1"
#define GENERATED_URL "https://disks.a2cogs.com/disks/game%02u.po"
#define STANDALONE_BYTES (280U * 512U)
#define REPLAY_BYTES 33553920U

static const char mixed_catalog[] =
    "{\"pages\":4,\"count\":25,\"volumes\":["
    "{\"name\":\"Standalone Game\",\"url\":\"" STANDALONE_URL "\",\"blocks\":280,\"cat\":\"G\",\"host\":\"any\"},"
    "{\"name\":\"Airheart\",\"url\":\"" REPLAY_URL "\",\"blocks\":65535,\"cat\":\"G\",\"host\":\"any\","
      "\"in\":\"" REPLAY_TITLE "\"},"
    "{\"name\":\"GS Game\",\"url\":\"https://example.com/gs.po\",\"blocks\":1,\"cat\":\"G\",\"host\":\"gs\"},"
    "{\"name\":\"Utility\",\"url\":\"https://example.com/util.po\",\"blocks\":1,\"cat\":\"S\",\"host\":\"any\"},"
    "{\"name\":\"Unsupported\",\"url\":\"https://example.com/odd.woz\",\"blocks\":1,\"cat\":\"G\",\"host\":\"any\"}]}";
static const char empty_page[] = "{\"pages\":4,\"count\":25,\"volumes\":[]}";
static const char final_catalog_page[] =
    "{\"pages\":4,\"count\":25,\"volumes\":["
    "{\"name\":\"Game 19\",\"url\":\"https://disks.a2cogs.com/disks/game19.po\",\"blocks\":280,\"cat\":\"G\",\"host\":\"any\"},"
    "{\"name\":\"" REPLAY_TITLE "\",\"url\":\"" REPLAY_URL "\",\"blocks\":65535,\"cat\":\"G\",\"host\":\"any\"}]}";
static const char partial_catalog[] =
    "{\"pages\":2,\"count\":1,\"volumes\":["
    "{\"name\":\"Replacement\",\"url\":\"" STANDALONE_URL "\",\"blocks\":280,\"cat\":\"G\",\"host\":\"any\"}]}";

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

static void start_queued_page(uint16_t page)
{
    unsigned previous_requests = requests;
    assert(cogs_service_status()->state == COGS_LOADING);
    assert(owner == HTTP_OWNER_NONE);
    cogs_service_poll(1U);
    assert(requests == previous_requests + 1U && owner == HTTP_OWNER_COGS && retain_requested);
    char expected[96];
    (void)snprintf(expected, sizeof(expected),
                   "https://a2cogs.com/disks/search?q=&p=%u&cat=G&host=8", (unsigned)page);
    assert(strcmp(request_url, expected) == 0);
}

static void begin_scan(void)
{
    unsigned previous_requests = requests;
    assert(cogs_service_browse_first() == 0);
    assert(requests == previous_requests && owner == HTTP_OWNER_NONE);
    start_queued_page(1U);
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
    if (message != NULL) {
        assert(strstr(cogs_service_status()->detail, message) != NULL);
    }
    assert(!cogs_service_active() && owner == HTTP_OWNER_NONE && !writer_active);
}

static void answer_catalog(const char *body)
{
    retained = body;
    reply(200U, HTTP_S_DONE);
}

static void answer_numbered_page(unsigned pages, unsigned total, unsigned start, unsigned count)
{
    char body[4096];
    size_t used = (size_t)snprintf(body, sizeof(body),
                                 "{\"pages\":%u,\"count\":%u,\"volumes\":[", pages, total);
    for (unsigned i = 0; i < count; ++i) {
        unsigned number = start + i;
        int added = snprintf(body + used, sizeof(body) - used,
            "%s{\"name\":\"Game %02u\",\"url\":\"" GENERATED_URL "\",\"blocks\":280,\"cat\":\"G\",\"host\":\"any\"}",
            i == 0U ? "" : ",", number, number);
        assert(added > 0 && (size_t)added < sizeof(body) - used);
        used += (size_t)added;
    }
    assert(sizeof(body) - used >= 3U);
    strcpy(body + used, "]}");
    answer_catalog(body);
}

static void expect_catalog_page(unsigned page, unsigned pages, unsigned count)
{
    assert(cogs_service_status()->page == page);
    assert(cogs_service_page()->pages == pages);
    assert(cogs_service_page()->count == count);
}

static void expect_entry(unsigned row, const char *title, const char *url, uint32_t bytes)
{
    assert(row < cogs_service_page()->count);
    const cogs_catalog_entry_t *entry = &cogs_service_page()->entries[row];
    assert(strcmp(entry->title, title) == 0);
    assert(strcmp(entry->url, url) == 0 && entry->bytes == bytes);
}

static void expect_numbered_entry(unsigned row, unsigned number)
{
    char title[32], url[HTTP_URL_MAX];
    (void)snprintf(title, sizeof(title), "Game %02u", number);
    (void)snprintf(url, sizeof(url), GENERATED_URL, number);
    expect_entry(row, title, url, STANDALONE_BYTES);
}

static void browse_cached(unsigned page, unsigned pages, unsigned count)
{
    unsigned previous_requests = requests;
    assert(cogs_service_browse((uint16_t)page) == 0);
    assert(!cogs_service_active() && owner == HTTP_OWNER_NONE);
    expect_catalog_page(page, pages, count);
    cogs_service_poll(1U);
    assert(requests == previous_requests && owner == HTTP_OWNER_NONE);
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

static void check_save(uint8_t row, const char *url, const char *name, uint32_t bytes,
                       const char *final_path)
{
    unsigned previous_begins = writer_begins;
    begin_asset(row, url);
    assert(cogs_service_status()->total == bytes);
    image_headers(bytes, "application/octet-stream");
    assert(writer_active && writer_begins == previous_begins + 1U);
    assert(strcmp(writer_name, name) == 0 && writer_expected_bytes == bytes);
    cogs_service_poll(1U);
    assert(cogs_service_status()->received == 512U);
    (void)snprintf(writer_path, sizeof(writer_path), "%s", final_path);
    writer_action = WRITER_COMPLETE;
    cogs_service_poll(1U);
    expect_idle(COGS_SAVED, final_path);
    char expected[sizeof(cogs_service_status()->detail)];
    (void)snprintf(expected, sizeof(expected), "saved: %s", final_path);
    assert(strcmp(cogs_service_status()->detail, expected) == 0);
    assert(cogs_service_status()->received == bytes);
}

static void expect_no_cache(void)
{
    assert(cogs_service_page()->count == 0U && cogs_service_browse(1U) != 0);
    assert(cogs_service_download(0U) != 0);
}

static void check_failed_scans(void)
{
    unsigned previous_requests = requests;
    assert(cogs_service_browse_first() == 0);
    cogs_service_poll(0U);
    expect_idle(COGS_FAILED, "idle card and Ethernet");
    assert(requests == previous_requests);
    expect_no_cache();

    begin_scan();
    answer_catalog(partial_catalog);
    assert(cogs_service_active() && cogs_service_page()->count == 0U);
    start_queued_page(2U);
    reply(504U, HTTP_S_DONE);
    expect_idle(COGS_FAILED, "HTTP 504");
    expect_no_cache();

    begin_scan();
    response.result = HTTP_E_DNS;
    reply(0U, HTTP_S_FAILED);
    expect_idle(COGS_FAILED, "(-106)");
    expect_no_cache();

    begin_scan();
    answer_catalog("{\"pages\":2,\"volumes\":[");
    expect_idle(COGS_FAILED, "Invalid CoGS catalog");
    expect_no_cache();

    begin_scan();
    answer_catalog(partial_catalog);
    start_queued_page(2U);
    answer_catalog("{\"pages\":3,\"volumes\":[]}");
    expect_idle(COGS_FAILED, "Invalid CoGS catalog");
    expect_no_cache();

    begin_scan();
    cogs_service_poll(0U);
    expect_idle(COGS_FAILED, "idle card and Ethernet");
    expect_no_cache();

    owner = HTTP_OWNER_UPDATE;
    unsigned previous_cancels = http_cancels;
    assert(cogs_service_browse_first() == 0);
    cogs_service_poll(1U);
    assert(!cogs_service_active() && cogs_service_status()->state == COGS_FAILED);
    assert(owner == HTTP_OWNER_UPDATE && http_cancels == previous_cancels);
    owner = HTTP_OWNER_NONE;
    expect_no_cache();
    puts("PASS scan failures, permission loss and foreign HTTP owner isolation");

    begin_scan();
    answer_catalog(partial_catalog);
    assert(cogs_service_active() && owner == HTTP_OWNER_NONE);
    previous_requests = requests;
    cogs_service_cancel();
    cogs_service_poll(1U);
    expect_idle(COGS_IDLE, "cancelled");
    assert(requests == previous_requests);
    expect_no_cache();

    begin_scan();
    answer_catalog(partial_catalog);
    start_queued_page(2U);
    cogs_service_cancel();
    cogs_service_poll(1U);
    expect_idle(COGS_IDLE, "cancelled");
    expect_no_cache();

    begin_scan();
    http_cleanup_blocked = 1U;
    answer_catalog(partial_catalog);
    assert(cogs_service_active() && owner == HTTP_OWNER_COGS);
    previous_requests = requests;
    cogs_service_cancel();
    cogs_service_poll(1U);
    assert(cogs_service_active() && owner == HTTP_OWNER_COGS && requests == previous_requests);
    http_cleanup_blocked = 0U;
    cogs_service_poll(1U);
    expect_idle(COGS_IDLE, "cancelled");
    assert(requests == previous_requests);
    expect_no_cache();
    puts("PASS scan cancellation discards staged rows even during HTTP cleanup");
}

static void check_scan_bounds(void)
{
    begin_scan();
    answer_catalog("{\"pages\":257,\"volumes\":[]}");
    expect_idle(COGS_FAILED, NULL);
    expect_no_cache();

    begin_scan();
    for (unsigned page = 1U; page <= 15U; ++page) {
        if (page > 1U) {
            start_queued_page((uint16_t)page);
        }
        answer_numbered_page(15U, 257U, (page - 1U) * 18U + 1U, page == 15U ? 5U : 18U);
        assert(cogs_service_page()->count == 0U);
    }
    expect_idle(COGS_FAILED, NULL);
    expect_no_cache();
    puts("PASS source page and eligible entry bounds reject oversize without partial cache");
}

static void check_compact_scan(void)
{
    unsigned previous_requests = requests;
    begin_scan();
    assert(cogs_service_browse(1U) != 0 && cogs_service_download(0U) != 0);
    answer_catalog(mixed_catalog);
    assert(cogs_service_page()->count == 0U);
    start_queued_page(2U);
    answer_catalog(empty_page);
    start_queued_page(3U);
    answer_numbered_page(4U, 25U, 1U, 18U);
    assert(cogs_service_page()->count == 0U);
    start_queued_page(4U);
    answer_catalog(final_catalog_page);
    expect_idle(COGS_READY, "Choose a game");
    expect_catalog_page(1U, 2U, 20U);
    assert(requests == previous_requests + 4U && cogs_service_browse(3U) != 0);
    expect_entry(0U, "Standalone Game", STANDALONE_URL, STANDALONE_BYTES);
    for (unsigned row = 1U; row < 20U; ++row) {
        expect_numbered_entry(row, row);
    }
    check_save(19U, "https://disks.a2cogs.com/disks/game19.po", "Game 19.po", STANDALONE_BYTES,
               "0:/_software/CoGS/Game 19.po");
    browse_cached(2U, 2U, 1U);
    expect_entry(0U, REPLAY_TITLE, REPLAY_URL, REPLAY_BYTES);
    assert(cogs_service_download(1U) != 0);
    check_save(0U, REPLAY_URL, REPLAY_TITLE ".hdv", REPLAY_BYTES,
               "0:/_software/CoGS/" REPLAY_TITLE ".hdv");
    browse_cached(1U, 2U, 20U);
    expect_entry(0U, "Standalone Game", STANDALONE_URL, STANDALONE_BYTES);

    browse_cached(2U, 2U, 1U);
    previous_requests = requests;
    assert(cogs_service_browse_first() == 0);
    expect_idle(COGS_READY, "Choose a game");
    expect_catalog_page(1U, 2U, 20U);
    cogs_service_poll(1U);
    assert(requests == previous_requests);
    puts("PASS raw scan, Replay parent download, ordered 20+1 compaction and cache-only paging");
}

static void check_downloads(void)
{
    check_save(0U, STANDALONE_URL, "Standalone Game.po", STANDALONE_BYTES,
               "0:/_software/CoGS/Standalone Game-1.po");
    puts("PASS standalone volume name, exact byte count, and final save path");

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

    puts("PASS download HTTP and image MIME/length errors");

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

    begin_asset(0U, STANDALONE_URL);
    image_headers(STANDALONE_BYTES, "application/octet-stream");
    unsigned previous_writer_cancels = writer_cancels;
    cogs_service_poll(0U);
    expect_idle(COGS_READY, "Download cancelled");
    assert(writer_cancels == previous_writer_cancels + 1U);
    puts("PASS download permission loss");
}

static void check_empty_scan(void)
{
    begin_scan();
    answer_catalog("{\"pages\":1,\"count\":0,\"volumes\":[]}");
    expect_idle(COGS_READY, "No 8-bit game disks");
    expect_catalog_page(0U, 0U, 0U);
    assert(cogs_service_browse(1U) != 0);
    assert(cogs_service_download(0U) != 0);
    unsigned previous_requests = requests;
    assert(cogs_service_browse_first() == 0);
    expect_idle(COGS_READY, "No 8-bit game disks");
    expect_catalog_page(0U, 0U, 0U);
    cogs_service_poll(1U);
    assert(requests == previous_requests);
    puts("PASS empty completed catalog is also cached");
}

int main(int argc, char **argv)
{
    assert(cogs_service_browse(0U) != 0 && cogs_service_browse(2U) != 0);
    expect_no_cache();
    if (argc == 2 && strcmp(argv[1], "empty") == 0) {
        check_empty_scan();
        return 0;
    }
    assert(argc == 1);
    check_failed_scans();
    check_scan_bounds();
    check_compact_scan();
    check_downloads();
    return 0;
}
