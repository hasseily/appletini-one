#include "pc_transporter_service.h"

#include <stdio.h>
#include <string.h>

#include "xil_cache.h"
#include "xil_mmu.h"
#include "xiltimer.h"

#include "pc_transporter_machine.h"
#include "pc_transporter_regs.h"
#include "pc_transporter_cga_capture.h"
#include "onee_input_service.h"
#include "../lib/common.h"
#include "../lib/uart.h"

#define PCT_SLICE           64U
#define PCT_BURST_US        250U
#define PCT_BURST_MAX_US    1000U
#define PCT_VIDEO_BURST_US  120U
#define PCT_BENCH_ID_PORT   0x7B00U
#define PCT_BENCH_TIME_PORT 0x7B02U
#define PCT_BENCH_ID        0x4250U

typedef enum {
    PCT_SITE_FOREGROUND,
    PCT_SITE_CGA,
    PCT_SITE_I2C,
} pct_slice_site_t;

static const char *const slice_site_names[] = {
    [PCT_SITE_FOREGROUND] = "foreground",
    [PCT_SITE_CGA] = "cga",
    [PCT_SITE_I2C] = "i2c",
};

static pc_transporter_machine_t machine;
static bool initialized;
static bool enabled;
static bool maximum_speed;
static uint32_t bench_time_us;
static bool opcode_failed;
static bool memory_failed;
static bool posted;
static uint32_t request_sequence;
static uint32_t log_uart;
static uint8_t posted_ack;
static uint8_t reset_sequence;
static bool guest_running;
static bool guest_keyboard_exclusive;
static uint32_t guest_slices;
static uint32_t i2c_slices;
static uint32_t drain_skips;
static XTime last_slice_start;
static XTime last_slice_finish;
static XTime max_slice_ticks;
static XTime max_gap_ticks;
static XTime stop_gap_ticks;
static bool stop_seen;
static pct_slice_site_t last_slice_site;
static pct_slice_site_t max_gap_from;
static pct_slice_site_t max_gap_to;
static uint16_t last_slice_cs;
static uint16_t last_slice_ip;
static uint16_t last_slice_cx;

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
    return route && guest_keyboard_exclusive;
}

static void sync_keyboard_focus(bool running, uint32_t status)
{
    /* Portware arms Apple IRQs before releasing HALT. Keep its focus through
     * short IRQ masks. A guest that never arms IRQs can still poll scan codes. */
    guest_keyboard_exclusive = running &&
        ((guest_running && guest_keyboard_exclusive) ||
         (status & PCT_APPLE_IRQ_ENABLED) != 0U);
    guest_running = running;
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

static uint16_t read_shared_word(void *opaque, uint32_t offset)
{
    (void)opaque;
    return *(volatile uint16_t *)(uintptr_t)(PCT_DDR_BASE + offset);
}

static void write_shared_word(void *opaque, uint32_t offset, uint16_t data)
{
    (void)opaque;
    *(volatile uint16_t *)(uintptr_t)(PCT_DDR_BASE + offset) = data;
}

static uint32_t read_shared_dword(void *opaque, uint32_t offset)
{
    (void)opaque;
    return *(volatile uint32_t *)(uintptr_t)(PCT_DDR_BASE + offset);
}

static bool read_port(void *opaque, uint16_t port, uint8_t *data)
{
    (void)opaque;
    if (port >= PCT_BENCH_ID_PORT && port < PCT_BENCH_TIME_PORT) {
        *data = (uint8_t)(PCT_BENCH_ID >> (8U * (port - PCT_BENCH_ID_PORT)));
        return true;
    }
    if (port >= PCT_BENCH_TIME_PORT && port < PCT_BENCH_TIME_PORT + 4U) {
        if (port == PCT_BENCH_TIME_PORT) {
            XTime now;
            XTime_GetTime(&now);
            /* The BSP defines COUNTS_PER_SECOND as an unparenthesized division. */
            const uint64_t counts_per_second = (COUNTS_PER_SECOND);
            const uint32_t seconds = (uint32_t)(now / counts_per_second);
            bench_time_us = seconds * 1000000U +
                (uint32_t)((now % counts_per_second) * 1000000ULL / counts_per_second);
        }
        *data = (uint8_t)(bench_time_us >> (8U * (port - PCT_BENCH_TIME_PORT)));
        return true;
    }
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

bool pc_transporter_service_guest_running(void)
{
    return guest_running;
}

uint32_t pc_transporter_service_cga_state(void)
{
    if (!enabled || opcode_failed || memory_failed) {
        return 0U;
    }
    const uint32_t state = REG_READ(PCT_CGA_STATE);
    if ((state & PCT_CGA_VIDEO_ENABLE) == 0U) {
        return 0U;
    }
    if (guest_running) {
        return state;
    }

    /* Portware draws its 80-column Control Panel and blink-enabled warnings
     * with HALT set. CGADEMO2 restores 40-column text with blink clear when
     * it returns to the Apple screen. */
    const uint32_t ready = PCT_ENABLED | PCT_HALT | PCT_DRAINED;
    if ((state & (PCT_CGA_GRAPHICS | PCT_CGA_TEXT_80COL | PCT_CGA_BLINK)) != 0U &&
        (REG_READ(PCT_STATUS) & (ready | PCT_RESET | PCT_FAULT)) == ready) {
        return state;
    }
    return 0U;
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
    .read_shared_word = read_shared_word,
    .write_shared_word = write_shared_word,
    .read_shared_dword = read_shared_dword,
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
    pc_transporter_machine_set_maximum_speed(&machine, maximum_speed);
    memset((void *)(uintptr_t)PCT_EMS_DDR_BASE, 0, PCT_EMS_BYTES);
    pc_transporter_machine_set_ems_memory(&machine, (uint16_t *)(uintptr_t)PCT_EMS_DDR_BASE);
    log_uart = uart_base;
    initialized = true;
}

void pc_transporter_service_set_maximum_speed(bool maximum)
{
    maximum_speed = maximum;
    if (initialized) {
        pc_transporter_machine_set_maximum_speed(&machine, maximum);
    }
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
    opcode_failed = false;
    memory_failed = false;
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
    char line[256];
    uint32_t status = REG_READ(PCT_STATUS);
    uint32_t transfer = REG_READ(PCT_TRANSFER);
    uint32_t late = REG_READ(PCT_LATE_READS);
    uint32_t drops = REG_READ(PCT_WRITE_DROPS);
    uint32_t errors = REG_READ(PCT_DDR_ERRORS);
    uint32_t holds = REG_READ(PCT_VIRTUAL_HOLDS);
    uint32_t max_hold = REG_READ(PCT_VIRTUAL_HOLD_MAX);
    uint32_t captured, incomplete;
    pc_transporter_cga_capture_get_stats(&captured, &incomplete);
    (void)snprintf(line, sizeof(line),
        "PC Transporter: status=%08lx transfer=%08lx pc=%04x:%04x fault=%u\r\n"
        "late=%lu write_drops=%lu ddr_errors=%lu key_drops=%lu illegal=%lu\r\n"
        "guest_slices=%lu max_slice_us=%lu max_slice_gap_us=%lu\r\n",
        (unsigned long)status, (unsigned long)transfer,
        (unsigned)machine.cpu.segregs[regcs], (unsigned)machine.cpu.ip,
        (unsigned)(opcode_failed || memory_failed),
        (unsigned long)late, (unsigned long)drops, (unsigned long)errors,
        (unsigned long)key_drops, (unsigned long)machine.illegal_instructions,
        (unsigned long)guest_slices,
        (unsigned long)(max_slice_ticks * 1000000ULL / (COUNTS_PER_SECOND)),
        (unsigned long)(max_gap_ticks * 1000000ULL / (COUNTS_PER_SECOND)));
    uart_puts(uart_base, line);
    (void)snprintf(line, sizeof(line),
        "virtual_held_reads=%lu max_hold_clks=%lu\r\n"
        "cga_captured_frames=%lu cga_incomplete_frames=%lu\r\n"
        "cpu_speed=%s ems_kib=%u\r\n",
        (unsigned long)holds, (unsigned long)max_hold,
        (unsigned long)captured, (unsigned long)incomplete,
        maximum_speed ? "maximum" : "standard", PCT_EMS_BYTES / 1024U);
    uart_puts(uart_base, line);
    if (guest_slices != 0U) {
        (void)snprintf(line, sizeof(line),
            "last_slice_pc=%04x:%04x last_slice_cx=%04x\r\n"
            "max_gap_sites=%s/%s drain_skips=%lu i2c_slices=%lu last_site=%s\r\n"
            "stop_seen=%u stop_gap_ticks=%llu stop_gap_us=%lu\r\n",
            (unsigned)last_slice_cs, (unsigned)last_slice_ip, (unsigned)last_slice_cx,
            slice_site_names[max_gap_from], slice_site_names[max_gap_to],
            (unsigned long)drain_skips, (unsigned long)i2c_slices,
            slice_site_names[last_slice_site],
            (unsigned)stop_seen, (unsigned long long)stop_gap_ticks,
            (unsigned long)(stop_gap_ticks * 1000000ULL / (COUNTS_PER_SECOND)));
        uart_puts(uart_base, line);
    }
}

static bool run_guest_slice(bool service_keyboard, pct_slice_site_t site, XTime *finished)
{
    uint32_t status = REG_READ(PCT_STATUS);
    uint8_t generation = (uint8_t)(status >> 16);
    bool new_generation = generation != reset_sequence;
    if (new_generation) {
        /* A new guest boot can replace the code that faulted. */
        opcode_failed = false;
    }
    bool running = !opcode_failed && !memory_failed &&
        (status & (PCT_ENABLED | PCT_RESET | PCT_HALT | PCT_FAULT)) == PCT_ENABLED;
    if (guest_running && (!running || new_generation) && guest_slices != 0U) {
        XTime stopped;
        XTime_GetTime(&stopped);
        // The Apple can take time to reach the reset after sampling a deadline.
        stop_gap_ticks = stopped - last_slice_finish;
        stop_seen = true;
    }
    if (new_generation || (status & PCT_ENABLED) == 0U) {
        pc_transporter_machine_reset(&machine);
        reset_sequence = generation;
        posted = false;
        guest_running = false;
        key_read = key_write = 0U;
        memset(key_state, 0, sizeof(key_state));
    }
    if ((status & PCT_ENABLED) == 0U) {
        return false;
    }
    if (running && !guest_running) {
        guest_slices = 0U;
        i2c_slices = 0U;
        drain_skips = 0U;
        last_slice_start = 0U;
        max_slice_ticks = 0U;
        max_gap_ticks = 0U;
        stop_gap_ticks = 0U;
        stop_seen = false;
        max_gap_from = site;
        max_gap_to = site;
    }
    sync_keyboard_focus(running, status);
    if (service_keyboard) {
        poll_keyboard();
    }
    if ((status & PCT_FAULT) != 0U && !memory_failed) {
        memory_failed = true;
        uart_puts(log_uart, "PC Transporter: Apple bus memory fault\r\n");
        uart_puts(log_uart, "Deadline misses / dropped writes / DDR errors: ");
        uart_puthex(log_uart, REG_READ(PCT_LATE_READS));
        uart_puthex(log_uart, REG_READ(PCT_WRITE_DROPS));
        uart_puthex(log_uart, REG_READ(PCT_DDR_ERRORS));
        uart_puts(log_uart, "\r\n");
        REG_WRITE(PCT_DIAG_CONTROL, PCT_CANCEL);
    }
    const bool failed = opcode_failed || memory_failed;
    pc_transporter_machine_set_control(&machine, (status & PCT_RESET) != 0U,
        failed || (status & PCT_HALT) != 0U || (status & PCT_DRAINED) == 0U);
    if (running && (status & PCT_DRAINED) == 0U) {
        drain_skips++;
    }
    if (failed || (status & PCT_DRAINED) == 0U) {
        return false;
    }
    if (posted && (status & PCT_PENDING) == 0U && (uint8_t)(status >> 8) != posted_ack) {
        uint16_t data = (uint16_t)REG_READ(PCT_ACK_DATA);
        if ((uint8_t)(REG_READ(PCT_STATUS) >> 16) != generation) {
            return false;
        }
        posted = false;
        (void)pc_transporter_machine_io_complete(&machine, request_sequence, data);
    }
    bool interrupt = (status & PCT_INT) != 0U;
    uint8_t vector = interrupt ? read_shared(NULL, PCT_INT_VECTOR) : 0U;
    if (interrupt && ((REG_READ(PCT_STATUS) ^ status) & (0xffff0000U | PCT_INT)) != 0U) {
        return false;
    }
    pc_transporter_machine_set_interrupt(&machine, interrupt, vector);
    if (!guest_running || machine.cpu.io_pending != 0U ||
        (machine.cpu.hltstate != 0U && (!interrupt || machine.cpu.ifl == 0U))) {
        last_slice_start = 0U;
        return false;
    }
    uint32_t accepted_before = machine.accepted_interrupts;
    uint32_t illegal_before = machine.illegal_instructions;
    XTime start;
    XTime_GetTime(&start);
    pc_transporter_machine_exec(&machine, PCT_SLICE);
    XTime_GetTime(finished);
    if (last_slice_start != 0U && start - last_slice_start > max_gap_ticks) {
        max_gap_ticks = start - last_slice_start;
        max_gap_from = last_slice_site;
        max_gap_to = site;
    }
    if (*finished - start > max_slice_ticks) {
        max_slice_ticks = *finished - start;
    }
    last_slice_start = start;
    last_slice_finish = *finished;
    last_slice_site = site;
    guest_slices++;
    if (site == PCT_SITE_I2C) {
        i2c_slices++;
    }
    /* Keep the last execution state even if the Apple resets the guest. */
    last_slice_cs = machine.cpu.segregs[regcs];
    last_slice_ip = machine.cpu.ip;
    last_slice_cx = machine.cpu.regs.wordregs[regcx];
    if (machine.accepted_interrupts != accepted_before) {
        REG_WRITE(PCT_DIAG_CONTROL, PCT_ACCEPT_INT | ((status >> 24) << 8) |
            ((uint32_t)generation << 20));
        __asm__ volatile ("dsb sy" ::: "memory");
    }
    if (machine.illegal_instructions != illegal_before) {
        opcode_failed = true;
        guest_running = false;
        REG_WRITE(PCT_DIAG_CONTROL, PCT_CANCEL);
        uart_puts(log_uart, "PC Transporter: unsupported opcode and address ");
        uart_puthex(log_uart, machine.last_illegal_opcode);
        uart_puthex(log_uart, machine.last_illegal_address);
        uart_puts(log_uart, "\r\n");
    }
    bool ready = !opcode_failed && machine.cpu.io_pending == 0U && machine.cpu.hltstate == 0U;
    if (!ready) {
        last_slice_start = 0U;
    }
    return ready;
}

static void poll_guest(pct_slice_site_t site)
{
    if (!enabled) {
        return;
    }
    // CGA calls this between small groups of rows and must yield on schedule.
    const uint32_t budget_us = site == PCT_SITE_CGA ? PCT_VIDEO_BURST_US :
        (maximum_speed ? PCT_BURST_MAX_US : PCT_BURST_US);
    const XTime budget = (budget_us * (uint64_t)(COUNTS_PER_SECOND)) / 1000000ULL;
    XTime start, finished;
    bool service_keyboard = true;

    // Yield to other foreground work after a short burst. Recheck the FPGA
    // before each slice so Apple reset, halt and I/O still take effect.
    XTime_GetTime(&start);
    do {
        if (!run_guest_slice(service_keyboard, site, &finished)) {
            return;
        }
        service_keyboard = false;
    } while (finished - start < budget);
}

void pc_transporter_service_poll(void)
{
    poll_guest(PCT_SITE_FOREGROUND);
}

void pc_transporter_service_video_poll(void)
{
    poll_guest(PCT_SITE_CGA);
}

void pc_transporter_service_i2c_poll(void)
{
    XTime finished;
    if (guest_running) {
        (void)run_guest_slice(false, PCT_SITE_I2C, &finished);
    }
}
