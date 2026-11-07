#ifndef PC_TRANSPORTER_REGS_H
#define PC_TRANSPORTER_REGS_H

#define PCT_MMIO_BASE       0x40080000U
#define PCT_REG(index)      (PCT_MMIO_BASE + 4U * (index))
#define PCT_CARD_ID         0x50435435U

#define PCT_STATUS          PCT_REG(1U)
#define PCT_TRANSFER        PCT_REG(2U)
#define PCT_PORT_INDEX      PCT_REG(3U)
#define PCT_PORT_DATA       PCT_REG(4U)
#define PCT_LATE_READS      PCT_REG(5U)
#define PCT_WRITE_DROPS     PCT_REG(6U)
#define PCT_DDR_ERRORS      PCT_REG(7U)
#define PCT_DIAG_CONTROL    PCT_REG(8U)
#define PCT_KEYS            PCT_REG(9U)
#define PCT_ACK_DATA        PCT_REG(10U)
#define PCT_CGA_STATE       PCT_REG(11U)
#define PCT_CGA_CRTC_LOW    PCT_REG(12U)
#define PCT_CGA_CRTC_HIGH   PCT_REG(13U)

#define PCT_ENABLED         (1U << 0)
#define PCT_RESET           (1U << 1)
#define PCT_HALT            (1U << 2)
#define PCT_INT             (1U << 3)
#define PCT_PENDING         (1U << 5)
#define PCT_DRAINED         (1U << 6)
#define PCT_FAULT           (1U << 7)
#define PCT_POST            (1U << 31)
#define PCT_CLEAR_COUNTERS  (1U << 0)
#define PCT_CANCEL          (1U << 1)
#define PCT_ACCEPT_INT      (1U << 2)

#endif
