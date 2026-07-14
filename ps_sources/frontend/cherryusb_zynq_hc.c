#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

#include "sleep.h"
#include "xil_cache.h"
#include "xil_types.h"
#include "xparameters.h"
#include "xusbps_hw.h"

#include "usbh_core.h"

#include "cherryusb_platform.h"
#include "usb_phy_init.h"
#include "../lib/uart.h"

#ifdef XPAR_XUSBPS_1_BASEADDR
#define CHERRYUSB_USB1_BASE XPAR_XUSBPS_1_BASEADDR
#else
#define CHERRYUSB_USB1_BASE 0xE0003000U
#endif

#define REG_READ(addr) (*(volatile uint32_t *)(uintptr_t)(addr))
#define REG_WRITE(addr, value) (*(volatile uint32_t *)(uintptr_t)(addr) = (uint32_t)(value))

static uint8_t g_usb1_hc_running;
static uint8_t g_usb1_irq_polling;

static uint32_t usbps_portsc_clean(uint32_t portsc)
{
    return portsc & ~(XUSBPS_PORTSCR_CSC_MASK |
                      XUSBPS_PORTSCR_PEC_MASK |
                      XUSBPS_PORTSCR_OCC_MASK);
}

static void usbps_set_host_mode(uintptr_t base)
{
    uint32_t mode;
    uint32_t portsc;

    mode = REG_READ(base + XUSBPS_MODE_OFFSET);
    mode &= ~XUSBPS_MODE_CM_MASK;
    mode |= XUSBPS_MODE_CM_HOST_MASK;
    REG_WRITE(base + XUSBPS_MODE_OFFSET, mode);

    REG_WRITE(base + XUSBPS_ISR_OFFSET, XUSBPS_IXR_ALL);

    portsc = usbps_portsc_clean(REG_READ(base + XUSBPS_PORTSCR1_OFFSET));
    portsc &= ~XUSBPS_PORTSCR_PHCD_MASK;
    portsc |= XUSBPS_PORTSCR_PP_MASK;
    REG_WRITE(base + XUSBPS_PORTSCR1_OFFSET, portsc);
}

void usb_hc_low_level_init(struct usbh_bus *bus)
{
    uintptr_t base = bus->hcd.reg_base;

    g_usb1_hc_running = 0U;
    usb_phy_early_init();

    REG_WRITE(base + XUSBPS_CMD_OFFSET, 0U);
    XUsbPs_ResetHw(base);
    usleep(1000U);

    usbps_set_host_mode(base);
}

void usb_hc_low_level2_init(struct usbh_bus *bus)
{
    usbps_set_host_mode(bus->hcd.reg_base);
    g_usb1_hc_running = 1U;
}

void usb_hc_low_level_deinit(struct usbh_bus *bus)
{
    uint32_t cmd;
    uint32_t portsc;

    g_usb1_hc_running = 0U;
    cmd = REG_READ(bus->hcd.reg_base + XUSBPS_CMD_OFFSET);
    cmd &= ~XUSBPS_CMD_RS_MASK;
    REG_WRITE(bus->hcd.reg_base + XUSBPS_CMD_OFFSET, cmd);

    portsc = usbps_portsc_clean(REG_READ(bus->hcd.reg_base +
                                         XUSBPS_PORTSCR1_OFFSET));
    portsc &= ~XUSBPS_PORTSCR_PP_MASK;
    portsc |= XUSBPS_PORTSCR_PHCD_MASK;
    REG_WRITE(bus->hcd.reg_base + XUSBPS_PORTSCR1_OFFSET, portsc);
}

void cherryusb_baremetal_poll_irq(void)
{
    if (g_usb1_hc_running == 0U || g_usb1_irq_polling != 0U) {
        return;
    }

    g_usb1_irq_polling = 1U;
    USBH_IRQHandler(CHERRYUSB_USB1_BUSID);
    g_usb1_irq_polling = 0U;
}

uint32_t cherryusb_usb1_portsc(void)
{
    return REG_READ(CHERRYUSB_USB1_BASE + XUSBPS_PORTSCR1_OFFSET);
}

uint8_t usbh_get_port_speed(struct usbh_bus *bus, const uint8_t port)
{
    uint32_t pspd;
    uint32_t portsc;

    if (bus == NULL || port == 0U) {
        return USB_SPEED_UNKNOWN;
    }

    portsc = REG_READ(bus->hcd.reg_base + XUSBPS_PORTSCR1_OFFSET);
    pspd = (portsc & XUSBPS_PORTSCR_PSPD_MASK) >> 26;
    if (pspd == 1U) {
        return USB_SPEED_LOW;
    }
    if (pspd == 2U) {
        return USB_SPEED_HIGH;
    }
    return USB_SPEED_FULL;
}

int cherryusb_printf(const char *fmt, ...)
{
    char buf[192];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (n > 0) {
        buf[sizeof(buf) - 1U] = '\0';
        uart_puts(UART0_BASE, buf);
    }

    return n;
}

#ifdef CONFIG_USB_DCACHE_ENABLE
static void dcache_range(uintptr_t addr, size_t size, uint8_t invalidate)
{
    uintptr_t start;
    uintptr_t end;

    if (size == 0U) {
        return;
    }

    start = addr & ~(uintptr_t)31U;
    end = (addr + size + 31U) & ~(uintptr_t)31U;

    if (invalidate != 0U) {
        Xil_DCacheInvalidateRange((INTPTR)start, (uint32_t)(end - start));
    } else {
        Xil_DCacheFlushRange((INTPTR)start, (uint32_t)(end - start));
    }
}

void usb_dcache_clean(uintptr_t addr, size_t size)
{
    dcache_range(addr, size, 0U);
}

void usb_dcache_invalidate(uintptr_t addr, size_t size)
{
    dcache_range(addr, size, 1U);
}

void usb_dcache_flush(uintptr_t addr, size_t size)
{
    dcache_range(addr, size, 0U);
}
#endif
