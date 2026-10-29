/* SPDX-License-Identifier: MIT */
#include "http_entropy.h"
#include "mbedtls/sha256.h"
#include "mbedtls/platform_util.h"
#include <string.h>

#define SAMPLE_GAP_US 125U
/* Reserve 50ms of the two-second budget for bounded hardware restoration. */
#define TOTAL_TIMEOUT_US 1950000U
#define WINDOW_SAMPLES 256U
#define SYMBOL_LIMIT 240U
#define REPEAT_LIMIT 64U
#define PERIODS 16U
#define CYCLE_LIMIT 128U
#define SPIN_LIMIT 1000000U

static http_entropy_stats_t last = {.result = HTTP_ENTROPY_NOT_RUN};
static uint8_t attempted;

#ifdef HTTP_ENTROPY_TEST
#define now_us http_entropy_test_time_us
#define source_begin http_entropy_test_begin
#define source_sample http_entropy_test_sample
#define source_end http_entropy_test_end
#else
#include "xadcps.h"
#include "xparameters.h"
#include "xiltimer.h"

/* UG585, PS-XADC interface: EOC/EOS are PL signals, not PS status bits.
 * PS reads therefore use conservative conversion spacing, verified clock
 * and ADC settings, and changing raw codes as a liveness check. This is
 * not a hardware EOC observation. No PL XADC instance currently owns DRP.
 * https://docs.amd.com/r/en-US/ug585-zynq-7000-SoC-TRM/Command-and-Data-Packets
 * https://docs.amd.com/r/en-US/ug585-zynq-7000-SoC-TRM/Register-XADCIF_CFG-Details
 * https://docs.amd.com/r/en-US/ug480_7Series_XADC/Continuous-Sampling
 */
#define SLCR_ARM_PLL 0xF8000100U
#define SLCR_DDR_PLL 0xF8000104U
#define SLCR_IO_PLL 0xF8000108U
#define SLCR_PLL_STATUS 0xF800010CU
#define SLCR_PCAP_CLOCK 0xF8000168U
#define DEVCFG_UNLOCK 0xF8007034U
#define BOARD_REFERENCE_HZ 33333333U
#define FIFO_TIMEOUT_US 1000U

static struct {
    XAdcPs adc;
    uint32_t config, mask, interrupts, control;
    uint16_t cfr[3];
    uint8_t owned, serial, saved, changed;
} hardware;

static uint64_t now_us(void)
{
    XTime ticks;
    XTime_GetTime(&ticks);
    return (uint64_t)ticks * 1000000ULL / (uint64_t)(COUNTS_PER_SECOND);
}
static uint32_t reg_read(uint32_t offset)
{ return XAdcPs_ReadReg(hardware.adc.Config.BaseAddress, offset); }
static void reg_write(uint32_t offset, uint32_t value)
{ XAdcPs_WriteReg(hardware.adc.Config.BaseAddress, offset, value); }
static int wait_status(uint32_t mask, int set)
{
    uint64_t start = now_us();
    for (unsigned spin = 0; spin != SPIN_LIMIT; ++spin) {
        if (!!(reg_read(XADCPS_MSTS_OFFSET) & mask) == set) return 0;
        if (now_us() - start >= FIFO_TIMEOUT_US) break;
    }
    return HTTP_ENTROPY_TIMEOUT;
}
static int command(uint32_t value, uint16_t *answer)
{
    if (wait_status(XADCPS_MSTS_CFIFOF_MASK, 0)) return HTTP_ENTROPY_TIMEOUT;
    XAdcPs_WriteFifo(&hardware.adc, value);
    if (wait_status(XADCPS_MSTS_DFIFOE_MASK, 0)) return HTTP_ENTROPY_TIMEOUT;
    (void)XAdcPs_ReadFifo(&hardware.adc); /* Previous command's response. */
    if (wait_status(XADCPS_MSTS_CFIFOF_MASK, 0)) return HTTP_ENTROPY_TIMEOUT;
    XAdcPs_WriteFifo(&hardware.adc, 0U); /* NOP shifts this response out. */
    if (wait_status(XADCPS_MSTS_DFIFOE_MASK, 0)) return HTTP_ENTROPY_TIMEOUT;
    *answer = (uint16_t)XAdcPs_ReadFifo(&hardware.adc);
    return 0;
}
static int adc_read(unsigned address, uint16_t *value)
{
    return command(XADCPS_JTAG_CMD_READ_MASK |
                   ((uint32_t)address << XADCPS_JTAG_ADDR_SHIFT), value);
}
static int adc_write(unsigned address, uint16_t value)
{
    uint16_t ignored;
    return command(XADCPS_JTAG_CMD_WRITE_MASK |
                   ((uint32_t)address << XADCPS_JTAG_ADDR_SHIFT) | value, &ignored);
}
static int checked_clock(void)
{
    /* The board reference is Y116, 33.3333 MHz. Read clock configuration
     * rather than assuming the nominal 200 MHz PCAP clock. No clocks change. */
    uint32_t clock = Xil_In32(SLCR_PCAP_CLOCK), source = (clock >> 4) & 3U;
    uint32_t divisor = (clock >> 8) & 63U;
    uint32_t pll_at = source < 2U ? SLCR_IO_PLL : source == 2U ? SLCR_ARM_PLL : SLCR_DDR_PLL;
    uint32_t lock = source < 2U ? 4U : source == 2U ? 1U : 2U;
    uint32_t pll = Xil_In32(pll_at), feedback = (pll >> 12) & 127U;
    if (!(clock & 1U) || !divisor || !feedback || (pll & 0x13U) ||
        !(Xil_In32(SLCR_PLL_STATUS) & lock)) return HTTP_ENTROPY_CONFIG;
    uint32_t serial_hz = (uint32_t)(((uint64_t)BOARD_REFERENCE_HZ * feedback) / divisor / 8U);
    /* TCKRATE=2 divides PCAP by eight; CFR2=4 divides DCLK by four.
     * At >=1 MHz ADCCLK, conversion+acquisition+register transfer is well
     * below 125us (26 ADCCLK + 16 DCLK); the first result waits >=250us. */
    return serial_hz < 4000000U || serial_hz > 50000000U ? HTTP_ENTROPY_CONFIG : 0;
}
static int source_begin(void)
{
    uint16_t check;
    memset(&hardware, 0, sizeof(hardware));
#if defined(XPAR_XXADCPS_0_BASEADDR)
    hardware.adc.Config.BaseAddress = XPAR_XXADCPS_0_BASEADDR;
#elif defined(XPAR_XADCPS_0_BASEADDR)
    hardware.adc.Config.BaseAddress = XPAR_XADCPS_0_BASEADDR;
#else
    hardware.adc.Config.BaseAddress = 0xF8007100U;
#endif
    hardware.adc.IsReady = XIL_COMPONENT_IS_READY;
    if (checked_clock()) return HTTP_ENTROPY_CONFIG;
    /* Do not reset/flush another client's queued transactions. The app
     * calls once in the foreground; PS ENABLE temporarily excludes JTAG. */
    hardware.control = reg_read(XADCPS_MCTL_OFFSET);
    /* MCTL RESET is asserted at POR. It holds the PS communication channel
     * in reset, not the ADC registers. Bit0 is reserved in UG585 even though
     * the BSP names it FLUSH: never write it. */
    if (hardware.control & 1U) return HTTP_ENTROPY_UNAVAILABLE;
    uint32_t status = reg_read(XADCPS_MSTS_OFFSET);
    if ((status & (XADCPS_MSTS_CFIFOE_MASK | XADCPS_MSTS_DFIFOE_MASK)) !=
        (XADCPS_MSTS_CFIFOE_MASK | XADCPS_MSTS_DFIFOE_MASK)) return HTTP_ENTROPY_UNAVAILABLE;
    hardware.config = reg_read(XADCPS_CFG_OFFSET);
    hardware.mask = reg_read(XADCPS_INT_MASK_OFFSET);
    hardware.interrupts = reg_read(XADCPS_INT_STS_OFFSET);
    Xil_Out32(DEVCFG_UNLOCK, XADCPS_UNLK_VALUE);
    hardware.owned = 1U;
    reg_write(XADCPS_INT_MASK_OFFSET, hardware.mask | XADCPS_INTX_ALL_MASK);
    uint32_t config = (hardware.config & ~(XADCPS_CFG_TCKRATE_MASK | XADCPS_CFG_WEDGE_MASK |
                                          XADCPS_CFG_IGAP_MASK)) |
                      XADCPS_CFG_ENABLE_MASK | XADCPS_CFG_REDGE_MASK | 0x200U | 20U;
    reg_write(XADCPS_CFG_OFFSET, config);
    if (reg_read(XADCPS_CFG_OFFSET) != config) return HTTP_ENTROPY_CONFIG;
    reg_write(XADCPS_MCTL_OFFSET, hardware.control & ~XADCPS_MCTL_RESET_MASK);
    if (reg_read(XADCPS_MCTL_OFFSET) & XADCPS_MCTL_RESET_MASK) return HTTP_ENTROPY_CONFIG;
    hardware.serial = 1U;
    for (unsigned i = 0; i != 3; ++i)
        if (adc_read(XADCPS_CFR0_OFFSET + i, &hardware.cfr[i])) return HTTP_ENTROPY_TIMEOUT;
    hardware.saved = 1U;
    uint16_t cfr0 = hardware.cfr[0] & (uint16_t)~(XADCPS_CFR0_AVG_VALID_MASK |
                    XADCPS_CFR0_CAL_AVG_MASK | XADCPS_CFR0_MUX_MASK | XADCPS_CFR0_DU_MASK |
                    XADCPS_CFR0_EC_MASK | XADCPS_CFR0_ACQ_MASK | XADCPS_CFR0_CHANNEL_MASK);
    uint16_t cfr1 = (hardware.cfr[1] & (uint16_t)~XADCPS_CFR1_SEQ_VALID_MASK) |
                    XADCPS_CFR1_SEQ_SINGCHAN_MASK;
    uint16_t cfr2 = (hardware.cfr[2] & (uint16_t)~0xFF30U) | 0x0400U;
    hardware.changed = 1U;
    if (adc_write(XADCPS_CFR1_OFFSET, hardware.cfr[1] & (uint16_t)~XADCPS_CFR1_SEQ_VALID_MASK) ||
        adc_write(XADCPS_CFR0_OFFSET, cfr0) || adc_write(XADCPS_CFR2_OFFSET, cfr2) ||
        adc_write(XADCPS_CFR1_OFFSET, cfr1)) return HTTP_ENTROPY_TIMEOUT;
    const uint16_t expected[] = {cfr0, cfr1, cfr2};
    for (unsigned i = 0; i != 3; ++i) {
        if (adc_read(XADCPS_CFR0_OFFSET + i, &check)) return HTTP_ENTROPY_TIMEOUT;
        if (check != expected[i]) return HTTP_ENTROPY_CONFIG;
    }
    /* Internal reference selected by the board's grounded VREFP/VREFN. */
    if (adc_read(0x3FU, &check)) return HTTP_ENTROPY_TIMEOUT;
    return check & 0x0200U ? 0 : HTTP_ENTROPY_CONFIG;
}
static int source_sample(uint16_t *raw)
{ return adc_read(XADCPS_TEMP_OFFSET, raw); }
static int serial_sync(void)
{
    if (wait_status(XADCPS_MSTS_CFIFOE_MASK, 1)) return HTTP_ENTROPY_RESTORE;
    /* FIFO empty can precede completion of the packet being serialized.
     * At the checked >=4MHz serial rate a packet+gap is below 25us. Allow
     * 125us for that last packet, then drain its response before a NOP.
     * PS communication reset does NOT clear FIFOs (UG585 30.7.2). */
    uint64_t start = now_us();
    unsigned spin;
    for (spin = 0; spin != SPIN_LIMIT && now_us()-start < SAMPLE_GAP_US; ++spin) {}
    if (spin == SPIN_LIMIT) return HTTP_ENTROPY_RESTORE;
    unsigned count;
    for (count = 0; count != 16U && !(reg_read(XADCPS_MSTS_OFFSET) & XADCPS_MSTS_DFIFOE_MASK); ++count)
        (void)XAdcPs_ReadFifo(&hardware.adc);
    if (!(reg_read(XADCPS_MSTS_OFFSET) & XADCPS_MSTS_DFIFOE_MASK)) return HTTP_ENTROPY_RESTORE;
    uint16_t ignored;
    return command(0U, &ignored) ? HTTP_ENTROPY_RESTORE : 0;
}
static int source_end(void)
{
    int result = 0;
    if (!hardware.owned) return 0;
    if (hardware.serial) result = serial_sync();
    if (!result && hardware.saved && hardware.changed) {
        uint16_t check;
        if (adc_write(XADCPS_CFR1_OFFSET, hardware.cfr[1] & (uint16_t)~XADCPS_CFR1_SEQ_VALID_MASK) ||
            adc_write(XADCPS_CFR0_OFFSET, hardware.cfr[0]) ||
            adc_write(XADCPS_CFR2_OFFSET, hardware.cfr[2]) ||
            adc_write(XADCPS_CFR1_OFFSET, hardware.cfr[1])) result = HTTP_ENTROPY_RESTORE;
        if (!result)
            for (unsigned i = 0; i != 3; ++i) {
                if (adc_read(XADCPS_CFR0_OFFSET + i, &check) || check != hardware.cfr[i]) {
                    result = HTTP_ENTROPY_RESTORE; break;
                }
            }
    }
    /* Preserve pre-existing interrupt flags and any real alarm events. */
    uint32_t new_flags = reg_read(XADCPS_INT_STS_OFFSET) & ~hardware.interrupts &
                        (XADCPS_INTX_CFIFO_LTH_MASK | XADCPS_INTX_DFIFO_GTH_MASK);
    reg_write(XADCPS_INT_STS_OFFSET, new_flags);
    reg_write(XADCPS_MCTL_OFFSET, hardware.control);
    reg_write(XADCPS_CFG_OFFSET, hardware.config);
    reg_write(XADCPS_INT_MASK_OFFSET, hardware.mask);
    if (reg_read(XADCPS_CFG_OFFSET) != hardware.config ||
        reg_read(XADCPS_INT_MASK_OFFSET) != hardware.mask ||
        reg_read(XADCPS_MCTL_OFFSET) != hardware.control) result = HTTP_ENTROPY_RESTORE;
    hardware.owned = 0U;
    mbedtls_platform_zeroize(hardware.cfr, sizeof(hardware.cfr));
    return result;
}
#endif

void http_entropy_status(http_entropy_stats_t *out)
{ if (out) *out = last; }
const char *http_entropy_result_name(int result)
{
    switch (result) {
    case HTTP_ENTROPY_OK: return "ok-experimental";
    case HTTP_ENTROPY_UNAVAILABLE: return "unavailable";
    case HTTP_ENTROPY_TIMEOUT: return "timeout";
    case HTTP_ENTROPY_CONFIG: return "config";
    case HTTP_ENTROPY_REPEATED: return "repeated";
    case HTTP_ENTROPY_PROPORTION: return "proportion";
    case HTTP_ENTROPY_CYCLE: return "cycle";
    case HTTP_ENTROPY_HASH: return "hash";
    case HTTP_ENTROPY_USED: return "already-attempted";
    case HTTP_ENTROPY_RESTORE: return "restore";
    default: return "not-run";
    }
}
int http_entropy_seed(uint8_t out[HTTP_ENTROPY_BYTES])
{
    if (!out) return HTTP_ENTROPY_UNAVAILABLE;
    memset(out, 0, HTTP_ENTROPY_BYTES);
    if (attempted) return HTTP_ENTROPY_USED;
    attempted = 1U;
    memset(&last, 0, sizeof(last)); last.min_raw = 4095U;
    uint64_t start = now_us(), next = start;
    uint16_t history[PERIODS] = {0}, cycles[PERIODS] = {0}, histogram[16] = {0};
    uint16_t previous = 0, run = 0, window_changes = 0;
    uint8_t digest[32] = {0}, expanded[32] = {0};
    mbedtls_sha256_context hash;
    mbedtls_sha256_init(&hash);
    int result = source_begin();
    static const uint8_t domain[] = "Appletini experimental XADC raw12 v1";
    if (result) goto done;
    if (mbedtls_sha256_starts(&hash, 0) ||
        mbedtls_sha256_update(&hash, domain, sizeof(domain)-1U)) {
        result = HTTP_ENTROPY_HASH; goto done;
    }
    next = now_us() + 2U * SAMPLE_GAP_US;
    for (unsigned i = 0; i != HTTP_ENTROPY_SAMPLES; ++i) {
        unsigned spin;
        for (spin = 0; spin != SPIN_LIMIT; ++spin) {
            uint64_t time = now_us();
            if (time - start >= TOTAL_TIMEOUT_US) { result = HTTP_ENTROPY_TIMEOUT; goto done; }
            if (time >= next) break;
        }
        if (spin == SPIN_LIMIT) { result = HTTP_ENTROPY_TIMEOUT; goto done; }
        uint16_t word;
        if ((result = source_sample(&word)) != 0) goto done;
        next = now_us() + SAMPLE_GAP_US;
        /* Only the 12 MSBs are ADC data. Changing padding must not pass. */
        uint16_t raw = (word >> 4) & 4095U;
        ++last.samples;
        if (raw < last.min_raw) last.min_raw = raw;
        if (raw > last.max_raw) last.max_raw = raw;
        if (i && raw != previous) { ++last.changes; ++window_changes; run = 0; }
        if (++run > last.max_repeat) last.max_repeat = run;
        if (run > REPEAT_LIMIT) { result = HTTP_ENTROPY_REPEATED; goto done; }
        for (unsigned period = 1; period <= PERIODS && period <= i; ++period) {
            unsigned p = period-1U;
            cycles[p] = raw == history[(i-period) % PERIODS] ? (uint16_t)(cycles[p]+1U) : 0U;
            if (cycles[p] >= CYCLE_LIMIT) { result = HTTP_ENTROPY_CYCLE; goto done; }
        }
        history[i % PERIODS] = previous = raw;
        uint16_t count = ++histogram[raw & 15U];
        if (count > last.max_symbol_count) last.max_symbol_count = count;
        if ((i+1U) % WINDOW_SAMPLES == 0U) {
            if (count > SYMBOL_LIMIT || window_changes < 16U) {
                result = HTTP_ENTROPY_PROPORTION; goto done;
            }
            for (unsigned symbol = 0; symbol != 16; ++symbol)
                if (histogram[symbol] > SYMBOL_LIMIT) { result = HTTP_ENTROPY_PROPORTION; goto done; }
            memset(histogram, 0, sizeof(histogram)); window_changes = 0;
        }
        uint8_t sample[2] = {(uint8_t)raw, (uint8_t)(raw >> 8)};
        if (mbedtls_sha256_update(&hash, sample, sizeof(sample))) { result = HTTP_ENTROPY_HASH; goto done; }
    }
    if (last.changes < HTTP_ENTROPY_SAMPLES/8U) { result = HTTP_ENTROPY_PROPORTION; goto done; }
    if (mbedtls_sha256_finish(&hash, digest)) { result = HTTP_ENTROPY_HASH; goto done; }
    /* Domain-separated expansion to the seed record's 48 bytes. This adds
     * no entropy and makes no claim of 384 bits of input entropy. */
    for (uint8_t block = 0; block != 2U; ++block) {
        if (mbedtls_sha256_starts(&hash, 0) ||
            mbedtls_sha256_update(&hash, domain, sizeof(domain)-1U) ||
            mbedtls_sha256_update(&hash, &block, 1U) ||
            mbedtls_sha256_update(&hash, digest, sizeof(digest)) ||
            mbedtls_sha256_finish(&hash, expanded)) { result = HTTP_ENTROPY_HASH; goto done; }
        memcpy(out + block*32U, expanded, block ? 16U : 32U);
    }
done:
    {
        int restore = source_end();
        if (restore) result = HTTP_ENTROPY_RESTORE;
    }
    uint64_t elapsed = now_us()-start;
    last.elapsed_us = elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
    last.result = result;
    if (result) mbedtls_platform_zeroize(out, HTTP_ENTROPY_BYTES);
    mbedtls_sha256_free(&hash);
    mbedtls_platform_zeroize(digest, sizeof(digest));
    mbedtls_platform_zeroize(expanded, sizeof(expanded));
    mbedtls_platform_zeroize(history, sizeof(history));
    return result;
}
