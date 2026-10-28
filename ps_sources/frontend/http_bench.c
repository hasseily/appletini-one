/* Test-only workload monitor using the original checkpoint's counters. */
#ifdef APPLETINI_HTTP_BENCH
#include "http_bench.h"
#include "http_service.h"
#include "https_probe.h"
#include "../lib/uart.h"
#include "../lib/crc32.h"
#include "apple_fb_handoff.h"
#include "compositor.h"
#include "smartport_service.h"
#include "usb_hid_service.h"
#include "usb_xbox_360.h"
#include "xiltimer.h"
#include <stdio.h>
#include <string.h>
#define PROBE_STORAGE_BLOCKS 64U
#define PROBE_STORAGE_PERIOD_US 10000ULL
#define PROBE_PHASE_US 3000000ULL
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
} http_bench_phase_t;
static struct {
    uint8_t active, phase_index;
    uint32_t uart, storage_cursor;
    uint64_t next_storage, reference_valid;
    uint32_t reference_crc[PROBE_STORAGE_BLOCKS];
    uint8_t storage_buffer[512] __attribute__((aligned(32)));
    char url[HTTP_URL_MAX];
    uint32_t stress_seconds, stress_requests, stress_start_errors;
    http_bench_phase_t phases[3];
} g_bench;
static uint64_t probe_us(void)
{
    XTime ticks;
    XTime_GetTime(&ticks);
    return (uint64_t)ticks * 1000000ULL / (uint64_t)(COUNTS_PER_SECOND);
}
static void update_max(uint32_t *maximum, uint64_t value)
{ if (value > *maximum) *maximum = value > UINT32_MAX ? UINT32_MAX : (uint32_t)value; }
static void snapshot(http_bench_phase_t *phase, int end)
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
    http_bench_phase_t *phase;
    uint64_t now;
    if (!g_bench.active ||
        service >= HTTPS_PROBE_SERVICE_COUNT) return;
    phase = &g_bench.phases[g_bench.phase_index];
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
    http_bench_phase_t *phase = &g_bench.phases[g_bench.phase_index];
    uint64_t start;
    if (now < g_bench.next_storage) return;
    g_bench.next_storage = now + PROBE_STORAGE_PERIOD_US;
    index = g_bench.storage_cursor++ % PROBE_STORAGE_BLOCKS;
    start = probe_us();
    /* Read-only SD-backed SmartPort image workload, not an Apple bus request. */
    result = smartport_service_read_block(1U, index, g_bench.storage_buffer, 512U, &actual);
    start = probe_us() - start;
    phase->storage_total_us += start;
    update_max(&phase->storage_max_us, start);
    if (result || actual != 512U) { phase->storage_errors++; return; }
    phase->storage_reads++;
    crc = crc32_finish(crc32_update(crc32_init(), g_bench.storage_buffer, 512U));
    if (!(g_bench.reference_valid & (1ULL << index))) {
        g_bench.reference_crc[index] = crc;
        g_bench.reference_valid |= 1ULL << index;
    } else if (g_bench.reference_crc[index] != crc) phase->storage_crc_errors++;
}

static void print_phases(void)
{
    char line[1024];
    static const char *const names[] = {"baseline", "https", "recovery"};
    for (unsigned i = 0; i < 3U; ++i) {
        const http_bench_phase_t *p = &g_bench.phases[i];
        (void)snprintf(line, sizeof(line),
            "[http-phase] name=%s us=%llu health=%u video_frames=%lu capture_records=%lu capture_frames=%lu blits=%lu gaps=%lu oversized=%lu resync=%lu "
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
        uart_puts(g_bench.uart, line);
    }
}

int http_bench_active(void) { return g_bench.active; }
void http_bench_cancel(void) { g_bench.active=0; }
int http_bench_start(const char *url, uint32_t uart)
{
    http_url_t parsed;
    if (g_bench.active || http_service_active()) return HTTP_E_BUSY;
    if (http_url_parse(url, &parsed)) return HTTP_E_ARGUMENT;
    memset(&g_bench, 0, sizeof(g_bench));
    g_bench.active = 1; g_bench.uart = uart;
    strcpy(g_bench.url, url);
    snapshot(&g_bench.phases[0], 0);
    return 0;
}
int http_bench_stress_start(unsigned seconds, uint32_t uart)
{
    if(seconds<10U||seconds>600U)return HTTP_E_ARGUMENT;
    int result=http_bench_start("https://rsa2048.badssl.com/",uart);
    if(!result)g_bench.stress_seconds=seconds;
    return result;
}
static void stress_request(void)
{
    const char *url=(g_bench.stress_requests&1U)?"https://rsa4096.badssl.com/":"https://rsa2048.badssl.com/";
    http_request_t request={url,"GET","",NULL,0,0};
    int result=http_service_start(&request);
    char line[144];
    ++g_bench.stress_requests;
    if(result)++g_bench.stress_start_errors;
    else http_service_console_begin(g_bench.uart);
    (void)snprintf(line,sizeof(line),"[http-stress] request=%lu url=%s start_rc=%d\r\n",
        (unsigned long)g_bench.stress_requests,url,result);
    uart_puts(g_bench.uart,line);
}
void http_bench_poll(void)
{
    if (!g_bench.active) return;
    uint64_t now = probe_us();
    http_bench_phase_t *phase = &g_bench.phases[g_bench.phase_index];
    if (phase->last_poll) update_max(&phase->max_loop_us, now - phase->last_poll);
    phase->last_poll = now;
    /* Stress uses real Apple ProDOS writes. The old read-only reference
     * workload must not flag those deliberate data changes as corruption. */
    if(!g_bench.stress_seconds)storage_poll(now);
    if (g_bench.phase_index == 0 && now - phase->start_us >= PROBE_PHASE_US) {
        snapshot(phase, 1);
        g_bench.phase_index = 1;
        snapshot(&g_bench.phases[1], 0);
        if(g_bench.stress_seconds){stress_request();return;}
        http_request_t request = {g_bench.url, "GET", "", NULL, 0, 0};
        int result = http_service_start(&request);
        if (!result) http_service_console_begin(g_bench.uart);
        else {
            char line[64];
            snprintf(line, sizeof(line), "[http-bench] start rc=%d\r\n", result);
            uart_puts(g_bench.uart, line);
        }
    } else if (g_bench.phase_index == 1 && !http_service_active()) {
        if(g_bench.stress_seconds&&now-phase->start_us<(uint64_t)g_bench.stress_seconds*1000000ULL) {
            stress_request();return;
        }
        snapshot(phase, 1);
        g_bench.phase_index = 2;
        snapshot(&g_bench.phases[2], 0);
    } else if (g_bench.phase_index == 2 && now - phase->start_us >= PROBE_PHASE_US) {
        snapshot(phase, 1);
        print_phases();
        if(g_bench.stress_seconds) {
            char line[100];
            (void)snprintf(line,sizeof(line),"[http-stress-summary] requests=%lu start_errors=%lu\r\n",
                (unsigned long)g_bench.stress_requests,(unsigned long)g_bench.stress_start_errors);
            uart_puts(g_bench.uart,line);
        }
        g_bench.active = 0;
        uart_puts(g_bench.uart, "[http-bench] done\r\n");
    }
}
#endif
