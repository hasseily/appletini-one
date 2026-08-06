#ifndef APPLETINI_PSDMA_H
#define APPLETINI_PSDMA_H

#include <stdint.h>

/* The PS-DMA command port has one hardware command slot. All users run on
 * CPU0 foreground code; IRQ handlers must never call this API. The owner
 * value makes an accidental nested use fail instead of replacing a live
 * transfer. */
typedef enum {
    PSDMA_OWNER_NONE = 0,
    PSDMA_OWNER_SMARTPORT,
    PSDMA_OWNER_PSRAM_CAL,
    PSDMA_OWNER_UART
} psdma_owner_t;

typedef enum {
    PSDMA_MC_TO_DDR = 0,
    PSDMA_DDR_TO_MC = 1
} psdma_direction_t;

typedef enum {
    PSDMA_OK = 0,
    PSDMA_ERR_ARG = -1,
    PSDMA_ERR_OWNED = -2,
    PSDMA_ERR_BUSY = -3,
    PSDMA_ERR_TIMEOUT = -4,
    PSDMA_ERR_ABORT = -5
} psdma_result_t;

/* Run one aligned transfer and release ownership before returning.
 * timeout_us bounds the transfer. abort_timeout_us bounds the drain after
 * a timeout. PSDMA_ERR_TIMEOUT means the abort drained safely;
 * PSDMA_ERR_ABORT means the engine did not prove that all accepted work
 * stopped, so the caller must fail closed. */
psdma_result_t psdma_transfer(psdma_owner_t owner,
                              uint32_t mc_addr,
                              uint32_t ddr_addr,
                              uint32_t length,
                              psdma_direction_t direction,
                              uint32_t timeout_us,
                              uint32_t abort_timeout_us);

psdma_owner_t psdma_current_owner(void);

#endif
