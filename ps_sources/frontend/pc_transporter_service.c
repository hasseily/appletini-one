#include "pc_transporter_service.h"

#include <stdio.h>
#include <string.h>

#include "xil_cache.h"
#include "xil_mmu.h"

#include "pc_transporter_machine.h"
#include "pc_transporter_regs.h"
#include "onee_input_service.h"
#include "../lib/common.h"
#include "../lib/uart.h"

#define PCT_SLICE           64U

static pc_transporter_machine_t machine;
static bool initialized;
static bool enabled;
static bool failed;
static bool posted;
static uint32_t request_sequence;
static uint32_t log_uart;
static uint8_t posted_ack;
static uint8_t reset_sequence;
static bool guest_running;

static uint8_t key_state[ONEE_INPUT_DEVICE_SLOT_COUNT][16];
static uint8_t key_modifiers[ONEE_INPUT_DEVICE_SLOT_COUNT];
static uint8_t key_queue[64];
static uint8_t key_read, key_write;
static bool caps_lock;
static uint32_t key_drops;

/* USB HID usage to XT scan code for the basic PC keyboard. */
static const uint8_t xt_keys[0x64] = {
    [0x04] = 0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23,
    0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19,
    0x10, 0x13, 0x1f, 0x14, 0x16, 0x2f, 0x11, 0x2d,
    0x15, 0x2c, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b,
    [0x28] = 0x1c, 0x01, 0x0e, 0x0f, 0x39, 0x0c, 0x0d, 0x1a,
    0x1b, 0x2b, 0x2b, 0x27, 0x28, 0x29, 0x33, 0x34, 0x35, 0x3a,
    [0x3a] = 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40, 0x41, 0x42, 0x43, 0x44,
    [0x47] = 0x46,
    [0x49] = 0x52, 0x47, 0x49, 0x53, 0x4f, 0x51, 0x4d, 0x4b, 0x50, 0x48,
    [0x53] = 0x45, 0x35, 0x37, 0x4a, 0x4e, 0x1c,
    0x4f, 0x50, 0x51, 0x4b, 0x4c, 0x4d, 0x47, 0x48, 0x49, 0x52, 0x53,
};

static void collect_keys(uint8_t state[16])
{
    memset(state, 0, 16U);
    for (unsigned slot = 0; slot < ONEE_INPUT_DEVICE_SLOT_COUNT; ++slot) {
        for (unsigned byte = 0; byte < 16U; ++byte) {
            state[byte] |= key_state[slot][byte];
        }
    }
}

bool pc_transporter_service_keyboard_report(uint8_t slot, uint8_t modifiers,
                                            const uint8_t *keys, uint32_t count,
                                            bool capture)
{
    bool route = guest_running && capture;
    if (!enabled || slot >= ONEE_INPUT_DEVICE_SLOT_COUNT) {
        return false;
    }
    uint8_t before[16], after[16];
    collect_keys(before);
    key_modifiers[slot] = modifiers;
    memset(key_state[slot], 0, sizeof(key_state[slot]));
    if (route) {
        static const uint8_t xt_modifiers[8] = {0x1d, 0x2a, 0x38, 0, 0x1d, 0x36, 0x38, 0};
        for (unsigned bit = 0; bit < 8U; ++bit) {
            unsigned code = xt_modifiers[bit];
            if (code != 0U && (modifiers & (1U << bit)) != 0U) {
                key_state[slot][code >> 3] |= 1U << (code & 7U);
            }
        }
        for (uint32_t i = 0; keys != NULL && i < count; ++i) {
            unsigned code = keys[i] < sizeof(xt_keys) ? xt_keys[keys[i]] : 0U;
            if (code != 0U) {
                key_state[slot][code >> 3] |= 1U << (code & 7U);
            }
        }
    }
    collect_keys(after);
    for (unsigned code = 1U; code < 0x80U; ++code) {
        unsigned mask = 1U << (code & 7U);
        if (((before[code >> 3] ^ after[code >> 3]) & mask) == 0U) {
            continue;
        }
        bool make = (after[code >> 3] & mask) != 0U;
        if (make && code == 0x3aU) {
            caps_lock = !caps_lock;
        }
        uint8_t next = (key_write + 1U) % sizeof(key_queue);
        if (next != key_read) {
            key_queue[key_write] = (uint8_t)(code | (make ? 0U : 0x80U));
            key_write = next;
        } else {
            key_drops++;
        }
    }
    return route;
}

static void poll_keyboard(void)
{
    uint8_t modifier = 0U;
    for (unsigned slot = 0; slot < ONEE_INPUT_DEVICE_SLOT_COUNT; ++slot) {
        modifier |= key_modifiers[slot];
    }
    uint8_t apple_modifier = (modifier & 0x11U) != 0U ? 0U : 1U;
    apple_modifier |= (modifier & 0x22U) != 0U ? 0U : 2U;
    apple_modifier |= (modifier & 0x04U) != 0U ? 4U : 0U;
    apple_modifier |= (modifier & 0x40U) != 0U ? 8U : 0U;
    apple_modifier |= caps_lock ? 0U : 0x80U;
    uint32_t state = REG_READ(PCT_KEYS);
    if ((state & (1U << 16)) != 0U) {
        return;
    }
    if (key_read != key_write) {
        REG_WRITE(PCT_KEYS, (1U << 16) | ((uint32_t)key_queue[key_read] << 8) | apple_modifier);
        key_read = (key_read + 1U) % sizeof(key_queue);
    } else if ((uint8_t)state != apple_modifier) {
        REG_WRITE(PCT_KEYS, apple_modifier);
    }
}

static uint8_t read_shared(void *opaque, uint32_t offset)
{
    (void)opaque;
    return *(volatile uint8_t *)(uintptr_t)(PCT_DDR_BASE + offset);
}

static void write_shared(void *opaque, uint32_t offset, uint8_t data)
{
    (void)opaque;
    *(volatile uint8_t *)(uintptr_t)(PCT_DDR_BASE + offset) = data;
}

static bool read_port(void *opaque, uint16_t port, uint8_t *data)
{
    (void)opaque;
    if (port >= 0x400U) {
        return false;
    }
    REG_WRITE(PCT_PORT_INDEX, port);
    /* Complete the index write before the read address reaches the FPGA. */
    __asm__ volatile ("dsb sy" ::: "memory");
    *data = (uint8_t)REG_READ(PCT_PORT_DATA);
    return true;
}

static bool write_port(void *opaque, uint16_t port, uint8_t data)
{
    (void)opaque;
    if (port >= 0x400U) {
        return false;
    }
    REG_WRITE(PCT_PORT_INDEX, port);
    REG_WRITE(PCT_PORT_DATA, data);
    return true;
}

bool pc_transporter_service_video_active(void)
{
    return guest_running;
}

static void submit_io(void *opaque, const pc_transporter_io_request_t *request)
{
    uint32_t status = REG_READ(PCT_STATUS);
    uint32_t transfer = PCT_POST | request->port |
        ((uint32_t)request->lanes << 18) | ((uint32_t)reset_sequence << 20);

    (void)opaque;
    if ((status & (PCT_ENABLED | PCT_RESET | PCT_PENDING)) != PCT_ENABLED ||
        (uint8_t)(status >> 16) != reset_sequence) {
        return;
    }
    if (!request->write) {
        transfer |= 1U << 16;
    }
    if (request->lanes == PCT_IO_LANES_WORD) {
        transfer |= 1U << 17;
    }
    posted_ack = (uint8_t)(status >> 8);
    request_sequence = request->sequence;
    posted = true;
    /* Publish port data and RAM writes before the Apple IRQ. */
    __asm__ volatile ("dsb sy" ::: "memory");
    REG_WRITE(PCT_TRANSFER, transfer);
}

static const pc_transporter_machine_ops_t machine_ops = {
    .read_shared = read_shared,
    .write_shared = write_shared,
    .read_port = read_port,
    .write_port = write_port,
    .submit_io = submit_io,
};

void pc_transporter_service_init(uint32_t uart_base)
{
    if (initialized || REG_READ(PCT_REG(0U)) != PCT_CARD_ID) {
        return;
    }
    memset((void *)(uintptr_t)PCT_DDR_BASE, 0, PCT_RAM_BYTES);
    Xil_DCacheFlushRange(PCT_DDR_BASE, PCT_RAM_BYTES);
    Xil_SetTlbAttributes(PCT_DDR_BASE, NORM_NONCACHE);
    pc_transporter_machine_init(&machine, &machine_ops, NULL);
    log_uart = uart_base;
    initialized = true;
}

void pc_transporter_service_set_enabled(bool active)
{
    if (!initialized || enabled == active) {
        return;
    }
    if (active) {
        REG_WRITE(PCT_DIAG_CONTROL, PCT_CLEAR_COUNTERS);
        key_drops = 0U;
    } else {
        REG_WRITE(PCT_DIAG_CONTROL, PCT_CANCEL);
    }
    pc_transporter_machine_reset(&machine);
    reset_sequence = (uint8_t)(REG_READ(PCT_STATUS) >> 16);
    posted = false;
    failed = false;
    guest_running = false;
    key_read = key_write = 0U;
    memset(key_state, 0, sizeof(key_state));
    enabled = active;
}

void pc_transporter_service_uart_status(uint32_t uart_base)
{
    if (!initialized) {
        uart_puts(uart_base, "PC Transporter test mode has not started.\r\n");
        return;
    }
    char line[208];
    uint32_t status = REG_READ(PCT_STATUS);
    uint32_t transfer = REG_READ(PCT_TRANSFER);
    uint32_t late = REG_READ(PCT_LATE_READS);
    uint32_t drops = REG_READ(PCT_WRITE_DROPS);
    uint32_t errors = REG_READ(PCT_DDR_ERRORS);
    (void)snprintf(line, sizeof(line),
        "PC Transporter: status=%08lx transfer=%08lx pc=%04x:%04x fault=%u\r\n"
        "late=%lu write_drops=%lu ddr_errors=%lu key_drops=%lu illegal=%lu\r\n",
        (unsigned long)status, (unsigned long)transfer,
        (unsigned)machine.cpu.segregs[regcs], (unsigned)machine.cpu.ip, (unsigned)failed,
        (unsigned long)late, (unsigned long)drops, (unsigned long)errors,
        (unsigned long)key_drops, (unsigned long)machine.illegal_instructions);
    uart_puts(uart_base, line);
}

void pc_transporter_service_poll(void)
{
    if (!enabled) {
        return;
    }
    uint32_t status = REG_READ(PCT_STATUS);
    uint8_t generation = (uint8_t)(status >> 16);
    if (generation != reset_sequence || (status & PCT_ENABLED) == 0U) {
        pc_transporter_machine_reset(&machine);
        reset_sequence = generation;
        posted = false;
        guest_running = false;
        key_read = key_write = 0U;
        memset(key_state, 0, sizeof(key_state));
    }
    if ((status & PCT_ENABLED) == 0U) {
        return;
    }
    guest_running = !failed && (status & (PCT_RESET | PCT_HALT | PCT_FAULT)) == 0U;
    poll_keyboard();
    if ((status & PCT_FAULT) != 0U) {
        if (!failed) {
            uart_puts(log_uart, "PC Transporter: Apple bus memory fault\r\n");
            uart_puts(log_uart, "Deadline misses / dropped writes / DDR errors: ");
            uart_puthex(log_uart, REG_READ(PCT_LATE_READS));
            uart_puthex(log_uart, REG_READ(PCT_WRITE_DROPS));
            uart_puthex(log_uart, REG_READ(PCT_DDR_ERRORS));
            uart_puts(log_uart, "\r\n");
            REG_WRITE(PCT_DIAG_CONTROL, PCT_CANCEL);
        }
        failed = true;
    }
    pc_transporter_machine_set_control(&machine, (status & PCT_RESET) != 0U,
        failed || (status & PCT_HALT) != 0U || (status & PCT_DRAINED) == 0U);
    if (failed || (status & PCT_DRAINED) == 0U) {
        return;
    }
    if (posted && (status & PCT_PENDING) == 0U && (uint8_t)(status >> 8) != posted_ack) {
        uint16_t data = (uint16_t)REG_READ(PCT_ACK_DATA);
        if ((uint8_t)(REG_READ(PCT_STATUS) >> 16) != generation) {
            return;
        }
        posted = false;
        (void)pc_transporter_machine_io_complete(&machine, request_sequence, data);
    }
    bool interrupt = (status & PCT_INT) != 0U;
    uint8_t vector = interrupt ? read_shared(NULL, PCT_INT_VECTOR) : 0U;
    if (interrupt && ((REG_READ(PCT_STATUS) ^ status) & (0xffff0000U | PCT_INT)) != 0U) {
        return;
    }
    pc_transporter_machine_set_interrupt(&machine, interrupt, vector);
    uint32_t accepted_before = machine.accepted_interrupts;
    uint32_t illegal_before = machine.illegal_instructions;
    pc_transporter_machine_exec(&machine, PCT_SLICE);
    if (machine.accepted_interrupts != accepted_before) {
        REG_WRITE(PCT_DIAG_CONTROL, PCT_ACCEPT_INT | ((status >> 24) << 8) |
            ((uint32_t)generation << 20));
        __asm__ volatile ("dsb sy" ::: "memory");
    }
    if (machine.illegal_instructions != illegal_before) {
        failed = true;
        guest_running = false;
        REG_WRITE(PCT_DIAG_CONTROL, PCT_CANCEL);
        uart_puts(log_uart, "PC Transporter: unsupported opcode and address ");
        uart_puthex(log_uart, machine.last_illegal_opcode);
        uart_puthex(log_uart, machine.last_illegal_address);
        uart_puts(log_uart, "\r\n");
    }
}
