/* SPDX-License-Identifier: MIT */
#include "download_filename.h"
#include <string.h>

#define SOURCE_MAX 1024U

static unsigned char lower(unsigned char c)
{ return c >= 'A' && c <= 'Z' ? (unsigned char)(c + 'a' - 'A') : c; }
static int hex(unsigned char c)
{
    c = lower(c);
    if (c >= '0' && c <= '9') return c - '0';
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}
static int reserved(const char *name)
{
    static const char *const devices[] = {
        "con", "prn", "aux", "nul", "clock$", "conin$", "conout$"
    };
    char stem[10];
    size_t n = strcspn(name, ".");
    while (n && name[n - 1U] == ' ') --n;
    if (n >= sizeof(stem)) return 0;
    for (size_t i = 0; i < n; ++i) stem[i] = (char)lower((unsigned char)name[i]);
    stem[n] = 0;
    for (size_t i = 0; i < sizeof(devices) / sizeof(devices[0]); ++i)
        if (!strcmp(stem, devices[i])) return 1;
    return n == 4U && stem[3] >= '1' && stem[3] <= '9' &&
           ((!memcmp(stem, "com", 3)) || (!memcmp(stem, "lpt", 3)));
}
static unsigned char safe(unsigned char c)
{
    if (c < 32U || c >= 127U || strchr("<>:\"/\\|?*", c)) return '_';
    return c;
}
int download_filename(const char *preferred, const char *final_url,
                      char *out, size_t capacity)
{
    static const char fallback[] = "download.bin";
    char work[SOURCE_MAX + 1U];
    const char *start, *end;
    size_t n = 0, limit, length, extension = 0;
    int from_url = !(preferred && *preferred);
    if (out && capacity) out[0] = 0;
    if (!out || capacity < sizeof(fallback)) return -1;
    limit = capacity - 1U;
    if (limit > DOWNLOAD_FILENAME_MAX) limit = DOWNLOAD_FILENAME_MAX;
    if (!from_url) {
        start = preferred; end = start + strlen(start);
        for (const char *p = start; p < end; ++p)
            if (*p == '/' || *p == '\\') start = p + 1;
    } else {
        if (!final_url) final_url = "";
        end = final_url + strcspn(final_url, "?#");
        const char *scheme = strstr(final_url, "://");
        start = scheme && scheme < end ? scheme + 3 : final_url;
        int path = !scheme;
        for (const char *p = start; p < end; ++p)
            if (*p == '/') { start = p + 1; path = 1; }
        if (!path) start = end;
    }
    if ((size_t)(end - start) > SOURCE_MAX) return -1;
    while (start < end) {
        unsigned char c = (unsigned char)*start++;
        if (from_url && c == '%' && end - start >= 2) {
            int a = hex((unsigned char)start[0]), b = hex((unsigned char)start[1]);
            if (a >= 0 && b >= 0) { c = (unsigned char)((a << 4) | b); start += 2; }
        }
        work[n++] = (char)safe(c);
    }
    while (n && (work[n - 1U] == '.' || work[n - 1U] == ' ')) --n;
    work[n] = 0; start = work;
    while (*start == '.' || *start == ' ') ++start;
    length = strlen(start);
    if (!length) { start = fallback; length = sizeof(fallback) - 1U; }
    if (length > limit) {
        const char *dot = strrchr(start, '.');
        if (dot && dot != start) {
            extension = strlen(dot);
            if (extension < 2U || extension > 16U || extension >= limit)
                extension = 0;
        }
        memcpy(out, start, limit - extension);
        if (extension) memcpy(out + limit - extension, start + length - extension, extension);
        length = limit;
    } else memcpy(out, start, length);
    while (length && (out[length - 1U] == '.' || out[length - 1U] == ' ')) --length;
    out[length] = 0;
    if (reserved(out)) {
        if (length == limit) --length;
        memmove(out + 1, out, length);
        out[0] = '_'; out[++length] = 0;
    }
    return 0;
}
