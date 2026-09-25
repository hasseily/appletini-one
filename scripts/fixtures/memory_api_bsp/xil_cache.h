/* Native memory API syntax-test stub; not a firmware BSP. */
#ifndef XIL_CACHE_H
#define XIL_CACHE_H
#include <stdint.h>
typedef uintptr_t UINTPTR;
void Xil_DCacheFlushRange(UINTPTR, unsigned);
void Xil_DCacheInvalidateRange(UINTPTR, unsigned);
#endif
