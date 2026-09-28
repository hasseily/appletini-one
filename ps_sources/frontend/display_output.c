#include "display_output.h"

#include <string.h>
#include "xiltimer.h"
#include "../lib/common.h"
#include "../lib/framebuffer.h"
#include "compositor.h"
#include "compositor_layout.h"
#include "display_modes.h"

#define DISPLAY_SWITCH_TIMEOUT ((XTime)COUNTS_PER_SECOND * 2U)

static int display_status_present(uint32_t status)
{
    return (status & FB_MODE_SIGNATURE_MASK) == FB_MODE_SIGNATURE;
}

static int display_status_ready(uint32_t status)
{
    return display_status_present(status) &&
           (status & (FB_MODE_BUSY | FB_MODE_LOCKED | FB_MODE_HELD)) == FB_MODE_LOCKED &&
           (status & FB_MODE_ID_MASK) < DISPLAY_MODE_COUNT;
}

int display_output_apply(uint8_t mode, void (*service)(void))
{
    uint32_t status = REG_READ(FB_MODE_STATUS_REG);
    uint32_t base = 0U;
    XTime start, now;

    if (mode >= DISPLAY_MODE_COUNT) {
        return -1;
    }
    if (!display_status_present(status)) {
        /* A new frontend can still run at 1080p with an older bitstream. */
        compositor_set_output_mode(DISPLAY_MODE_DEFAULT);
        return mode == DISPLAY_MODE_DEFAULT ? 0 : -2;
    }
    if (display_status_ready(status) &&
        (status & FB_MODE_ID_MASK) == mode) {
        compositor_set_output_mode(mode);
        return 0;
    }
    if ((status & FB_MODE_BUSY) != 0U) {
        return -3;
    }

    compositor_set_paused(1U);
    /* A vblank can advance the pending base while we read. Excluding both
     * live and pending slots still leaves one slot that the PL cannot use. */
    const uint32_t live = REG_READ(FB_LAST_LATCHED_REG);
    const uint32_t pending = REG_READ(FB_BASE_ADDR_REG);
    for (uint32_t slot = 0U; slot < COMP_OUT_SLOT_COUNT; ++slot) {
        if (comp_out_slot_addr[slot] != live &&
            comp_out_slot_addr[slot] != pending) {
            base = comp_out_slot_addr[slot];
            break;
        }
    }
    if (base == 0U) {
        compositor_set_paused(0U);
        return -4;
    }
    memset((void *)(uintptr_t)base, 0, COMP_OUT_MAX_BYTES);
    __sync_synchronize();
    REG_WRITE(FB_MODE_BASE_REG, base);
    REG_WRITE(FB_MODE_REQUEST_REG, mode);
    XTime_GetTime(&start);
    do {
        status = REG_READ(FB_MODE_STATUS_REG);
        if (display_status_ready(status) &&
            (status & FB_MODE_ID_MASK) == mode) {
            compositor_set_output_mode(mode);
            compositor_set_paused(0U);
            return 0;
        }
        if ((status & FB_MODE_ERROR) != 0U &&
            (status & FB_MODE_BUSY) == 0U) {
            break;
        }
        if (service != NULL) {
            service();
        }
        XTime_GetTime(&now);
    } while ((now - start) < DISPLAY_SWITCH_TIMEOUT);

    /* The PL tries to restore its default clock after a failed switch.
     * Never resume writes with a stride that disagrees with live scanout. */
    if (display_status_ready(status)) {
        compositor_set_output_mode((uint8_t)(status & FB_MODE_ID_MASK));
        compositor_set_paused(0U);
    }
    return -5;
}
