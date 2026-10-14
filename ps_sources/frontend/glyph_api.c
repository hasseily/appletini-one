#include "glyph_api.h"

#include <stddef.h>
#include <string.h>

static uint8_t staged_rom[GLYPH_API_ROM_SIZE];
static uint8_t covered[GLYPH_API_ROM_SIZE / 8U];
static uint32_t transaction;
static uint16_t covered_bytes;
static uint8_t last_error;
static uint8_t last_operation;
static uint8_t executing;

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)get16(p) | ((uint32_t)get16(p + 2) << 16);
}

static void put16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *p, uint32_t value)
{
    put16(p, (uint16_t)value);
    put16(p + 2, (uint16_t)(value >> 16));
}

static void discard_upload(void)
{
    transaction = 0U;
    covered_bytes = 0U;
    memset(covered, 0, sizeof(covered));
}

static void backend_status(const glyph_api_backend_t *backend,
                            uint16_t *flags, uint32_t *generation)
{
    *flags = 0U;
    *generation = 0U;
    if (backend != NULL && backend->status != NULL)
        backend->status(backend->ctx, flags, generation);
    *flags &= GLYPH_API_AVAILABLE | GLYPH_API_CUSTOM_ACTIVE | GLYPH_API_PENDING;
    if (backend == NULL || backend->apply == NULL)
        *flags &= (uint16_t)~GLYPH_API_AVAILABLE;
}

void glyph_api_reset(void)
{
    discard_upload();
    last_error = GLYPH_API_OK;
    last_operation = 0U;
}

uint32_t glyph_api_crc32(const uint8_t *data, uint16_t length)
{
    uint32_t crc = 0xFFFFFFFFUL;
    uint16_t i;
    uint8_t bit;

    for (i = 0U; i < length; ++i) {
        crc ^= data[i];
        for (bit = 0U; bit < 8U; ++bit)
            crc = (crc >> 1) ^ ((crc & 1U) ? 0xEDB88320UL : 0U);
    }
    return crc ^ 0xFFFFFFFFUL;
}

void glyph_api_status(uint8_t out[GLYPH_API_STATUS_SIZE],
                       const glyph_api_backend_t *backend)
{
    uint16_t flags;
    uint32_t generation;

    backend_status(backend, &flags, &generation);
    if (transaction != 0U) flags |= GLYPH_API_UPLOADING;
    memset(out, 0, GLYPH_API_STATUS_SIZE);
    memcpy(out, "AGLY", 4U);
    out[4] = GLYPH_API_MAJOR;
    out[5] = GLYPH_API_MINOR;
    out[6] = GLYPH_API_HEADER_SIZE;
    put16(out + 8, flags);
    put16(out + 10, GLYPH_API_ROM_SIZE);
    put16(out + 12, GLYPH_API_MAX_CHUNK);
    out[14] = last_error;
    out[15] = last_operation;
    put32(out + 16, transaction);
    put16(out + 20, covered_bytes);
    put32(out + 24, generation);
}

uint8_t glyph_api_execute(const uint8_t *payload, uint16_t length,
                          const glyph_api_backend_t *backend)
{
    uint8_t error = GLYPH_API_OK;
    uint8_t operation;
    uint32_t id;
    uint16_t offset;
    uint16_t count;
    uint16_t flags;
    uint32_t generation;

    if (executing != 0U) return GLYPH_API_BUSY;
    executing = 1U;
    last_operation = 0U;
    if (payload == NULL || length < GLYPH_API_HEADER_SIZE ||
        memcmp(payload, "AGLY", 4U) != 0 ||
        payload[4] != GLYPH_API_MAJOR || payload[6] != 0U || payload[7] != 0U) {
        error = GLYPH_API_BAD_HEADER;
        goto finished;
    }
    operation = payload[5];
    last_operation = operation;
    id = get32(payload + 8);
    offset = get16(payload + 12);
    count = get16(payload + 14);
    if (operation < GLYPH_API_BEGIN || operation > GLYPH_API_RESTORE) {
        error = GLYPH_API_BAD_COMMAND;
        goto finished;
    }
    if ((operation == GLYPH_API_WRITE &&
         (count == 0U || count > GLYPH_API_MAX_CHUNK ||
          (uint32_t)offset + count > GLYPH_API_ROM_SIZE)) ||
        (operation != GLYPH_API_WRITE && (offset != 0U || count != 0U))) {
        error = GLYPH_API_RANGE;
        goto finished;
    }
    if (length != GLYPH_API_HEADER_SIZE +
                  (operation == GLYPH_API_WRITE ? count :
                   (operation == GLYPH_API_COMMIT ? 4U : 0U))) {
        error = GLYPH_API_BAD_HEADER;
        goto finished;
    }
    if ((operation == GLYPH_API_RESTORE && (id != 0U || transaction != 0U)) ||
        (operation != GLYPH_API_RESTORE &&
         (id == 0U || (operation == GLYPH_API_BEGIN ?
                      (transaction != 0U && transaction != id) : transaction != id)))) {
        error = GLYPH_API_TRANSACTION;
        goto finished;
    }
    /* ABORT is local cleanup and still works when the backend is unavailable. */
    if (operation == GLYPH_API_ABORT) {
        discard_upload();
        goto finished;
    }
    backend_status(backend, &flags, &generation);
    if ((flags & GLYPH_API_AVAILABLE) == 0U) {
        error = GLYPH_API_UNAVAILABLE;
        goto finished;
    }
    if ((flags & GLYPH_API_PENDING) != 0U) {
        error = GLYPH_API_BUSY;
        goto finished;
    }
    switch (operation) {
    case GLYPH_API_BEGIN:
        if (transaction == 0U) {
            discard_upload();
            transaction = id;
        }
        break;
    case GLYPH_API_WRITE: {
        uint16_t i;
        memcpy(staged_rom + offset, payload + GLYPH_API_HEADER_SIZE, count);
        for (i = offset; i < (uint16_t)(offset + count); ++i) {
            uint8_t mask = (uint8_t)(1U << (i & 7U));
            if ((covered[i >> 3] & mask) == 0U) {
                covered[i >> 3] |= mask;
                ++covered_bytes;
            }
        }
        break;
    }
    case GLYPH_API_COMMIT:
        if (covered_bytes != GLYPH_API_ROM_SIZE) {
            error = GLYPH_API_INCOMPLETE;
            break;
        }
        if (glyph_api_crc32(staged_rom, GLYPH_API_ROM_SIZE) !=
            get32(payload + GLYPH_API_HEADER_SIZE)) {
            error = GLYPH_API_CHECKSUM;
            break;
        }
        /* apply() must check readiness again and copy the image on success. */
        error = backend->apply(backend->ctx, staged_rom);
        if (error == GLYPH_API_OK) discard_upload();
        break;
    case GLYPH_API_RESTORE:
        error = backend->apply(backend->ctx, NULL);
        break;
    default:
        break;
    }

finished:
    last_error = error;
    executing = 0U;
    return error;
}
