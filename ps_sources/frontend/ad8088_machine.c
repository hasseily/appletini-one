/* ALF AD8088 machine model -- see ad8088_machine.h. */

#include "ad8088_machine.h"

#include <string.h>

/* A far return from code called by the clean-room monitor lands here. The
 * reserved address returns HLT and marks the call complete; no copyrighted
 * AD8088 PROM image is required. */
#define AD8088_RETURN_SEGMENT 0x3000U
#define AD8088_RETURN_OFFSET  0xFFF0U
#define AD8088_RETURN_LINEAR  0x0003FFF0U
#define AD8088_INITIAL_SP     0x07FAU

static ad8088_machine_t *ad8088_from_cpu(CPU_t *cpu)
{
    return (cpu != NULL) ? (ad8088_machine_t *)cpu->opaque : NULL;
}

static int ad8088_is_local_ram(uint32_t address)
{
    return address < AD8088_BASE_RAM_END ||
           (address >= AD8088_PLUS_RAM_BASE &&
            address < AD8088_PLUS_RAM_END) ||
           (address >= AD8088_PLUS_RAM_HIGH_BASE &&
            address < AD8088_PLUS_RAM_HIGH_END);
}

int ad8088_machine_read(ad8088_machine_t *machine,
                        uint32_t address,
                        uint8_t *value)
{
    address &= AD8088_ADDRESS_MASK;
    if (machine == NULL || value == NULL) {
        return -1;
    }
    if (address == AD8088_RETURN_LINEAR) {
        machine->return_seen = 1U;
        *value = 0xF4U; /* HLT */
        return 0;
    }
    if (ad8088_is_local_ram(address)) {
        *value = machine->memory[address];
        return 0;
    }
    if (address >= AD8088_APPLE_WINDOW_BASE &&
        address < AD8088_APPLE_WINDOW_END && machine->apple_read != NULL) {
        const int rc = machine->apple_read(machine->opaque,
            (uint16_t)(address - AD8088_APPLE_WINDOW_BASE), value);
        if (rc == AD8088_XFER_OK) {
            machine->apple_reads++;
            return AD8088_XFER_OK;
        }
        machine->bus_fault = (rc == AD8088_XFER_BLOCKED) ? 2U : 1U;
        *value = 0xFFU;
        return rc;
    }

    /* The $30000-$3FFFF gap and unpopulated ROM ranges float high. The
     * original 4 KB PROM is represented by the high-level monitor in
     * ad8088_service.c. */
    *value = 0xFFU;
    return 0;
}

int ad8088_machine_write(ad8088_machine_t *machine,
                         uint32_t address,
                         uint8_t value)
{
    address &= AD8088_ADDRESS_MASK;
    if (machine == NULL) {
        return -1;
    }
    if (ad8088_is_local_ram(address)) {
        machine->memory[address] = value;
        return 0;
    }
    if (address >= AD8088_APPLE_WINDOW_BASE &&
        address < AD8088_APPLE_WINDOW_END && machine->apple_write != NULL) {
        const int rc = machine->apple_write(machine->opaque,
            (uint16_t)(address - AD8088_APPLE_WINDOW_BASE), value);
        if (rc == AD8088_XFER_OK) {
            machine->apple_writes++;
            return AD8088_XFER_OK;
        }
        machine->bus_fault = (rc == AD8088_XFER_BLOCKED) ? 2U : 1U;
        return rc;
    }
    return 0; /* writes to absent/reserved memory are ignored */
}

/* Callback symbols consumed by the vendored XTulator CPU core. */
uint8_t cpu_read(CPU_t *cpu, uint32_t address)
{
    ad8088_machine_t *machine = ad8088_from_cpu(cpu);
    uint8_t value = 0xFFU;
    (void)ad8088_machine_read(machine, address, &value);
    return value;
}

void cpu_write(CPU_t *cpu, uint32_t address, uint8_t value)
{
    (void)ad8088_machine_write(ad8088_from_cpu(cpu), address, value);
}

uint8_t port_read(CPU_t *cpu, uint16_t port)
{
    ad8088_machine_t *machine = ad8088_from_cpu(cpu);
    if (machine == NULL || machine->port_read == NULL) {
        return 0xFFU;
    }
    return machine->port_read(machine->opaque, (uint8_t)port);
}

uint16_t port_readw(CPU_t *cpu, uint16_t port)
{
    const uint16_t lo = port_read(cpu, port);
    const uint16_t hi = port_read(cpu, (uint16_t)(port + 1U));
    return (uint16_t)(lo | (hi << 8));
}

void port_write(CPU_t *cpu, uint16_t port, uint8_t value)
{
    ad8088_machine_t *machine = ad8088_from_cpu(cpu);
    if (machine != NULL && machine->port_write != NULL) {
        machine->port_write(machine->opaque, (uint8_t)port, value);
    }
}

void port_writew(CPU_t *cpu, uint16_t port, uint16_t value)
{
    port_write(cpu, port, (uint8_t)value);
    port_write(cpu, (uint16_t)(port + 1U), (uint8_t)(value >> 8));
}

void ad8088_machine_init(ad8088_machine_t *machine,
                         uint8_t *memory,
                         void *opaque,
                         ad8088_apple_read_fn apple_read,
                         ad8088_apple_write_fn apple_write,
                         ad8088_port_read_fn port_read_cb,
                         ad8088_port_write_fn port_write_cb)
{
    if (machine == NULL) {
        return;
    }
    memset(machine, 0, sizeof(*machine));
    machine->memory = memory;
    machine->opaque = opaque;
    machine->apple_read = apple_read;
    machine->apple_write = apple_write;
    machine->port_read = port_read_cb;
    machine->port_write = port_write_cb;
    ad8088_machine_reset(machine);
}

void ad8088_machine_reset(ad8088_machine_t *machine)
{
    if (machine == NULL) {
        return;
    }
    memset(&machine->cpu, 0, sizeof(machine->cpu));
    machine->cpu.opaque = machine;
    cpu_reset(&machine->cpu);
    machine->active = 0U;
    machine->return_seen = 0U;
    machine->bus_fault = 0U;
}

void ad8088_machine_start_far(ad8088_machine_t *machine,
                              uint16_t offset,
                              uint16_t segment,
                              uint8_t al)
{
    if (machine == NULL) {
        return;
    }
    ad8088_machine_reset(machine);

    /* RETF pops IP followed by CS. The monitor stack lives at the top of
     * on-board RAM, matching the manual's SS=0, stack-below-$0800 contract. */
    machine->memory[AD8088_INITIAL_SP + 0U] =
        (uint8_t)(AD8088_RETURN_OFFSET & 0xFFU);
    machine->memory[AD8088_INITIAL_SP + 1U] =
        (uint8_t)(AD8088_RETURN_OFFSET >> 8);
    machine->memory[AD8088_INITIAL_SP + 2U] =
        (uint8_t)(AD8088_RETURN_SEGMENT & 0xFFU);
    machine->memory[AD8088_INITIAL_SP + 3U] =
        (uint8_t)(AD8088_RETURN_SEGMENT >> 8);

    machine->cpu.segregs[regcs] = segment;
    machine->cpu.segregs[regss] = 0U;
    machine->cpu.segregs[regds] = 0U;
    machine->cpu.segregs[reges] = 0U;
    machine->cpu.ip = offset;
    machine->cpu.regs.wordregs[regsp] = AD8088_INITIAL_SP;
    machine->cpu.regs.byteregs[regal] = al;
    machine->cpu.hltstate = 0U;
    machine->active = 1U;
}

void ad8088_machine_exec(ad8088_machine_t *machine, uint32_t instructions)
{
    uint64_t before;

    if (machine == NULL || machine->active == 0U || instructions == 0U) {
        return;
    }
    before = machine->cpu.totalexec;
    cpu_exec(&machine->cpu, instructions);
    machine->instructions += machine->cpu.totalexec - before;
    if (machine->return_seen != 0U) {
        machine->active = 0U;
    }
}
