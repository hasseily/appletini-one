/* Host-harness stub for the Xilinx BSP MMU API. */
#ifndef XIL_MMU_H
#define XIL_MMU_H

#include <stdint.h>

#ifndef NORM_NONCACHE
#define NORM_NONCACHE 0x11DE2u
#endif

static __inline void Xil_SetTlbAttributes(uintptr_t addr, uint32_t attrib)
{ (void)addr; (void)attrib; }

#endif
