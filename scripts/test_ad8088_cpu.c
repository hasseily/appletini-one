/* Host-side smoke test for the vendored 8086/8088 core and Appletini
 * AD8088 memory/I/O adapter. This deliberately uses only 8088 instructions. */

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
    return (port == 1U) ? io->port_value : 0xFFU;
}

static void port_write_cb(void *opaque, uint8_t port, uint8_t value)
{
    test_io_t *io = (test_io_t *)opaque;
    if (port == 0U) {
        io->port_written = value;
    }
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
        io.port_value = 0x8BU;
        io.port_written = 0U;
        memcpy(&memory[0x0300U], program, sizeof(program));
        ad8088_machine_start_far(&machine, 0x0300U, 0x0000U, 0x00U);
        ad8088_machine_exec(&machine, 32U);
        if (!check(machine.active == 0U && io.port_written == 0x8BU,
                   "8088 mailbox port callbacks failed")) {
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

    puts("test_ad8088_cpu: all checks passed");
    return 0;
}
