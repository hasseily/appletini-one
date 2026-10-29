/* SPDX-License-Identifier: MIT */
#ifndef HTTP_ENTROPY_H
#define HTTP_ENTROPY_H
#include <stdint.h>

#define HTTP_ENTROPY_BYTES 48U
#define HTTP_ENTROPY_SAMPLES 4096U
enum {
    HTTP_ENTROPY_NOT_RUN = 1,
    HTTP_ENTROPY_OK = 0,
    HTTP_ENTROPY_UNAVAILABLE = -1,
    HTTP_ENTROPY_TIMEOUT = -2,
    HTTP_ENTROPY_CONFIG = -3,
    HTTP_ENTROPY_REPEATED = -4,
    HTTP_ENTROPY_PROPORTION = -5,
    HTTP_ENTROPY_CYCLE = -6,
    HTTP_ENTROPY_HASH = -7,
    HTTP_ENTROPY_USED = -8,
    HTTP_ENTROPY_RESTORE = -9
};
typedef struct {
    uint32_t samples, changes, elapsed_us;
    uint16_t min_raw, max_raw, max_repeat, max_symbol_count;
    int result;
} http_entropy_stats_t;

/* Experimental first-use collector, once per boot, foreground CPU0 only.
 * SHA-256 conditions physical samples; these checks do not establish an
 * entropy rate or certify the XADC as a cryptographic random source.
 * The caller persists/rotates a seed before network use. Failure zeros out. */
int http_entropy_seed(uint8_t out[HTTP_ENTROPY_BYTES]);
void http_entropy_status(http_entropy_stats_t *out);
const char *http_entropy_result_name(int result);

#ifdef HTTP_ENTROPY_TEST
/* Deterministic adapter: raw is the full MSB-justified XADC word. */
uint64_t http_entropy_test_time_us(void);
int http_entropy_test_begin(void);
int http_entropy_test_sample(uint16_t *raw);
int http_entropy_test_end(void);
#endif
#endif
