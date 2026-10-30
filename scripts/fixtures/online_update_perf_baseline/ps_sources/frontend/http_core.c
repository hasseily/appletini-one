#include "http_core.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

enum { P_STATUS, P_HEADERS, P_LENGTH, P_CLOSE, P_CHUNK_SIZE, P_CHUNK_DATA,
       P_CHUNK_CR, P_CHUNK_LF, P_TRAILERS };

static int equal(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a++) != tolower((unsigned char)*b++)) return 0;
    }
    return !*a && !*b;
}

static int token(const char *s)
{
    if (!*s) return 0;
    for (; *s; ++s)
        if (!isalnum((unsigned char)*s) && !strchr("!#$%&'*+-.^_`|~", *s)) return 0;
    return 1;
}

int http_url_parse(const char *text, http_url_t *url)
{
    const char *p, *host, *end;
    size_t n;
    unsigned long port = 0;
    memset(url, 0, sizeof(*url));
    if (!text || strlen(text) >= HTTP_URL_MAX) return HTTP_LIMIT;
    for (p = text; *p; ++p)
        if ((unsigned char)*p <= 32U || (unsigned char)*p == 127U || *p == '\\') return HTTP_FORMAT;
    if (!strncmp(text, "https://", 8)) { url->tls = 1; p = text + 8; }
    else if (!strncmp(text, "http://", 7)) p = text + 7;
    else return HTTP_UNSUPPORTED;
    host = p;
    while (*p && !strchr(":/?#", *p)) {
        if (!isalnum((unsigned char)*p) && *p != '-' && *p != '.') return HTTP_FORMAT;
        ++p;
    }
    n = (size_t)(p - host);
    if (!n || n >= sizeof(url->host)) return HTTP_LIMIT;
    memcpy(url->host, host, n);
    for (size_t i = 0; i < n; ++i) url->host[i] = (char)tolower((unsigned char)url->host[i]);
    if (*p == ':') {
        ++p;
        if (!isdigit((unsigned char)*p)) return HTTP_FORMAT;
        while (isdigit((unsigned char)*p)) {
            port = port * 10U + (unsigned)(*p++ - '0');
            if (port > 65535U) return HTTP_FORMAT;
        }
        if (!port) return HTTP_FORMAT;
    } else port = url->tls ? 443U : 80U;
    url->port = (uint16_t)port;
    if (*p && *p != '/' && *p != '?' && *p != '#') return HTTP_FORMAT;
    end = strchr(p, '#');
    if (!end) end = p + strlen(p);
    n = (size_t)(end - p);
    if (*p != '/') {
        url->path[0] = '/';
        if (n + 2U > sizeof(url->path)) return HTTP_LIMIT;
        memcpy(url->path + 1, p, n);
    } else {
        if (n + 1U > sizeof(url->path)) return HTTP_LIMIT;
        memcpy(url->path, p, n);
    }
    return 0;
}

static void remove_dot_segments(char *path)
{
    char clean[HTTP_URL_MAX];
    size_t at = 0, used = 0, end = strcspn(path, "?#");
    while (at < end) {
        size_t left = end - at;
        if ((left >= 3 && !memcmp(path + at, "../", 3)) ||
            (left >= 2 && !memcmp(path + at, "./", 2))) {
            at += path[at] == '.' && path[at + 1] == '/' ? 2U : 3U;
        } else if (left >= 3 && !memcmp(path + at, "/./", 3)) at += 2;
        else if (left == 2 && !memcmp(path + at, "/.", 2)) {
            clean[used++] = '/'; at = end;
        } else if ((left >= 4 && !memcmp(path + at, "/../", 4)) ||
                   (left == 3 && !memcmp(path + at, "/..", 3))) {
            while (used && clean[used - 1] != '/') --used;
            if (used) --used;
            at += 3;
            if (at == end) clean[used++] = '/';
        } else if ((left == 1 && path[at] == '.') ||
                   (left == 2 && !memcmp(path + at, "..", 2))) at = end;
        else {
            clean[used++] = path[at++];
            while (at < end && path[at] != '/') clean[used++] = path[at++];
        }
    }
    strcpy(clean + used, path + end);
    strcpy(path, clean);
}

int http_url_resolve(const char *base, const char *location, char *out, size_t cap)
{
    http_url_t url, check;
    char path[HTTP_URL_MAX], origin[HTTP_HOST_MAX + 24U];
    int n;
    if (http_url_parse(base, &url)) return HTTP_FORMAT;
    if (!location || !*location) return HTTP_FORMAT;
    if (!strncmp(location, "http://", 7) || !strncmp(location, "https://", 8))
        n = snprintf(out, cap, "%s", location);
    else if (!strncmp(location, "//", 2))
        n = snprintf(out, cap, "%s:%s", url.tls ? "https" : "http", location);
    else {
        if (strchr(location, ':') && (!strchr(location, '/') || strchr(location, ':') < strchr(location, '/')))
            return HTTP_UNSUPPORTED;
        if (location[0] == '/') n = snprintf(path, sizeof(path), "%s", location);
        else {
            char *query = strchr(url.path, '?');
            if (query && location[0] != '#') *query = '\0';
            if (location[0] == '?' || location[0] == '#')
                n = snprintf(path, sizeof(path), "%s%s", url.path, location);
            else {
                char *slash = strrchr(url.path, '/');
                if (slash) slash[1] = '\0';
                n = snprintf(path, sizeof(path), "%s%s", url.path, location);
            }
        }
        if (n < 0 || (size_t)n >= sizeof(path)) return HTTP_LIMIT;
        n = snprintf(origin, sizeof(origin), "%s://%s:%u", url.tls ? "https" : "http", url.host, url.port);
        if (n < 0 || (size_t)n >= sizeof(origin)) return HTTP_LIMIT;
        n = snprintf(out, cap, "%s%s", origin, path);
    }
    if (n < 0 || (size_t)n >= cap) return HTTP_LIMIT;
    if (http_url_parse(out, &check)) return HTTP_FORMAT;
    char *authority = strstr(out, "://");
    char *target = authority ? strpbrk(authority + 3, "/?#") : NULL;
    if (target && *target == '/') remove_dot_segments(target);
    return http_url_parse(out, &check);
}

int http_request_build(const http_url_t *url, const char *method,
                       const char *headers, size_t body_length,
                       int strip_credentials, char *out, size_t cap)
{
    size_t used;
    int n, user_agent = 0;
    if (!token(method)) return HTTP_FORMAT;
    n = snprintf(out, cap, "%s %s HTTP/1.1\r\nHost: %s:%u\r\n"
                 "Connection: close\r\nAccept-Encoding: identity\r\n"
                 "Content-Length: %lu\r\n",
                 method, url->path, url->host, url->port, (unsigned long)body_length);
    if (n < 0 || (size_t)n >= cap) return HTTP_LIMIT;
    used = (size_t)n;
    while (headers && *headers) {
        const char *end = strchr(headers, '\n');
        char name[128];
        size_t length = end ? (size_t)(end - headers) : strlen(headers);
        const char *colon;
        if (length && headers[length - 1U] == '\r') --length;
        if (!length) return HTTP_FORMAT;
        colon = memchr(headers, ':', length);
        if (!colon || (size_t)(colon - headers) >= sizeof(name)) return HTTP_FORMAT;
        memcpy(name, headers, (size_t)(colon - headers)); name[colon - headers] = '\0';
        if (!token(name)) return HTTP_FORMAT;
        for (size_t i = 0; i < length; ++i)
            if (((unsigned char)headers[i] < 32U && headers[i] != '\t') || headers[i] == 127) return HTTP_FORMAT;
        if (equal(name, "Host") || equal(name, "Content-Length") || equal(name, "Transfer-Encoding") ||
            equal(name, "Connection") || equal(name, "Accept-Encoding")) return HTTP_FORMAT;
        /* A caller may select its browser identity. Never send two UAs. */
        if (equal(name, "User-Agent") && user_agent++) return HTTP_FORMAT;
        if (!(strip_credentials && (equal(name, "Authorization") || equal(name, "Cookie") ||
                                   equal(name, "Proxy-Authorization")))) {
            if (length + 2U >= cap - used) return HTTP_LIMIT;
            memcpy(out + used, headers, length); used += length;
            memcpy(out + used, "\r\n", 2); used += 2;
        }
        headers = end ? end + 1 : headers + length;
    }
    if (!user_agent) {
        static const char default_agent[] = "User-Agent: Appletini-HTTP/1\r\n";
        if (sizeof(default_agent) + 2U > cap - used) return HTTP_LIMIT;
        memcpy(out + used, default_agent, sizeof(default_agent) - 1U);
        used += sizeof(default_agent) - 1U;
    }
    if (cap - used < 3U) return HTTP_LIMIT;
    memcpy(out + used, "\r\n", 3);
    return (int)(used + 2U);
}

void http_parser_init(http_parser_t *p, int head, uint32_t limit)
{
    memset(p, 0, sizeof(*p));
    p->head = head != 0; p->body_limit = limit;
}

static char *parameter_token_end(char *value)
{
    while (*value && (isalnum((unsigned char)*value) || strchr("!#$%&'*+-.^_`|~", *value))) ++value;
    return value;
}
static int hex_digit(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = (unsigned char)tolower(c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}
/* RFC 8187 UTF-8 extended value. Unsupported charsets leave the ordinary
 * filename as fallback. Never decode percent escapes in a plain filename. */
static int extended_filename(char *value, char out[HTTP_FILENAME_MAX])
{
    char *language = strchr(value, '\''), *bytes;
    size_t n = 0;
    int overflow = 0;
    if (!language) return HTTP_FORMAT;
    *language++ = '\0';
    bytes = strchr(language, '\'');
    if (!bytes) return HTTP_FORMAT;
    *bytes++ = '\0';
    if (!equal(value, "UTF-8")) return 0;
    while (*bytes) {
        unsigned char c = (unsigned char)*bytes++;
        if (c == '%') {
            int high, low;
            if (!bytes[0] || !bytes[1] || (high = hex_digit((unsigned char)bytes[0])) < 0 ||
                (low = hex_digit((unsigned char)bytes[1])) < 0) return HTTP_FORMAT;
            c = (unsigned char)(high * 16 + low); bytes += 2;
        } else if (c == '\'' || c == '*' || c >= 128U) return HTTP_FORMAT;
        if (c < 32U || c == 127U) return HTTP_FORMAT;
        if (n + 1U < HTTP_FILENAME_MAX) out[n++] = (char)c;
        else overflow = 1;
    }
    out[overflow ? 0U : n] = '\0';
    return 1;
}
static int content_disposition(http_parser_t *p, char *value)
{
    char *end = parameter_token_end(value), saved;
    uint8_t filename_seen = 0, extended_seen = 0, extended_valid = 0;
    if (p->disposition_seen || end == value) return HTTP_FORMAT;
    p->disposition_seen = 1;
    saved = *end; *end = '\0'; p->attachment = (uint8_t)equal(value, "attachment"); *end = saved;
    value = end;
    for (;;) {
        char *name, *name_end, *first, *last, *next;
        uint8_t filename, extended, quoted;
        while (*value == ' ' || *value == '\t') ++value;
        if (!*value) return 0;
        if (*value++ != ';') return HTTP_FORMAT;
        while (*value == ' ' || *value == '\t') ++value;
        name = value; name_end = parameter_token_end(name);
        if (name_end == name) return HTTP_FORMAT;
        saved = *name_end; *name_end = '\0';
        filename = (uint8_t)equal(name, "filename");
        extended = (uint8_t)equal(name, "filename*");
        *name_end = saved; value = name_end;
        while (*value == ' ' || *value == '\t') ++value;
        if (*value++ != '=') return HTTP_FORMAT;
        while (*value == ' ' || *value == '\t') ++value;
        quoted = (uint8_t)(*value == '"');
        if (quoted) {
            first = ++value; last = first;
            while (*value && *value != '"') {
                if (*value == '\\' && !*++value) return HTTP_FORMAT;
                *last++ = *value++;
            }
            if (*value++ != '"') return HTTP_FORMAT;
        } else {
            first = value; last = parameter_token_end(first); value = last;
            if (last == first) return HTTP_FORMAT;
        }
        next = value;
        while (*next == ' ' || *next == '\t') ++next;
        if (*next && *next != ';') return HTTP_FORMAT;
        saved = *last; *last = '\0';
        if (filename) {
            if (filename_seen++) return HTTP_FORMAT;
            if (!extended_valid) {
                size_t length = (size_t)(last - first);
                if (length < sizeof(p->suggested_filename)) memcpy(p->suggested_filename, first, length + 1U);
                else p->suggested_filename[0] = '\0';
            }
        } else if (extended) {
            int result;
            if (extended_seen++ || quoted) return HTTP_FORMAT;
            result = extended_filename(first, p->suggested_filename);
            if (result < 0) return result;
            extended_valid = (uint8_t)result;
        }
        *last = saved;
        value = next;
    }
}

static int response_line(http_parser_t *p)
{
    char *colon, *value, *end;
    if (p->state == P_STATUS) {
        if (strlen(p->line) < 12U || (strncmp(p->line, "HTTP/1.1 ", 9) && strncmp(p->line, "HTTP/1.0 ", 9)) ||
            !isdigit((unsigned char)p->line[9]) || !isdigit((unsigned char)p->line[10]) ||
            !isdigit((unsigned char)p->line[11]) || (p->line[12] && p->line[12] != ' ')) return HTTP_FORMAT;
        p->status = (uint16_t)((p->line[9] - '0') * 100 + (p->line[10] - '0') * 10 + p->line[11] - '0');
        if (p->status < 100 || p->status > 599 || p->status == 101) return HTTP_UNSUPPORTED;
        p->state = P_HEADERS; return 0;
    }
    if (p->state == P_CHUNK_SIZE) {
        uint32_t size = 0;
        unsigned count = 0;
        for (value = p->line; *value && *value != ';'; ++value) {
            unsigned digit;
            if (*value >= '0' && *value <= '9') digit = (unsigned)(*value - '0');
            else if (*value >= 'a' && *value <= 'f') digit = (unsigned)(*value - 'a') + 10U;
            else if (*value >= 'A' && *value <= 'F') digit = (unsigned)(*value - 'A') + 10U;
            else return HTTP_FORMAT;
            if (++count > 8U || size > (UINT32_MAX - digit) / 16U) return HTTP_LIMIT;
            size = size * 16U + digit;
        }
        if (!count || size > p->body_limit - p->body_bytes) return HTTP_LIMIT;
        p->chunk_remaining = size; p->state = size ? P_CHUNK_DATA : P_TRAILERS;
        p->header_bytes = 0;
        return 0;
    }
    if (!p->line[0]) {
        if (p->state == P_TRAILERS) { p->done = 1; return HTTP_COMPLETE; }
        if (p->status < 200) {
            if (++p->interim > 4U) return HTTP_LIMIT;
            p->state = P_STATUS; p->header_bytes = 0;
            p->has_length = p->chunked = p->headers_ready = p->attachment = p->disposition_seen = 0;
            p->remaining = p->content_length = 0;
            p->location[0] = p->content_type[0] = p->suggested_filename[0] = '\0';
            return 0;
        }
        if (p->chunked && p->has_length) return HTTP_FORMAT;
        if (p->head || p->status == 204 || p->status == 304 || (p->has_length && !p->remaining)) {
            p->headers_ready = p->done = 1; return HTTP_COMPLETE;
        }
        if (p->has_length && p->remaining > p->body_limit) return HTTP_LIMIT;
        p->headers_ready = 1;
        p->state = p->chunked ? P_CHUNK_SIZE : p->has_length ? P_LENGTH : P_CLOSE;
        return 0;
    }
    colon = strchr(p->line, ':');
    if (!colon) return HTTP_FORMAT;
    *colon = '\0';
    if (!token(p->line)) return HTTP_FORMAT;
    value = colon + 1;
    while (*value == ' ' || *value == '\t') ++value;
    end = value + strlen(value);
    while (end > value && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
    if (p->state == P_TRAILERS) {
        if (equal(p->line, "Content-Length") || equal(p->line, "Transfer-Encoding")) return HTTP_FORMAT;
        return 0;
    }
    if (equal(p->line, "Content-Length")) {
        uint32_t size = 0;
        if (!*value) return HTTP_FORMAT;
        for (; *value; ++value) {
            unsigned digit = (unsigned)(*value - '0');
            if (digit > 9U || size > (UINT32_MAX - digit) / 10U) return HTTP_FORMAT;
            size = size * 10U + digit;
        }
        if (p->has_length && size != p->remaining) return HTTP_FORMAT;
        p->remaining = p->content_length = size; p->has_length = 1;
    } else if (equal(p->line, "Transfer-Encoding")) {
        if (p->chunked || !equal(value, "chunked")) return HTTP_UNSUPPORTED;
        p->chunked = 1;
    } else if (equal(p->line, "Content-Encoding") && !equal(value, "identity")) return HTTP_UNSUPPORTED;
    else if (equal(p->line, "Content-Type")) {
        if (strlen(value) >= sizeof(p->content_type)) return HTTP_LIMIT;
        if (p->content_type[0] && strcmp(p->content_type, value)) return HTTP_FORMAT;
        strcpy(p->content_type, value);
    } else if (equal(p->line, "Location")) {
        if (strlen(value) >= sizeof(p->location)) return HTTP_LIMIT;
        strcpy(p->location, value);
    } else if (equal(p->line, "Content-Disposition")) {
        return content_disposition(p, value);
    }
    return 0;
}

int http_parser_feed(http_parser_t *p, const uint8_t *data, size_t length,
                     size_t *used, http_body_sink_t sink, void *ctx)
{
    *used = 0;
    while (*used < length && !p->done) {
        size_t n = length - *used;
        int result;
        if (p->state == P_LENGTH || p->state == P_CLOSE || p->state == P_CHUNK_DATA) {
            if (p->state == P_LENGTH && n > p->remaining) n = p->remaining;
            if (p->state == P_CHUNK_DATA && n > p->chunk_remaining) n = p->chunk_remaining;
            if (n > p->body_limit - p->body_bytes) return HTTP_LIMIT;
            result = sink(ctx, data + *used, n);
            if (result) return result > 0 ? HTTP_PAUSED : HTTP_LIMIT;
            *used += n; p->body_bytes += (uint32_t)n;
            if (p->state == P_LENGTH && !(p->remaining -= (uint32_t)n)) p->done = 1;
            if (p->state == P_CHUNK_DATA && !(p->chunk_remaining -= (uint32_t)n)) p->state = P_CHUNK_CR;
            continue;
        }
        uint8_t c = data[(*used)++];
        if (p->state == P_CHUNK_CR) { if (c != '\r') return HTTP_FORMAT; p->state = P_CHUNK_LF; continue; }
        if (p->state == P_CHUNK_LF) { if (c != '\n') return HTTP_FORMAT; p->state = P_CHUNK_SIZE; continue; }
        if (++p->header_bytes > HTTP_HEADER_MAX) return HTTP_LIMIT;
        if (p->saw_cr) {
            if (c != '\n') return HTTP_FORMAT;
            p->line[p->line_length] = '\0'; p->saw_cr = 0;
            result = response_line(p); p->line_length = 0;
            if (result) return result;
        } else if (c == '\r') p->saw_cr = 1;
        else {
            if (c == '\n' || c == 0 || (c < 32U && c != '\t') || c == 127U) return HTTP_FORMAT;
            if (p->line_length + 1U >= sizeof(p->line)) return HTTP_LIMIT;
            p->line[p->line_length++] = (char)c;
        }
    }
    return p->done ? HTTP_COMPLETE : HTTP_MORE;
}

int http_parser_eof(http_parser_t *p)
{
    if (p->state == P_CLOSE) p->done = 1;
    return p->done ? HTTP_COMPLETE : HTTP_FORMAT;
}

static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

int http_dns_query(const char *host, uint16_t id, uint8_t *out, size_t cap)
{
    size_t at = 12;
    if (cap < 18U || !host || !*host) return HTTP_FORMAT;
    memset(out, 0, 12); out[0] = (uint8_t)(id >> 8); out[1] = (uint8_t)id;
    out[2] = 1; out[5] = 1;
    while (*host) {
        const char *dot = strchr(host, '.');
        size_t n = dot ? (size_t)(dot - host) : strlen(host);
        if (!n || n > 63U || at + n + 6U > cap) return HTTP_LIMIT;
        out[at++] = (uint8_t)n; memcpy(out + at, host, n); at += n;
        host += n; if (*host == '.') ++host;
    }
    out[at++] = 0; out[at++] = 0; out[at++] = 1; out[at++] = 0; out[at++] = 1;
    return (int)at;
}

static int dns_name(const uint8_t *p, size_t len, size_t *at, char *out, size_t cap)
{
    size_t pos = *at, used = 0, resume = 0;
    for (unsigned step = 0; step < 128U; ++step) {
        if (pos >= len) return HTTP_FORMAT;
        unsigned n = p[pos++];
        if ((n & 0xC0U) == 0xC0U) {
            if (pos >= len) return HTTP_FORMAT;
            if (!resume) resume = pos + 1U;
            pos = ((n & 63U) << 8) | p[pos]; continue;
        }
        if (n > 63U || pos + n > len || used + n + 1U >= cap) return HTTP_FORMAT;
        if (!n) { out[used] = '\0'; *at = resume ? resume : pos; return 0; }
        if (used) out[used++] = '.';
        for (unsigned i = 0; i < n; ++i) out[used++] = (char)tolower(p[pos++]);
    }
    return HTTP_FORMAT;
}

int http_dns_answer(const uint8_t *p, size_t len, uint16_t id,
                    const char *host, uint8_t address[4])
{
    char name[HTTP_HOST_MAX], target[HTTP_HOST_MAX];
    size_t at = 12;
    uint16_t answers;
    if (len < 12U || be16(p) != id || (p[2] & 0xFAU) != 0x80U || (p[3] & 15U) || be16(p + 4) != 1U)
        return HTTP_FORMAT;
    if (dns_name(p, len, &at, name, sizeof(name)) || !equal(name, host) || at + 4U > len ||
        be16(p + at) != 1U || be16(p + at + 2U) != 1U) return HTTP_FORMAT;
    at += 4; answers = be16(p + 6); strcpy(target, host);
    if (answers > 64U) return HTTP_LIMIT;
    for (unsigned i = 0; i < answers; ++i) {
        if (dns_name(p, len, &at, name, sizeof(name)) || at + 10U > len) return HTTP_FORMAT;
        uint16_t type = be16(p + at), cls = be16(p + at + 2), size = be16(p + at + 8);
        at += 10;
        if (at + size > len) return HTTP_FORMAT;
        if (cls == 1U && equal(name, target)) {
            if (type == 1U && size == 4U) { memcpy(address, p + at, 4); return 0; }
            if (type == 5U) {
                size_t next = at;
                if (dns_name(p, len, &next, target, sizeof(target)) || next > at + size) return HTTP_FORMAT;
            }
        }
        at += size;
    }
    return HTTP_FORMAT;
}

int http_rtc_epoch(unsigned year, unsigned month, unsigned day,
                    unsigned hour, unsigned minute, unsigned second,
                    int offset, uint32_t *epoch)
{
    static const unsigned days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    uint64_t total = 0;
    int64_t result;
    if (year < 2024U || year > 2099U || month < 1U || month > 12U || day < 1U ||
        day > days[month - 1U] + (month == 2U && year % 4U == 0) ||
        hour > 23U || minute > 59U || second > 59U || offset < -840 || offset > 840) return HTTP_FORMAT;
    for (unsigned y = 1970; y < year; ++y) total += 365U + (y % 4U == 0);
    for (unsigned m = 1; m < month; ++m) total += days[m - 1U] + (m == 2U && year % 4U == 0);
    result = (int64_t)((total + day - 1U) * 86400U + hour * 3600U + minute * 60U + second) - offset * 60;
    if (result < 1704067200LL || result > UINT32_MAX) return HTTP_FORMAT;
    *epoch = (uint32_t)result; return 0;
}
