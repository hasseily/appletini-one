/* ALF AD8088 Plus machine model: 8088 CPU, fully expanded RAM map, Apple
 * shared-memory window, and the two processor-side mailbox ports. */

#ifndef AD8088_MACHINE_H
#define AD8088_MACHINE_H

#include <stdint.h>

#include "cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AD8088_ADDRESS_MASK          0x000FFFFFU
#define AD8088_BASE_RAM_END          0x00010000U
#define AD8088_APPLE_WINDOW_BASE     0x00010000U
#define AD8088_APPLE_WINDOW_END      0x00020000U
#define AD8088_PLUS_RAM_BASE         0x00020000U
#define AD8088_PLUS_RAM_END          0x00030000U
#define AD8088_PLUS_RAM_HIGH_BASE    0x00040000U
#define AD8088_PLUS_RAM_HIGH_END     0x000C0000U
/* The original AD128K range remains a compatible subset of Plus RAM. */
#define AD8088_AD128K_BASE           0x00040000U
#define AD8088_AD128K_END            0x00060000U
#define AD8088_MEMORY_BYTES          0x00100000U

/* Apple-window callbacks distinguish a hard transfer fault from a temporary
 * bus-owner conflict. The monitor may retry BLOCKED work without telling the
 * Apple that the command finished. */
#define AD8088_XFER_OK                0
#define AD8088_XFER_ERROR            (-1)
#define AD8088_XFER_BLOCKED          (-2)

typedef int (*ad8088_apple_read_fn)(void *opaque, uint16_t addr,
                                    uint8_t *value);
typedef int (*ad8088_apple_write_fn)(void *opaque, uint16_t addr,
                                     uint8_t value);
typedef uint8_t (*ad8088_port_read_fn)(void *opaque, uint8_t port);
typedef void (*ad8088_port_write_fn)(void *opaque, uint8_t port,
                                     uint8_t value);

typedef struct {
    CPU_t cpu;
    uint8_t *memory;
    void *opaque;
    ad8088_apple_read_fn apple_read;
    ad8088_apple_write_fn apple_write;
    ad8088_port_read_fn port_read;
    ad8088_port_write_fn port_write;
    uint8_t active;
    uint8_t return_seen;
    uint8_t bus_fault;
    uint64_t instructions;
    uint32_t apple_reads;
    uint32_t apple_writes;
} ad8088_machine_t;

void ad8088_machine_init(ad8088_machine_t *machine,
                         uint8_t *memory,
                         void *opaque,
                         ad8088_apple_read_fn apple_read,
                         ad8088_apple_write_fn apple_write,
                         ad8088_port_read_fn port_read,
                         ad8088_port_write_fn port_write);
void ad8088_machine_reset(ad8088_machine_t *machine);
void ad8088_machine_start_far(ad8088_machine_t *machine,
                              uint16_t offset,
                              uint16_t segment,
                              uint8_t al);
void ad8088_machine_exec(ad8088_machine_t *machine, uint32_t instructions);

int ad8088_machine_read(ad8088_machine_t *machine,
                        uint32_t address,
                        uint8_t *value);
int ad8088_machine_write(ad8088_machine_t *machine,
                         uint32_t address,
                         uint8_t value);

#ifdef __cplusplus
}
#endif

#endif /* AD8088_MACHINE_H */
