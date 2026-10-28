#ifndef HTTP_SERVICE_H
#define HTTP_SERVICE_H
#include <stddef.h>
#include <stdint.h>
#include "http_core.h"

enum { HTTP_S_IDLE, HTTP_S_PREPARE, HTTP_S_DNS_OPEN, HTTP_S_DNS_SEND, HTTP_S_DNS_WAIT,
       HTTP_S_OPEN, HTTP_S_CONNECT, HTTP_S_HANDSHAKE, HTTP_S_WRITE, HTTP_S_READ,
       HTTP_S_DONE, HTTP_S_FAILED };
enum { HTTP_E_BUSY = -100, HTTP_E_ARGUMENT = -101, HTTP_E_IO = -102,
       HTTP_E_TIME = -103, HTTP_E_SEED = -104, HTTP_E_TRUST = -105,
       HTTP_E_DNS = -106, HTTP_E_TIMEOUT = -107, HTTP_E_CANCELLED = -108,
       HTTP_E_REDIRECT = -109, HTTP_E_STORAGE_BUSY = -110,
       HTTP_E_NETWORK_NOT_READY = -111 };
typedef struct {
    const char *url, *method, *headers;
    const uint8_t *body;
    size_t body_length;
    uint8_t retain;
} http_request_t;
typedef struct {
    uint8_t state, retained;
    uint16_t status;
    int result;
    uint32_t verify, body_bytes, buffered, max_step_us, max_slice_us, tls_heap_peak;
    uint64_t handshake_us, elapsed_us, seed_generation;
    char url[HTTP_URL_MAX];
    char content_type[128];
} http_status_t;

#ifdef APPLETINI_HTTP_SERVICE
/* Run once during boot, before Apple release. No seed sync or CA parsing
 * belongs to the live request poll. Files changed later require a reboot. */
int http_service_boot_prepare(void);
int http_service_start(const http_request_t *request);
void http_service_poll(void);
void http_service_cancel(void);
void http_service_storage_busy(int busy);
int http_service_active(void);
size_t http_service_read(uint8_t *out, size_t capacity);
const uint8_t *http_service_retained(size_t *length);
void http_service_status(http_status_t *out);
void http_service_clock(unsigned year, unsigned month, unsigned day,
                         unsigned hour, unsigned minute, unsigned second, int valid);
int http_service_command(int argc, char **argv, uint32_t uart);
void http_service_console_begin(uint32_t uart);
#else
static inline int http_service_boot_prepare(void) { return 0; }
static inline void http_service_poll(void) {}
static inline int http_service_active(void) { return 0; }
static inline void http_service_storage_busy(int busy) { (void)busy; }
static inline void http_service_clock(unsigned y,unsigned m,unsigned d,unsigned h,unsigned n,unsigned s,int v)
{ (void)y;(void)m;(void)d;(void)h;(void)n;(void)s;(void)v; }
static inline int http_service_command(int c,char **a,uint32_t u) { (void)c;(void)a;(void)u;return 0; }
#endif
#endif
