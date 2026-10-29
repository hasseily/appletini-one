/* SPDX-License-Identifier: MIT
 * Host facade for the BSP's low-level XAdcPs macros. Register constants
 * match UG585 and xadcps_hw.h; no generated workspace is needed to test. */
#ifndef HTTP_ENTROPY_HOST_XADCPS_H
#define HTTP_ENTROPY_HOST_XADCPS_H
#include <stdint.h>
typedef struct { struct { uint32_t BaseAddress; } Config; uint32_t IsReady; } XAdcPs;
uint32_t http_entropy_mmio_read(uint32_t address);
void http_entropy_mmio_write(uint32_t address, uint32_t value);
#define Xil_In32(a) http_entropy_mmio_read(a)
#define Xil_Out32(a,v) http_entropy_mmio_write(a,v)
#define XAdcPs_ReadReg(b,o) Xil_In32((b)+(o))
#define XAdcPs_WriteReg(b,o,v) Xil_Out32((b)+(o),(v))
#define XAdcPs_WriteFifo(i,v) XAdcPs_WriteReg((i)->Config.BaseAddress,0x10U,(v))
#define XAdcPs_ReadFifo(i) XAdcPs_ReadReg((i)->Config.BaseAddress,0x14U)
#define XIL_COMPONENT_IS_READY 0x11111111U
#define XADCPS_CFG_OFFSET 0x00U
#define XADCPS_INT_STS_OFFSET 0x04U
#define XADCPS_INT_MASK_OFFSET 0x08U
#define XADCPS_MSTS_OFFSET 0x0CU
#define XADCPS_MCTL_OFFSET 0x18U
#define XADCPS_CFG_ENABLE_MASK 0x80000000U
#define XADCPS_CFG_WEDGE_MASK 0x2000U
#define XADCPS_CFG_REDGE_MASK 0x1000U
#define XADCPS_CFG_TCKRATE_MASK 0x300U
#define XADCPS_CFG_IGAP_MASK 0x1FU
#define XADCPS_INTX_ALL_MASK 0x3FFU
#define XADCPS_INTX_CFIFO_LTH_MASK 0x200U
#define XADCPS_INTX_DFIFO_GTH_MASK 0x100U
#define XADCPS_MSTS_CFIFOF_MASK 0x800U
#define XADCPS_MSTS_CFIFOE_MASK 0x400U
#define XADCPS_MSTS_DFIFOE_MASK 0x100U
#define XADCPS_MCTL_RESET_MASK 0x10U
#define XADCPS_MCTL_FLUSH_MASK 0x01U
#define XADCPS_TEMP_OFFSET 0x00U
#define XADCPS_CFR0_OFFSET 0x40U
#define XADCPS_CFR1_OFFSET 0x41U
#define XADCPS_CFR2_OFFSET 0x42U
#define XADCPS_CFR0_CAL_AVG_MASK 0x8000U
#define XADCPS_CFR0_AVG_VALID_MASK 0x3000U
#define XADCPS_CFR0_MUX_MASK 0x800U
#define XADCPS_CFR0_DU_MASK 0x400U
#define XADCPS_CFR0_EC_MASK 0x200U
#define XADCPS_CFR0_ACQ_MASK 0x100U
#define XADCPS_CFR0_CHANNEL_MASK 0x1FU
#define XADCPS_CFR1_SEQ_VALID_MASK 0xF000U
#define XADCPS_CFR1_SEQ_SINGCHAN_MASK 0x3000U
#define XADCPS_JTAG_CMD_READ_MASK 0x04000000U
#define XADCPS_JTAG_CMD_WRITE_MASK 0x08000000U
#define XADCPS_JTAG_ADDR_SHIFT 16U
#define XADCPS_UNLK_VALUE 0x757BDF0DU
#endif
