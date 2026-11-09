#ifndef TEST_XILTIMER_H
#define TEST_XILTIMER_H

#include <stdint.h>

typedef uint64_t XTime;
#define COUNTS_PER_SECOND 666666687U/2

void XTime_GetTime(XTime *time);

#endif
