#include "http_core.h"
#include "http_seed.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t output[65536];
static size_t output_length;
static int paused;
static int sink(void *ctx, const uint8_t *data, size_t n)
{
    (void)ctx;
    if (paused) { paused = 0; return 1; }
    assert(n <= sizeof(output) - output_length);
    memcpy(output + output_length, data, n); output_length += n;
    return 0;
}

static int decode(const char *wire, size_t slice, uint32_t limit, int eof, int head)
{
    http_parser_t p;
    size_t at = 0, length = strlen(wire);
    int result = 0;
    output_length = 0;
    http_parser_init(&p, head, limit);
    while (at < length && result >= 0 && result != HTTP_COMPLETE) {
        size_t n = length - at, used;
        if (n > slice) n = slice;
        result = http_parser_feed(&p, (const uint8_t *)wire + at, n, &used, sink, NULL);
        at += used;
        assert(used || result != HTTP_MORE);
    }
    if (result >= 0 && result != HTTP_COMPLETE && eof) result = http_parser_eof(&p);
    return result;
}

static void response_tests(void)
{
    const char *fixed = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhelloPIPELINED";
    const char *chunked = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                          "2;ext=yes\r\nhe\r\n3\r\nllo\r\n0\r\nX-Trailer: ok\r\n\r\n";
    for (size_t step = 1; step < 100; ++step) {
        paused = 1;
        assert(decode(fixed, step, 32768, 1, 0) == HTTP_COMPLETE);
        assert(output_length == 5 && !memcmp(output, "hello", 5));
        paused = 1;
        assert(decode(chunked, step, 32768, 1, 0) == HTTP_COMPLETE);
        assert(output_length == 5 && !memcmp(output, "hello", 5));
        assert(decode("HTTP/1.0 200 OK\r\n\r\nhello", step, 10, 1, 0) == HTTP_COMPLETE);
        assert(output_length == 5);
        assert(decode("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nx",
                      step, 10, 1, 0) == HTTP_COMPLETE);
        assert(output_length == 1 && output[0] == 'x');
    }
    assert(decode("HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\nhello", 3, 10, 1, 0) == HTTP_FORMAT);
    assert(decode("HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\nhello", 3, 5, 1, 0) == HTTP_LIMIT);
    assert(decode("HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nx", 1, 10, 1, 0) == HTTP_FORMAT);
    assert(decode("HTTP/1.1 200 OK\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n", 3, 10, 1, 0) == HTTP_FORMAT);
    assert(decode("HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\n\r\n", 10, 10, 1, 0) == HTTP_UNSUPPORTED);
    assert(decode("HTTP/1.1 200 OK\n\n", 1, 10, 1, 0) == HTTP_FORMAT);
    assert(decode("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1\r\nx!\n", 1, 10, 1, 0) == HTTP_FORMAT);
    assert(decode("HTTP/1.1 204 No Content\r\n\r\n", 1, 10, 1, 0) == HTTP_COMPLETE);
    assert(decode("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n", 1, 10, 1, 1) == HTTP_COMPLETE);
    assert(decode("HTTP/1.1 101 Switching Protocols\r\n\r\n", 1, 10, 1, 0) == HTTP_UNSUPPORTED);
}

static void url_request_tests(void)
{
    http_url_t u;
    char text[4096];
    assert(!http_url_parse("https://EXAMPLE.test:8443/path?q=x#ignored", &u));
    assert(u.tls && u.port == 8443 && !strcmp(u.host, "example.test") && !strcmp(u.path, "/path?q=x"));
    assert(!http_url_parse("http://example.test?q=1", &u) && !strcmp(u.path, "/?q=1"));
    assert(http_url_parse("http://user:pass@example.test/", &u) < 0);
    assert(http_url_parse("https://example.test:65536/", &u) < 0);
    assert(http_url_parse("http://example.test/\r\ninjected", &u) < 0);
    assert(!http_url_resolve("https://example.test/a/b", "/next", text, sizeof(text)));
    assert(!strcmp(text, "https://example.test:443/next"));
    assert(!http_url_resolve("https://example.test/a/b", "next", text, sizeof(text)));
    assert(!strcmp(text, "https://example.test:443/a/next"));
    assert(!http_url_resolve("https://example.test/a/b?old=1", "../next?x=/../", text, sizeof(text)));
    assert(!strcmp(text, "https://example.test:443/next?x=/../"));
    assert(!http_url_resolve("https://example.test/a/b?old=1", "#new", text, sizeof(text)));
    assert(!strcmp(text, "https://example.test:443/a/b?old=1#new"));
    assert(!http_url_resolve("https://example.test/a/b", "../../..", text, sizeof(text)));
    assert(!strcmp(text, "https://example.test:443/"));
    assert(!http_url_resolve("https://example.test/a", "//other.test/path", text, sizeof(text)));
    assert(!strcmp(text, "https://other.test/path"));
    assert(http_url_resolve("https://example.test/a", "javascript:alert(1)", text, sizeof(text)) < 0);
    assert(!http_url_parse("https://example.test/test", &u));
    assert(http_request_build(&u, "POST", "Content-Type: application/json\r\nAuthorization: Bearer secret\r\nCookie: secret\r\n",
                              2, 1, text, sizeof(text)) > 0);
    assert(strstr(text, "Content-Length: 2\r\n") && strstr(text, "application/json"));
    assert(!strstr(text, "secret"));
    assert(http_request_build(&u, "GET\r\nEVIL", "", 0, 0, text, sizeof(text)) < 0);
    assert(http_request_build(&u, "GET", "Host: attacker", 0, 0, text, sizeof(text)) < 0);
    assert(http_request_build(&u, "GET", "X-A: ok\rInjected: bad", 0, 0, text, sizeof(text)) < 0);
}

static void dns_clock_tests(void)
{
    uint8_t packet[512], address[4];
    uint32_t epoch;
    int n = http_dns_query("example.test", 0x1234, packet, sizeof(packet));
    assert(n > 12);
    packet[2] = 0x81; packet[3] = 0x80; packet[7] = 1;
    const uint8_t answer[] = {0xC0,12,0,1,0,1,0,0,0,60,0,4,10,17,1,17};
    memcpy(packet + n, answer, sizeof(answer));
    assert(!http_dns_answer(packet, (size_t)n + sizeof(answer), 0x1234, "example.test", address));
    assert(address[0] == 10 && address[3] == 17);
    assert(http_dns_answer(packet, (size_t)n + sizeof(answer), 0x1235, "example.test", address) < 0);
    packet[2] |= 2;
    assert(http_dns_answer(packet, (size_t)n + sizeof(answer), 0x1234, "example.test", address) < 0);
    packet[2] &= (uint8_t)~2U; packet[12] = 0xC0; packet[13] = 12;
    assert(http_dns_answer(packet, (size_t)n + sizeof(answer), 0x1234, "example.test", address) < 0);
    assert(!http_rtc_epoch(2024,1,1,2,0,0,120,&epoch) && epoch == 1704067200U);
    assert(!http_rtc_epoch(2024,2,29,0,0,0,0,&epoch) && epoch == 1709164800U);
    assert(http_rtc_epoch(2025,2,29,0,0,0,0,&epoch) < 0);
    assert(http_rtc_epoch(2024,1,1,0,0,0,900,&epoch) < 0);
}

static uint8_t seed_files[2][HTTP_SEED_RECORD];
static int fail_write, corrupt_write;
static int read_seed(void *ctx, unsigned slot, uint8_t *data, size_t length)
{ (void)ctx; memcpy(data,seed_files[slot],length); return 0; }
static int write_seed(void *ctx, unsigned slot, const uint8_t *data, size_t length)
{
    (void)ctx;
    if (fail_write) return -1;
    memcpy(seed_files[slot],data,length);
    if (corrupt_write) seed_files[slot][20] ^= 1;
    return 0;
}
static void seed_tests(void)
{
    uint8_t a[HTTP_SEED_BYTES],b[HTTP_SEED_BYTES],out[HTTP_SEED_BYTES];
    uint64_t generation;
    unsigned slot;
    assert(http_seed_path_private("/ATNETA.SED") && http_seed_path_private("/atnetb.sed.. "));
    assert(!http_seed_path_private("/FIRMWARE.BIN") && !http_seed_path_private("/ATNETCA.PEM"));
    memset(a,0x41,sizeof(a)); memset(b,0x42,sizeof(b));
    http_seed_encode(seed_files[0],1,a); http_seed_encode(seed_files[1],1,a);
    assert(!http_seed_select(read_seed,NULL,out,&generation,&slot));
    assert(generation==1 && slot==1 && !memcmp(out,a,sizeof(a)));
    fail_write=1;
    assert(http_seed_commit(read_seed,write_seed,NULL,slot,2,b)<0);
    fail_write=0;
    assert(!http_seed_commit(read_seed,write_seed,NULL,slot,2,b));
    assert(!http_seed_select(read_seed,NULL,out,&generation,&slot));
    assert(generation==2 && slot==0 && !memcmp(out,b,sizeof(b)));
    corrupt_write=1;
    assert(http_seed_commit(read_seed,write_seed,NULL,slot,3,a)<0);
    /* A corrupt newer slot must not fall back to an already spent seed. */
    assert(http_seed_select(read_seed,NULL,out,&generation,&slot)<0);
    for (size_t i=0;i<sizeof(out);++i) assert(out[i]==0);
    corrupt_write=0;
    http_seed_encode(seed_files[0],7,a); http_seed_encode(seed_files[1],2,b);
    assert(http_seed_select(read_seed,NULL,out,&generation,&slot)<0);
}

int main(void)
{
    response_tests(); url_request_tests(); dns_clock_tests(); seed_tests();
    puts("PASS HTTP core: fragmented bodies/backpressure, framing failures, URL/header validation, DNS identity/loops, RTC dates");
    return 0;
}
