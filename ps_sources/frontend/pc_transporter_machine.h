#ifndef PC_TRANSPORTER_MACHINE_H
#define PC_TRANSPORTER_MACHINE_H

#include <stdbool.h>
#include <stdint.h>

#include "cpu.h"
#include "pc_transporter_memory.h"

#define PCT_BIOS_RAM_BASE   0xA0000U
#define PCT_IO_DATA_BASE    0xBF000U
#define PCT_IO_CONTROL_BASE 0xBF400U
#define PCT_INT_VECTOR      0xBFFFEU

#define PCT_IO_LANE_LOW     1U
#define PCT_IO_LANE_HIGH    2U
#define PCT_IO_LANES_WORD   3U

typedef struct {
    uint32_t sequence;
    uint16_t port;
    /* Byte data is in bits 7:0, even when lanes is PCT_IO_LANE_HIGH. */
    uint16_t data;
    uint8_t lanes;
    bool write;
    bool wait;
} pc_transporter_io_request_t;

typedef struct {
    uint8_t (*read_shared)(void *opaque, uint32_t offset);
    void (*write_shared)(void *opaque, uint32_t offset, uint8_t value);
    /* Return true if a device or the Apple port window handles this byte. */
    bool (*read_port)(void *opaque, uint16_t port, uint8_t *value);
    bool (*write_port)(void *opaque, uint16_t port, uint8_t value);
    /* Post one request. The owner must acknowledge it outside this call. */
    void (*submit_io)(void *opaque, const pc_transporter_io_request_t *request);
    /* Optional little-endian transfers for aligned words in shared RAM. */
    uint16_t (*read_shared_word)(void *opaque, uint32_t offset);
    void (*write_shared_word)(void *opaque, uint32_t offset, uint16_t value);
    /* Optional little-endian instruction fetch from aligned conventional RAM. */
    uint32_t (*read_shared_dword)(void *opaque, uint32_t offset);
} pc_transporter_machine_ops_t;

typedef struct {
    CPU_t cpu;
    const pc_transporter_machine_ops_t *ops;
    void *opaque;
    uint32_t (*read_code_dword)(void *opaque, uint32_t offset);
    uint32_t code_fetch_block;
    uint32_t code_fetch_data;
    pc_transporter_io_request_t request;
    uint32_t next_sequence;
    bool request_pending;
    bool reset_held;
    bool halt_held;
    bool interrupt_asserted;
    uint8_t interrupt_vector;
    uint32_t accepted_interrupts;
    bool transfer_active;
    bool waiting_for_reply;
    bool reply_ready;
    uint8_t transfer_part;
    uint16_t transfer_write_data;
    uint16_t transfer_read_data;
    uint16_t reply_data;
    uint32_t illegal_instructions;
    uint32_t last_illegal_address;
    uint8_t last_illegal_opcode;
    uint16_t *ems_memory;
    uint16_t ems_page[PCT_EMS_WINDOWS];
    uint8_t ems_selected;
    uint8_t ems_bank_low;
} pc_transporter_machine_t;

/* Keep ops and opaque valid for the lifetime of machine. */
void pc_transporter_machine_init(pc_transporter_machine_t *machine,
                                 const pc_transporter_machine_ops_t *ops,
                                 void *opaque);
void pc_transporter_machine_set_maximum_speed(pc_transporter_machine_t *machine,
                                              bool maximum);
/* Attach PCT_EMS_BYTES of CPU-owned, little-endian backing or NULL. */
void pc_transporter_machine_set_ems_memory(pc_transporter_machine_t *machine,
                                           uint16_t *memory);
/* Cancel any request, unmap EMS and hold RESET and HALT. RAM stays intact. */
void pc_transporter_machine_reset(pc_transporter_machine_t *machine);
void pc_transporter_machine_set_control(pc_transporter_machine_t *machine,
                                        bool reset, bool halt);
void pc_transporter_machine_set_interrupt(pc_transporter_machine_t *machine,
                                          bool asserted, uint8_t vector);
void pc_transporter_machine_exec(pc_transporter_machine_t *machine,
                                 uint32_t budget);
/* Ignore stale acknowledgments. Data uses the same layout as request.data. */
bool pc_transporter_machine_io_complete(pc_transporter_machine_t *machine,
                                        uint32_t sequence, uint16_t data);

#endif /* PC_TRANSPORTER_MACHINE_H */
