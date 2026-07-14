/*
 * framebuffer.h -- AXI register definitions for framebuffer scanout and
 * Apple storage diagnostics. RGB565 drawing primitives live in fb16.h.
 */

#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H

#include <stdint.h>

/*==========================================================================
 * Framebuffer Control Registers (AXI GP0)
 *
 * The fb-control registers live inside video_top's AXI client slot at
 * 0x40010000.
 *
 * Register layout inside video_top:
 *   0x00  FB_BASE_ADDR_REG       (RW) base address PS wants displayed.
 *                                     PL latches at vblank.
 *   0x04                          reserved; reads return zero.
 *   0x08  FB_STATUS_REG          (R)  vblank frame counter (increments
 *                                     once per fb_reader vblank latch).
 *   0x0C  FB_LAST_LATCHED_REG    (R)  base address fb_reader committed
 *                                     to scanning at the last vblank.
 *                                     Compare against comp_out_slot_addr[]
 *                                     to learn which slot is live.
 *==========================================================================*/

#ifndef FB_CONTROL_BASE
#define FB_CONTROL_BASE     0x40010000U
#endif
#ifndef PSRAM_CONTROL_BASE
#define PSRAM_CONTROL_BASE   0x40030000
#endif
#ifndef APPLE_TOP_BASE
#define APPLE_TOP_BASE       0x40020000U
#endif
#define FB_CONTROL_SIZE     0x40000000U
#define FB_BASE_ADDR_REG    (FB_CONTROL_BASE + 0x00)
#define FB_STATUS_REG       (FB_CONTROL_BASE + 0x08)
#define FB_LAST_LATCHED_REG (FB_CONTROL_BASE + 0x0C)
#define FB_DEBUG_REG        (FB_CONTROL_BASE + 0x10)
#define FB_DEBUG2_REG       (FB_CONTROL_BASE + 0x14)
/* FB_DEBUG_REG layout (read-only):
 *   [2:0]   fb_reader FSM state (S_IDLE=0, S_RESET_FIFO=1, S_BURST=2)
 *   [3]     axi_read_err sticky -- SLVERR/DECERR seen on a read beat
 *   [25:8]  burst_count -- bursts completed within current frame
 * FB_DEBUG2_REG layout (read-only):
 *   [31:16] scanout underrun episodes (fb_reader FIFO empty mid-scan)
 *   [15:0]  saturating count of AXI read-response errors */

/* Apple DMA / SmartPort debug windows layered on top of the slot-2 mailbox. */
#define AT_DMA_CTRL_REG             (APPLE_TOP_BASE + 0x300U)
#define AT_DMA_CTRL_HOLD_REQ_BIT    (1U << 0)
#define AT_DMA_CTRL_HOLD_ACTIVE_BIT (1U << 4)
#define AT_DMA_CTRL_SP_ACTIVE_BIT   (1U << 8)
#define AT_DMA_CTRL_ENG_DMA_BIT     (1U << 9)
#define AT_DMA_CTRL_ENG_RDY_BIT     (1U << 10)

#define AT_PROBE_CTRL_REG           (APPLE_TOP_BASE + 0x304U)
#define AT_PROBE_PARAMS_REG         (APPLE_TOP_BASE + 0x308U)
#define AT_PROBE_COUNT_REG          (APPLE_TOP_BASE + 0x30CU)
#define AT_PROBE_CTRL_GO_BIT        (1U << 0)
#define AT_PROBE_CTRL_ACTIVE_BIT    (1U << 16)
#define AT_PROBE_PARAMS_READ_BIT    (1U << 24)

#define AT_TRACE_CTRL_REG           (APPLE_TOP_BASE + 0x310U)
#define AT_TRACE_CFG_REG            (APPLE_TOP_BASE + 0x314U)
#define AT_TRACE_INDEX_REG          (APPLE_TOP_BASE + 0x318U)
#define AT_TRACE_WORD0_REG          (APPLE_TOP_BASE + 0x31CU)
#define AT_TRACE_WORD1_REG          (APPLE_TOP_BASE + 0x320U)
#define AT_TRACE_CTRL_ARM_BIT       (1U << 0)
#define AT_TRACE_CTRL_CAPTURING_BIT (1U << 1)
#define AT_TRACE_CTRL_DONE_BIT      (1U << 2)
#define AT_TRACE_CTRL_CLEAR_BIT     (1U << 7)
#define AT_TRACE_CFG_RELEASE_BIT    (1U << 16)
#define AT_TRACE_DEPTH              128U

#define AT_TAIL_CTRL_REG            (APPLE_TOP_BASE + 0x324U)
#define AT_TAIL_WORD0_REG           (APPLE_TOP_BASE + 0x328U)
#define AT_TAIL_WORD1_REG           (APPLE_TOP_BASE + 0x32CU)
#define AT_TAIL_CTRL_CLEAR_BIT      (1U << 8)
#define AT_TAIL_DEPTH               16U

#define AT_PEEK_INDEX_REG           (APPLE_TOP_BASE + 0x330U)
#define AT_PEEK_DATA_REG            (APPLE_TOP_BASE + 0x334U)
#define AT_PEEK_DEPTH               32U

#define SP_DBG_FIFO_CNT_REG         (APPLE_TOP_BASE + 0x028U)
#define SP_DBG_EXEC_CNT_REG         (APPLE_TOP_BASE + 0x02CU)
#define SP_DBG_DMA_SNAP_REG         (APPLE_TOP_BASE + 0x030U)
#define SP_DBG_RESET_REG            (APPLE_TOP_BASE + 0x034U)
#define SP_DBG_EVT_CTRL_REG         (APPLE_TOP_BASE + 0x038U)
#define SP_DBG_EVT_WORD0_REG        (APPLE_TOP_BASE + 0x03CU)
#define SP_DBG_EVT_WORD1_REG        (APPLE_TOP_BASE + 0x040U)
#define SP_DBG_EVT_DEPTH            8U

#endif
