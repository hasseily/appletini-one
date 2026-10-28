#include "net_transport_test.h"

#ifdef APPLETINI_NET_TRANSPORT_TEST
#include <string.h>
#ifdef APPLETINI_TIMER_CALIBRATION
#include "xiltimer.h"
#endif

static uint8_t generation = 1U;
static uint32_t reset_sequence, read_count;

static void put16(uint8_t *p, uint16_t n)
{
    p[0] = (uint8_t)n;
    p[1] = (uint8_t)(n >> 8);
}

static void put32(uint8_t *p, uint32_t n)
{
    put16(p, (uint16_t)n);
    put16(p + 2, (uint16_t)(n >> 16));
}

void net_transport_test_reset(uint32_t sequence)
{
    if (++generation == 0U) generation = 1U;
    reset_sequence = sequence;
    read_count = 0U;
}

uint16_t net_transport_test_status(uint8_t selector, const uint8_t list[9],
                                   uint8_t *out, size_t capacity)
{
    uint32_t offset;
    uint16_t wanted, count;
    if (!list || !out || list[0] != 3U || list[1] != 0U) return 0U;
    if (selector == NET_TEST_DESCRIPTOR_SELECTOR) {
        if (capacity < NET_TEST_DESCRIPTOR_SIZE) return 0U;
#ifdef APPLETINI_TIMER_CALIBRATION
        /* Private bench request: bracket this ARM timestamp with Apple
         * timer samples. Normal descriptor/data requests stay unchanged. */
        if (list[5] == 0xA7U && list[6] == 0xCAU) {
            XTime ticks;
            XTime_GetTime(&ticks);
            memset(out, 0, NET_TEST_DESCRIPTOR_SIZE);
            memcpy(out, "ATTC", 4U);
            put32(out + 4, (uint32_t)ticks);
            put32(out + 8, (uint32_t)((uint64_t)ticks >> 32));
            put32(out + 12, (uint32_t)COUNTS_PER_SECOND);
            return NET_TEST_DESCRIPTOR_SIZE;
        }
#endif
        memset(out, 0, NET_TEST_DESCRIPTOR_SIZE);
        memcpy(out, "ATNT", 4U);
        out[5] = 1U; /* Test ABI 0.1, not an HTTP capability claim. */
        out[6] = NET_TEST_DESCRIPTOR_SIZE;
        out[7] = generation;
        put16(out + 8, 0x8000U); /* TEST_SOURCE only. */
        out[10] = 1U; /* Source ready. */
        put32(out + 12, NET_TEST_SOURCE_SIZE);
        put16(out + 16, NET_TEST_MIN_REPLY);
        put16(out + 18, NET_TEST_MAX_REPLY);
        put16(out + 20, NET_TEST_HEADER_SIZE);
        put16(out + 22, 1U); /* Pattern version. */
        put32(out + 24, reset_sequence);
        put32(out + 28, read_count);
        return NET_TEST_DESCRIPTOR_SIZE;
    }
    if (selector != NET_TEST_DATA_SELECTOR) return 0U;
    offset = (uint32_t)list[5] | ((uint32_t)list[6] << 8);
    wanted = (uint16_t)list[7] | ((uint16_t)list[8] << 8);
    if (wanted < NET_TEST_MIN_REPLY || wanted > NET_TEST_MAX_REPLY ||
        capacity < wanted) return 0U;
    count = (uint16_t)(wanted - NET_TEST_HEADER_SIZE);
    if (offset >= NET_TEST_SOURCE_SIZE) count = 0U;
    else if (count > NET_TEST_SOURCE_SIZE - offset)
        count = (uint16_t)(NET_TEST_SOURCE_SIZE - offset);
    out[0] = generation;
    out[1] = offset + count >= NET_TEST_SOURCE_SIZE ? 2U : 1U; /* END / MORE */
    put32(out + 2, offset);
    put16(out + 6, count);
    for (uint16_t i = 0U; i < count; ++i) {
        uint32_t at = offset + i;
        out[NET_TEST_HEADER_SIZE + i] =
            (uint8_t)((at * 73U + (at >> 8) * 19U + 0x5AU) ^ (at >> 4));
    }
    ++read_count;
    return (uint16_t)(NET_TEST_HEADER_SIZE + count);
}
#else
void net_transport_test_reset(uint32_t reset_sequence) { (void)reset_sequence; }
uint16_t net_transport_test_status(uint8_t selector, const uint8_t list[9],
                                   uint8_t *out, size_t capacity)
{
    (void)selector; (void)list; (void)out; (void)capacity;
    return 0U;
}
#endif
