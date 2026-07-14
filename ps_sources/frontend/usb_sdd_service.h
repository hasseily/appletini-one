/******************************************************************************
 * usb_sdd_service.h -- SuperDuperDisplay bus-event streaming over USB0.
 *
 * When active, USB0 enumerates as an FTDI FT601 (usb_ft60x.h /
 * usb0_personality.h) and this service streams one 32-bit event per Apple
 * bus cycle to the host using SuperDuperDisplay register-message framing:
 *
 *   card->host:  [ (incr<<31)|count, address, data... ]
 *     address 0x1004: bus events, batches of up to 4000 words
 *     address 0x1000: status word (bit0 EventEnable, bit1 Overflow)
 *   host->card:  same framing
 *     address 0x1000: bit0 enables bus events (re-enable clears overflow)
 *     address 0x0014: NSC time (2 words, BCD nibbles; second word 0x0018)
 *     address 0x0008: acknowledged watchdog heartbeat
 *
 * Event source: the PL sdd_bus_tap -> dedicated apple_cycle_egress ring
 * (HP2 write) at SDD_RING_BASE; each 64-bit record carries the SDD event
 * word in its low half. All-zero records are egress overflow gap markers
 * and trigger the SDD overflow protocol.
 *
 * The service owns the USB0 controller while active; the mass-storage
 * service must be disconnected first (main.c orchestrates the switch).
 ******************************************************************************/

#ifndef USB_SDD_SERVICE_H
#define USB_SDD_SERVICE_H

#include <stdint.h>

#include "card_control_regs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* PL register indices 0x50..0x5A in the card-control block. */
#define SDD_REG_CFG_ENABLE          CARD_CTRL_REG_ADDR(0x50U)
#define SDD_REG_CFG_RING_BASE       CARD_CTRL_REG_ADDR(0x51U)
#define SDD_REG_CFG_RING_SIZE_LOG2  CARD_CTRL_REG_ADDR(0x52U)
#define SDD_REG_CFG_PRODUCER_ADDR   CARD_CTRL_REG_ADDR(0x53U)
#define SDD_REG_CFG_CONSUMER_PTR    CARD_CTRL_REG_ADDR(0x54U)
#define SDD_REG_CFG_RESET_PULSE     CARD_CTRL_REG_ADDR(0x55U)
#define SDD_REG_STAT_PRODUCER_PTR   CARD_CTRL_REG_ADDR(0x56U)
#define SDD_REG_STAT_RECORDS        CARD_CTRL_REG_ADDR(0x57U)
#define SDD_REG_STAT_GAP_MARKERS    CARD_CTRL_REG_ADDR(0x58U)
#define SDD_REG_STAT_BURSTS         CARD_CTRL_REG_ADDR(0x59U)
#define SDD_REG_STAT_FULL_STALLS    CARD_CTRL_REG_ADDR(0x5AU)

/* DDR layout: inside the 0x3F000000 NORM_NONCACHE section shared with the
 * renderer egress (see compositor_layout.h). 0x3F021000..0x3F0FFFFF is
 * free; we take the producer-pointer page at 0x3F030000 and a 512 KB ring
 * at 0x3F040000. */
#define SDD_PRODUCER_PTR_ADDR   0x3F030000U
#define SDD_RING_BASE           0x3F040000U
#define SDD_RING_SIZE_LOG2      19U    /* ~125 ms of full-rate events */
#define SDD_RING_SIZE_BYTES     (1U << SDD_RING_SIZE_LOG2)
#define SDD_RECORD_BYTES        8U

/* USB0 dQH/dTD/buffer arena for the FT601 personality. Lives in the same
 * NORM_NONCACHE 1 MB section as the ring: the XUsbPs descriptor machinery
 * is unsafe in cached DDR without the storage path's hand-tuned cache
 * maintenance. Non-cacheable storage keeps descriptor and DMA views coherent. */
#define SDD_DMA_ARENA_ADDR      0x3F0C0000U /* after the 512 KB ring */
#define SDD_DMA_ARENA_BYTES     (64U * 1024U)

/* Lifecycle: init() once at boot (records state only). start() assumes the
 * storage service is already soft-disconnected; it reconfigures USB0 with
 * the FT601 personality and attaches. stop() detaches USB0 and returns the
 * controller to the storage personality without re-attaching it. */
int  usb_sdd_service_init(void);
int  usb_sdd_service_start(void);
void usb_sdd_service_stop(void);
int  usb_sdd_service_active(void);

/* Main-loop poll: drains the PL ring into USB IN transfers and handles
 * host->card messages. Cheap no-op while inactive. */
void usb_sdd_service_poll(void);

/* Called from the FT60x personality when the host completes
 * SET_CONFIGURATION (enumeration finished). */
void usb_sdd_service_note_configured(void);

/* One-line status for the uart control 'sdd' command. */
void usb_sdd_service_print_status(uint32_t uart_base);

#ifdef __cplusplus
}
#endif

#endif /* USB_SDD_SERVICE_H */
