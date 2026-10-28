#ifndef HTTP_BENCH_H
#define HTTP_BENCH_H
#include <stdint.h>
#ifdef APPLETINI_HTTP_BENCH
int http_bench_start(const char *url, uint32_t uart);
int http_bench_stress_start(unsigned seconds, uint32_t uart);
int http_bench_active(void);
void http_bench_cancel(void);
void http_bench_poll(void);
#else
static inline int http_bench_active(void) { return 0; }
static inline void http_bench_cancel(void) {}
static inline void http_bench_poll(void) {}
#endif
#endif
