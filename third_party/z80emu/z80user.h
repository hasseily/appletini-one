/* z80user.h -- Appletini Applicard memory/IO binding for z80emu.
 *
 * Memory goes through 8 KB page tables owned by applicard_z80_ctx_t
 * (banked 512 KB RAM, boot-ROM shadow, ROM-write bucket), so every access
 * is two loads and a shift with no function call. Only IN/OUT leave the
 * page tables and touch the PL latch registers over AXI.
 */

#ifndef __Z80USER_INCLUDED__
#define __Z80USER_INCLUDED__

#ifdef __cplusplus
extern "C" {
#endif

#include "applicard_z80.h"

#define APPLICARD_Z80_CTX ((applicard_z80_ctx_t *) context)

#define Z80_READ_BYTE(address, x)                                       \
{                                                                       \
        unsigned a_ = (address) & 0xffff;                               \
        (x) = APPLICARD_Z80_CTX->rd_page[a_ >> 13][a_ & 0x1fff];        \
}

#define Z80_FETCH_BYTE(address, x)      Z80_READ_BYTE((address), (x))

#define Z80_READ_WORD(address, x)                                       \
{                                                                       \
        unsigned a0_ = (address) & 0xffff;                              \
        unsigned a1_ = ((address) + 1) & 0xffff;                        \
        (x) = APPLICARD_Z80_CTX->rd_page[a0_ >> 13][a0_ & 0x1fff]       \
            | (APPLICARD_Z80_CTX->rd_page[a1_ >> 13][a1_ & 0x1fff]      \
               << 8);                                                   \
}

#define Z80_FETCH_WORD(address, x)      Z80_READ_WORD((address), (x))

#define Z80_WRITE_BYTE(address, x)                                      \
{                                                                       \
        unsigned a_ = (address) & 0xffff;                               \
        APPLICARD_Z80_CTX->wr_page[a_ >> 13][a_ & 0x1fff] =             \
            (unsigned char) (x);                                        \
}

#define Z80_WRITE_WORD(address, x)                                      \
{                                                                       \
        unsigned a0_ = (address) & 0xffff;                              \
        unsigned a1_ = ((address) + 1) & 0xffff;                        \
        APPLICARD_Z80_CTX->wr_page[a0_ >> 13][a0_ & 0x1fff] =           \
            (unsigned char) (x);                                        \
        APPLICARD_Z80_CTX->wr_page[a1_ >> 13][a1_ & 0x1fff] =           \
            (unsigned char) ((x) >> 8);                                 \
}

#define Z80_READ_WORD_INTERRUPT(address, x)     Z80_READ_WORD((address), (x))

#define Z80_WRITE_WORD_INTERRUPT(address, x)    Z80_WRITE_WORD((address), (x))

/* The IN handler flags stop_requested when the idle governor detects the
 * program spinning on the handshake status port; ending the slice there
 * hands the CPU back to the main loop instead of burning the budget.
 */

#define Z80_INPUT_BYTE(port, x)                                         \
{                                                                       \
        (x) = applicard_z80_in(APPLICARD_Z80_CTX,                       \
                               (unsigned char) ((port) & 0xff));        \
        if (APPLICARD_Z80_CTX->stop_requested)                          \
                number_cycles = 0;                                      \
}

#define Z80_OUTPUT_BYTE(port, x)                                        \
{                                                                       \
        applicard_z80_out(APPLICARD_Z80_CTX,                            \
                          (unsigned char) ((port) & 0xff),              \
                          (unsigned char) ((x) & 0xff));                \
}

#ifdef __cplusplus
}
#endif

#endif
