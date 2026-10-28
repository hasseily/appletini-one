#ifndef HTTP_HOST_XILTIMER_H
#define HTTP_HOST_XILTIMER_H
#include <stdint.h>
typedef uint64_t XTime;
#define COUNTS_PER_SECOND 1000000U
extern uint64_t host_time;
static inline void XTime_GetTime(XTime *out) { *out = host_time; }
#endif
