/* SPDX-License-Identifier: GPL-3.0-only */
/* USB3300 PHY/VBUS setup adapted from hasseily/multitini-one c2b87fa. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

#include "sleep.h"
#include "xil_cache.h"
#include "xil_io.h"
#include "xil_types.h"
#include "xparameters.h"
#include "xusbps_hw.h"

#include "usbh_core.h"

#include "cherryusb_platform.h"
#include "cherryusb_ulpi.h"
#include "usb_phy_init.h"
#include "../lib/uart.h"

#ifdef XPAR_XUSBPS_1_BASEADDR
#define CHERRYUSB_USB1_BASE XPAR_XUSBPS_1_BASEADDR
#else
#define CHERRYUSB_USB1_BASE 0xE0003000U
#endif

#define REG_READ(addr) Xil_In32((UINTPTR)(addr))
#define REG_WRITE(addr, value) Xil_Out32((UINTPTR)(addr), (uint32_t)(value))
#define CHERRYUSB_ULPI_TIMEOUT_US UINT32_C(5000)
#define CHERRYUSB_ULPI_ERR_WAKE_TIMEOUT (-1001)
#define CHERRYUSB_ULPI_ERR_TRANSACTION_TIMEOUT (-1002)
#define CHERRYUSB_ULPI_ERR_VERIFY (-1003)
#define CHERRYUSB_ULPI_ERR_CONTROLLER_RESET_TIMEOUT (-1004)
#define CHERRYUSB_ULPI_ERR_VBUS_VALID_TIMEOUT (-1005)
#define CHERRYUSB_ULPI_ERR_IDENTITY (-1006)
#define CHERRYUSB_ULPI_ERR_CONTROLLER_NOT_RUNNING (-1007)
#define CHERRYUSB_ULPI_ERR_VBUS_OFF_TIMEOUT (-1008)
#define CHERRYUSB_USB3300_VENDOR_ID UINT16_C(0x0424)
#define CHERRYUSB_USB3300_PRODUCT_ID UINT16_C(0x0004)

static uint8_t g_usb1_hc_running;
static uint8_t g_usb1_irq_polling;
static uint8_t g_usb1_hc_initialized;
static cherryusb_usb1_phy_status_t g_usb1_phy;

static int usbps_ulpi_wait_clear(uintptr_t base, uint32_t mask, int error)
{
    uint32_t elapsed;
    const uintptr_t viewport = base + XUSBPS_ULPIVIEW_OFFSET;

    for (elapsed = 0U; elapsed < CHERRYUSB_ULPI_TIMEOUT_US; ++elapsed) {
        g_usb1_phy.viewport = REG_READ(viewport);
        if ((g_usb1_phy.viewport & mask) == 0U) {
            return 0;
        }
        usleep(1U);
    }

    ++g_usb1_phy.timeouts;
    g_usb1_phy.last_error = error;
    return error;
}

static int usbps_ulpi_wake(uintptr_t base)
{
    ++g_usb1_phy.accesses;
    REG_WRITE(base + XUSBPS_ULPIVIEW_OFFSET,
              cherryusb_ulpi_wakeup_command());
    return usbps_ulpi_wait_clear(base, CHERRYUSB_ULPI_VIEW_WAKEUP,
                                 CHERRYUSB_ULPI_ERR_WAKE_TIMEOUT);
}

static int usbps_ulpi_read(uintptr_t base, uint8_t address, uint8_t *value)
{
    int result;

    result = usbps_ulpi_wake(base);
    if (result != 0) {
        return result;
    }

    REG_WRITE(base + XUSBPS_ULPIVIEW_OFFSET,
              cherryusb_ulpi_read_command(address));
    result = usbps_ulpi_wait_clear(base, CHERRYUSB_ULPI_VIEW_RUN,
                                   CHERRYUSB_ULPI_ERR_TRANSACTION_TIMEOUT);
    if (result != 0) {
        return result;
    }

    *value = (uint8_t)((g_usb1_phy.viewport &
                        CHERRYUSB_ULPI_VIEW_DATRD_MASK) >> 8);
    g_usb1_phy.last_error = 0;
    return 0;
}

static int usbps_ulpi_write(uintptr_t base, uint8_t address, uint8_t value)
{
    int result;

    result = usbps_ulpi_wake(base);
    if (result != 0) {
        return result;
    }

    REG_WRITE(base + XUSBPS_ULPIVIEW_OFFSET,
              cherryusb_ulpi_write_command(address, value));
    result = usbps_ulpi_wait_clear(base, CHERRYUSB_ULPI_VIEW_RUN,
                                   CHERRYUSB_ULPI_ERR_TRANSACTION_TIMEOUT);
    if (result == 0) {
        g_usb1_phy.last_error = 0;
    }
    return result;
}

static int usb3300_read_identity(uintptr_t base)
{
    uint8_t vendor_low;
    uint8_t vendor_high;
    uint8_t product_low;
    uint8_t product_high;
    int result;

    result = usbps_ulpi_read(base, CHERRYUSB_ULPI_REG_VENDOR_LOW,
                             &vendor_low);
    if (result != 0) return result;
    result = usbps_ulpi_read(base, CHERRYUSB_ULPI_REG_VENDOR_HIGH,
                             &vendor_high);
    if (result != 0) return result;
    result = usbps_ulpi_read(base, CHERRYUSB_ULPI_REG_PRODUCT_LOW,
                             &product_low);
    if (result != 0) return result;
    result = usbps_ulpi_read(base, CHERRYUSB_ULPI_REG_PRODUCT_HIGH,
                             &product_high);
    if (result != 0) return result;

    g_usb1_phy.vendor_id = (uint16_t)vendor_low |
                           ((uint16_t)vendor_high << 8);
    g_usb1_phy.product_id = (uint16_t)product_low |
                            ((uint16_t)product_high << 8);
    g_usb1_phy.identity_valid = 1U;
    return 0;
}

static int usb3300_host_prepare(uintptr_t base)
{
    int result;

    g_usb1_phy.identity_valid = 0U;
    g_usb1_phy.vbus_drive_enabled = 0U;
    g_usb1_phy.vbus_drive_known = 0U;
    g_usb1_phy.vbus_valid_known = 0U;

    result = usb3300_read_identity(base);
    if (result != 0) return result;
    if (g_usb1_phy.vendor_id != CHERRYUSB_USB3300_VENDOR_ID ||
        g_usb1_phy.product_id != CHERRYUSB_USB3300_PRODUCT_ID) {
        g_usb1_phy.last_error = CHERRYUSB_ULPI_ERR_IDENTITY;
        return CHERRYUSB_ULPI_ERR_IDENTITY;
    }

    /* Same USB3300/TPS25221 wiring as Multitini: keep EXTVBUS disabled.
     * Its open-drain FAULT# net has no pull-up; use the internal comparator.
     * Leave VBUS drive off until EHCI is running and ready to enumerate. */
    result = usbps_ulpi_write(base, CHERRYUSB_ULPI_REG_FUNCTION_CONTROL,
                              CHERRYUSB_ULPI_FUNCTION_HOST);
    if (result != 0) return result;
    result = usbps_ulpi_write(base, CHERRYUSB_ULPI_REG_INTERFACE_CONTROL,
                              CHERRYUSB_ULPI_INTERFACE_HOST_SET);
    if (result != 0) return result;
    result = usbps_ulpi_write(base,
                              CHERRYUSB_ULPI_REG_INTERRUPT_RISING_SET,
                              CHERRYUSB_ULPI_INTERRUPT_VBUS_VALID);
    if (result != 0) return result;
    result = usbps_ulpi_write(base,
                              CHERRYUSB_ULPI_REG_INTERRUPT_FALLING_SET,
                              CHERRYUSB_ULPI_INTERRUPT_VBUS_VALID);
    if (result != 0) return result;
    result = usbps_ulpi_write(base, CHERRYUSB_ULPI_REG_OTG_CONTROL,
                              CHERRYUSB_ULPI_OTG_HOST_BASE_SET);
    if (result != 0) return result;
    result = usbps_ulpi_read(base, CHERRYUSB_ULPI_REG_FUNCTION_CONTROL,
                             &g_usb1_phy.function_control);
    if (result != 0) return result;
    result = usbps_ulpi_read(base, CHERRYUSB_ULPI_REG_OTG_CONTROL,
                             &g_usb1_phy.otg_control);
    if (result != 0) return result;
    g_usb1_phy.vbus_drive_known = 1U;
    result = usbps_ulpi_read(base, CHERRYUSB_ULPI_REG_INTERFACE_CONTROL,
                             &g_usb1_phy.interface_control);
    if (result != 0) return result;

    g_usb1_phy.vbus_drive_enabled =
        cherryusb_usb3300_vbus_is_enabled(g_usb1_phy.otg_control);
    if (g_usb1_phy.vbus_drive_enabled != 0U ||
        g_usb1_phy.function_control != CHERRYUSB_ULPI_FUNCTION_HOST ||
        (g_usb1_phy.otg_control & CHERRYUSB_ULPI_OTG_HOST_BASE_SET) !=
            CHERRYUSB_ULPI_OTG_HOST_BASE_SET ||
        (g_usb1_phy.otg_control & CHERRYUSB_ULPI_OTG_HOST_CLEAR) != 0U ||
        (g_usb1_phy.interface_control &
         CHERRYUSB_ULPI_INTERFACE_HOST_SET) !=
            CHERRYUSB_ULPI_INTERFACE_HOST_SET ||
        (g_usb1_phy.interface_control &
         CHERRYUSB_ULPI_INTERFACE_HOST_CLEAR) != 0U) {
        g_usb1_phy.last_error = CHERRYUSB_ULPI_ERR_VERIFY;
        return CHERRYUSB_ULPI_ERR_VERIFY;
    }
    return 0;
}

static int usb3300_wait_vbus_state(uintptr_t base, uint8_t expected_valid,
                                   int timeout_error)
{
    uint32_t elapsed;
    int result;

    g_usb1_phy.vbus_valid_known = 0U;
    for (elapsed = 0U; elapsed < UINT32_C(100); ++elapsed) {
        result = usbps_ulpi_read(base,
                                 CHERRYUSB_ULPI_REG_INTERRUPT_STATUS,
                                 &g_usb1_phy.interrupt_status);
        if (result != 0) return result;
        g_usb1_phy.vbus_valid_known = 1U;
        g_usb1_phy.vbus_valid =
            ((g_usb1_phy.interrupt_status &
              CHERRYUSB_ULPI_INTERRUPT_VBUS_VALID) != 0U) ? 1U : 0U;
        if (g_usb1_phy.vbus_valid == expected_valid) {
            return 0;
        }
        cherryusb_background_poll();
        usleep(1000U);
    }

    g_usb1_phy.last_error = timeout_error;
    return timeout_error;
}

static int usb3300_host_power_disable(uintptr_t base)
{
    int result;

    g_usb1_phy.vbus_drive_known = 0U;
    result = usbps_ulpi_write(base, CHERRYUSB_ULPI_REG_OTG_CLEAR,
                              CHERRYUSB_ULPI_OTG_VBUS_DRIVE);
    if (result != 0) return result;
    result = usbps_ulpi_read(base, CHERRYUSB_ULPI_REG_OTG_CONTROL,
                             &g_usb1_phy.otg_control);
    if (result != 0) return result;
    g_usb1_phy.vbus_drive_known = 1U;

    g_usb1_phy.vbus_drive_enabled =
        cherryusb_usb3300_vbus_is_enabled(g_usb1_phy.otg_control);
    if (g_usb1_phy.vbus_drive_enabled != 0U) {
        g_usb1_phy.last_error = CHERRYUSB_ULPI_ERR_VERIFY;
        return CHERRYUSB_ULPI_ERR_VERIFY;
    }
    return usb3300_wait_vbus_state(base, 0U,
                                   CHERRYUSB_ULPI_ERR_VBUS_OFF_TIMEOUT);
}

static int usb3300_wait_vbus_valid(uintptr_t base)
{
    return usb3300_wait_vbus_state(base, 1U,
                                   CHERRYUSB_ULPI_ERR_VBUS_VALID_TIMEOUT);
}

static int usb3300_host_power_enable(uintptr_t base)
{
    int cleanup_result;
    int result;

    g_usb1_phy.vbus_drive_known = 0U;
    g_usb1_phy.vbus_valid = 0U;
    g_usb1_phy.vbus_valid_known = 0U;
    result = usbps_ulpi_write(base, CHERRYUSB_ULPI_REG_OTG_SET,
                              CHERRYUSB_ULPI_OTG_VBUS_DRIVE);
    if (result != 0) goto power_fail;
    g_usb1_phy.vbus_drive_enabled = 1U;

    result = usbps_ulpi_read(base, CHERRYUSB_ULPI_REG_OTG_CONTROL,
                             &g_usb1_phy.otg_control);
    if (result == 0) {
        g_usb1_phy.vbus_drive_known = 1U;
    }
    if (result == 0 &&
        (g_usb1_phy.otg_control & CHERRYUSB_ULPI_OTG_HOST_SET) ==
            CHERRYUSB_ULPI_OTG_HOST_SET &&
        (g_usb1_phy.otg_control & CHERRYUSB_ULPI_OTG_HOST_CLEAR) == 0U) {
        g_usb1_phy.vbus_drive_enabled = 1U;
        result = usb3300_wait_vbus_valid(base);
        if (result == 0) {
            g_usb1_phy.disable_result = 0;
            return 0;
        }
    }
    if (result == 0) {
        result = CHERRYUSB_ULPI_ERR_VERIFY;
    }

power_fail:
    cleanup_result = usb3300_host_power_disable(base);
    g_usb1_phy.disable_result = cleanup_result;
    if (cleanup_result != 0) {
        return cleanup_result;
    }
    g_usb1_phy.last_error = result;
    return result;
}

static int usbps_wait_controller_reset(uintptr_t base)
{
    uint32_t elapsed;

    for (elapsed = 0U; elapsed < UINT32_C(100000); elapsed += 10U) {
        if ((REG_READ(base + XUSBPS_CMD_OFFSET) & XUSBPS_CMD_RST_MASK) == 0U) {
            return 0;
        }
        if ((elapsed % 1000U) == 0U) {
            cherryusb_background_poll();
        }
        usleep(10U);
    }
    g_usb1_phy.last_error = CHERRYUSB_ULPI_ERR_CONTROLLER_RESET_TIMEOUT;
    return CHERRYUSB_ULPI_ERR_CONTROLLER_RESET_TIMEOUT;
}

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
    uint32_t portsc;

    g_usb1_hc_running = 0U;
    g_usb1_phy.power_result = 0;
    g_usb1_phy.disable_result = 0;
    g_usb1_phy.identity_valid = 0U;
    g_usb1_phy.vbus_drive_known = 0U;
    g_usb1_phy.vbus_valid_known = 0U;
    usb_phy_early_init();
    g_usb1_hc_initialized = 1U;

    /* A previous host stop can leave the ULPI clock suspended. Wake it before
     * resetting the controller; preserve pending write-one-to-clear changes. */
    portsc = usbps_portsc_clean(REG_READ(base + XUSBPS_PORTSCR1_OFFSET));
    if ((portsc & XUSBPS_PORTSCR_PHCD_MASK) != 0U) {
        REG_WRITE(base + XUSBPS_PORTSCR1_OFFSET,
                  portsc & ~XUSBPS_PORTSCR_PHCD_MASK);
        usleep(1000U);
    }
    REG_WRITE(base + XUSBPS_IER_OFFSET, 0U);
    REG_WRITE(base + XUSBPS_CMD_OFFSET, 0U);
    REG_WRITE(base + XUSBPS_CMD_OFFSET, XUSBPS_CMD_RST_MASK);
    g_usb1_phy.power_result = usbps_wait_controller_reset(base);
    if (g_usb1_phy.power_result != 0) {
        return;
    }
    usleep(1000U);

    usbps_set_host_mode(base);
    g_usb1_phy.power_result = usb3300_host_prepare(base);
}

void usb_hc_low_level2_init(struct usbh_bus *bus)
{
    if (g_usb1_phy.power_result == 0) {
        usbps_set_host_mode(bus->hcd.reg_base);
        g_usb1_phy.power_result = usb3300_host_prepare(bus->hcd.reg_base);
    }
    if (g_usb1_phy.power_result != 0) {
        cherryusb_printf("[usb1] ULPI host VBUS failed: %d viewport=%08x\r\n",
                   g_usb1_phy.power_result,
                   (unsigned int)g_usb1_phy.viewport);
    }
    g_usb1_hc_running = 0U;
}

void usb_hc_low_level_deinit(struct usbh_bus *bus)
{
    uint32_t cmd;
    uint32_t portsc;
    int reset_result;
    int power_result;

    g_usb1_hc_running = 0U;
    cmd = REG_READ(bus->hcd.reg_base + XUSBPS_CMD_OFFSET);
    cmd &= ~XUSBPS_CMD_RS_MASK;
    REG_WRITE(bus->hcd.reg_base + XUSBPS_CMD_OFFSET, cmd);

    /* Remove VBUS before any controller wait that can time out. */
    power_result = cherryusb_usb1_host_power_stop();
    reset_result = usbps_wait_controller_reset(bus->hcd.reg_base);
    if (reset_result != 0) {
        cherryusb_printf("[usb1] controller reset timeout; VBUS disable=%d\r\n",
                   power_result);
    }

    portsc = usbps_portsc_clean(REG_READ(bus->hcd.reg_base +
                                         XUSBPS_PORTSCR1_OFFSET));
    portsc &= ~XUSBPS_PORTSCR_PP_MASK;
    if (power_result == 0) {
        portsc |= XUSBPS_PORTSCR_PHCD_MASK;
    } else {
        portsc &= ~XUSBPS_PORTSCR_PHCD_MASK;
    }
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

/* Report only the last setup/shutdown result. UART status must not wake the
 * PHY or start ULPI transactions while the host is running. */
void cherryusb_usb1_phy_status(cherryusb_usb1_phy_status_t *status)
{
    if (status != NULL) {
        *status = g_usb1_phy;
    }
}

int cherryusb_usb1_power_result(void)
{
    return g_usb1_phy.power_result;
}

int cherryusb_usb1_host_power_start(void)
{
    uint32_t command;
    uint32_t status;

    if (g_usb1_hc_initialized == 0U) {
        g_usb1_phy.power_result = CHERRYUSB_ULPI_ERR_CONTROLLER_NOT_RUNNING;
        g_usb1_phy.last_error = CHERRYUSB_ULPI_ERR_CONTROLLER_NOT_RUNNING;
        return g_usb1_phy.power_result;
    }
    if (g_usb1_phy.power_result != 0) {
        return g_usb1_phy.power_result;
    }

    command = REG_READ(CHERRYUSB_USB1_BASE + XUSBPS_CMD_OFFSET);
    status = REG_READ(CHERRYUSB_USB1_BASE + XUSBPS_ISR_OFFSET);
    if ((command & (XUSBPS_CMD_RS_MASK | XUSBPS_CMD_RST_MASK)) !=
            XUSBPS_CMD_RS_MASK ||
        (status & XUSBPS_IXR_HCH_MASK) != 0U) {
        g_usb1_phy.power_result = CHERRYUSB_ULPI_ERR_CONTROLLER_NOT_RUNNING;
        g_usb1_phy.last_error = CHERRYUSB_ULPI_ERR_CONTROLLER_NOT_RUNNING;
        return g_usb1_phy.power_result;
    }

    g_usb1_phy.power_result =
        usb3300_host_power_enable(CHERRYUSB_USB1_BASE);
    if (g_usb1_phy.power_result == 0) {
        cherryusb_printf("[usb1] ULPI %04x:%04x host VBUS valid func=%02x iface=%02x otg=%02x\r\n",
                   (unsigned int)g_usb1_phy.vendor_id,
                   (unsigned int)g_usb1_phy.product_id,
                   (unsigned int)g_usb1_phy.function_control,
                   (unsigned int)g_usb1_phy.interface_control,
                   (unsigned int)g_usb1_phy.otg_control);
        g_usb1_hc_running = 1U;
    } else {
        cherryusb_printf("[usb1] ULPI host VBUS failed: %d viewport=%08x\r\n",
                   g_usb1_phy.power_result,
                   (unsigned int)g_usb1_phy.viewport);
        g_usb1_hc_running = 0U;
    }
    return g_usb1_phy.power_result;
}

int cherryusb_usb1_host_power_stop(void)
{
    int result;

    /* Stop polling first, then remove connector power independently of the
     * CherryUSB EHCI teardown path. usb_hc_deinit() can return before its
     * low-level hook when the controller fails to halt. */
    g_usb1_hc_running = 0U;
    if (g_usb1_hc_initialized == 0U) {
        return 0;
    }
    result = usb3300_host_power_disable(CHERRYUSB_USB1_BASE);
    g_usb1_phy.disable_result = result;
    if (result != 0) {
        cherryusb_printf("[usb1] ULPI VBUS disable failed: %d viewport=%08x\r\n",
                   result, (unsigned int)g_usb1_phy.viewport);
    }
    return result;
}

int cherryusb_usb1_power_stop_result(void)
{
    return g_usb1_phy.disable_result;
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
