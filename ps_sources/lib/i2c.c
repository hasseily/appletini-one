#include "i2c.h"
#include "xstatus.h"
#include <stdio.h>

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
