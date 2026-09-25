/* Native memory API syntax-test stub; not a firmware BSP. */
#ifndef XSCUGIC_H
#define XSCUGIC_H
#include "xil_exception.h"
#define XST_SUCCESS 0
typedef struct {int unused;} XScuGic;
int XScuGic_Connect(XScuGic *, unsigned, Xil_InterruptHandler, void *);
void XScuGic_SetPriorityTriggerType(XScuGic *, unsigned, unsigned, unsigned);
void XScuGic_Enable(XScuGic *, unsigned);
#endif
