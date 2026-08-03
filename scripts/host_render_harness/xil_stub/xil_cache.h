/* Host-harness stub for the Xilinx BSP cache API. The harness runs the
 * CPU1 render stack single-threaded on x86; cache maintenance is moot. */
#ifndef XIL_CACHE_H
#define XIL_CACHE_H

#include <stdint.h>

typedef intptr_t INTPTR;

static __inline void Xil_DCacheInvalidateRange(INTPTR a, unsigned l)
{ (void)a; (void)l; }
static __inline void Xil_DCacheFlushRange(INTPTR a, unsigned l)
{ (void)a; (void)l; }
static __inline void Xil_DCacheFlush(void) {}
static __inline void Xil_DCacheInvalidate(void) {}

#endif
