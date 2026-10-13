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

static uint8_t core1_launch_attempted;
static uint8_t core1_hold_attempted;

static inline void amp_memory_barrier(void)
{
#if defined(AMP_HOST_TEST)
    extern void amp_test_memory_barrier(void);
    amp_test_memory_barrier();
#else
    __asm__ volatile ("dsb sy" ::: "memory");
#endif
}

static inline void amp_wake_core1(void)
{
    amp_memory_barrier();
#if defined(AMP_HOST_TEST)
    extern void amp_test_wake_core1(void);
    amp_test_wake_core1();
#else
    __asm__ volatile ("sev");
#endif
}

int amp_hold_core1(void)
{
    uint32_t ctrl;
    int rc = -1;

    /* Even a failed reset attempt makes the BootROM WFE handoff unsafe.
     * A reset CPU starts at its reset vector, not at 0xFFFFFFF0. */
    core1_hold_attempted = 1U;
    ctrl = Xil_In32(AMP_CPU_RESET_CTRL);

    /* UG585 requires reset before clock stop. Preserve CPU0 and APU bits. */
    Xil_Out32(AMP_SLCR_UNLOCK, 0x0000DF0DU);
    Xil_Out32(AMP_CPU_RESET_CTRL, ctrl | AMP_CPU1_RESET);
    amp_memory_barrier();
    if (Xil_In32(AMP_CPU_RESET_CTRL) != (ctrl | AMP_CPU1_RESET)) {
        goto done;
    }
    Xil_Out32(AMP_CPU_RESET_CTRL,
              ctrl | AMP_CPU1_RESET | AMP_CPU1_CLOCK_STOP);
    amp_memory_barrier();
    if (Xil_In32(AMP_CPU_RESET_CTRL) ==
        (ctrl | AMP_CPU1_RESET | AMP_CPU1_CLOCK_STOP)) {
        rc = 0;
    }
done:
    Xil_Out32(AMP_SLCR_LOCK, 0x0000767BU);
    amp_memory_barrier();
    return rc;
}

uint32_t amp_release_core1(void)
{
    uint32_t size = (uint32_t)(core1_blob_end - core1_blob_start);

    /* Cold start only: BootROM has already parked CPU1 in its WFE loop.
     * Resetting it here would execute the low reset vector (the FSBL),
     * which reinitializes clocks and DDR while CPU0 is running. Never copy
     * over an already launched worker or resume after any hold attempt. */
    if (core1_launch_attempted != 0U || core1_hold_attempted != 0U) {
        return 0U;
    }
    core1_launch_attempted = 1U;
    if (size == 0U ||
        (Xil_In32(AMP_CPU_RESET_CTRL) &
         (AMP_CPU1_RESET | AMP_CPU1_CLOCK_STOP)) != 0U) {
        return 0U;
    }

    void *dst = (void *)(uintptr_t)AMP_CORE1_ENTRY_DEFAULT;
    memcpy(dst, core1_blob_start, size);
    Xil_DCacheFlushRange((INTPTR)dst, size);

    Xil_Out32(AMP_CPU1_START_ADDR, AMP_CORE1_ENTRY_DEFAULT);
    Xil_DCacheFlush();
    amp_wake_core1();

    return size;
}
