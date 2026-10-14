/* Actual reset/submit/transport guards come from the generated include.
 * Only their MMIO inputs and renderer backend use host substitutes. */
#include "glyph_api.h"
#include "video_rom_service.h"
#include "card_control_regs.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { ++checks; if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __func__, __LINE__, #condition); \
    exit(1); } } while (0)

static uint32_t mock_read(uint32_t address);
#define REG_READ(address) mock_read(address)

static uint32_t reset_status;
static uint32_t fifo_status;
static uint32_t generation;
static unsigned applies, reads, checks;
static uint8_t active[4096];
static uint8_t image[4096];
static uint8_t payload[16 + 512];
static uint8_t custom;

#include "glyph_reset_production.h"

static uint32_t mock_read(uint32_t address)
{
    ++reads;
    if (address == CARD_CTRL_APPLE_RESET_STATUS_REG) return reset_status;
    if (address == MEM_SP_STATUS) return fifo_status;
    CHECK(0); /* No vTW state, DMA, or hold registers belong to glyph submit. */
    return 0;
}

uint8_t video_rom_service_apply(const uint8_t *rom)
{
    ++applies;
    custom = rom != NULL;
    if (rom != NULL) memcpy(active, rom, sizeof(active));
    else memset(active, 0, sizeof(active));
    ++generation;
    return GLYPH_API_OK;
}

void video_rom_service_status(uint16_t *flags, uint32_t *out_generation)
{
    *flags = GLYPH_API_AVAILABLE | (custom ? GLYPH_API_CUSTOM_ACTIVE : 0U);
    *out_generation = generation;
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

static uint16_t u16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t u32(const uint8_t *p)
{
    return (uint32_t)u16(p) | ((uint32_t)u16(p + 2) << 16);
}

static uint8_t command(uint8_t operation, uint32_t id,
                       uint16_t offset, uint16_t count)
{
    uint16_t length = 16;
    memset(payload, 0, sizeof(payload));
    memcpy(payload, "AGLY", 4);
    payload[4] = 1;
    payload[5] = operation;
    put32(payload + 8, id);
    put16(payload + 12, offset);
    put16(payload + 14, count);
    if (operation == GLYPH_API_WRITE) {
        memcpy(payload + 16, image + offset, count);
        length += count;
    } else if (operation == GLYPH_API_COMMIT) {
        put32(payload + 16, glyph_api_crc32(image, sizeof(image)));
        length += 4;
    }
    return glyph_api_execute(payload, length, &sp_glyph_backend);
}

static void status(uint32_t transaction, uint16_t coverage, uint8_t error)
{
    uint8_t out[32];
    glyph_api_status(out, &sp_glyph_backend);
    CHECK(u32(out + 16) == transaction);
    CHECK(u16(out + 20) == coverage);
    CHECK(out[14] == error);
    CHECK(u32(out + 24) == generation);
    CHECK((u16(out + 8) & GLYPH_API_CUSTOM_ACTIVE) ==
          (custom ? GLYPH_API_CUSTOM_ACTIVE : 0U));
}

static void upload(uint32_t id)
{
    CHECK(command(GLYPH_API_BEGIN, id, 0, 0) == GLYPH_API_OK);
    for (uint16_t offset = 0; offset < 4096; offset += 512)
        CHECK(command(GLYPH_API_WRITE, id, offset, 512) == GLYPH_API_OK);
}

int main(void)
{
    for (unsigned i = 0; i < sizeof(image); ++i) image[i] = (uint8_t)(i * 17U + 11U);
    reset_status = CARD_CTRL_APPLE_RESET_RES_BIT | 42U;
    fifo_status = MEM_SP_EXEC_PENDING; /* Native CPU: no EXEC_VTW bit. */
    sp_glyph_check_reset();
    CHECK(g_glyph_reset_sequence_valid && g_glyph_reset_sequence == 42U);
    status(0, 0, 0);

    CHECK(command(GLYPH_API_BEGIN, 91, 0, 0) == GLYPH_API_OK);
    CHECK(command(GLYPH_API_WRITE, 91, 0, 512) == GLYPH_API_OK);
    sp_glyph_check_reset();
    status(91, 512, 0);
    CHECK(command(GLYPH_API_COMMIT, 91, 0, 0) == GLYPH_API_INCOMPLETE);

    /* Background poll catches reset before the UI hook runs. It must leave
     * the new request's EXEC_PENDING/FIFO state in place. */
    reset_status = CARD_CTRL_APPLE_RESET_RES_BIT | 43U;
    sp_glyph_check_reset();
    CHECK(fifo_status == MEM_SP_EXEC_PENDING);
    status(0, 0, 0);
    CHECK(command(GLYPH_API_COMMIT, 91, 0, 0) == GLYPH_API_TRANSACTION);
    CHECK(applies == 0);
    CHECK(command(GLYPH_API_BEGIN, 91, 0, 0) == GLYPH_API_OK);
    CHECK(command(GLYPH_API_WRITE, 91, 1024, 512) == GLYPH_API_OK);
    /* The delayed UI reset hook sees that same sequence; preserve new data. */
    sp_glyph_check_reset();
    status(91, 512, 0);
    for (uint16_t offset = 0; offset < 4096; offset += 512)
        CHECK(command(GLYPH_API_WRITE, 91, offset, 512) == GLYPH_API_OK);

    memory_api_hw_prepare(0);
    CHECK(memory_api_hw_response_valid());
    state.poisoned = 1; /* A vTW DMA fault must not block this independent API. */
    CHECK(memory_api_hw_response_valid());
    CHECK(command(GLYPH_API_COMMIT, 91, 0, 0) == GLYPH_API_OK);
    CHECK(applies == 1 && custom && memcmp(active, image, sizeof(image)) == 0);

    upload(92);
    memory_api_hw_prepare(0);
    reset_status = CARD_CTRL_APPLE_RESET_RES_BIT | 44U;
    /* A reset after request capture must block COMMIT before renderer submit. */
    CHECK(!memory_api_hw_response_valid());
    CHECK(command(GLYPH_API_COMMIT, 92, 0, 0) == GLYPH_API_UNAVAILABLE);
    CHECK(applies == 1 && memcmp(active, image, sizeof(image)) == 0);
    status(92, 4096, GLYPH_API_UNAVAILABLE);
    sp_glyph_check_reset();
    status(0, 0, 0);
    CHECK(applies == 1 && custom && memcmp(active, image, sizeof(image)) == 0);

    /* RES# held low and discarded transport also reject restore/commit. */
    memory_api_hw_prepare(0);
    reset_status &= ~CARD_CTRL_APPLE_RESET_RES_BIT;
    CHECK(command(GLYPH_API_RESTORE, 0, 0, 0) == GLYPH_API_UNAVAILABLE);
    reset_status |= CARD_CTRL_APPLE_RESET_RES_BIT;
    fifo_status = 0;
    CHECK(command(GLYPH_API_RESTORE, 0, 0, 0) == GLYPH_API_UNAVAILABLE);
    CHECK(applies == 1 && custom);
    fifo_status = MEM_SP_EXEC_PENDING;
    CHECK(command(GLYPH_API_RESTORE, 0, 0, 0) == GLYPH_API_OK);
    CHECK(applies == 2 && !custom);

    reset_status = CARD_CTRL_APPLE_RESET_RES_BIT | 255U;
    sp_glyph_check_reset();
    CHECK(command(GLYPH_API_BEGIN, 93, 0, 0) == GLYPH_API_OK);
    CHECK(command(GLYPH_API_WRITE, 93, 4095, 1) == GLYPH_API_OK);
    reset_status = CARD_CTRL_APPLE_RESET_RES_BIT; /* eight-bit sequence wraps */
    sp_glyph_check_reset();
    status(0, 0, 0);
    CHECK(g_glyph_reset_sequence == 0);
    CHECK(applies == 2);
    CHECK(reads > 0);
    printf("glyph reset/submit: %u checks, real reset guard + native session guard PASS\n", checks);
    return 0;
}
