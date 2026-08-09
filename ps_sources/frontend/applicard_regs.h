/* PS view of the applicard_card.sv AxiSimple register file.
 *
 * The card is AxiSimple crossbar slave 7 (see axisimple_wrapper.sv /
 * appletini_yarz_top.sv); registers are 32-bit at base + index*4.
 *
 * Handshake model (documented in README_APPLICARD.md):
 *   - The 6502 writes $C0D1 -> PL latches TOZ80, sets F_Z80, bumps SEQ.
 *     The PS consumes with a seq-matched ACK write to CONTROL.
 *   - The PS writes TO6502 -> PL latches the byte and sets F_6502; the
 *     6502 read of $C0D0 clears it.
 *   - $C0D5 / $C0D7 / Apple bus reset latch the sticky RESET_REQ /
 *     NMI_REQ bits until the PS clears them via CONTROL.
 */

#ifndef APPLICARD_REGS_H
#define APPLICARD_REGS_H

#include <stdint.h>

#define APPLICARD_REGS_BASE        0x40070000U

#define APPLICARD_REG_STATUS       (APPLICARD_REGS_BASE + 0x00U)
#define APPLICARD_REG_TO6502       (APPLICARD_REGS_BASE + 0x04U)
#define APPLICARD_REG_CONTROL      (APPLICARD_REGS_BASE + 0x08U)
#define APPLICARD_REG_DEBUG        (APPLICARD_REGS_BASE + 0x0CU)

/* Slot-5 personality and AD8088 extension. The original Z80 registers above
 * retain their released addresses and behavior. */
#define APPLICARD_REG_MODE         (APPLICARD_REGS_BASE + 0x10U)
#define APPLICARD_REG_AD_STATUS    (APPLICARD_REGS_BASE + 0x14U)
#define APPLICARD_REG_AD_CONTROL   (APPLICARD_REGS_BASE + 0x18U)
#define APPLICARD_REG_AD_PORTS0    (APPLICARD_REGS_BASE + 0x1CU)
#define APPLICARD_REG_AD_PORTS1    (APPLICARD_REGS_BASE + 0x20U)
#define APPLICARD_REG_AD_PORTS2    (APPLICARD_REGS_BASE + 0x24U)
#define APPLICARD_REG_AD_PORTS3    (APPLICARD_REGS_BASE + 0x28U)
#define APPLICARD_REG_BUS_COMMAND  (APPLICARD_REGS_BASE + 0x2CU)
#define APPLICARD_REG_BUS_STATUS   (APPLICARD_REGS_BASE + 0x30U)
#define APPLICARD_REG_BUS_BUFFER   (APPLICARD_REGS_BASE + 0x100U)

#define APPLICARD_MODE_Z80         0U
#define APPLICARD_MODE_AD8088      1U

/* STATUS fields */
#define APPLICARD_STATUS_F_Z80      (1U << 0)  /* byte pending for the Z80 */
#define APPLICARD_STATUS_F_6502     (1U << 1)  /* byte pending for the 6502 */
#define APPLICARD_STATUS_RESET_REQ  (1U << 2)  /* $C0D5 or Apple bus reset */
#define APPLICARD_STATUS_NMI_REQ    (1U << 3)  /* $C0D7 */
#define APPLICARD_STATUS_SEQ(s)     (((s) >> 8) & 0xFFU)
#define APPLICARD_STATUS_DATA(s)    (((s) >> 16) & 0xFFU)

/* CONTROL bits (write-only) */
#define APPLICARD_CONTROL_ACK_TOZ80(seq) \
    (0x1U | ((((uint32_t)(seq)) & 0xFFU) << 8))
#define APPLICARD_CONTROL_CLR_RESET (1U << 1)
#define APPLICARD_CONTROL_CLR_NMI   (1U << 2)

/* AD_STATUS fields. */
#define AD8088_STATUS_CMD_PENDING    (1U << 0)
#define AD8088_STATUS_FLAG           (1U << 1)
#define AD8088_STATUS_RUNNING        (1U << 2)
#define AD8088_STATUS_ABORT_REQ      (1U << 3)
#define AD8088_STATUS_RESET_REQ      (1U << 4)
#define AD8088_STATUS_BUS_BUSY       (1U << 5)
#define AD8088_STATUS_BUS_DONE       (1U << 6)
#define AD8088_STATUS_BUS_ERROR      (1U << 7)
#define AD8088_STATUS_LAST_PORT(s)   (((s) >> 8) & 0x0FU)
#define AD8088_STATUS_BUS_BLOCKED    (1U << 12)
#define AD8088_STATUS_LAST_DATA(s)   (((s) >> 16) & 0xFFU)
#define AD8088_STATUS_SEQ(s)         (((s) >> 24) & 0xFFU)

/* AD_CONTROL bits. ACK is sequence-matched; SET_FLAG is deliberately
 * separate because CALL starts user code with the card busy, while ordinary
 * PROM commands finish by raising ready. */
#define AD8088_CONTROL_ACK(seq) \
    (0x1U | ((((uint32_t)(seq)) & 0xFFU) << 8))
#define AD8088_CONTROL_SET_FLAG       (1U << 1)
#define AD8088_CONTROL_CLEAR_FLAG     (1U << 2)
#define AD8088_CONTROL_SET_RUNNING    (1U << 3)
#define AD8088_CONTROL_CLEAR_RUNNING  (1U << 4)
#define AD8088_CONTROL_ACK_REQUESTS   (1U << 5)
#define AD8088_CONTROL_MONITOR_RESET  (1U << 6)
#define AD8088_CONTROL_CANCEL_BUS     (1U << 7)
#define AD8088_CONTROL_FINISH(seq) \
    (AD8088_CONTROL_ACK(seq) | AD8088_CONTROL_SET_FLAG)

/* BUS_COMMAND: [15:0] Apple address, [23:16] write byte, [24] read/write,
 * [30] bulk mode, [31] GO. In bulk mode [23:16] is count-minus-one and
 * APPLICARD_REG_BUS_BUFFER exposes one four-byte staging word. Four bytes keep
 * the complete physical DMA hold below the 6502's 10 us retention limit.
 * BUS_STATUS: [7:0] read byte, [8] busy, [9] done, [10] error, [11] blocked,
 * [12] bulk, [23:16] last bulk index and [31:24] transaction sequence.
 * Bit 11 is latched per transaction: it stays set when the command was
 * rejected or stopped because another owner (Disk II window, vTW) held the
 * bus, even if that window closed before the PS read the completion. It
 * clears when the next command is accepted. */
#define AD8088_BUS_COMMAND_GO         (1UL << 31)
#define AD8088_BUS_COMMAND_BULK       (1UL << 30)
#define AD8088_BUS_COMMAND_RW         (1UL << 24)
#define AD8088_BUS_COMMAND_WDATA(v)   ((((uint32_t)(v)) & 0xFFU) << 16)
#define AD8088_BUS_COMMAND_COUNT(v)   (((((uint32_t)(v)) - 1U) & 0xFFU) << 16)
#define AD8088_BUS_STATUS_BUSY        (1U << 8)
#define AD8088_BUS_STATUS_DONE        (1U << 9)
#define AD8088_BUS_STATUS_ERROR       (1U << 10)
#define AD8088_BUS_STATUS_BLOCKED     (1U << 11)
#define AD8088_BUS_STATUS_BULK        (1U << 12)
#define AD8088_BUS_STATUS_INDEX(s)    (((s) >> 16) & 0xFFU)
#define AD8088_BUS_STATUS_SEQ(s)      (((s) >> 24) & 0xFFU)
#define AD8088_BUS_BUFFER_BYTES       4U

#endif /* APPLICARD_REGS_H */
