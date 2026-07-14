#include <stdint.h>
#include <string.h>

#include "../image_versions.h"
#include "../lib/uart.h"

#include "updater.h"
#include "updater_layout.h"
#include "serial_boot.h"
#include "boot_control.h"
#include "golden_led.h"

#define GOLDEN_UART_BAUD  921600U
#define GOLDEN_SERIAL_BOOT_WINDOW_SECONDS  1U

static void print_hex32(uint32_t v)
{
    uart_puthex(UART0_BASE, v);
    uart_puthex(UART1_BASE, v);
}

static void print_result(const updater_result_t *r)
{
    uart_puts(UART0_BASE, "[UPD] result: ");
    uart_puts(UART1_BASE, "[UPD] result: ");

    if (r->error_code == 0) {
        uart_puts(UART0_BASE, "OK");
        uart_puts(UART1_BASE, "OK");
    } else {
        uart_puts(UART0_BASE, "ERR");
        uart_puts(UART1_BASE, "ERR");
    }
    uart_puts(UART0_BASE, " code=");
    uart_puts(UART1_BASE, " code=");
    uart_putdec(UART0_BASE, (uint32_t)r->error_code);
    uart_putdec(UART1_BASE, (uint32_t)r->error_code);
    uart_puts(UART0_BASE, " msg=");
    uart_puts(UART1_BASE, " msg=");
    uart_puts(UART0_BASE, r->error_msg);
    uart_puts(UART1_BASE, r->error_msg);
    uart_puts(UART0_BASE, "\r\n");
    uart_puts(UART1_BASE, "\r\n");
}

static void boot_firmware_slot(const flash_layout_t *layout)
{
    uint32_t boot_offset = updater_layout_firmware_offset(layout);

    uart_puts(UART0_BASE, "[GOLDEN] Booting firmware @0x");
    uart_puts(UART1_BASE, "[GOLDEN] Booting firmware @0x");
    print_hex32(boot_offset);
    uart_puts(UART0_BASE, "\r\n");
    uart_puts(UART1_BASE, "\r\n");
    /* Always hand off to firmware with the LED off, regardless of which
     * path reached here (normal boot, serial "boot", post-update). */
    golden_led_off();
    boot_control_boot_qspi_image_offset(boot_offset);
}

int main(void)
{
    updater_result_t res;
    const flash_layout_t *layout = updater_layout_active();
    serial_boot_action_t action;
    uint8_t metadata_sync_attempted = 0U;
    int run_rc;

    uart_init_both(GOLDEN_UART_BAUD);
    golden_led_init();   /* MIO0 status LED, default off */
    uart_puts(UART0_BASE, "\r\n[GOLDEN] boot_updater start @921600\r\n");
    uart_puts(UART1_BASE, "\r\n[GOLDEN] boot_updater start @921600\r\n");
    uart_puts(UART0_BASE, "[GOLDEN] Boot image version: " APPLETINI_BOOT_IMAGE_VERSION_FULL "\r\n");
    uart_puts(UART1_BASE, "[GOLDEN] Boot image version: " APPLETINI_BOOT_IMAGE_VERSION_FULL "\r\n");

    action = serial_boot_menu(layout, GOLDEN_SERIAL_BOOT_WINDOW_SECONDS);

    for (;;) {
        if (action == SERIAL_BOOT_ACTION_REBOOT_GOLDEN) {
            boot_control_soft_reset();
        }

        if (action == SERIAL_BOOT_ACTION_BOOT_FIRMWARE) {
            boot_firmware_slot(layout);
        }

        if (metadata_sync_attempted == 0U) {
            metadata_sync_attempted = 1U;
            if (updater_sync_boot_metadata() != 0) {
                uart_puts(UART0_BASE, "[GOLDEN] Metadata sync failed (continuing)\r\n");
                uart_puts(UART1_BASE, "[GOLDEN] Metadata sync failed (continuing)\r\n");
            }
        }

        run_rc = updater_run(&res);
        if (run_rc == 0 && res.updated && res.verified) {
            uart_puts(UART0_BASE, "[GOLDEN] Update OK CRC file=0x");
            uart_puts(UART1_BASE, "[GOLDEN] Update OK CRC file=0x");
            print_hex32(res.file_crc32);
            uart_puts(UART0_BASE, " flash=0x");
            uart_puts(UART1_BASE, " flash=0x");
            print_hex32(res.flash_crc32);
            uart_puts(UART0_BASE, "\r\n[GOLDEN] Rebooting...\r\n");
            uart_puts(UART1_BASE, "\r\n[GOLDEN] Rebooting...\r\n");
            golden_led_off();   /* update done */
            boot_control_soft_reset();
        }

        if (run_rc != 0 || res.error_code != 0) {
            print_result(&res);
            if (res.found_file) {
                uart_puts(UART0_BASE, "[GOLDEN] Update failed after detecting FIRMWARE.BIN; refusing to boot firmware automatically.\r\n");
                uart_puts(UART1_BASE, "[GOLDEN] Update failed after detecting FIRMWARE.BIN; refusing to boot firmware automatically.\r\n");
                /* A real update was attempted and failed: blink the error
                 * code on the LED before dropping into the serial monitor.
                 * Scoped to found_file so a missing SD / no-update-file
                 * normal boot never blinks (the LED was never turned on). */
                golden_led_error_code(res.error_code);
                action = serial_boot_menu(layout, 0U);
                continue;
            }
        } else {
            uart_puts(UART0_BASE, "[GOLDEN] No update file\r\n");
            uart_puts(UART1_BASE, "[GOLDEN] No update file\r\n");
        }

        if (!updater_has_verified_firmware()) {
            uart_puts(UART0_BASE, "[GOLDEN] No verified firmware record; staying in golden monitor.\r\n");
            uart_puts(UART1_BASE, "[GOLDEN] No verified firmware record; staying in golden monitor.\r\n");
            action = serial_boot_menu(layout, 0U);
            continue;
        }

        boot_firmware_slot(layout);
    }

    for (;;) {
        /* Should not return */
    }
}
