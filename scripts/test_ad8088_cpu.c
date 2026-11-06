/* Host-side smoke test for the vendored 8086/8088 core and Appletini
 * AD8088 memory/I/O adapter. The default build uses only 8088 instructions.
 * A second build with -DCPU_V20 adds the 80186/V30 string I/O checks that
 * the PC Transporter work needs.
 *
 * Build and run from the repository root with any host C compiler, once per
 * profile, into a scratch directory outside the repository:
 *
 *   gcc -std=c99 -Wall -Wextra -I ps_sources/frontend -I third_party/xtulator_cpu
 *       scripts/test_ad8088_cpu.c ps_sources/frontend/ad8088_machine.c
 *       third_party/xtulator_cpu/cpu.c -o <scratch>/test_8088 && <scratch>/test_8088
 *   gcc -DCPU_V20 <same arguments> -o <scratch>/test_v20 && <scratch>/test_v20
 *
 * The port callbacks model a bridge that can leave a transfer pending: bit k
 * of a mask makes port call number k (counted from 1) set cpu->io_pending
 * and return without completing. The I/O instruction must then restore IP,
 * change no register or memory, and complete once on the retry. The test
 * clears the flag where the real machine model would see the bridge
 * acknowledgment. The AD8088 adapter still splits word I/O into two byte
 * calls, so a pending high byte would repeat the low byte; the Transporter
 * machine model must give the core real word callbacks before word ports
 * may trap. */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ad8088_machine.h"

typedef struct {
    uint8_t apple[65536];
    uint8_t port_value;
    uint8_t port_written;
    uint32_t reads;
    uint32_t writes;
    /* Pending-transfer model, see the file comment. */
    CPU_t *cpu;
    uint32_t read_calls;
    uint32_t read_done;
    uint32_t pend_read_mask;
    uint32_t write_calls;
    uint32_t write_done;
    uint32_t pend_write_mask;
    uint8_t out_log[8];
    uint32_t out_count;
} test_io_t;

static uint8_t memory[AD8088_MEMORY_BYTES];

static int apple_read(void *opaque, uint16_t addr, uint8_t *value)
{
    test_io_t *io = (test_io_t *)opaque;
    *value = io->apple[addr];
    io->reads++;
    return 0;
}

static int apple_write(void *opaque, uint16_t addr, uint8_t value)
{
    test_io_t *io = (test_io_t *)opaque;
    io->apple[addr] = value;
    io->writes++;
    return 0;
}

static uint8_t port_read_cb(void *opaque, uint8_t port)
{
    test_io_t *io = (test_io_t *)opaque;
    io->read_calls++;
    if (io->read_calls < 32U &&
        (io->pend_read_mask & (1UL << io->read_calls)) != 0U) {
        io->cpu->io_pending = 1U;
        return 0xFFU;
    }
    io->read_done++;
    return (port == 1U) ? io->port_value : 0xFFU;
}

static void port_write_cb(void *opaque, uint8_t port, uint8_t value)
{
    test_io_t *io = (test_io_t *)opaque;
    io->write_calls++;
    if (io->write_calls < 32U &&
        (io->pend_write_mask & (1UL << io->write_calls)) != 0U) {
        io->cpu->io_pending = 1U;
        return;
    }
    io->write_done++;
    if (port == 0U) {
        io->port_written = value;
        if (io->out_count < sizeof(io->out_log)) {
            io->out_log[io->out_count++] = value;
        }
    }
}

static void io_reset_ports(test_io_t *io)
{
    io->port_written = 0U;
    io->read_calls = 0U;
    io->read_done = 0U;
    io->pend_read_mask = 0U;
    io->write_calls = 0U;
    io->write_done = 0U;
    io->pend_write_mask = 0U;
    io->out_count = 0U;
    memset(io->out_log, 0, sizeof(io->out_log));
}

/* What the machine model does once the bridge reports the acknowledgment. */
static void bridge_completed(ad8088_machine_t *machine)
{
    machine->cpu.io_pending = 0U;
}

static int check(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "test_ad8088_cpu: %s\n", message);
        return 0;
    }
    return 1;
}

int main(void)
{
    ad8088_machine_t machine;
    test_io_t io;
    uint8_t value;

    memset(memory, 0, sizeof(memory));
    memset(&io, 0, sizeof(io));
    ad8088_machine_init(&machine, memory, &io,
                        apple_read, apple_write,
                        port_read_cb, port_write_cb);
    io.cpu = &machine.cpu;

    if (!check(ad8088_machine_write(&machine, 0x00FFFFU, 0x12U) == 0,
               "Plus base RAM write failed") ||
        !check(ad8088_machine_read(&machine, 0x00FFFFU, &value) == 0 &&
               value == 0x12U, "Plus base RAM read failed") ||
        !check(ad8088_machine_write(&machine, 0x020000U, 0x34U) == 0,
               "Plus expansion RAM write failed") ||
        !check(ad8088_machine_read(&machine, 0x020000U, &value) == 0 &&
               value == 0x34U, "Plus expansion RAM read failed") ||
        !check(ad8088_machine_write(&machine, 0x030000U, 0x45U) == 0,
               "reserved-gap write failed") ||
        !check(ad8088_machine_read(&machine, 0x030000U, &value) == 0 &&
               value == 0xFFU, "reserved gap must float high") ||
        !check(ad8088_machine_write(&machine, 0x040000U, 0x56U) == 0,
               "AD128K write failed") ||
        !check(ad8088_machine_read(&machine, 0x040000U, &value) == 0 &&
               value == 0x56U, "AD128K read failed") ||
        !check(ad8088_machine_write(&machine, 0x0BFFFFU, 0x67U) == 0,
               "Plus high RAM write failed") ||
        !check(ad8088_machine_read(&machine, 0x0BFFFFU, &value) == 0 &&
               value == 0x67U, "Plus high RAM read failed") ||
        !check(ad8088_machine_write(&machine, 0x0C0000U, 0x78U) == 0,
               "reserved-ROM write failed") ||
        !check(ad8088_machine_read(&machine, 0x0C0000U, &value) == 0 &&
               value == 0xFFU, "reserved ROM must float high")) {
        return 1;
    }

    io.apple[0x2345U] = 0xA5U;
    if (!check(ad8088_machine_read(&machine, 0x012345U, &value) == 0 &&
               value == 0xA5U && io.reads == 1U,
               "Apple shared-memory read mapping failed") ||
        !check(ad8088_machine_write(&machine, 0x01ABCDU, 0x5AU) == 0 &&
               io.apple[0xABCDU] == 0x5AU && io.writes == 1U,
               "Apple shared-memory write mapping failed")) {
        return 1;
    }

    /* Reboot Camp stages about 24 KB in Apple RAM and uses PROM command $FE
     * to copy it to local $00800. Exercise a slightly larger transfer so the
     * regression crosses the original card's old $02000 RAM limit. */
    {
        const uint32_t source = AD8088_APPLE_WINDOW_BASE + 0x2400U;
        const uint32_t destination = 0x0800U;
        const uint32_t count = 0x6200U;

        for (uint32_t i = 0U; i < count; ++i) {
            io.apple[0x2400U + i] = (uint8_t)(i ^ (i >> 8));
        }
        for (uint32_t i = 0U; i < count; ++i) {
            if (!check(ad8088_machine_read(&machine, source + i, &value) == 0,
                       "Reboot Camp staged read failed") ||
                !check(ad8088_machine_write(&machine, destination + i,
                                            value) == 0,
                       "Reboot Camp local write failed")) {
                return 1;
            }
        }
        for (uint32_t i = 0U; i < count; ++i) {
            const uint8_t expected = (uint8_t)(i ^ (i >> 8));
            if (!check(memory[destination + i] == expected,
                       "Reboot Camp copy did not survive the $02000 boundary")) {
                return 1;
            }
        }
    }

    /* Execute an instruction stream spanning $01FFF->$02000. */
    {
        static const uint8_t program[] = {
            0xB8U, 0x34U, 0x12U,       /* MOV AX,$1234 */
            0x05U, 0x01U, 0x00U,       /* ADD AX,1 */
            0xA3U, 0x00U, 0x04U,       /* MOV [$0400],AX */
            0xCBU                      /* RETF */
        };
        memcpy(&memory[0x01FFDU], program, sizeof(program));
        ad8088_machine_start_far(&machine, 0x1FFDU, 0x0000U, 0x00U);
        ad8088_machine_exec(&machine, 32U);
        if (!check(machine.active == 0U && machine.return_seen != 0U,
                   "8088 execution failed across the old 8 KB boundary") ||
            !check(memory[0x0400U] == 0x35U && memory[0x0401U] == 0x12U,
                   "cross-boundary 8088 result is wrong")) {
            return 1;
        }
    }

    /* MOV AX,$1234; ADD AX,2; MOV [$0400],AX; RETF. */
    {
        static const uint8_t program[] = {
            0xB8U, 0x34U, 0x12U,
            0x05U, 0x02U, 0x00U,
            0xA3U, 0x00U, 0x04U,
            0xCBU
        };
        memcpy(&memory[0x0100U], program, sizeof(program));
        ad8088_machine_start_far(&machine, 0x0100U, 0x0000U, 0x77U);
        ad8088_machine_exec(&machine, 32U);
        if (!check(machine.active == 0U && machine.return_seen != 0U,
                   "far-call sentinel was not reached") ||
            !check(memory[0x0400U] == 0x36U && memory[0x0401U] == 0x12U,
                   "8088 arithmetic/store result is wrong")) {
            return 1;
        }
    }

    /* MOV AX,$1000; MOV DS,AX; MOV AL,[$1234]; MOV [$1235],AL; RETF. */
    {
        static const uint8_t program[] = {
            0xB8U, 0x00U, 0x10U,
            0x8EU, 0xD8U,
            0xA0U, 0x34U, 0x12U,
            0xA2U, 0x35U, 0x12U,
            0xCBU
        };
        io.apple[0x1234U] = 0xC3U;
        memcpy(&memory[0x0200U], program, sizeof(program));
        ad8088_machine_start_far(&machine, 0x0200U, 0x0000U, 0x00U);
        ad8088_machine_exec(&machine, 32U);
        if (!check(machine.active == 0U && io.apple[0x1235U] == 0xC3U,
                   "8088 Apple-window load/store failed")) {
            return 1;
        }
    }

    /* IN AL,1; OUT 0,AL; RETF. */
    {
        static const uint8_t program[] = {
            0xE4U, 0x01U,
            0xE6U, 0x00U,
            0xCBU
        };
        io_reset_ports(&io);
        io.port_value = 0x8BU;
        memcpy(&memory[0x0300U], program, sizeof(program));
        ad8088_machine_start_far(&machine, 0x0300U, 0x0000U, 0x00U);
        ad8088_machine_exec(&machine, 32U);
        if (!check(machine.active == 0U && io.port_written == 0x8BU,
                   "8088 mailbox port callbacks failed")) {
            return 1;
        }
    }

    /* The same program with the IN pending twice. AL and IP must not move,
     * a slice while pending must do nothing, and the OUT must run once. */
    {
        io_reset_ports(&io);
        io.port_value = 0x5AU;
        io.pend_read_mask = (1UL << 1) | (1UL << 2);
        ad8088_machine_start_far(&machine, 0x0300U, 0x0000U, 0x11U);
        ad8088_machine_exec(&machine, 32U);
        if (!check(machine.active == 1U && machine.cpu.io_pending == 1U,
                   "pending IN did not end the slice") ||
            !check(machine.cpu.ip == 0x0300U, "pending IN did not restore IP") ||
            !check(machine.cpu.regs.byteregs[regal] == 0x11U,
                   "pending IN changed AL") ||
            !check(io.read_calls == 1U && io.write_calls == 0U,
                   "pending IN ran ahead")) {
            return 1;
        }
        ad8088_machine_exec(&machine, 32U);
        if (!check(io.read_calls == 1U && machine.cpu.ip == 0x0300U,
                   "a slice ran while the transfer was pending")) {
            return 1;
        }
        bridge_completed(&machine);
        ad8088_machine_exec(&machine, 32U);
        if (!check(machine.active == 1U && io.read_calls == 2U &&
                   machine.cpu.ip == 0x0300U &&
                   machine.cpu.regs.byteregs[regal] == 0x11U,
                   "second pending IN retry is wrong")) {
            return 1;
        }
        bridge_completed(&machine);
        ad8088_machine_exec(&machine, 32U);
        if (!check(machine.active == 0U && io.read_calls == 3U &&
                   io.read_done == 1U && io.write_done == 1U &&
                   io.port_written == 0x5AU,
                   "IN retry did not complete exactly once")) {
            return 1;
        }
    }

    /* MOV AL,$3C; OUT 0,AL; RETF with the OUT pending once. */
    {
        static const uint8_t program[] = {
            0xB0U, 0x3CU,
            0xE6U, 0x00U,
            0xCBU
        };
        io_reset_ports(&io);
        io.pend_write_mask = (1UL << 1);
        memcpy(&memory[0x0320U], program, sizeof(program));
        ad8088_machine_start_far(&machine, 0x0320U, 0x0000U, 0x00U);
        ad8088_machine_exec(&machine, 32U);
        if (!check(machine.active == 1U && machine.cpu.ip == 0x0322U &&
                   io.write_calls == 1U && io.write_done == 0U &&
                   io.port_written == 0U,
                   "pending OUT did not stop before the port write took effect")) {
            return 1;
        }
        bridge_completed(&machine);
        ad8088_machine_exec(&machine, 32U);
        if (!check(machine.active == 0U && io.write_calls == 2U &&
                   io.write_done == 1U && io.port_written == 0x3CU,
                   "OUT retry did not complete exactly once")) {
            return 1;
        }
    }

    /* The exact instruction pattern used by AD8088_Test.dsk/.po: copy a
     * message from local card RAM to Apple text memory through ES=$1000. */
    {
        static const uint8_t program[] = {
            0xB8U, 0x00U, 0x10U,       /* MOV AX,$1000 */
            0x8EU, 0xC0U,              /* MOV ES,AX */
            0xBEU, 0x16U, 0x01U,       /* MOV SI,$0116 */
            0xBFU, 0x80U, 0x06U,       /* MOV DI,$0680 */
            0xB9U, 0x17U, 0x00U,       /* MOV CX,23 */
            0xFCU,                     /* CLD */
            0xACU,                     /* LODSB */
            0x0CU, 0x80U,              /* OR AL,$80 */
            0xAAU,                     /* STOSB */
            0xE2U, 0xFAU,              /* LOOP -6 */
            0xCBU,                     /* RETF */
            '8', '0', '8', '8', ' ', 'E', 'X', 'E', 'C', 'U', 'T', 'E',
            'D', ' ', 'T', 'H', 'I', 'S', ' ', 'L', 'I', 'N', 'E'
        };
        memset(&io.apple[0x0680U], 0, 23U);
        memcpy(&memory[0x0100U], program, sizeof(program));
        ad8088_machine_start_far(&machine, 0x0100U, 0x0000U, 0x00U);
        ad8088_machine_exec(&machine, 512U);
        if (!check(machine.active == 0U && machine.return_seen != 0U,
                   "visible disk-test 8088 program did not return") ||
            !check(io.apple[0x0680U] == (uint8_t)('8' | 0x80U) &&
                   io.apple[0x0696U] == (uint8_t)('E' | 0x80U),
                   "8088 REP-style text loop did not write Apple memory")) {
            return 1;
        }
    }

#if !defined(CPU_8086)
    /* 80186/V30 string I/O. INS steps DI only and OUTS steps SI only. A
     * pending transfer inside REP must leave CX, the index and memory at the
     * last completed iteration and restore IP to the REP prefix. */

    /* MOV AX,0; MOV ES,AX; MOV SI,$1111; MOV DI,$0400; MOV DX,1; MOV CX,4;
     * CLD; REP INSB; RETF. The third transfer pends. */
    {
        static const uint8_t program[] = {
            0xB8U, 0x00U, 0x00U,       /* 0100 MOV AX,0 */
            0x8EU, 0xC0U,              /* 0103 MOV ES,AX */
            0xBEU, 0x11U, 0x11U,       /* 0105 MOV SI,$1111 */
            0xBFU, 0x00U, 0x04U,       /* 0108 MOV DI,$0400 */
            0xBAU, 0x01U, 0x00U,       /* 010B MOV DX,1 */
            0xB9U, 0x04U, 0x00U,       /* 010E MOV CX,4 */
            0xFCU,                     /* 0111 CLD */
            0xF3U, 0x6CU,              /* 0112 REP INSB */
            0xCBU                      /* 0114 RETF */
        };
        io_reset_ports(&io);
        io.port_value = 0x7EU;
        io.pend_read_mask = (1UL << 3);
        memset(&memory[0x0400U], 0, 8U);
        memcpy(&memory[0x0100U], program, sizeof(program));
        ad8088_machine_start_far(&machine, 0x0100U, 0x0000U, 0x00U);
        ad8088_machine_exec(&machine, 64U);
        if (!check(machine.active == 1U && machine.cpu.io_pending == 1U &&
                   machine.cpu.ip == 0x0112U,
                   "pending REP INSB did not restore IP to the REP prefix") ||
            !check(machine.cpu.regs.wordregs[regcx] == 2U &&
                   machine.cpu.regs.wordregs[regdi] == 0x0402U,
                   "pending REP INSB left CX or DI past the completed iterations") ||
            !check(machine.cpu.regs.wordregs[regsi] == 0x1111U,
                   "INSB changed SI") ||
            !check(memory[0x0400U] == 0x7EU && memory[0x0401U] == 0x7EU &&
                   memory[0x0402U] == 0x00U,
                   "pending REP INSB wrote memory for the pending transfer") ||
            !check(io.read_calls == 3U && io.read_done == 2U,
                   "pending REP INSB port call count is wrong")) {
            return 1;
        }
        bridge_completed(&machine);
        ad8088_machine_exec(&machine, 64U);
        if (!check(machine.active == 0U &&
                   machine.cpu.regs.wordregs[regcx] == 0U &&
                   machine.cpu.regs.wordregs[regdi] == 0x0404U &&
                   machine.cpu.regs.wordregs[regsi] == 0x1111U,
                   "REP INSB end state is wrong") ||
            !check(memory[0x0402U] == 0x7EU && memory[0x0403U] == 0x7EU &&
                   memory[0x0404U] == 0x00U,
                   "REP INSB did not complete the remaining transfers once") ||
            !check(io.read_done == 4U && io.read_calls == 5U,
                   "REP INSB completed a transfer twice")) {
            return 1;
        }
    }

    /* MOV SI,$0500; MOV DI,$2222; MOV DX,0; MOV CX,3; CLD; REP OUTSB; RETF.
     * The second transfer pends. */
    {
        static const uint8_t program[] = {
            0xBEU, 0x00U, 0x05U,       /* 0200 MOV SI,$0500 */
            0xBFU, 0x22U, 0x22U,       /* 0203 MOV DI,$2222 */
            0xBAU, 0x00U, 0x00U,       /* 0206 MOV DX,0 */
            0xB9U, 0x03U, 0x00U,       /* 0209 MOV CX,3 */
            0xFCU,                     /* 020C CLD */
            0xF3U, 0x6EU,              /* 020D REP OUTSB */
            0xCBU                      /* 020F RETF */
        };
        io_reset_ports(&io);
        io.pend_write_mask = (1UL << 2);
        memory[0x0500U] = 0x31U;
        memory[0x0501U] = 0x32U;
        memory[0x0502U] = 0x33U;
        memcpy(&memory[0x0200U], program, sizeof(program));
        ad8088_machine_start_far(&machine, 0x0200U, 0x0000U, 0x00U);
        ad8088_machine_exec(&machine, 64U);
        if (!check(machine.active == 1U && machine.cpu.ip == 0x020DU,
                   "pending REP OUTSB did not restore IP to the REP prefix") ||
            !check(machine.cpu.regs.wordregs[regcx] == 2U &&
                   machine.cpu.regs.wordregs[regsi] == 0x0501U,
                   "pending REP OUTSB left CX or SI past the completed iteration") ||
            !check(machine.cpu.regs.wordregs[regdi] == 0x2222U,
                   "OUTSB changed DI") ||
            !check(io.out_count == 1U && io.out_log[0] == 0x31U &&
                   io.write_calls == 2U && io.write_done == 1U,
                   "pending REP OUTSB port call count is wrong")) {
            return 1;
        }
        bridge_completed(&machine);
        ad8088_machine_exec(&machine, 64U);
        if (!check(machine.active == 0U &&
                   machine.cpu.regs.wordregs[regcx] == 0U &&
                   machine.cpu.regs.wordregs[regsi] == 0x0503U &&
                   machine.cpu.regs.wordregs[regdi] == 0x2222U,
                   "REP OUTSB end state is wrong") ||
            !check(io.out_count == 3U && io.out_log[1] == 0x32U &&
                   io.out_log[2] == 0x33U && io.write_done == 3U &&
                   io.write_calls == 4U,
                   "REP OUTSB did not complete each transfer exactly once")) {
            return 1;
        }
    }

    /* MOV AX,0; MOV ES,AX; MOV DI,$0406; MOV SI,$1234; MOV DX,1; MOV CX,2;
     * STD; REP INSW; CLD; RETF. Words step DI down by two; SI is untouched.
     * The adapter reads port 1 then port 2, so each word is $FF9C. */
    {
        static const uint8_t program[] = {
            0xB8U, 0x00U, 0x00U,       /* 0300 MOV AX,0 */
            0x8EU, 0xC0U,              /* 0303 MOV ES,AX */
            0xBFU, 0x06U, 0x04U,       /* 0305 MOV DI,$0406 */
            0xBEU, 0x34U, 0x12U,       /* 0308 MOV SI,$1234 */
            0xBAU, 0x01U, 0x00U,       /* 030B MOV DX,1 */
            0xB9U, 0x02U, 0x00U,       /* 030E MOV CX,2 */
            0xFDU,                     /* 0311 STD */
            0xF3U, 0x6DU,              /* 0312 REP INSW */
            0xFCU,                     /* 0314 CLD */
            0xCBU                      /* 0315 RETF */
        };
        io_reset_ports(&io);
        io.port_value = 0x9CU;
        memset(&memory[0x0400U], 0, 8U);
        memcpy(&memory[0x0300U], program, sizeof(program));
        ad8088_machine_start_far(&machine, 0x0300U, 0x0000U, 0x00U);
        ad8088_machine_exec(&machine, 64U);
        if (!check(machine.active == 0U &&
                   machine.cpu.regs.wordregs[regcx] == 0U &&
                   machine.cpu.regs.wordregs[regdi] == 0x0402U &&
                   machine.cpu.regs.wordregs[regsi] == 0x1234U,
                   "REP INSW with DF=1 left the wrong registers") ||
            !check(memory[0x0406U] == 0x9CU && memory[0x0407U] == 0xFFU &&
                   memory[0x0404U] == 0x9CU && memory[0x0405U] == 0xFFU &&
                   memory[0x0403U] == 0x00U && memory[0x0408U] == 0x00U,
                   "REP INSW with DF=1 wrote the wrong words")) {
            return 1;
        }
    }

    /* MOV SI,$0620; MOV DI,$4444; MOV DX,0; OUTSB; RETF without REP. The
     * single transfer pends once, so IP must return to the OUTSB itself. */
    {
        static const uint8_t program[] = {
            0xBEU, 0x20U, 0x06U,       /* 0600 MOV SI,$0620 */
            0xBFU, 0x44U, 0x44U,       /* 0603 MOV DI,$4444 */
            0xBAU, 0x00U, 0x00U,       /* 0606 MOV DX,0 */
            0x6EU,                     /* 0609 OUTSB */
            0xCBU                      /* 060A RETF */
        };
        io_reset_ports(&io);
        io.pend_write_mask = (1UL << 1);
        memory[0x0620U] = 0x77U;
        memcpy(&memory[0x0600U], program, sizeof(program));
        ad8088_machine_start_far(&machine, 0x0600U, 0x0000U, 0x00U);
        ad8088_machine_exec(&machine, 64U);
        if (!check(machine.active == 1U && machine.cpu.ip == 0x0609U &&
                   machine.cpu.regs.wordregs[regsi] == 0x0620U &&
                   machine.cpu.regs.wordregs[regdi] == 0x4444U &&
                   io.write_done == 0U,
                   "pending OUTSB changed state before completion")) {
            return 1;
        }
        bridge_completed(&machine);
        ad8088_machine_exec(&machine, 64U);
        if (!check(machine.active == 0U &&
                   machine.cpu.regs.wordregs[regsi] == 0x0621U &&
                   machine.cpu.regs.wordregs[regdi] == 0x4444U &&
                   io.write_done == 1U && io.out_log[0] == 0x77U,
                   "OUTSB retry did not complete exactly once")) {
            return 1;
        }
    }

    puts("test_ad8088_cpu: V20 string I/O checks passed");
#endif

    puts("test_ad8088_cpu: all checks passed");
    return 0;
}
