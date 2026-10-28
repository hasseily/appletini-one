#ifndef NET_TRANSPORT_TEST_H
#define NET_TRANSPORT_TEST_H

#include <stddef.h>
#include <stdint.h>

/* Step-1 diagnostic only. These bytes do not freeze the step-2 network ABI. */
#define NET_TEST_DESCRIPTOR_SELECTOR 0x82U
#define NET_TEST_DATA_SELECTOR       0x83U
#define NET_TEST_DESCRIPTOR_SIZE     32U
#define NET_TEST_HEADER_SIZE         8U
#define NET_TEST_MIN_REPLY           9U
#define NET_TEST_MAX_REPLY           512U
#define NET_TEST_SOURCE_SIZE         32768UL

void net_transport_test_reset(uint32_t reset_sequence);
uint16_t net_transport_test_status(uint8_t selector, const uint8_t list[9],
                                   uint8_t *out, size_t capacity);

#endif
