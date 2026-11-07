#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "pc_transporter_machine.h"

#define PROGRAM 0x0100U
#define RESET_VECTOR_RAM (PCT_BIOS_RAM_BASE + 0xFFF0U)
#define SLICE 64U

static uint8_t ram[PCT_RAM_BYTES];
static pc_transporter_machine_t machine;
static pc_transporter_io_request_t last_request;
static unsigned requests;
static unsigned wrap_port_writes;

static uint8_t shared_read(void *opaque, uint32_t offset)
{
    (void)opaque;
    assert(offset < sizeof(ram));
    return ram[offset];
}

static void shared_write(void *opaque, uint32_t offset, uint8_t value)
{
    (void)opaque;
    assert(offset < sizeof(ram));
    ram[offset] = value;
}

static bool device_write(void *opaque, uint16_t port, uint8_t value)
{
    (void)opaque;
    if (port != 0xFFFFU) {
        return false;
    }
    assert(value == 0x11U);
    wrap_port_writes++;
    return true;
}

static void submit_io(void *opaque, const pc_transporter_io_request_t *request)
{
    (void)opaque;
    last_request = *request;
    requests++;
}

static const pc_transporter_machine_ops_t ops = {
    .read_shared = shared_read,
    .write_shared = shared_write,
    .write_port = device_write,
    .submit_io = submit_io,
};

static void start_program(const uint8_t *code, size_t length)
{
    /* Jump to 0000:0100 through the downloaded BIOS alias. */
    static const uint8_t reset_jump[] = {0xEA, 0x00, 0x01, 0x00, 0x00};

    memset(ram, 0, sizeof(ram));
    memcpy(ram + PROGRAM, code, length);
    memcpy(ram + RESET_VECTOR_RAM, reset_jump, sizeof(reset_jump));
    requests = 0U;
    wrap_port_writes = 0U;
    pc_transporter_machine_reset(&machine);
    pc_transporter_machine_set_control(&machine, false, false);
}

static void expect_request(unsigned count, uint16_t port, uint8_t lanes,
                           bool write, bool wait)
{
    assert(requests == count);
    assert(last_request.port == port);
    assert(last_request.lanes == lanes);
    assert(last_request.write == write);
    assert(last_request.wait == wait);
    assert(machine.request_pending);
}

static void test_memory_and_reset_vector(void)
{
    static const uint8_t code[] = {
        0xB8, 0x00, 0xB8,       /* MOV AX,B800 */
        0x8E, 0xD8,             /* MOV DS,AX */
        0xC6, 0x06, 0, 0, 0x41, /* MOV BYTE [0],41 */
        0xB8, 0x00, 0xF0,       /* MOV AX,F000 */
        0x8E, 0xD8,             /* MOV DS,AX */
        0xC6, 0x06, 0xF0, 0xFF, 0x00, /* Write to the read-only alias */
        0xB8, 0x00, 0xC0,       /* MOV AX,C000 */
        0x8E, 0xD8,             /* MOV DS,AX */
        0xA0, 0, 0,             /* MOV AL,[0] from unmapped memory */
        0xF4                    /* HLT */
    };

    start_program(code, sizeof(code));
    pc_transporter_machine_set_control(&machine, false, true);
    pc_transporter_machine_exec(&machine, SLICE);
    assert(machine.cpu.ip == 0U);
    pc_transporter_machine_set_control(&machine, false, false);
    pc_transporter_machine_exec(&machine, SLICE);
    assert(machine.cpu.hltstate);
    assert(machine.cpu.regs.byteregs[regal] == 0xFFU);
    assert(ram[PCT_CGA_RAM_BASE] == 0x41U);
    assert(ram[RESET_VECTOR_RAM] == 0xEAU);
    assert(requests == 0U);
}

static void test_notification_and_prior_wait(void)
{
    static const uint8_t code[] = {
        0xBA, 0x34, 0x01, /* MOV DX,0134 */
        0xB0, 0x11,       /* MOV AL,11 */
        0xEE,             /* OUT DX,AL */
        0xBA, 0x36, 0x01, /* MOV DX,0136 */
        0xEC,             /* IN AL,DX */
        0xA2, 0x00, 0x02, /* MOV [0200],AL */
        0xBA, 0x38, 0x01, /* MOV DX,0138 */
        0xEC,             /* IN AL,DX */
        0xF4
    };

    start_program(code, sizeof(code));
    ram[PCT_IO_CONTROL_BASE + 0x134U] = 0x02U; /* Write notify. */
    ram[PCT_IO_CONTROL_BASE + 0x136U] = 0x03U; /* Write flags do not trap read. */
    ram[PCT_IO_CONTROL_BASE + 0x138U] = 0x04U; /* Read waits only for prior service. */
    ram[PCT_IO_DATA_BASE + 0x136U] = 0x31U;
    ram[PCT_IO_DATA_BASE + 0x138U] = 0x42U;
    pc_transporter_machine_exec(&machine, SLICE);
    expect_request(1U, 0x134U, PCT_IO_LANE_LOW, true, false);
    assert(last_request.data == 0x11U);
    assert(ram[PCT_IO_DATA_BASE + 0x134U] == 0x11U);
    assert(ram[0x200U] == 0x31U);
    assert(machine.cpu.regs.byteregs[regal] == 0x31U);
    assert(machine.cpu.io_pending);
    assert(!pc_transporter_machine_io_complete(&machine, last_request.sequence - 1U, 0U));
    pc_transporter_machine_exec(&machine, SLICE);
    assert(machine.cpu.io_pending && requests == 1U);
    assert(pc_transporter_machine_io_complete(&machine, last_request.sequence, 0U));
    assert(!pc_transporter_machine_io_complete(&machine, last_request.sequence, 0U));
    pc_transporter_machine_exec(&machine, SLICE);
    assert(machine.cpu.hltstate && requests == 1U);
    assert(machine.cpu.regs.byteregs[regal] == 0x42U);
}

static void test_cpu_profile(void)
{
    static const uint8_t code[] = {
        0xB8, 0x03, 0x00, /* MOV AX,3 */
        0x62, 0x06, 0x34, 0x12, /* BOUND AX,[1234] */
        0x0F,             /* Unsupported NEC instruction prefix */
        0xF4
    };

    start_program(code, sizeof(code));
    ram[0x1234U] = 2U;
    ram[0x1236U] = 4U;
    ram[5U * 4U + 1U] = 0x03U;
    ram[6U * 4U + 1U] = 0x04U;
    ram[0x300U] = 0xF4U;
    ram[0x400U] = 0xF4U;
    const uint32_t initial_illegal = machine.illegal_instructions;
    pc_transporter_machine_exec(&machine, SLICE);
    assert(machine.cpu.hltstate && machine.cpu.ip == 0x401U);
    assert(machine.illegal_instructions == initial_illegal + 1U);
    assert(machine.last_illegal_address == PROGRAM + 7U);
    assert(machine.last_illegal_opcode == 0x0FU);
}

static void test_word_io(void)
{
    static const uint8_t aligned_read[] = {0xBA, 0x34, 0x01, 0xED, 0xF4};
    static const uint8_t wrapped_write[] = {
        0xBA, 0xFF, 0xFF, /* MOV DX,FFFF */
        0xB8, 0x11, 0x22, /* MOV AX,2211 */
        0xEF,             /* OUT DX,AX */
        0xF4
    };

    start_program(aligned_read, sizeof(aligned_read));
    ram[PCT_IO_CONTROL_BASE + 0x134U] = 0xC0U; /* Only the odd read lane traps. */
    pc_transporter_machine_exec(&machine, SLICE);
    expect_request(1U, 0x134U, PCT_IO_LANES_WORD, false, true);
    assert(machine.cpu.regs.wordregs[regax] == 0U);
    assert(pc_transporter_machine_io_complete(&machine, last_request.sequence, 0x6655U));
    pc_transporter_machine_exec(&machine, SLICE);
    assert(machine.cpu.regs.wordregs[regax] == 0x6655U);
    assert(machine.cpu.hltstate && requests == 1U);

    start_program(wrapped_write, sizeof(wrapped_write));
    ram[PCT_IO_CONTROL_BASE] = 0x03U;
    pc_transporter_machine_exec(&machine, SLICE);
    expect_request(1U, 0x0000U, PCT_IO_LANE_LOW, true, true);
    assert(last_request.data == 0x22U && wrap_port_writes == 1U);
    assert(ram[PCT_IO_DATA_BASE] == 0x22U);
    assert(pc_transporter_machine_io_complete(&machine, last_request.sequence, 0U));
    pc_transporter_machine_exec(&machine, SLICE);
    assert(machine.cpu.hltstate && requests == 1U && wrap_port_writes == 1U);
}

static void test_odd_rep_insw(void)
{
    static const uint8_t code[] = {
        0xBA, 0x35, 0x01, /* MOV DX,0135 */
        0xBF, 0x04, 0x02, /* MOV DI,0204 */
        0xB9, 0x02, 0x00, /* MOV CX,2 */
        0xFD,             /* STD */
        0xF3, 0x6D,       /* REP INSW */
        0xF4
    };

    start_program(code, sizeof(code));
    ram[PCT_IO_CONTROL_BASE + 0x134U] = 0xC0U;
    ram[PCT_IO_CONTROL_BASE + 0x136U] = 0x0CU;
    pc_transporter_machine_exec(&machine, SLICE);
    expect_request(1U, 0x135U, PCT_IO_LANE_HIGH, false, true);
    const uint32_t first_sequence = last_request.sequence;
    assert(pc_transporter_machine_io_complete(&machine, first_sequence, 0xAA11U));
    pc_transporter_machine_exec(&machine, SLICE);
    expect_request(2U, 0x136U, PCT_IO_LANE_LOW, false, true);
    assert(machine.cpu.regs.wordregs[regdi] == 0x204U);
    assert(machine.cpu.regs.wordregs[regcx] == 2U);
    assert(ram[0x204U] == 0U && ram[0x205U] == 0U);
    assert(!pc_transporter_machine_io_complete(&machine, first_sequence, 0U));
    assert(pc_transporter_machine_io_complete(&machine, last_request.sequence, 0x22U));
    pc_transporter_machine_exec(&machine, SLICE);
    expect_request(3U, 0x135U, PCT_IO_LANE_HIGH, false, true);
    assert(machine.cpu.regs.wordregs[regdi] == 0x202U);
    assert(machine.cpu.regs.wordregs[regcx] == 1U);
    assert(ram[0x204U] == 0x11U && ram[0x205U] == 0x22U);
    assert(pc_transporter_machine_io_complete(&machine, last_request.sequence, 0x33U));
    pc_transporter_machine_exec(&machine, SLICE);
    expect_request(4U, 0x136U, PCT_IO_LANE_LOW, false, true);
    assert(pc_transporter_machine_io_complete(&machine, last_request.sequence, 0x44U));
    pc_transporter_machine_exec(&machine, SLICE);
    assert(machine.cpu.hltstate && requests == 4U);
    assert(machine.cpu.regs.wordregs[regdi] == 0x200U);
    assert(machine.cpu.regs.wordregs[regcx] == 0U);
    assert(ram[0x202U] == 0x33U && ram[0x203U] == 0x44U);
}

static void test_odd_outsw_latches_both_bytes(void)
{
    static const uint8_t code[] = {
        0xBA, 0x35, 0x01, /* MOV DX,0135 */
        0xBE, 0x00, 0x02, /* MOV SI,0200 */
        0xB9, 0x01, 0x00, /* MOV CX,1 */
        0xF3, 0x6F,       /* REP OUTSW */
        0xF4
    };

    start_program(code, sizeof(code));
    ram[PCT_IO_CONTROL_BASE + 0x134U] = 0x30U;
    ram[PCT_IO_CONTROL_BASE + 0x136U] = 0x03U;
    ram[0x200U] = 0x12U;
    ram[0x201U] = 0x34U;
    pc_transporter_machine_exec(&machine, SLICE);
    expect_request(1U, 0x135U, PCT_IO_LANE_HIGH, true, true);
    assert(last_request.data == 0x12U);
    assert(machine.cpu.regs.wordregs[regsi] == 0x200U);
    ram[0x200U] = 0x56U;
    ram[0x201U] = 0x78U;
    assert(pc_transporter_machine_io_complete(&machine, last_request.sequence, 0U));
    pc_transporter_machine_exec(&machine, SLICE);
    expect_request(2U, 0x136U, PCT_IO_LANE_LOW, true, true);
    assert(last_request.data == 0x34U);
    assert(machine.cpu.regs.wordregs[regsi] == 0x200U);
    assert(machine.cpu.regs.wordregs[regcx] == 1U);
    assert(pc_transporter_machine_io_complete(&machine, last_request.sequence, 0U));
    pc_transporter_machine_exec(&machine, SLICE);
    assert(machine.cpu.hltstate && requests == 2U);
    assert(machine.cpu.regs.wordregs[regsi] == 0x202U);
    assert(machine.cpu.regs.wordregs[regcx] == 0U);
    assert(ram[PCT_IO_DATA_BASE + 0x135U] == 0x12U);
    assert(ram[PCT_IO_DATA_BASE + 0x136U] == 0x34U);
}

static void test_reset_and_interrupt_while_waiting(void)
{
    static const uint8_t code[] = {0xBA, 0x34, 0x01, 0xFB, 0xEC, 0xF4, 0xF4};
    static const uint8_t interrupt_code[] = {
        0xA2, 0x00, 0x03,       /* MOV [0300],AL */
        0xFE, 0x06, 0x01, 0x03, /* INC BYTE [0301] */
        0xCF                    /* IRET */
    };

    start_program(code, sizeof(code));
    const uint32_t initial_accepted = machine.accepted_interrupts;
    ram[PCT_IO_CONTROL_BASE + 0x134U] = 0x0CU;
    ram[0x30U * 4U + 1U] = 0x02U; /* INT 30h points to 0000:0200. */
    memcpy(ram + 0x200U, interrupt_code, sizeof(interrupt_code));
    pc_transporter_machine_exec(&machine, SLICE);
    expect_request(1U, 0x134U, PCT_IO_LANE_LOW, false, true);
    const uint32_t cancelled = last_request.sequence;
    pc_transporter_machine_set_control(&machine, true, true);
    assert(!pc_transporter_machine_io_complete(&machine, cancelled, 0x55U));
    pc_transporter_machine_set_control(&machine, false, false);
    pc_transporter_machine_exec(&machine, SLICE);
    expect_request(2U, 0x134U, PCT_IO_LANE_LOW, false, true);
    assert(last_request.sequence != cancelled);
    assert(!pc_transporter_machine_io_complete(&machine, cancelled, 0x55U));
    pc_transporter_machine_set_interrupt(&machine, true, 0x30U);
    pc_transporter_machine_exec(&machine, SLICE);
    assert(ram[0x300U] == 0U);
    assert(machine.accepted_interrupts == initial_accepted);
    assert(pc_transporter_machine_io_complete(&machine, last_request.sequence, 0x66U));
    pc_transporter_machine_exec(&machine, SLICE);
    assert(machine.cpu.regs.byteregs[regal] == 0x66U && ram[0x300U] == 0U);
    assert(machine.accepted_interrupts == initial_accepted);
    pc_transporter_machine_exec(&machine, SLICE);
    assert(ram[0x300U] == 0x66U && machine.cpu.hltstate);
    assert(machine.accepted_interrupts == initial_accepted + 1U);
    assert(ram[0x301U] == 1U && !machine.interrupt_asserted);
    pc_transporter_machine_exec(&machine, SLICE);
    assert(machine.accepted_interrupts == initial_accepted + 1U && ram[0x301U] == 1U);
    pc_transporter_machine_set_interrupt(&machine, true, 0x30U);
    pc_transporter_machine_exec(&machine, SLICE);
    assert(machine.accepted_interrupts == initial_accepted + 2U && ram[0x301U] == 2U);
    pc_transporter_machine_reset(&machine);
    assert(machine.accepted_interrupts == initial_accepted + 2U);
}

static void read_fixture(const char *path, uint8_t *dest, size_t size)
{
    FILE *input = fopen(path, "rb");
    assert(input != NULL);
    assert(fread(dest, 1U, size, input) == size);
    assert(fgetc(input) == EOF);
    assert(fclose(input) == 0);
}

static void test_original_bios(const char *bios_path, const char *routes_path)
{
    memset(ram, 0, sizeof(ram));
    read_fixture(bios_path, ram + PCT_BIOS_RAM_BASE + 0xE000U, 8192U);
    read_fixture(routes_path, ram + PCT_IO_CONTROL_BASE, 1024U);
    assert(ram[PCT_BIOS_RAM_BASE + 0xFFF0U] == 0xEAU);
    requests = 0U;
    pc_transporter_machine_reset(&machine);
    pc_transporter_machine_set_control(&machine, false, false);
    const uint32_t illegal_before = machine.illegal_instructions;

    pc_transporter_machine_exec(&machine, SLICE);
    expect_request(1U, 0x3D8U, PCT_IO_LANE_LOW, true, false);
    assert(last_request.data == 0U);
    assert(pc_transporter_machine_io_complete(&machine, last_request.sequence, 0U));
    pc_transporter_machine_exec(&machine, SLICE);
    expect_request(2U, 0x61U, PCT_IO_LANE_HIGH, true, false);
    assert(last_request.data == 0xB0U);
    assert(pc_transporter_machine_io_complete(&machine, last_request.sequence, 0U));
    pc_transporter_machine_exec(&machine, SLICE);
    expect_request(3U, 0x62U, PCT_IO_LANE_LOW, false, true);
    assert(machine.illegal_instructions == illegal_before);
    puts("Original AEPCBIOS reached the first read request that waits for Portware");
}

int main(int argc, char **argv)
{
    pc_transporter_machine_init(&machine, &ops, NULL);
    test_memory_and_reset_vector();
    test_notification_and_prior_wait();
    test_cpu_profile();
    test_word_io();
    test_odd_rep_insw();
    test_odd_outsw_latches_both_bytes();
    test_reset_and_interrupt_while_waiting();
    if (argc == 3) {
        test_original_bios(argv[1], argv[2]);
    } else if (argc != 1) {
        fputs("Use test_pc_transporter_machine [AEPCBIOS routing.bin]\n", stderr);
        return 1;
    }
    puts("test_pc_transporter_machine: all checks passed");
    return 0;
}
