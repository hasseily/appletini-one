#ifdef APPLETINI_HTTP_SERVICE
#include "http_download.h"
#include "download_filename.h"
#include "ff.h"
#include <stdio.h>
#include <string.h>

#define COLLISION_LIMIT 10000U
#define COPY_BYTES 4096U

typedef struct {
    http_download_status_t status;
    FIL file;
    uint8_t open, owns_part, owns_stream, phase;
    unsigned suffix;
    char name[HTTP_DOWNLOAD_NAME_MAX], final[HTTP_DOWNLOAD_PATH_MAX];
    char part[HTTP_DOWNLOAD_PATH_MAX];
} staged_download_t;

static staged_download_t net_download;
static staged_download_t cogs_download;
/* Only one HTTP request can feed a staged download at a time. */
static uint8_t buffer[COPY_BYTES];
static http_status_t source_status;
static FATFS download_fs;

static uint8_t stream_owner(const staged_download_t *download)
{
    return download == &cogs_download ? HTTP_OWNER_COGS : HTTP_OWNER_DOWNLOAD;
}

static int cleanup(staged_download_t *download)
{
    if (download->open) {
        FRESULT result = f_close(&download->file);
        if (result == FR_OK || result == FR_INVALID_OBJECT) download->open = 0;
    }
    if (download->owns_part && !download->open) {
        FRESULT result = f_unlink(download->part);
        if (result == FR_OK || result == FR_NO_FILE) download->owns_part = 0;
    }
    if (download->owns_stream) {
        const uint8_t owner = stream_owner(download);
        if (http_service_getowner() != owner || !http_service_cancel_owned(owner))
            download->owns_stream = 0;
    }
    return download->open || download->owns_part || download->owns_stream;
}

static void fail(staged_download_t *download, int result)
{
    if (download->status.state == HTTP_DL_DONE) return;
    if (cleanup(download)) result = HTTP_DL_E_CLEANUP;
    download->status.result = (int16_t)(result ? result : HTTP_DL_E_STORAGE);
    download->status.state = HTTP_DL_FAILED;
}

static void cancel(staged_download_t *download)
{
    const int pending = cleanup(download);
    if (download->status.state == HTTP_DL_DONE || download->status.state == HTTP_DL_NONE) return;
    download->status.result = (int16_t)(pending ? HTTP_DL_E_CLEANUP : HTTP_E_CANCELLED);
    download->status.state = pending ? HTTP_DL_FAILED : HTTP_DL_CANCELLED;
}

static int active(const staged_download_t *download)
{
    return download->status.state == HTTP_DL_WAITING || download->status.state == HTTP_DL_WRITING ||
           download->open || download->owns_part || download->owns_stream;
}

int http_download_quiesce(void)
{
    cancel(&net_download);
    cancel(&cogs_download);
    return http_download_active() ? HTTP_DL_E_CLEANUP : 0;
}

void http_download_reset(void)
{
    cancel(&net_download);
    if (!active(&net_download)) memset(&net_download, 0, sizeof(net_download));
}

void http_download_wait(void)
{
    http_download_reset();
    if (!active(&net_download)) net_download.status.state = HTTP_DL_WAITING;
}

void http_download_fail(int result) { fail(&net_download, result); }
void http_download_cancel(void) { cancel(&net_download); }
int http_download_active(void) { return active(&net_download) || active(&cogs_download); }
void http_download_status(http_download_status_t *out) { *out = net_download.status; }
const char *http_download_path(void)
{
    return net_download.status.state == HTTP_DL_DONE ? net_download.final : "";
}

static int ensure_directory(const char *path)
{
    FILINFO info;
    FRESULT result = f_stat(path, &info);
    if (result == FR_NOT_ENABLED) {
        if (f_mount(&download_fs, "0:/", 1) != FR_OK) return HTTP_DL_E_STORAGE;
        result = f_stat(path, &info);
    }
    if (result == FR_NO_FILE) {
        result = f_mkdir(path);
        if (result == FR_OK) return 0;
        if (result != FR_EXIST) return HTTP_DL_E_STORAGE;
        result = f_stat(path, &info);
    }
    return result == FR_OK && (info.fattrib & AM_DIR) ? 0 : HTTP_DL_E_STORAGE;
}

static int final_name(staged_download_t *download)
{
    const char *directory = download == &cogs_download ?
        HTTP_DOWNLOAD_COGS_DIRECTORY : HTTP_DOWNLOAD_DIRECTORY;
    const char *extension = strrchr(download->name, '.');
    const size_t stem = extension ? (size_t)(extension - download->name) : strlen(download->name);
    int length;
    if (!download->suffix)
        length = snprintf(download->final, sizeof(download->final), "%s/%s",
                          directory, download->name);
    else
        length = snprintf(download->final, sizeof(download->final), "%s/%.*s-%u%s",
                          directory, (int)stem, download->name, download->suffix,
                          extension ? extension : "");
    return length < 0 || (size_t)length >= sizeof(download->final) ? HTTP_DL_E_ARGUMENT : 0;
}

static int open_part(staged_download_t *download)
{
    FILINFO info;
    /* Probe only one name per poll. */
    if (download->suffix >= COLLISION_LIMIT) return HTTP_DL_E_NAMES;
    if (final_name(download)) return HTTP_DL_E_ARGUMENT;
    FRESULT found = f_stat(download->final, &info);
    if (found == FR_OK) {
        ++download->suffix;
        return 0;
    }
    if (found != FR_NO_FILE) return HTTP_DL_E_STORAGE;
    int length = snprintf(download->part, sizeof(download->part), "%s.part", download->final);
    if (length < 0 || (size_t)length >= sizeof(download->part)) return HTTP_DL_E_ARGUMENT;
    FRESULT result = f_open(&download->file, download->part, FA_WRITE | FA_CREATE_NEW);
    if (result == FR_EXIST) {
        ++download->suffix;
        return 0;
    }
    if (result != FR_OK) return HTTP_DL_E_STORAGE;
    download->open = download->owns_part = download->phase = 1;
    return 1;
}

static int begin(staged_download_t *download, const http_status_t *source,
                 const char *preferred, uint32_t expected_bytes)
{
    if (!source || !source->headers_ready || source->status < 200 || source->status >= 300 ||
        source->retained || source->state == HTTP_S_FAILED || source->state == HTTP_S_IDLE)
        return HTTP_DL_E_ARGUMENT;
    if (download->open || download->owns_part || download->owns_stream) return HTTP_E_BUSY;

    const int cogs = download == &cogs_download;
    if (cogs && (!expected_bytes || !preferred || !*preferred || source->status != 200U))
        return HTTP_DL_E_ARGUMENT;
    if (cogs && source->content_length_known && source->content_length != expected_bytes)
        return HTTP_DL_E_LENGTH;
    int result = cogs ? http_service_handoff_download_owned(HTTP_OWNER_COGS) :
                        http_service_handoff_download();
    if (result) {
        fail(download, HTTP_DL_E_OWNER);
        return HTTP_DL_E_OWNER;
    }
    download->owns_stream = 1;
    download->status.state = HTTP_DL_WRITING;
    download->status.total_known = cogs || source->content_length_known;
    download->status.total = cogs ? expected_bytes : source->content_length;
    const char *hint = preferred && *preferred ? preferred : source->suggested_filename;
    if (download_filename(hint, source->url, download->name, sizeof(download->name)))
        result = HTTP_DL_E_ARGUMENT;
    if (!result && cogs) result = ensure_directory("0:/_software");
    if (!result) result = ensure_directory(cogs ? HTTP_DOWNLOAD_COGS_DIRECTORY :
                                                  HTTP_DOWNLOAD_DIRECTORY);
    if (!result) {
        const int opened = open_part(download);
        if (opened < 0) result = opened;
    }
    if (result) fail(download, result);
    return result;
}

int http_download_begin(const http_status_t *source, const char *preferred)
{
    if (active(&cogs_download)) return HTTP_E_BUSY;
    return begin(&net_download, source, preferred, 0U);
}

int http_download_begin_cogs(const http_status_t *source, const char *preferred,
                             uint32_t expected_bytes)
{
    if (http_download_active()) return HTTP_E_BUSY;
    memset(&cogs_download, 0, sizeof(cogs_download));
    const int result = begin(&cogs_download, source, preferred, expected_bytes);
    if (result && cogs_download.status.state == HTTP_DL_NONE) fail(&cogs_download, result);
    return result;
}

static int commit(staged_download_t *download)
{
    if (download->phase != 2) {
        if (f_sync(&download->file) != FR_OK) return HTTP_DL_E_SYNC;
        FRESULT closed = f_close(&download->file);
        if (closed == FR_OK || closed == FR_INVALID_OBJECT) download->open = 0;
        if (closed != FR_OK) return HTTP_DL_E_CLOSE;
        download->phase = 2;
    }
    if (download->suffix >= COLLISION_LIMIT) return HTTP_DL_E_NAMES;
    if (final_name(download)) return HTTP_DL_E_ARGUMENT;
    FRESULT result = f_rename(download->part, download->final);
    if (result == FR_EXIST) {
        ++download->suffix;
        return 0;
    }
    if (result != FR_OK) return HTTP_DL_E_RENAME;
    download->owns_part = 0;
    if (!http_service_release_owned(stream_owner(download))) download->owns_stream = 0;
    (void)cleanup(download);
    download->status.state = HTTP_DL_DONE;
    download->status.result = 0;
    return 1;
}

static void poll(staged_download_t *download)
{
    if (download->status.state != HTTP_DL_WRITING) return;
    const uint8_t owner = stream_owner(download);
    if (http_service_getowner() != owner) {
        fail(download, HTTP_DL_E_OWNER);
        return;
    }
    http_service_status(&source_status);
    if (source_status.state == HTTP_S_FAILED) {
        fail(download, source_status.result);
        return;
    }
    if (download->phase == 2) {
        const int result = commit(download);
        if (result < 0) fail(download, result);
        return;
    }
    if (!download->open) {
        const int opened = open_part(download);
        if (opened < 0) fail(download, opened);
        if (opened <= 0) return;
    }
    size_t length = http_service_read_owned(owner, buffer, sizeof(buffer));
    if (length) {
        if (length > UINT32_MAX - download->status.written) {
            fail(download, HTTP_DL_E_LIMIT);
            return;
        }
        if (download->status.total_known && length > download->status.total - download->status.written) {
            fail(download, HTTP_DL_E_LENGTH);
            return;
        }
        UINT wrote = 0;
        if (f_write(&download->file, buffer, (UINT)length, &wrote) != FR_OK || wrote != length) {
            fail(download, HTTP_DL_E_WRITE);
            return;
        }
        download->status.written += (uint32_t)length;
    }
    http_service_status(&source_status);
    if (source_status.state == HTTP_S_DONE && !source_status.buffered) {
        if (source_status.status < 200 || source_status.status >= 300 ||
            (download->status.total_known && download->status.written != download->status.total)) {
            fail(download, HTTP_DL_E_LENGTH);
            return;
        }
        const int result = commit(download);
        if (result < 0) fail(download, result);
    }
}

void http_download_poll(void) { poll(&net_download); }
void http_download_cogs_poll(void)
{
    if (cogs_download.status.state == HTTP_DL_WRITING) poll(&cogs_download);
    else (void)cleanup(&cogs_download);
}
void http_download_cogs_cancel(void) { cancel(&cogs_download); }
int http_download_cogs_active(void) { return active(&cogs_download); }
void http_download_cogs_status(http_download_status_t *out) { *out = cogs_download.status; }
const char *http_download_cogs_path(void)
{
    return cogs_download.status.state == HTTP_DL_DONE ? cogs_download.final : "";
}
#endif
