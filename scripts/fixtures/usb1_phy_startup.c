/* Execute production USB1 glue with a register-level USB3300 model. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "usbh_core.h"
#include "xusbps_hw.h"
#include "cherryusb_platform.h"

#define CHECK(v) do { if (!(v)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #v); exit(1); \
} } while (0)
#define USB1_BASE UINT32_C(0xE0003000)
#define VIEW_WAKE UINT32_C(0x80000000)
#define VIEW_RUN UINT32_C(0x40000000)
#define VIEW_WRITE UINT32_C(0x20000000)
#define PHY_DRIVE 0x60U
#define PHY_VALID 0x02U

void usb_hc_low_level_init(struct usbh_bus *bus);
void usb_hc_low_level2_init(struct usbh_bus *bus);
void usb_hc_low_level_deinit(struct usbh_bus *bus);

static struct usbh_bus bus = { { USB1_BASE }, NULL, NULL };
struct usbh_bus g_usbhost_bus[1];
static uint32_t regs[0x200U / 4U];
static uint8_t phy[256];
static unsigned reads, writes, irq_calls, ulpi_commands, phy_init_calls;
static unsigned wake_timeout, transfer_timeout, reset_timeout;
static unsigned ignore_write_register, no_vbus, stuck_vbus;
static unsigned suspend_writes, drive_enable_writes, drive_disable_writes;
static unsigned background_polls;
static uint64_t elapsed_us;

static unsigned reg_index(uintptr_t addr)
{
    /* All controller transactions must stay on USB1, including error paths. */
    CHECK(addr >= USB1_BASE && addr < USB1_BASE + sizeof(regs));
    CHECK((addr & 3U) == 0U);
    return (unsigned)((addr - USB1_BASE) / 4U);
}

static void update_vbus(void)
{
    if (stuck_vbus != 0U || ((phy[0x0A] & PHY_DRIVE) != 0U && no_vbus == 0U)) {
        phy[0x13] |= PHY_VALID;
    } else {
        phy[0x13] &= (uint8_t)~PHY_VALID;
    }
}

uint32_t Xil_In32(uintptr_t addr)
{
    ++reads;
    return regs[reg_index(addr)];
}

void Xil_Out32(uintptr_t addr, uint32_t value)
{
    const unsigned idx = reg_index(addr);
    const uint32_t offset = (uint32_t)(addr - USB1_BASE);
    ++writes;
    if (offset == XUSBPS_ULPIVIEW_OFFSET) {
        ++ulpi_commands;
        regs[idx] = value;
        if ((value & VIEW_WAKE) != 0U) {
            if (wake_timeout == 0U) regs[idx] &= ~VIEW_WAKE;
        } else if ((value & VIEW_RUN) != 0U && transfer_timeout == 0U) {
            const uint8_t address = (uint8_t)(value >> 16);
            const uint8_t data = (uint8_t)value;
            if ((value & VIEW_WRITE) != 0U) {
                if (address != ignore_write_register) {
                    if (address == 0x0B) phy[0x0A] |= data;
                    else if (address == 0x0C) phy[0x0A] &= (uint8_t)~data;
                    else if (address == 0x08) phy[0x07] |= data;
                    else if (address == 0x09) phy[0x07] &= (uint8_t)~data;
                    else phy[address] = data;
                }
                if (address == 0x0B && (data & PHY_DRIVE) != 0U) {
                    /* Connector power may only follow a running EHCI host. */
                    CHECK((regs[XUSBPS_CMD_OFFSET / 4U] & 3U) == 1U);
                    CHECK((regs[XUSBPS_ISR_OFFSET / 4U] & XUSBPS_IXR_HCH_MASK) == 0U);
                    ++drive_enable_writes;
                }
                if ((address == 0x0C && (data & PHY_DRIVE) != 0U) ||
                    (address == 0x0A && (data & PHY_DRIVE) == 0U)) {
                    ++drive_disable_writes;
                }
                update_vbus();
            } else {
                regs[idx] = (value & ~UINT32_C(0x0000FF00)) |
                            ((uint32_t)phy[address] << 8);
            }
            regs[idx] &= ~VIEW_RUN;
        }
    } else if (offset == XUSBPS_ISR_OFFSET) {
        /* HCH is a read-only status bit; the other tested bits are W1C. */
        regs[idx] &= ~(value & ~XUSBPS_IXR_HCH_MASK);
    } else if (offset == XUSBPS_CMD_OFFSET) {
        regs[idx] = value;
        if ((value & XUSBPS_CMD_RST_MASK) != 0U) {
            CHECK((regs[XUSBPS_PORTSCR1_OFFSET / 4U] & XUSBPS_PORTSCR_PHCD_MASK) == 0U);
            CHECK(regs[XUSBPS_IER_OFFSET / 4U] == 0U);
        }
        if ((value & XUSBPS_CMD_RST_MASK) != 0U && reset_timeout == 0U) {
            regs[idx] = 0U;
            regs[XUSBPS_MODE_OFFSET / 4U] = 0U;
            regs[XUSBPS_PORTSCR1_OFFSET / 4U] = 0U;
        }
        if ((regs[idx] & XUSBPS_CMD_RS_MASK) == 0U) {
            regs[XUSBPS_ISR_OFFSET / 4U] |= XUSBPS_IXR_HCH_MASK;
        } else {
            regs[XUSBPS_ISR_OFFSET / 4U] &= ~XUSBPS_IXR_HCH_MASK;
        }
    } else if (offset == XUSBPS_PORTSCR1_OFFSET) {
        const uint32_t changes = XUSBPS_PORTSCR_CSC_MASK |
                                 XUSBPS_PORTSCR_PEC_MASK |
                                 XUSBPS_PORTSCR_OCC_MASK;
        if ((value & XUSBPS_PORTSCR_PHCD_MASK) != 0U) {
            /* Keep the PHY clock live if verified power removal failed. */
            CHECK((phy[0x0A] & PHY_DRIVE) == 0U);
            CHECK((phy[0x13] & PHY_VALID) == 0U);
            ++suspend_writes;
        }
        regs[idx] = (value & ~changes) | ((regs[idx] & changes) & ~value);
    } else {
        regs[idx] = value;
    }
}

void XUsbPs_ResetHw(uintptr_t base)
{
    CHECK(base == USB1_BASE);
    Xil_Out32(base + XUSBPS_CMD_OFFSET, XUSBPS_CMD_RST_MASK);
}

void usleep(unsigned int delay)
{
    elapsed_us += delay;
    CHECK(elapsed_us < UINT64_C(5000000));
}

void usb_phy_early_init(void) { ++phy_init_calls; }
void cherryusb_background_poll(void) { ++background_polls; }
void uart_puts(uint32_t base, const char *text) { (void)base; (void)text; }
void Xil_DCacheInvalidateRange(intptr_t addr, uint32_t size) { (void)addr; (void)size; }
void Xil_DCacheFlushRange(intptr_t addr, uint32_t size) { (void)addr; (void)size; }
void USBH_IRQHandler(uint8_t busid)
{
    CHECK(busid == 0U);
    ++irq_calls;
    /* A nested background poll must not recurse into the IRQ handler. */
    cherryusb_baremetal_poll_irq();
}

static void reset_model(unsigned warm)
{
    memset(regs, 0, sizeof(regs));
    memset(phy, warm != 0U ? 0xFF : 0, sizeof(phy));
    phy[0] = 0x24; phy[1] = 0x04; phy[2] = 0x04; phy[3] = 0;
    regs[XUSBPS_ISR_OFFSET / 4U] = XUSBPS_IXR_HCH_MASK;
    if (warm != 0U) {
        regs[XUSBPS_PORTSCR1_OFFSET / 4U] = XUSBPS_PORTSCR_PHCD_MASK;
        regs[XUSBPS_IER_OFFSET / 4U] = UINT32_MAX;
    }
    reads = writes = irq_calls = ulpi_commands = phy_init_calls = 0U;
    wake_timeout = transfer_timeout = reset_timeout = 0U;
    ignore_write_register = 256U;
    no_vbus = stuck_vbus = suspend_writes = 0U;
    drive_enable_writes = drive_disable_writes = 0U;
    background_polls = 0U;
    elapsed_us = 0U;
    update_vbus();
}

static void run_controller(void)
{
    Xil_Out32(USB1_BASE + XUSBPS_CMD_OFFSET, XUSBPS_CMD_RS_MASK);
}

static void prepare(void)
{
    usb_hc_low_level_init(&bus);
    CHECK(phy_init_calls == 1U);
    cherryusb_baremetal_poll_irq();
    CHECK(irq_calls == 0U);
    /* CherryUSB performs its own second EHCI reset before low_level2_init. */
    XUsbPs_ResetHw(USB1_BASE);
    usb_hc_low_level2_init(&bus);
    cherryusb_baremetal_poll_irq();
    CHECK(irq_calls == 0U);
}

static void check_cached_status(void)
{
    cherryusb_usb1_phy_status_t status;
    unsigned before_reads = reads, before_writes = writes;
    cherryusb_usb1_phy_status(&status);
    CHECK(reads == before_reads && writes == before_writes);
    CHECK(status.vendor_id == 0x0424 && status.product_id == 0x0004);
}

static void test_success(unsigned warm)
{
    reset_model(warm);
    prepare();
    CHECK(cherryusb_usb1_power_result() == 0);
    CHECK(phy[0x04] == 0x41 && phy[0x07] == 0x10 && phy[0x0A] == 0x06);
    CHECK(drive_enable_writes == 0U);
    run_controller();
    CHECK(cherryusb_usb1_host_power_start() == 0);
    CHECK(phy[0x0A] == 0x66 && (phy[0x13] & PHY_VALID) != 0U);
    CHECK(drive_enable_writes == 1U);
    cherryusb_baremetal_poll_irq();
    CHECK(irq_calls == 1U);
    check_cached_status();
    CHECK(cherryusb_usb1_host_power_stop() == 0);
    cherryusb_baremetal_poll_irq();
    CHECK(irq_calls == 1U);
    CHECK((phy[0x0A] & PHY_DRIVE) == 0U && (phy[0x13] & PHY_VALID) == 0U);
    usb_hc_low_level_deinit(&bus);
    CHECK(suspend_writes == 1U);
    printf("PASS USB1 %s startup, verified VBUS, cached status, IRQ guard, shutdown\n", warm ? "warm" : "cold");
}

static void test_not_running(void)
{
    reset_model(0U);
    prepare();
    CHECK(cherryusb_usb1_host_power_start() == -1007);
    CHECK(drive_enable_writes == 0U);
    cherryusb_baremetal_poll_irq();
    CHECK(irq_calls == 0U);
    puts("PASS USB1 refuses connector power before EHCI runs");
}

static void test_prepare_failure(unsigned kind, int error)
{
    reset_model(0U);
    if (kind == 0U) wake_timeout = 1U;
    if (kind == 1U) transfer_timeout = 1U;
    if (kind == 2U) phy[0] = 0;
    if (kind == 3U) ignore_write_register = 0x07;
    usb_hc_low_level_init(&bus);
    usb_hc_low_level2_init(&bus);
    run_controller();
    CHECK(cherryusb_usb1_host_power_start() == error);
    CHECK(drive_enable_writes == 0U);
    cherryusb_baremetal_poll_irq();
    CHECK(irq_calls == 0U);
    CHECK(elapsed_us < 200000U);
    printf("PASS USB1 bounded preparation failure %d propagates\n", error);
}

static void test_vbus_failure(void)
{
    reset_model(0U);
    prepare();
    run_controller();
    no_vbus = 1U;
    CHECK(cherryusb_usb1_host_power_start() == -1005);
    CHECK((phy[0x0A] & PHY_DRIVE) == 0U);
    CHECK(drive_disable_writes > 0U && elapsed_us < 200000U);
    CHECK(background_polls > 0U);
    cherryusb_baremetal_poll_irq();
    CHECK(irq_calls == 0U);
    puts("PASS USB1 VBUS timeout disables drive and blocks IRQ polling");
}

static void test_shutdown_failure(void)
{
    reset_model(0U);
    prepare();
    run_controller();
    CHECK(cherryusb_usb1_host_power_start() == 0);
    stuck_vbus = 1U;
    CHECK(cherryusb_usb1_host_power_stop() == -1008);
    CHECK((phy[0x0A] & PHY_DRIVE) == 0U);
    usb_hc_low_level_deinit(&bus);
    CHECK(suspend_writes == 0U && elapsed_us < 300000U);
    CHECK(cherryusb_usb1_power_stop_result() == -1008);
    puts("PASS USB1 failed power-off verification keeps PHY clock available");
}

static void test_reset_timeout(void)
{
    reset_model(0U);
    reset_timeout = 1U;
    usb_hc_low_level_init(&bus);
    usb_hc_low_level2_init(&bus);
    CHECK(cherryusb_usb1_power_result() == -1004);
    CHECK(cherryusb_usb1_host_power_start() == -1004);
    CHECK(ulpi_commands == 0U && elapsed_us >= 100000U && elapsed_us < 110000U);
    CHECK(background_polls > 0U);
    cherryusb_baremetal_poll_irq();
    CHECK(irq_calls == 0U);
    puts("PASS USB1 controller reset timeout is bounded and cannot enable VBUS");
}

static void test_drive_readback_failure(void)
{
    reset_model(0U);
    prepare();
    run_controller();
    ignore_write_register = 0x0B;
    CHECK(cherryusb_usb1_host_power_start() == -1003);
    CHECK((phy[0x0A] & PHY_DRIVE) == 0U);
    cherryusb_baremetal_poll_irq();
    CHECK(irq_calls == 0U);
    puts("PASS USB1 VBUS drive readback mismatch fails startup");
}

/* These two public lifecycle functions come verbatim from usb_hid_service.c.
 * Stub unrelated HID/mouse state, but keep the production PHY glue below them. */
static unsigned g_started, g_power_shutdown_required, g_ready;
static unsigned g_report_count, g_submit_error_count, g_transfer_error_count;
static int g_last_error, g_x, g_y;
static unsigned init_calls, deinit_calls;
static int init_result;
static unsigned missing_hub_handles;
#define UART0_BASE 0xE0000000U
static void hid_slots_reset_all(void) {}
static void mouse_publish_state(unsigned ready, int x, int y, unsigned buttons)
{ (void)ready; (void)x; (void)y; (void)buttons; }
static void mouse_mark_disconnected(void) { g_ready = 0U; }
static void onee_input_service_release_all(void) {}
static void cherry_event_handler(uint8_t b, uint8_t h, uint8_t p, uint8_t i, uint8_t e)
{ (void)b; (void)h; (void)p; (void)i; (void)e; }
static int usbh_initialize(uint8_t busid, uintptr_t base,
    void (*handler)(uint8_t, uint8_t, uint8_t, uint8_t, uint8_t))
{
    CHECK(busid == 0U && base == USB1_BASE && handler == cherry_event_handler);
    ++init_calls;
    if (init_result != 0) return init_result;
    g_usbhost_bus[0].hub_mq = missing_hub_handles == 1U ? NULL : &bus;
    g_usbhost_bus[0].hub_sem = missing_hub_handles == 2U ? NULL : &bus;
    usb_hc_low_level_init(&bus);
    XUsbPs_ResetHw(USB1_BASE);
    usb_hc_low_level2_init(&bus);
    run_controller();
    return 0;
}
static int usbh_deinitialize(uint8_t busid)
{
    CHECK(busid == 0U);
    /* Power removal must precede teardown, which can fail before its hook. */
    CHECK((phy[0x0A] & PHY_DRIVE) == 0U);
    ++deinit_calls;
    usb_hc_low_level_deinit(&bus);
    g_usbhost_bus[0].hub_mq = g_usbhost_bus[0].hub_sem = NULL;
    return 0;
}
void usb_hid_service_stop(void);
#include "usb1_hid_lifecycle.inc"

static void reset_hid_model(void)
{
    reset_model(0U);
    g_started = g_power_shutdown_required = g_ready = 0U;
    init_calls = deinit_calls = 0U;
    init_result = g_last_error = 0;
    missing_hub_handles = 0U;
    memset(g_usbhost_bus, 0, sizeof(g_usbhost_bus));
}

static void test_public_lifecycle(void)
{
    reset_hid_model();
    CHECK(usb_hid_service_start() == 0);
    CHECK(g_started == 1U && init_calls == 1U);
    CHECK(usb_hid_service_start() == 0 && init_calls == 1U);
    usb_hid_service_stop();
    CHECK(g_started == 0U && g_power_shutdown_required == 0U && deinit_calls == 1U);
    puts("PASS USB1 public HID start/stop sequences controller and connector power");
}

static void test_public_failures(void)
{
    reset_hid_model();
    no_vbus = 1U;
    CHECK(usb_hid_service_start() == -1005 && g_last_error == -1005);
    CHECK(g_started == 0U && g_power_shutdown_required == 0U && deinit_calls == 1U);
    reset_hid_model();
    init_result = -42;
    CHECK(usb_hid_service_start() == -42 && g_last_error == -42);
    CHECK(g_started == 0U && g_power_shutdown_required == 0U && deinit_calls == 0U);
    CHECK((phy[0x0A] & PHY_DRIVE) == 0U);
    puts("PASS USB1 public HID startup propagates controller/PHY errors and cleans up");
}

static void test_public_shutdown_retry(void)
{
    reset_hid_model();
    CHECK(usb_hid_service_start() == 0);
    stuck_vbus = 1U;
    usb_hid_service_stop();
    CHECK(g_started == 0U && g_power_shutdown_required == 1U);
    CHECK(usb_hid_service_start() == -1008 && init_calls == 1U);
    stuck_vbus = 0U;
    update_vbus();
    CHECK(usb_hid_service_start() == 0 && init_calls == 2U);
    usb_hid_service_stop();
    CHECK(g_started == 0U && g_power_shutdown_required == 0U);
    puts("PASS USB1 public HID retries failed power-off before a new start");
}

static void test_missing_hub_handles(void)
{
    for (unsigned missing = 1U; missing <= 2U; ++missing) {
        reset_hid_model();
        missing_hub_handles = missing;
        CHECK(usb_hid_service_start() == -USB_ERR_IO);
        CHECK(g_started == 0U && g_power_shutdown_required == 0U);
        CHECK(drive_enable_writes == 0U && deinit_calls == 1U);
        CHECK(g_last_error == -USB_ERR_IO);
    }
    puts("PASS USB1 detects swallowed CherryUSB hub allocation failures");
}

int main(void)
{
    CHECK(cherryusb_usb1_host_power_stop() == 0);
    CHECK(cherryusb_usb1_host_power_start() == -1007);
    CHECK(reads == 0U && writes == 0U);
    puts("PASS USB1 lifecycle before initialization performs no controller access");
    test_success(0U);
    test_success(1U);
    test_not_running();
    test_prepare_failure(0U, -1001);
    test_prepare_failure(1U, -1002);
    test_prepare_failure(2U, -1006);
    test_prepare_failure(3U, -1003);
    test_vbus_failure();
    test_shutdown_failure();
    test_reset_timeout();
    test_drive_readback_failure();
    test_public_lifecycle();
    test_public_failures();
    test_public_shutdown_retry();
    test_missing_hub_handles();
    puts("16 USB1 PHY/HID native behavior groups passed; every MMIO access stayed on USB1");
    return 0;
}
