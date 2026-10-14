/* Exercise the production parser. The mock copies a pending ROM and applies
 * it only when frame_boundary() runs, like the two ARM CPUs. */
#include "glyph_api.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __func__, __LINE__, #condition); \
    exit(1); } } while (0)

enum { OK = 0, UNAVAILABLE = 0x60, HEADER = 0x61, COMMAND = 0x62,
       RANGE = 0x63, TRANSACTION = 0x64, INCOMPLETE = 0x65,
       BUSY = 0x66, CHECKSUM = 0x67 };
enum { BEGIN = 1, WRITE = 2, COMMIT = 3, ABORT = 4, RESTORE = 5 };
enum { AVAILABLE = 1, CUSTOM = 2, PENDING = 4, UPLOADING = 8 };

typedef struct {
    uint8_t active[4096];
    uint8_t pending[4096];
    uint8_t menu[4096];
    unsigned available, custom, queued, queued_custom;
    unsigned applies, copies, restores, apply_error, reenter;
    uint32_t generation;
} mock_t;

static mock_t mock;
static glyph_api_backend_t backend;
static uint8_t payload[16U + 512U];
static uint8_t image[4096];
static unsigned passed;

static uint16_t u16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t u32(const uint8_t *p)
{
    return (uint32_t)u16(p) | ((uint32_t)u16(p + 2) << 16);
}

static void put16(uint8_t *p, unsigned value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *p, uint32_t value)
{
    put16(p, (unsigned)value);
    put16(p + 2, (unsigned)(value >> 16));
}

static uint8_t apply(void *ctx, const uint8_t *rom)
{
    mock_t *m = ctx;
    ++m->applies;
    if (m->reenter) {
        uint8_t nested[16] = { 'A', 'G', 'L', 'Y', 1, BEGIN, 0, 0,
                               0x45, 0x23, 0, 0, 0, 0, 0, 0 };
        m->reenter = 0;
        CHECK(glyph_api_execute(nested, sizeof(nested), &backend) == BUSY);
    }
    if (m->apply_error != 0U) return (uint8_t)m->apply_error;
    if (!m->available) return UNAVAILABLE;
    if (m->queued) return BUSY;
    if (rom != NULL) {
        memcpy(m->pending, rom, sizeof(m->pending));
        m->queued_custom = 1;
        ++m->copies;
    } else {
        memcpy(m->pending, m->menu, sizeof(m->pending));
        m->queued_custom = 0;
        ++m->restores;
    }
    m->queued = 1;
    return OK;
}

static void renderer_status(void *ctx, uint16_t *flags, uint32_t *generation)
{
    mock_t *m = ctx;
    /* Unknown backend flags must never leak into the public ABI. */
    *flags = (uint16_t)(0x8000U | (m->available ? AVAILABLE : 0U) |
                       (m->custom ? CUSTOM : 0U) | (m->queued ? PENDING : 0U));
    *generation = m->generation;
}

static void frame_boundary(void)
{
    CHECK(mock.queued);
    memcpy(mock.active, mock.pending, sizeof(mock.active));
    mock.custom = mock.queued_custom;
    mock.queued = 0;
    ++mock.generation;
}

static void reset(void)
{
    unsigned i;
    glyph_api_reset();
    memset(&mock, 0, sizeof(mock));
    mock.available = 1;
    mock.generation = 0x12345678UL;
    for (i = 0; i < sizeof(image); ++i) {
        image[i] = (uint8_t)(i * 73U + (i >> 3) + 19U);
        mock.menu[i] = (uint8_t)(i * 5U + 3U);
    }
    memcpy(mock.active, mock.menu, sizeof(mock.active));
    backend.ctx = &mock;
    backend.apply = apply;
    backend.status = renderer_status;
}

static uint16_t make(unsigned operation, uint32_t id,
                     unsigned offset, unsigned count)
{
    uint16_t length = (uint16_t)(16U + (operation == WRITE ? count :
                                      (operation == COMMIT ? 4U : 0U)));
    memset(payload, 0, sizeof(payload));
    memcpy(payload, "AGLY", 4);
    payload[4] = 1;
    payload[5] = (uint8_t)operation;
    put32(payload + 8, id);
    put16(payload + 12, offset);
    put16(payload + 14, count);
    return length;
}

static uint8_t command(unsigned operation, uint32_t id)
{
    uint16_t length = make(operation, id, 0, 0);
    return glyph_api_execute(payload, length, &backend);
}

static uint8_t write_chunk(uint32_t id, unsigned offset, unsigned count)
{
    uint16_t length = make(WRITE, id, offset, count);
    CHECK(count <= 512 && offset + count <= sizeof(image));
    memcpy(payload + 16, image + offset, count);
    return glyph_api_execute(payload, length, &backend);
}

static uint8_t commit(uint32_t id, uint32_t crc)
{
    uint16_t length = make(COMMIT, id, 0, 0);
    put32(payload + 16, crc);
    return glyph_api_execute(payload, length, &backend);
}

static void upload(uint32_t id)
{
    unsigned offset;
    CHECK(command(BEGIN, id) == OK);
    for (offset = 0; offset < sizeof(image); offset += 512)
        CHECK(write_chunk(id, offset, 512) == OK);
}

static void status(uint16_t expected_flags, uint32_t id,
                   uint16_t coverage, uint8_t error, uint8_t operation)
{
    uint8_t out[32];
    memset(out, 0xCC, sizeof(out));
    glyph_api_status(out, &backend);
    CHECK(memcmp(out, "AGLY", 4) == 0);
    CHECK(out[4] == 1 && out[5] == 0 && out[6] == 16 && out[7] == 0);
    CHECK(u16(out + 8) == expected_flags);
    CHECK(u16(out + 10) == 4096 && u16(out + 12) == 512);
    CHECK(out[14] == error && out[15] == operation);
    CHECK(u32(out + 16) == id && u16(out + 20) == coverage);
    CHECK(u16(out + 22) == 0 && u32(out + 24) == mock.generation);
    CHECK(u32(out + 28) == 0);
}

static void test_crc(void)
{
    CHECK(glyph_api_crc32((const uint8_t *)"123456789", 9) == 0xCBF43926UL);
    CHECK(glyph_api_crc32(NULL, 0) == 0);
    ++passed;
}

static void test_malformed_headers(void)
{
    unsigned i;
    uint16_t length;
    reset();
    CHECK(glyph_api_execute(NULL, 0, &backend) == HEADER);
    CHECK(glyph_api_execute(NULL, 16, &backend) == HEADER);
    for (i = 0; i < 16; ++i) {
        make(BEGIN, 1, 0, 0);
        CHECK(glyph_api_execute(payload, (uint16_t)i, &backend) == HEADER);
    }
    for (i = 0; i < 8; ++i) {
        if (i == 5) continue;
        length = make(BEGIN, 1, 0, 0);
        payload[i] ^= 0x80;
        CHECK(glyph_api_execute(payload, length, &backend) == HEADER);
    }
    make(BEGIN, 1, 0, 0);
    CHECK(glyph_api_execute(payload, 17, &backend) == HEADER);
    CHECK(command(0, 1) == COMMAND);
    CHECK(command(6, 1) == COMMAND);
    CHECK(command(255, 1) == COMMAND);
    CHECK(mock.applies == 0);
    status(AVAILABLE, 0, 0, COMMAND, 255);
    ++passed;
}

static void test_ranges_and_lengths(void)
{
    const unsigned invalid[][2] = { {0, 0}, {0, 513}, {4096, 1},
                                   {4095, 2}, {65535, 1}, {65535, 65535} };
    unsigned i;
    reset();
    CHECK(command(BEGIN, 9) == OK);
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        make(WRITE, 9, invalid[i][0], invalid[i][1]);
        CHECK(glyph_api_execute(payload, 16, &backend) == RANGE);
    }
    for (i = BEGIN; i <= RESTORE; ++i) {
        if (i == WRITE) continue;
        make(i, 9, 1, 0);
        CHECK(glyph_api_execute(payload, 16, &backend) == RANGE);
        make(i, 9, 0, 1);
        CHECK(glyph_api_execute(payload, 16, &backend) == RANGE);
    }
    make(WRITE, 9, 0, 512);
    CHECK(glyph_api_execute(payload, 527, &backend) == HEADER);
    make(WRITE, 9, 0, 511);
    CHECK(glyph_api_execute(payload, 528, &backend) == HEADER);
    make(COMMIT, 9, 0, 0);
    CHECK(glyph_api_execute(payload, 16, &backend) == HEADER);
    CHECK(glyph_api_execute(payload, 21, &backend) == HEADER);
    status(AVAILABLE | UPLOADING, 9, 0, HEADER, COMMIT);
    CHECK(mock.applies == 0);
    ++passed;
}

static void test_transactions(void)
{
    reset();
    CHECK(command(BEGIN, 0) == TRANSACTION);
    CHECK(write_chunk(1, 0, 1) == TRANSACTION);
    CHECK(commit(1, 0) == TRANSACTION);
    CHECK(command(ABORT, 1) == TRANSACTION);
    CHECK(command(RESTORE, 1) == TRANSACTION);
    CHECK(command(BEGIN, 0x89ABCDEFUL) == OK);
    CHECK(write_chunk(0x89ABCDEFUL, 8, 123) == OK);
    CHECK(command(BEGIN, 0x89ABCDEFUL) == OK);
    status(AVAILABLE | UPLOADING, 0x89ABCDEFUL, 123, OK, BEGIN);
    CHECK(command(BEGIN, 2) == TRANSACTION);
    CHECK(write_chunk(2, 0, 1) == TRANSACTION);
    CHECK(commit(2, 0) == TRANSACTION);
    CHECK(command(ABORT, 2) == TRANSACTION);
    CHECK(command(RESTORE, 0) == TRANSACTION);
    status(AVAILABLE | UPLOADING, 0x89ABCDEFUL, 123, TRANSACTION, RESTORE);
    CHECK(mock.applies == 0);
    ++passed;
}

static void test_complete_atomic_transfer(void)
{
    reset();
    upload(0xCAFEBABEUL);
    status(AVAILABLE | UPLOADING, 0xCAFEBABEUL, 4096, OK, WRITE);
    CHECK(mock.applies == 0 && memcmp(mock.active, mock.menu, 4096) == 0);
    make(WRITE, 0xCAFEBABEUL, 0, 512);
    memset(payload + 16, 0xEF, 512);
    payload[6] = 1;
    CHECK(glyph_api_execute(payload, 528, &backend) == HEADER);
    payload[6] = 0;
    CHECK(glyph_api_execute(payload, 527, &backend) == HEADER);
    put16(payload + 14, 511);
    CHECK(glyph_api_execute(payload, 528, &backend) == HEADER);
    status(AVAILABLE | UPLOADING, 0xCAFEBABEUL, 4096, HEADER, WRITE);
    CHECK(commit(0xCAFEBABEUL, glyph_api_crc32(image, 4096)) == OK);
    CHECK(mock.applies == 1 && mock.copies == 1 && mock.queued);
    CHECK(memcmp(mock.pending, image, 4096) == 0);
    CHECK(memcmp(mock.active, mock.menu, 4096) == 0);
    status(AVAILABLE | PENDING, 0, 0, OK, COMMIT);
    CHECK(command(BEGIN, 2) == BUSY);
    CHECK(write_chunk(0xCAFEBABEUL, 0, 1) == TRANSACTION);
    frame_boundary();
    CHECK(memcmp(mock.active, image, 4096) == 0);
    status(AVAILABLE | CUSTOM, 0, 0, TRANSACTION, WRITE);
    CHECK(mock.generation == 0x12345679UL);
    ++passed;
}

static void test_overlap_and_holes(void)
{
    unsigned offset;
    reset();
    CHECK(command(BEGIN, 41) == OK);
    for (offset = 4096; offset != 0; offset -= 512)
        CHECK(write_chunk(41, offset - 512, 512) == OK);
    CHECK(write_chunk(41, 511, 512) == OK);
    CHECK(write_chunk(41, 4095, 1) == OK);
    status(AVAILABLE | UPLOADING, 41, 4096, OK, WRITE);
    CHECK(commit(41, glyph_api_crc32(image, 4096)) == OK);
    CHECK(memcmp(mock.pending, image, 4096) == 0);
    frame_boundary();
    CHECK(command(BEGIN, 42) == OK);
    CHECK(write_chunk(42, 0, 511) == OK);
    for (offset = 512; offset < 4096; offset += 512)
        CHECK(write_chunk(42, offset, 512) == OK);
    CHECK(write_chunk(42, 0, 511) == OK);
    CHECK(commit(42, glyph_api_crc32(image, 4096)) == INCOMPLETE);
    CHECK(mock.applies == 1 && !mock.queued);
    status(AVAILABLE | CUSTOM | UPLOADING, 42, 4095, INCOMPLETE, COMMIT);
    CHECK(write_chunk(42, 511, 1) == OK);
    CHECK(commit(42, glyph_api_crc32(image, 4096)) == OK);
    CHECK(memcmp(mock.pending, image, 4096) == 0);
    ++passed;
}

static void test_checksum_repair(void)
{
    uint32_t crc;
    reset();
    upload(71);
    crc = glyph_api_crc32(image, 4096);
    CHECK(commit(71, crc ^ 1U) == CHECKSUM);
    CHECK(mock.applies == 0 && !mock.queued);
    status(AVAILABLE | UPLOADING, 71, 4096, CHECKSUM, COMMIT);
    /* Rewriting a byte preserves coverage but changes the staged checksum. */
    image[2048] ^= 0x20;
    CHECK(write_chunk(71, 2048, 1) == OK);
    CHECK(commit(71, crc) == CHECKSUM);
    CHECK(mock.applies == 0 && memcmp(mock.active, mock.menu, 4096) == 0);
    CHECK(commit(71, glyph_api_crc32(image, 4096)) == OK);
    CHECK(memcmp(mock.pending, image, 4096) == 0);
    ++passed;
}

static void test_busy_and_backend_errors(void)
{
    reset();
    upload(81);
    mock.apply_error = BUSY;
    CHECK(commit(81, glyph_api_crc32(image, 4096)) == BUSY);
    status(AVAILABLE | UPLOADING, 81, 4096, BUSY, COMMIT);
    CHECK(!mock.queued && memcmp(mock.active, mock.menu, 4096) == 0);
    mock.apply_error = UNAVAILABLE;
    CHECK(commit(81, glyph_api_crc32(image, 4096)) == UNAVAILABLE);
    status(AVAILABLE | UPLOADING, 81, 4096, UNAVAILABLE, COMMIT);
    mock.apply_error = 0;
    mock.queued = 1;
    CHECK(write_chunk(81, 0, 1) == BUSY);
    CHECK(commit(81, glyph_api_crc32(image, 4096)) == BUSY);
    CHECK(mock.applies == 2);
    mock.queued = 0;
    CHECK(commit(81, glyph_api_crc32(image, 4096)) == OK);
    CHECK(mock.applies == 3 && memcmp(mock.pending, image, 4096) == 0);
    ++passed;
}

static void test_abort_and_reset(void)
{
    reset();
    CHECK(command(BEGIN, 91) == OK);
    CHECK(write_chunk(91, 0, 512) == OK);
    mock.available = 0;
    CHECK(command(ABORT, 91) == OK);
    status(0, 0, 0, OK, ABORT);
    CHECK(command(ABORT, 91) == TRANSACTION);
    mock.available = 1;
    CHECK(command(BEGIN, 92) == OK);
    CHECK(write_chunk(92, 512, 512) == OK);
    CHECK(commit(92, 0) == INCOMPLETE);
    glyph_api_reset();
    status(AVAILABLE, 0, 0, OK, 0);
    CHECK(mock.applies == 0 && memcmp(mock.active, mock.menu, 4096) == 0);
    CHECK(command(BEGIN, 93) == OK);
    CHECK(write_chunk(93, 0, 512) == OK);
    status(AVAILABLE | UPLOADING, 93, 512, OK, WRITE);
    CHECK(commit(93, 0) == INCOMPLETE);
    ++passed;
}

static void test_restore(void)
{
    reset();
    upload(101);
    CHECK(commit(101, glyph_api_crc32(image, 4096)) == OK);
    CHECK(command(RESTORE, 0) == BUSY);
    CHECK(mock.restores == 0 && mock.applies == 1);
    frame_boundary();
    CHECK(command(RESTORE, 0) == OK);
    status(AVAILABLE | CUSTOM | PENDING, 0, 0, OK, RESTORE);
    CHECK(mock.restores == 1 && mock.applies == 2);
    CHECK(memcmp(mock.active, image, 4096) == 0);
    frame_boundary();
    status(AVAILABLE, 0, 0, OK, RESTORE);
    CHECK(memcmp(mock.active, mock.menu, 4096) == 0);
    /* Reset must not restore an already active software ROM. */
    upload(102);
    CHECK(commit(102, glyph_api_crc32(image, 4096)) == OK);
    frame_boundary();
    glyph_api_reset();
    status(AVAILABLE | CUSTOM, 0, 0, OK, 0);
    CHECK(mock.applies == 3 && memcmp(mock.active, image, 4096) == 0);
    ++passed;
}

static void test_unavailable_and_reentry(void)
{
    uint8_t out[32];
    reset();
    mock.available = 0;
    CHECK(command(BEGIN, 111) == UNAVAILABLE);
    status(0, 0, 0, UNAVAILABLE, BEGIN);
    mock.available = 1;
    make(BEGIN, 111, 0, 0);
    CHECK(glyph_api_execute(payload, 16, NULL) == UNAVAILABLE);
    glyph_api_status(out, NULL);
    CHECK(u16(out + 8) == 0 && u32(out + 24) == 0);
    backend.apply = NULL;
    CHECK(command(BEGIN, 111) == UNAVAILABLE);
    backend.apply = apply;
    backend.status = NULL;
    CHECK(command(BEGIN, 111) == UNAVAILABLE);
    backend.status = renderer_status;
    upload(111);
    mock.available = 0;
    CHECK(write_chunk(111, 0, 1) == UNAVAILABLE);
    CHECK(commit(111, glyph_api_crc32(image, 4096)) == UNAVAILABLE);
    CHECK(mock.applies == 0);
    mock.available = 1;
    mock.reenter = 1;
    CHECK(commit(111, glyph_api_crc32(image, 4096)) == OK);
    status(AVAILABLE | PENDING, 0, 0, OK, COMMIT);
    CHECK(mock.applies == 1 && memcmp(mock.pending, image, 4096) == 0);
    ++passed;
}

static void test_byte_coverage_and_parser_stress(void)
{
    unsigned i;
    uint32_t seed = 0x90125483UL;
    reset();
    CHECK(command(BEGIN, 121) == OK);
    /* An odd stride visits every byte once, including each coverage-bit edge. */
    for (i = 0; i < 4096; ++i)
        CHECK(write_chunk(121, (i * 257U) & 4095U, 1) == OK);
    status(AVAILABLE | UPLOADING, 121, 4096, OK, WRITE);
    CHECK(commit(121, glyph_api_crc32(image, 4096)) == OK);
    frame_boundary();
    for (i = 0; i < 2000; ++i) {
        unsigned j;
        uint16_t length;
        for (j = 0; j < sizeof(payload); ++j) {
            seed = seed * 1664525UL + 1013904223UL;
            payload[j] = (uint8_t)(seed >> 24);
        }
        length = (uint16_t)(seed % (sizeof(payload) + 1U));
        CHECK(glyph_api_execute(payload, length, &backend) == HEADER);
    }
    CHECK(mock.applies == 1 && memcmp(mock.active, image, 4096) == 0);
    ++passed;
}

int main(void)
{
    test_crc();
    test_malformed_headers();
    test_ranges_and_lengths();
    test_transactions();
    test_complete_atomic_transfer();
    test_overlap_and_holes();
    test_checksum_repair();
    test_busy_and_backend_errors();
    test_abort_and_reset();
    test_restore();
    test_unavailable_and_reentry();
    test_byte_coverage_and_parser_stress();
    printf("glyph API: %u native test groups passed\n", passed);
    return 0;
}
