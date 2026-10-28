/* Opt-in CPU0 checkpoint; no slot personality or persistent settings changes. */
#ifdef APPLETINI_HTTPS_PROBE
#include "https_probe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/ecp.h"
#include "mbedtls/memory_buffer_alloc.h"
#include "mbedtls/platform_time.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/sha256.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "https_probe_test_ca.h"
#include "../lib/common.h"
#include "../lib/crc32.h"
#include "../lib/uart.h"
#include "apple_fb_handoff.h"
#include "card_control_regs.h"
#include "compositor.h"
#include "ftp_sd_service.h"
#include "smartport_service.h"
#include "usb_hid_service.h"
#include "usb_xbox_360.h"
#include "uthernet2_control.h"
#include "xiltimer.h"

#define PROBE_SOCKET 0U
#define PROBE_IO_CHUNK 512U
#define PROBE_IO_CALLS 8U
#define PROBE_SLICE_US 750U
#define PROBE_SLICE_STEPS 32U
#define PROBE_PHASE_US 3000000ULL
#define PROBE_TIMEOUT_US 30000000ULL
#define PROBE_STORAGE_BLOCKS 64U
#define PROBE_STORAGE_PERIOD_US 10000ULL
#define PROBE_EXPECTED_BODY 65536U

typedef enum {
    PROBE_IDLE, PROBE_DHCP, PROBE_BASELINE, PROBE_OPEN, PROBE_CONNECT,
    PROBE_SETUP, PROBE_HANDSHAKE, PROBE_WRITE, PROBE_READ, PROBE_RECOVERY,
    PROBE_REPORT
} probe_state_t;

typedef struct {
    uint64_t start_us, end_us;
    uint64_t last_poll, last_service[HTTPS_PROBE_SERVICE_COUNT];
    uint32_t max_loop_us, max_service_us[HTTPS_PROBE_SERVICE_COUNT];
    uint32_t service_calls[HTTPS_PROBE_SERVICE_COUNT];
    uint32_t storage_reads, storage_errors, storage_crc_errors, storage_max_us;
    uint64_t storage_total_us;
    uint32_t frames_start, frames_end, blits_start, blits_end;
    uint32_t usb_start, usb_end, usb_errors_start, usb_errors_end;
    uint32_t xbox_start, xbox_end, xbox_errors_start, xbox_errors_end;
    uint32_t sp_reads_start, sp_reads_end, sp_writes_start, sp_writes_end;
    uint32_t cache_miss_start, cache_miss_end;
    uint8_t video_valid_start, video_valid_end;
    apple_fb_health_t video_start, video_end;
} probe_phase_t;

static struct {
    probe_state_t state;
    uint32_t uart, saved_slot_mask, epoch, verify_flags;
    uint64_t epoch_at, deadline, next_storage, handshake_start, handshake_us;
    uint32_t max_step_us, max_handshake_step_us, max_slice_us, calls;
    uint32_t handshake_calls, io_bytes;
    uint64_t handshake_busy_us;
    uint8_t owns_eth, send_pending, seed_ready, entropy_used, phase_index, io_credit;
    uint8_t seed[48], peer[4];
    uint16_t port, source_port;
    char host[128], request[256], response_header[2048];
    size_t request_len, request_sent, header_len;
    uint32_t body_bytes;
    uint8_t headers_done, body_valid;
    int result, http_status;
    uint32_t reference_crc[PROBE_STORAGE_BLOCKS];
    uint64_t reference_valid;
    uint32_t storage_cursor;
    uint8_t storage_buffer[512] __attribute__((aligned(32)));
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config config;
    mbedtls_x509_crt ca;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_sha256_context body_hash;
    uint8_t body_digest[32];
    probe_phase_t phases[3];
} g_probe;

static unsigned char g_tls_heap[512U * 1024U] __attribute__((aligned(32)));

int https_probe_active(void) { return g_probe.state != PROBE_IDLE; }

static uint64_t probe_us(void)
{
    XTime now;
    XTime_GetTime(&now);
    /* The BSP macro expands to CPU_CLOCK/2 without outer parentheses. */
    const uint64_t frequency = (uint64_t)(COUNTS_PER_SECOND);
    return (uint64_t)now * 1000000ULL / frequency;
}

static uint32_t bounded_u32(uint64_t value)
{ return value > UINT32_MAX ? UINT32_MAX : (uint32_t)value; }

mbedtls_ms_time_t mbedtls_ms_time(void)
{ return (mbedtls_ms_time_t)(probe_us() / 1000ULL); }

static void update_max(uint32_t *maximum, uint64_t value)
{ if (value > *maximum) *maximum = bounded_u32(value); }

static mbedtls_time_t probe_time(mbedtls_time_t *out)
{
    mbedtls_time_t value = (mbedtls_time_t)(g_probe.epoch +
        (probe_us() - g_probe.epoch_at) / 1000000ULL);
    if (out != NULL) *out = value;
    return value;
}

struct tm *mbedtls_platform_gmtime_r(const mbedtls_time_t *input, struct tm *out)
{
    const struct tm *value = gmtime(input);
    if (value == NULL) return NULL;
    *out = *value;
    return out;
}

/* A fresh 384-bit OS-CSPRNG sample is provisioned over the owned UART per run.
 * Never infer entropy from the timer, MAC, board serial or DHCP transaction ID. */
static int probe_entropy(void *ctx, unsigned char *output, size_t length)
{
    (void)ctx;
    if (!g_probe.seed_ready || g_probe.entropy_used || length > sizeof(g_probe.seed))
        return MBEDTLS_ERR_CTR_DRBG_ENTROPY_SOURCE_FAILED;
    memcpy(output, g_probe.seed, length);
    mbedtls_platform_zeroize(g_probe.seed, sizeof(g_probe.seed));
    g_probe.seed_ready = 0U;
    g_probe.entropy_used = 1U;
    return 0;
}

static void socket_close(void)
{
    (void)uthernet2_w5100_socket_command(PROBE_SOCKET, W5100_CR_CLOSE);
    (void)uthernet2_write_reg(uthernet2_w5100_socket_reg(PROBE_SOCKET, W5100_SN_IR), 0xFFU);
    g_probe.send_pending = 0U;
}

static int claim_ethernet(void)
{
    if (ftp_sd_service_active()) return -1;
    if (!g_probe.owns_eth) {
        g_probe.saved_slot_mask = REG_READ(CARD_CTRL_SLOT_ENABLE_REG);
        REG_WRITE(CARD_CTRL_SLOT_ENABLE_REG, g_probe.saved_slot_mask & ~(1UL << 1));
        g_probe.owns_eth = 1U;
    }
    return 0;
}

static void release_ethernet(void)
{
    if (g_probe.owns_eth) {
        /* Preserve changes to other slots made while the probe ran. */
        uint32_t mask = REG_READ(CARD_CTRL_SLOT_ENABLE_REG);
        mask = (mask & ~(1UL << 1)) | (g_probe.saved_slot_mask & (1UL << 1));
        REG_WRITE(CARD_CTRL_SLOT_ENABLE_REG, mask);
        g_probe.owns_eth = 0U;
    }
}

static int probe_send(void *ctx, const unsigned char *bytes, size_t length)
{
    uint8_t interrupt, status;
    uint16_t free_size, pointer;
    (void)ctx;
    if (!g_probe.io_credit) return MBEDTLS_ERR_SSL_WANT_WRITE;
    g_probe.io_credit--;
    if (g_probe.send_pending) {
        if (uthernet2_read_reg(uthernet2_w5100_socket_reg(PROBE_SOCKET, W5100_SN_IR), &interrupt))
            return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
        if (interrupt & W5100_IR_TIMEOUT) return MBEDTLS_ERR_SSL_TIMEOUT;
        if (!(interrupt & W5100_IR_SENDOK)) return MBEDTLS_ERR_SSL_WANT_WRITE;
        (void)uthernet2_write_reg(uthernet2_w5100_socket_reg(PROBE_SOCKET, W5100_SN_IR), W5100_IR_SENDOK);
        g_probe.send_pending = 0U;
    }
    if (uthernet2_w5100_socket_status(PROBE_SOCKET, &status) || status != W5100_SR_ESTABLISHED)
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    if (uthernet2_w5100_read16_stable(uthernet2_w5100_socket_reg(PROBE_SOCKET, W5100_SN_TX_FSR), &free_size))
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    if (!free_size) return MBEDTLS_ERR_SSL_WANT_WRITE;
    if (length > PROBE_IO_CHUNK) length = PROBE_IO_CHUNK;
    if (length > free_size) length = free_size;
    if (uthernet2_w5100_read16(uthernet2_w5100_socket_reg(PROBE_SOCKET, W5100_SN_TX_WR), &pointer) ||
        uthernet2_w5100_socket_ring_write(PROBE_SOCKET, pointer, bytes, (uint16_t)length) ||
        uthernet2_w5100_write16(uthernet2_w5100_socket_reg(PROBE_SOCKET, W5100_SN_TX_WR), (uint16_t)(pointer + length)) ||
        uthernet2_w5100_socket_command(PROBE_SOCKET, W5100_CR_SEND))
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    g_probe.send_pending = 1U;
    g_probe.io_bytes += (uint32_t)length;
    return (int)length;
}

static int probe_receive(void *ctx, unsigned char *bytes, size_t capacity)
{
    uint16_t available, pointer;
    uint8_t status;
    (void)ctx;
    if (!g_probe.io_credit) return MBEDTLS_ERR_SSL_WANT_READ;
    g_probe.io_credit--;
    if (uthernet2_w5100_read16_stable(uthernet2_w5100_socket_reg(PROBE_SOCKET, W5100_SN_RX_RSR), &available))
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    if (!available) {
        if (uthernet2_w5100_socket_status(PROBE_SOCKET, &status)) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
        if (status == W5100_SR_CLOSE_WAIT || status == W5100_SR_CLOSED) return 0;
        return MBEDTLS_ERR_SSL_WANT_READ;
    }
    if (capacity > PROBE_IO_CHUNK) capacity = PROBE_IO_CHUNK;
    if (available > capacity) available = (uint16_t)capacity;
    if (uthernet2_w5100_read16(uthernet2_w5100_socket_reg(PROBE_SOCKET, W5100_SN_RX_RD), &pointer) ||
        uthernet2_w5100_socket_ring_read(PROBE_SOCKET, pointer, bytes, available) ||
        uthernet2_w5100_write16(uthernet2_w5100_socket_reg(PROBE_SOCKET, W5100_SN_RX_RD), (uint16_t)(pointer + available)) ||
        uthernet2_w5100_socket_command(PROBE_SOCKET, W5100_CR_RECV)) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    g_probe.io_bytes += available;
    return available;
}

static int pending_result(int result)
{
    return result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE ||
           result == MBEDTLS_ERR_SSL_CRYPTO_IN_PROGRESS;
}

static void snapshot(probe_phase_t *phase, int end)
{
    usb_hid_service_status_t usb;
    smartport_activity_t storage;
    uint32_t xbox_reports, xbox_errors;
    smartport_cache_stats_t cache;
    usb_hid_service_get_status(&usb);
    usb_xbox_360_get_activity(&xbox_reports, &xbox_errors);
    memset(&storage, 0, sizeof(storage));
    (void)smartport_service_get_activity(&storage);
    smartport_service_get_cache_stats(&cache);
    if (end) {
        phase->end_us = probe_us();
        phase->frames_end = apple_fb_reader_publish_seq();
        phase->blits_end = g_compositor_frames_published;
        phase->usb_end = usb.report_count;
        phase->usb_errors_end = usb.submit_error_count + usb.transfer_error_count;
        phase->xbox_end = xbox_reports;
        phase->xbox_errors_end = xbox_errors;
        phase->sp_reads_end = storage.read_count;
        phase->sp_writes_end = storage.write_count;
        phase->cache_miss_end = cache.misses;
        phase->video_valid_end = (uint8_t)apple_fb_health_get(&phase->video_end);
    } else {
        phase->start_us = probe_us();
        phase->frames_start = apple_fb_reader_publish_seq();
        phase->blits_start = g_compositor_frames_published;
        phase->usb_start = usb.report_count;
        phase->usb_errors_start = usb.submit_error_count + usb.transfer_error_count;
        phase->xbox_start = xbox_reports;
        phase->xbox_errors_start = xbox_errors;
        phase->sp_reads_start = storage.read_count;
        phase->sp_writes_start = storage.write_count;
        phase->cache_miss_start = cache.misses;
        phase->video_valid_start = (uint8_t)apple_fb_health_get(&phase->video_start);
    }
}

void https_probe_service_tick(unsigned service)
{
    probe_phase_t *phase;
    uint64_t now;
    if (g_probe.state < PROBE_BASELINE || g_probe.state > PROBE_RECOVERY ||
        service >= HTTPS_PROBE_SERVICE_COUNT) return;
    phase = &g_probe.phases[g_probe.phase_index];
    now = probe_us();
    if (phase->last_service[service])
        update_max(&phase->max_service_us[service], now - phase->last_service[service]);
    phase->last_service[service] = now;
    phase->service_calls[service]++;
}

static void storage_poll(uint64_t now)
{
    uint32_t actual = 0U, crc, index;
    int result;
    probe_phase_t *phase = &g_probe.phases[g_probe.phase_index];
    uint64_t start;
    if (now < g_probe.next_storage) return;
    g_probe.next_storage = now + PROBE_STORAGE_PERIOD_US;
    index = g_probe.storage_cursor++ % PROBE_STORAGE_BLOCKS;
    start = probe_us();
    /* Read-only SD-backed SmartPort image workload, not an Apple bus request. */
    result = smartport_service_read_block(1U, index, g_probe.storage_buffer, 512U, &actual);
    start = probe_us() - start;
    phase->storage_total_us += start;
    update_max(&phase->storage_max_us, start);
    if (result || actual != 512U) { phase->storage_errors++; return; }
    phase->storage_reads++;
    crc = crc32_finish(crc32_update(crc32_init(), g_probe.storage_buffer, 512U));
    if (!(g_probe.reference_valid & (1ULL << index))) {
        g_probe.reference_crc[index] = crc;
        g_probe.reference_valid |= 1ULL << index;
    } else if (g_probe.reference_crc[index] != crc) phase->storage_crc_errors++;
}

static int tls_setup(void)
{
    static const int suites[] = {MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256, 0};
    static const unsigned char personalization[] = "Appletini CPU0 HTTPS checkpoint";
    int result;
    mbedtls_memory_buffer_alloc_init(g_tls_heap, sizeof(g_tls_heap));
    mbedtls_ssl_init(&g_probe.ssl);
    mbedtls_ssl_config_init(&g_probe.config);
    mbedtls_x509_crt_init(&g_probe.ca);
    mbedtls_ctr_drbg_init(&g_probe.drbg);
    mbedtls_sha256_init(&g_probe.body_hash);
    g_probe.entropy_used = 0U;
    mbedtls_ctr_drbg_set_entropy_len(&g_probe.drbg, 48U);
    (void)mbedtls_platform_set_time(probe_time);
    result = mbedtls_ctr_drbg_seed(&g_probe.drbg, probe_entropy, NULL,
                                  personalization, sizeof(personalization) - 1U);
    if (result) return result;
    result = mbedtls_x509_crt_parse_der(&g_probe.ca, https_probe_ca_der, sizeof(https_probe_ca_der));
    if (result) return result;
    result = mbedtls_ssl_config_defaults(&g_probe.config, MBEDTLS_SSL_IS_CLIENT,
                                        MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (result) return result;
    mbedtls_ssl_conf_authmode(&g_probe.config, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&g_probe.config, &g_probe.ca, NULL);
    mbedtls_ssl_conf_rng(&g_probe.config, mbedtls_ctr_drbg_random, &g_probe.drbg);
    mbedtls_ssl_conf_ciphersuites(&g_probe.config, suites);
    mbedtls_ecp_set_max_ops(32U);
    result = mbedtls_ssl_setup(&g_probe.ssl, &g_probe.config);
    if (result) return result;
    result = mbedtls_ssl_set_hostname(&g_probe.ssl, g_probe.host);
    if (result) return result;
    mbedtls_ssl_set_bio(&g_probe.ssl, NULL, probe_send, probe_receive, NULL);
    return mbedtls_sha256_starts(&g_probe.body_hash, 0);
}

static void request_end(int result)
{
    g_probe.result = result;
    g_probe.verify_flags = mbedtls_ssl_get_verify_result(&g_probe.ssl);
    (void)mbedtls_sha256_finish(&g_probe.body_hash, g_probe.body_digest);
    socket_close();
    snapshot(&g_probe.phases[1], 1);
    g_probe.phase_index = 2U;
    snapshot(&g_probe.phases[2], 0);
    g_probe.state = PROBE_RECOVERY;
}

static void response_bytes(const uint8_t *bytes, size_t length)
{
    size_t at = 0U;
    while (at < length && !g_probe.headers_done) {
        if (g_probe.header_len + 1U >= sizeof(g_probe.response_header)) {
            request_end(MBEDTLS_ERR_SSL_BUFFER_TOO_SMALL); return;
        }
        g_probe.response_header[g_probe.header_len++] = (char)bytes[at++];
        if (g_probe.header_len >= 4U &&
            !memcmp(g_probe.response_header + g_probe.header_len - 4U, "\r\n\r\n", 4U)) {
            g_probe.headers_done = 1U;
            g_probe.response_header[g_probe.header_len] = '\0';
            (void)sscanf(g_probe.response_header, "HTTP/1.%*u %d", &g_probe.http_status);
        }
    }
    if (at < length) {
        (void)mbedtls_sha256_update(&g_probe.body_hash, bytes + at, length - at);
        while (at < length) {
            if (bytes[at++] != (uint8_t)(g_probe.body_bytes % 251U)) g_probe.body_valid = 0U;
            g_probe.body_bytes++;
        }
    }
}

static void print_report(void)
{
    char line[640];
    size_t peak, blocks;
    static const char *const names[] = {"baseline", "https", "recovery"};
    for (unsigned i = 0; i < 3U; ++i) {
        const probe_phase_t *p = &g_probe.phases[i];
        (void)snprintf(line, sizeof(line),
            "[https-phase] name=%s us=%llu health=%u video_frames=%lu capture_records=%lu capture_frames=%lu blits=%lu gaps=%lu oversized=%lu resync=%lu "
            "hid=%lu hid_errors=%lu xbox=%lu xbox_errors=%lu loop_max_us=%lu usb1_gap_us=%lu sp_gap_us=%lu "
            "usb0_gap_us=%lu sd_reads=%lu sd_errors=%lu sd_crc_errors=%lu sd_avg_us=%llu sd_max_us=%lu "
            "cache_misses=%lu apple_reads=%lu apple_writes=%lu\r\n",
            names[i], (unsigned long long)(p->end_us - p->start_us),
            p->video_valid_start && p->video_valid_end,
            (unsigned long)(p->frames_end - p->frames_start),
            (unsigned long)(p->video_end.records - p->video_start.records),
            (unsigned long)(p->video_end.frames - p->video_start.frames),
            (unsigned long)(p->blits_end - p->blits_start),
            (unsigned long)(p->video_end.gaps - p->video_start.gaps),
            (unsigned long)(p->video_end.oversized - p->video_start.oversized),
            (unsigned long)(p->video_end.resyncs - p->video_start.resyncs),
            (unsigned long)(p->usb_end - p->usb_start), (unsigned long)(p->usb_errors_end - p->usb_errors_start),
            (unsigned long)(p->xbox_end - p->xbox_start), (unsigned long)(p->xbox_errors_end - p->xbox_errors_start),
            (unsigned long)p->max_loop_us, (unsigned long)p->max_service_us[HTTPS_PROBE_USB1],
            (unsigned long)p->max_service_us[HTTPS_PROBE_SMARTPORT], (unsigned long)p->max_service_us[HTTPS_PROBE_USB0],
            (unsigned long)p->storage_reads, (unsigned long)p->storage_errors, (unsigned long)p->storage_crc_errors,
            (unsigned long long)(p->storage_reads ? p->storage_total_us / p->storage_reads : 0U),
            (unsigned long)p->storage_max_us, (unsigned long)(p->cache_miss_end - p->cache_miss_start),
            (unsigned long)(p->sp_reads_end - p->sp_reads_start),
            (unsigned long)(p->sp_writes_end - p->sp_writes_start));
        uart_puts(g_probe.uart, line);
    }
    mbedtls_memory_buffer_alloc_max_get(&peak, &blocks);
    (void)snprintf(line, sizeof(line),
        "[https-result] rc=%d verify=0x%08lX status=%d body=%lu body_valid=%u handshake_us=%llu "
        "step_max_us=%lu handshake_step_max_us=%lu slice_max_us=%lu handshake_calls=%lu handshake_busy_us=%llu "
        "tls_heap_peak=%lu calls=%lu io_bytes=%lu tls=%s cipher=%s\r\n",
        g_probe.result, (unsigned long)g_probe.verify_flags, g_probe.http_status,
        (unsigned long)g_probe.body_bytes, g_probe.body_valid,
        (unsigned long long)g_probe.handshake_us, (unsigned long)g_probe.max_step_us,
        (unsigned long)g_probe.max_handshake_step_us, (unsigned long)g_probe.max_slice_us,
        (unsigned long)g_probe.handshake_calls, (unsigned long long)g_probe.handshake_busy_us,
        (unsigned long)peak, (unsigned long)g_probe.calls, (unsigned long)g_probe.io_bytes,
        mbedtls_ssl_get_version(&g_probe.ssl), mbedtls_ssl_get_ciphersuite(&g_probe.ssl));
    uart_puts(g_probe.uart, line);
    uart_puts(g_probe.uart, "[https-sha256] ");
    for (unsigned i = 0; i < sizeof(g_probe.body_digest); ++i) {
        (void)snprintf(line, sizeof(line), "%02x", g_probe.body_digest[i]);
        uart_puts(g_probe.uart, line);
    }
    uart_puts(g_probe.uart, "\r\n");
    mbedtls_ssl_free(&g_probe.ssl);
    mbedtls_ssl_config_free(&g_probe.config);
    mbedtls_x509_crt_free(&g_probe.ca);
    mbedtls_ctr_drbg_free(&g_probe.drbg);
    mbedtls_sha256_free(&g_probe.body_hash);
    mbedtls_memory_buffer_alloc_free();
    mbedtls_platform_zeroize(g_probe.seed, sizeof(g_probe.seed));
    g_probe.seed_ready = 0U;
    release_ethernet();
    g_probe.state = PROBE_IDLE;
    uart_puts(g_probe.uart, "[https] done\r\n");
}

static int probe_step(void)
{
    uint64_t start = probe_us();
    int result = 0;
    uint8_t status, buffer[512];
    if (g_probe.state == PROBE_SETUP) {
        result = tls_setup();
        if (!result) {
            uint8_t port_bytes[2];
            result = mbedtls_ctr_drbg_random(&g_probe.drbg, port_bytes, sizeof(port_bytes));
            g_probe.source_port = (uint16_t)(49152U +
                ((((unsigned)port_bytes[0] << 8) | port_bytes[1]) & 16383U));
        }
        if (!result) {
            socket_close();
            result = uthernet2_write_reg(W5100_REG_RMSR, W5100_SOCKET_MEM_4_2_1_1) ||
                uthernet2_write_reg(W5100_REG_TMSR, W5100_SOCKET_MEM_4_2_1_1) ||
                uthernet2_write_reg(uthernet2_w5100_socket_reg(PROBE_SOCKET, W5100_SN_MR), W5100_SN_MR_TCP) ||
                uthernet2_w5100_write16(uthernet2_w5100_socket_reg(PROBE_SOCKET, W5100_SN_PORT), g_probe.source_port) ||
                uthernet2_w5100_socket_command(PROBE_SOCKET, W5100_CR_OPEN);
            if (result) result = MBEDTLS_ERR_SSL_INTERNAL_ERROR;
        }
        if (!result) g_probe.state = PROBE_OPEN;
    } else if (g_probe.state == PROBE_OPEN) {
        result = uthernet2_w5100_socket_status(PROBE_SOCKET, &status);
        if (result || status == W5100_SR_CLOSED) result = MBEDTLS_ERR_SSL_INTERNAL_ERROR;
        else if (status == W5100_SR_INIT) {
            /* W5100S client sequence: OPEN -> INIT -> destination -> CONNECT. */
            result = uthernet2_write_block(uthernet2_w5100_socket_reg(PROBE_SOCKET, W5100_SN_DIPR), g_probe.peer, 4U) ||
                uthernet2_w5100_write16(uthernet2_w5100_socket_reg(PROBE_SOCKET, 0x10U), g_probe.port) ||
                uthernet2_w5100_socket_command(PROBE_SOCKET, 0x04U);
            if (result) result = MBEDTLS_ERR_SSL_INTERNAL_ERROR;
            else g_probe.state = PROBE_CONNECT;
        }
    } else if (g_probe.state == PROBE_CONNECT) {
        result = uthernet2_w5100_socket_status(PROBE_SOCKET, &status);
        if (result || status == W5100_SR_CLOSED) result = MBEDTLS_ERR_SSL_INTERNAL_ERROR;
        else if (status == W5100_SR_ESTABLISHED) {
            g_probe.handshake_start = probe_us(); g_probe.state = PROBE_HANDSHAKE;
        }
    } else if (g_probe.state == PROBE_HANDSHAKE) {
        result = mbedtls_ssl_handshake_step(&g_probe.ssl);
        const uint64_t busy_us = probe_us() - start;
        update_max(&g_probe.max_handshake_step_us, busy_us);
        g_probe.handshake_busy_us += busy_us;
        g_probe.handshake_calls++;
        if (!result && mbedtls_ssl_is_handshake_over(&g_probe.ssl)) {
            g_probe.handshake_us = probe_us() - g_probe.handshake_start;
            g_probe.verify_flags = mbedtls_ssl_get_verify_result(&g_probe.ssl);
            g_probe.state = PROBE_WRITE;
        }
    } else if (g_probe.state == PROBE_WRITE) {
        result = mbedtls_ssl_write(&g_probe.ssl,
            (const unsigned char *)g_probe.request + g_probe.request_sent,
            g_probe.request_len - g_probe.request_sent);
        if (result > 0) {
            g_probe.request_sent += (size_t)result; result = 0;
            if (g_probe.request_sent == g_probe.request_len) g_probe.state = PROBE_READ;
        }
    } else if (g_probe.state == PROBE_READ) {
        result = mbedtls_ssl_read(&g_probe.ssl, buffer, sizeof(buffer));
        if (result > 0) {
            response_bytes(buffer, (size_t)result); result = 0;
            if (g_probe.state == PROBE_READ && g_probe.body_bytes >= PROBE_EXPECTED_BODY)
                request_end(g_probe.body_bytes == PROBE_EXPECTED_BODY && g_probe.body_valid &&
                            g_probe.http_status == 200 ? 0 : MBEDTLS_ERR_SSL_INTERNAL_ERROR);
        } else if (result == 0 || result == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            request_end(MBEDTLS_ERR_SSL_CONN_EOF); result = 0;
        }
    }
    g_probe.calls++;
    update_max(&g_probe.max_step_us, probe_us() - start);
    if (result && !pending_result(result)) request_end(result);
    return result;
}

void https_probe_poll(void)
{
    uint64_t now, slice_start;
    int result;
    probe_phase_t *phase;
    if (g_probe.state == PROBE_IDLE) return;
    if (g_probe.state == PROBE_DHCP) {
        char detail[100];
        uthernet2_network_config_t lease;
        result = uthernet2_dhcp_poll(&lease, detail, sizeof(detail));
        if (result) {
            uart_puts(g_probe.uart, detail); uart_puts(g_probe.uart, "\r\n");
            release_ethernet(); g_probe.state = PROBE_IDLE;
        }
        return;
    }
    if (g_probe.state == PROBE_REPORT) { print_report(); return; }
    now = probe_us();
    phase = &g_probe.phases[g_probe.phase_index];
    if (phase->last_poll) update_max(&phase->max_loop_us, now - phase->last_poll);
    phase->last_poll = now;
    storage_poll(now);
    if (g_probe.state == PROBE_BASELINE) {
        if (now - phase->start_us < PROBE_PHASE_US) return;
        snapshot(phase, 1);
        g_probe.phase_index = 1U;
        snapshot(&g_probe.phases[1], 0);
        g_probe.deadline = now + PROBE_TIMEOUT_US;
        g_probe.state = PROBE_SETUP;
    }
    if (g_probe.state == PROBE_RECOVERY) {
        if (now - phase->start_us >= PROBE_PHASE_US) {
            snapshot(phase, 1); g_probe.state = PROBE_REPORT;
        }
        return;
    }
    if (now > g_probe.deadline) { request_end(MBEDTLS_ERR_SSL_TIMEOUT); return; }

    /* Batch work that can make progress now. The time limit is checked between
     * restartable calls, so one call may extend the slice beyond this budget.
     * Never spin on an empty socket or wait for the chip to finish a SEND. */
    slice_start = probe_us();
    g_probe.io_credit = PROBE_IO_CALLS;
    for (unsigned step = 0U; step < PROBE_SLICE_STEPS; ++step) {
        const uint32_t previous_bytes = g_probe.io_bytes;
        result = probe_step();
        if (g_probe.state != PROBE_HANDSHAKE && g_probe.state != PROBE_WRITE &&
            g_probe.state != PROBE_READ) break;
        if ((result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE) &&
            (g_probe.io_bytes == previous_bytes || !g_probe.io_credit)) break;
        if (probe_us() - slice_start >= PROBE_SLICE_US) break;
    }
    update_max(&g_probe.max_slice_us, probe_us() - slice_start);
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int https_probe_command(int argc, char **argv, uint32_t uart_base)
{
    char line[200];
    uthernet2_test_result_t net;
    unsigned ip[4];
    if (argc < 1 || strcmp(argv[0], "https")) return 0;
    if (g_probe.state != PROBE_IDLE) {
        uart_puts(uart_base, "[https] busy\r\n"); return 1;
    }
    g_probe.uart = uart_base;
    if (argc == 3 && !strcmp(argv[1], "seed")) {
        g_probe.seed_ready = 0U;
        mbedtls_platform_zeroize(g_probe.seed, sizeof(g_probe.seed));
        if (strlen(argv[2]) == 96U) {
            for (unsigned i = 0; i < sizeof(g_probe.seed); ++i) {
                int high = hex_digit(argv[2][i * 2U]), low = hex_digit(argv[2][i * 2U + 1U]);
                if (high < 0 || low < 0) goto usage;
                g_probe.seed[i] = (uint8_t)(high * 16 + low);
            }
            mbedtls_platform_zeroize(argv[2], 96U);
            g_probe.seed_ready = 1U;
            uart_puts(uart_base, "[https] fresh seed accepted\r\n"); return 1;
        }
    } else if (argc == 2 && !strcmp(argv[1], "net")) {
        if (!uthernet2_test(&net)) {
            (void)snprintf(line, sizeof(line), "[https-net] link=%u ip=%u.%u.%u.%u gateway=%u.%u.%u.%u\r\n",
                net.link_up, net.config.ip[0], net.config.ip[1], net.config.ip[2], net.config.ip[3],
                net.config.gateway[0], net.config.gateway[1], net.config.gateway[2], net.config.gateway[3]);
            uart_puts(uart_base, line);
        } else uart_puts(uart_base, "[https-net] unavailable\r\n");
        return 1;
    } else if (argc == 2 && !strcmp(argv[1], "dhcp")) {
        if (claim_ethernet()) goto unavailable;
        if (uthernet2_read_network_config(&net.config) ||
            uthernet2_dhcp_start(net.config.mac, line, sizeof(line))) {
            release_ethernet(); goto unavailable;
        }
        g_probe.state = PROBE_DHCP;
        uart_puts(uart_base, "[https] DHCP started\r\n"); return 1;
    } else if (argc == 6 && !strcmp(argv[1], "run")) {
        /* argv: https run address port hostname epoch. Fixed GET /probe. */
        uint32_t epoch = (uint32_t)strtoul(argv[5], NULL, 10);
        unsigned long port = strtoul(argv[3], NULL, 10);
        if (!g_probe.seed_ready) { uart_puts(uart_base, "[https] fresh seed required\r\n"); return 1; }
        if (sscanf(argv[2], "%u.%u.%u.%u", &ip[0], &ip[1], &ip[2], &ip[3]) != 4 ||
            ip[0] > 255U || ip[1] > 255U || ip[2] > 255U || ip[3] > 255U ||
            port == 0U || port > 65535U || strlen(argv[4]) >= sizeof(g_probe.host) ||
            epoch < 1577836800UL) goto usage;
        if (claim_ethernet()) goto unavailable;
        memset(g_probe.phases, 0, sizeof(g_probe.phases));
        g_probe.epoch = epoch; g_probe.epoch_at = probe_us();
        g_probe.port = (uint16_t)port;
        for (unsigned i = 0; i < 4U; ++i) g_probe.peer[i] = (uint8_t)ip[i];
        (void)snprintf(g_probe.host, sizeof(g_probe.host), "%s", argv[4]);
        g_probe.request_len = (size_t)snprintf(g_probe.request, sizeof(g_probe.request),
            "GET /probe HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", g_probe.host);
        g_probe.request_sent = g_probe.header_len = 0U;
        g_probe.body_bytes = 0U; g_probe.headers_done = 0U; g_probe.body_valid = 1U;
        g_probe.result = 0; g_probe.http_status = 0; g_probe.verify_flags = UINT32_MAX;
        g_probe.reference_valid = g_probe.storage_cursor = 0U;
        g_probe.max_step_us = g_probe.max_handshake_step_us = g_probe.calls = 0U;
        g_probe.max_slice_us = g_probe.handshake_calls = g_probe.io_bytes = 0U;
        g_probe.handshake_busy_us = 0U;
        g_probe.handshake_us = 0U;
        g_probe.phase_index = 0U;
        g_probe.next_storage = probe_us();
        snapshot(&g_probe.phases[0], 0);
        g_probe.state = PROBE_BASELINE;
        uart_puts(uart_base, "[https] baseline started; read-only SP1 workload, then verified TLS\r\n");
        return 1;
    }
usage:
    uart_puts(uart_base, "usage: https net | dhcp | seed <96 hex digits> | run <IPv4> <port> <hostname> <UTC epoch>\r\n");
    return 1;
unavailable:
    uart_puts(uart_base, "[https] Ethernet unavailable or FTP owns it\r\n");
    return 1;
}
#endif
