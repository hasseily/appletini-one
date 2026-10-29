/* SPDX-License-Identifier: MIT */
#include "http_entropy.h"
#include "xadcps.h"
#include "xiltimer.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *test;
static uint32_t config = 0x1114U, interrupt_mask = 0x3FFU, interrupts = 0x200U;
static uint32_t control;
static uint16_t registers[128], original[3], pipeline, fifo[16];
static unsigned fifo_at, fifo_length, writes, commands, temp_reads, flushed, faulted;
static uint32_t random_state = 0x319A27D5U;
static uint64_t time_us, last_temperature;
static uint64_t late_at;
static unsigned late_delivered;

void XTime_GetTime(XTime *out) { *out = time_us++; }
uint32_t http_entropy_mmio_read(uint32_t address)
{
    if (late_at && time_us >= late_at) {
        fifo[(fifo_at+fifo_length++)%16U] = 0xBEEFU;
        late_at = 0U; ++late_delivered;
    }
    switch (address) {
    case 0xF8000168U: return !strcmp(test,"clock-off") ? 0x500U : 0x501U;
    case 0xF8000108U: return !strcmp(test,"clock-fast") ? 127U << 12 : 30U << 12;
    case 0xF800010CU: return !strcmp(test,"pll-unlocked") ? 0U : 4U;
    case 0xF8007100U: return config;
    case 0xF8007104U: return interrupts;
    case 0xF8007108U: return interrupt_mask;
    case 0xF800710CU:
        return 0x400U | (fifo_length ? 0U : 0x100U);
    case 0xF8007114U: {
        assert(fifo_length && "empty RDFIFO read would cause an APB bus fault");
        uint32_t value = fifo[fifo_at]; fifo_at = (fifo_at+1U)%16U; --fifo_length;
        return value;
    }
    case 0xF8007118U: return control;
    default: assert(!"unexpected MMIO read"); return 0U;
    }
}
void http_entropy_mmio_write(uint32_t address, uint32_t value)
{
    ++writes;
    switch (address) {
    case 0xF8007034U: assert(value == XADCPS_UNLK_VALUE); return;
    case 0xF8007100U: config = value; return;
    case 0xF8007104U: interrupts &= ~value; return;
    case 0xF8007108U: interrupt_mask = value; return;
    case 0xF8007118U:
        assert(!(value & 1U));
        if ((value & XADCPS_MCTL_RESET_MASK) && !(control & XADCPS_MCTL_RESET_MASK)) {
            ++flushed; /* Communication reset does not clear either FIFO. */
        }
        control = value;
        return;
    case 0xF8007110U: {
        unsigned operation = (value >> 26) & 15U, reg = (value >> 16) & 1023U;
        ++commands;
        assert(config & XADCPS_CFG_ENABLE_MASK);
        assert(!(control & XADCPS_MCTL_RESET_MASK));
        assert(fifo_length < 16U);
        if ((!strcmp(test,"persistent-timeout") && temp_reads == 100U) ||
            (!strcmp(test,"late-response") && !faulted && temp_reads == 100U) ||
            (!strcmp(test,"fifo-timeout") && !faulted && temp_reads == 100U) ||
            (!strcmp(test,"save-timeout") && !faulted && commands == 1U)) {
            if (!strcmp(test,"late-response")) late_at = time_us + 1050U;
            faulted = 1U; return; /* No response; bounded poll must fail. */
        }
        fifo[(fifo_at+fifo_length++)%16U] = pipeline;
        interrupts |= 0x300U;
        if (operation == 1U) {
            assert(reg < 128U);
            if (reg == 0U) {
                ++temp_reads;
                assert(time_us-last_temperature >= 125U);
                assert((registers[0x40] & 0xBF1FU) == 0U); /* TEMP/no averaging/event/mux */
                assert((registers[0x41] & 0xF000U) == 0x3000U);
                assert((registers[0x42] & 0xFF30U) == 0x400U);
                last_temperature = time_us;
                random_state ^= random_state << 13; random_state ^= random_state >> 17;
                random_state ^= random_state << 5;
                registers[0] = (uint16_t)((2048U+(random_state&3U)) << 4);
                if (!strcmp(test,"stale") || !strcmp(test,"por-stale")) registers[0] = 2048U << 4;
            }
            pipeline = registers[reg];
        } else if (operation == 2U) {
            assert(reg >= 0x40U && reg <= 0x42U);
            if ((!strcmp(test,"config-mismatch") && !faulted && reg == 0x40U) ||
                (!strcmp(test,"restore-mismatch") && temp_reads == HTTP_ENTROPY_SAMPLES && reg == 0x40U))
                faulted = 1U;
            else registers[reg] = (uint16_t)value;
            pipeline = 0U;
        } else { assert(operation == 0U); pipeline = 0U; }
        return;
    }
    default: assert(!"unexpected MMIO write");
    }
}

int main(int argc, char **argv)
{
    assert(argc == 2); test = argv[1];
    registers[0x40] = original[0] = 0xA110U;
    registers[0x41] = original[1] = 0x20F0U;
    registers[0x42] = original[2] = 0x0C00U;
    registers[0x3F] = 0x0200U;
    if (!strncmp(test,"por-",4)) control = 0x10U;
    if (!strcmp(test,"busy")) fifo_length = 1U;
    uint8_t out[HTTP_ENTROPY_BYTES];
    int result = http_entropy_seed(out);
    http_entropy_stats_t stats; http_entropy_status(&stats);
    assert(config == 0x1114U && interrupt_mask == 0x3FFU && interrupts == 0x200U);
    assert(control == (!strncmp(test,"por-",4) ? 0x10U : 0U));
    if (strcmp(test,"restore-mismatch") && strcmp(test,"persistent-timeout"))
        for (unsigned i = 0; i < 3U; ++i) assert(registers[0x40U+i] == original[i]);
    if (!strcmp(test,"varied") || !strcmp(test,"por-reset")) {
        assert(result == 0 && temp_reads == HTTP_ENTROPY_SAMPLES);
        assert(flushed == (!strcmp(test,"por-reset") ? 1U : 0U));
        assert(stats.elapsed_us >= 500000U && stats.elapsed_us < 1000000U);
    } else {
        int expected = HTTP_ENTROPY_CONFIG;
        if (!strcmp(test,"busy")) expected = HTTP_ENTROPY_UNAVAILABLE;
        if (!strcmp(test,"fifo-timeout") || !strcmp(test,"save-timeout") ||
            !strcmp(test,"late-response")) expected = HTTP_ENTROPY_TIMEOUT;
        if (!strcmp(test,"restore-mismatch") || !strcmp(test,"persistent-timeout")) expected = HTTP_ENTROPY_RESTORE;
        if (!strcmp(test,"stale") || !strcmp(test,"por-stale")) expected = HTTP_ENTROPY_REPEATED;
        assert(result == expected);
        for (unsigned i = 0; i < sizeof(out); ++i) assert(out[i] == 0U);
    }
    if (!strncmp(test,"clock-",6) || !strcmp(test,"pll-unlocked") || !strcmp(test,"busy")) assert(!writes);
    if (!strcmp(test,"late-response")) assert(late_delivered == 1U && !fifo_length);
    printf("MMIO %s result=%s samples=%lu elapsed=%lu commands=%u\n",test,
           http_entropy_result_name(result),(unsigned long)stats.samples,
           (unsigned long)stats.elapsed_us,commands);
    return 0;
}
