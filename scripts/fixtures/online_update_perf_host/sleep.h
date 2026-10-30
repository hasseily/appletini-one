#ifndef ONLINE_UPDATE_PERF_SLEEP_H
#define ONLINE_UPDATE_PERF_SLEEP_H
#include <stdint.h>
extern uint64_t host_time;
static inline void usleep(unsigned long microseconds){host_time+=microseconds;}
#endif
