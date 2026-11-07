#ifndef AXISIMPLE_MMIO_H
#define AXISIMPLE_MMIO_H

#include <stdint.h>

#include "xil_mmu.h"

/* AxiSimple has no exclusive monitor. Its nine 64 KiB clients occupy
 * 0x40000000-0x4008FFFF within this 1 MiB MMU section. */
static inline void axisimple_mmio_mmu_init(void)
{
    Xil_SetTlbAttributes(0x40000000U, DEVICE_MEMORY);
}

#endif
