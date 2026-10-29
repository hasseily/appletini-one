/* SPDX-License-Identifier: MIT */
#include "http_entropy.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *test;
static uint64_t time_us;
static uint32_t random_state = 0x319A27D5U;
static unsigned samples, begins, ends;

uint64_t http_entropy_test_time_us(void)
{ return !strcmp(test,"frozen-time") ? 0U : time_us++; }
int http_entropy_test_begin(void)
{
    ++begins;
    return !strcmp(test,"unavailable") ? HTTP_ENTROPY_UNAVAILABLE : 0;
}
int http_entropy_test_sample(uint16_t *raw)
{
    unsigned at = samples++;
    if (!strcmp(test,"timeout") && at == 100U) return HTTP_ENTROPY_TIMEOUT;
    if (!strcmp(test,"wall-timeout")) time_us += 1000U;
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    unsigned code = 2048U + (random_state & 3U);
    if (!strcmp(test,"two-codes")) code = 2048U + (random_state & 2U);
    if (!strcmp(test,"stuck") || !strcmp(test,"padding")) code = 2048U;
    if (!strcmp(test,"cycle")) code = 2048U + at % 4U;
    if (!strcmp(test,"cycle16")) code = 2048U + at % 16U;
    if (!strcmp(test,"biased")) code = 2048U + (at % 64U == 0U);
    if (!strcmp(test,"upper-only")) code = 2048U + (random_state & 3U)*16U;
    *raw = (uint16_t)(code << 4);
    if (!strcmp(test,"padding") || !strcmp(test,"varied-padding")) *raw |= (uint16_t)(at & 15U);
    return 0;
}
int http_entropy_test_end(void)
{ ++ends; return !strcmp(test,"restore") ? HTTP_ENTROPY_RESTORE : 0; }

int main(int argc, char **argv)
{
    assert(argc == 2); test = argv[1];
    http_entropy_stats_t before, stats;
    http_entropy_status(&before);
    assert(before.result == HTTP_ENTROPY_NOT_RUN && before.samples == 0U);
    if (!strcmp(test,"different")) random_state ^= 0x9B27AD41U;
    uint8_t out[HTTP_ENTROPY_BYTES]; memset(out,0xA5,sizeof(out));
    int result = http_entropy_seed(out);
    http_entropy_status(&stats);
    assert(begins == 1U && ends == 1U && stats.result == result);
    if (!strcmp(test,"varied") || !strcmp(test,"varied-padding") ||
        !strcmp(test,"different") || !strcmp(test,"two-codes")) {
        assert(result == 0 && stats.samples == HTTP_ENTROPY_SAMPLES);
        assert(stats.changes >= HTTP_ENTROPY_SAMPLES/8U && stats.min_raw < stats.max_raw);
        unsigned nonzero = 0; for (unsigned i = 0; i < sizeof(out); ++i) nonzero |= out[i];
        assert(nonzero);
    } else {
        int expected = HTTP_ENTROPY_PROPORTION;
        if (!strcmp(test,"stuck") || !strcmp(test,"padding")) expected = HTTP_ENTROPY_REPEATED;
        if (!strcmp(test,"cycle") || !strcmp(test,"cycle16")) expected = HTTP_ENTROPY_CYCLE;
        if (!strcmp(test,"timeout") || !strcmp(test,"wall-timeout") ||
            !strcmp(test,"frozen-time")) expected = HTTP_ENTROPY_TIMEOUT;
        if (!strcmp(test,"unavailable")) expected = HTTP_ENTROPY_UNAVAILABLE;
        if (!strcmp(test,"restore")) expected = HTTP_ENTROPY_RESTORE;
        assert(result == expected);
        for (unsigned i = 0; i < sizeof(out); ++i) assert(out[i] == 0U);
    }
    printf("%s result=%s samples=%lu changes=%lu elapsed=%lu seed=",test,
           http_entropy_result_name(result),(unsigned long)stats.samples,
           (unsigned long)stats.changes,(unsigned long)stats.elapsed_us);
    for (unsigned i = 0; i < sizeof(out); ++i) printf("%02x",out[i]);
    puts("");
    memset(out,0xA5,sizeof(out));
    assert(http_entropy_seed(out) == HTTP_ENTROPY_USED);
    for (unsigned i = 0; i < sizeof(out); ++i) assert(out[i] == 0U);
    assert(begins == 1U && ends == 1U);
    assert(http_entropy_seed(NULL) == HTTP_ENTROPY_UNAVAILABLE);
    return 0;
}
