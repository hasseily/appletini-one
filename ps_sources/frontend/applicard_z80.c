/* Applicard Z80 machine model -- see applicard_z80.h. */

#include "applicard_z80.h"

#include <string.h>

#include "applicard_regs.h"
#include "../lib/common.h"

void applicard_z80_map_update(applicard_z80_ctx_t *ctx)
{
    /* GZ80 hardware decodes bits 1-3; we honor bits 1-5 as a strict
     * superset (32 banks / 2 MB) for our own expansion software. */
    const unsigned bank = (ctx->bank_reg >> 1) & 0x1FU;
    const unsigned common = (unsigned)(ctx->bank_reg & 0x40U);
    uint8_t *const bank_base = ctx->ram + (bank * APPLICARD_Z80_BANK_SIZE);

    for (unsigned p = 0; p < APPLICARD_Z80_PAGES; ++p) {
        uint8_t *base;
        if (common != 0U && p >= (APPLICARD_Z80_PAGES / 2U)) {
            /* Common area: upper 32 K stays on bank 0. */
            base = ctx->ram + (p * APPLICARD_Z80_PAGE_SIZE);
        } else {
            base = bank_base + (p * APPLICARD_Z80_PAGE_SIZE);
        }
        ctx->rd_page[p] = base;
        ctx->wr_page[p] = base;
    }

    if (ctx->rom_mapped != 0U) {
        /* 2 KB ROM repeated through $0000-$7FFF; writes are discarded
         * while the shadow is active. */
        for (unsigned p = 0; p < (APPLICARD_Z80_PAGES / 2U); ++p) {
            ctx->rd_page[p] = ctx->rom_mirror;
            ctx->wr_page[p] = ctx->write_bucket;
        }
    }
}

void applicard_z80_reset(applicard_z80_ctx_t *ctx)
{
    Z80Reset(&ctx->state);
    ctx->rom_mapped = 1U;
    ctx->bank_reg = 0U;
    ctx->to6502_shadow = 0U;
    ctx->last_status_byte = 0xFFU; /* impossible value: first poll never streaks */
    ctx->blocked = 0U;
    ctx->stop_requested = 0U;
    ctx->status_streak = 0U;
    applicard_z80_map_update(ctx);
}

uint8_t applicard_z80_in(applicard_z80_ctx_t *ctx, uint8_t port)
{
    switch (port & 0xE0U) {
    case 0x00U:
        /* Readback of the Z80->6502 output latch. */
        return ctx->to6502_shadow;

    case 0x20U: {
        /* Consume the 6502->Z80 latch. One STATUS read returns flag,
         * sequence number and data together; the seq-matched ACK is
         * rejected by the PL if the 6502 posted a fresh byte (or reset
         * the card) in between. Like the hardware, the latch contents
         * are returned whether or not a byte was pending. */
        const uint32_t st = REG_READ(APPLICARD_REG_STATUS);
        if ((st & APPLICARD_STATUS_F_Z80) != 0U) {
            REG_WRITE(APPLICARD_REG_CONTROL,
                      APPLICARD_CONTROL_ACK_TOZ80(APPLICARD_STATUS_SEQ(st)));
            ctx->bytes_from_apple++;
        }
        ctx->status_streak = 0U;
        return (uint8_t)APPLICARD_STATUS_DATA(st);
    }

    case 0x40U: {
        /* Handshake status: bit7 = byte pending from the 6502, bit0 =
         * our last byte not yet consumed. This is the port every wait
         * loop spins on, so it doubles as the idle-governor probe: an
         * unchanged value for APPLICARD_Z80_IDLE_STREAK consecutive
         * polls means the program is blocked on the 6502 and the slice
         * ends early. */
        const uint32_t st = REG_READ(APPLICARD_REG_STATUS);
        uint8_t value = 0U;
        ctx->status_reads++;
        if ((st & APPLICARD_STATUS_F_Z80) != 0U) {
            value |= 0x80U;
        }
        if ((st & APPLICARD_STATUS_F_6502) != 0U) {
            value |= 0x01U;
        }
        if (value == ctx->last_status_byte) {
            if (++ctx->status_streak >= APPLICARD_Z80_IDLE_STREAK) {
                ctx->blocked = 1U;
                ctx->stop_requested = 1U;
                ctx->status_streak = 0U;
            }
        } else {
            ctx->last_status_byte = value;
            ctx->status_streak = 0U;
        }
        return value;
    }

    default:
        /* $60-$7F unused read, $80-$9F unpopulated CTC, rest reserved. */
        return 0xFFU;
    }
}

void applicard_z80_out(applicard_z80_ctx_t *ctx, uint8_t port, uint8_t value)
{
    switch (port & 0xE0U) {
    case 0x00U:
        /* Send a byte to the 6502: PL latches it and raises F_6502. */
        ctx->to6502_shadow = value;
        REG_WRITE(APPLICARD_REG_TO6502, value);
        ctx->bytes_to_apple++;
        ctx->status_streak = 0U;
        break;

    case 0x60U:
        if ((uint8_t)(value & 0x01U) != ctx->rom_mapped) {
            ctx->rom_mapped = value & 0x01U;
            applicard_z80_map_update(ctx);
        }
        break;

    case 0xC0U:
        if (value != ctx->bank_reg) {
            ctx->bank_reg = value;
            applicard_z80_map_update(ctx);
        }
        break;

    default:
        /* $20/$40 have no write side, $80 CTC unpopulated, $A0 reserved
         * Apple-IRQ, $E0 unused. */
        break;
    }
}
