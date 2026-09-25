#include "memory_api.h"

#include <stddef.h>
#include <string.h>

typedef struct {
    uint32_t source;
    uint32_t destination;
    uint16_t length;
    uint8_t operation;
    uint8_t flags;
    uint8_t fill;
} memory_descriptor_t;

static uint8_t last_error;
static uint8_t last_completed_descriptors;
static uint32_t last_completed_bytes;
static uint32_t last_elapsed_us;
static uint8_t executing;

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
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

static uint8_t endpoint(const uint8_t *p, uint16_t length, uint32_t *phys)
{
    uint16_t address = get16(p + 2);
    uint32_t bank;

    if (p[0] > MEMORY_API_AUX ||
        (p[0] == MEMORY_API_MAIN && p[1] != 0U) ||
        (p[0] == MEMORY_API_AUX && p[1] > MEMORY_API_MAX_AUX_BANK)) {
        return MEMORY_API_RANGE;
    }
    if (length == 0U || address < MEMORY_API_MIN_ADDR ||
        (uint32_t)address + length > MEMORY_API_LIMIT_ADDR) {
        return MEMORY_API_RANGE;
    }
    bank = (p[0] == MEMORY_API_MAIN) ? 0U : (uint32_t)p[1] + 1U;
    *phys = (bank << 16) | address;
    return MEMORY_API_OK;
}

void memory_api_reset(void)
{
    last_error = MEMORY_API_OK;
    last_completed_descriptors = 0U;
    last_completed_bytes = 0U;
    last_elapsed_us = 0U;
}

void memory_api_status(uint8_t out[MEMORY_API_STATUS_SIZE],
                        const memory_api_backend_t *backend)
{
    memset(out, 0, MEMORY_API_STATUS_SIZE);
    memcpy(out, "AMEM", 4U);
    out[4] = MEMORY_API_MAJOR;
    out[5] = MEMORY_API_MINOR;
    out[6] = MEMORY_API_DESCRIPTOR_SIZE;
    out[7] = MEMORY_API_MAX_DESCRIPTORS;
    put16(out + 8, MEMORY_API_FEATURE_COPY | MEMORY_API_FEATURE_FILL |
                    MEMORY_API_FEATURE_PRIVATE);
    put16(out + 10, MEMORY_API_MIN_ADDR);
    put16(out + 12, MEMORY_API_LIMIT_ADDR);
    out[14] = MEMORY_API_MAX_AUX_BANK;
    out[15] = backend->available(backend->ctx) ? 1U : 0U;
    put32(out + 16, backend->micros(backend->ctx));
    put16(out + 20, MEMORY_API_DMA_CHUNK);
    out[22] = last_error;
    out[23] = last_completed_descriptors;
    put32(out + 24, last_elapsed_us);
    put32(out + 28, last_completed_bytes);
}

uint8_t memory_api_execute(const uint8_t *payload, uint16_t length,
                           const memory_api_backend_t *backend)
{
    memory_descriptor_t descriptors[MEMORY_API_MAX_DESCRIPTORS];
    uint8_t data[MEMORY_API_COPY_CHUNK];
    uint8_t count = 0U;
    uint8_t needs_ramworks = 0U;
    uint8_t held = 0U;
    uint8_t error = MEMORY_API_OK;
    uint8_t i;
    uint32_t started;

    if (executing != 0U) {
        return MEMORY_API_BUSY;
    }
    executing = 1U;
    memory_api_reset();
    started = backend->micros(backend->ctx);
    if (payload == NULL || length < MEMORY_API_HEADER_SIZE ||
        memcmp(payload, "AMEM", 4U) != 0 ||
        payload[4] != MEMORY_API_MAJOR || payload[6] != 0U || payload[7] != 0U ||
        payload[5] == 0U || payload[5] > MEMORY_API_MAX_DESCRIPTORS ||
        length != MEMORY_API_HEADER_SIZE +
                      (uint16_t)payload[5] * MEMORY_API_DESCRIPTOR_SIZE) {
        error = MEMORY_API_BAD_HEADER;
        goto finished;
    }
    count = payload[5];
    for (i = 0U; i < count; ++i) {
        const uint8_t *p = payload + MEMORY_API_HEADER_SIZE +
                           (uint16_t)i * MEMORY_API_DESCRIPTOR_SIZE;
        memory_descriptor_t *d = &descriptors[i];

        d->operation = p[0];
        d->flags = p[1];
        d->length = get16(p + 10);
        d->fill = p[12];
        d->source = 0U;
        if ((d->operation != MEMORY_API_COPY && d->operation != MEMORY_API_FILL) ||
            (d->flags & (uint8_t)~MEMORY_API_PRIVATE) != 0U ||
            p[13] != 0U || p[14] != 0U || p[15] != 0U ||
            (d->operation == MEMORY_API_COPY && d->fill != 0U) ||
            (d->operation == MEMORY_API_FILL &&
             (p[2] != 0U || p[3] != 0U || p[4] != 0U || p[5] != 0U))) {
            error = MEMORY_API_BAD_DESCRIPTOR;
            goto finished;
        }
        error = endpoint(p + 6, d->length, &d->destination);
        if (error != MEMORY_API_OK) goto finished;
        if (d->operation == MEMORY_API_COPY) {
            error = endpoint(p + 2, d->length, &d->source);
            if (error != MEMORY_API_OK) goto finished;
            if ((d->source >> 16) == (d->destination >> 16) &&
                d->source < d->destination + d->length &&
                d->destination < d->source + d->length) {
                error = MEMORY_API_OVERLAP;
                goto finished;
            }
        }
        if (d->source >= 0x20000UL || d->destination >= 0x20000UL)
            needs_ramworks = 1U;
    }
    /* STATUS availability is advisory. begin() must distinguish an inactive
     * session (safe UNAVAILABLE) from an undrained previous failure (UNSAFE),
     * even when both currently report unavailable. */
    error = backend->begin(backend->ctx, needs_ramworks);
    if (error != MEMORY_API_OK) goto finished;
    held = 1U;
    /* Check the complete list's privacy policy under the hold, before the
     * first destination write. */
    for (i = 0U; i < count; ++i) {
        if ((descriptors[i].flags & MEMORY_API_PRIVATE) == 0U &&
            backend->private_required(backend->ctx, descriptors[i].destination,
                                       descriptors[i].length) != 0U) {
            error = MEMORY_API_PRIVATE_REQUIRED;
            goto finished;
        }
    }
    for (i = 0U; i < count; ++i) {
        const memory_descriptor_t *d = &descriptors[i];
        uint16_t offset = 0U;
        while (offset < d->length) {
            uint16_t size = (uint16_t)(d->length - offset);
            if (size > MEMORY_API_COPY_CHUNK) size = MEMORY_API_COPY_CHUNK;
            if (d->operation == MEMORY_API_COPY) {
                error = backend->read(backend->ctx, d->source + offset, data, size);
                if (error != MEMORY_API_OK) goto finished;
            } else {
                memset(data, d->fill, size);
            }
            error = backend->write(backend->ctx, d->destination + offset, data, size);
            if (error != MEMORY_API_OK) goto finished;
            last_completed_bytes += size;
            offset = (uint16_t)(offset + size);
        }
        last_completed_descriptors++;
    }

finished:
    if (held != 0U) {
        uint8_t end_error = backend->end(backend->ctx);
        if (end_error == MEMORY_API_UNSAFE || error == MEMORY_API_OK)
            error = end_error;
    }
    last_error = error;
    last_elapsed_us = backend->micros(backend->ctx) - started;
    executing = 0U;
    return error;
}
