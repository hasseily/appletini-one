/* Applicard Z80 machine model: banked memory map + I/O port dispatch.
 *
 * Wraps the vendored z80emu core (third_party/z80emu) with the PCPI
 * Applicard / GG Labs GZ80S memory and I/O semantics:
 *
 *   memory  64 KB address space through 8 KB read/write page tables.
 *           2 MB RAM = 32 banks x 64 KB. GZ80 bank register (Z80 OUT
 *           $C0-$DF): data bits 1-3 select the bank (RAM A16-A18) on the
 *           real GZ80; we honor bits 1-5 (32 banks) as a strict superset
 *           -- GZ80 software never sets bits 4-5, so it sees identical
 *           behavior, while our own RAM-disk driver gets 2 MB. Bit 6
 *           keeps the upper 32 K on bank 0 ("common area"; range inferred
 *           from the GZ80 PAL's A15 input -- verify against real software).
 *           2 KB boot ROM shadowed at $0000, mirrored through $7FFF, when
 *           mapped (OUT $60-$7F bit 0); writes to the shadowed half are
 *           discarded while mapped (MAME-verified behavior).
 *
 *   ports   $00-$1F  IN: readback of the Z80->6502 latch  OUT: send byte
 *           $20-$3F  IN: consume the 6502->Z80 latch
 *           $40-$5F  IN: bit7 = byte pending from 6502, bit0 = our byte
 *                        not yet consumed
 *           $60-$7F  OUT: bit0 = ROM shadow enable
 *           $80-$9F  CTC socket (unpopulated) -- reads $FF
 *           $C0-$DF  OUT: GZ80 bank register
 *
 * The latch/flag traffic goes to applicard_card.sv over AxiSimple
 * (applicard_regs.h); everything else is plain cached-DDR data touched
 * only by this core, so no cache maintenance is ever needed.
 */

#ifndef APPLICARD_Z80_H
#define APPLICARD_Z80_H

#include <stdint.h>

#include "z80emu.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APPLICARD_Z80_PAGE_SHIFT   13U
#define APPLICARD_Z80_PAGE_SIZE    (1U << APPLICARD_Z80_PAGE_SHIFT) /* 8 KB */
#define APPLICARD_Z80_PAGES        8U
#define APPLICARD_Z80_BANKS        32U
#define APPLICARD_Z80_BANK_SIZE    0x10000U
#define APPLICARD_Z80_RAM_SIZE     (APPLICARD_Z80_BANKS * APPLICARD_Z80_BANK_SIZE)
#define APPLICARD_Z80_ROM_SIZE     2048U

/* Consecutive unchanged status-port polls before the slice is abandoned
 * and the service drops to one PL status check per main-loop pass.
 *
 * Sizing: each poll is one AXI STATUS read (~0.25 us host time). The
 * threshold must out-wait a full 6502 protocol round trip, not just one
 * byte: PCPI console-status requests (which MBASIC issues between EVERY
 * statement for Ctrl-C detection) take ~40-100 us for the 6502 to answer,
 * during which the Z80 spins on this port. Measured on hardware: 32 made
 * every transferred byte pay a main-loop wakeup (disk crawl); 256 still
 * tripped mid-round-trip on almost every MBASIC statement (FRACT 2:00);
 * 1024 got FRACT to 0:58 but slow replies and 80-col character rendering
 * (~0.3-1 ms, scrolls worse) still tripped it. 4096 polls = ~1 ms of
 * spinning: burning up to 1 ms to catch a flag beats sleeping ~2 ms for
 * the next main-loop pass, and it stays inside the burst wall cap. A
 * genuine reply changes the status byte and resets the streak, so only
 * truly dead waits (keyboard idle) block. */
#define APPLICARD_Z80_IDLE_STREAK  4096U

typedef struct applicard_z80_ctx {
    Z80_STATE state;

    /* 8 KB page tables; every Z80 memory access is rd/wr_page[a>>13][a&0x1fff]. */
    uint8_t *rd_page[APPLICARD_Z80_PAGES];
    uint8_t *wr_page[APPLICARD_Z80_PAGES];

    uint8_t *ram;                                   /* 512 KB, cached DDR */
    uint8_t rom_mirror[APPLICARD_Z80_PAGE_SIZE];    /* 2 KB ROM x4 */
    uint8_t write_bucket[APPLICARD_Z80_PAGE_SIZE];  /* ROM-shadow write sink */

    uint8_t rom_mapped;   /* OUT $60 bit 0 */
    uint8_t bank_reg;     /* OUT $C0: bits 1-3 bank, bit 6 common enable */
    uint8_t to6502_shadow;/* last byte sent to the 6502 (IN $00 readback) */

    /* Idle governor state (see applicard_service.c). */
    uint8_t  last_status_byte;
    uint8_t  blocked;
    uint8_t  stop_requested;
    uint16_t status_streak;

    /* Cumulative stats for the `z80 status` UART command. */
    uint32_t bytes_to_apple;
    uint32_t bytes_from_apple;
    uint32_t status_reads;   /* IN $40 handshake polls (spin indicator) */
} applicard_z80_ctx_t;

uint8_t applicard_z80_in(applicard_z80_ctx_t *ctx, uint8_t port);
void applicard_z80_out(applicard_z80_ctx_t *ctx, uint8_t port, uint8_t value);
void applicard_z80_map_update(applicard_z80_ctx_t *ctx);

/* Hardware reset: PC=0, ROM shadow mapped, bank 0, governor cleared. */
void applicard_z80_reset(applicard_z80_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* APPLICARD_Z80_H */
