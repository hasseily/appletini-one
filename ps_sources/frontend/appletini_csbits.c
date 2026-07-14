/*
 * appletini_csbits.c -- See appletini_csbits.h.
 *
 * Algorithm ported verbatim from AppleWin source/NTSC_CharSet.cpp,
 * userVideoRom4K() (Copyright (C) 2010-2011, William S Simms;
 * (C) 2016, Tom Charlesworth; GPLv2). This implements the enhanced //e 4 KB
 * ROM path, without the II+, IIJ+, Base64A, Pravets, PAL, or unenhanced //e
 * character-set variants.
 */

#include <stdint.h>
#include <string.h>

#include "appletini_csbits.h"

/* Defined in apple2e_video_rom_data.c (generated). */
extern const uint8_t apple2e_video_rom[4096];

uint8_t csbits_enhanced2e[2][256][8];
static uint8_t s_active_video_rom[4096];

/* AppleWin's userVideoRom4K(), specialized to our embedded ROM and
 * the //e Enhanced charset. The XOR with 0xFF inverts the dumped ROM
 * bytes -- per UTAIIe:8-11, "dot patterns in the video ROM are
 * inverted..." -- so the resulting csbits hold active-high dot rows.
 *
 * Address layout (UTAIIe:8-14, Table 8.3):
 *   primary [00..3F] INVERSE  / [40..7F] FLASH      <- ROM 0x000..0x1FF
 *   primary [80..BF] NORMAL                          <- ROM 0x400..0x5FF
 *   primary [C0..FF] NORMAL                          <- ROM 0x600..0x7FF
 *   alt     [00..7F] INVERSE / [80..FF] NORMAL       <- ROM 0x000..0x7FF
 */
/* Build csbits from an arbitrary 4 KB //e video ROM image (same layout as
 * 342-0265 / 342-0133). `rom` must point to >= 4096 valid bytes; callers
 * (the SD-override loader on CPU0) are responsible for size/sanity checks. */
static void appletini_csbits_build(const uint8_t *rom)
{
    int RA = 0;
    int i;

    memset(csbits_enhanced2e, 0, sizeof(csbits_enhanced2e));

    /* Primary [00..3F] inverse, [40..7F] flash -- both pull from ROM
     * addresses 0..0x1FF; first 64 chars cloned to slots 64..127. */
    for (i = 0; i < 64; i++, RA += 8) {
        for (int y = 0; y < 8; y++) {
            uint8_t v = (uint8_t)(rom[RA + y] ^ 0xFF);
            csbits_enhanced2e[0][i][y]      = v;
            csbits_enhanced2e[0][i + 64][y] = v;
        }
    }

    /* Primary [80..BF] normal -- ROM 0x400..0x5FF (RA10=1, RA9=0). */
    RA = (1 << 10) | (0 << 9);
    for (i = 128; i < 192; i++, RA += 8) {
        for (int y = 0; y < 8; y++) {
            csbits_enhanced2e[0][i][y] =
                (uint8_t)(rom[RA + y] ^ 0xFF);
        }
    }

    /* Primary [C0..FF] normal -- ROM 0x600..0x7FF (RA10=1, RA9=1). */
    RA = (1 << 10) | (1 << 9);
    for (i = 192; i < 256; i++, RA += 8) {
        for (int y = 0; y < 8; y++) {
            csbits_enhanced2e[0][i][y] =
                (uint8_t)(rom[RA + y] ^ 0xFF);
        }
    }

    /* Alt charset -- entire 256 glyphs from ROM 0x000..0x7FF straight. */
    RA = 0;
    for (i = 0; i < 256; i++, RA += 8) {
        for (int y = 0; y < 8; y++) {
            csbits_enhanced2e[1][i][y] =
                (uint8_t)(rom[RA + y] ^ 0xFF);
        }
    }
}

void appletini_video_rom_use_baked(void)
{
    memcpy(s_active_video_rom, apple2e_video_rom, sizeof(s_active_video_rom));
    appletini_csbits_build(s_active_video_rom);
}

void appletini_video_rom_use_override(const uint8_t *rom)
{
    memcpy(s_active_video_rom, rom, sizeof(s_active_video_rom));
    appletini_csbits_build(s_active_video_rom);
}

uint8_t appletini_video_rom_read(uint16_t addr)
{
    return s_active_video_rom[addr & 0x0FFFu];
}

/* Default init: build from the baked //e Enhanced US ROM. */
void appletini_csbits_init(void)
{
    appletini_video_rom_use_baked();
}
