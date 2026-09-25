/* Runtime tests of the production parser/executor; no implementation copied here. */
#include "memory_api.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __func__, __LINE__, #condition); \
    exit(1); } } while (0)

enum { UNAVAILABLE = 0x60, HEADER = 0x61, DESCRIPTOR = 0x62, RANGE = 0x63,
       OVERLAP = 0x64, PRIVATE = 0x65, BUSY = 0x66, IO = 0x67,
       SESSION_LOST = 0x68, UNSAFE = 0x69 };
enum { COPY = 1, FILL = 2, MAIN = 0, AUX = 1 };

typedef struct {
    unsigned begins, ends, reads, writes, private_checks;
    unsigned begin_error, end_error, read_fail_at, write_fail_at, fail_code;
    unsigned enabled, held, need_ramworks, max_transfer;
    unsigned inject_partial_write, reenter;
    uint32_t first_read, first_write, now;
} mock_t;

static uint8_t memory[128U * 65536U];
static uint8_t payload[8U + 16U * 16U];
static mock_t mock;
static memory_api_backend_t backend;
static unsigned passed;

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t u32(const uint8_t *p) { return (uint32_t)u16(p) | ((uint32_t)u16(p + 2) << 16); }
static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static uint8_t available(void *ctx) { return (uint8_t)((mock_t *)ctx)->enabled; }
static uint8_t begin(void *ctx, uint8_t needs_ramworks)
{
    mock_t *m = ctx;
    CHECK(!m->held);
    ++m->begins;
    m->need_ramworks = needs_ramworks;
    if (m->begin_error) return (uint8_t)m->begin_error;
    if (!m->enabled) return UNAVAILABLE;
    m->held = 1;
    return 0;
}
static uint8_t read_memory(void *ctx, uint32_t phys, uint8_t *out, uint16_t length)
{
    mock_t *m = ctx;
    CHECK(m->held && length && length <= 504U);
    CHECK(phys >= 0x200U && phys + length <= sizeof(memory));
    ++m->reads;
    if (m->reads == 1) m->first_read = phys;
    if (length > m->max_transfer) m->max_transfer = length;
    if (m->read_fail_at == m->reads) return (uint8_t)m->fail_code;
    memcpy(out, memory + phys, length);
    return 0;
}
static uint8_t write_memory(void *ctx, uint32_t phys, const uint8_t *in, uint16_t length)
{
    mock_t *m = ctx;
    CHECK(m->held && length && length <= 504U);
    CHECK(phys >= 0x200U && phys + length <= sizeof(memory));
    ++m->writes;
    if (m->writes == 1) m->first_write = phys;
    if (length > m->max_transfer) m->max_transfer = length;
    if (m->reenter) {
        m->reenter = 0;
        CHECK(memory_api_execute(payload, (uint16_t)(8U + 16U * payload[5]), &backend) == BUSY);
    }
    if (m->write_fail_at == m->writes) {
        if (m->inject_partial_write) memcpy(memory + phys, in, length / 2U);
        return (uint8_t)m->fail_code;
    }
    memcpy(memory + phys, in, length);
    return 0;
}
static uint8_t end(void *ctx)
{
    mock_t *m = ctx;
    CHECK(m->held);
    ++m->ends;
    m->held = 0;
    return (uint8_t)m->end_error;
}
static uint8_t private_required(void *ctx, uint32_t phys, uint16_t length)
{
    mock_t *m = ctx;
    CHECK(m->held);
    ++m->private_checks;
    (void)length;
    return (uint8_t)(phys < 0x20000U);
}
static uint32_t micros(void *ctx) { mock_t *m = ctx; return ++m->now; }

static void reset(void)
{
    memory_api_reset();
    memset(&mock, 0, sizeof(mock));
    memset(memory, 0xA5, sizeof(memory));
    memset(payload, 0, sizeof(payload));
    memcpy(payload, "AMEM\1\1", 6);
    mock.enabled = 1;
    mock.fail_code = IO;
    backend = (memory_api_backend_t){ .ctx = &mock, .available = available,
        .begin = begin, .read = read_memory, .write = write_memory, .end = end,
        .private_required = private_required, .micros = micros };
}

static uint8_t *desc(unsigned index, unsigned op, unsigned flags,
                     unsigned ss, unsigned sb, unsigned sa,
                     unsigned ds, unsigned db, unsigned da,
                     unsigned length, unsigned fill)
{
    uint8_t *d = payload + 8U + 16U * index;
    memset(d, 0, 16);
    d[0] = (uint8_t)op; d[1] = (uint8_t)flags;
    d[2] = (uint8_t)ss; d[3] = (uint8_t)sb; put16(d + 4, sa);
    d[6] = (uint8_t)ds; d[7] = (uint8_t)db; put16(d + 8, da);
    put16(d + 10, length); d[12] = (uint8_t)fill;
    return d;
}
static uint8_t run(void) { return memory_api_execute(payload, (uint16_t)(8U + 16U * payload[5]), &backend); }
static void untouched(void) { CHECK(mock.writes == 0 && memory[0x8000] == 0xA5); }
static void pattern(uint32_t address, unsigned size, unsigned seed)
{
    unsigned i;
    for (i = 0; i < size; ++i) memory[address + i] = (uint8_t)((i * 73U) ^ (i >> 3) ^ seed);
}

static void test_header_and_descriptor_validation(void)
{
    unsigned i;
    for (i = 0; i < 8; ++i) {
        reset(); desc(0, FILL, 1, 0, 0, 0, MAIN, 0, 0x8000, 40, 7);
        if (i < 4) payload[i] ^= 0x20;
        else if (i == 4) payload[4] = 2;
        else if (i == 5) payload[5] = 0;
        else payload[i] = 1;
        CHECK(memory_api_execute(payload, 24, &backend) == HEADER);
        CHECK(mock.begins == 0); untouched();
    }
    reset(); CHECK(memory_api_execute(NULL, 0, &backend) == HEADER); untouched();
    reset(); desc(0, FILL, 1, 0, 0, 0, MAIN, 0, 0x8000, 40, 7);
    CHECK(memory_api_execute(payload, 23, &backend) == HEADER);
    CHECK(memory_api_execute(payload, 25, &backend) == HEADER);
    payload[5] = 17;
    CHECK(memory_api_execute(payload, sizeof(payload), &backend) == HEADER);
    for (i = 0; i < 7; ++i) {
        uint8_t *d;
        reset(); d = desc(0, FILL, 1, 0, 0, 0, MAIN, 0, 0x8000, 40, 7);
        if (i == 0) d[0] = 3;
        else if (i == 1) d[1] = 2;
        else if (i < 5) d[11 + i] = 1;
        else if (i == 5) d[2] = AUX;
        else d[4] = 1;
        CHECK(run() == DESCRIPTOR); CHECK(mock.begins == 0); untouched();
    }
    ++passed;
}

static void test_range_and_whole_list_validation(void)
{
    static const unsigned bad[][4] = {
        {MAIN, 1, 0x8000, 1}, {AUX, 127, 0x8000, 1},
        {2, 0, 0x8000, 1}, {MAIN, 0, 0x1ff, 1},
        {MAIN, 0, 0xbfff, 2}, {AUX, 1, 0xffff, 2},
        {AUX, 1, 0x8000, 0xffff}, {MAIN, 0, 0x8000, 0}
    };
    unsigned i;
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        reset(); payload[5] = 2;
        desc(0, FILL, 1, 0, 0, 0, MAIN, 0, 0x8000, 40, 7);
        desc(1, FILL, 0, 0, 0, 0, bad[i][0], bad[i][1], bad[i][2], bad[i][3], 7);
        CHECK(run() == RANGE); CHECK(mock.begins == 0); untouched();
    }
    reset(); desc(0, COPY, 1, MAIN, 0, 0x7fff, MAIN, 0, 0x8000, 2, 0);
    CHECK(run() == OVERLAP); CHECK(mock.begins == 0); untouched();
    reset(); desc(0, COPY, 1, MAIN, 0, 0x8000, MAIN, 0, 0x7fff, 2, 0);
    CHECK(run() == OVERLAP); CHECK(mock.begins == 0); untouched();
    reset(); desc(0, COPY, 1, MAIN, 0, 0x8000, MAIN, 0, 0x8000, 2, 0);
    CHECK(run() == OVERLAP); CHECK(mock.begins == 0); untouched();
    reset(); desc(0, COPY, 1, AUX, 127, 0x8000, MAIN, 0, 0x8000, 2, 0);
    CHECK(run() == RANGE); CHECK(mock.begins == 0); untouched();
    reset(); desc(0, COPY, 1, AUX, 1, 0xbfff, MAIN, 0, 0x8000, 2, 0);
    CHECK(run() == RANGE); CHECK(mock.begins == 0); untouched();
    reset(); desc(0, COPY, 1, AUX, 1, 0x8000, MAIN, 0, 0x8000, 2, 1);
    CHECK(run() == DESCRIPTOR); CHECK(mock.begins == 0); untouched();
    ++passed;
}

static void test_copy_all_alignments_and_banks(void)
{
    unsigned s, d;
    for (s = 0; s < 8; ++s) for (d = 0; d < 8; ++d) {
        reset(); pattern(0x18000U + s, 1041, s + d);
        desc(0, COPY, 1, AUX, 0, 0x8000 + s, AUX, 126, 0x9000 + d, 1041, 0);
        CHECK(run() == 0);
        CHECK(!memcmp(memory + 0x18000U + s, memory + 0x7f9000U + d, 1041));
        CHECK(memory[0x7f8fffU + d] == 0xA5 && memory[0x7f9000U + d + 1041] == 0xA5);
        CHECK(mock.first_read == 0x18000U + s && mock.first_write == 0x7f9000U + d);
        CHECK(mock.max_transfer <= 504 && mock.writes == 3 && mock.reads == 3);
        CHECK(mock.begins == 1 && mock.ends == 1 && !mock.held && mock.need_ramworks);
    }
    ++passed;
}

static void test_fill_and_ordered_dependencies(void)
{
    unsigned i;
    reset(); payload[5] = 3;
    desc(0, FILL, 1, 0, 0, 0, AUX, 1, 0x8001, 1057, 0x3c);
    desc(1, COPY, 1, AUX, 1, 0x8001, MAIN, 0, 0x9003, 1057, 0);
    desc(2, COPY, 1, MAIN, 0, 0x9003, AUX, 2, 0x8005, 1057, 0);
    CHECK(run() == 0);
    for (i = 0; i < 1057; ++i) CHECK(memory[0x28001U + i] == 0x3c &&
        memory[0x9003U + i] == 0x3c && memory[0x38005U + i] == 0x3c);
    CHECK(memory[0x9002] == 0xA5 && memory[0x9003 + 1057] == 0xA5);
    CHECK(mock.begins == 1 && mock.ends == 1 && mock.writes == 9 && mock.reads == 6);
    ++passed;
}

static void test_private_prevalidation_under_hold(void)
{
    reset(); payload[5] = 2;
    desc(0, FILL, 0, 0, 0, 0, AUX, 1, 0x8000, 8, 1);
    desc(1, FILL, 0, 0, 0, 0, MAIN, 0, 0x2000, 8, 2);
    CHECK(run() == PRIVATE); untouched(); CHECK(mock.begins == 1 && mock.ends == 1);
    reset(); desc(0, FILL, 1, 0, 0, 0, MAIN, 0, 0x2000, 8, 2);
    CHECK(run() == 0 && memory[0x2000] == 2);
    reset(); desc(0, FILL, 0, 0, 0, 0, AUX, 1, 0x2000, 8, 2);
    CHECK(run() == 0 && memory[0x22000] == 2);
    ++passed;
}

static void test_unavailable_and_backend_failures(void)
{
    reset(); desc(0, FILL, 1, 0, 0, 0, MAIN, 0, 0x8000, 1009, 9);
    mock.enabled = 0;
    CHECK(run() == UNAVAILABLE && mock.begins == 1); untouched();
    reset(); desc(0, FILL, 1, 0, 0, 0, MAIN, 0, 0x8000, 1009, 9);
    mock.begin_error = UNAVAILABLE;
    CHECK(run() == UNAVAILABLE && mock.ends == 0); untouched();
    reset(); desc(0, FILL, 1, 0, 0, 0, MAIN, 0, 0x8000, 1009, 9);
    mock.enabled = 0; mock.begin_error = UNSAFE;
    CHECK(run() == UNSAFE && mock.begins == 1 && mock.ends == 0); untouched();
    reset(); desc(0, COPY, 1, AUX, 1, 0x8000, MAIN, 0, 0x8000, 1009, 0);
    mock.read_fail_at = 2;
    CHECK(run() == IO && mock.reads == 2 && mock.writes == 1 && mock.ends == 1);
    reset(); desc(0, FILL, 1, 0, 0, 0, MAIN, 0, 0x8000, 1009, 9);
    mock.write_fail_at = 2; mock.inject_partial_write = 1;
    CHECK(run() == IO && mock.writes == 2 && mock.ends == 1);
    CHECK(memory[0x8000 + 503] == 9 && memory[0x8000 + 504 + 251] == 9);
    CHECK(memory[0x8000 + 504 + 252] == 0xA5); /* No retry of uncertain chunk. */
    reset(); desc(0, FILL, 1, 0, 0, 0, MAIN, 0, 0x8000, 8, 9);
    mock.end_error = SESSION_LOST;
    CHECK(run() == SESSION_LOST && mock.writes == 1 && mock.ends == 1);
    reset(); desc(0, FILL, 1, 0, 0, 0, MAIN, 0, 0x8000, 1009, 9);
    mock.write_fail_at = 2; mock.fail_code = SESSION_LOST;
    CHECK(run() == SESSION_LOST && mock.writes == 2 && mock.ends == 1);
    ++passed;
}

static void test_status_progress_reset_and_busy(void)
{
    uint8_t status[32];
    reset(); memory_api_status(status, &backend);
    CHECK(!memcmp(status, "AMEM\1\0\20\20", 8));
    CHECK(u16(status + 8) == 7 && u16(status + 10) == 0x200 && u16(status + 12) == 0xc000);
    CHECK(status[14] == 126 && status[15] == 1 && u16(status + 20) == 512);
    CHECK(status[22] == 0 && status[23] == 0 && u32(status + 28) == 0);
    mock.enabled = 0; memory_api_status(status, &backend); CHECK(status[15] == 0);

    reset(); payload[5] = 2;
    desc(0, FILL, 1, 0, 0, 0, MAIN, 0, 0x8000, 7, 3);
    desc(1, FILL, 0, 0, 0, 0, AUX, 1, 0x8000, 1009, 5);
    mock.write_fail_at = 3; mock.inject_partial_write = 1;
    CHECK(run() == IO);
    memory_api_status(status, &backend);
    CHECK(status[22] == IO && status[23] == 1 && u32(status + 28) == 511);
    CHECK(u32(status + 24) > 0);
    CHECK(mock.writes == 3 && mock.begins == 1 && mock.ends == 1);
    memory_api_reset(); memory_api_status(status, &backend);
    CHECK(status[22] == 0 && status[23] == 0 && u32(status + 28) == 0 && u32(status + 24) == 0);

    reset(); desc(0, FILL, 1, 0, 0, 0, MAIN, 0, 0x8000, 7, 3);
    mock.reenter = 1; CHECK(run() == 0);
    memory_api_status(status, &backend);
    CHECK(status[22] == 0 && status[23] == 1 && u32(status + 28) == 7);
    CHECK(mock.begins == 1 && mock.ends == 1 && mock.writes == 1);

    reset(); desc(0, FILL, 1, 0, 0, 0, MAIN, 0, 0x8000, 7, 3);
    mock.write_fail_at = 1; mock.end_error = UNSAFE;
    CHECK(run() == UNSAFE); memory_api_status(status, &backend);
    CHECK(status[22] == UNSAFE && status[23] == 0 && u32(status + 28) == 0);
    ++passed;
}

static void test_limits_and_large_exact_range(void)
{
    unsigned i;
    reset(); payload[5] = 16;
    for (i = 0; i < 16; ++i)
        desc(i, FILL, 0, 0, 0, 0, AUX, 1, 0x8000 + i, 1, i * 17);
    CHECK(run() == 0 && mock.writes == 16);
    for (i = 0; i < 16; ++i) CHECK(memory[0x28000U + i] == i * 17);
    reset(); pattern(0x7f0200U, 0xbe00, 219);
    desc(0, COPY, 1, AUX, 126, 0x200, MAIN, 0, 0x200, 0xbe00, 0);
    CHECK(run() == 0 && !memcmp(memory + 0x7f0200U, memory + 0x200, 0xbe00));
    CHECK(memory[0x1ff] == 0xA5 && memory[0xc000] == 0xA5);
    ++passed;
}

int main(void)
{
    test_header_and_descriptor_validation();
    test_range_and_whole_list_validation();
    test_copy_all_alignments_and_banks();
    test_fill_and_ordered_dependencies();
    test_private_prevalidation_under_hold();
    test_unavailable_and_backend_failures();
    test_status_progress_reset_and_busy();
    test_limits_and_large_exact_range();
    printf("PASS memory API host runtime: %u groups\n", passed);
    return 0;
}
