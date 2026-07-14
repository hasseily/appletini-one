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

extern const unsigned char core1_blob_start[];
extern const unsigned char core1_blob_end[];
extern const uint32_t      core1_blob_size;

static inline void amp_wake_core1(void)
{
    __asm__ volatile ("dsb sy" ::: "memory");
    __asm__ volatile ("sev");
}

uint32_t amp_release_core1(void)
{
    uint32_t size = (uint32_t)(core1_blob_end - core1_blob_start);

    void *dst = (void *)(uintptr_t)AMP_CORE1_ENTRY_DEFAULT;
    memcpy(dst, core1_blob_start, size);
    Xil_DCacheFlushRange((INTPTR)dst, size);

    Xil_Out32(AMP_CPU1_START_ADDR, AMP_CORE1_ENTRY_DEFAULT);
    Xil_DCacheFlush();
    amp_wake_core1();

    return size;
}
