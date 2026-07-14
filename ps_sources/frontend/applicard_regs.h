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

#endif /* APPLICARD_REGS_H */
