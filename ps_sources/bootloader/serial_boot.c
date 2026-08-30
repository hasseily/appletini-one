#include "serial_boot.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "ff.h"
#include "xiltimer.h"
#include "xstatus.h"

#include "../image_versions.h"
#include "../lib/crc32.h"
#include "../lib/golden_self_update.h"
#include "../lib/number_parse.h"
#include "../lib/qspi_nor.h"
#include "../lib/uart.h"

#define SERIAL_BOOT_CMD_MAX           96U
#define SERIAL_BOOT_ARG_MAX           5
#define SERIAL_BOOT_SD_ROOT           "0:/"
#define SERIAL_BOOT_FW_PATH           "0:/FIRMWARE.BIN"
#define SERIAL_BOOT_RX_TMP_PATH       "0:/FIRMWARE.RX"
#define SERIAL_BOOT_GOLDEN_PATH       "0:/BOOT.BIN"
#define SERIAL_BOOT_XMODEM_MAX_RETRY  20U
#define SERIAL_BOOT_XMODEM_START_TRY  60U
#define SERIAL_BOOT_XMODEM_BYTE_MS    1000U

#define XMODEM_SOH                    0x01U
#define XMODEM_STX                    0x02U
#define XMODEM_EOT                    0x04U
#define XMODEM_ACK                    0x06U
#define XMODEM_NAK                    0x15U
#define XMODEM_CAN                    0x18U
#define XMODEM_CRC_REQ                'C'

static FATFS g_serial_fs;
static uint8_t g_xmodem_buf[1024] __attribute__((aligned(64)));
/* The linker places .bss in DDR. Keep the full golden image out of SD so a
 * selfrx recovery also works when no card is mounted. */
static uint8_t g_golden_rx_buf[GOLDEN_SELF_UPDATE_SLOT_SIZE] __attribute__((aligned(64)));
static uint32_t g_staged_boot_offset = GOLDEN_SELF_UPDATE_TRIAL_OFFSET;

typedef enum {
    SERIAL_RX_TARGET_SD_FILE = 0,
    SERIAL_RX_TARGET_MEMORY
} serial_rx_target_kind_t;

typedef struct {
    serial_rx_target_kind_t kind;
    const char *command_name;
    const char *limit_name;
    uint32_t max_size;
    uint8_t *memory;
    const char *tmp_path;
    const char *final_path;
} serial_rx_target_t;

static uint32_t other_uart(uint32_t base)
{
    return (base == UART0_BASE) ? UART1_BASE : UART0_BASE;
}

static void serial_puts_both(const char *s)
{
    uart_puts(UART0_BASE, s);
    uart_puts(UART1_BASE, s);
}

static void serial_logf(const char *fmt, ...)
{
    char line[220];
    va_list ap;

    va_start(ap, fmt);
    (void)vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    serial_puts_both(line);
}

static uint8_t ascii_tolower(uint8_t c)
{
    if (c >= 'A' && c <= 'Z') {
        return (uint8_t)(c + ('a' - 'A'));
    }
    return c;
}

static int str_ieq(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        if (ascii_tolower((uint8_t)*a) != ascii_tolower((uint8_t)*b)) {
            return 0;
        }
        ++a;
        ++b;
    }
    return (*a == '\0' && *b == '\0') ? 1 : 0;
}

static int split_args(char *line, char **argv, int max_args)
{
    int argc = 0;
    char *p = line;

    while (*p != '\0' && argc < max_args) {
        while (*p == ' ' || *p == '\t') {
            ++p;
        }
        if (*p == '\0') {
            break;
        }
        argv[argc++] = p;
        while (*p != '\0' && *p != ' ' && *p != '\t') {
            ++p;
        }
        if (*p != '\0') {
            *p++ = '\0';
        }
    }
    return argc;
}

static XTime ticks_from_ms(uint32_t ms)
{
    return (XTime)(((uint64_t)COUNTS_PER_SECOND * (uint64_t)ms) / 1000ULL);
}

static int uart_getc_timeout(uint32_t base, uint8_t *out, uint32_t timeout_ms)
{
    XTime start = 0U;
    XTime now = 0U;
    const XTime timeout_ticks = ticks_from_ms(timeout_ms);

    XTime_GetTime(&start);
    do {
        char c;
        if (uart_getc_nonblock(base, &c)) {
            *out = (uint8_t)c;
            return 1;
        }
        XTime_GetTime(&now);
    } while ((now - start) < timeout_ticks);

    return 0;
}

static int poll_any_uart(uint32_t *base_out, char *c_out)
{
    char c;

    if (uart_getc_nonblock(UART0_BASE, &c)) {
        *base_out = UART0_BASE;
        *c_out = c;
        return 1;
    }
    if (uart_getc_nonblock(UART1_BASE, &c)) {
        *base_out = UART1_BASE;
        *c_out = c;
        return 1;
    }
    return 0;
}

static uint16_t crc16_ccitt(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0U;
    uint32_t i;

    for (i = 0U; i < len; ++i) {
        uint8_t bit;
        crc ^= (uint16_t)data[i] << 8;
        for (bit = 0U; bit < 8U; ++bit) {
            if ((crc & 0x8000U) != 0U) {
                crc = (uint16_t)((crc << 1) ^ 0x1021U);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}

static FRESULT serial_sd_mount(void)
{
    return f_mount(&g_serial_fs, SERIAL_BOOT_SD_ROOT, 1U);
}

static void print_help(void)
{
    serial_puts_both("\r\n[SER] Golden boot serial monitor\r\n");
    serial_puts_both("[SER] Prefix commands with ':' or type a command directly.\r\n");
    serial_puts_both("[SER] Commands:\r\n");
    serial_puts_both("[SER]   help | ?\r\n");
    serial_puts_both("[SER]   status\r\n");
    serial_puts_both("[SER]   continue      - leave monitor and follow normal SD update/boot path\r\n");
    serial_puts_both("[SER]   update        - run SD FIRMWARE.BIN updater now\r\n");
    serial_puts_both("[SER]   boot          - boot firmware slot without SD update\r\n");
    serial_puts_both("[SER]   reboot        - reboot back through golden image\r\n");
    serial_puts_both("[SER]   rx [size] [crc32] - receive FIRMWARE.BIN by XMODEM-CRC to SD, then update\r\n");
    serial_puts_both("[SER]   selfupdate    - stage SD BOOT.BIN in the golden trial slot\r\n");
    serial_puts_both("[SER]   selfrx <size> <crc32> - receive BOOT.BIN by XMODEM-CRC, then stage it\r\n");
    serial_puts_both("[SER] Size and CRC accept decimal or 0xHEX. Size is recommended to trim XMODEM padding.\r\n");
    serial_puts_both("[SER] selfrx requires a nonzero size and an explicit CRC32.\r\n");
}

static void print_status(const flash_layout_t *layout)
{
    FILINFO fi;
    FRESULT fr;
    qspi_nor_t nor;

    serial_logf("\r\n[SER] Boot image: %s\r\n", APPLETINI_BOOT_IMAGE_VERSION_FULL);
    serial_logf("[SER] Firmware label: %s\r\n", APPLETINI_FIRMWARE_IMAGE_VERSION_FULL);
    serial_logf("[SER] Layout: %s\r\n", updater_layout_name(layout));
    if (layout != NULL) {
        serial_logf("[SER] Golden   @0x%08lX size=0x%08lX\r\n",
                    (unsigned long)layout->golden.offset,
                    (unsigned long)layout->golden.size);
        serial_logf("[SER] Firmware @0x%08lX size=0x%08lX\r\n",
                    (unsigned long)layout->firmware.offset,
                    (unsigned long)layout->firmware.size);
        serial_logf("[SER] Metadata @0x%08lX size=0x%08lX\r\n",
                    (unsigned long)layout->metadata.offset,
                    (unsigned long)layout->metadata.size);
        if (qspi_nor_init(&nor, layout->qspi_addr_bytes, layout->erase_size_bytes) == XST_SUCCESS) {
            serial_logf("[SER] Flash: JEDEC %02X %02X %02X capacity=%lu addr=%u-byte erase=0x%08lX\r\n",
                        (unsigned int)nor.jedec_id[0],
                        (unsigned int)nor.jedec_id[1],
                        (unsigned int)nor.jedec_id[2],
                        (unsigned long)nor.capacity_bytes,
                        (unsigned int)nor.addr_bytes,
                        (unsigned long)nor.sector_size);
        } else {
            serial_puts_both("[SER] Flash: QSPI init failed\r\n");
        }
    }

    fr = serial_sd_mount();
    if (fr != FR_OK) {
        serial_logf("[SER] SD: mount failed fr=%d\r\n", (int)fr);
        return;
    }

    fr = f_stat(SERIAL_BOOT_FW_PATH, &fi);
    if (fr == FR_OK) {
        serial_logf("[SER] SD: %s size=%llu\r\n",
                    SERIAL_BOOT_FW_PATH,
                    (unsigned long long)fi.fsize);
    } else {
        serial_logf("[SER] SD: no %s fr=%d\r\n",
                    SERIAL_BOOT_FW_PATH,
                    (int)fr);
    }

    fr = f_stat(SERIAL_BOOT_GOLDEN_PATH, &fi);
    if (fr == FR_OK) {
        serial_logf("[SER] SD: %s size=%llu\r\n",
                    SERIAL_BOOT_GOLDEN_PATH,
                    (unsigned long long)fi.fsize);
    } else {
        serial_logf("[SER] SD: no %s fr=%d\r\n",
                    SERIAL_BOOT_GOLDEN_PATH,
                    (int)fr);
    }
}

static void xmodem_abort(uint32_t uart_base)
{
    uart_putc_one(uart_base, (char)XMODEM_CAN);
    uart_putc_one(uart_base, (char)XMODEM_CAN);
    uart_putc_one(uart_base, (char)XMODEM_CAN);
}

static int receive_block_bytes(uint32_t uart_base, uint8_t *buf, uint32_t len)
{
    uint32_t i;

    for (i = 0U; i < len; ++i) {
        if (!uart_getc_timeout(uart_base, &buf[i], SERIAL_BOOT_XMODEM_BYTE_MS)) {
            return -1;
        }
    }
    return 0;
}

static int receive_xmodem(uint32_t uart_base,
                          const serial_rx_target_t *target,
                          uint8_t have_size,
                          uint32_t expected_size,
                          uint8_t have_crc,
                          uint32_t expected_crc)
{
    FIL f;
    FRESULT fr;
    uint32_t total_written = 0U;
    uint32_t crc32 = crc32_init();
    uint8_t expected_block = 1U;
    uint32_t retries = 0U;
    uint8_t started = 0U;
    uint8_t file_open = 0U;
    int result = -1;

    if (target == NULL || target->command_name == NULL || target->limit_name == NULL ||
        target->max_size == 0U) {
        serial_puts_both("[SER] rx failed: invalid receive target\r\n");
        return -1;
    }
    if (have_size != 0U && expected_size > target->max_size) {
        serial_logf("[SER] %s failed: size %lu exceeds %s %lu\r\n",
                    target->command_name,
                    (unsigned long)expected_size,
                    target->limit_name,
                    (unsigned long)target->max_size);
        return -1;
    }

    if (target->kind == SERIAL_RX_TARGET_SD_FILE) {
        if (target->tmp_path == NULL || target->final_path == NULL) {
            serial_puts_both("[SER] rx failed: invalid SD receive target\r\n");
            return -1;
        }
        fr = serial_sd_mount();
        if (fr != FR_OK) {
            serial_logf("[SER] %s failed: SD mount fr=%d\r\n",
                        target->command_name,
                        (int)fr);
            return -1;
        }

        (void)f_unlink(target->tmp_path);
        fr = f_open(&f, target->tmp_path, FA_CREATE_ALWAYS | FA_WRITE);
        if (fr != FR_OK) {
            serial_logf("[SER] %s failed: open temp fr=%d\r\n",
                        target->command_name,
                        (int)fr);
            return -1;
        }
        file_open = 1U;
    } else if (target->kind == SERIAL_RX_TARGET_MEMORY) {
        if (target->memory == NULL) {
            serial_puts_both("[SER] selfrx failed: no receive buffer\r\n");
            return -1;
        }
    } else {
        serial_puts_both("[SER] rx failed: invalid receive target kind\r\n");
        return -1;
    }

    uart_puts(uart_base, "\r\n[SER] Start XMODEM-CRC sender now.\r\n");
    uart_puts(uart_base, "[SER] Protocol bytes will follow on this UART.\r\n");
    if (have_size == 0U) {
        uart_puts(uart_base, "[SER] No size supplied; final file may include XMODEM padding.\r\n");
    }
    uart_puts(other_uart(uart_base), "\r\n[SER] XMODEM receive active on the other UART.\r\n");

    for (;;) {
        uint8_t header;
        uint32_t block_size;
        uint8_t block_num;
        uint8_t block_inv;
        uint8_t crc_hi;
        uint8_t crc_lo;
        uint16_t got_crc;
        uint16_t calc_crc;

        if (!started) {
            uint32_t start_try;
            int saw_header = 0;

            for (start_try = 0U; start_try < SERIAL_BOOT_XMODEM_START_TRY; ++start_try) {
                uart_putc_one(uart_base, (char)XMODEM_CRC_REQ);
                if (uart_getc_timeout(uart_base, &header, 1000U)) {
                    saw_header = 1;
                    break;
                }
            }
            if (!saw_header) {
                uart_puts(uart_base, "\r\n[SER] rx timeout waiting for XMODEM sender\r\n");
                goto out_abort;
            }
            started = 1U;
        } else if (!uart_getc_timeout(uart_base, &header, SERIAL_BOOT_XMODEM_BYTE_MS)) {
            if (++retries > SERIAL_BOOT_XMODEM_MAX_RETRY) {
                uart_puts(uart_base, "\r\n[SER] rx timeout\r\n");
                goto out_abort;
            }
            uart_putc_one(uart_base, (char)XMODEM_NAK);
            continue;
        }

        if (header == XMODEM_EOT) {
            if (have_size != 0U && total_written != expected_size) {
                uart_puts(uart_base, "\r\n[SER] rx failed: short transfer\r\n");
                goto out_abort;
            }
            crc32 = crc32_finish(crc32);
            if (have_crc != 0U && crc32 != expected_crc) {
                uart_puts(uart_base, "\r\n[SER] rx failed: CRC32 mismatch\r\n");
                goto out_abort;
            }
            if (target->kind == SERIAL_RX_TARGET_SD_FILE) {
                if (f_sync(&f) != FR_OK) {
                    uart_puts(uart_base, "\r\n[SER] rx failed: f_sync\r\n");
                    goto out_abort;
                }
                if (f_close(&f) != FR_OK) {
                    uart_puts(uart_base, "\r\n[SER] rx failed: close\r\n");
                    file_open = 0U;
                    goto out_abort;
                }
                file_open = 0U;
                (void)f_unlink(target->final_path);
                fr = f_rename(target->tmp_path, target->final_path);
                if (fr != FR_OK) {
                    serial_logf("\r\n[SER] rx failed: rename fr=%d\r\n", (int)fr);
                    goto out_abort;
                }
            }
            uart_putc_one(uart_base, (char)XMODEM_ACK);
            serial_logf("\r\n[SER] %s complete: wrote %lu bytes crc32=0x%08lX\r\n",
                        target->command_name,
                        (unsigned long)total_written,
                        (unsigned long)crc32);
            result = 0;
            goto out_done;
        }

        if (header == XMODEM_CAN) {
            uart_puts(uart_base, "\r\n[SER] rx cancelled by sender\r\n");
            goto out_abort;
        }

        if (header == XMODEM_SOH) {
            block_size = 128U;
        } else if (header == XMODEM_STX) {
            block_size = 1024U;
        } else {
            if (++retries > SERIAL_BOOT_XMODEM_MAX_RETRY) {
                uart_puts(uart_base, "\r\n[SER] rx failed: bad header\r\n");
                goto out_abort;
            }
            uart_putc_one(uart_base, (char)XMODEM_NAK);
            continue;
        }

        if (!uart_getc_timeout(uart_base, &block_num, SERIAL_BOOT_XMODEM_BYTE_MS) ||
            !uart_getc_timeout(uart_base, &block_inv, SERIAL_BOOT_XMODEM_BYTE_MS) ||
            receive_block_bytes(uart_base, g_xmodem_buf, block_size) != 0 ||
            !uart_getc_timeout(uart_base, &crc_hi, SERIAL_BOOT_XMODEM_BYTE_MS) ||
            !uart_getc_timeout(uart_base, &crc_lo, SERIAL_BOOT_XMODEM_BYTE_MS)) {
            if (++retries > SERIAL_BOOT_XMODEM_MAX_RETRY) {
                uart_puts(uart_base, "\r\n[SER] rx failed: packet timeout\r\n");
                goto out_abort;
            }
            uart_putc_one(uart_base, (char)XMODEM_NAK);
            continue;
        }

        got_crc = (uint16_t)(((uint16_t)crc_hi << 8) | crc_lo);
        calc_crc = crc16_ccitt(g_xmodem_buf, block_size);
        if ((uint8_t)(block_num + block_inv) != 0xFFU || got_crc != calc_crc) {
            if (++retries > SERIAL_BOOT_XMODEM_MAX_RETRY) {
                uart_puts(uart_base, "\r\n[SER] rx failed: packet CRC\r\n");
                goto out_abort;
            }
            uart_putc_one(uart_base, (char)XMODEM_NAK);
            continue;
        }

        if (block_num == (uint8_t)(expected_block - 1U)) {
            uart_putc_one(uart_base, (char)XMODEM_ACK);
            retries = 0U;
            continue;
        }
        if (block_num != expected_block) {
            uart_puts(uart_base, "\r\n[SER] rx failed: block sequence\r\n");
            goto out_abort;
        }

        if (have_size != 0U && total_written >= expected_size) {
            uart_puts(uart_base, "\r\n[SER] rx failed: extra data beyond expected size\r\n");
            goto out_abort;
        }

        {
            uint32_t to_write = block_size;
            UINT wrote = 0U;

            if (have_size != 0U && (total_written + to_write) > expected_size) {
                to_write = expected_size - total_written;
            }
            if ((total_written + to_write) > target->max_size) {
                uart_puts(uart_base, "\r\n[SER] rx failed: file exceeds target\r\n");
                goto out_abort;
            }
            if (target->kind == SERIAL_RX_TARGET_MEMORY) {
                memcpy(&target->memory[total_written], g_xmodem_buf, to_write);
            } else {
                fr = f_write(&f, g_xmodem_buf, (UINT)to_write, &wrote);
                if (fr != FR_OK || wrote != to_write) {
                    uart_puts(uart_base, "\r\n[SER] rx failed: SD write\r\n");
                    goto out_abort;
                }
            }
            crc32 = crc32_update(crc32, g_xmodem_buf, to_write);
            total_written += to_write;
        }

        expected_block++;
        retries = 0U;
        uart_putc_one(uart_base, (char)XMODEM_ACK);
    }

out_abort:
    xmodem_abort(uart_base);
    if (file_open != 0U) {
        (void)f_close(&f);
    }
    if (target->kind == SERIAL_RX_TARGET_SD_FILE) {
        (void)f_unlink(target->tmp_path);
    }
out_done:
    return result;
}

static int receive_firmware_xmodem(uint32_t uart_base,
                                   const flash_layout_t *layout,
                                   uint8_t have_size,
                                   uint32_t expected_size,
                                   uint8_t have_crc,
                                   uint32_t expected_crc)
{
    serial_rx_target_t target;

    if (layout == NULL) {
        serial_puts_both("[SER] rx failed: no flash layout\r\n");
        return -1;
    }

    target.kind = SERIAL_RX_TARGET_SD_FILE;
    target.command_name = "rx";
    target.limit_name = "firmware slot";
    target.max_size = layout->firmware.size;
    target.memory = NULL;
    target.tmp_path = SERIAL_BOOT_RX_TMP_PATH;
    target.final_path = SERIAL_BOOT_FW_PATH;
    return receive_xmodem(uart_base,
                          &target,
                          have_size,
                          expected_size,
                          have_crc,
                          expected_crc);
}

static int receive_golden_xmodem(uint32_t uart_base,
                                 uint32_t expected_size,
                                 uint32_t expected_crc)
{
    serial_rx_target_t target;

    target.kind = SERIAL_RX_TARGET_MEMORY;
    target.command_name = "selfrx";
    target.limit_name = "golden slot";
    target.max_size = GOLDEN_SELF_UPDATE_SLOT_SIZE;
    target.memory = g_golden_rx_buf;
    target.tmp_path = NULL;
    target.final_path = NULL;
    return receive_xmodem(uart_base,
                          &target,
                          1U,
                          expected_size,
                          1U,
                          expected_crc);
}

static serial_boot_action_t run_golden_self_update_from_sd(uint32_t uart_base)
{
    golden_self_update_result_t result;
    FRESULT fr;

    fr = serial_sd_mount();
    if (fr != FR_OK) {
        serial_logf("[SER] selfupdate failed: SD mount fr=%d\r\n", (int)fr);
        return SERIAL_BOOT_ACTION_NONE;
    }

    serial_logf("[SER] Staging %s in the golden trial slot\r\n",
                SERIAL_BOOT_GOLDEN_PATH);
    if (golden_self_update_run(SERIAL_BOOT_GOLDEN_PATH,
                               uart_base,
                               other_uart(uart_base),
                               &result) != 0 ||
        result.updated == 0 || result.verified == 0 ||
        result.reset_required == 0 ||
        result.boot_offset != GOLDEN_SELF_UPDATE_TRIAL_OFFSET) {
        serial_puts_both("[SER] selfupdate failed; staying in monitor\r\n");
        return SERIAL_BOOT_ACTION_NONE;
    }

    g_staged_boot_offset = result.boot_offset;
    serial_puts_both("[SER] Golden trial verified; booting trial slot\r\n");
    return SERIAL_BOOT_ACTION_BOOT_GOLDEN_TRIAL;
}

static serial_boot_action_t run_golden_self_update_from_memory(uint32_t uart_base,
                                                               uint32_t image_size)
{
    golden_self_update_result_t result;

    serial_puts_both("[SER] Staging received image in the golden trial slot\r\n");
    if (golden_self_update_run_memory(g_golden_rx_buf,
                                      image_size,
                                      uart_base,
                                      other_uart(uart_base),
                                      &result) != 0 ||
        result.updated == 0 || result.verified == 0 ||
        result.reset_required == 0 ||
        result.boot_offset != GOLDEN_SELF_UPDATE_TRIAL_OFFSET) {
        serial_puts_both("[SER] selfrx update failed; staying in monitor\r\n");
        return SERIAL_BOOT_ACTION_NONE;
    }

    g_staged_boot_offset = result.boot_offset;
    serial_puts_both("[SER] Golden trial verified; booting trial slot\r\n");
    return SERIAL_BOOT_ACTION_BOOT_GOLDEN_TRIAL;
}

static serial_boot_action_t process_command(uint32_t uart_base,
                                            char *line,
                                            const flash_layout_t *layout)
{
    char *argv[SERIAL_BOOT_ARG_MAX];
    int argc = split_args(line, argv, SERIAL_BOOT_ARG_MAX);

    if (argc == 0) {
        return SERIAL_BOOT_ACTION_NONE;
    }

    if (str_ieq(argv[0], "help") || str_ieq(argv[0], "?")) {
        print_help();
        return SERIAL_BOOT_ACTION_NONE;
    }
    if (str_ieq(argv[0], "status")) {
        print_status(layout);
        return SERIAL_BOOT_ACTION_NONE;
    }
    if (str_ieq(argv[0], "continue") || str_ieq(argv[0], "auto")) {
        serial_puts_both("[SER] Continuing normal SD update/boot path\r\n");
        return SERIAL_BOOT_ACTION_CONTINUE;
    }
    if (str_ieq(argv[0], "update")) {
        serial_puts_both("[SER] Running SD updater now\r\n");
        return SERIAL_BOOT_ACTION_RUN_UPDATER;
    }
    if (str_ieq(argv[0], "boot") || str_ieq(argv[0], "firmware") || str_ieq(argv[0], "run")) {
        serial_puts_both("[SER] Booting firmware slot now\r\n");
        return SERIAL_BOOT_ACTION_BOOT_FIRMWARE;
    }
    if (str_ieq(argv[0], "reboot") || str_ieq(argv[0], "reset")) {
        serial_puts_both("[SER] Rebooting through golden image\r\n");
        return SERIAL_BOOT_ACTION_REBOOT_GOLDEN;
    }
    if (str_ieq(argv[0], "selfupdate")) {
        if (argc != 1) {
            serial_puts_both("[SER] usage: selfupdate\r\n");
            return SERIAL_BOOT_ACTION_NONE;
        }
        return run_golden_self_update_from_sd(uart_base);
    }
    if (str_ieq(argv[0], "selfrx")) {
        uint32_t expected_size;
        uint32_t expected_crc;

        if (argc != 3 ||
            appletini_parse_u32(argv[1], &expected_size) != 0 ||
            appletini_parse_u32(argv[2], &expected_crc) != 0 ||
            expected_size == 0U) {
            serial_puts_both("[SER] usage: selfrx <nonzero-size> <crc32>\r\n");
            return SERIAL_BOOT_ACTION_NONE;
        }
        if (expected_size > GOLDEN_SELF_UPDATE_SLOT_SIZE) {
            serial_logf("[SER] selfrx failed: size %lu exceeds golden slot %lu\r\n",
                        (unsigned long)expected_size,
                        (unsigned long)GOLDEN_SELF_UPDATE_SLOT_SIZE);
            return SERIAL_BOOT_ACTION_NONE;
        }
        if (receive_golden_xmodem(uart_base, expected_size, expected_crc) != 0) {
            serial_puts_both("[SER] selfrx failed; staying in monitor\r\n");
            return SERIAL_BOOT_ACTION_NONE;
        }
        return run_golden_self_update_from_memory(uart_base, expected_size);
    }
    if (str_ieq(argv[0], "rx") || str_ieq(argv[0], "receive")) {
        uint32_t expected_size = 0U;
        uint32_t expected_crc = 0U;
        uint8_t have_size = 0U;
        uint8_t have_crc = 0U;

        if (argc >= 2) {
            if (appletini_parse_u32(argv[1], &expected_size) != 0) {
                serial_puts_both("[SER] usage: rx [size] [crc32]\r\n");
                return SERIAL_BOOT_ACTION_NONE;
            }
            have_size = 1U;
        }
        if (argc >= 3) {
            if (appletini_parse_u32(argv[2], &expected_crc) != 0) {
                serial_puts_both("[SER] usage: rx [size] [crc32]\r\n");
                return SERIAL_BOOT_ACTION_NONE;
            }
            have_crc = 1U;
        }
        if (receive_firmware_xmodem(uart_base, layout, have_size, expected_size, have_crc, expected_crc) == 0) {
            return SERIAL_BOOT_ACTION_RUN_UPDATER;
        }
        serial_puts_both("[SER] rx failed; staying in monitor\r\n");
        return SERIAL_BOOT_ACTION_NONE;
    }

    serial_puts_both("[SER] Unknown command. Use ? or :help\r\n");
    return SERIAL_BOOT_ACTION_NONE;
}

uint32_t serial_boot_staged_boot_offset(void)
{
    return g_staged_boot_offset;
}

serial_boot_action_t serial_boot_menu(const flash_layout_t *layout,
                                      uint32_t auto_timeout_seconds)
{
    char cmd[SERIAL_BOOT_CMD_MAX];
    uint32_t cmd_len = 0U;
    uint8_t cmd_mode = 0U;
    uint32_t cmd_uart = UART0_BASE;
    XTime start = 0U;
    XTime now = 0U;
    XTime timeout_ticks = 0U;

    if (auto_timeout_seconds == 0U) {
        serial_puts_both("\r\n[SER] Golden serial monitor active. Use ? for help.\r\n");
    } else {
        serial_logf("\r\n[SER] Golden serial monitor: press ? or type :help within %lu seconds.\r\n",
                    (unsigned long)auto_timeout_seconds);
        XTime_GetTime(&start);
        timeout_ticks = (XTime)((uint64_t)COUNTS_PER_SECOND * (uint64_t)auto_timeout_seconds);
    }

    for (;;) {
        char c;
        uint32_t base;

        if (poll_any_uart(&base, &c)) {
            if (cmd_mode != 0U) {
                if (cmd_len == 0U && (c == ':' || c == ';')) {
                    continue;
                }
                if (c == '\r' || c == '\n') {
                    serial_boot_action_t action;
                    uart_puts(cmd_uart, "\r\n");
                    cmd[cmd_len] = '\0';
                    cmd_mode = 0U;
                    action = process_command(cmd_uart, cmd, layout);
                    cmd_len = 0U;
                    if (action != SERIAL_BOOT_ACTION_NONE) {
                        return action;
                    }
                    uart_puts(cmd_uart, "[SER] cmd> ");
                    cmd_mode = 1U;
                    continue;
                }
                if ((c == 8 || c == 127) && cmd_len > 0U) {
                    --cmd_len;
                    uart_puts(cmd_uart, "\b \b");
                    continue;
                }
                if (c >= 32 && c <= 126 && cmd_len < (SERIAL_BOOT_CMD_MAX - 1U)) {
                    cmd[cmd_len++] = c;
                    uart_putc_one(cmd_uart, c);
                }
                continue;
            }

            if (c == '?') {
                print_help();
                cmd_mode = 1U;
                cmd_uart = base;
                cmd_len = 0U;
                uart_puts(cmd_uart, "[SER] cmd> ");
                continue;
            }
            if (c == ':' || c == ';') {
                cmd_mode = 1U;
                cmd_uart = base;
                cmd_len = 0U;
                uart_puts(cmd_uart, "\r\n[SER] cmd> ");
                continue;
            }
            if (c == '\r' || c == '\n') {
                continue;
            }
            if (c >= 32 && c <= 126) {
                cmd_mode = 1U;
                cmd_uart = base;
                cmd_len = 0U;
                uart_puts(cmd_uart, "\r\n[SER] cmd> ");
                cmd[cmd_len++] = c;
                uart_putc_one(cmd_uart, c);
                continue;
            }
        }

        if (auto_timeout_seconds != 0U && cmd_mode == 0U) {
            XTime_GetTime(&now);
            if ((now - start) >= timeout_ticks) {
                return SERIAL_BOOT_ACTION_CONTINUE;
            }
        }
    }
}
