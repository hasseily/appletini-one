/*
 * appletini_csbits.h -- Port of AppleWin's NTSC_CharSet for the Apple //e
 * Enhanced video ROM by default. Decodes the active 4 KB ROM into a pair of
 * 256-glyph x 8-row bitmap tables (primary charset + alt/mousetext).
 *
 * Companion to appletini_ntsc.{h,c}; consumed by step_text40 / step_text80
 * via getCharSetBits().
 *
 * The ROM data is in apple2e_video_rom_data.c (generated from
 * hdl/apple/apple2e_video_rom_342_0265_a.mem -- same data the FPGA uses).
 *
 * Includes only the enhanced US //e character data. Apple II+, II/J+,
 * Base64A, Pravets, non-enhanced //e, and PAL variants are omitted.
 */

#ifndef APPLETINI_CSBITS_H
#define APPLETINI_CSBITS_H

#include <stdint.h>

/* csbits[charset_idx][char_code][row] = 7-bit dot pattern (active high).
 * charset_idx 0 = primary, 1 = alt (mousetext). */
extern uint8_t csbits_enhanced2e[2][256][8];

/* Decode the embedded (baked Enhanced US) ROM into csbits_enhanced2e[].
 * Call once at init. */
void appletini_csbits_init(void);

/* Select the active 4 KB video ROM image. These also rebuild
 * csbits_enhanced2e[] for the standard text renderer. */
void appletini_video_rom_use_baked(void);
void appletini_video_rom_use_override(const uint8_t *rom);

/* Raw active-ROM read for the PAL Accurate renderer, whose character
 * generator path computes video-ROM addresses directly. */
uint8_t appletini_video_rom_read(uint16_t addr);

#endif /* APPLETINI_CSBITS_H */
