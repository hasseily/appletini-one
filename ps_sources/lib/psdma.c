#include "psdma.h"

#include "common.h"
#include "framebuffer.h"
#include "xiltimer.h"

#define PSDMA_BASE             PSRAM_CONTROL_BASE
#define PSDMA_MC_ADDR_REG      (PSDMA_BASE + 0x00U)
#define PSDMA_DDR_ADDR_REG     (PSDMA_BASE + 0x04U)
#define PSDMA_LEN_RW_REG       (PSDMA_BASE + 0x08U)
#define PSDMA_STATUS_REG       (PSDMA_BASE + 0x0CU)
#define PSDMA_CONTROL_REG      (PSDMA_BASE + 0x10U)

#define PSDMA_TO_MC_BIT        (1UL << 31)
#define PSDMA_DONE_BIT         (1UL << 0)
#define PSDMA_BUSY_BIT         (1UL << 1)
#define PSDMA_ABORTED_BIT      (1UL << 2)
#define PSDMA_ABORT_BIT        (1UL << 0)

static psdma_owner_t g_psdma_owner = PSDMA_OWNER_NONE;

static uint64_t psdma_timeout_ticks(uint32_t timeout_us)
{
    uint64_t ticks = ((uint64_t)timeout_us * (uint64_t)COUNTS_PER_SECOND) /
                     1000000ULL;

    return (ticks != 0U) ? ticks : 1U;
}

static uint8_t psdma_timed_out(XTime started, uint64_t limit)
{
    XTime now;

    XTime_GetTime(&now);
    return ((uint64_t)(now - started) >= limit) ? 1U : 0U;
}

static psdma_result_t psdma_claim(psdma_owner_t owner)
{
    if (owner == PSDMA_OWNER_NONE) {
        return PSDMA_ERR_ARG;
    }
    if (g_psdma_owner != PSDMA_OWNER_NONE) {
        return PSDMA_ERR_OWNED;
    }
    g_psdma_owner = owner;
    return PSDMA_OK;
}

static void psdma_release(psdma_owner_t owner)
{
    if (g_psdma_owner == owner) {
        g_psdma_owner = PSDMA_OWNER_NONE;
    }
}

/* Stop admission, then wait until the engine has drained any AXI beat or
 * PSRAM request it already accepted. A clear BUSY bit is the only safe point
 * at which another owner may use the command port. */
static psdma_result_t psdma_abort_owned(uint32_t timeout_us)
{
    XTime started;
    const uint64_t limit = psdma_timeout_ticks(timeout_us);

    REG_WRITE(PSDMA_CONTROL_REG, PSDMA_ABORT_BIT);
    XTime_GetTime(&started);
    for (;;) {
        const uint32_t status = REG_READ(PSDMA_STATUS_REG);

        if ((status & PSDMA_BUSY_BIT) == 0U) {
            /* ABORTED may be clear if completion won the race with the
             * control write. BUSY clear still proves that no accepted AXI
             * or PSRAM work remains, which is the safety condition. */
            return PSDMA_OK;
        }
        if (psdma_timed_out(started, limit) != 0U) {
            return PSDMA_ERR_ABORT;
        }
    }
}

psdma_result_t psdma_transfer(psdma_owner_t owner,
                              uint32_t mc_addr,
                              uint32_t ddr_addr,
                              uint32_t length,
                              psdma_direction_t direction,
                              uint32_t timeout_us,
                              uint32_t abort_timeout_us)
{
    psdma_result_t rc;
    XTime started;
    uint64_t limit;
    uint32_t status;

    if ((direction != PSDMA_MC_TO_DDR && direction != PSDMA_DDR_TO_MC) ||
        length == 0U || length > 0xFFFFU ||
        (mc_addr & 7U) != 0U || (ddr_addr & 7U) != 0U ||
        (length & 7U) != 0U ||
        mc_addr > 0x01000000U - length ||
        timeout_us == 0U || abort_timeout_us == 0U) {
        return PSDMA_ERR_ARG;
    }

    rc = psdma_claim(owner);
    if (rc != PSDMA_OK) {
        return rc;
    }

    status = REG_READ(PSDMA_STATUS_REG);
    if ((status & PSDMA_BUSY_BIT) != 0U) {
        rc = psdma_abort_owned(abort_timeout_us);
        psdma_release(owner);
        return (rc == PSDMA_OK) ? PSDMA_ERR_BUSY : PSDMA_ERR_ABORT;
    }

    REG_WRITE(PSDMA_MC_ADDR_REG, mc_addr);
    REG_WRITE(PSDMA_DDR_ADDR_REG, ddr_addr);
    REG_WRITE(PSDMA_LEN_RW_REG,
              ((direction == PSDMA_DDR_TO_MC) ? PSDMA_TO_MC_BIT : 0U) |
              length);

    limit = psdma_timeout_ticks(timeout_us);
    XTime_GetTime(&started);
    for (;;) {
        status = REG_READ(PSDMA_STATUS_REG);
        if ((status & PSDMA_DONE_BIT) != 0U) {
            psdma_release(owner);
            return PSDMA_OK;
        }
        if ((status & PSDMA_ABORTED_BIT) != 0U &&
            (status & PSDMA_BUSY_BIT) == 0U) {
            psdma_release(owner);
            return PSDMA_ERR_ABORT;
        }
        if (psdma_timed_out(started, limit) != 0U) {
            rc = psdma_abort_owned(abort_timeout_us);
            psdma_release(owner);
            return (rc == PSDMA_OK) ? PSDMA_ERR_TIMEOUT : PSDMA_ERR_ABORT;
        }
    }
}

psdma_owner_t psdma_current_owner(void)
{
    return g_psdma_owner;
}
