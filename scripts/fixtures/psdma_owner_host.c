/* Run the production shared-path reservation with fake hardware status. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#define COMMON_H
static uint32_t mock_read(uint32_t address);
static void mock_write(uint32_t address, uint32_t value);
#define REG_READ(a) mock_read(a)
#define REG_WRITE(a, v) mock_write(a, v)
#include "../../ps_sources/lib/psdma.c"

static uint32_t hardware_status;
static unsigned reads;
static uint32_t mock_read(uint32_t address)
{
    assert(address == PSDMA_STATUS_REG);
    ++reads;
    return hardware_status;
}
static void mock_write(uint32_t address, uint32_t value)
{
    (void)address;
    (void)value;
    assert(0); /* Reserving the port must not submit or cancel another DMA. */
}
void XTime_GetTime(XTime *out) { *out = 0; }

int main(void)
{
    assert(psdma_acquire(PSDMA_OWNER_NONE) == PSDMA_ERR_ARG);
    assert(psdma_current_owner() == PSDMA_OWNER_NONE && reads == 0);
    hardware_status = PSDMA_BUSY_BIT;
    assert(psdma_acquire(PSDMA_OWNER_MEMORY) == PSDMA_ERR_BUSY);
    assert(psdma_current_owner() == PSDMA_OWNER_NONE && reads == 1);
    hardware_status = PSDMA_DONE_BIT;
    assert(psdma_acquire(PSDMA_OWNER_MEMORY) == PSDMA_OK);
    assert(psdma_current_owner() == PSDMA_OWNER_MEMORY && reads == 2);
    assert(psdma_acquire(PSDMA_OWNER_MEMORY) == PSDMA_ERR_OWNED);
    assert(psdma_acquire(PSDMA_OWNER_SMARTPORT) == PSDMA_ERR_OWNED);
    assert(reads == 2);
    assert(psdma_transfer(PSDMA_OWNER_SMARTPORT, 0x20000, 0x100000,
                          8, PSDMA_MC_TO_DDR, 1000, 1000) == PSDMA_ERR_OWNED);
    psdma_release(PSDMA_OWNER_SMARTPORT);
    assert(psdma_current_owner() == PSDMA_OWNER_MEMORY);
    psdma_release(PSDMA_OWNER_MEMORY);
    assert(psdma_current_owner() == PSDMA_OWNER_NONE);
    assert(psdma_acquire(PSDMA_OWNER_SMARTPORT) == PSDMA_OK);
    psdma_release(PSDMA_OWNER_SMARTPORT);
    puts("PASS shared PSRAM ownership: hardware busy, nested use, owner-only release");
    return 0;
}
