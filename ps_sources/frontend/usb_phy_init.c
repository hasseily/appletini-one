/******************************************************************************
 * USB PHY initialization for frontend services.
 *
 * The FSBL does not always leave the USB controllers clocked and out of reset.
 * Call usb_phy_early_init() before touching either USB controller.
 ******************************************************************************/

#include <stdint.h>

#include "usb_phy_init.h"

#define SLCR_BASE           0xF8000000U
#define SLCR_UNLOCK         (SLCR_BASE + 0x008U)
#define SLCR_LOCK           (SLCR_BASE + 0x004U)
#define SLCR_USB0_CLK_CTRL  (SLCR_BASE + 0x130U)
#define SLCR_USB1_CLK_CTRL  (SLCR_BASE + 0x134U)
#define SLCR_USB_RST_CTRL   (SLCR_BASE + 0x210U)
#define SLCR_MIO_PIN_28     (SLCR_BASE + 0x770U)

#define SLCR_UNLOCK_KEY     0xDF0DU
#define SLCR_LOCK_KEY       0x767BU
#define REG_READ(addr)       (*(volatile uint32_t *)(uintptr_t)(addr))
#define REG_WRITE(addr, val) (*(volatile uint32_t *)(uintptr_t)(addr) = (uint32_t)(val))

static int g_usb_phy_initialized = 0;

static void delay_cycles(uint32_t cycles)
{
    volatile uint32_t i;
    for (i = 0; i < cycles; i++) {
        __asm__ volatile("nop");
    }
}

void usb_phy_early_init(void)
{
    uint32_t usb0_clk;
    uint32_t usb1_clk;
    uint32_t usb_rst;

    if (g_usb_phy_initialized != 0) {
        return;
    }

    REG_WRITE(SLCR_UNLOCK, SLCR_UNLOCK_KEY);

    usb0_clk = REG_READ(SLCR_USB0_CLK_CTRL);
    usb1_clk = REG_READ(SLCR_USB1_CLK_CTRL);
    usb0_clk |= 0x00000001U;
    usb1_clk |= 0x00000001U;
    REG_WRITE(SLCR_USB0_CLK_CTRL, usb0_clk);
    REG_WRITE(SLCR_USB1_CLK_CTRL, usb1_clk);

    delay_cycles(666666U);

    usb_rst = REG_READ(SLCR_USB_RST_CTRL);
    usb_rst &= ~0x00000003U;
    REG_WRITE(SLCR_USB_RST_CTRL, usb_rst);

    delay_cycles(6666U);

    REG_WRITE(SLCR_LOCK, SLCR_LOCK_KEY);

    delay_cycles(666666U);

    g_usb_phy_initialized = 1;
}

void usb_phy_get_status(usb_phy_status_t *status)
{
    uint32_t i;

    if (status == 0) {
        return;
    }

    status->usb0_clk_ctrl = REG_READ(SLCR_USB0_CLK_CTRL);
    status->usb1_clk_ctrl = REG_READ(SLCR_USB1_CLK_CTRL);
    status->usb_rst_ctrl = REG_READ(SLCR_USB_RST_CTRL);
    for (i = 0; i < 12U; ++i) {
        status->usb0_mio[i] = REG_READ(SLCR_MIO_PIN_28 + (i * 4U));
    }
}
