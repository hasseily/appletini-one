/* Clean-room ALF AD8088 Plus monitor and PS-side 8088 service.
 *
 * The hardware manuals define the mailbox, memory map and PROM commands but
 * no redistributable PROM image is needed: commands are dispatched here, and
 * CALL/user routines execute in the GPL XTulator 8086 core configured for the
 * 8088 instruction set. The local-memory map is the 640 KB configuration used
 * by AD8088 Plus software; the original AD128K range remains compatible.
 */

#include "ad8088_service.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "xiltimer.h"

#include "ad8088_machine.h"
#include "applicard_regs.h"
#include "../lib/common.h"
#include "../lib/uart.h"

#define AD8088_BUS_TIMEOUT_US       20000U
#define AD8088_BULK_POLL_BUDGET_US  1000U
#define AD8088_SEQUENCE_PAIRS_MAX   32768U
/* Extra exec-slice time granted while a triggered paddle measurement is
 * still counting, in the same clock base as the wall cap. */
#define AD8088_PADDLE_EXTEND_US     4000U
/* How long a running 8088 program may stall on a blocked Apple bus (a
 * Disk II timing window) before the access is declared failed. */
#define AD8088_MACHINE_BLOCK_STALL_US 100000U

typedef enum {
    AD_MONITOR_IDLE = 0,
    AD_MONITOR_SEQUENCE,
    AD_MONITOR_FILL,
    AD_MONITOR_COPY,
    AD_MONITOR_CALL,
    AD_MONITOR_RECOVER
} ad_monitor_state_t;

static ad8088_machine_t g_machine;
static uint32_t g_uart_base;
static uint8_t g_enabled;
static uint8_t g_trace;
static uint8_t g_memory_initialized;
static uint32_t g_wall_cap_us = AD8088_WALL_CAP_STANDARD_US;
static void (*g_checkpoint)(void);
static ad_monitor_state_t g_monitor_state;
static uint8_t g_ports[16];
static uint8_t g_resume_sequence;
static uint16_t g_sequence_addr;
static uint32_t g_sequence_pairs;
static uint32_t g_bulk_src;
static uint32_t g_bulk_dst;
static uint32_t g_bulk_remaining;
static uint8_t g_bulk_value;
static uint8_t g_bulk_stage[AD8088_BUS_BUFFER_BYTES];
static uint32_t g_random = 0x13579BDFU;
static uint32_t g_reset_count;
static uint32_t g_command_count;
static uint32_t g_error_count;
static uint32_t g_signal_count;
/* Last hard bus-transfer failure, for `8088 status` attribution.
 * kind: 1 = PL reported DONE+ERROR, 2 = PS-side wait timeout,
 * 3 = bulk index mismatch, 4 = blocked-stall timeout (bus stayed
 * blocked past AD8088_MACHINE_BLOCK_STALL_US). status is the raw
 * BUS_STATUS word at failure (bit 11 = sticky blocked verdict). cs:ip
 * is captured only when the failure killed a running 8088 program. */
static uint8_t  g_last_err_kind;
static uint16_t g_last_err_addr;
static uint32_t g_last_err_status;
static uint16_t g_last_err_cs;
static uint16_t g_last_err_ip;
/* Programs stopped because an Apple reset (or explicit abort) rejected an
 * in-flight window access. This is the normal fate of a running program
 * when the user resets the machine; it is not an error. */
static uint32_t g_reset_stop_count;

/* Paddle-measurement observatory. Guest code measures a paddle by
 * triggering $C070 and counting its own $C064/$C065 poll iterations until
 * bit 7 falls. The iteration count is what the guest sees; the wall-clock
 * microseconds are the ground truth of the analog pulse. Comparing the two
 * across measurements separates guest-visible jitter caused by our bursty
 * execution (counts vary, us stable) from real analog jitter (us varies). */
#define AD8088_PADDLE_RING 4U
typedef struct {
    uint32_t count;
    uint32_t us;
} ad_paddle_sample_t;
static ad_paddle_sample_t g_paddle_ring[2][AD8088_PADDLE_RING];
static uint8_t  g_paddle_ring_pos[2];
static uint32_t g_paddle_measurements[2];
static uint8_t  g_paddle_active[2];
static uint32_t g_paddle_count[2];
static XTime    g_paddle_start;
static XTime    g_paddle_deadline;

static uint64_t ad8088_us_to_ticks(uint32_t us)
{
    return ((uint64_t)us * COUNTS_PER_SECOND) / 1000000ULL;
}

static void ad8088_paddle_trigger(void)
{
    XTime_GetTime(&g_paddle_start);
    g_paddle_deadline = g_paddle_start +
        (XTime)ad8088_us_to_ticks(AD8088_PADDLE_EXTEND_US);
    for (uint32_t p = 0U; p < 2U; ++p) {
        g_paddle_active[p] = 1U;
        g_paddle_count[p] = 0U;
    }
}

static void ad8088_paddle_read(uint32_t paddle, uint8_t value)
{
    if (g_paddle_active[paddle] == 0U) {
        return;
    }
    g_paddle_count[paddle]++;
    if ((value & 0x80U) == 0U) {
        XTime now;
        ad_paddle_sample_t *sample =
            &g_paddle_ring[paddle][g_paddle_ring_pos[paddle]];

        XTime_GetTime(&now);
        sample->count = g_paddle_count[paddle];
        sample->us = (uint32_t)(((uint64_t)(now - g_paddle_start) *
                                 1000000ULL) / COUNTS_PER_SECOND);
        g_paddle_ring_pos[paddle] =
            (uint8_t)((g_paddle_ring_pos[paddle] + 1U) % AD8088_PADDLE_RING);
        g_paddle_measurements[paddle]++;
        g_paddle_active[paddle] = 0U;
        if (g_paddle_active[paddle ^ 1U] == 0U) {
            g_paddle_deadline = 0U;
        }
    }
}

/* While a triggered measurement is still counting, keep the exec slice
 * alive: a main-loop gap lets the one-shot expire unobserved and collapses
 * the guest's iteration count -- one garbage paddle value per gap. Bounded
 * to one extra wall-cap per slice so the main loop cannot starve. */
static uint8_t ad8088_paddle_extend_active(XTime now, XTime start)
{
    if (g_paddle_active[0] == 0U && g_paddle_active[1] == 0U) {
        return 0U;
    }
    if (g_paddle_deadline == 0U || now >= g_paddle_deadline) {
        return 0U;
    }
    return ((uint64_t)(now - start) <
            ad8088_us_to_ticks(g_wall_cap_us + AD8088_PADDLE_EXTEND_US))
        ? 1U : 0U;
}
static XTime g_status_tick;
static uint64_t g_status_instructions;

/* Formats take up to three unsigned-long values; extra arguments beyond a
 * format's specifiers are ignored, so short formats pass zeros. */
static void ad8088_trace(const char *format, uint32_t a, uint32_t b,
                         uint32_t c)
{
    char line[96];

    if (g_trace == 0U) {
        return;
    }
    (void)snprintf(line, sizeof(line), format,
                   (unsigned long)a, (unsigned long)b, (unsigned long)c);
    uart_puts(g_uart_base, line);
}

static int ad8088_wait_bus_complete(uint8_t initial_seq,
                                    uint8_t *rdata,
                                    uint32_t expected_index)
{
    XTime start;
    uint32_t status;
    uint32_t spins = 0U;

    XTime_GetTime(&start);
    for (;;) {
        XTime now;
        status = REG_READ(APPLICARD_REG_BUS_STATUS);
        if (AD8088_BUS_STATUS_SEQ(status) != initial_seq &&
            (status & AD8088_BUS_STATUS_DONE) != 0U) {
            if ((status & AD8088_BUS_STATUS_ERROR) != 0U ||
                (expected_index < AD8088_BUS_BUFFER_BYTES &&
                 AD8088_BUS_STATUS_INDEX(status) != expected_index)) {
                if ((status & AD8088_BUS_STATUS_BLOCKED) != 0U) {
                    return AD8088_XFER_BLOCKED;
                }
                g_last_err_kind = ((status & AD8088_BUS_STATUS_ERROR) != 0U)
                    ? 1U : 3U;
                g_last_err_status = status;
                return AD8088_XFER_ERROR;
            }
            if (rdata != NULL) {
                *rdata = (uint8_t)status;
            }
            return AD8088_XFER_OK;
        }
        XTime_GetTime(&now);
        if ((uint64_t)(now - start) >=
            ad8088_us_to_ticks(AD8088_BUS_TIMEOUT_US)) {
            REG_WRITE(APPLICARD_REG_AD_CONTROL, AD8088_CONTROL_CANCEL_BUS);
            g_last_err_kind = 2U;
            g_last_err_status = status;
            return AD8088_XFER_ERROR;
        }
        if (++spins >= 1024U) {
            spins = 0U;
            if (g_checkpoint != NULL) {
                g_checkpoint();
            }
        }
    }
}

static int ad8088_pl_bus_transfer(uint16_t addr, uint8_t rw,
                                  uint8_t wdata, uint8_t *rdata)
{
    uint32_t status;
    uint8_t initial_seq;

    status = REG_READ(APPLICARD_REG_AD_STATUS);
    if ((status & AD8088_STATUS_BUS_BLOCKED) != 0U) {
        return AD8088_XFER_BLOCKED;
    }
    initial_seq = (uint8_t)AD8088_BUS_STATUS_SEQ(
        REG_READ(APPLICARD_REG_BUS_STATUS));

    REG_WRITE(APPLICARD_REG_BUS_COMMAND,
        AD8088_BUS_COMMAND_GO |
        ((rw != 0U) ? AD8088_BUS_COMMAND_RW : 0U) |
        AD8088_BUS_COMMAND_WDATA(wdata) | addr);
    {
        const int rc = ad8088_wait_bus_complete(initial_seq, rdata,
                                                AD8088_BUS_BUFFER_BYTES);
        if (rc == AD8088_XFER_ERROR) {
            g_last_err_addr = addr;
        }
        return rc;
    }
}

static int ad8088_pl_bus_bulk(uint16_t addr, uint8_t rw,
                              uint8_t *data, uint32_t count)
{
    uint8_t initial_seq;
    uint32_t status;

    if (data == NULL || count == 0U || count > AD8088_BUS_BUFFER_BYTES) {
        return AD8088_XFER_ERROR;
    }
    status = REG_READ(APPLICARD_REG_AD_STATUS);
    if ((status & AD8088_STATUS_BUS_BLOCKED) != 0U) {
        return AD8088_XFER_BLOCKED;
    }

    if (rw == 0U) {
        for (uint32_t offset = 0U; offset < count; offset += 4U) {
            uint32_t word = 0U;
            for (uint32_t byte = 0U; byte < 4U && offset + byte < count;
                 ++byte) {
                word |= (uint32_t)data[offset + byte] << (byte * 8U);
            }
            REG_WRITE(APPLICARD_REG_BUS_BUFFER + offset, word);
        }
    }

    initial_seq = (uint8_t)AD8088_BUS_STATUS_SEQ(
        REG_READ(APPLICARD_REG_BUS_STATUS));
    REG_WRITE(APPLICARD_REG_BUS_COMMAND,
        AD8088_BUS_COMMAND_GO | AD8088_BUS_COMMAND_BULK |
        ((rw != 0U) ? AD8088_BUS_COMMAND_RW : 0U) |
        AD8088_BUS_COMMAND_COUNT(count) | addr);
    {
        const int rc = ad8088_wait_bus_complete(initial_seq, NULL,
                                                 count - 1U);
        if (rc != AD8088_XFER_OK) {
            if (rc == AD8088_XFER_ERROR) {
                g_last_err_addr = addr;
            }
            return rc;
        }
    }

    if (rw != 0U) {
        for (uint32_t offset = 0U; offset < count; offset += 4U) {
            const uint32_t word =
                REG_READ(APPLICARD_REG_BUS_BUFFER + offset);
            for (uint32_t byte = 0U; byte < 4U && offset + byte < count;
                 ++byte) {
                data[offset + byte] = (uint8_t)(word >> (byte * 8U));
            }
        }
    }
    return AD8088_XFER_OK;
}

/* One Apple-bus access on behalf of an executing 8088 program. While the
 * bus is blocked (a Disk II timing window, vTW handover), the program
 * stalls here with checkpoints -- real hardware would insert wait states --
 * instead of consuming a failed access mid-instruction. Monitor paths run
 * with the machine inactive and keep their yield-and-retry semantics. */
static int ad8088_machine_bus_access(uint16_t addr, uint8_t rw,
                                     uint8_t wdata, uint8_t *rdata)
{
    XTime start = 0U;

    for (;;) {
        const int rc = ad8088_pl_bus_transfer(addr, rw, wdata, rdata);
        if (rc != AD8088_XFER_BLOCKED || g_machine.active == 0U) {
            return rc;
        }
        if (start == 0U) {
            XTime_GetTime(&start);
        } else {
            XTime now;
            XTime_GetTime(&now);
            if ((uint64_t)(now - start) >=
                ad8088_us_to_ticks(AD8088_MACHINE_BLOCK_STALL_US)) {
                g_last_err_kind = 4U;
                g_last_err_status = 0U;
                g_last_err_addr = addr;
                return rc;
            }
        }
        if (g_checkpoint != NULL) {
            g_checkpoint();
        }
    }
}

static int ad8088_apple_read(void *opaque, uint16_t addr, uint8_t *value)
{
    const int rc = ad8088_machine_bus_access(addr, 1U, 0U, value);
    (void)opaque;
    /* Page filter first: window RAM traffic pays one compare. */
    if (rc == AD8088_XFER_OK && (addr & 0xFF00U) == 0xC000U) {
        if (addr == 0xC070U) {
            ad8088_paddle_trigger();
        } else if (addr == 0xC064U || addr == 0xC065U) {
            ad8088_paddle_read((uint32_t)(addr - 0xC064U), *value);
        }
    }
    return rc;
}

static int ad8088_apple_write(void *opaque, uint16_t addr, uint8_t value)
{
    const int rc = ad8088_machine_bus_access(addr, 0U, value, NULL);
    (void)opaque;
    if (rc == AD8088_XFER_OK && addr == 0xC070U) {
        ad8088_paddle_trigger();
    }
    return rc;
}

static uint8_t ad8088_cpu_port_read(void *opaque, uint8_t port)
{
    const uint32_t status = REG_READ(APPLICARD_REG_AD_STATUS);
    (void)opaque;

    if (port == 0U) {
        return (uint8_t)AD8088_STATUS_LAST_DATA(status);
    }
    if (port == 1U) {
        return (uint8_t)(((status & AD8088_STATUS_FLAG) != 0U ? 0x80U : 0U) |
                         AD8088_STATUS_LAST_PORT(status));
    }
    return 0xFFU;
}

static void ad8088_cpu_port_write(void *opaque, uint8_t port, uint8_t value)
{
    (void)opaque;
    (void)value;
    if (port == 0U) {
        REG_WRITE(APPLICARD_REG_AD_CONTROL, AD8088_CONTROL_SET_FLAG);
        g_signal_count++;
    }
}

static void ad8088_read_ports(void)
{
    for (uint32_t group = 0U; group < 4U; ++group) {
        const uint32_t packed =
            REG_READ(APPLICARD_REG_AD_PORTS0 + (group * 4U));
        for (uint32_t byte = 0U; byte < 4U; ++byte) {
            g_ports[group * 4U + byte] =
                (uint8_t)(packed >> (byte * 8U));
        }
    }
}

static void ad8088_publish_ports3(void)
{
    const uint32_t packed =
        (uint32_t)g_ports[12] |
        ((uint32_t)g_ports[13] << 8) |
        ((uint32_t)g_ports[14] << 16) |
        ((uint32_t)g_ports[15] << 24);
    REG_WRITE(APPLICARD_REG_AD_PORTS3, packed);
}

static uint16_t ad8088_address_b(void)
{
    return (uint16_t)((uint16_t)g_ports[14] |
                      ((uint16_t)g_ports[15] << 8));
}

static void ad8088_set_address_b(uint16_t address)
{
    g_ports[14] = (uint8_t)address;
    g_ports[15] = (uint8_t)(address >> 8);
    ad8088_publish_ports3();
}

static int ad8088_read_bytes(uint32_t address, uint8_t *data, uint32_t count)
{
    for (uint32_t i = 0U; i < count; ++i) {
        const int rc = ad8088_machine_read(&g_machine, address + i, &data[i]);
        if (rc != AD8088_XFER_OK) {
            if (rc == AD8088_XFER_BLOCKED) {
                g_machine.bus_fault = 0U;
            }
            return rc;
        }
    }
    return AD8088_XFER_OK;
}

static int ad8088_write_bytes(uint32_t address,
                              const uint8_t *data,
                              uint32_t count)
{
    for (uint32_t i = 0U; i < count; ++i) {
        const int rc = ad8088_machine_write(&g_machine, address + i, data[i]);
        if (rc != AD8088_XFER_OK) {
            if (rc == AD8088_XFER_BLOCKED) {
                g_machine.bus_fault = 0U;
            }
            return rc;
        }
    }
    return AD8088_XFER_OK;
}

static uint32_t ad8088_reference_linear(const uint8_t ref[4])
{
    const uint16_t offset = (uint16_t)((uint16_t)ref[0] |
                                       ((uint16_t)ref[1] << 8));
    const uint16_t segment = (uint16_t)((uint16_t)ref[2] |
                                        ((uint16_t)ref[3] << 8));
    return ((((uint32_t)segment << 4) + offset) & AD8088_ADDRESS_MASK);
}

static void ad8088_ready(void)
{
    g_monitor_state = AD_MONITOR_IDLE;
    g_resume_sequence = 0U;
    REG_WRITE(APPLICARD_REG_AD_CONTROL,
              AD8088_CONTROL_CLEAR_RUNNING | AD8088_CONTROL_SET_FLAG);
}

static void ad8088_async_complete(void)
{
    if (g_resume_sequence != 0U) {
        g_resume_sequence = 0U;
        g_monitor_state = AD_MONITOR_SEQUENCE;
    } else {
        ad8088_ready();
    }
}

static void ad8088_async_start(uint8_t top_level, uint8_t seq)
{
    uint32_t control = AD8088_CONTROL_CLEAR_FLAG |
                       AD8088_CONTROL_SET_RUNNING;
    if (top_level != 0U) {
        control |= AD8088_CONTROL_ACK(seq);
    }
    REG_WRITE(APPLICARD_REG_AD_CONTROL, control);
}

static void ad8088_async_fail(void)
{
    g_error_count++;
    g_bulk_remaining = 0U;
    g_resume_sequence = 0U;
    g_monitor_state = AD_MONITOR_RECOVER;
    REG_WRITE(APPLICARD_REG_AD_CONTROL,
              AD8088_CONTROL_CANCEL_BUS |
              AD8088_CONTROL_CLEAR_RUNNING |
              AD8088_CONTROL_CLEAR_FLAG);
}

static void ad8088_poll_recover(void)
{
    if ((REG_READ(APPLICARD_REG_AD_STATUS) &
         AD8088_STATUS_BUS_BUSY) == 0U) {
        ad8088_ready();
    }
}

static void ad8088_finish_command(uint8_t top_level, uint8_t seq)
{
    if (top_level != 0U) {
        REG_WRITE(APPLICARD_REG_AD_CONTROL, AD8088_CONTROL_FINISH(seq));
        g_monitor_state = AD_MONITOR_IDLE;
    }
}

static int32_t ad8088_load_i32(const uint8_t bytes[4])
{
    const uint32_t value = (uint32_t)bytes[0] |
                           ((uint32_t)bytes[1] << 8) |
                           ((uint32_t)bytes[2] << 16) |
                           ((uint32_t)bytes[3] << 24);
    return (int32_t)value;
}

static void ad8088_store_u32(uint8_t bytes[4], uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static int ad8088_integer_command(uint8_t command)
{
    uint8_t data[6];
    const uint32_t address = AD8088_APPLE_WINDOW_BASE + ad8088_address_b();
    int rc;

    rc = ad8088_read_bytes(address, data, sizeof(data));
    if (rc != AD8088_XFER_OK) {
        return rc;
    }
    if (command == 29U || command == 30U) {
        uint32_t product;
        if (command == 29U) {
            const uint16_t a = (uint16_t)((uint16_t)data[0] |
                                          ((uint16_t)data[1] << 8));
            const uint16_t b = (uint16_t)((uint16_t)data[2] |
                                          ((uint16_t)data[3] << 8));
            product = (uint32_t)a * (uint32_t)b;
        } else {
            const int16_t a = (int16_t)((uint16_t)data[0] |
                                        ((uint16_t)data[1] << 8));
            const int16_t b = (int16_t)((uint16_t)data[2] |
                                        ((uint16_t)data[3] << 8));
            product = (uint32_t)((int32_t)a * (int32_t)b);
        }
        ad8088_store_u32(data, product);
        return ad8088_write_bytes(address, data, 4U);
    }

    if (command == 31U) {
        const uint32_t dividend = (uint32_t)ad8088_load_i32(data);
        const uint16_t divisor = (uint16_t)((uint16_t)data[4] |
                                            ((uint16_t)data[5] << 8));
        uint16_t quotient = 0x8000U;
        uint16_t remainder = 0U;
        if (divisor != 0U && (dividend / divisor) <= 0xFFFFU) {
            quotient = (uint16_t)(dividend / divisor);
            remainder = (uint16_t)(dividend % divisor);
        }
        data[0] = (uint8_t)quotient;
        data[1] = (uint8_t)(quotient >> 8);
        data[2] = (uint8_t)remainder;
        data[3] = (uint8_t)(remainder >> 8);
        return ad8088_write_bytes(address, data, 4U);
    }

    if (command == 32U) {
        const int32_t dividend = ad8088_load_i32(data);
        const int16_t divisor = (int16_t)((uint16_t)data[4] |
                                          ((uint16_t)data[5] << 8));
        int16_t quotient = (int16_t)0x8000U;
        int16_t remainder = 0;
        if (divisor != 0) {
            const int64_t q = (int64_t)dividend / (int64_t)divisor;
            if (q >= -32768LL && q <= 32767LL) {
                quotient = (int16_t)q;
                remainder = (int16_t)(dividend % divisor);
            }
        }
        data[0] = (uint8_t)quotient;
        data[1] = (uint8_t)((uint16_t)quotient >> 8);
        data[2] = (uint8_t)remainder;
        data[3] = (uint8_t)((uint16_t)remainder >> 8);
        return ad8088_write_bytes(address, data, 4U);
    }
    return -1;
}

static double ad8088_float_decode(const uint8_t value[5])
{
    const int32_t mantissa = ad8088_load_i32(value);
    if (mantissa == 0 && value[4] == 0U) {
        return 0.0;
    }
    /* The manual's integer form uses exponent $9F, so the stored mantissa
     * is scaled by 2^(exponent-159). */
    return ldexp((double)mantissa, (int)value[4] - 159);
}

static void ad8088_float_encode(double number, uint8_t value[5])
{
    int exponent;
    double fraction;
    int64_t mantissa;

    if (number == 0.0 || isnan(number)) {
        memset(value, 0, 5U);
        return;
    }
    if (!isfinite(number)) {
        ad8088_store_u32(value,
            (number < 0.0) ? 0x80000000U : 0x7FFFFFFFU);
        value[4] = 0xFFU;
        return;
    }

    fraction = frexp(number, &exponent);
    if (fraction == -0.5) {
        fraction = -1.0;
        exponent--;
    }
    if (exponent < -128) {
        memset(value, 0, 5U);
        return;
    }
    if (exponent > 127) {
        ad8088_store_u32(value,
            (number < 0.0) ? 0x80000000U : 0x7FFFFFFFU);
        value[4] = 0xFFU;
        return;
    }
    mantissa = (int64_t)(fraction * 2147483648.0);
    if (mantissa > 2147483647LL) mantissa = 2147483647LL;
    if (mantissa < -2147483648LL) mantissa = -2147483648LL;
    ad8088_store_u32(value, (uint32_t)(int32_t)mantissa);
    value[4] = (uint8_t)(exponent + 128);
}

static int ad8088_float_command(uint8_t command)
{
    uint16_t b = ad8088_address_b();
    const uint16_t original_b = b;
    uint8_t tos_raw[5];
    uint8_t nos_raw[5];
    double tos;
    double nos;
    double result;
    int rc;

    if (command == 33U) {
        ad8088_set_address_b((uint16_t)(b - 5U));
        return 0;
    }
    if (command == 34U) {
        ad8088_set_address_b((uint16_t)(b + 5U));
        return 0;
    }
    rc = ad8088_read_bytes(AD8088_APPLE_WINDOW_BASE + b, tos_raw, 5U);
    if (rc != AD8088_XFER_OK) {
        return rc;
    }
    if (command == 35U) {
        ad8088_float_encode((double)ad8088_load_i32(tos_raw), tos_raw);
        return ad8088_write_bytes(AD8088_APPLE_WINDOW_BASE + b, tos_raw, 5U);
    }
    if (command == 36U) {
        double fixed = floor(ad8088_float_decode(tos_raw));
        int32_t integer;
        if (fixed > 2147483647.0) fixed = 2147483647.0;
        if (fixed < -2147483648.0) fixed = -2147483648.0;
        integer = (int32_t)fixed;
        ad8088_store_u32(tos_raw, (uint32_t)integer);
        tos_raw[4] = 0x9FU;
        return ad8088_write_bytes(AD8088_APPLE_WINDOW_BASE + b, tos_raw, 5U);
    }

    tos = ad8088_float_decode(tos_raw);
    result = tos;
    if (command >= 37U && command <= 40U) {
        rc = ad8088_read_bytes(AD8088_APPLE_WINDOW_BASE + b + 5U,
                               nos_raw, 5U);
        if (rc != AD8088_XFER_OK) {
            return rc;
        }
        nos = ad8088_float_decode(nos_raw);
        switch (command) {
        case 37U: result = nos + tos; break;
        case 38U: result = nos - tos; break;
        case 39U: result = nos * tos; break;
        default: result = nos / tos; break;
        }
        b = (uint16_t)(b + 5U);
    } else {
        switch (command) {
        case 41U: result = -tos; break;
        case 42U: result = log2(tos); break;
        case 43U: result = exp2(tos); break;
        case 44U: {
            rc = ad8088_read_bytes(AD8088_APPLE_WINDOW_BASE + b + 5U,
                                   nos_raw, 5U);
            if (rc != AD8088_XFER_OK) return rc;
            nos = ad8088_float_decode(nos_raw);
            result = pow(nos, tos);
            b = (uint16_t)(b + 5U);
            break;
        }
        case 45U: result = sin(tos); break;
        case 46U: result = cos(tos); break;
        case 47U: result = atan(tos); break;
        default: return -1;
        }
    }
    ad8088_float_encode(result, tos_raw);
    rc = ad8088_write_bytes(AD8088_APPLE_WINDOW_BASE + b, tos_raw, 5U);
    if (rc == AD8088_XFER_OK && b != original_b) {
        ad8088_set_address_b(b);
    }
    return rc;
}

static int ad8088_read_far_pointer(uint32_t address,
                                   uint16_t *offset,
                                   uint16_t *segment)
{
    uint8_t ref[4];
    const int rc = ad8088_read_bytes(address, ref, sizeof(ref));
    if (rc != AD8088_XFER_OK) {
        return rc;
    }
    *offset = (uint16_t)((uint16_t)ref[0] | ((uint16_t)ref[1] << 8));
    *segment = (uint16_t)((uint16_t)ref[2] | ((uint16_t)ref[3] << 8));
    return 0;
}

static void ad8088_copy_command_parameters(void)
{
    /* PROM contract: ports 1-15 appear in local RAM $001E-$002C. */
    memcpy(&g_machine.memory[0x1EU], &g_ports[1], 15U);
}

static int ad8088_start_user_command(uint8_t command,
                                     uint16_t *offset,
                                     uint16_t *segment)
{
    const uint8_t first = g_machine.memory[0x14U];
    uint8_t table_ref[4];
    uint32_t table;

    if (command < first || first < 48U) {
        return -1;
    }
    memcpy(table_ref, &g_machine.memory[0x15U], sizeof(table_ref));
    table = ad8088_reference_linear(table_ref) +
            ((uint32_t)(command - first) * 4U);
    return ad8088_read_far_pointer(table, offset, segment);
}

static void ad8088_start_call(uint16_t offset,
                              uint16_t segment,
                              uint8_t command,
                              uint8_t top_level,
                              uint8_t seq)
{
    ad8088_copy_command_parameters();
    ad8088_machine_start_far(&g_machine, offset, segment, command);
    g_monitor_state = AD_MONITOR_CALL;
    g_resume_sequence = (top_level == 0U) ? 1U : 0U;
    if (top_level != 0U) {
        REG_WRITE(APPLICARD_REG_AD_CONTROL,
                  AD8088_CONTROL_ACK(seq) |
                  AD8088_CONTROL_CLEAR_FLAG |
                  AD8088_CONTROL_SET_RUNNING);
    } else {
        REG_WRITE(APPLICARD_REG_AD_CONTROL,
                  AD8088_CONTROL_CLEAR_FLAG |
                  AD8088_CONTROL_SET_RUNNING);
    }
}

static int ad8088_dispatch_command(uint8_t command,
                                   uint8_t top_level,
                                   uint8_t seq)
{
    uint8_t table[10];
    uint16_t offset;
    uint16_t segment;
    const uint16_t b = ad8088_address_b();
    int rc;

    if (command == 0U) {
        g_command_count++;
        ad8088_machine_reset(&g_machine);
        ad8088_finish_command(top_level, seq);
        return AD8088_XFER_OK;
    }
    if (command >= 29U && command <= 32U) {
        rc = ad8088_integer_command(command);
        if (rc == AD8088_XFER_BLOCKED) return rc;
        g_command_count++;
        if (rc != AD8088_XFER_OK) g_error_count++;
        ad8088_finish_command(top_level, seq);
        return rc;
    }
    if (command >= 33U && command <= 47U) {
        rc = ad8088_float_command(command);
        if (rc == AD8088_XFER_BLOCKED) return rc;
        g_command_count++;
        if (rc != AD8088_XFER_OK) g_error_count++;
        ad8088_finish_command(top_level, seq);
        return rc;
    }
    if (command >= 48U && command <= 247U) {
        rc = ad8088_start_user_command(command, &offset, &segment);
        if (rc == AD8088_XFER_BLOCKED) return rc;
        g_command_count++;
        if (rc == AD8088_XFER_OK) {
            ad8088_start_call(offset, segment, command, top_level, seq);
        } else {
            g_error_count++;
            ad8088_finish_command(top_level, seq);
        }
        return rc;
    }
    if (command == 251U) { /* SEQUENCE */
        g_command_count++;
        g_sequence_addr = b;
        g_sequence_pairs = 0U;
        g_monitor_state = AD_MONITOR_SEQUENCE;
        g_resume_sequence = 0U;
        if (top_level != 0U) {
            REG_WRITE(APPLICARD_REG_AD_CONTROL, AD8088_CONTROL_ACK(seq));
        }
        return AD8088_XFER_OK;
    }
    if (command == 252U) { /* RANDOM */
        const uint32_t next_random = ((g_random << 1) |
            (((g_random >> 2) ^ (g_random >> 30)) & 1U)) & 0x7FFFFFFFU;
        const uint8_t random_byte = (uint8_t)next_random;
        rc = ad8088_write_bytes(AD8088_APPLE_WINDOW_BASE + b,
                                &random_byte, 1U);
        if (rc == AD8088_XFER_BLOCKED) return rc;
        g_command_count++;
        if (rc == AD8088_XFER_OK) g_random = next_random;
        else g_error_count++;
        ad8088_finish_command(top_level, seq);
        return rc;
    }
    if (command == 253U) { /* SET MEMORY */
        rc = ad8088_read_bytes(AD8088_APPLE_WINDOW_BASE + b, table, 7U);
        if (rc == AD8088_XFER_BLOCKED) return rc;
        g_command_count++;
        if (rc != AD8088_XFER_OK) {
            g_error_count++;
            ad8088_finish_command(top_level, seq);
            return rc;
        }
        g_bulk_dst = ad8088_reference_linear(table);
        g_bulk_remaining = (uint32_t)table[4] | ((uint32_t)table[5] << 8);
        g_bulk_value = table[6];
        ad8088_trace("8088 FD fill dst=%05lX cnt=%04lX val=%02lX\r\n",
                     g_bulk_dst, g_bulk_remaining, g_bulk_value);
        g_monitor_state = AD_MONITOR_FILL;
        g_resume_sequence = (top_level == 0U) ? 1U : 0U;
        ad8088_async_start(top_level, seq);
        return AD8088_XFER_OK;
    }
    if (command == 254U) { /* MOVE DATA */
        rc = ad8088_read_bytes(AD8088_APPLE_WINDOW_BASE + b, table, 10U);
        if (rc == AD8088_XFER_BLOCKED) return rc;
        g_command_count++;
        if (rc != AD8088_XFER_OK) {
            g_error_count++;
            ad8088_finish_command(top_level, seq);
            return rc;
        }
        g_bulk_dst = ad8088_reference_linear(&table[0]);
        g_bulk_src = ad8088_reference_linear(&table[4]);
        g_bulk_remaining = (uint32_t)table[8] | ((uint32_t)table[9] << 8);
        ad8088_trace("8088 FE move dst=%05lX src=%05lX cnt=%04lX\r\n",
                     g_bulk_dst, g_bulk_src, g_bulk_remaining);
        g_monitor_state = AD_MONITOR_COPY;
        g_resume_sequence = (top_level == 0U) ? 1U : 0U;
        ad8088_async_start(top_level, seq);
        return AD8088_XFER_OK;
    }
    if (command == 255U) { /* CALL */
        rc = ad8088_read_far_pointer(AD8088_APPLE_WINDOW_BASE + b,
                                     &offset, &segment);
        if (rc == AD8088_XFER_BLOCKED) return rc;
        g_command_count++;
        if (rc != AD8088_XFER_OK) {
            g_error_count++;
            ad8088_finish_command(top_level, seq);
            return rc;
        }
        ad8088_trace("8088 FF call %04lX:%04lX\r\n",
                     (uint32_t)segment, (uint32_t)offset, 0U);
        ad8088_start_call(offset, segment, command, top_level, seq);
        return AD8088_XFER_OK;
    }

    /* Graphics and MET-reserved commands are accepted as no-ops. This keeps
     * the mailbox live while making the unimplemented scope explicit. */
    g_command_count++;
    ad8088_finish_command(top_level, seq);
    return AD8088_XFER_OK;
}

static void ad8088_poll_sequence(void)
{
    uint8_t pair[2];
    int rc;

    if (g_sequence_pairs >= AD8088_SEQUENCE_PAIRS_MAX) {
        g_error_count++;
        ad8088_ready();
        return;
    }
    rc = ad8088_apple_read(NULL, g_sequence_addr, &pair[0]);
    if (rc == AD8088_XFER_BLOCKED) return;
    if (rc == AD8088_XFER_OK) {
        rc = ad8088_apple_read(NULL, (uint16_t)(g_sequence_addr + 1U),
                               &pair[1]);
    }
    if (rc == AD8088_XFER_BLOCKED) return;
    if (rc != AD8088_XFER_OK) {
        g_error_count++;
        ad8088_ready();
        return;
    }
    g_sequence_pairs++;
    g_sequence_addr = (uint16_t)(g_sequence_addr + 2U);
    pair[0] &= 0x0FU;
    g_ports[pair[0]] = pair[1];
    if (pair[0] >= 12U) {
        ad8088_publish_ports3();
    }
    if (pair[0] != 0U) {
        return;
    }
    if (pair[1] == 0U) {
        ad8088_ready();
    } else if (pair[1] == 251U) {
        g_sequence_addr = ad8088_address_b();
    } else {
        rc = ad8088_dispatch_command(pair[1], 0U, 0U);
        if (rc == AD8088_XFER_BLOCKED) {
            g_sequence_addr = (uint16_t)(g_sequence_addr - 2U);
            g_sequence_pairs--;
        }
    }
}

static void ad8088_poll_bulk(void)
{
    XTime start;
    XTime now;
    uint32_t moved = 0U;

    XTime_GetTime(&start);
    for (;;) {
        uint32_t count;
        int rc = AD8088_XFER_OK;
        const uint8_t src_apple = g_monitor_state == AD_MONITOR_COPY &&
            g_bulk_src >= AD8088_APPLE_WINDOW_BASE &&
            g_bulk_src < AD8088_APPLE_WINDOW_END;
        const uint8_t dst_apple =
            g_bulk_dst >= AD8088_APPLE_WINDOW_BASE &&
            g_bulk_dst < AD8088_APPLE_WINDOW_END;

        if (g_bulk_remaining == 0U) {
            ad8088_async_complete();
            return;
        }
        if ((REG_READ(APPLICARD_REG_AD_STATUS) &
             AD8088_STATUS_BUS_BLOCKED) != 0U) {
            return;
        }

        count = g_bulk_remaining;
        if (count > AD8088_BUS_BUFFER_BYTES) {
            count = AD8088_BUS_BUFFER_BYTES;
        }
        if (src_apple && count > AD8088_APPLE_WINDOW_END - g_bulk_src) {
            count = AD8088_APPLE_WINDOW_END - g_bulk_src;
        }
        if (dst_apple && count > AD8088_APPLE_WINDOW_END - g_bulk_dst) {
            count = AD8088_APPLE_WINDOW_END - g_bulk_dst;
        }

        /* Apple-to-Apple copies remain bytewise and forward. Other Apple
         * transfers use four-byte bursts, but release /DMA between bursts. */
        if (!(src_apple && dst_apple) && (src_apple || dst_apple)) {
            if (g_monitor_state == AD_MONITOR_FILL) {
                memset(g_bulk_stage, g_bulk_value, count);
            } else if (src_apple) {
                rc = ad8088_pl_bus_bulk((uint16_t)(g_bulk_src -
                    AD8088_APPLE_WINDOW_BASE), 1U, g_bulk_stage, count);
                if (rc == AD8088_XFER_OK) g_machine.apple_reads += count;
            } else {
                rc = ad8088_read_bytes(g_bulk_src, g_bulk_stage, count);
            }
            if (rc == AD8088_XFER_BLOCKED) return;
            if (rc != AD8088_XFER_OK) {
                ad8088_async_fail();
                return;
            }

            if (dst_apple) {
                rc = ad8088_pl_bus_bulk((uint16_t)(g_bulk_dst -
                    AD8088_APPLE_WINDOW_BASE), 0U, g_bulk_stage, count);
                if (rc == AD8088_XFER_OK) g_machine.apple_writes += count;
            } else {
                rc = ad8088_write_bytes(g_bulk_dst, g_bulk_stage, count);
            }
        } else {
            uint8_t value = g_bulk_value;
            count = 1U;
            if (g_monitor_state == AD_MONITOR_COPY) {
                rc = ad8088_read_bytes(g_bulk_src, &value, 1U);
            }
            if (rc == AD8088_XFER_OK) {
                rc = ad8088_write_bytes(g_bulk_dst, &value, 1U);
            }
        }

        if (rc == AD8088_XFER_BLOCKED) return;
        if (rc != AD8088_XFER_OK) {
            ad8088_async_fail();
            return;
        }

        g_bulk_src = (g_bulk_src + count) & AD8088_ADDRESS_MASK;
        g_bulk_dst = (g_bulk_dst + count) & AD8088_ADDRESS_MASK;
        g_bulk_remaining -= count;
        moved += count;

        if ((moved & 63U) == 0U && g_checkpoint != NULL) {
            g_checkpoint();
        }
        XTime_GetTime(&now);
        if ((uint64_t)(now - start) >=
            ad8088_us_to_ticks(AD8088_BULK_POLL_BUDGET_US)) {
            return;
        }
    }
}

static void ad8088_poll_call(void)
{
    XTime start;
    XTime now;

    if ((REG_READ(APPLICARD_REG_AD_STATUS) &
         AD8088_STATUS_BUS_BLOCKED) != 0U) {
        return;
    }
    XTime_GetTime(&start);
    do {
        ad8088_machine_exec(&g_machine, AD8088_SLICE_INSTRUCTIONS);
        if (g_machine.active == 0U || g_machine.bus_fault != 0U) {
            break;
        }
        if (g_checkpoint != NULL) {
            g_checkpoint();
        }
        XTime_GetTime(&now);
    } while ((uint64_t)(now - start) < ad8088_us_to_ticks(g_wall_cap_us) ||
             ad8088_paddle_extend_active(now, start) != 0U);

    if (g_machine.bus_fault != 0U) {
        if ((REG_READ(APPLICARD_REG_AD_STATUS) &
             (AD8088_STATUS_RESET_REQ | AD8088_STATUS_ABORT_REQ)) != 0U) {
            /* RES fell (or an abort was posted) inside this exec slice and
             * the PL rejected the in-flight window access. The program
             * dies with the machine; do not count a fault. */
            g_reset_stop_count++;
            ad8088_trace("8088 program stopped by reset at %04lX:%04lX\r\n",
                         (uint32_t)g_machine.cpu.segregs[regcs],
                         (uint32_t)g_machine.cpu.ip, 0U);
        } else {
            g_error_count++;
            g_last_err_cs = g_machine.cpu.segregs[regcs];
            g_last_err_ip = g_machine.cpu.ip;
            ad8088_trace("8088 bus fault killed program at %04lX:%04lX\r\n",
                         (uint32_t)g_last_err_cs, (uint32_t)g_last_err_ip,
                         0U);
        }
        g_machine.active = 0U;
    }
    if (g_machine.active == 0U) {
        REG_WRITE(APPLICARD_REG_AD_CONTROL, AD8088_CONTROL_CLEAR_RUNNING);
        ad8088_async_complete();
    }
}

void ad8088_service_init(uint32_t uart_base)
{
    g_uart_base = uart_base;
    ad8088_machine_init(&g_machine, (uint8_t *)AD8088_RAM_BASE, NULL,
                        ad8088_apple_read, ad8088_apple_write,
                        ad8088_cpu_port_read, ad8088_cpu_port_write);
    uart_puts(uart_base,
        "AD8088: clean-room monitor + 640K Plus RAM ready (8088 core idle)\r\n");
}

void ad8088_service_set_enabled(uint8_t enable)
{
    if (enable != 0U) {
        if (g_memory_initialized == 0U) {
            memset((uint8_t *)AD8088_RAM_BASE, 0, AD8088_BASE_RAM_END);
            memset((uint8_t *)AD8088_RAM_BASE + AD8088_PLUS_RAM_BASE,
                   0, AD8088_PLUS_RAM_END - AD8088_PLUS_RAM_BASE);
            memset((uint8_t *)AD8088_RAM_BASE + AD8088_PLUS_RAM_HIGH_BASE,
                   0, AD8088_PLUS_RAM_HIGH_END - AD8088_PLUS_RAM_HIGH_BASE);
            g_memory_initialized = 1U;
        }
        ad8088_machine_reset(&g_machine);
        g_monitor_state = AD_MONITOR_IDLE;
        g_resume_sequence = 0U;
        g_enabled = 1U;
        REG_WRITE(APPLICARD_REG_AD_CONTROL, AD8088_CONTROL_MONITOR_RESET);
    } else {
        g_enabled = 0U;
        g_machine.active = 0U;
        REG_WRITE(APPLICARD_REG_AD_CONTROL, AD8088_CONTROL_CANCEL_BUS);
    }
}

uint8_t ad8088_service_is_enabled(void)
{
    return g_enabled;
}

void ad8088_service_set_wall_cap(uint32_t us)
{
    if (us < 200U) us = 200U;
    if (us > 20000U) us = 20000U;
    g_wall_cap_us = us;
}

void ad8088_service_set_trace(uint8_t enable)
{
    g_trace = (enable != 0U) ? 1U : 0U;
}

void ad8088_service_set_checkpoint(void (*checkpoint)(void))
{
    g_checkpoint = checkpoint;
}

void ad8088_service_poll(void)
{
    uint32_t status;

    if (g_enabled == 0U ||
        (REG_READ(APPLICARD_REG_MODE) & 1U) != APPLICARD_MODE_AD8088) {
        return;
    }

    g_random = ((g_random << 1) |
               (((g_random >> 2) ^ (g_random >> 30)) & 1U)) & 0x7FFFFFFFU;
    status = REG_READ(APPLICARD_REG_AD_STATUS);

    if ((status & (AD8088_STATUS_RESET_REQ | AD8088_STATUS_ABORT_REQ)) != 0U) {
        ad8088_trace(((status & AD8088_STATUS_RESET_REQ) != 0U) ?
                         "8088 reset serviced (state=%lu)\r\n" :
                         "8088 abort serviced (state=%lu)\r\n",
                     (uint32_t)g_monitor_state, 0U, 0U);
        ad8088_machine_reset(&g_machine);
        g_monitor_state = AD_MONITOR_IDLE;
        g_resume_sequence = 0U;
        REG_WRITE(APPLICARD_REG_AD_CONTROL,
                  AD8088_CONTROL_CANCEL_BUS |
                  AD8088_CONTROL_MONITOR_RESET);
        g_reset_count++;
        return;
    }

    switch (g_monitor_state) {
    case AD_MONITOR_SEQUENCE:
        ad8088_poll_sequence();
        break;
    case AD_MONITOR_FILL:
    case AD_MONITOR_COPY:
        ad8088_poll_bulk();
        break;
    case AD_MONITOR_CALL:
        ad8088_poll_call();
        break;
    case AD_MONITOR_RECOVER:
        ad8088_poll_recover();
        break;
    case AD_MONITOR_IDLE:
    default:
        if ((status & AD8088_STATUS_CMD_PENDING) != 0U) {
            const uint8_t seq = (uint8_t)AD8088_STATUS_SEQ(status);
            ad8088_read_ports();
            (void)ad8088_dispatch_command(g_ports[0], 1U, seq);
        }
        break;
    }
}

void ad8088_service_uart_status(uint32_t uart_base)
{
    char line[192];
    const uint32_t status = REG_READ(APPLICARD_REG_AD_STATUS);
    XTime now;
    uint64_t ips = 0U;
    uint8_t ips_valid = 0U;

    XTime_GetTime(&now);
    if (g_status_tick != 0U && now > g_status_tick) {
        ips = ((g_machine.instructions - g_status_instructions) *
               (uint64_t)COUNTS_PER_SECOND) /
              (uint64_t)(now - g_status_tick);
        ips_valid = 1U;
    }
    (void)snprintf(line, sizeof(line),
        "8088: en=%u state=%u run=%u flag=%u cmd=%u PC=%04X:%04X SP=%04X\r\n",
        (unsigned)g_enabled, (unsigned)g_monitor_state,
        (unsigned)((status & AD8088_STATUS_RUNNING) != 0U),
        (unsigned)((status & AD8088_STATUS_FLAG) != 0U),
        (unsigned)g_ports[0],
        (unsigned)g_machine.cpu.segregs[regcs],
        (unsigned)g_machine.cpu.ip,
        (unsigned)g_machine.cpu.regs.wordregs[regsp]);
    uart_puts(uart_base, line);
    (void)snprintf(line, sizeof(line),
        "8088: ins=%llu ips=%s%llu apple-r=%lu apple-w=%lu signals=%lu resets=%lu reset-stops=%lu commands=%lu errors=%lu wall=%luus\r\n",
        (unsigned long long)g_machine.instructions,
        ips_valid != 0U ? "" : "n/a ",
        (unsigned long long)ips,
        (unsigned long)g_machine.apple_reads,
        (unsigned long)g_machine.apple_writes,
        (unsigned long)g_signal_count,
        (unsigned long)g_reset_count,
        (unsigned long)g_reset_stop_count,
        (unsigned long)g_command_count,
        (unsigned long)g_error_count,
        (unsigned long)g_wall_cap_us);
    uart_puts(uart_base, line);
    if (g_error_count != 0U) {
        /* kind: 1=PL DONE+ERROR, 2=wait timeout, 3=bulk index mismatch,
         * 4=blocked-stall timeout. status bit 11 = sticky blocked. */
        (void)snprintf(line, sizeof(line),
            "8088: lasterr kind=%u addr=$%04X status=%08lX pc=%04X:%04X\r\n",
            (unsigned)g_last_err_kind,
            (unsigned)g_last_err_addr,
            (unsigned long)g_last_err_status,
            (unsigned)g_last_err_cs,
            (unsigned)g_last_err_ip);
        uart_puts(uart_base, line);
    }
    g_status_tick = now;
    g_status_instructions = g_machine.instructions;
}

void ad8088_service_uart_paddle(uint32_t uart_base)
{
    char line[120];

    for (uint32_t p = 0U; p < 2U; ++p) {
        char *cursor = line;
        cursor += snprintf(cursor, sizeof(line),
                           "paddle%lu: n=%lu", (unsigned long)p,
                           (unsigned long)g_paddle_measurements[p]);
        for (uint32_t i = 0U; i < AD8088_PADDLE_RING; ++i) {
            /* Newest first. */
            const uint32_t slot =
                (g_paddle_ring_pos[p] + AD8088_PADDLE_RING - 1U - i) %
                AD8088_PADDLE_RING;
            cursor += snprintf(cursor,
                               (size_t)(line + sizeof(line) - cursor),
                               " %lu/%luus",
                               (unsigned long)g_paddle_ring[p][slot].count,
                               (unsigned long)g_paddle_ring[p][slot].us);
        }
        uart_puts(uart_base, line);
        uart_puts(uart_base, "\r\n");
    }
}

void ad8088_service_uart_dump(uint32_t uart_base, uint32_t addr,
                              uint32_t len)
{
    char line[96];
    if (len == 0U || len > 256U) len = 64U;
    for (uint32_t row = 0U; row < len; row += 16U) {
        char *p = line;
        p += snprintf(p, sizeof(line), "%05lX:",
                      (unsigned long)((addr + row) & AD8088_ADDRESS_MASK));
        for (uint32_t i = 0U; i < 16U && row + i < len; ++i) {
            uint8_t value = 0xFFU;
            (void)ad8088_machine_read(&g_machine, addr + row + i, &value);
            p += snprintf(p, (size_t)(line + sizeof(line) - p),
                          " %02X", value);
        }
        uart_puts(uart_base, line);
        uart_puts(uart_base, "\r\n");
    }
}
