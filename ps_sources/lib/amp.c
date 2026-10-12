/*
 * amp.c -- See amp.h.
 *
 * CPU0 copies the embedded CPU1 image into DDR, publishes that entry point
 * through the Zynq CPU1 boot-vector slot, and wakes CPU1 with SEV.
 */

#include <string.h>

#include "xil_cache.h"
#include "xil_io.h"

#include "amp.h"

#define AMP_CPU1_START_ADDR  0xFFFFFFF0U
#define AMP_SLCR_UNLOCK     0xF8000008U
#define AMP_SLCR_LOCK       0xF8000004U
#define AMP_CPU_RESET_CTRL  0xF8000244U
#define AMP_CPU1_RESET      (1U << 1)
#define AMP_CPU1_CLOCK_STOP (1U << 5)

extern const unsigned char core1_blob_start[];
extern const unsigned char core1_blob_end[];
extern const uint32_t      core1_blob_size;

static inline void amp_wake_core1(void)
{
    __asm__ volatile ("dsb sy" ::: "memory");
    __asm__ volatile ("sev");
}

int amp_hold_core1(void)
{
    uint32_t ctrl = Xil_In32(AMP_CPU_RESET_CTRL);
    int rc = -1;

    /* UG585 requires reset before clock stop. Preserve CPU0 and APU bits. */
    Xil_Out32(AMP_SLCR_UNLOCK, 0x0000DF0DU);
    Xil_Out32(AMP_CPU_RESET_CTRL, ctrl | AMP_CPU1_RESET);
    __asm__ volatile ("dsb sy" ::: "memory");
    if (Xil_In32(AMP_CPU_RESET_CTRL) != (ctrl | AMP_CPU1_RESET)) {
        goto done;
    }
    Xil_Out32(AMP_CPU_RESET_CTRL,
              ctrl | AMP_CPU1_RESET | AMP_CPU1_CLOCK_STOP);
    __asm__ volatile ("dsb sy" ::: "memory");
    if (Xil_In32(AMP_CPU_RESET_CTRL) ==
        (ctrl | AMP_CPU1_RESET | AMP_CPU1_CLOCK_STOP)) {
        rc = 0;
    }
done:
    Xil_Out32(AMP_SLCR_LOCK, 0x0000767BU);
    __asm__ volatile ("dsb sy" ::: "memory");
    return rc;
}

static int amp_resume_held_core1(void)
{
    uint32_t ctrl = Xil_In32(AMP_CPU_RESET_CTRL);
    int rc = -1;

    if ((ctrl & (AMP_CPU1_RESET | AMP_CPU1_CLOCK_STOP)) == 0U) {
        return 0;
    }
    /* A partial failed hold must become a verified hold before release. */
    if ((ctrl & (AMP_CPU1_RESET | AMP_CPU1_CLOCK_STOP)) !=
        (AMP_CPU1_RESET | AMP_CPU1_CLOCK_STOP)) {
        if (amp_hold_core1() != 0) {
            return -1;
        }
        ctrl = Xil_In32(AMP_CPU_RESET_CTRL);
    }
    Xil_Out32(AMP_SLCR_UNLOCK, 0x0000DF0DU);
    /* Publish the entry vector first, then release reset before the clock. */
    Xil_Out32(AMP_CPU_RESET_CTRL, ctrl & ~AMP_CPU1_RESET);
    __asm__ volatile ("dsb sy" ::: "memory");
    if (Xil_In32(AMP_CPU_RESET_CTRL) != (ctrl & ~AMP_CPU1_RESET)) {
        goto done;
    }
    Xil_Out32(AMP_CPU_RESET_CTRL,
              ctrl & ~(AMP_CPU1_RESET | AMP_CPU1_CLOCK_STOP));
    __asm__ volatile ("dsb sy" ::: "memory");
    if (Xil_In32(AMP_CPU_RESET_CTRL) ==
        (ctrl & ~(AMP_CPU1_RESET | AMP_CPU1_CLOCK_STOP))) {
        rc = 0;
    }
done:
    Xil_Out32(AMP_SLCR_LOCK, 0x0000767BU);
    __asm__ volatile ("dsb sy" ::: "memory");
    return rc;
}

uint32_t amp_release_core1(void)
{
    uint32_t size = (uint32_t)(core1_blob_end - core1_blob_start);

    /* Never rewrite CPU1's DDR while it may still execute a previous image. */
    if (size == 0U || amp_hold_core1() != 0) {
        return 0U;
    }

    void *dst = (void *)(uintptr_t)AMP_CORE1_ENTRY_DEFAULT;
    memcpy(dst, core1_blob_start, size);
    Xil_DCacheFlushRange((INTPTR)dst, size);

    Xil_Out32(AMP_CPU1_START_ADDR, AMP_CORE1_ENTRY_DEFAULT);
    Xil_DCacheFlush();
    if (amp_resume_held_core1() != 0) {
        return 0U;
    }
    amp_wake_core1();

    return size;
}
