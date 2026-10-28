#ifndef HTTP_CORE_H
#define HTTP_CORE_H
#include <stddef.h>
#include <stdint.h>

#define HTTP_URL_MAX 1024U
#define HTTP_HOST_MAX 254U
#define HTTP_LINE_MAX 4096U
#define HTTP_HEADER_MAX 16384U
#define HTTP_RETAIN_MAX 32768U

enum { HTTP_MORE = 0, HTTP_COMPLETE = 1, HTTP_PAUSED = 2,
       HTTP_FORMAT = -1, HTTP_LIMIT = -2, HTTP_UNSUPPORTED = -3 };
typedef struct {
    char host[HTTP_HOST_MAX], path[HTTP_URL_MAX];
    uint16_t port;
    uint8_t tls;
} http_url_t;
typedef int (*http_body_sink_t)(void *, const uint8_t *, size_t);
typedef struct {
    char line[HTTP_LINE_MAX], location[HTTP_URL_MAX], content_type[128];
    uint32_t body_bytes, body_limit, remaining, chunk_remaining, header_bytes;
    uint16_t status, line_length;
    uint8_t state, has_length, chunked, head, interim, saw_cr, done;
} http_parser_t;

int http_url_parse(const char *text, http_url_t *url);
int http_url_resolve(const char *base, const char *location, char *out, size_t cap);
int http_request_build(const http_url_t *url, const char *method,
                       const char *headers, size_t body_length,
                       int strip_credentials, char *out, size_t cap);
void http_parser_init(http_parser_t *parser, int head, uint32_t limit);
int http_parser_feed(http_parser_t *parser, const uint8_t *data, size_t length,
                     size_t *used, http_body_sink_t sink, void *ctx);
int http_parser_eof(http_parser_t *parser);
/* Pure helpers shared by the board service and native failure tests. */
int http_dns_query(const char *host, uint16_t id, uint8_t *out, size_t cap);
int http_dns_answer(const uint8_t *packet, size_t length, uint16_t id,
                    const char *host, uint8_t address[4]);
int http_rtc_epoch(unsigned year, unsigned month, unsigned day,
                    unsigned hour, unsigned minute, unsigned second,
                    int utc_offset_minutes, uint32_t *epoch);
#endif
