#ifndef CARD_CONTROL_REGS_H
#define CARD_CONTROL_REGS_H

#include <stdint.h>

#ifndef APPLE_DEBUG_BASE
#define APPLE_DEBUG_BASE 0x40000000U
#endif

#define CARD_CTRL_REG_ADDR(index) (APPLE_DEBUG_BASE + ((uint32_t)(index) * 4U))

#define CARD_CTRL_SLOT_ENABLE_REG          CARD_CTRL_REG_ADDR(0x00U)
#define CARD_CTRL_FEATURE_ENABLE_REG       CARD_CTRL_REG_ADDR(0x01U)
#define CARD_CTRL_SOFTSW_STATE_REG         CARD_CTRL_REG_ADDR(0x02U)
#define RESET_RELEASE_REG                  CARD_CTRL_REG_ADDR(0x03U)
#define CARD_CTRL_MENU_CHIME_REG           CARD_CTRL_REG_ADDR(0x07U)
#define CARD_CTRL_PHASOR_PAN_LO_REG        CARD_CTRL_REG_ADDR(0x08U)
#define CARD_CTRL_APPLE_RESET_STATUS_REG   CARD_CTRL_REG_ADDR(0x09U)
#define CARD_CTRL_PHASOR_PAN_HI_REG        CARD_CTRL_REG_ADDR(0x0AU)
#define CARD_CTRL_PHASOR_AUDIO_REG         CARD_CTRL_REG_ADDR(0x0CU)
#define CARD_CTRL_DISK2_SOUND_BASE_REG     CARD_CTRL_REG_ADDR(0x10U)
#define CARD_CTRL_DISK2_SOUND_CONTROL_REG  CARD_CTRL_REG_ADDR(0x11U)

/* Card feature-enable bits (CARD_CTRL_FEATURE_ENABLE_REG). */
#define CARD_CTRL_FEATURE_NSC_ENABLE_BIT         (1UL << 0)
#define CARD_CTRL_FEATURE_SUPERSPRITE_ENABLE_BIT (1UL << 1)
#define CARD_CTRL_FEATURE_SSC_ENABLE_BIT         (1UL << 2)

/* SuperSprite (TMS9918 VDP) PS-facing readback window. The PL owns the VDP
 * register/VRAM interface; the PS renders the picture in software.
 *   SS_REGS_LO/HI : VDP registers R0..R7 (R0 in byte 0 of LO).
 *   SS_STATUS     : [7:0] status byte, [23:8] frame counter,
 *                   [24] apple_video switch, [25] vdp_overlay switch.
 *   SS_VRAM_DATA  : reads VRAM[SS_VRAM_ADDR] (write the address first).
 *   SS_VRAM_ADDR  : 14-bit VRAM read pointer.
 *   SS_SPR_FLAGS  : PS-computed sprite status {5S, C, fifth_num[4:0]} merged
 *                   into the Apple-visible status register. */
#define CARD_CTRL_SS_REGS_LO_REG           CARD_CTRL_REG_ADDR(0x40U)
#define CARD_CTRL_SS_REGS_HI_REG           CARD_CTRL_REG_ADDR(0x41U)
#define CARD_CTRL_SS_STATUS_REG            CARD_CTRL_REG_ADDR(0x42U)
#define CARD_CTRL_SS_VRAM_DATA_REG         CARD_CTRL_REG_ADDR(0x43U)
#define CARD_CTRL_SS_VRAM_ADDR_REG         CARD_CTRL_REG_ADDR(0x44U)
#define CARD_CTRL_SS_SPR_FLAGS_REG         CARD_CTRL_REG_ADDR(0x45U)

/* W5100S host-access window. The PL Uthernet II bridge owns the physical
 * parallel bus; these registers let CPU0 issue single-byte reads/writes
 * through the same sequencer without changing the Apple-visible shadow
 * registers. */
#define CARD_CTRL_ETH_ADDR_REG             CARD_CTRL_REG_ADDR(0x46U)
#define CARD_CTRL_ETH_DATA_REG             CARD_CTRL_REG_ADDR(0x47U)
#define CARD_CTRL_ETH_CMD_REG              CARD_CTRL_REG_ADDR(0x48U)
#define CARD_CTRL_ETH_STATUS_REG           CARD_CTRL_REG_ADDR(0x49U)
#define CARD_CTRL_ETH_CMD_GO               (1UL << 0)
#define CARD_CTRL_ETH_CMD_WRITE            (1UL << 1)
#define CARD_CTRL_ETH_STATUS_READY         (1UL << 0)
#define CARD_CTRL_ETH_STATUS_BUSY          (1UL << 1)
#define CARD_CTRL_ETH_STATUS_DONE          (1UL << 2)
#define CARD_CTRL_ETH_STATUS_ERROR         (1UL << 3)
#define CARD_CTRL_ETH_STATUS_RDATA_SHIFT   8U
#define CARD_CTRL_ETH_STATUS_RDATA_MASK    0xFFUL
#define CARD_CTRL_ETH_STATUS_SEQ_SHIFT     16U
#define CARD_CTRL_ETH_STATUS_SEQ_MASK      0xFFUL

/* Virtual SSC printer FIFO drain window (ssc_card, slot 1). Every byte the
 * Apple writes to the ACIA transmit register queues in a 2 KB PL FIFO;
 * the printer service pops one byte per SSC_CTRL_POP write.
 *   SSC_STATUS : [11:0] queued byte count, [16] overflow (sticky),
 *                [17] card enabled.
 *   SSC_HEAD   : [7:0] oldest byte, [8] valid.
 *   SSC_CTRL   : write-only strobes.
 *   SSC_ACIA   : [7:0] 6551 command latch, [15:8] control latch. */
#define CARD_CTRL_SSC_STATUS_REG           CARD_CTRL_REG_ADDR(0x4AU)
#define CARD_CTRL_SSC_HEAD_REG             CARD_CTRL_REG_ADDR(0x4BU)
#define CARD_CTRL_SSC_CTRL_REG             CARD_CTRL_REG_ADDR(0x4CU)
#define CARD_CTRL_SSC_ACIA_REG             CARD_CTRL_REG_ADDR(0x4DU)
#define CARD_CTRL_SSC_STATUS_COUNT_MASK    0x0FFFUL
#define CARD_CTRL_SSC_STATUS_OVERFLOW      (1UL << 16)
#define CARD_CTRL_SSC_STATUS_ENABLED       (1UL << 17)
#define CARD_CTRL_SSC_HEAD_VALID           (1UL << 8)
#define CARD_CTRL_SSC_CTRL_POP             (1UL << 0)
#define CARD_CTRL_SSC_CTRL_CLEAR           (1UL << 1)
#define CARD_CTRL_SSC_CTRL_OVF_CLEAR       (1UL << 2)

/* Session-only ONE//e stand-alone supervisor. A write to bit 0 is the
 * operator's current request. Readback reports the PL safety interlock:
 *   [0] request, [1] effective, [2] physical bus isolated,
 *   [3] outputs forced off, [4] live Apple activity,
 *   [5] sticky activity lockout, [6] connector quiet,
 *   [7] a new manual selection is armed, [8] ONE//e selected,
 *   [9] isolation hold, [12:10] inhibit reason,
 *   [13] power-sense pin fitted, [14] Apple power sensed,
 *   [15] ONE//e HDL present, [31:24] signature/version (0xE1).
 * Firmware may write REQUEST high only from the explicit Boot Settings
 * action. Polling and startup paths may only clear it. */
#define CARD_CTRL_ONEE_MODE_REG                  CARD_CTRL_REG_ADDR(0x5BU)
#define CARD_CTRL_ONEE_CTRL_REQUEST_BIT          (1UL << 0)
#define CARD_CTRL_ONEE_STATUS_REQUEST_BIT        (1UL << 0)
#define CARD_CTRL_ONEE_STATUS_EFFECTIVE_BIT      (1UL << 1)
#define CARD_CTRL_ONEE_STATUS_ISOLATED_BIT       (1UL << 2)
#define CARD_CTRL_ONEE_STATUS_OUTPUTS_OFF_BIT     (1UL << 3)
#define CARD_CTRL_ONEE_STATUS_ACTIVITY_BIT       (1UL << 4)
#define CARD_CTRL_ONEE_STATUS_LOCKOUT_BIT        (1UL << 5)
#define CARD_CTRL_ONEE_STATUS_QUIET_BIT          (1UL << 6)
#define CARD_CTRL_ONEE_STATUS_RESELECT_ARMED_BIT (1UL << 7)
#define CARD_CTRL_ONEE_STATUS_SELECTED_BIT       (1UL << 8)
#define CARD_CTRL_ONEE_STATUS_ISOLATION_HOLD_BIT (1UL << 9)
#define CARD_CTRL_ONEE_STATUS_INHIBIT_SHIFT      10U
#define CARD_CTRL_ONEE_STATUS_INHIBIT_MASK       0x7UL
#define CARD_CTRL_ONEE_STATUS_POWER_SENSE_PRESENT_BIT (1UL << 13)
#define CARD_CTRL_ONEE_STATUS_APPLE_POWER_BIT    (1UL << 14)
#define CARD_CTRL_ONEE_STATUS_HDL_PRESENT_BIT    (1UL << 15)
#define CARD_CTRL_ONEE_STATUS_SIGNATURE_SHIFT    24U
#define CARD_CTRL_ONEE_STATUS_SIGNATURE_MASK     0xFFUL
#define CARD_CTRL_ONEE_STATUS_SIGNATURE          0xE1UL
#define CARD_CTRL_ONEE_INHIBIT_NONE              0U
#define CARD_CTRL_ONEE_INHIBIT_RESET             1U
#define CARD_CTRL_ONEE_INHIBIT_APPLE_POWER       2U
#define CARD_CTRL_ONEE_INHIBIT_APPLE_ACTIVITY    3U
#define CARD_CTRL_ONEE_INHIBIT_ACTIVITY_LOCKOUT  4U
#define CARD_CTRL_ONEE_INHIBIT_RESELECT_REQUIRED 5U
#define CARD_CTRL_ONEE_INHIBIT_MANUAL_OFF        6U

/* ONE//e virtual video standard. Write DESIRED_50HZ (0 NTSC, 1 PAL).
 * Read DESIRED_50HZ for the saved request and ACTIVE_50HZ for the cadence
 * held by the current session. ACTIVE changes only while ONE//e is stopped
 * or its resolved private RESET line is low. */
#define CARD_CTRL_ONEE_VIDEO_REG                  CARD_CTRL_REG_ADDR(0xA2U)
#define CARD_CTRL_ONEE_VIDEO_DESIRED_50HZ_BIT     (1UL << 0)
#define CARD_CTRL_ONEE_VIDEO_ACTIVE_50HZ_BIT      (1UL << 1)

/* Written by the PS after the boot ROM reports the host machine. The PL
 * interlocks INH
 * and DMA on it. UNKNOWN is the reset state and is treated as a GS
 * (maximum caution). */
#define CARD_CTRL_MACHINE_MODE_REG         CARD_CTRL_REG_ADDR(0x60U)
#define CARD_CTRL_RESET_FORENSICS_REG      CARD_CTRL_REG_ADDR(0x64U)
#define CARD_CTRL_RESET_FORENSICS_RSTN_MASK      0x0FU
#define CARD_CTRL_RESET_FORENSICS_RES_SEEN_BIT   (1UL << 4)
#define CARD_CTRL_RESET_FORENSICS_INTERNAL_BIT   (1UL << 5)
#define CARD_CTRL_RESET_FORENSICS_EXTERNAL_BIT   (1UL << 6)
#define CARD_MACHINE_MODE_UNKNOWN          0U
#define CARD_MACHINE_MODE_IIPLUS           1U
#define CARD_MACHINE_MODE_IIE              2U
#define CARD_MACHINE_MODE_IIGS             3U

/* When set, psram_simple serves the auxiliary 64K and RamWorks banks from
 * PSRAM. The boot service enables it only for a detected //e, with RAM enabled
 * in configuration and no physical auxiliary card detected. Reset state 0 is
 * snoop-only. */
#define CARD_CTRL_AUX_PROVIDE_REG          CARD_CTRL_REG_ADDR(0x61U)

/* Virtual TransWarp accelerator (vtw_core_top). CTRL bit0 requests the
 * session (the PL additionally gates on machine mode == IIe or II/II+),
 * bit1 releases the 65C02 core, [3:2] pick the speed mode, bit6 ignores
 * every software write to $C074, bit7 disables the private Disk II read
 * shortcut, bit8 pauses the core at a completed cycle without resetting it,
 * and [31:16] holds the divided-mode pace.
 * SHADOW_ADDR/DATA expose the 144 KB shadow through an auto-incrementing
 * byte port; SYNC_CMD/STATUS issue single real bus cycles while the core is
 * held (the boot-time ROM copy path). */
/* Interrupt-chain debug (II+ mouse/Phasor freeze diagnosis). Read-only.
 * Saturating 8-bit counts of each link plus mode/flag snapshots. Counts
 * reset with the card-emulation reset (power-on / config reset). */
#define CARD_CTRL_IRQDBG_MOUSE_REG         CARD_CTRL_REG_ADDR(0x2BU)
#define CARD_CTRL_IRQDBG_PHASOR_REG        CARD_CTRL_REG_ADDR(0x2CU)
/* Mouse word: [31:24] VBL-pulse count, [23:16] assert_irq count,
 * [9] vbl_pending, [8] irq_pending, [3:0] mouse mode register. */
#define CARD_CTRL_IRQDBG_MOUSE_VBL_SHIFT   24U
#define CARD_CTRL_IRQDBG_MOUSE_IRQ_SHIFT   16U
#define CARD_CTRL_IRQDBG_MOUSE_VBLPEND_BIT 0x0200U
#define CARD_CTRL_IRQDBG_MOUSE_IRQPEND_BIT 0x0100U
#define CARD_CTRL_IRQDBG_MOUSE_MODE_MASK   0x0FU
/* Phasor word: [31:24] SSI backend-done count, [23:16] SSI direct_irq count,
 * [15:8] CPU $FFFE IRQ/BRK-vector fetch count, [0] SSI ints-enabled flag. */
#define CARD_CTRL_IRQDBG_SSI_BACKEND_SHIFT 24U
#define CARD_CTRL_IRQDBG_SSI_IRQ_SHIFT     16U
#define CARD_CTRL_IRQDBG_VEC_SHIFT         8U
#define CARD_CTRL_IRQDBG_SSI_ENABLE_BIT    0x0001U
#define CARD_CTRL_IRQDBG_COUNT_MASK        0xFFU

/* Bus-capture forensics (II+ ghost-write investigation). Read-only
 * except 0x2D: writing anything there clears every counter and the
 * mouse write ring. All counters saturate at 0xFFFF.
 *   QUALITY  {PHI0 ring events[31:16], short (<300ns) edges[15:0]}
 *   TAPMM    {read tap-mismatches[31:16], write tap-mismatches[15:0]}
 *   STROBE   {extra data_en[31:16], missing data_en[15:0]}
 *   TAPLAST  {addr[31:16], early byte[15:8], late byte[7:0]}
 *   MRING0-3 last 8 mouse DEVSEL ($C0A0-AF) writes, 2 per register,
 *            entry {4'b0, addr[3:0], data[7:0]}, newest in MRING0[15:0]. */
#define CARD_CTRL_BUSDBG_QUALITY_REG       CARD_CTRL_REG_ADDR(0x2DU)
#define CARD_CTRL_BUSDBG_TAPMM_REG         CARD_CTRL_REG_ADDR(0x2EU)
#define CARD_CTRL_BUSDBG_STROBE_REG        CARD_CTRL_REG_ADDR(0x2FU)
#define CARD_CTRL_BUSDBG_TAPLAST_REG       CARD_CTRL_REG_ADDR(0x30U)
#define CARD_CTRL_BUSDBG_MRING_REG(n)      CARD_CTRL_REG_ADDR(0x31U + (n))
/* {ghost writes[31:16], last ghost address[15:0]}: owned-but-undriven bus
 * cycles whose R/W sampled low -- floating residue that matured into a
 * write no master issued. Cleared with the other busdbg counters. */
#define CARD_CTRL_BUSDBG_GHOSTWR_REG       CARD_CTRL_REG_ADDR(0x3AU)

/* SHR paged-mode fallback: bit 0 widens the vTW posted-write window to main
 * $6000-$9FFF. The vTW core tracks aux $9DF8 directly during accelerated
 * writes; CPU1 also drives this bit from captured/rendered state so takeover
 * and recovery start with the right window. */
#define CARD_CTRL_VIDEO_POST_WIDE_REG      CARD_CTRL_REG_ADDR(0x35U)

/* vTW physical-transaction forensics; cleared with "busdbg clear".
 * WR_CHECK {mismatch_count[15:0], expected[7:0], observed[7:0]}
 * WR_ADDR  last mismatching synchronous-write address in [15:0]
 * C000_CTX {previous address[15:0], kind[1:0], rw, data_drive, 4'b0,
 *           last $C000 byte[7:0]}; kind 0=park, 1=posted,
 *           2=sync read, 3=sync write
 * C000_CNT {bit7-high reads[15:0], total reads[15:0]} */
#define CARD_CTRL_VTW_WR_CHECK_REG         CARD_CTRL_REG_ADDR(0x36U)
#define CARD_CTRL_VTW_WR_ADDR_REG          CARD_CTRL_REG_ADDR(0x37U)
#define CARD_CTRL_VTW_C000_CTX_REG         CARD_CTRL_REG_ADDR(0x38U)
#define CARD_CTRL_VTW_C000_CNT_REG         CARD_CTRL_REG_ADDR(0x39U)

#define CARD_CTRL_VTW_CTRL_REG             CARD_CTRL_REG_ADDR(0x70U)
#define CARD_CTRL_VTW_SHADOW_ADDR_REG      CARD_CTRL_REG_ADDR(0x71U)
#define CARD_CTRL_VTW_SHADOW_DATA_REG      CARD_CTRL_REG_ADDR(0x72U)
#define CARD_CTRL_VTW_SYNC_CMD_REG         CARD_CTRL_REG_ADDR(0x73U)
#define CARD_CTRL_VTW_SYNC_STATUS_REG      CARD_CTRL_REG_ADDR(0x74U)
#define CARD_CTRL_VTW_STATUS_REG           CARD_CTRL_REG_ADDR(0x75U)
#define CARD_CTRL_VTW_CNT_CORE_REG         CARD_CTRL_REG_ADDR(0x76U)
#define CARD_CTRL_VTW_CNT_BUS_REG          CARD_CTRL_REG_ADDR(0x77U)
#define CARD_CTRL_VTW_CNT_POSTED_REG       CARD_CTRL_REG_ADDR(0x78U)
#define CARD_CTRL_VTW_POST_STATS_REG       CARD_CTRL_REG_ADDR(0x79U)
#define CARD_CTRL_VTW_CNT_INVALID_REG      CARD_CTRL_REG_ADDR(0x7AU)
#define CARD_CTRL_VTW_LAST_SYNC_REG        CARD_CTRL_REG_ADDR(0x7BU)
/* Last eight $C1xx-$CFFF vTW bus cycles: two 16-bit addresses per
 * register, newest address in RING0[15:0]. */
#define CARD_CTRL_VTW_CXXX_RING_REG(n)     CARD_CTRL_REG_ADDR(0x7CU + (n))
/* Last eight $C00x/$C01x soft-switch cycles with latched data, two
 * 16-bit entries per register, newest in RING0[15:0]. Entry format:
 * {2'b0, rw, addr[4:0], data[7:0]}. */
#define CARD_CTRL_VTW_C0_RING_REG(n)       CARD_CTRL_REG_ADDR(0x6CU + (n))

/* Event-frozen II+ diagnostics, re-armed by "busdbg clear".
 * TRACE_STATUS: bit0 frozen, [2:1] reason
 *   2 = $C000 read with bit7 high,
 *   3 = instruction fetch from internal $C600 self-test.
 * RESET does not freeze these rings: the post-reset path remains visible,
 * while CARD_CTRL_RESET_FORENSICS_REG records the reset and its source.
 * BUS_FAULTS: {address mismatch count[15:0],
 *              self-drive data mismatch count[7:0],
 *              observed DMA-released count[7:0]}.
 * IO_TRACE entry (newest n=0):
 *   {rw, self_drive, dma_released, addr_bad, data_bad, 3'b0,
 *    requested_addr[15:0], sampled_data[7:0]}.
 * PC_TRACE packs two 16-bit instruction-fetch PCs per register, newest in
 * the low half of register 0. */
#define CARD_CTRL_VTW_TRACE_STATUS_REG      CARD_CTRL_REG_ADDR(0x80U)
#define CARD_CTRL_VTW_BUS_FAULTS_REG        CARD_CTRL_REG_ADDR(0x81U)
#define CARD_CTRL_VTW_IO_TRACE_REG(n)       CARD_CTRL_REG_ADDR(0x82U + (n))
#define CARD_CTRL_VTW_PC_TRACE_REG(n)       CARD_CTRL_REG_ADDR(0x92U + (n))
/* CPU0 may inject a video write into the vTW's normal posted queue while the
 * core waits for a SmartPort response. PUSH packs {data[7:0],addr[15:0]} in
 * bits [23:0]. STATUS bit31 is ready and [30:0] counts accepted pushes. */
#define CARD_CTRL_VTW_POST_PUSH_REG          CARD_CTRL_REG_ADDR(0x9AU)
#define CARD_CTRL_VTW_POST_STATUS_REG        CARD_CTRL_REG_ADDR(0x9BU)
#define CARD_CTRL_VTW_POST_READY_BIT         (1UL << 31)
#define CARD_CTRL_VTW_POST_ACCEPT_MASK       0x7FFFFFFFUL
/* Write bit0 to freeze the vTW core and flush+invalidate its RamWorks line
 * cache; the core stays frozen until bit1 (release) is written, so a PS-DMA
 * block write cannot race any core access. Session end or Apple RES#
 * auto-releases. Status: bit31 busy, bit30 core held, bits 29:0 count
 * completed requests (a request cancelled by auto-release still counts,
 * with held low). */
#define CARD_CTRL_VTW_RW_FLUSH_REG            CARD_CTRL_REG_ADDR(0x9CU)
#define CARD_CTRL_VTW_RW_FLUSH_REQ_BIT        1UL
#define CARD_CTRL_VTW_RW_FLUSH_RELEASE_BIT    2UL
#define CARD_CTRL_VTW_RW_FLUSH_BUSY_BIT       (1UL << 31)
#define CARD_CTRL_VTW_RW_FLUSH_HELD_BIT       (1UL << 30)
#define CARD_CTRL_VTW_RW_FLUSH_COUNT_MASK     0x3FFFFFFFUL
/* Four-byte shadow writes. DATA4 queues one little-endian word. STATUS bit31
 * means queue space is available, bit30 means accepted data is still
 * draining, and bits29:0 count accepted words. A SHADOW_ADDR write clears
 * queued packed data so byte fallback cannot race a late direct write. */
#define CARD_CTRL_VTW_SHADOW_DATA4_REG         CARD_CTRL_REG_ADDR(0x9DU)
#define CARD_CTRL_VTW_SHADOW_DATA4_STATUS_REG  CARD_CTRL_REG_ADDR(0x9EU)
#define CARD_CTRL_VTW_SHADOW_DATA4_READY_BIT   (1UL << 31)
#define CARD_CTRL_VTW_SHADOW_DATA4_BUSY_BIT    (1UL << 30)
#define CARD_CTRL_VTW_SHADOW_DATA4_ACCEPT_MASK 0x3FFFFFFFUL
#define CARD_CTRL_VTW_SHADOW_READ4_REG          CARD_CTRL_REG_ADDR(0x9FU)
#define CARD_CTRL_VTW_SHADOW_READ4_DATA_REG     CARD_CTRL_REG_ADDR(0xA0U)
#define CARD_CTRL_VTW_SHADOW_READ4_STATUS_REG   CARD_CTRL_REG_ADDR(0xA1U)
#define CARD_CTRL_VTW_SHADOW_READ4_READY_BIT    (1UL << 31)
#define CARD_CTRL_VTW_SHADOW_READ4_BUSY_BIT     (1UL << 30)
#define CARD_CTRL_VTW_SHADOW_READ4_COUNT_MASK   0x3FFFFFFFUL
#define CARD_CTRL_VTW_TRACE_FROZEN_BIT      (1UL << 0)
#define CARD_CTRL_VTW_TRACE_REASON_SHIFT    1U
#define CARD_CTRL_VTW_TRACE_REASON_MASK     0x3UL
#define CARD_CTRL_VTW_TRACE_REASON_C000     2U
#define CARD_CTRL_VTW_TRACE_REASON_C600     3U

/* Per-region slowdown (mirrors the physical TransWarp's DIP block 2): after
 * the core touches an enabled timing-sensitive region it drops to
 * cycle-locked 1 MHz for DURATION Apple cycles, retriggered on each such
 * access. [9:0] = region enables; [31:16] = duration in Apple cycles
 * (0 disables the feature). Region-enable bit layout:
 *   [6:0] per-slot device I/O $C0n0-$C0nF for slots 1..7 (bit0 = slot 1)
 *   [7]   floating-bus/video timing: $C019 and $C030-$C05F
 *   [8]   paddle   $C064-$C067 (PADDL0-3) and $C070 (PTRIG)
 *   [9]   reserved */
#define CARD_CTRL_VTW_SLOWDOWN_REG         CARD_CTRL_REG_ADDR(0x6BU)
#define CARD_CTRL_VTW_SLOWDOWN_MASK_SHIFT  0U
#define CARD_CTRL_VTW_SLOWDOWN_MASK_MASK   0x1FFUL
#define CARD_CTRL_VTW_SLOWDOWN_FLOATBUS_BIT (1UL << 7)
#define CARD_CTRL_VTW_SLOWDOWN_PADDLE_BIT  (1UL << 8)
#define CARD_CTRL_VTW_SLOWDOWN_SLOT_BIT(s) (1UL << ((s) - 1U))  /* s = 1..7 */
#define CARD_CTRL_VTW_SLOWDOWN_DUR_SHIFT   16U
#define CARD_CTRL_VTW_SLOWDOWN_DUR_MASK    0xFFFFUL

#define CARD_CTRL_VTW_CTRL_ENABLE_BIT      (1UL << 0)
#define CARD_CTRL_VTW_CTRL_CORE_RUN_BIT    (1UL << 1)
#define CARD_CTRL_VTW_CTRL_SPEED_SHIFT     2U
#define CARD_CTRL_VTW_CTRL_SPEED_MASK      0x3UL
#define CARD_CTRL_VTW_CTRL_APPLE_RES_BIT   (1UL << 4)
/* II/II+ host: serve $C061-$C063 Apple-key reads internally as $00
 * (controller-less game-connector buttons float "pressed"). Set by the
 * PS whenever the machine is a II/II+; clear to restore real reads for
 * hosts with a controller attached. */
#define CARD_CTRL_VTW_CTRL_IIPLUS_BTNS_BIT (1UL << 5)
/* Discard every core write to $C074. This also clears an already-latched
 * $C074 state so the configured or USB-selected speed takes effect live. */
#define CARD_CTRL_VTW_CTRL_IGNORE_C074_BIT  (1UL << 6)
/* Route virtual Disk II through the original physical 1 MHz path. */
#define CARD_CTRL_VTW_CTRL_DISABLE_D2_ACCEL_BIT (1UL << 7)
/* Freeze the 65C02 at a completed cycle without dropping CORE_RUN or reset.
 * ONE//e uses this while the Appletini config menu owns USB input. */
#define CARD_CTRL_VTW_CTRL_PAUSE_BIT       (1UL << 8)
/* 16-bit pace divider at [31:16] (50 kHz slug mode needs divider 2667). */
#define CARD_CTRL_VTW_CTRL_DIVIDER_SHIFT   16U
#define CARD_CTRL_VTW_CTRL_DIVIDER_MASK    0xFFFFUL
#define CARD_CTRL_VTW_SPEED_FULL           0U
#define CARD_CTRL_VTW_SPEED_DIVIDED        1U
#define CARD_CTRL_VTW_SPEED_1MHZ           2U
#define CARD_CTRL_VTW_SYNC_CMD_RW_BIT      (1UL << 24)
#define CARD_CTRL_VTW_SYNC_WDATA_SHIFT     16U
#define CARD_CTRL_VTW_SYNC_STATUS_BUSY     (1UL << 8)
#define CARD_CTRL_VTW_SYNC_STATUS_DONE     (1UL << 9)
#define CARD_CTRL_VTW_STATUS_C074_MASK     0x3UL
#define CARD_CTRL_VTW_STATUS_BUS_OWNED     (1UL << 2)
#define CARD_CTRL_VTW_STATUS_ENABLE_EFF    (1UL << 3)
#define CARD_CTRL_VTW_STATUS_CORE_RUN     (1UL << 4)
/* [15:5]: the vTW's private //e switch state, MSB first:
 * {intcxrom, slotc3rom, intc8rom, lc_read, lc_write, lc_bank2,
 *  altzp, ramrd, ramwrt, 80store, text} */
#define CARD_CTRL_VTW_STATUS_VSSS_SHIFT    5U
#define CARD_CTRL_VTW_STATUS_VSSS_MASK     0x7FFUL
#define CARD_CTRL_VTW_STATUS_PC_SHIFT      16U
/* VTW_LAST_SYNC: [15:0] address, [23:16] data, [24] rw (1=read),
 * [31:25] saturating count of IRQ assertions the vTW core observed. */
#define CARD_CTRL_VTW_LAST_SYNC_DATA_SHIFT 16U
#define CARD_CTRL_VTW_LAST_SYNC_RW_BIT     (1UL << 24)
#define CARD_CTRL_VTW_LAST_SYNC_IRQ_SHIFT  25U
#define CARD_CTRL_VTW_LAST_SYNC_IRQ_MASK   0x7FUL

#define RESET_RELEASE_CPU0_READY_BIT       (1UL << 0)

#define CARD_CTRL_DISK2_SOUND_ENABLE_BIT       (1UL << 0)
#define CARD_CTRL_DISK2_SOUND_VOLUME_SHIFT     8U
#define CARD_CTRL_DISK2_SOUND_VOLUME_MASK      0xFUL
#define CARD_CTRL_DISK2_SOUND_EVENT_SHIFT      16U
#define CARD_CTRL_DISK2_SOUND_EVENT_MASK       0xFUL
#define CARD_CTRL_DISK2_SOUND_EVENT_DOOR_OPEN  4U
#define CARD_CTRL_DISK2_SOUND_EVENT_DOOR_CLOSE 5U
#define CARD_CTRL_DISK2_SOUND_DEFAULT_VOLUME   5U
#define CARD_CTRL_DISK2_SOUND_MAX_VOLUME       10U


/* CARD_CTRL_PHASOR_AUDIO_REG bits 0..25 are the tone/volume/PSG-mode fields
 * packed by control_set_phasor_audio(). Bit 26 locks the card to Mockingboard-
 * compatible mode: the PL ignores the Apple-bus $C0nX Phasor mode switch so
 * software can never detect or enable Phasor-native behavior. */
#define CARD_CTRL_PHASOR_AUDIO_MOCKINGBOARD_ONLY_BIT (1UL << 26)

#define CARD_CTRL_SLOT_ETHERNET    1U
#define CARD_CTRL_SLOT_MOUSE       2U
#define CARD_CTRL_SLOT_MOCKINGBOARD 4U
#define CARD_CTRL_SLOT_DISK2       6U
#define CARD_CTRL_SLOT_SMARTPORT   7U
#define CARD_CTRL_SLOT_BIT(slot)   (1UL << (slot))
#define CARD_CTRL_SLOT_ENABLE_VALID_MASK 0x0000007EUL
#define CARD_CTRL_SLOT_ENABLE_RESET_MASK \
    (CARD_CTRL_SLOT_BIT(CARD_CTRL_SLOT_ETHERNET) | \
     CARD_CTRL_SLOT_BIT(CARD_CTRL_SLOT_MOUSE) | \
     CARD_CTRL_SLOT_BIT(CARD_CTRL_SLOT_MOCKINGBOARD))
#define CARD_CTRL_SLOT_ENABLE_REQUIRED_MASK CARD_CTRL_SLOT_BIT(CARD_CTRL_SLOT_SMARTPORT)

#define CARD_CTRL_SOFTSW_STATE_MASK           0x001FFFFFUL
#define CARD_CTRL_APPLE_RESET_SEQ_MASK        0x000000FFUL
#define CARD_CTRL_APPLE_RESET_RES_BIT          (1UL << 8)
#define CARD_CTRL_APPLE_RESET_VTW_1MHZ_BIT     (1UL << 9)
#define CARD_CTRL_APPLE_RESET_SHR_ACTIVE_BIT   (1UL << 10)
#define CARD_CTRL_SOFTSW_80STORE_BIT          (1UL << 0)
#define CARD_CTRL_SOFTSW_RAMRD_BIT            (1UL << 1)
#define CARD_CTRL_SOFTSW_RAMWRT_BIT           (1UL << 2)
#define CARD_CTRL_SOFTSW_ALTZP_BIT            (1UL << 3)
#define CARD_CTRL_SOFTSW_TEXT_BIT             (1UL << 4)
#define CARD_CTRL_SOFTSW_MIXED_BIT            (1UL << 5)
#define CARD_CTRL_SOFTSW_PAGE2_BIT            (1UL << 6)
#define CARD_CTRL_SOFTSW_HIRES_BIT            (1UL << 7)
#define CARD_CTRL_SOFTSW_ALTCHARSET_BIT       (1UL << 8)
#define CARD_CTRL_SOFTSW_80COL_BIT            (1UL << 9)
#define CARD_CTRL_SOFTSW_DHIRES_BIT           (1UL << 10)
#define CARD_CTRL_SOFTSW_LCRAM_BANK2_BIT      (1UL << 11)
#define CARD_CTRL_SOFTSW_LCRAM_READ_BIT       (1UL << 12)
#define CARD_CTRL_SOFTSW_LCRAM_WRITE_BIT      (1UL << 13)
#define CARD_CTRL_SOFTSW_RAMWORKS_BANK_SHIFT  14U
#define CARD_CTRL_SOFTSW_RAMWORKS_BANK_MASK   0x7FU

#endif
