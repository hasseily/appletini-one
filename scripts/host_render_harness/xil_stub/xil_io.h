/* Host-harness stub for Xil_In32/Xil_Out32. Register addresses reach
 * this stub already redirected into fake register arrays (see
 * harness_prefix.h), so plain pointer dereference is safe. */
#ifndef XIL_IO_H
#define XIL_IO_H

#include <stdint.h>

static __inline uint32_t Xil_In32(uintptr_t addr)
{ return *(uint32_t *)addr; }
static __inline void Xil_Out32(uintptr_t addr, uint32_t v)
{ *(uint32_t *)addr = v; }
static __inline uint8_t Xil_In8(uintptr_t addr)
{ return *(uint8_t *)addr; }
static __inline void Xil_Out8(uintptr_t addr, uint8_t v)
{ *(uint8_t *)addr = v; }

#endif
