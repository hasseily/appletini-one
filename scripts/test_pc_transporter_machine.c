#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "pc_transporter_machine.h"

#define PROGRAM 0x0100U
#define RESET_VECTOR_RAM (PCT_BIOS_RAM_BASE + 0xFFF0U)
#define SLICE 64U

static uint8_t ram[PCT_RAM_BYTES];
static uint16_t ems[PCT_EMS_BYTES / 2U];
static pc_transporter_machine_t machine;
static pc_transporter_io_request_t last_request;
static unsigned requests;
static unsigned wrap_port_writes;
static unsigned byte_reads, dword_reads;
static unsigned word_reads, word_writes;
static unsigned mapped_reads[2], mapped_writes[2];

static uint8_t shared_read(void *opaque, uint32_t offset)
{
    (void)opaque;
    assert(offset < sizeof(ram));
    byte_reads++;
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

static uint16_t shared_read_word(void *opaque, uint32_t offset)
{
    (void)opaque;
    assert((offset & 1U) == 0U && offset < PCT_IO_DATA_BASE);
    word_reads++;
    return ram[offset] | (uint16_t)ram[offset + 1U] << 8;
}

static void shared_write_word(void *opaque, uint32_t offset, uint16_t value)
{
    (void)opaque;
    assert((offset & 1U) == 0U && offset < PCT_IO_DATA_BASE);
    word_writes++;
    ram[offset] = (uint8_t)value;
    ram[offset + 1U] = (uint8_t)(value >> 8);
}

static uint32_t shared_read_dword(void *opaque, uint32_t offset)
{
    (void)opaque;
    assert((offset & 3U) == 0U && offset < PCT_BIOS_RAM_BASE);
    dword_reads++;
    return (uint32_t)ram[offset] | ((uint32_t)ram[offset + 1U] << 8) |
        ((uint32_t)ram[offset + 2U] << 16) | ((uint32_t)ram[offset + 3U] << 24);
}

static bool mapped_port_read(void *opaque, uint16_t port, uint8_t *value)
{
    (void)opaque;
    assert(port < 2U);
    mapped_reads[port]++;
    *value = ram[PCT_IO_DATA_BASE + port];
    return true;
}

static bool mapped_port_write(void *opaque, uint16_t port, uint8_t value)
{
    (void)opaque;
    assert(port < 2U);
    mapped_writes[port]++;
    ram[PCT_IO_DATA_BASE + port] = value;
    return true;
}

static const pc_transporter_machine_ops_t word_ops = {
    .read_shared = shared_read,
    .write_shared = shared_write,
    .read_shared_word = shared_read_word,
    .write_shared_word = shared_write_word,
    .read_shared_dword = shared_read_dword,
    .read_port = mapped_port_read,
    .write_port = mapped_port_write,
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
    byte_reads = dword_reads = 0U;
    word_reads = word_writes = 0U;
    memset(mapped_reads, 0, sizeof(mapped_reads));
    memset(mapped_writes, 0, sizeof(mapped_writes));
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

static void test_aligned_rep_words(void)
{
    static const uint8_t code[] = {0xFC, 0xF3, 0xA5, 0xFD, 0xF3, 0xAB, 0xF4};
    static const uint8_t words[] = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC};

    start_program(code, sizeof(code));
    memcpy(ram + 0x200U, words, sizeof(words));
    machine.cpu.regs.wordregs[regsi] = 0x200U;
    machine.cpu.regs.wordregs[regdi] = 0x300U;
    machine.cpu.regs.wordregs[regcx] = 3U;
    pc_transporter_machine_exec(&machine, 2U); /* Far jump and CLD. */

    pc_transporter_machine_exec(&machine, 2U); /* First REP MOVSW element. */
    assert(word_reads == 1U && word_writes == 1U);
    assert(machine.cpu.ip == PROGRAM + 1U);
    assert(machine.cpu.regs.wordregs[regsi] == 0x202U);
    assert(machine.cpu.regs.wordregs[regdi] == 0x302U);
    assert(machine.cpu.regs.wordregs[regcx] == 2U);
    assert(memcmp(ram + 0x300U, words, 2U) == 0 && ram[0x302U] == 0U);

    pc_transporter_machine_exec(&machine, 4U);
    assert(word_reads == 3U && word_writes == 3U);
    assert(machine.cpu.ip == PROGRAM + 1U);
    assert(machine.cpu.regs.wordregs[regsi] == 0x206U);
    assert(machine.cpu.regs.wordregs[regdi] == 0x306U);
    assert(machine.cpu.regs.wordregs[regcx] == 0U);
    assert(memcmp(ram + 0x300U, words, sizeof(words)) == 0);

    pc_transporter_machine_exec(&machine, 2U); /* Empty REP, then STD. */
    assert(machine.cpu.ip == PROGRAM + 4U && machine.cpu.df);
    machine.cpu.regs.wordregs[regdi] = 0x308U;
    machine.cpu.regs.wordregs[regcx] = 2U;
    machine.cpu.regs.wordregs[regax] = 0xABCDU;
    pc_transporter_machine_exec(&machine, 2U); /* First REP STOSW element. */
    assert(word_reads == 3U && word_writes == 4U);
    assert(machine.cpu.ip == PROGRAM + 4U);
    assert(machine.cpu.regs.wordregs[regdi] == 0x306U);
    assert(machine.cpu.regs.wordregs[regcx] == 1U);
    assert(ram[0x308U] == 0xCDU && ram[0x309U] == 0xABU && ram[0x306U] == 0U);

    pc_transporter_machine_exec(&machine, SLICE);
    assert(word_reads == 3U && word_writes == 5U);
    assert(machine.cpu.regs.wordregs[regsi] == 0x206U);
    assert(machine.cpu.regs.wordregs[regdi] == 0x304U);
    assert(machine.cpu.regs.wordregs[regcx] == 0U && machine.cpu.hltstate);
    assert(ram[0x306U] == 0xCDU && ram[0x307U] == 0xABU);
    assert(memcmp(ram + 0x300U, words, sizeof(words)) == 0);
}

static void start_word_access(uint16_t segment, uint16_t offset)
{
    /* MOV AX,[BX], MOV [BX],AX, HLT. No word-sized instruction immediates. */
    static const uint8_t code[] = {0x8B, 0x07, 0x89, 0x07, 0xF4};

    start_program(code, sizeof(code));
    machine.cpu.segregs[regds] = segment;
    machine.cpu.regs.wordregs[regbx] = offset;
}

static void check_word_access(uint16_t value, unsigned fast_accesses)
{
    pc_transporter_machine_exec(&machine, 2U); /* Far jump and word read. */
    assert(machine.cpu.regs.wordregs[regax] == value);
    if (word_reads != fast_accesses) {
        fprintf(stderr, "Word read at %04x:%04x used %u callbacks, expected %u\n",
                machine.cpu.segregs[regds], machine.cpu.regs.wordregs[regbx],
                word_reads, fast_accesses);
    }
    assert(word_reads == fast_accesses);
    machine.cpu.regs.wordregs[regax] = 0xABCDU;
    pc_transporter_machine_exec(&machine, SLICE);
    assert(machine.cpu.hltstate && word_writes == fast_accesses);
}

static void test_word_memory_boundaries(void)
{
    start_word_access(0xBE00U, 0x0FFEU); /* Last aligned word before the port window. */
    ram[PCT_IO_DATA_BASE - 2U] = 0x12U;
    ram[PCT_IO_DATA_BASE - 1U] = 0x34U;
    check_word_access(0x3412U, 1U);
    assert(ram[PCT_IO_DATA_BASE - 2U] == 0xCDU && ram[PCT_IO_DATA_BASE - 1U] == 0xABU);
    assert(mapped_reads[0] == 0U && mapped_writes[0] == 0U);

    start_word_access(0x0000U, 0x0201U); /* Odd word in ordinary RAM. */
    ram[0x201U] = 0x12U;
    ram[0x202U] = 0x34U;
    check_word_access(0x3412U, 0U);
    assert(ram[0x201U] == 0xCDU && ram[0x202U] == 0xABU);
    assert(ram[0x200U] == 0U && ram[0x203U] == 0U);

    start_word_access(0xBE00U, 0x0FFFU); /* Low byte in RAM, high byte in port 0. */
    ram[PCT_IO_DATA_BASE - 1U] = 0x12U;
    ram[PCT_IO_DATA_BASE] = 0x34U;
    check_word_access(0x3412U, 0U);
    assert(ram[PCT_IO_DATA_BASE - 1U] == 0xCDU && ram[PCT_IO_DATA_BASE] == 0xABU);
    assert(mapped_reads[0] == 1U && mapped_writes[0] == 1U);
    assert(mapped_reads[1] == 0U && mapped_writes[1] == 0U);

    start_word_access(0xBF00U, 0x0000U); /* Even word in the port window. */
    ram[PCT_IO_DATA_BASE] = 0x12U;
    ram[PCT_IO_DATA_BASE + 1U] = 0x34U;
    check_word_access(0x3412U, 0U);
    assert(ram[PCT_IO_DATA_BASE] == 0xCDU && ram[PCT_IO_DATA_BASE + 1U] == 0xABU);
    assert(mapped_reads[0] == 1U && mapped_writes[0] == 1U);
    assert(mapped_reads[1] == 1U && mapped_writes[1] == 1U);

    start_word_access(0xF000U, 0x1234U); /* Read-only BIOS alias. */
    ram[PCT_BIOS_RAM_BASE + 0x1234U] = 0x12U;
    ram[PCT_BIOS_RAM_BASE + 0x1235U] = 0x34U;
    check_word_access(0x3412U, 0U);
    assert(ram[PCT_BIOS_RAM_BASE + 0x1234U] == 0x12U);
    assert(ram[PCT_BIOS_RAM_BASE + 0x1235U] == 0x34U);

    start_word_access(0xFFFFU, 0x000FU); /* Low byte in BIOS, high byte wraps to zero. */
    ram[PCT_BIOS_RAM_BASE + 0xFFFFU] = 0x12U;
    ram[0] = 0x34U;
    check_word_access(0x3412U, 0U);
    assert(ram[PCT_BIOS_RAM_BASE + 0xFFFFU] == 0x12U && ram[0] == 0xABU);

    start_word_access(0xC000U, 0x0000U); /* Unmapped memory. */
    check_word_access(0xFFFFU, 0U);
}

static void test_fast_instruction_fetch(void)
{
    static const uint8_t loop[] = {
        0xB9, 16, 0,        /* MOV CX,16 */
        0xB8, 0x34, 0x12,  /* MOV AX,1234 */
        0x31, 0xD2,        /* XOR DX,DX */
        0x05, 0x57, 0x13,  /* ADD AX,1357 */
        0xD1, 0xC0,        /* ROL AX,1 */
        0x31, 0xC2,        /* XOR DX,AX */
        0xE2, 0xF7,        /* LOOP back to ADD */
        0xF4,
    };
    start_program(loop, sizeof(loop));
    pc_transporter_machine_exec(&machine, 2U * SLICE);
    assert(machine.cpu.hltstate && machine.cpu.regs.wordregs[regcx] == 0U);
    const uint16_t ax = machine.cpu.regs.wordregs[regax];
    const uint16_t dx = machine.cpu.regs.wordregs[regdx];
    const unsigned standard = byte_reads + word_reads;
    assert(dword_reads == 0U);

    pc_transporter_machine_set_maximum_speed(&machine, true);
    start_program(loop, sizeof(loop));
    pc_transporter_machine_exec(&machine, 2U * SLICE);
    assert(machine.cpu.hltstate && machine.cpu.regs.wordregs[regcx] == 0U);
    assert(machine.cpu.regs.wordregs[regax] == ax && machine.cpu.regs.wordregs[regdx] == dx);
    const unsigned maximum = byte_reads + word_reads + dword_reads;
    assert(dword_reads != 0U && maximum < standard);
    printf("Register loop RAM reads: Standard %u, Maximum %u\n", standard, maximum);

    static const uint8_t self_modify[] = {
        0xC6, 0x06, 0x05, 0x01, 0xF4, /* MOV BYTE [0105],F4 */
        0x90,                          /* This NOP becomes HLT. */
        0xB8, 0xEF, 0xBE,             /* This MOV must never run. */
        0xF4,
    };
    start_program(self_modify, sizeof(self_modify));
    pc_transporter_machine_exec(&machine, SLICE);
    assert(machine.cpu.hltstate && machine.cpu.ip == 0x0106U);
    assert(machine.cpu.regs.wordregs[regax] != 0xBEEFU);

    start_program(loop, sizeof(loop));
    machine.cpu.segregs[regcs] = 0x1000U;
    machine.cpu.ip = 0xFFFEU;
    ram[0x1FFFEU] = 0xB8U;
    ram[0x1FFFFU] = 0x34U;
    ram[0x20000U] = 0x12U; /* Match the legacy contiguous word at IP FFFF. */
    ram[0x10001U] = 0xF4U; /* Next instruction wraps to CS:0001. */
    pc_transporter_machine_exec(&machine, 2U);
    assert(machine.cpu.hltstate && machine.cpu.regs.wordregs[regax] == 0x1234U);
    assert(dword_reads == 3U);

    start_program(loop, sizeof(loop));
    machine.cpu.segregs[regcs] = 0xFFFFU;
    machine.cpu.ip = 0x000EU;
    ram[PCT_BIOS_RAM_BASE + 0xFFFEU] = 0xB8U;
    ram[PCT_BIOS_RAM_BASE + 0xFFFFU] = 0x34U;
    ram[0] = 0x12U; /* Word in the BIOS alias spans the 20-bit boundary. */
    ram[1] = 0xF4U;
    pc_transporter_machine_exec(&machine, 2U);
    assert(machine.cpu.hltstate && machine.cpu.regs.wordregs[regax] == 0x1234U);
    assert(dword_reads == 0U);

    start_program(loop, sizeof(loop));
    machine.cpu.segregs[regcs] = 0xBF00U;
    machine.cpu.ip = 0U;
    ram[PCT_IO_DATA_BASE] = 0xB0U; /* MOV AL,42 in the side-effectful window. */
    ram[PCT_IO_DATA_BASE + 1U] = 0x42U;
    pc_transporter_machine_exec(&machine, 1U);
    assert(machine.cpu.regs.byteregs[regal] == 0x42U && dword_reads == 0U);
    assert(mapped_reads[0] == 1U && mapped_reads[1] == 1U);

    pc_transporter_machine_set_maximum_speed(&machine, false);
    test_aligned_rep_words();
    const unsigned standard_rep = byte_reads + word_reads;
    pc_transporter_machine_set_maximum_speed(&machine, true);
    test_aligned_rep_words();
    const unsigned maximum_rep = byte_reads + word_reads + dword_reads;
    assert(dword_reads != 0U && maximum_rep < standard_rep);
    printf("REP fixture RAM reads: Standard %u, Maximum %u\n", standard_rep, maximum_rep);
    pc_transporter_machine_set_maximum_speed(&machine, false);
}

static void test_ems(void)
{
    static const uint8_t code[] = {
        0xBA, 0x60, 0x02, 0xEC,        /* IN AL,0260: hardware ID. */
        0xA2, 0x00, 0x02,              /* MOV [0200],AL */
        0xB8, 0x00, 0xD0, 0x8E, 0xC0, /* MOV AX,D000; MOV ES,AX */
        0x26, 0xA1, 0, 0,              /* Read the initially unmapped page. */
        0xA3, 0x01, 0x02,
        0xBA, 0x61, 0x02, 0xB0, 0, 0xEE, /* Select window 0. */
        0x42, 0xB8, 0xFF, 0, 0xEF,     /* Commit bank 255 with a word OUT. */
        0x26, 0xC7, 0x06, 0xFE, 0x3F, 0xAA, 0xBB, /* MOV WORD ES:[3FFE],BBAA */
        0x26, 0xC6, 0x06, 0, 0, 0x42, /* MOV BYTE ES:[0000],42 */
        0xBA, 0x61, 0x02, 0xB0, 1, 0xEE, /* Select window 1 and bank 0. */
        0x42, 0xB8, 0, 0, 0xEF,
        0x26, 0xC7, 0x06, 0, 0x40, 0x33, 0x44,
        0x26, 0xA1, 0xFF, 0x3F,        /* Read across the bank boundary. */
        0xA3, 0x03, 0x02,
        0x26, 0xC7, 0x06, 0xFF, 0x3F, 0x55, 0x66, /* Write across it. */
        0xBA, 0x61, 0x02, 0xB0, 3, 0xEE, /* Select window 3 and bank 2. */
        0x42, 0xB8, 2, 0, 0xEF,
        0x26, 0xC7, 0x06, 0xFF, 0xFF, 0x76, 0x98, /* MOV WORD ES:[FFFF],9876 */
        0x26, 0xA1, 0xFF, 0xFF,        /* Word crosses into unmapped E0000. */
        0xA3, 0x05, 0x02,
        0xBA, 0x61, 0x02, 0xB0, 0, 0xEE, /* Select window 0 again. */
        0x42, 0xB0, 1, 0xEE,           /* Stage low byte without changing the map. */
        0x26, 0xA0, 0, 0,
        0xA2, 0x07, 0x02,
        0x42, 0xB0, 1, 0xEE,           /* Committing invalid bank 257 unmaps it. */
        0x26, 0xA0, 0, 0,
        0xA2, 0x08, 0x02,
        0xBA, 0x61, 0x02, 0xB0, 4, 0xEE, /* Invalid selector leaves all maps intact. */
        0x42, 0xB8, 0xFF, 0, 0xEF,
        0x26, 0xA1, 0, 0x40,
        0xA3, 0x09, 0x02,
        0xBA, 0x61, 0x02, 0xB0, 2, 0xEE, /* Also map bank 0 into window 2. */
        0x42, 0xB8, 0, 0, 0xEF,
        0x26, 0xA1, 0, 0x80,
        0xA3, 0x0B, 0x02,
        0xBA, 0x61, 0x02, 0xB0, 3, 0xEE, /* Explicitly unmap window 3. */
        0x42, 0xB8, 0xFF, 0xFF, 0xEF,
        0x26, 0xA1, 0xFE, 0xFF,
        0xA3, 0x0D, 0x02,
        0xF4
    };
    static const uint8_t read_after_reset[] = {
        0xB8, 0, 0xD0, 0x8E, 0xD8, /* MOV AX,D000; MOV DS,AX */
        0xA1, 0, 0x40,              /* MOV AX,[4000] */
        0xF4
    };
    static const uint8_t results[] = {0xE6, 0xFF, 0xFF, 0xBB, 0x33, 0x76, 0xFF, 0x42,
                                      0xFF, 0x66, 0x44, 0x66, 0x44, 0xFF, 0xFF};
    pc_transporter_machine_ops_t ems_ops = word_ops;
    ems_ops.submit_io = submit_io;
    pc_transporter_machine_init(&machine, &ems_ops, NULL);
    pc_transporter_machine_set_ems_memory(&machine, ems);
    start_program(code, sizeof(code));
    ram[PCT_IO_CONTROL_BASE + 0x260U] = 0xFFU;
    ram[PCT_IO_CONTROL_BASE + 0x262U] = 0xFFU;
    pc_transporter_machine_exec(&machine, 96U);
    assert(machine.cpu.hltstate && requests == 0U);
    assert(memcmp(ram + 0x200U, results, sizeof(results)) == 0);
    assert(ems[0] == 0x4466U);
    assert(ems[(PCT_EMS_PAGE_BYTES * 3U) / 2U - 1U] == 0x7600U);
    assert(ems[PCT_EMS_BYTES / 2U - 1U] == 0x55AAU);

    memcpy(ram + PROGRAM, read_after_reset, sizeof(read_after_reset));
    pc_transporter_machine_set_control(&machine, true, true);
    pc_transporter_machine_set_control(&machine, false, false);
    pc_transporter_machine_exec(&machine, SLICE);
    assert(machine.cpu.hltstate && machine.cpu.regs.wordregs[regax] == 0xFFFFU);
    assert(ems[0] == 0x4466U);
    pc_transporter_machine_init(&machine, &word_ops, NULL);
    puts("PC Transporter EMS: four windows, bank 255, split words, and reset passed");
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
    pc_transporter_machine_init(&machine, &word_ops, NULL);
    test_aligned_rep_words();
    test_word_memory_boundaries();
    test_fast_instruction_fetch();
    test_ems();
    puts("test_pc_transporter_machine: all checks passed");
    return 0;
}
