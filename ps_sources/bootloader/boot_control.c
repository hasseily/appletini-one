#include "boot_control.h"

#include "../lib/common.h"

#define SLCR_BASE            0xF8000000U
#define SLCR_LOCK            (SLCR_BASE + 0x0004U)
#define SLCR_UNLOCK          (SLCR_BASE + 0x0008U)
#define SLCR_LOCK_KEY        0x0000767BU
#define SLCR_UNLOCK_KEY      0x0000DF0DU
#define PS_RST_CTRL_REG      (SLCR_BASE + 0x0200U)
#define PS_RST_MASK          0x00000001U

#define DEVCFG_BASE          0xF8007000U
#define DEVCFG_MULTIBOOT     (DEVCFG_BASE + 0x002CU)

#define ZYNQ_MULTIBOOT_GRANULARITY_BYTES 0x8000U

void boot_control_soft_reset(void)
{
    REG_WRITE(SLCR_UNLOCK, SLCR_UNLOCK_KEY);
    REG_WRITE(PS_RST_CTRL_REG, PS_RST_MASK);
    REG_WRITE(SLCR_LOCK, SLCR_LOCK_KEY);
    for (;;) {
        __asm__ volatile("nop");
    }
}

void boot_control_boot_qspi_image_offset(uint32_t image_offset_bytes)
{
    uint32_t mb = image_offset_bytes / ZYNQ_MULTIBOOT_GRANULARITY_BYTES;

    /* Zynq FSBL computes image start = multiboot_reg * 0x8000. */
    REG_WRITE(SLCR_UNLOCK, SLCR_UNLOCK_KEY);
    REG_WRITE(DEVCFG_MULTIBOOT, mb & 0x1FFFU);
    REG_WRITE(PS_RST_CTRL_REG, PS_RST_MASK);
    REG_WRITE(SLCR_LOCK, SLCR_LOCK_KEY);
    for (;;) {
        __asm__ volatile("nop");
    }
}
