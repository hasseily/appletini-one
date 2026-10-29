/* Test the production parser before any download manager sees its metadata. */
#include "http_core.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static http_parser_t parser;
static uint8_t body[1024];
static size_t body_length;
static int expected_attachment;
static const char *expected_filename;
static unsigned tests;

static int sink(void *context, const uint8_t *bytes, size_t length)
{
    (void)context;
    assert(parser.headers_ready && parser.status >= 200);
    assert(parser.attachment == expected_attachment);
    assert(!strcmp(parser.suggested_filename, expected_filename));
    assert(length <= sizeof(body) - body_length);
    memcpy(body + body_length, bytes, length); body_length += length;
    return 0;
}
static int parse(const char *wire, size_t length, size_t fragment)
{
    size_t at = 0;
    int result = HTTP_MORE;
    memset(&parser, 0xA5, sizeof(parser));
    http_parser_init(&parser, 0, sizeof(body));
    body_length = 0;
    while (at < length && result == HTTP_MORE) {
        size_t n = length - at, used;
        if (n > fragment) n = fragment;
        result = http_parser_feed(&parser, (const uint8_t *)wire + at, n, &used, sink, NULL);
        assert(used || result != HTTP_MORE); at += used;
    }
    if (result == HTTP_MORE) result = http_parser_eof(&parser);
    return result;
}
static void good(const char *disposition, const char *filename, int attachment)
{
    char wire[2048];
    int n = snprintf(wire, sizeof(wire), "HTTP/1.1 200 OK\r\nContent-Disposition: %s\r\nContent-Length: 5\r\n\r\n", disposition);
    const uint8_t binary[] = {'A', 0, 'B', 255, 'C'};
    assert(n > 0); memcpy(wire + n, binary, sizeof(binary));
    expected_attachment = attachment; expected_filename = filename;
    for (size_t step = 1; step < 200; step = step * 3U + 1U) {
        assert(parse(wire, (size_t)n + sizeof(binary), step) == HTTP_COMPLETE);
        assert(body_length == sizeof(binary) && !memcmp(body, binary, sizeof(binary)));
        assert(parser.content_length == 5 && parser.has_length && !parser.remaining);
    }
    ++tests;
}
static void bad(const char *value)
{
    char wire[2048];
    int n = snprintf(wire, sizeof(wire), "HTTP/1.1 200 OK\r\nContent-Disposition: %s\r\nContent-Length: 5\r\n\r\nABCDE", value);
    expected_attachment = 0; expected_filename = "";
    assert(n > 0);
    for (size_t step = 1; step < 200; step = step * 3U + 1U) {
        assert(parse(wire, (size_t)n, step) == HTTP_FORMAT);
        assert(!parser.headers_ready && !body_length);
    }
    ++tests;
}
int main(void)
{
    good("attachment", "", 1);
    good("ATTACHMENT ; FILENAME = \"disk image.2mg\"", "disk image.2mg", 1);
    good("attachment; filename=disk.dsk", "disk.dsk", 1);
    good("attachment; filename=\"disk;part.dsk\"; size=5", "disk;part.dsk", 1);
    good("attachment; filename=\"disk\\\"name.dsk\"", "disk\"name.dsk", 1);
    good("attachment; filename=\"disk%20image.dsk\"", "disk%20image.dsk", 1);
    good("attachment; filename=\"../advisory.dsk\"", "../advisory.dsk", 1);
    good("attachment; filename*=UTF-8''disk%20image%2B.2mg", "disk image+.2mg", 1);
    good("attachment; filename*=UTF-8'en'%E2%98%83.dsk", "\xE2\x98\x83.dsk", 1);
    good("attachment; filename=old.dsk; filename*=UTF-8''new.dsk", "new.dsk", 1);
    good("attachment; filename*=UTF-8''new.dsk; filename=old.dsk", "new.dsk", 1);
    good("attachment; filename=old.dsk; filename*=unsupported''new.dsk", "old.dsk", 1);
    good("attachment; filename*=unsupported''new.dsk; filename=old.dsk", "old.dsk", 1);
    good("inline; filename=image.png", "image.png", 0);
    good("form-data; name=field", "", 0);
    good("attachment; filename=\"\"", "", 1);
    char long_value[256];
    strcpy(long_value, "attachment; filename=");
    memset(long_value + strlen(long_value), 'a', 200); long_value[221] = '\0';
    good(long_value, "", 1); /* Drop an oversized hint; do not truncate a path. */
    bad(""); bad("attachment;"); bad("attachment; filename"); bad("attachment; filename=");
    bad("attachment; filename=\"unterminated"); bad("attachment; filename=one two");
    bad("attachment; filename=a; FILENAME=b");
    bad("attachment; filename*=UTF-8''a; filename*=UTF-8''b");
    bad("attachment; filename*=\"UTF-8''a\"");
    bad("attachment; filename*=UTF-8'a"); bad("attachment; filename*=UTF-8''bad%");
    bad("attachment; filename*=UTF-8''bad%GG"); bad("attachment; filename*=UTF-8''bad%00");
    bad("attachment; filename*=UTF-8''bad%0A"); bad("attachment; filename*=UTF-8''bad%7F");

    expected_attachment = 0; expected_filename = "";
    const char *interim = "HTTP/1.1 103 Early Hints\r\nContent-Disposition: attachment; filename=wrong.dsk\r\nContent-Length: 12\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nx";
    assert(parse(interim, strlen(interim), 1) == HTTP_COMPLETE);
    assert(!parser.attachment && !parser.suggested_filename[0] && parser.content_length == 1); ++tests;
    const char *trailers = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1\r\nx\r\n0\r\nContent-Disposition: attachment; filename=late.dsk\r\n\r\n";
    assert(parse(trailers, strlen(trailers), 1) == HTTP_COMPLETE);
    assert(!parser.has_length && !parser.attachment && !parser.suggested_filename[0]); ++tests;
    const char *duplicate = "HTTP/1.1 200 OK\r\nContent-Disposition: attachment\r\nContent-Disposition: inline\r\n\r\n";
    assert(parse(duplicate, strlen(duplicate), 2) == HTTP_FORMAT && !body_length); ++tests;
    printf("PASS %u attachment parser cases: fragmented metadata-before-body, binary bytes, filename precedence/bounds, malformed fields, interim and trailer isolation\n", tests);
    return 0;
}
