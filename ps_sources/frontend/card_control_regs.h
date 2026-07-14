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

/* Written by the PS after the boot ROM reports the host machine. The PL
 * interlocks INH
 * and DMA on it. UNKNOWN is the reset state and is treated as a GS
 * (maximum caution). */
#define CARD_CTRL_MACHINE_MODE_REG         CARD_CTRL_REG_ADDR(0x60U)
#define CARD_MACHINE_MODE_UNKNOWN          0U
#define CARD_MACHINE_MODE_IIPLUS           1U
#define CARD_MACHINE_MODE_IIE              2U
#define CARD_MACHINE_MODE_IIGS             3U

/* When set, psram_simple serves the auxiliary 64K and RamWorks banks from
 * PSRAM. The boot service enables it only for a detected //e, with RAM enabled
 * in configuration and no physical auxiliary card detected. Reset state 0 is
 * snoop-only. */
#define CARD_CTRL_AUX_PROVIDE_REG          CARD_CTRL_REG_ADDR(0x61U)

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
