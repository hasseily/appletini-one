/* SPDX-License-Identifier: MIT */
#ifndef HTTP_ENTROPY_HOST_XILTIMER_H
#define HTTP_ENTROPY_HOST_XILTIMER_H
#include <stdint.h>
typedef uint64_t XTime;
/* Keep this unparenthesized like the BSP to detect cast/division bugs. */
#define COUNTS_PER_SECOND 2000000U / 2U
void XTime_GetTime(XTime *out);
#endif
