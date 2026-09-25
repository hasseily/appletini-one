/* Native memory API syntax-test stub; not a firmware BSP. */
#ifndef XILTIMER_H
#define XILTIMER_H
#include <stdint.h>
typedef uint64_t XTime;
/* Match the Zynq BSP's unparenthesized timer-frequency macro chain. */
#define XPAR_CPU_CORE_CLOCK_FREQ_HZ 666666687U
#define XSLEEPTIMER_FREQ XPAR_CPU_CORE_CLOCK_FREQ_HZ/2
#define COUNTS_PER_SECOND XSLEEPTIMER_FREQ
void XTime_GetTime(XTime *);
#endif
