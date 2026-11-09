#ifndef TEST_XIICPS_H
#define TEST_XIICPS_H

#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t s32;
typedef uintptr_t UINTPTR;

#define XPAR_XIICPS_0_BASEADDR 0xE0004000U
#define XIL_COMPONENT_IS_READY 0x11111111U
#define XIICPS_FIFO_DEPTH 16

#define XIICPS_CR_OFFSET  0x00U
#define XIICPS_SR_OFFSET  0x04U
#define XIICPS_ISR_OFFSET 0x10U
#define XIICPS_IMR_OFFSET 0x20U
#define XIICPS_IER_OFFSET 0x24U
#define XIICPS_IDR_OFFSET 0x28U

#define XIICPS_SR_BA_MASK         0x0100U
#define XIICPS_IXR_COMP_MASK      0x0001U
#define XIICPS_IXR_DATA_MASK      0x0002U
#define XIICPS_IXR_NACK_MASK      0x0004U
#define XIICPS_IXR_TO_MASK        0x0008U
#define XIICPS_IXR_SLV_RDY_MASK   0x0010U
#define XIICPS_IXR_RX_OVR_MASK    0x0020U
#define XIICPS_IXR_ARB_LOST_MASK  0x0200U
#define XIICPS_IXR_ALL_INTR_MASK  0x02FFU

#define XIICPS_EVENT_COMPLETE_SEND 0x0001U
#define XIICPS_EVENT_COMPLETE_RECV 0x0002U
#define XIICPS_EVENT_TIME_OUT      0x0004U
#define XIICPS_EVENT_ERROR         0x0008U
#define XIICPS_EVENT_ARB_LOST      0x0010U
#define XIICPS_EVENT_NACK          0x0020U
#define XIICPS_EVENT_RX_OVR        0x0080U
#define XIICPS_EVENT_TX_OVR        0x0100U
#define XIICPS_EVENT_RX_UNF        0x0200U

typedef void (*XIicPs_IntrHandler)(void *, u32);

typedef struct {
    UINTPTR BaseAddress;
} XIicPs_Config;

typedef struct {
    XIicPs_Config Config;
    u32 IsReady;
    u32 IsRepeatedStart;
    XIicPs_IntrHandler StatusHandler;
    void *CallBackRef;
} XIicPs;

XIicPs_Config *XIicPs_LookupConfig(UINTPTR base);
s32 XIicPs_CfgInitialize(XIicPs *i2c, XIicPs_Config *config, UINTPTR base);
s32 XIicPs_SetSClk(XIicPs *i2c, u32 frequency);
s32 XIicPs_MasterSendPolled(XIicPs *i2c, u8 *data, s32 len, u16 address);
s32 XIicPs_MasterRecvPolled(XIicPs *i2c, u8 *data, s32 len, u16 address);
s32 XIicPs_BusIsBusy(XIicPs *i2c);
void XIicPs_MasterSend(XIicPs *i2c, u8 *data, s32 len, u16 address);
void XIicPs_MasterRecv(XIicPs *i2c, u8 *data, s32 len, u16 address);
void XIicPs_MasterInterruptHandler(XIicPs *i2c);
void XIicPs_SetStatusHandler(XIicPs *i2c, void *ref, XIicPs_IntrHandler handler);
void XIicPs_Abort(XIicPs *i2c);
u32 XIicPs_ReadReg(UINTPTR base, u32 offset);
void XIicPs_WriteReg(UINTPTR base, u32 offset, u32 data);

#define XIicPs_EnableInterrupts(base, mask) XIicPs_WriteReg(base, XIICPS_IER_OFFSET, mask)
#define XIicPs_DisableInterrupts(base, mask) XIicPs_WriteReg(base, XIICPS_IDR_OFFSET, mask)
#define XIicPs_DisableAllInterrupts(base) XIicPs_DisableInterrupts(base, XIICPS_IXR_ALL_INTR_MASK)

#endif
