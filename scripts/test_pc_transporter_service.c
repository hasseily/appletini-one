#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "pc_transporter_service.h"
#include "pc_transporter_machine.h"
#include "pc_transporter_regs.h"

typedef uint64_t XTime;
/* Match the BSP's macro shape so clock arithmetic exercises operator precedence. */
#define COUNTS_PER_SECOND 2000000U / 2U
#define NORM_NONCACHE 0U

#define PROGRAM 0x0100U
#define MARKER 0x0300U
#define RUNNING (PCT_ENABLED | PCT_DRAINED)
#define STOPPED (RUNNING | PCT_RESET | PCT_HALT)

static uint32_t shared_dwords[PCT_RAM_BYTES / 4U];
#undef PCT_DDR_BASE
#define PCT_DDR_BASE ((uintptr_t)shared_dwords)
static uint16_t ems_words[PCT_EMS_BYTES / 2U];
#undef PCT_EMS_DDR_BASE
#define PCT_EMS_DDR_BASE ((uintptr_t)ems_words)

static uint32_t registers[18];
static unsigned cancel_count, clear_count, opcode_logs, memory_logs;
static char status_line[256];
static XTime ticks;

static uint32_t *reg_ptr(uint32_t address)
{
    assert(address >= PCT_MMIO_BASE && address <= PCT_CGA_RASTER);
    assert((address & 3U) == 0U);
    return &registers[(address - PCT_MMIO_BASE) / 4U];
}

#define REG_READ(address) (*reg_ptr(address))

static void REG_WRITE(uint32_t address, uint32_t data)
{
    if (address == PCT_DIAG_CONTROL) {
        if ((data & PCT_CLEAR_COUNTERS) != 0U) {
            ++clear_count;
            REG_READ(PCT_STATUS) &= ~PCT_FAULT;
        }
        if ((data & PCT_CANCEL) != 0U) {
            ++cancel_count;
            REG_READ(PCT_STATUS) &= ~PCT_PENDING;
        }
    }
    *reg_ptr(address) = data;
}

static void XTime_GetTime(XTime *time)
{
    ticks += 50U;
    *time = ticks;
}

static void Xil_DCacheFlushRange(uintptr_t address, unsigned size)
{
    assert(address == PCT_DDR_BASE && size == PCT_RAM_BYTES);
}

static void Xil_SetTlbAttributes(uintptr_t address, unsigned attributes)
{
    assert(address == PCT_DDR_BASE && attributes == NORM_NONCACHE);
}

static void uart_puts(uint32_t base, const char *message)
{
    (void)base;
    if (strstr(message, "unsupported opcode") != NULL) {
        ++opcode_logs;
    } else if (strstr(message, "Apple bus memory fault") != NULL) {
        ++memory_logs;
    } else if (strstr(message, "PC Transporter: status=") != NULL) {
        (void)snprintf(status_line, sizeof(status_line), "%s", message);
    }
}

static void uart_puthex(uint32_t base, uint32_t value)
{
    (void)base;
    (void)value;
}

void pc_transporter_cga_capture_get_stats(uint32_t *completed, uint32_t *dropped)
{
    *completed = 0U;
    *dropped = 0U;
}

#include "pc_transporter_service_native.inc"

static void poll(unsigned generation, uint32_t state)
{
    REG_READ(PCT_STATUS) = (generation << 16) | state;
    pc_transporter_service_poll();
}

static void check_status(bool running, unsigned fault, unsigned illegal)
{
    char expected[24];
    assert(pc_transporter_service_guest_running() == running);
    pc_transporter_service_uart_status(0U);
    (void)snprintf(expected, sizeof(expected), "fault=%u\r\n", fault);
    assert(strstr(status_line, expected) != NULL);
    (void)snprintf(expected, sizeof(expected), "illegal=%u\r\n", illegal);
    assert(strstr(status_line, expected) != NULL);
}

int main(void)
{
    static const uint8_t reset_jump[] = {0xEA, 0x00, 0x01, 0x00, 0x00};
    static const uint8_t bad_code[] = {0x0F};
    static const uint8_t good_code[] = {0xC6, 0x06, 0x00, 0x03, 0xA5, 0xF4};
    uint8_t *const ram = (uint8_t *)shared_dwords;

    REG_READ(PCT_REG(0U)) = PCT_CARD_ID;
    REG_READ(PCT_STATUS) = STOPPED;
    pc_transporter_service_set_maximum_speed(true);
    pc_transporter_service_init(0U);
    pc_transporter_service_set_maximum_speed(false);
    pc_transporter_service_set_enabled(true);
    memcpy(ram + PCT_BIOS_RAM_BASE + 0xFFF0U, reset_jump, sizeof(reset_jump));
    ram[6U * 4U + 1U] = 2U; /* Invalid opcode vector points to a HLT at 0000:0200. */
    ram[0x0200U] = 0xF4U;

    memcpy(ram + PROGRAM, bad_code, sizeof(bad_code));
    poll(0U, RUNNING);
    check_status(false, 1U, 1U);
    assert(opcode_logs == 1U && cancel_count == 1U);

    memcpy(ram + PROGRAM, good_code, sizeof(good_code));
    poll(0U, RUNNING | PCT_HALT);
    poll(0U, RUNNING);
    check_status(false, 1U, 1U);
    assert(ram[MARKER] == 0U);

    poll(1U, STOPPED);
    check_status(false, 0U, 1U);
    assert(ram[MARKER] == 0U);
    poll(1U, RUNNING);
    check_status(true, 0U, 1U);
    assert(ram[MARKER] == 0xA5U);

    memcpy(ram + PROGRAM, bad_code, sizeof(bad_code));
    ram[MARKER] = 0U;
    poll(2U, RUNNING);
    check_status(false, 1U, 2U);
    memcpy(ram + PROGRAM, good_code, sizeof(good_code));
    poll(3U, RUNNING); /* Apple released reset between polls. */
    check_status(true, 0U, 2U);
    assert(ram[MARKER] == 0xA5U);
    puts("PASS opcode fault recovery on guest reset, including a missed reset edge");

    memcpy(ram + PROGRAM, bad_code, sizeof(bad_code));
    ram[MARKER] = 0U;
    poll(4U, RUNNING);
    check_status(false, 1U, 3U);
    assert(opcode_logs == 3U && cancel_count == 3U);
    poll(4U, RUNNING | PCT_FAULT);
    assert(memory_logs == 1U && cancel_count == 4U);

    memcpy(ram + PROGRAM, good_code, sizeof(good_code));
    poll(5U, STOPPED | PCT_FAULT);
    poll(5U, RUNNING | PCT_FAULT);
    check_status(false, 1U, 3U);
    assert(ram[MARKER] == 0U);
    assert(memory_logs == 1U && cancel_count == 4U && clear_count == 1U);
    puts("PASS PL memory fault overrides opcode fault and blocks a guest reset");

    pc_transporter_service_set_enabled(false);
    pc_transporter_service_set_enabled(true);
    static const uint8_t cpu_loop[] = {0xEB, 0xFE};
    memcpy(ram + PROGRAM, cpu_loop, sizeof(cpu_loop));
    poll(6U, RUNNING);
    const uint32_t standard = guest_slices;
    assert(standard == 3U);
    pc_transporter_service_set_maximum_speed(true);
    poll(6U, RUNNING);
    const uint32_t maximum = guest_slices - standard;
    assert(maximum == 10U);
    pc_transporter_service_video_poll();
    assert(guest_slices - standard - maximum == 2U);
    pc_transporter_service_i2c_poll();
    assert(guest_slices - standard - maximum == 3U);
    poll(6U, RUNNING | PCT_HALT);
    assert(guest_slices - standard - maximum == 3U);
    puts("PASS maximum foreground CPU budget with unchanged video, I2C and HALT behavior");

    static const uint8_t bench_clock[] = {
        0xBA, 0x00, 0x7B, 0xED, 0xA3, 0x00, 0x03, // Signature
        0xBA, 0x02, 0x7B, 0xED, 0xA3, 0x02, 0x03, // First low word
        0xBA, 0x04, 0x7B, 0xED, 0xA3, 0x04, 0x03, // First high word
        0xBA, 0x02, 0x7B, 0xED, 0xA3, 0x06, 0x03, // Second low word
        0xBA, 0x04, 0x7B, 0xED, 0xA3, 0x08, 0x03, // Second high word
        0xF4,
    };
    memcpy(ram + PROGRAM, bench_clock, sizeof(bench_clock));
    ticks = 2000000U - 175U;
    poll(7U, RUNNING);
    assert(ram[MARKER] == 'P' && ram[MARKER + 1U] == 'B');
    const uint8_t boundary_bytes[] = {0x67U, 0x84U, 0x1EU, 0U, 0x99U, 0x84U, 0x1EU, 0U};
    assert(memcmp(ram + MARKER + 2U, boundary_bytes, sizeof(boundary_bytes)) == 0);

    poll(7U, RUNNING | PCT_HALT);
    ticks = UINT32_MAX - 150U;
    poll(8U, RUNNING);
    const uint8_t clock_bytes[] = {0xFFU, 0xFFU, 0xFFU, 0xFFU, 49U, 0U, 0U, 0U};
    assert(memcmp(ram + MARKER + 2U, clock_bytes, sizeof(clock_bytes)) == 0);
    puts("PASS guest benchmark clock signature, coherent words, second boundary, and wrap");
    return 0;
}
