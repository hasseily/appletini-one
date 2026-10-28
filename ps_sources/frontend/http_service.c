#ifdef APPLETINI_HTTP_SERVICE
#include "http_service.h"
#include "http_seed.h"
#include "http_bench.h"
#include "net_service.h"
#include "uthernet2_control.h"
#include "ftp_sd_service.h"
#include "card_control_regs.h"
#include "usb_sdd_service.h"
#include "../lib/common.h"
#include "../lib/uart.h"
#include "ff.h"
#include "xiltimer.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/ecp.h"
#include "mbedtls/memory_buffer_alloc.h"
#include "mbedtls/platform_time.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/sha256.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SOCKET_TCP 0U
#define SOCKET_DNS 1U
#define UDP_MODE 2U
#define UDP_READY 0x22U
#define REQUEST_MAX 4096U
#define STREAM_RING 4096U
#define SLICE_US 750U
#define SLICE_STEPS 32U
#define IO_CALLS 8U
#define IO_CHUNK 512U
#define REQUEST_TIMEOUT 30000000ULL
#define DNS_TIMEOUT 3000000ULL
#define CONNECT_RETRY_US 200000ULL

static struct {
    http_status_t status;
    http_url_t url;
    http_parser_t parser;
    char method[16], headers[2049], request[REQUEST_MAX];
    uint8_t body[2048], receive[IO_CHUNK], stream[STREAM_RING], retained[HTTP_RETAIN_MAX];
    size_t body_length, sent, request_length, rx_at, rx_length, read_at, ring_at, ring_count;
    uint8_t dns[4], peer[4], credit, sending, owns_eth, redirects, strip_credentials;
    uint8_t crypto_ready, trust_ready, entropy_used, reseed_needed;
    uint8_t entropy[HTTP_SEED_BYTES];
    uint16_t source_port, dns_id;
    uint32_t saved_mask, io_bytes;
    uint64_t started, completed, deadline, dns_deadline, connect_at, handshake_started;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config config;
    mbedtls_x509_crt ca;
    mbedtls_ctr_drbg_context drbg;
} g_http;
static uint8_t tls_heap[512U * 1024U] __attribute__((aligned(32)));
static uint8_t trust_pem[65568U] __attribute__((aligned(32)));
static const char *const seed_paths[] = {"0:/ATNETA.SED", "0:/ATNETB.SED"};
static uint32_t rtc_local;
static uint64_t rtc_at;
static uint8_t rtc_valid;
static int rtc_offset;
static int storage_busy;
static uint8_t configured_dns[4], boot_prepared;
static int prepare_error;
static uint32_t prepare_us;
/* Paint only unused foreground stack, below a guard under the current SP.
 * The untouched guard makes the later used-byte result an upper bound. */
#if defined(__arm__)
extern uint32_t _stack_end[], _stack[];
static uint8_t stack_painted;
static void stack_prepare(void)
{
    uintptr_t sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    if(stack_painted || sp<(uintptr_t)_stack_end+512U || sp>(uintptr_t)_stack)return;
    volatile uint32_t *end=(volatile uint32_t *)((sp-512U)&~3U);
    for(volatile uint32_t *p=_stack_end;p<end;++p)*p=0xA56C39E7U;
    stack_painted=1;
}
static uint32_t stack_used_upper(void)
{
    if(!stack_painted)return 0;
    volatile uint32_t *p=_stack_end;
    while(p<_stack && *p==0xA56C39E7U)++p;
    return (uint32_t)((uintptr_t)_stack-(uintptr_t)p);
}
#else
static void stack_prepare(void){}
static uint32_t stack_used_upper(void){return 0;}
#endif
static uint8_t trace_enabled;
static FATFS http_fs;
static struct { uint8_t active; uint32_t uart; mbedtls_sha256_context hash; } console;
static void trace(const char *text)
{ if (trace_enabled && console.active) { uart_puts(console.uart,"[http-stage] ");uart_puts(console.uart,text);uart_puts(console.uart,"\r\n"); } }

static uint64_t microseconds(void)
{
    XTime ticks;
    XTime_GetTime(&ticks);
    return (uint64_t)ticks * 1000000ULL / (uint64_t)(COUNTS_PER_SECOND);
}
static void maximum(uint32_t *out, uint64_t value)
{ if (value > *out) *out = value > UINT32_MAX ? UINT32_MAX : (uint32_t)value; }

mbedtls_ms_time_t mbedtls_ms_time(void)
{ return (mbedtls_ms_time_t)(microseconds() / 1000U); }

static uint32_t utc_now(void)
{
    uint64_t age = microseconds() - rtc_at;
    int64_t value = (int64_t)rtc_local - rtc_offset * 60 + (int64_t)(age / 1000000U);
    return rtc_valid && age < 5000000ULL && value >= 1704067200LL && value <= UINT32_MAX ? (uint32_t)value : 0;
}
static mbedtls_time_t tls_time(mbedtls_time_t *out)
{
    mbedtls_time_t now = (mbedtls_time_t)utc_now();
    if (out) *out = now;
    return now;
}
struct tm *mbedtls_platform_gmtime_r(const mbedtls_time_t *input, struct tm *out)
{
    const struct tm *value = gmtime(input);
    if (!value) return NULL;
    *out = *value; return out;
}
void http_service_clock(unsigned year, unsigned month, unsigned day,
                         unsigned hour, unsigned minute, unsigned second, int valid)
{
    uint32_t epoch;
    if (!valid || http_rtc_epoch(year,month,day,hour,minute,second,0,&epoch)) { rtc_valid = 0; return; }
    if (!rtc_valid || epoch != rtc_local) { rtc_local = epoch; rtc_at = microseconds(); rtc_valid = 1; }
}

static int ensure_fs(void)
{
    FILINFO info;
    FRESULT result = f_stat("0:/ATNET.CFG", &info);
    /* Mount only when no FATFS is registered. Remounting an active filesystem
     * would invalidate SmartPort's long-lived FIL objects. */
    if (result == FR_NOT_ENABLED) return f_mount(&http_fs,"0:/",1) == FR_OK ? 0 : -1;
    return result == FR_OK || result == FR_NO_FILE ? 0 : -1;
}
static int file_read(const char *path, uint8_t *out, size_t length)
{
    FIL file;
    UINT got = 0;
    if (ensure_fs() || f_open(&file, path, FA_READ) != FR_OK) return -1;
    int result = f_size(&file) != length || f_read(&file, out, (UINT)length, &got) != FR_OK || got != length;
    if (f_close(&file) != FR_OK) result = 1;
    return result ? -1 : 0;
}
static int file_write(const char *path, const uint8_t *data, size_t length)
{
    FIL file;
    UINT wrote = 0;
    if (ensure_fs() || f_open(&file, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return -1;
    int result = f_write(&file, data, (UINT)length, &wrote) != FR_OK || wrote != length;
    if (!result && f_sync(&file) != FR_OK) result = 1;
    if (f_close(&file) != FR_OK) result = 1;
    return result ? -1 : 0;
}
static int seed_read(void *ctx, unsigned slot, uint8_t *out, size_t length)
{ (void)ctx; return file_read(seed_paths[slot], out, length); }
static int seed_write(void *ctx, unsigned slot, const uint8_t *data, size_t length)
{ (void)ctx; return file_write(seed_paths[slot], data, length); }
static int entropy(void *ctx, unsigned char *out, size_t length)
{
    (void)ctx;
    if (g_http.entropy_used || length != HTTP_SEED_BYTES) return MBEDTLS_ERR_CTR_DRBG_ENTROPY_SOURCE_FAILED;
    memcpy(out, g_http.entropy, length);
    mbedtls_platform_zeroize(g_http.entropy, sizeof(g_http.entropy));
    g_http.entropy_used = 1; return 0;
}
static int random_bytes(void *ctx, unsigned char *out, size_t length)
{
    int result = mbedtls_ctr_drbg_random(ctx, out, length);
    /* A boot seed is single-use. If automatic reseeding reaches that boundary,
     * fail this request, then rotate persisted state before the next request. */
    if (result) g_http.reseed_needed = 1;
    return result;
}
static void crypto_reset(void)
{
    mbedtls_ssl_free(&g_http.ssl);
    mbedtls_ssl_config_free(&g_http.config);
    mbedtls_x509_crt_free(&g_http.ca);
    mbedtls_ctr_drbg_free(&g_http.drbg);
    mbedtls_platform_zeroize(tls_heap, sizeof(tls_heap));
    g_http.crypto_ready = g_http.trust_ready = g_http.reseed_needed = 0;
}
static int ipv4(const char *text, uint8_t out[4])
{
    unsigned a,b,c,d;
    char tail;
    if (sscanf(text,"%u.%u.%u.%u%c",&a,&b,&c,&d,&tail) != 4 || a > 255 || b > 255 || c > 255 || d > 255) return -1;
    out[0]=(uint8_t)a; out[1]=(uint8_t)b; out[2]=(uint8_t)c; out[3]=(uint8_t)d; return 0;
}
static int load_config(void)
{
    uint8_t bytes[8];
    if (file_read("0:/ATNET.CFG", bytes, sizeof(bytes)) || bytes[0] != 'N' || bytes[1] != 1) return HTTP_E_ARGUMENT;
    memcpy(configured_dns, bytes + 2, 4);
    memcpy(g_http.dns, configured_dns, 4);
    if (!(g_http.dns[0] | g_http.dns[1] | g_http.dns[2] | g_http.dns[3])) {
        uthernet2_network_config_t network;
        if (uthernet2_read_network_config(&network)) return HTTP_E_IO;
        memcpy(g_http.dns, network.dns, sizeof(g_http.dns));
    }
    rtc_offset = (int16_t)((unsigned)bytes[6] | ((unsigned)bytes[7] << 8));
    if (rtc_offset < -840 || rtc_offset > 840) return HTTP_E_TIME;
    return 0;
}
static int crypto_init(void)
{
    uint8_t next[HTTP_SEED_BYTES];
    uint64_t generation;
    unsigned slot;
    int result;
    if (g_http.crypto_ready && !g_http.reseed_needed) return 0;
    if (g_http.reseed_needed) crypto_reset();
    mbedtls_memory_buffer_alloc_init(tls_heap, sizeof(tls_heap));
    mbedtls_ssl_init(&g_http.ssl);
    mbedtls_ssl_config_init(&g_http.config);
    mbedtls_x509_crt_init(&g_http.ca);
    mbedtls_ctr_drbg_init(&g_http.drbg);
    if (http_seed_select(seed_read, NULL, g_http.entropy, &generation, &slot)) return HTTP_E_SEED;
    g_http.entropy_used = 0;
    mbedtls_ctr_drbg_set_entropy_len(&g_http.drbg, HTTP_SEED_BYTES);
    result = mbedtls_ctr_drbg_seed(&g_http.drbg, entropy, NULL,
                                (const uint8_t *)"Appletini HTTP service v1", 24);
    if (!result) result = mbedtls_ctr_drbg_random(&g_http.drbg, next, sizeof(next));
    /* No socket is opened and no random output is exposed before this commits. */
    if (!result) result = http_seed_commit(seed_read, seed_write, NULL, slot, generation + 1U, next);
    mbedtls_platform_zeroize(next, sizeof(next));
    mbedtls_platform_zeroize(g_http.entropy, sizeof(g_http.entropy));
    if (result) { mbedtls_ctr_drbg_free(&g_http.drbg); return HTTP_E_SEED; }
    g_http.status.seed_generation = generation + 1U;
    g_http.crypto_ready = 1;
    (void)mbedtls_platform_set_time(tls_time);
    mbedtls_ecp_set_max_ops(32);
    return 0;
}
static int trust_load(void)
{
    FIL file;
    UINT got;
    if (g_http.trust_ready) return 0;
    if (f_open(&file, "0:/ATNETCA.PEM", FA_READ) != FR_OK) return HTTP_E_TRUST;
    size_t length = f_size(&file);
    int result = !length || length >= sizeof(trust_pem) ||
        f_read(&file, trust_pem, (UINT)length, &got) != FR_OK || got != length;
    if (f_close(&file) != FR_OK) result = 1;
    if (!result) {
        trust_pem[length] = 0;
        result = mbedtls_x509_crt_parse(&g_http.ca, trust_pem, length + 1U);
    }
    memset(trust_pem, 0, sizeof(trust_pem));
    if (result) { mbedtls_x509_crt_free(&g_http.ca); mbedtls_x509_crt_init(&g_http.ca); return HTTP_E_TRUST; }
    g_http.trust_ready = 1; return 0;
}

int http_service_boot_prepare(void)
{
    stack_prepare();
    uint64_t started = microseconds();
    int result;
    if (http_service_active()) return HTTP_E_BUSY;
    boot_prepared = 1;
    prepare_error = load_config();
    if (!prepare_error) prepare_error = crypto_init();
    result = prepare_error ? prepare_error : trust_load();
    prepare_us = (uint32_t)(microseconds() - started);
    return result;
}

static int request_config(void)
{
    if (!boot_prepared) return HTTP_E_SEED;
    if (prepare_error) return prepare_error;
    if (!g_http.crypto_ready || g_http.reseed_needed) return HTTP_E_SEED;
    memcpy(g_http.dns,configured_dns,4);
    if (!(g_http.dns[0] | g_http.dns[1] | g_http.dns[2] | g_http.dns[3])) {
        uthernet2_network_config_t network;
        if (uthernet2_read_network_config(&network)) return HTTP_E_IO;
        memcpy(g_http.dns,network.dns,4);
    }
    return 0;
}

static void close_socket(unsigned socket)
{
    (void)uthernet2_w5100_socket_command((uint8_t)socket, W5100_CR_CLOSE);
    (void)uthernet2_write_reg(uthernet2_w5100_socket_reg((uint8_t)socket,W5100_SN_IR),0xFF);
}
static int connection_timeout(unsigned socket, uint8_t *timed_out)
{
    uint8_t flags;
    if (uthernet2_read_reg(uthernet2_w5100_socket_reg((uint8_t)socket,W5100_SN_IR),&flags)) return HTTP_E_IO;
    *timed_out = (flags & W5100_IR_TIMEOUT) != 0;
    /* UDP SEND times out on ARP; TCP CONNECT can also time out on SYN.
     * Acknowledge the flag before retrying, while retaining the request deadline. */
    if (*timed_out && uthernet2_write_reg(uthernet2_w5100_socket_reg((uint8_t)socket,W5100_SN_IR),W5100_IR_TIMEOUT)) return HTTP_E_IO;
    return 0;
}
static int claim(void)
{
    if (ftp_sd_service_active() || usb_sdd_service_active()) return HTTP_E_STORAGE_BUSY;
    g_http.saved_mask = REG_READ(CARD_CTRL_SLOT_ENABLE_REG);
    REG_WRITE(CARD_CTRL_SLOT_ENABLE_REG, g_http.saved_mask & ~(1UL << 1));
    g_http.owns_eth = 1;
    for (unsigned i=0;i<4;++i) close_socket(i);
    return uthernet2_write_reg(W5100_REG_RMSR,W5100_SOCKET_MEM_4_2_1_1) ||
           uthernet2_write_reg(W5100_REG_TMSR,W5100_SOCKET_MEM_4_2_1_1) ? HTTP_E_IO : 0;
}
static void finish(int result)
{
    if (g_http.owns_eth) {
        close_socket(SOCKET_TCP); close_socket(SOCKET_DNS);
        uint32_t mask = REG_READ(CARD_CTRL_SLOT_ENABLE_REG);
        REG_WRITE(CARD_CTRL_SLOT_ENABLE_REG,(mask & ~(1UL << 1)) | (g_http.saved_mask & (1UL << 1)));
        g_http.owns_eth = 0;
    }
    if (g_http.url.tls) g_http.status.verify = mbedtls_ssl_get_verify_result(&g_http.ssl);
    g_http.status.result = result;
    if (result && g_http.status.retained) g_http.ring_count = g_http.read_at = 0;
    g_http.status.state = result ? HTTP_S_FAILED : HTTP_S_DONE;
    g_http.completed = microseconds();
    mbedtls_ssl_free(&g_http.ssl);
    mbedtls_ssl_init(&g_http.ssl);
}
int http_service_active(void)
{ return g_http.status.state >= HTTP_S_PREPARE && g_http.status.state <= HTTP_S_READ; }
void http_service_cancel(void) { if (http_service_active()) finish(HTTP_E_CANCELLED); }
void http_service_storage_busy(int busy)
{ storage_busy = busy != 0; if (storage_busy) { http_service_cancel();net_service_reset(); } }

static int socket_send(void *ctx, const unsigned char *bytes, size_t length)
{
    uint8_t flags;
    uint16_t room, pointer;
    (void)ctx;
    if (!g_http.credit) return MBEDTLS_ERR_SSL_WANT_WRITE;
    --g_http.credit;
    if (g_http.sending) {
        if (uthernet2_read_reg(uthernet2_w5100_socket_reg(SOCKET_TCP,W5100_SN_IR),&flags)) return HTTP_E_IO;
        if (flags & W5100_IR_TIMEOUT) return HTTP_E_TIMEOUT;
        if (!(flags & W5100_IR_SENDOK)) return MBEDTLS_ERR_SSL_WANT_WRITE;
        if (uthernet2_write_reg(uthernet2_w5100_socket_reg(SOCKET_TCP,W5100_SN_IR),W5100_IR_SENDOK)) return HTTP_E_IO;
        g_http.sending = 0;
    }
    if (uthernet2_w5100_read16_stable(uthernet2_w5100_socket_reg(SOCKET_TCP,W5100_SN_TX_FSR),&room)) return HTTP_E_IO;
    if (!room) return MBEDTLS_ERR_SSL_WANT_WRITE;
    if (length > IO_CHUNK) length = IO_CHUNK;
    if (length > room) length = room;
    if (uthernet2_w5100_read16(uthernet2_w5100_socket_reg(SOCKET_TCP,W5100_SN_TX_WR),&pointer) ||
        uthernet2_w5100_socket_ring_write(SOCKET_TCP,pointer,bytes,(uint16_t)length) ||
        uthernet2_w5100_write16(uthernet2_w5100_socket_reg(SOCKET_TCP,W5100_SN_TX_WR),(uint16_t)(pointer + length)) ||
        uthernet2_w5100_socket_command(SOCKET_TCP,W5100_CR_SEND)) return HTTP_E_IO;
    g_http.sending = 1; g_http.io_bytes += (uint32_t)length; return (int)length;
}
static int socket_receive(void *ctx, unsigned char *bytes, size_t length)
{
    uint16_t available,pointer;
    uint8_t status;
    (void)ctx;
    if (!g_http.credit) return MBEDTLS_ERR_SSL_WANT_READ;
    --g_http.credit;
    if (uthernet2_w5100_read16_stable(uthernet2_w5100_socket_reg(SOCKET_TCP,W5100_SN_RX_RSR),&available)) return HTTP_E_IO;
    if (!available) {
        if (uthernet2_w5100_socket_status(SOCKET_TCP,&status)) return HTTP_E_IO;
        return status == W5100_SR_CLOSED || status == W5100_SR_CLOSE_WAIT ? 0 : MBEDTLS_ERR_SSL_WANT_READ;
    }
    if (length > IO_CHUNK) length = IO_CHUNK;
    if (available > length) available = (uint16_t)length;
    if (uthernet2_w5100_read16(uthernet2_w5100_socket_reg(SOCKET_TCP,W5100_SN_RX_RD),&pointer) ||
        uthernet2_w5100_socket_ring_read(SOCKET_TCP,pointer,bytes,available) ||
        uthernet2_w5100_write16(uthernet2_w5100_socket_reg(SOCKET_TCP,W5100_SN_RX_RD),(uint16_t)(pointer + available)) ||
        uthernet2_w5100_socket_command(SOCKET_TCP,W5100_CR_RECV)) return HTTP_E_IO;
    g_http.io_bytes += available; return available;
}
static int body_sink(void *ctx, const uint8_t *data, size_t length)
{
    (void)ctx;
    if (g_http.parser.status >= 300 && g_http.parser.status <= 399 && g_http.parser.location[0]) return 0;
    if (g_http.status.retained) {
        if (length > HTTP_RETAIN_MAX - g_http.ring_count) return -1;
        memcpy(g_http.retained + g_http.ring_count,data,length); g_http.ring_count += length;
    } else {
        if (length > STREAM_RING - g_http.ring_count) return 1;
        for (size_t i=0;i<length;++i) g_http.stream[(g_http.ring_at + g_http.ring_count + i) % STREAM_RING] = data[i];
        g_http.ring_count += length;
    }
    return 0;
}
size_t http_service_read(uint8_t *out, size_t capacity)
{
    size_t n = g_http.status.retained ? g_http.ring_count - g_http.read_at : g_http.ring_count;
    if (n > capacity) n = capacity;
    if (g_http.status.retained) { memcpy(out,g_http.retained + g_http.read_at,n); g_http.read_at += n; }
    else {
        for (size_t i=0;i<n;++i) out[i] = g_http.stream[(g_http.ring_at + i) % STREAM_RING];
        g_http.ring_at = (g_http.ring_at + n) % STREAM_RING; g_http.ring_count -= n;
    }
    return n;
}
const uint8_t *http_service_retained(size_t *length)
{
    *length = g_http.status.retained ? g_http.ring_count : 0;
    return g_http.retained;
}
void http_service_status(http_status_t *out)
{
    *out = g_http.status;
    out->status = g_http.parser.status;
    out->body_bytes = g_http.parser.body_bytes;
    memcpy(out->content_type,g_http.parser.content_type,sizeof(out->content_type));
    out->buffered = (uint32_t)(g_http.status.retained ? g_http.ring_count - g_http.read_at : g_http.ring_count);
    out->elapsed_us = g_http.started ? (http_service_active() ? microseconds() : g_http.completed) - g_http.started : 0;
    if (g_http.crypto_ready) {
        size_t peak,blocks;
        mbedtls_memory_buffer_alloc_max_get(&peak,&blocks); out->tls_heap_peak = (uint32_t)peak;
    }
}

static int begin_connection(void)
{
    uint8_t random[4];
    if (random_bytes(&g_http.drbg,random,sizeof(random))) return HTTP_E_SEED;
    g_http.source_port = (uint16_t)(49152U + ((((unsigned)random[0] << 8) | random[1]) & 16383U));
    g_http.dns_id = (uint16_t)(((unsigned)random[2] << 8) | random[3]);
    g_http.sent = g_http.rx_at = g_http.rx_length = 0; g_http.sending = 0; g_http.connect_at = 0;
    http_parser_init(&g_http.parser,!strcmp(g_http.method,"HEAD"),g_http.status.retained ? HTTP_RETAIN_MAX : UINT32_MAX);
    if (!ipv4(g_http.url.host,g_http.peer)) g_http.status.state = HTTP_S_OPEN;
    else {
        if (!(g_http.dns[0] | g_http.dns[1] | g_http.dns[2] | g_http.dns[3])) return HTTP_E_DNS;
        close_socket(SOCKET_DNS);
        if (uthernet2_write_reg(uthernet2_w5100_socket_reg(SOCKET_DNS,W5100_SN_MR),UDP_MODE) ||
            uthernet2_w5100_write16(uthernet2_w5100_socket_reg(SOCKET_DNS,W5100_SN_PORT),g_http.source_port) ||
            uthernet2_w5100_socket_command(SOCKET_DNS,W5100_CR_OPEN)) return HTTP_E_IO;
        g_http.status.state = HTTP_S_DNS_OPEN;
        g_http.dns_deadline = microseconds() + DNS_TIMEOUT;
    }
    return 0;
}
static int redirect(void)
{
    char target[HTTP_URL_MAX];
    http_url_t next;
    uint16_t status = g_http.parser.status;
    if (status < 300 || status > 399 || !g_http.parser.location[0] || status == 304) return 1;
    if (status != 301 && status != 302 && status != 303 && status != 307 && status != 308) return HTTP_E_REDIRECT;
    if (++g_http.redirects > 3U || http_url_resolve(g_http.status.url,g_http.parser.location,target,sizeof(target)) ||
        http_url_parse(target,&next) || (g_http.url.tls && !next.tls)) return HTTP_E_REDIRECT;
    if (strcmp(next.host,g_http.url.host) || next.port != g_http.url.port || next.tls != g_http.url.tls)
        g_http.strip_credentials = 1;
    if ((status == 303 && strcmp(g_http.method,"HEAD")) || ((status == 301 || status == 302) && !strcmp(g_http.method,"POST"))) {
        strcpy(g_http.method,"GET"); g_http.body_length = 0;
    }
    close_socket(SOCKET_TCP); mbedtls_ssl_free(&g_http.ssl); mbedtls_ssl_init(&g_http.ssl);
    g_http.url = next; strcpy(g_http.status.url,target);
    if (next.tls) {
        if (!utc_now()) return HTTP_E_TIME;
        if (!g_http.trust_ready) return HTTP_E_TRUST;
    }
    return begin_connection();
}

int http_service_start(const http_request_t *request)
{
    http_url_t parsed;
    if (http_service_active() || console.active) return HTTP_E_BUSY;
    if (storage_busy || ftp_sd_service_active() || usb_sdd_service_active()) return HTTP_E_STORAGE_BUSY;
    if (!request || !request->url || !request->method || request->body_length > sizeof(g_http.body) ||
        (request->body_length && !request->body) || strlen(request->method) >= sizeof(g_http.method) ||
        (request->headers && strlen(request->headers) >= sizeof(g_http.headers)) ||
        http_url_parse(request->url,&parsed)) return HTTP_E_ARGUMENT;
    uint64_t generation = g_http.status.seed_generation;
    memset(&g_http.status,0,sizeof(g_http.status));
    g_http.status.seed_generation = generation;
    g_http.url = parsed; strcpy(g_http.status.url,request->url);
    strcpy(g_http.method,request->method);
    strcpy(g_http.headers,request->headers ? request->headers : "");
    g_http.body_length = request->body_length;
    if (request->body_length) memcpy(g_http.body,request->body,request->body_length);
    g_http.status.retained = request->retain != 0;
    g_http.ring_at = g_http.ring_count = g_http.read_at = 0;
    g_http.redirects = g_http.strip_credentials = 0;
    memset(&g_http.parser,0,sizeof(g_http.parser));
    g_http.started = microseconds(); g_http.completed = 0; g_http.deadline = g_http.started + REQUEST_TIMEOUT;
    g_http.status.verify = UINT32_MAX;
    g_http.status.state = HTTP_S_PREPARE;
    return 0;
}

static int step(void)
{
    uint8_t status;
    int result;
    static uint8_t traced_state = 255;
    if (trace_enabled && console.active && traced_state != g_http.status.state) {
        char line[32];traced_state=g_http.status.state;
        (void)snprintf(line,sizeof(line),"state=%u",traced_state);trace(line);
    }
    if (g_http.status.state == HTTP_S_PREPARE) {
        trace("config");if ((result=request_config())) return result;
        if (g_http.url.tls && !utc_now()) return HTTP_E_TIME;
        if (g_http.url.tls && !g_http.trust_ready) return HTTP_E_TRUST;
        trace("claim");
        if ((result = claim())) return result;
        return begin_connection();
    }
    if (g_http.status.state >= HTTP_S_DNS_OPEN && g_http.status.state <= HTTP_S_DNS_WAIT) {
        if (g_http.status.state == HTTP_S_DNS_OPEN) {
            if (uthernet2_w5100_socket_status(SOCKET_DNS,&status)) return HTTP_E_IO;
            if (status == UDP_READY) g_http.status.state = HTTP_S_DNS_SEND;
        } else if (g_http.status.state == HTTP_S_DNS_SEND) {
            uint8_t query[300]; uint16_t pointer;
            result = http_dns_query(g_http.url.host,g_http.dns_id,query,sizeof(query));
            if (result < 0) return HTTP_E_DNS;
            if (uthernet2_write_block(uthernet2_w5100_socket_reg(SOCKET_DNS,W5100_SN_DIPR),g_http.dns,4) ||
                uthernet2_w5100_write16(uthernet2_w5100_socket_reg(SOCKET_DNS,0x10),53) ||
                uthernet2_w5100_read16(uthernet2_w5100_socket_reg(SOCKET_DNS,W5100_SN_TX_WR),&pointer) ||
                uthernet2_w5100_socket_ring_write(SOCKET_DNS,pointer,query,(uint16_t)result) ||
                uthernet2_w5100_write16(uthernet2_w5100_socket_reg(SOCKET_DNS,W5100_SN_TX_WR),(uint16_t)(pointer + result)) ||
                uthernet2_w5100_socket_command(SOCKET_DNS,W5100_CR_SEND)) return HTTP_E_IO;
            g_http.dns_deadline = microseconds() + DNS_TIMEOUT;
            g_http.status.state = HTTP_S_DNS_WAIT;
        } else {
            uint16_t count,pointer;
            uint8_t wire[520], timed_out;
            if ((result = connection_timeout(SOCKET_DNS,&timed_out))) return result;
            if (timed_out) { trace("DNS ARP retry");g_http.status.state = HTTP_S_DNS_SEND;return 0; }
            if (uthernet2_w5100_read16_stable(uthernet2_w5100_socket_reg(SOCKET_DNS,W5100_SN_RX_RSR),&count)) return HTTP_E_IO;
            if (count < 8) {
                if (microseconds() >= g_http.dns_deadline) {
                    trace("DNS reply retry");g_http.status.state = HTTP_S_DNS_SEND;
                }
                return MBEDTLS_ERR_SSL_WANT_READ;
            }
            if (uthernet2_w5100_read16(uthernet2_w5100_socket_reg(SOCKET_DNS,W5100_SN_RX_RD),&pointer) ||
                uthernet2_w5100_socket_ring_read(SOCKET_DNS,pointer,wire,8)) return HTTP_E_IO;
            unsigned length = ((unsigned)wire[6] << 8) | wire[7];
            if (length > 512U || count < length + 8U) return HTTP_E_DNS;
            if (uthernet2_w5100_socket_ring_read(SOCKET_DNS,(uint16_t)(pointer + 8U),wire + 8,(uint16_t)length) ||
                uthernet2_w5100_write16(uthernet2_w5100_socket_reg(SOCKET_DNS,W5100_SN_RX_RD),(uint16_t)(pointer + length + 8U)) ||
                uthernet2_w5100_socket_command(SOCKET_DNS,W5100_CR_RECV)) return HTTP_E_IO;
            if (memcmp(wire,g_http.dns,4) || wire[4] || wire[5] != 53) return MBEDTLS_ERR_SSL_WANT_READ;
            if (http_dns_answer(wire + 8,length,g_http.dns_id,g_http.url.host,g_http.peer)) return HTTP_E_DNS;
            close_socket(SOCKET_DNS); g_http.status.state = HTTP_S_OPEN;
        }
        return 0;
    }
    if (g_http.status.state == HTTP_S_OPEN) {
        if (microseconds() < g_http.connect_at) return MBEDTLS_ERR_SSL_WANT_WRITE;
        close_socket(SOCKET_TCP);
        if (uthernet2_write_reg(uthernet2_w5100_socket_reg(SOCKET_TCP,W5100_SN_MR),W5100_SN_MR_TCP) ||
            uthernet2_w5100_write16(uthernet2_w5100_socket_reg(SOCKET_TCP,W5100_SN_PORT),g_http.source_port) ||
            uthernet2_w5100_socket_command(SOCKET_TCP,W5100_CR_OPEN)) return HTTP_E_IO;
        g_http.status.state = HTTP_S_CONNECT; g_http.sending = 0; return 0;
    }
    if (g_http.status.state == HTTP_S_CONNECT) {
        uint8_t timed_out;
        if (uthernet2_w5100_socket_status(SOCKET_TCP,&status)) return HTTP_E_IO;
        if ((result = connection_timeout(SOCKET_TCP,&timed_out))) return result;
        if (timed_out || status == W5100_SR_CLOSED) {
            trace("TCP connect retry");close_socket(SOCKET_TCP);g_http.sending = 0;
            g_http.connect_at = microseconds() + CONNECT_RETRY_US;
            g_http.status.state = HTTP_S_OPEN;return 0;
        }
        if (status == W5100_SR_INIT && !g_http.sending) {
            if (uthernet2_write_block(uthernet2_w5100_socket_reg(SOCKET_TCP,W5100_SN_DIPR),g_http.peer,4) ||
                uthernet2_w5100_write16(uthernet2_w5100_socket_reg(SOCKET_TCP,0x10),g_http.url.port) ||
                uthernet2_w5100_socket_command(SOCKET_TCP,4)) return HTTP_E_IO;
            g_http.sending = 1;
        } else if (status == W5100_SR_ESTABLISHED) {
            g_http.sending = 0;
            trace("request build");
            result = http_request_build(&g_http.url,g_http.method,g_http.headers,g_http.body_length,
                                        g_http.strip_credentials,g_http.request,sizeof(g_http.request));
            if (result < 0) return result;
            g_http.request_length = (size_t)result;
            if (g_http.url.tls) {
                static const int suites[] = { MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
                                              MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,0 };
                trace("config free");
                if (trace_enabled) {
                    char line[80];
                    (void)snprintf(line,sizeof(line),"heap=%d key_cert=%08lX ssl=%lu config=%lu",
                        mbedtls_memory_buffer_alloc_verify(),(unsigned long)g_http.config.MBEDTLS_PRIVATE(key_cert),
                        (unsigned long)sizeof(g_http.ssl),(unsigned long)sizeof(g_http.config));
                    trace(line);
                }
                mbedtls_ssl_config_free(&g_http.config); mbedtls_ssl_config_init(&g_http.config);
                trace("config defaults");
                if ((result = mbedtls_ssl_config_defaults(&g_http.config,MBEDTLS_SSL_IS_CLIENT,MBEDTLS_SSL_TRANSPORT_STREAM,MBEDTLS_SSL_PRESET_DEFAULT))) return result;
                mbedtls_ssl_conf_authmode(&g_http.config,MBEDTLS_SSL_VERIFY_REQUIRED);
                mbedtls_ssl_conf_ca_chain(&g_http.config,&g_http.ca,NULL);
                mbedtls_ssl_conf_rng(&g_http.config,random_bytes,&g_http.drbg);
                mbedtls_ssl_conf_ciphersuites(&g_http.config,suites);
                trace("ssl setup");
                if ((result = mbedtls_ssl_setup(&g_http.ssl,&g_http.config)) ||
                    (result = mbedtls_ssl_set_hostname(&g_http.ssl,g_http.url.host))) return result;
                trace("ssl ready");
                mbedtls_ssl_set_bio(&g_http.ssl,NULL,socket_send,socket_receive,NULL);
                g_http.handshake_started = microseconds(); g_http.status.state = HTTP_S_HANDSHAKE;
            } else { g_http.status.verify = 0; g_http.status.state = HTTP_S_WRITE; }
        }
        return 0;
    }
    if (g_http.status.state == HTTP_S_HANDSHAKE) {
        if (!utc_now()) return HTTP_E_TIME;
        result = mbedtls_ssl_handshake_step(&g_http.ssl);
        if (!result && mbedtls_ssl_is_handshake_over(&g_http.ssl)) {
            g_http.status.verify = mbedtls_ssl_get_verify_result(&g_http.ssl);
            g_http.status.handshake_us += microseconds() - g_http.handshake_started;
            g_http.status.state = HTTP_S_WRITE;
        }
        return result;
    }
    if (g_http.status.state == HTTP_S_WRITE) {
        const uint8_t *data;
        size_t length;
        if (g_http.sent < g_http.request_length) {
            data = (const uint8_t *)g_http.request + g_http.sent; length = g_http.request_length - g_http.sent;
        } else { size_t at = g_http.sent - g_http.request_length; data = g_http.body + at; length = g_http.body_length - at; }
        if (!length) { g_http.status.state = HTTP_S_READ; return 0; }
        result = g_http.url.tls ? mbedtls_ssl_write(&g_http.ssl,data,length) : socket_send(NULL,data,length);
        if (result > 0) { g_http.sent += (size_t)result; return 0; }
        return result;
    }
    if (g_http.status.state == HTTP_S_READ) {
        if (g_http.rx_at == g_http.rx_length) {
            result = g_http.url.tls ? mbedtls_ssl_read(&g_http.ssl,g_http.receive,sizeof(g_http.receive)) :
                                      socket_receive(NULL,g_http.receive,sizeof(g_http.receive));
            if (result == 0 || result == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
                if (http_parser_eof(&g_http.parser) != HTTP_COMPLETE) return HTTP_FORMAT;
                result = redirect(); if (result == 1) finish(0); return result < 0 ? result : 0;
            }
            if (result < 0) return result;
            g_http.rx_at = 0; g_http.rx_length = (size_t)result;
        }
        size_t used;
        result = http_parser_feed(&g_http.parser,g_http.receive + g_http.rx_at,g_http.rx_length - g_http.rx_at,&used,body_sink,NULL);
        g_http.rx_at += used;
        if (result == HTTP_PAUSED) return MBEDTLS_ERR_SSL_WANT_READ;
        if (result == HTTP_COMPLETE) { result = redirect(); if (result == 1) finish(0); return result < 0 ? result : 0; }
        return result;
    }
    return 0;
}

void http_service_console_begin(uint32_t uart)
{
    console.active = 1;
    console.uart = uart;
    mbedtls_sha256_init(&console.hash);
    (void)mbedtls_sha256_starts(&console.hash, 0);
}
static void console_poll(void)
{
    uint8_t bytes[512];
    size_t n;
    if (!console.active) return;
    for (unsigned i=0;i<8 && (n = http_service_read(bytes,sizeof(bytes))) != 0;++i)
        (void)mbedtls_sha256_update(&console.hash,bytes,n);
    if (!http_service_active() && (g_http.status.retained ?
        g_http.read_at == g_http.ring_count : !g_http.ring_count)) {
        http_status_t status;
        uint8_t hash[32]; char line[512];
        http_service_status(&status);
        (void)mbedtls_sha256_finish(&console.hash,hash);
        (void)snprintf(line,sizeof(line),"[http-result] rc=%d verify=0x%08lX status=%u body=%lu handshake_us=%llu elapsed_us=%llu step_max_us=%lu slice_max_us=%lu tls_heap_peak=%lu seed_generation=%llu\r\n",
            status.result,(unsigned long)status.verify,status.status,(unsigned long)status.body_bytes,
            (unsigned long long)status.handshake_us,(unsigned long long)status.elapsed_us,
            (unsigned long)status.max_step_us,(unsigned long)status.max_slice_us,(unsigned long)status.tls_heap_peak,
            (unsigned long long)status.seed_generation);
        uart_puts(console.uart,line); uart_puts(console.uart,"[http-sha256] ");
        for (unsigned i=0;i<32;++i) { (void)snprintf(line,sizeof(line),"%02x",hash[i]); uart_puts(console.uart,line); }
        uart_puts(console.uart,"\r\n[http] done\r\n");
        mbedtls_sha256_free(&console.hash); console.active = 0;
    }
}
void http_service_poll(void)
{
    if (http_service_active()) {
        uint64_t start = microseconds();
        g_http.credit = IO_CALLS;
        for (unsigned i=0;i<SLICE_STEPS && http_service_active();++i) {
            uint64_t before = microseconds();
            uint8_t old_state = g_http.status.state;
            uint32_t bytes = g_http.io_bytes;
            if (before >= g_http.deadline) {
                finish(old_state >= HTTP_S_DNS_OPEN && old_state <= HTTP_S_CONNECT ?
                       HTTP_E_NETWORK_NOT_READY : HTTP_E_TIMEOUT);
                break;
            }
            int result = step();
            maximum(&g_http.status.max_step_us,microseconds() - before);
            if (result < 0 && result != MBEDTLS_ERR_SSL_WANT_READ && result != MBEDTLS_ERR_SSL_WANT_WRITE && result != MBEDTLS_ERR_SSL_CRYPTO_IN_PROGRESS) { finish(result); break; }
            if ((result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE) &&
                (g_http.io_bytes == bytes || !g_http.credit)) break;
            if (old_state < HTTP_S_HANDSHAKE || microseconds() - start >= SLICE_US) break;
        }
        maximum(&g_http.status.max_slice_us,microseconds() - start);
    }
    console_poll();
    http_bench_poll();
}

#ifdef APPLETINI_HTTP_DIAGNOSTICS
static int hex(char c)
{ if (c>='0'&&c<='9') return c-'0'; if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1; }
#endif
int http_service_command(int argc, char **argv, uint32_t uart)
{
    char line[360];
    int result = 0;
    if (argc < 1 || strcmp(argv[0],"http")) return 0;
    if (argc == 2 && !strcmp(argv[1],"cancel")) { http_bench_cancel();http_service_cancel(); return 1; }
    if (argc == 2 && !strcmp(argv[1],"status")) {
        http_status_t status;http_service_status(&status);
        (void)snprintf(line,sizeof(line),"[http-status] state=%u result=%d verify=0x%08lX status=%u bytes=%lu step_max_us=%lu slice_max_us=%lu heap_peak=%lu utc=%lu rtc_offset=%d crypto=%u trust=%u seed_generation=%llu dns=%u.%u.%u.%u\r\n",
            status.state,status.result,(unsigned long)status.verify,status.status,(unsigned long)status.body_bytes,
            (unsigned long)status.max_step_us,(unsigned long)status.max_slice_us,(unsigned long)status.tls_heap_peak,
            (unsigned long)utc_now(),rtc_offset,g_http.crypto_ready,g_http.trust_ready,
            (unsigned long long)status.seed_generation,g_http.dns[0],g_http.dns[1],g_http.dns[2],g_http.dns[3]);
        uart_puts(uart,line);
        (void)snprintf(line,sizeof(line),"[http-json] slice_max_us=%lu boot_prepare_us=%lu ready=%u prepare_error=%d stack_used_upper=%lu stack_guard=512\r\n",(unsigned long)net_service_json_max_us(),(unsigned long)prepare_us,boot_prepared,prepare_error,(unsigned long)stack_used_upper());
        uart_puts(uart,line);return 1;
    }
    if (http_service_active() || console.active || http_bench_active() || storage_busy || ftp_sd_service_active() || usb_sdd_service_active()) { uart_puts(uart,"[http] busy\r\n"); return 1; }
#ifdef APPLETINI_HTTP_BENCH
    if(argc==3&&!strcmp(argv[1],"stress")) {
        char *end;unsigned long seconds=strtoul(argv[2],&end,10);
        result=*end||seconds>600U?HTTP_E_ARGUMENT:http_bench_stress_start((unsigned)seconds,uart);
        (void)snprintf(line,sizeof(line),"[http-stress] armed rc=%d\r\n",result);uart_puts(uart,line);return 1;
    }
    if (argc == 3 && !strcmp(argv[1], "bench")) {
        result = http_bench_start(argv[2], uart);
        (void)snprintf(line,sizeof(line),"[http-bench] armed rc=%d\r\n",result);
        uart_puts(uart,line); return 1;
    }
#endif
#ifdef APPLETINI_HTTP_DIAGNOSTICS
    if (argc==3 && !strcmp(argv[1],"trace")) {trace_enabled=!strcmp(argv[2],"on");uart_puts(uart,"[http] trace set\r\n");return 1;}
    if (argc==2 && !strcmp(argv[1],"prepare")) {
        crypto_reset();
        result=http_service_boot_prepare();
        (void)snprintf(line,sizeof(line),"[http] prepare rc=%d us=%lu\r\n",result,(unsigned long)prepare_us);uart_puts(uart,line);return 1;
    }
    if (argc==2 && !strcmp(argv[1],"net")) {
        uthernet2_test_result_t net;
        if (!uthernet2_test(&net)) { (void)snprintf(line,sizeof(line),"[http-net] link=%u ip=%u.%u.%u.%u gateway=%u.%u.%u.%u\r\n",net.link_up,
            net.config.ip[0],net.config.ip[1],net.config.ip[2],net.config.ip[3],net.config.gateway[0],net.config.gateway[1],net.config.gateway[2],net.config.gateway[3]);uart_puts(uart,line); }
        for (unsigned group=0;group<3;++group) {
            uint8_t bytes[64];uint16_t base=group?uthernet2_w5100_socket_reg((uint8_t)(group-1),0):0;
            size_t count=group?64:32;
            if(uthernet2_read_block(base,bytes,(uint16_t)count)) {uart_puts(uart,"[http-net] register read failed\r\n");continue;}
            (void)snprintf(line,sizeof(line),"[http-net-reg] %04X:",base);uart_puts(uart,line);
            for(size_t i=0;i<count;++i){(void)snprintf(line,sizeof(line)," %02X",bytes[i]);uart_puts(uart,line);}
            uart_puts(uart,"\r\n");
        }
        return 1;
    }
#endif
#ifdef APPLETINI_HTTP_DIAGNOSTICS
    if (argc == 2 && !strcmp(argv[1],"erase-test-provisioning")) {
        if (ensure_fs()) { uart_puts(uart,"[http] SD filesystem unavailable\r\n"); return 1; }
        crypto_reset();
        for (unsigned i = 0; i < 2; ++i) {
            FRESULT removed = f_unlink(seed_paths[i]);
            if (removed != FR_OK && removed != FR_NO_FILE) result = HTTP_E_SEED;
        }
        (void)snprintf(line,sizeof(line),"[http] erase-test-provisioning rc=%d\r\n",result);
        uart_puts(uart,line); return 1;
    }
    if (argc == 3 && !strcmp(argv[1],"provision")) {
        if (ensure_fs()) { uart_puts(uart,"[http] SD filesystem unavailable\r\n"); return 1; }
        uint8_t seed[HTTP_SEED_BYTES], record[HTTP_SEED_RECORD];
        FILINFO info;
        if (strlen(argv[2]) != HTTP_SEED_BYTES * 2U || f_stat(seed_paths[0],&info) != FR_NO_FILE ||
            f_stat(seed_paths[1],&info) != FR_NO_FILE) result = HTTP_E_SEED;
        else {
            for (unsigned i=0;i<HTTP_SEED_BYTES;++i) {
                int high=hex(argv[2][i*2]),low=hex(argv[2][i*2+1]);
                if (high<0||low<0) { result=HTTP_E_ARGUMENT;break; }
                seed[i]=(uint8_t)(high*16+low);
            }
            if (!result) {
                http_seed_encode(record,1,seed);
                result = seed_write(NULL,0,record,sizeof(record)) || seed_write(NULL,1,record,sizeof(record)) ? HTTP_E_SEED : 0;
            }
        }
        mbedtls_platform_zeroize(argv[2],strlen(argv[2]));
        mbedtls_platform_zeroize(seed,sizeof(seed)); mbedtls_platform_zeroize(record,sizeof(record));
        (void)snprintf(line,sizeof(line),"[http] provision rc=%d\r\n",result); uart_puts(uart,line); return 1;
    }
#endif
    if (argc == 4 && !strcmp(argv[1],"config")) {
        if (ensure_fs()) { uart_puts(uart,"[http] SD filesystem unavailable\r\n"); return 1; }
        uint8_t bytes[8]={'N',1}; char *end;
        long offset=strtol(argv[3],&end,10);
        if (*end || offset < -840 || offset > 840 || ipv4(argv[2],bytes+2)) result=HTTP_E_ARGUMENT;
        else { bytes[6]=(uint8_t)offset;bytes[7]=(uint8_t)((uint16_t)offset>>8);result=file_write("0:/ATNET.CFG",bytes,sizeof(bytes)); }
        if (!result) { result=load_config();if (!result && prepare_error==HTTP_E_ARGUMENT) prepare_error=HTTP_E_SEED; }
        (void)snprintf(line,sizeof(line),"[http] config rc=%d\r\n",result);uart_puts(uart,line);return 1;
    }
    if ((argc == 3 && (!strcmp(argv[1],"get") || !strcmp(argv[1],"retain"))) || (argc == 4 && !strcmp(argv[1],"post"))) {
        http_request_t request={argv[2],argc==4?"POST":"GET",argc==4?"Content-Type: application/json\r\n":"",
                                argc==4?(const uint8_t *)argv[3]:NULL,argc==4?strlen(argv[3]):0,!strcmp(argv[1],"retain")};
        result=http_service_start(&request);
        if (!result) http_service_console_begin(uart);
        (void)snprintf(line,sizeof(line),"[http] start rc=%d\r\n",result);uart_puts(uart,line);return 1;
    }
    uart_puts(uart,"usage: http status | config <DNS IPv4> <RTC UTC offset minutes> | get <URL> | post <URL> <JSON> | cancel\r\n");
    return 1;
}
#endif
