/* Native memory API syntax-test stub; not a firmware BSP. */
#ifndef XILTIMER_H
#define XILTIMER_H
#include <stdint.h>
typedef uint64_t XTime;
#define COUNTS_PER_SECOND 333333333U
void XTime_GetTime(XTime *);
#endif
