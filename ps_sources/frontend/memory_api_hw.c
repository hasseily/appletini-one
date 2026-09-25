#include "memory_api_hw.h"

#include <string.h>
#include "xil_cache.h"
#include "xiltimer.h"
#include "../lib/common.h"
#include "../lib/psdma.h"
#include "card_control_regs.h"

#define MEM_SP_STATUS              0x40020000U
#define MEM_SP_EXEC_PENDING        (1UL << 28)
#define MEM_SP_EXEC_VTW            (1UL << 31)
#define MEM_RAMWORKS_ENABLE        CARD_CTRL_REG_ADDR(0x62U)
#define MEM_LIVE_MASK              (CARD_CTRL_VTW_STATUS_BUS_OWNED | \
                                   CARD_CTRL_VTW_STATUS_ENABLE_EFF | \
                                   CARD_CTRL_VTW_STATUS_CORE_RUN)
#define MEM_HOLD_TIMEOUT_US        100000U
#define MEM_TRANSFER_TIMEOUT_US    10000U

typedef struct {
    uint32_t reset_sequence;
    uint8_t accelerated;
    uint8_t hold_requested;
    uint8_t poisoned;
} memory_hw_state_t;

static memory_hw_state_t state;
/* One bounce buffer; endpoints may have different eight-byte alignment.
 * No new Apple-visible RAM or extra MAIN context is allocated. */
static uint8_t bounce[MEMORY_API_DMA_CHUNK] __attribute__((aligned(32)));

static uint32_t hw_micros(void *context)
{
    XTime ticks;
    /* The BSP expands COUNTS_PER_SECOND to an unparenthesized division.
     * Evaluate that complete expression before using it as a divisor. */
    const uint64_t counts_per_second = (uint64_t)(COUNTS_PER_SECOND);
    (void)context;
    XTime_GetTime(&ticks);
    return (uint32_t)(((uint64_t)ticks / counts_per_second) * 1000000ULL +
                     (((uint64_t)ticks % counts_per_second) * 1000000ULL) /
                         counts_per_second);
}

static uint8_t hw_available(void *context)
{
    (void)context;
    return state.poisoned == 0U &&
        (REG_READ(CARD_CTRL_VTW_STATUS_REG) & MEM_LIVE_MASK) == MEM_LIVE_MASK &&
        (REG_READ(CARD_CTRL_APPLE_RESET_STATUS_REG) &
         CARD_CTRL_APPLE_RESET_RES_BIT) != 0U;
}

void memory_api_hw_prepare(uint8_t accelerated)
{
    state.accelerated = accelerated;
    state.hold_requested = 0U;
    state.reset_sequence = REG_READ(CARD_CTRL_APPLE_RESET_STATUS_REG) &
                           CARD_CTRL_APPLE_RESET_SEQ_MASK;
}

uint8_t memory_api_hw_response_valid(void)
{
    const uint32_t reset = REG_READ(CARD_CTRL_APPLE_RESET_STATUS_REG);
    return (reset & CARD_CTRL_APPLE_RESET_RES_BIT) != 0U &&
        (reset & CARD_CTRL_APPLE_RESET_SEQ_MASK) == state.reset_sequence &&
        (REG_READ(MEM_SP_STATUS) & MEM_SP_EXEC_PENDING) != 0U;
}

static uint8_t hw_session(void)
{
    const uint32_t reset = REG_READ(CARD_CTRL_APPLE_RESET_STATUS_REG);
    const uint32_t smartport = REG_READ(MEM_SP_STATUS);
    /* Read each live register once. These checks also run in DMA polls and
     * shadow-port loops, where duplicate AXI reads are significant. */
    return state.poisoned == 0U &&
        (reset & CARD_CTRL_APPLE_RESET_RES_BIT) != 0U &&
        (reset & CARD_CTRL_APPLE_RESET_SEQ_MASK) == state.reset_sequence &&
        (smartport & (MEM_SP_EXEC_PENDING | MEM_SP_EXEC_VTW)) ==
            (MEM_SP_EXEC_PENDING | MEM_SP_EXEC_VTW) &&
        (REG_READ(CARD_CTRL_VTW_STATUS_REG) & MEM_LIVE_MASK) == MEM_LIVE_MASK;
}

static uint8_t hw_held(void *context)
{
    const uint32_t hold = REG_READ(CARD_CTRL_VTW_RW_FLUSH_REG);
    (void)context;
    return hw_session() &&
        (hold & (CARD_CTRL_VTW_RW_FLUSH_BUSY_BIT |
                 CARD_CTRL_VTW_RW_FLUSH_HELD_BIT)) ==
            CARD_CTRL_VTW_RW_FLUSH_HELD_BIT;
}

static uint8_t hw_release(void)
{
    uint32_t started = hw_micros(NULL);
    REG_WRITE(CARD_CTRL_VTW_RW_FLUSH_REG, CARD_CTRL_VTW_RW_FLUSH_RELEASE_BIT);
    do {
        if ((REG_READ(CARD_CTRL_VTW_RW_FLUSH_REG) &
             (CARD_CTRL_VTW_RW_FLUSH_BUSY_BIT | CARD_CTRL_VTW_RW_FLUSH_HELD_BIT)) == 0U) {
            state.hold_requested = 0U;
            return MEMORY_API_OK;
        }
    } while ((uint32_t)(hw_micros(NULL) - started) < MEM_TRANSFER_TIMEOUT_US);
    state.poisoned = 1U;
    return MEMORY_API_UNSAFE;
}

static uint8_t hw_begin(void *context, uint8_t needs_ramworks)
{
    uint32_t before;
    uint32_t started;
    uint8_t error;
    (void)context;
    if (state.poisoned != 0U) return MEMORY_API_UNSAFE;
    if (!state.accelerated || !hw_available(NULL) ||
        (needs_ramworks && (REG_READ(MEM_RAMWORKS_ENABLE) & 1U) == 0U))
        return MEMORY_API_UNAVAILABLE;
    if (!hw_session()) return MEMORY_API_SESSION_LOST;
    before = REG_READ(CARD_CTRL_VTW_RW_FLUSH_REG);
    if (psdma_current_owner() != PSDMA_OWNER_NONE ||
        (before & (CARD_CTRL_VTW_RW_FLUSH_BUSY_BIT |
                   CARD_CTRL_VTW_RW_FLUSH_HELD_BIT)) != 0U)
        return MEMORY_API_BUSY;
    before &= CARD_CTRL_VTW_RW_FLUSH_COUNT_MASK;
    state.hold_requested = 1U;
    REG_WRITE(CARD_CTRL_VTW_RW_FLUSH_REG, CARD_CTRL_VTW_RW_FLUSH_REQ_BIT);
    started = hw_micros(NULL);
    do {
        const uint32_t hold = REG_READ(CARD_CTRL_VTW_RW_FLUSH_REG);
        if (!hw_session()) {
            error = MEMORY_API_SESSION_LOST;
            goto failed;
        }
        if ((hold & CARD_CTRL_VTW_RW_FLUSH_BUSY_BIT) == 0U &&
            (((hold & CARD_CTRL_VTW_RW_FLUSH_COUNT_MASK) - before) &
             CARD_CTRL_VTW_RW_FLUSH_COUNT_MASK) == 1U) {
            if ((hold & CARD_CTRL_VTW_RW_FLUSH_HELD_BIT) != 0U)
                return MEMORY_API_OK;
            error = MEMORY_API_SESSION_LOST;
            goto failed;
        }
    } while ((uint32_t)(hw_micros(NULL) - started) < MEM_HOLD_TIMEOUT_US);
    error = MEMORY_API_IO;
failed:
    return hw_release() == MEMORY_API_OK ? error : MEMORY_API_UNSAFE;
}

static uint8_t hw_dma(uint32_t physical, uint16_t length,
                       psdma_direction_t direction)
{
    psdma_result_t result;
    if (length == 0U || length > MEMORY_API_DMA_CHUNK ||
        ((physical | length) & 7U) != 0U || physical >= 0x800000UL ||
        length > 0x800000UL - physical)
        return MEMORY_API_RANGE;
    if (!hw_held(NULL)) return MEMORY_API_SESSION_LOST;
    /* Both directions first clean this cached bounce buffer. On a DMA read,
     * invalidate afterwards so later ARM stores cannot overwrite new data. */
    Xil_DCacheFlushRange((UINTPTR)bounce, sizeof(bounce));
    result = psdma_transfer_checked(PSDMA_OWNER_MEMORY, physical,
                                    (uint32_t)(uintptr_t)bounce, length,
                                    direction, MEM_TRANSFER_TIMEOUT_US,
                                    MEM_TRANSFER_TIMEOUT_US, hw_held, NULL);
    if (direction == PSDMA_MC_TO_DDR)
        Xil_DCacheInvalidateRange((UINTPTR)bounce, sizeof(bounce));
    if (result == PSDMA_ERR_ABORT) {
        state.poisoned = 1U;
        return MEMORY_API_UNSAFE;
    }
    if (result == PSDMA_ERR_CANCELLED || !hw_held(NULL))
        return MEMORY_API_SESSION_LOST;
    return result == PSDMA_OK ? MEMORY_API_OK : MEMORY_API_IO;
}

static uint8_t hw_shadow_read(uint32_t physical, uint8_t *data, uint16_t length)
{
    uint32_t aligned = physical & ~3UL;
    uint32_t end = physical + length;
    uint32_t count;
    uint32_t started;

    if (!hw_held(NULL)) return MEMORY_API_SESSION_LOST;
    REG_WRITE(CARD_CTRL_VTW_SHADOW_ADDR_REG, aligned);
    count = REG_READ(CARD_CTRL_VTW_SHADOW_READ4_STATUS_REG) &
            CARD_CTRL_VTW_SHADOW_READ4_COUNT_MASK;
    while (aligned < end) {
        uint32_t status;
        uint32_t word;
        uint8_t lane;
        started = hw_micros(NULL);
        do {
            if (!hw_held(NULL)) return MEMORY_API_SESSION_LOST;
            status = REG_READ(CARD_CTRL_VTW_SHADOW_READ4_STATUS_REG);
            if ((status & CARD_CTRL_VTW_SHADOW_READ4_READY_BIT) != 0U) break;
            if ((uint32_t)(hw_micros(NULL) - started) >= MEM_TRANSFER_TIMEOUT_US)
                return MEMORY_API_IO;
        } while (1);
        REG_WRITE(CARD_CTRL_VTW_SHADOW_READ4_REG, 1U);
        count = (count + 1U) & CARD_CTRL_VTW_SHADOW_READ4_COUNT_MASK;
        do {
            if (!hw_held(NULL)) return MEMORY_API_SESSION_LOST;
            status = REG_READ(CARD_CTRL_VTW_SHADOW_READ4_STATUS_REG);
            if ((status & CARD_CTRL_VTW_SHADOW_READ4_BUSY_BIT) == 0U &&
                (status & CARD_CTRL_VTW_SHADOW_READ4_COUNT_MASK) == count) break;
            if ((uint32_t)(hw_micros(NULL) - started) >= MEM_TRANSFER_TIMEOUT_US)
                return MEMORY_API_IO;
        } while (1);
        word = REG_READ(CARD_CTRL_VTW_SHADOW_READ4_DATA_REG);
        for (lane = 0U; lane < 4U; ++lane) {
            uint32_t address = aligned + lane;
            if (address >= physical && address < end)
                data[address - physical] = (uint8_t)(word >> (lane * 8U));
        }
        aligned += 4U;
    }
    return MEMORY_API_OK;
}

static uint8_t hw_shadow_wait(uint32_t pointer, uint32_t accepted,
                               uint32_t started)
{
    do {
        const uint32_t status = REG_READ(CARD_CTRL_VTW_SHADOW_DATA4_STATUS_REG);
        if (!hw_held(NULL)) return MEMORY_API_SESSION_LOST;
        if ((status & CARD_CTRL_VTW_SHADOW_DATA4_BUSY_BIT) == 0U &&
            (status & CARD_CTRL_VTW_SHADOW_DATA4_ACCEPT_MASK) == accepted &&
            (REG_READ(CARD_CTRL_VTW_SHADOW_ADDR_REG) & 0x3FFFFUL) == pointer)
            return MEMORY_API_OK;
    } while ((uint32_t)(hw_micros(NULL) - started) < MEM_TRANSFER_TIMEOUT_US);
    return MEMORY_API_IO;
}

static uint8_t hw_shadow_write(uint32_t physical, const uint8_t *data,
                                uint16_t length)
{
    uint32_t offset = 0U;
    uint32_t accepted;
    uint32_t started;
    uint8_t error;
    if (!hw_held(NULL)) return MEMORY_API_SESSION_LOST;
    accepted = REG_READ(CARD_CTRL_VTW_SHADOW_DATA4_STATUS_REG) &
               CARD_CTRL_VTW_SHADOW_DATA4_ACCEPT_MASK;
    REG_WRITE(CARD_CTRL_VTW_SHADOW_ADDR_REG, physical);
    /* Finish any unaligned prefix before admitting packed words. */
    while (offset < length && ((physical + offset) & 3U) != 0U) {
        if (!hw_held(NULL)) return MEMORY_API_SESSION_LOST;
        started = hw_micros(NULL);
        REG_WRITE(CARD_CTRL_VTW_SHADOW_DATA_REG, data[offset++]);
        error = hw_shadow_wait(physical + offset, accepted, started);
        if (error != MEMORY_API_OK) return error;
    }
    started = hw_micros(NULL);
    while (length - offset >= 4U) {
        uint32_t word;
        /* Preserve FIFO batching: prove final completion once per chunk,
         * while checking ownership before every new packed submission. */
        do {
            if (!hw_held(NULL)) return MEMORY_API_SESSION_LOST;
            if ((REG_READ(CARD_CTRL_VTW_SHADOW_DATA4_STATUS_REG) &
                 CARD_CTRL_VTW_SHADOW_DATA4_READY_BIT) != 0U) break;
            if ((uint32_t)(hw_micros(NULL) - started) >= MEM_TRANSFER_TIMEOUT_US)
                return MEMORY_API_IO;
        } while (1);
        word = (uint32_t)data[offset] |
               ((uint32_t)data[offset + 1U] << 8) |
               ((uint32_t)data[offset + 2U] << 16) |
               ((uint32_t)data[offset + 3U] << 24);
        REG_WRITE(CARD_CTRL_VTW_SHADOW_DATA4_REG, word);
        accepted = (accepted + 1U) & CARD_CTRL_VTW_SHADOW_DATA4_ACCEPT_MASK;
        offset += 4U;
    }
    error = hw_shadow_wait(physical + offset, accepted, started);
    if (error != MEMORY_API_OK) return error;
    while (offset < length) {
        if (!hw_held(NULL)) return MEMORY_API_SESSION_LOST;
        started = hw_micros(NULL);
        REG_WRITE(CARD_CTRL_VTW_SHADOW_DATA_REG, data[offset++]);
        error = hw_shadow_wait(physical + offset, accepted, started);
        if (error != MEMORY_API_OK) return error;
    }
    return MEMORY_API_OK;
}

static uint8_t hw_read(void *context, uint32_t physical, uint8_t *data,
                        uint16_t length)
{
    uint8_t error;
    uint16_t prefix = (uint16_t)(physical & 7U);
    uint16_t total = (uint16_t)((prefix + length + 7U) & ~7U);
    (void)context;
    if (physical < 0x20000UL)
        return hw_shadow_read(physical, data, length);
    error = hw_dma(physical & ~7UL, total, PSDMA_MC_TO_DDR);
    if (error == MEMORY_API_OK) memcpy(data, bounce + prefix, length);
    return error;
}

static uint8_t hw_write(void *context, uint32_t physical, const uint8_t *data,
                         uint16_t length)
{
    uint8_t error;
    uint16_t prefix = (uint16_t)(physical & 7U);
    uint16_t total = (uint16_t)((prefix + length + 7U) & ~7U);
    (void)context;
    if (physical < 0x20000UL)
        return hw_shadow_write(physical, data, length);
    /* Preserve unrelated bytes in a partial first/last PSRAM line. */
    if (prefix != 0U || total != length) {
        error = hw_dma(physical & ~7UL, total, PSDMA_MC_TO_DDR);
        if (error != MEMORY_API_OK) return error;
    }
    memcpy(bounce + prefix, data, length);
    return hw_dma(physical & ~7UL, total, PSDMA_DDR_TO_MC);
}

static uint8_t hw_end(void *context)
{
    uint8_t was_live = hw_held(NULL);
    uint32_t pointer;
    uint32_t started;
    (void)context;
    /* A failed shadow write may leave packed words queued. Changing its
     * pointer cancels them before release. A word already accepted may land;
     * the caller receives an error and must not retry as an atomic copy. */
    pointer = REG_READ(CARD_CTRL_VTW_SHADOW_ADDR_REG) & 0x3FFFFUL;
    REG_WRITE(CARD_CTRL_VTW_SHADOW_ADDR_REG, pointer);
    if (state.poisoned != 0U) return MEMORY_API_UNSAFE;
    /* READ4 ready proves the host port is idle, including scalar writes.
     * A pointer reset also performs a harmless read, so wait for that to
     * drain before allowing the CPU to use the shared shadow memory. */
    started = hw_micros(NULL);
    while ((REG_READ(CARD_CTRL_VTW_SHADOW_READ4_STATUS_REG) &
            CARD_CTRL_VTW_SHADOW_READ4_READY_BIT) == 0U) {
        if ((uint32_t)(hw_micros(NULL) - started) >= MEM_TRANSFER_TIMEOUT_US) {
            state.poisoned = 1U;
            return MEMORY_API_UNSAFE;
        }
    }
    if (state.hold_requested != 0U && hw_release() != MEMORY_API_OK)
        return MEMORY_API_UNSAFE;
    return was_live ? MEMORY_API_OK : MEMORY_API_SESSION_LOST;
}

static uint8_t hw_private_required(void *context, uint32_t physical,
                                    uint16_t length)
{
    (void)context;
    (void)length;
    /* v1 offers working-memory copies only. Every shadow destination needs
     * explicit PRIVATE: no renderer capture or motherboard replay is emitted. */
    return physical < 0x20000UL;
}

const memory_api_backend_t memory_api_hardware = {
    NULL, hw_available, hw_begin, hw_read, hw_write, hw_end,
    hw_private_required, hw_micros
};
