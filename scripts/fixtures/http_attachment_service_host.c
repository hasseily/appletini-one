/* Real HTTP engine/parser/ownership; only clock, SD and NIC are modeled. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define COMMON_H
static uint32_t slot_mask;
#define REG_READ(address) ((void)(address), slot_mask)
#define REG_WRITE(address, value) ((void)(address), slot_mask = (value))
#include "../../ps_sources/frontend/http_service.c"

uint64_t host_time;
static uint8_t incoming[16384], socket_state;
static uint16_t incoming_size, incoming_at;
static unsigned network_receives;

void uart_puts(uint32_t uart, const char *text) { (void)uart; (void)text; }
uint8_t ftp_sd_service_active(void) { return 0; }
int usb_sdd_service_active(void) { return 0; }
void net_service_reset(void) {}
uint32_t net_service_json_max_us(void) { return 0; }
int http_entropy_seed(uint8_t out[HTTP_SEED_BYTES]) { (void)out; return -1; }
void http_entropy_status(http_entropy_stats_t *out) { memset(out, 0, sizeof(*out)); }
const char *http_entropy_result_name(int value) { (void)value; return "fixture"; }
uint16_t uthernet2_w5100_socket_reg(uint8_t socket, uint16_t offset)
{ return (uint16_t)(0x400U + socket * 0x100U + offset); }
int uthernet2_read_reg(uint16_t address, uint8_t *out) { (void)address; *out = 0; return 0; }
int uthernet2_write_reg(uint16_t address, uint8_t value) { (void)address; (void)value; return 0; }
int uthernet2_w5100_set_rx_layout(uint8_t update_only) { (void)update_only; return 0; }
int uthernet2_w5100_socket_command(uint8_t socket, uint8_t command) { (void)socket; (void)command; return 0; }
int uthernet2_w5100_socket_status(uint8_t socket, uint8_t *out) { (void)socket; *out = socket_state; return 0; }
int uthernet2_w5100_read16_stable(uint16_t address, uint16_t *out)
{ assert((address & 255U) == W5100_SN_RX_RSR); *out = incoming_size - incoming_at; return 0; }
int uthernet2_w5100_read16(uint16_t address, uint16_t *out)
{ assert((address & 255U) == W5100_SN_RX_RD); *out = incoming_at; return 0; }
int uthernet2_w5100_write16(uint16_t address, uint16_t value)
{ if ((address & 255U) == W5100_SN_RX_RD) incoming_at = value; return 0; }
int uthernet2_read_block(uint16_t a, uint8_t *out, uint16_t n) { (void)a; memset(out, 0, n); return 0; }
int uthernet2_write_block(uint16_t a, const uint8_t *p, uint16_t n) { (void)a; (void)p; (void)n; return 0; }
int uthernet2_w5100_socket_ring_read(uint8_t socket, uint16_t address, uint8_t *out, uint16_t length)
{ assert(socket == SOCKET_TCP && address + length <= incoming_size); memcpy(out, incoming + address, length); ++network_receives; return 0; }
int uthernet2_w5100_socket_ring_write(uint8_t socket, uint16_t address, const uint8_t *p, uint16_t n)
{ (void)socket; (void)address; (void)p; (void)n; return 0; }
int uthernet2_read_network_config(uthernet2_network_config_t *out) { memset(out, 0, sizeof(*out)); return 0; }
int uthernet2_test(uthernet2_test_result_t *out) { memset(out, 0, sizeof(*out)); return 0; }

static void begin(int retain)
{
    memset(&g_http, 0, sizeof(g_http));
    mbedtls_ssl_init(&g_http.ssl);
    host_time = 1000000; incoming_size = incoming_at = 0; network_receives = 0;
    socket_state = W5100_SR_ESTABLISHED; storage_busy = 0; console.active = 0;
    http_request_t request = {"http://192.0.2.1/file", "GET", "", NULL, 0, (uint8_t)retain};
    assert(!http_service_start_owned(&request, HTTP_OWNER_NET));
    /* The NIC handshake is outside this fixture. Process real response RX. */
    g_http.status.state = HTTP_S_READ;
    http_parser_init(&g_http.parser, 0, retain ? HTTP_RETAIN_MAX : UINT32_MAX);
}
static void append(const void *bytes, size_t length)
{
    assert(length <= sizeof(incoming) - incoming_size);
    memcpy(incoming + incoming_size, bytes, length); incoming_size += (uint16_t)length;
}
static void header(unsigned status, const char *extra, uint32_t length)
{
    char text[1024];
    int n = snprintf(text, sizeof(text), "HTTP/1.1 %u Response\r\n%sContent-Length: %lu\r\n\r\n",
                     status, extra, (unsigned long)length);
    assert(n > 0 && (size_t)n < sizeof(text)); append(text, (size_t)n);
}
static void poll_until_headers(void)
{
    for (unsigned i = 0; !g_http.parser.headers_ready && i != 20U; ++i) http_service_poll();
    assert(g_http.parser.headers_ready);
}
static void finish_body(void)
{
    for (unsigned i = 0; http_service_active() && i != 100U; ++i) http_service_poll();
    assert(g_http.status.state == HTTP_S_DONE);
}
int main(void)
{
    uint8_t out[8192], payload[8192];
    http_status_t status;
    for (unsigned i = 0; i != sizeof(payload); ++i) payload[i] = (uint8_t)(i % 251U);

    begin(0);
    const char *incomplete = "HTTP/1.1 200 OK\r\nContent-Disposition: attachment; filename=disk.dsk\r\n";
    append(incomplete, strlen(incomplete));
    http_service_poll(); http_service_status(&status);
    assert(g_http.parser.attachment && !g_http.parser.headers_ready);
    assert(!status.headers_ready && !status.attachment && !status.suggested_filename[0]);
    assert(http_service_handoff_download() == HTTP_E_ARGUMENT);
    assert(!http_service_cancel_owned(HTTP_OWNER_NET));

    /* Headers and a binary prefix may arrive in the same read. Handoff
     * transfers the existing ring and parser; it does not restart HTTP. */
    begin(0); header(200, "Content-Disposition: attachment; filename=disk.dsk\r\n", sizeof(payload));
    append(payload, 200); poll_until_headers(); http_service_status(&status);
    assert(status.attachment && status.content_length_known && status.content_length == sizeof(payload));
    assert(!strcmp(status.suggested_filename, "disk.dsk") && status.buffered == 200);
    uint64_t started = g_http.started;
    assert(!http_service_handoff_download() && http_service_getowner() == HTTP_OWNER_DOWNLOAD);
    assert(g_http.started == started && g_http.parser.body_bytes == 200);
    assert(!http_service_read(out, sizeof(out)) && !http_service_read_owned(HTTP_OWNER_NET, out, sizeof(out)));
    http_service_cancel(); assert(http_service_active());
    http_request_t another = {"http://192.0.2.2/other", "GET", "", NULL, 0, 0};
    assert(http_service_start(&another) == HTTP_E_BUSY);
    assert(http_service_cancel_owned(HTTP_OWNER_NET) == HTTP_E_BUSY);
    assert(http_service_release_owned(HTTP_OWNER_DOWNLOAD) == HTTP_E_BUSY);
    size_t copied = http_service_read_owned(HTTP_OWNER_DOWNLOAD, out, sizeof(out));
    assert(copied == 200 && !memcmp(out, payload, copied));
    append(payload + copied, sizeof(payload) - copied);
    for (unsigned i = 0; i != 100U && (http_service_active() || g_http.ring_count); ++i) {
        http_service_poll();
        copied += http_service_read_owned(HTTP_OWNER_DOWNLOAD, out + copied, sizeof(out) - copied);
    }
    assert(copied == sizeof(payload) && !memcmp(out, payload, copied));
    assert(g_http.status.state == HTTP_S_DONE && g_http.started == started && network_receives > 8);
    http_service_status(&status); assert(status.content_length == sizeof(payload));
    assert(http_service_start(&another) == HTTP_E_BUSY); /* DONE still owned. */
    assert(!http_service_release_owned(HTTP_OWNER_DOWNLOAD) && http_service_getowner() == HTTP_OWNER_NONE);

    begin(0); header(200, "", 5); append("hello", 5); finish_body();
    assert(http_service_read_owned(HTTP_OWNER_NET, out, 1) == 1);
    assert(http_service_handoff_download() == HTTP_E_ARGUMENT); /* No missing prefix allowed. */
    assert(http_service_release_owned(HTTP_OWNER_NET) == HTTP_E_BUSY);
    assert(http_service_read_owned(HTTP_OWNER_NET, out, sizeof(out)) == 4);
    assert(!http_service_release_owned(HTTP_OWNER_NET));

    begin(1); header(200, "Content-Disposition: attachment\r\n", 5); append("hello", 5); finish_body();
    assert(http_service_handoff_download() == HTTP_E_ARGUMENT);
    size_t retained_length;
    assert(http_service_retained(&retained_length) == NULL && !retained_length);
    const uint8_t *retained = http_service_retained_owned(HTTP_OWNER_NET, &retained_length);
    assert(retained && retained_length == 5 && !memcmp(retained, "hello", 5));
    assert(!http_service_cancel_owned(HTTP_OWNER_NET));

    begin(0); header(404, "Content-Disposition: attachment\r\n", 1); append("x", 1); finish_body();
    assert(http_service_handoff_download() == HTTP_E_ARGUMENT);
    assert(!http_service_cancel_owned(HTTP_OWNER_NET));

    /* A redirect's attachment metadata must never create a download. */
    begin(0);
    mbedtls_memory_buffer_alloc_init(tls_heap, sizeof(tls_heap));
    mbedtls_ctr_drbg_init(&g_http.drbg); mbedtls_ctr_drbg_set_entropy_len(&g_http.drbg, HTTP_SEED_BYTES);
    memset(g_http.entropy, 0x63, sizeof(g_http.entropy));
    assert(!mbedtls_ctr_drbg_seed(&g_http.drbg, entropy, NULL, (const uint8_t *)"redirect test", 13));
    header(302, "Location: http://192.0.2.2/final\r\nContent-Disposition: attachment; filename=wrong.dsk\r\n", 3);
    append("old", 3);
    size_t used;
    assert(http_parser_feed(&g_http.parser, incoming, incoming_size, &used, body_sink, NULL) == HTTP_COMPLETE);
    assert(g_http.parser.attachment && !g_http.ring_count);
    assert(http_service_handoff_download() == HTTP_E_ARGUMENT);
    assert(!redirect()); http_service_status(&status);
    assert(!status.headers_ready && !status.attachment && !status.suggested_filename[0]);
    incoming_size = incoming_at = 0; g_http.status.state = HTTP_S_READ;
    header(200, "Content-Type: text/html\r\n", 2); append("ok", 2); finish_body();
    http_service_status(&status); assert(!status.attachment && !status.suggested_filename[0]);
    assert(http_service_read_owned(HTTP_OWNER_NET, out, sizeof(out)) == 2 && !memcmp(out, "ok", 2));
    assert(!http_service_cancel_owned(HTTP_OWNER_NET)); crypto_reset(); mbedtls_memory_buffer_alloc_free();

    /* Repeated polls/consumer reads do not renew the inactivity timer. A
     * positive NIC RX does, and the absolute ceiling still wins. */
    begin(0); header(200, "Content-Disposition: attachment\r\n", 100); poll_until_headers();
    assert(!http_service_handoff_download());
    uint64_t idle = g_http.download_idle, total = g_http.deadline;
    host_time += 10000000; http_service_poll(); assert(g_http.download_idle == idle);
    append(payload, 1); http_service_poll(); assert(g_http.download_idle == host_time + HTTP_DOWNLOAD_IDLE_US);
    idle = g_http.download_idle;
    assert(http_service_read_owned(HTTP_OWNER_DOWNLOAD, out, sizeof(out)) == 1 && g_http.download_idle == idle);
    host_time = idle; http_service_poll(); assert(g_http.status.result == HTTP_E_TIMEOUT);
    assert(http_service_getowner() == HTTP_OWNER_DOWNLOAD);
    assert(!http_service_cancel_owned(HTTP_OWNER_DOWNLOAD));
    begin(0); header(200, "", 100); poll_until_headers(); assert(!http_service_handoff_download());
    total = g_http.deadline; g_http.download_idle = total + 100;
    host_time = total; http_service_poll(); assert(g_http.status.result == HTTP_E_TIMEOUT);
    assert(!http_service_cancel_owned(HTTP_OWNER_DOWNLOAD));

    /* Timer conversion must remain correct beyond the old multiply overflow. */
    host_time = UINT64_C(50000000000000); assert(microseconds() == host_time);
    puts("PASS production attachment handoff: metadata before body, 8KiB binary stream, no refetch/prefix loss, owner isolation through DONE, redirects, idle RX progress, total timeout and long uptime");
    return 0;
}
