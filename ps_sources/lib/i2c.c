#include "i2c.h"
#include "xstatus.h"
#include "xiltimer.h"
#include <stddef.h>

static void (*wait_checkpoint)(void);

void i2c_set_wait_checkpoint(void (*checkpoint)(void))
{
    wait_checkpoint = checkpoint;
}

static void transfer_status(void *opaque, u32 status)
{
    u32 *events = opaque;
    *events |= status;
}

static int wait_for_transfer(XIicPs *i2c, const u32 *events, u32 complete)
{
    const UINTPTR base = i2c->Config.BaseAddress;
    const XTime timeout = (COUNTS_PER_SECOND) / 50U;
    XTime start, now;

    XTime_GetTime(&start);
    do {
        if ((XIicPs_ReadReg(base, XIICPS_ISR_OFFSET) &
             ~XIicPs_ReadReg(base, XIICPS_IMR_OFFSET)) != 0U) {
            XIicPs_MasterInterruptHandler(i2c);
        }
        if ((*events & ~complete) != 0U) {
            return -1;
        }
        if ((*events & complete) != 0U && !XIicPs_BusIsBusy(i2c)) {
            return 0;
        }
        wait_checkpoint();
        XTime_GetTime(&now);
    } while (now - start < timeout);
    return (*events & complete) != 0U ? -2 : -1;
}

static int read_with_checkpoint(XIicPs *i2c, uint8_t addr7, uint8_t reg,
                                uint8_t *data, int len)
{
    if (XIicPs_BusIsBusy(i2c)) {
        return -2;
    }
    const UINTPTR base = i2c->Config.BaseAddress;
    const u32 control = XIicPs_ReadReg(base, XIICPS_CR_OFFSET);
    const u32 enabled_interrupts = XIICPS_IXR_ALL_INTR_MASK &
        ~XIicPs_ReadReg(base, XIICPS_IMR_OFFSET);
    const XIicPs_IntrHandler previous_handler = i2c->StatusHandler;
    void *const previous_ref = i2c->CallBackRef;
    u32 events = 0U;
    int rc = -2;

    // Poll the driver's interrupt state on CPU0. The GIC has no I2C handler.
    XIicPs_SetStatusHandler(i2c, &events, transfer_status);
    XIicPs_MasterSend(i2c, &reg, 1, addr7);
    if (wait_for_transfer(i2c, &events, XIICPS_EVENT_COMPLETE_SEND) == 0) {
        events = 0U;
        XIicPs_MasterRecv(i2c, data, len, addr7);
        const int result = wait_for_transfer(i2c, &events, XIICPS_EVENT_COMPLETE_RECV);
        rc = result == 0 ? 0 : (result == -2 ? -4 : -3);
    }

    XIicPs_DisableInterrupts(base, XIICPS_IXR_ALL_INTR_MASK);
    if (rc != 0) {
        XIicPs_Abort(i2c);
        // Abort clears the controller configuration, including the SCL divisor.
        XIicPs_WriteReg(base, XIICPS_CR_OFFSET, control);
    }
    XIicPs_WriteReg(base, XIICPS_ISR_OFFSET, XIICPS_IXR_ALL_INTR_MASK);
    i2c->StatusHandler = previous_handler;
    i2c->CallBackRef = previous_ref;
    XIicPs_EnableInterrupts(base, enabled_interrupts);
    return rc;
}

int i2c_init(XIicPs *i2c)
{
    XIicPs_Config *cfg = XIicPs_LookupConfig(XPAR_XIICPS_0_BASEADDR);
    if (!cfg) return -1;
    int status = XIicPs_CfgInitialize(i2c, cfg, cfg->BaseAddress);
    if (status != XST_SUCCESS) return -2;
    XIicPs_SetSClk(i2c, 100000);
    return 0;
}

int i2c_write_bytes(XIicPs *i2c, uint8_t addr7, const uint8_t *data, int len)
{
    if (!i2c || !data || len <= 0) return -1;
    int status = XIicPs_MasterSendPolled(i2c, (u8 *)data, len, addr7);
    if (status != XST_SUCCESS) return -2;
    for (int i = 0; i < 1000000; i++) {
        if (!XIicPs_BusIsBusy(i2c))
            return 0;
    }
    return -3;
}

int i2c_read_bytes(XIicPs *i2c, uint8_t addr7, uint8_t reg, uint8_t *data, int len)
{
    if (!i2c || !data || len <= 0) return -1;
    if (wait_checkpoint != NULL && (unsigned)len <= XIICPS_FIFO_DEPTH) {
        return read_with_checkpoint(i2c, addr7, reg, data, len);
    }

    int status = XIicPs_MasterSendPolled(i2c, &reg, 1, addr7);
    if (status != XST_SUCCESS) return -2;
    for (int i = 0; i < 1000000; i++) {
        if (!XIicPs_BusIsBusy(i2c))
            break;
    }

    status = XIicPs_MasterRecvPolled(i2c, data, len, addr7);
    if (status != XST_SUCCESS) return -3;
    for (int i = 0; i < 1000000; i++) {
        if (!XIicPs_BusIsBusy(i2c))
            return 0;
    }
    return -4;
}
