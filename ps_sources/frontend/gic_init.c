/*
 * gic_init.c -- See gic_init.h.
 */

#include "gic_init.h"

#include "xparameters.h"
#include "xil_exception.h"

static XScuGic g_gic;
static int     g_gic_ready = 0;

XScuGic *gic_get(void)
{
    return g_gic_ready ? &g_gic : (XScuGic *)0;
}

int gic_init(void)
{
    XScuGic_Config *cfg;
    int status;

    if (g_gic_ready) {
        return 0;
    }

    cfg = XScuGic_LookupConfig(XPAR_XSCUGIC_0_BASEADDR);
    if (cfg == (XScuGic_Config *)0) {
        return -1;
    }

    status = XScuGic_CfgInitialize(&g_gic, cfg, cfg->CpuBaseAddress);
    if (status != XST_SUCCESS) {
        return status;
    }

    Xil_ExceptionRegisterHandler(
        XIL_EXCEPTION_ID_IRQ_INT,
        (Xil_ExceptionHandler)XScuGic_InterruptHandler,
        &g_gic);

    Xil_ExceptionEnable();

    g_gic_ready = 1;
    return 0;
}
