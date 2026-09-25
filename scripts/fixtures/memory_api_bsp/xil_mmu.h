/* Native memory API syntax-test stub; not a firmware BSP. */
#ifndef XIL_MMU_H
#define XIL_MMU_H
#include <stdint.h>
#define NORM_NONCACHE 0
void Xil_SetTlbAttributes(uintptr_t, unsigned);
#endif
