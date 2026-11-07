/* Build a private XTulator profile for the Transporter. This profile enables
 * 186-class I/O instructions. It does not implement the NEC 0Fh group. */
#define CPU_V20
#define byteregtable pct_core_byteregtable
#define parity pct_core_parity
#define cpu_read pct_core_read
#define cpu_readw pct_core_readw
#define cpu_write pct_core_write
#define cpu_writew pct_core_writew
#define port_read pct_core_port_read
#define port_readw pct_core_port_readw
#define port_write pct_core_port_write
#define port_writew pct_core_port_writew
#define cpu_exec pct_core_exec
#define cpu_intcall pct_core_intcall
#define cpu_reset pct_core_reset
#define cpu_registerIntCallback pct_core_register_callback
#include "pc_transporter_machine.h"
#include "cpu.c"
#undef CPU_V20
#undef byteregtable
#undef parity
#undef cpu_read
#undef cpu_readw
#undef cpu_write
#undef cpu_writew
#undef port_read
#undef port_readw
#undef port_write
#undef port_writew
#undef cpu_exec
#undef cpu_intcall
#undef cpu_reset
#undef cpu_registerIntCallback

#include <string.h>

#define PCT_GUEST_ADDRESS_MASK 0xFFFFFU
#define PCT_BIOS_ALIAS_BASE    0xF0000U
#define PCT_ROUTED_PORTS       0x0400U
#define PCT_IO_WAIT            1U
#define PCT_IO_NOTIFY          2U

static uint8_t port_byte_read(pc_transporter_machine_t *machine, uint16_t port)
{
    uint8_t value;

    if (machine->ops->read_port != NULL &&
        machine->ops->read_port(machine->opaque, port, &value)) {
        return value;
    }
    if (port < PCT_ROUTED_PORTS) {
        return machine->ops->read_shared(machine->opaque, PCT_IO_DATA_BASE + port);
    }
    return 0xFFU;
}

static void port_byte_write(pc_transporter_machine_t *machine, uint16_t port,
                            uint8_t value)
{
    if (machine->ops->write_port != NULL &&
        machine->ops->write_port(machine->opaque, port, value)) {
        return;
    }
    if (port < PCT_ROUTED_PORTS) {
        machine->ops->write_shared(machine->opaque, PCT_IO_DATA_BASE + port, value);
    }
}

uint8_t pct_core_read(CPU_t *cpu, uint32_t address)
{
    pc_transporter_machine_t *machine = cpu->opaque;

    address &= PCT_GUEST_ADDRESS_MASK;
    if (address >= PCT_BIOS_ALIAS_BASE) {
        address = PCT_BIOS_RAM_BASE + (address - PCT_BIOS_ALIAS_BASE);
    }
    if (address >= PCT_IO_DATA_BASE && address < PCT_IO_CONTROL_BASE) {
        return port_byte_read(machine, (uint16_t)(address - PCT_IO_DATA_BASE));
    }
    if (address < PCT_RAM_BYTES) {
        return machine->ops->read_shared(machine->opaque, address);
    }
    return 0xFFU;
}

void pct_core_write(CPU_t *cpu, uint32_t address, uint8_t value)
{
    pc_transporter_machine_t *machine = cpu->opaque;

    address &= PCT_GUEST_ADDRESS_MASK;
    if (address >= PCT_IO_DATA_BASE && address < PCT_IO_CONTROL_BASE) {
        port_byte_write(machine, (uint16_t)(address - PCT_IO_DATA_BASE), value);
    } else if (address < PCT_RAM_BYTES) {
        machine->ops->write_shared(machine->opaque, address, value);
    }
}

static uint8_t routing_control(pc_transporter_machine_t *machine,
                               uint16_t port, uint8_t lanes, bool write)
{
    uint8_t control;

    if (port >= PCT_ROUTED_PORTS) {
        return 0U;
    }
    /* Use the observed table stride and the patent's direction bits. For a
     * word, request service or wait if either byte lane calls for it. */
    control = machine->ops->read_shared(machine->opaque,
                                        PCT_IO_CONTROL_BASE + (port & 0xFFFEU));
    if (lanes == PCT_IO_LANES_WORD) {
        control |= control >> 4;
    } else if (lanes == PCT_IO_LANE_HIGH) {
        control >>= 4;
    }
    if (!write) {
        control >>= 2;
    }
    return control & (PCT_IO_WAIT | PCT_IO_NOTIFY);
}

static bool port_cycle(pc_transporter_machine_t *machine, uint16_t port,
                       uint8_t lanes, bool write, uint16_t *data)
{
    const uint8_t control = routing_control(machine, port, lanes, write);
    const bool notify = (control & PCT_IO_NOTIFY) != 0U &&
                        machine->ops->submit_io != NULL;
    const bool wait = (control & PCT_IO_WAIT) != 0U;

    if (machine->reply_ready) {
        *data = machine->reply_data;
        machine->reply_ready = false;
        return true;
    }
    if (machine->request_pending && (wait || notify)) {
        machine->cpu.io_pending = 1U;
        return false;
    }

    if (write) {
        port_byte_write(machine, port, (uint8_t)*data);
        if (lanes == PCT_IO_LANES_WORD) {
            port_byte_write(machine, (uint16_t)(port + 1U), (uint8_t)(*data >> 8));
        }
    } else if (!notify || !wait) {
        *data = port_byte_read(machine, port);
        if (lanes == PCT_IO_LANES_WORD) {
            *data |= (uint16_t)port_byte_read(machine, (uint16_t)(port + 1U)) << 8;
        }
    }

    if (notify) {
        machine->request = (pc_transporter_io_request_t){
            .sequence = ++machine->next_sequence,
            .port = port,
            .data = (write || !wait) ? *data : 0U,
            .lanes = lanes,
            .write = write,
            .wait = wait,
        };
        machine->request_pending = true;
        machine->waiting_for_reply = wait;
        machine->cpu.io_pending = wait;
        machine->ops->submit_io(machine->opaque, &machine->request);
        return !wait;
    }
    return true;
}

static uint16_t port_transfer(CPU_t *cpu, uint16_t port, bool word, bool write,
                              uint16_t value)
{
    pc_transporter_machine_t *machine = cpu->opaque;
    const bool split = word && (port & 1U) != 0U;

    if (!machine->transfer_active) {
        machine->transfer_active = true;
        machine->transfer_part = 0U;
        machine->transfer_write_data = value;
        machine->transfer_read_data = 0U;
    }
    do {
        const uint16_t cycle_port = (uint16_t)(port + machine->transfer_part);
        const uint8_t shift = (uint8_t)(machine->transfer_part * 8U);
        const uint8_t lanes = word && !split ? PCT_IO_LANES_WORD :
                              (cycle_port & 1U) != 0U ? PCT_IO_LANE_HIGH :
                              PCT_IO_LANE_LOW;
        const uint16_t mask = lanes == PCT_IO_LANES_WORD ? 0xFFFFU : 0xFFU;
        uint16_t data = (machine->transfer_write_data >> shift) & mask;

        if (!port_cycle(machine, cycle_port, lanes, write, &data)) {
            return 0U;
        }
        machine->transfer_read_data |= (uint16_t)((data & mask) << shift);
        machine->transfer_part++;
    } while (split && machine->transfer_part == 1U);

    machine->transfer_active = false;
    return machine->transfer_read_data;
}

uint8_t pct_core_port_read(CPU_t *cpu, uint16_t port)
{
    return (uint8_t)port_transfer(cpu, port, false, false, 0U);
}

uint16_t pct_core_port_readw(CPU_t *cpu, uint16_t port)
{
    return port_transfer(cpu, port, true, false, 0U);
}

void pct_core_port_write(CPU_t *cpu, uint16_t port, uint8_t value)
{
    (void)port_transfer(cpu, port, false, true, value);
}

void pct_core_port_writew(CPU_t *cpu, uint16_t port, uint16_t value)
{
    (void)port_transfer(cpu, port, true, true, value);
}

static void invalid_opcode(CPU_t *cpu, uint8_t vector)
{
    pc_transporter_machine_t *machine = cpu->opaque;

    if (cpu->opcode != 0xCDU) { /* INT 6 is a valid instruction. */
        machine->illegal_instructions++;
        machine->last_illegal_address = (segbase(cpu->savecs) + cpu->saveip) &
                                        PCT_GUEST_ADDRESS_MASK;
        machine->last_illegal_opcode = cpu->opcode;
    }
    pct_core_register_callback(cpu, vector, NULL);
    pct_core_intcall(cpu, vector);
    pct_core_register_callback(cpu, vector, invalid_opcode);
}

static void reset_cpu(pc_transporter_machine_t *machine)
{
    memset(&machine->cpu, 0, sizeof(machine->cpu));
    machine->cpu.opaque = machine;
    pct_core_reset(&machine->cpu);
    pct_core_register_callback(&machine->cpu, 6U, invalid_opcode);
    machine->request_pending = false;
    machine->interrupt_asserted = false;
    machine->transfer_active = false;
    machine->waiting_for_reply = false;
    machine->reply_ready = false;
}

void pc_transporter_machine_init(pc_transporter_machine_t *machine,
                                 const pc_transporter_machine_ops_t *ops,
                                 void *opaque)
{
    memset(machine, 0, sizeof(*machine));
    machine->ops = ops;
    machine->opaque = opaque;
    pc_transporter_machine_reset(machine);
}

void pc_transporter_machine_reset(pc_transporter_machine_t *machine)
{
    reset_cpu(machine);
    machine->reset_held = true;
    machine->halt_held = true;
}

void pc_transporter_machine_set_control(pc_transporter_machine_t *machine,
                                        bool reset, bool halt)
{
    if (reset && !machine->reset_held) {
        reset_cpu(machine);
    }
    machine->reset_held = reset;
    machine->halt_held = halt;
}

void pc_transporter_machine_set_interrupt(pc_transporter_machine_t *machine,
                                          bool asserted, uint8_t vector)
{
    machine->interrupt_asserted = asserted;
    machine->interrupt_vector = vector;
}

void pc_transporter_machine_exec(pc_transporter_machine_t *machine,
                                 uint32_t budget)
{
    if (budget == 0U || machine->reset_held || machine->halt_held ||
        machine->cpu.io_pending != 0U) {
        return;
    }
    if (machine->interrupt_asserted) {
        if (machine->transfer_active) {
            /* Finish the held transfer before accepting a guest interrupt. */
            budget = 1U;
        } else if (machine->cpu.ifl != 0U) {
            const uint8_t vector = machine->interrupt_vector;
            void (*callback)(CPU_t *, uint8_t) = machine->cpu.int_callback[vector];

            machine->interrupt_asserted = false;
            machine->accepted_interrupts++;
            machine->cpu.hltstate = 0U;
            /* A hardware vector is not an unsupported-instruction trap. */
            pct_core_register_callback(&machine->cpu, vector, NULL);
            pct_core_intcall(&machine->cpu, vector);
            pct_core_register_callback(&machine->cpu, vector, callback);
        }
    }
    if (machine->cpu.hltstate == 0U) {
        pct_core_exec(&machine->cpu, budget);
    }
}

bool pc_transporter_machine_io_complete(pc_transporter_machine_t *machine,
                                        uint32_t sequence, uint16_t data)
{
    if (!machine->request_pending || sequence != machine->request.sequence) {
        return false;
    }
    if (machine->waiting_for_reply) {
        machine->reply_data = data;
        machine->reply_ready = true;
    }
    machine->waiting_for_reply = false;
    machine->request_pending = false;
    machine->cpu.io_pending = 0U;
    return true;
}
