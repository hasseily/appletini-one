/*
 * appletini_ntsc.h -- Port of AppleWin's NTSC pixel-emission core for the
 * bare-metal Cortex-A9 //e renderer. Owns the precomputed chroma tables,
 * the per-pixel signal-bit shift register, the per-cycle scan-position
 * state, and the dot-emission helpers (updatePixels and friends).
 *
 * Used by apple_cycle_renderer.c, which drives this code one Apple cycle
 * at a time, transliterated from AppleWin's per-mode update functions.
 *
 * The 560x192 BGRA32 active image is rendered inside a 616x224 border-ring
 * buffer. apple_cycle_renderer.c rotates three such buffers in DDR, and the
 * compositor scales the published slot 2x4 into the output frame.
 *
 * This module covers NTSC enhanced //e color-monitor rendering. PAL refresh,
 * IIgs SHR, VidHD, RGB-card modes, save/load state, debugger hooks, deferred
 * mode changes, alternate monochrome colors, and TV scanline blending are
 * handled elsewhere or unsupported.
 *
 * Algorithm and tables ported from AppleWin source/NTSC.cpp
 * (Copyright (C) 2010-2011, William S Simms; (C) 2014-2016, Michael
 * Pohoreski, Tom Charlesworth; GPLv2).
 */

#ifndef APPLETINI_NTSC_H
#define APPLETINI_NTSC_H

#include <stdint.h>

#include "video_output.h"

/* Scratch-FB geometry.
 *
 * Each row carries invisible guard pixels on both sides. The left guard
 * gives AppleWin's per-mode pixel-offset writes (`g_pVideoAddress -= 2`
 * for color modes, plus another `-= 1` for DHGR/DLORES/TEXT80, see
 * NTSC.cpp:769 and :783) somewhere to land without trampling the
 * previous row's tail. The 560x192 active picture sits inside a fixed
 * 28-pixel/16-line IIgs-style border ring. */
#define ATN_SCRATCH_LEFT_BORDER_PIXELS  4u
#define ATN_SCRATCH_RIGHT_BORDER_PIXELS 4u
#define ATN_SCRATCH_BORDER_PIXELS       ATN_SCRATCH_LEFT_BORDER_PIXELS
#define ATN_ACTIVE_WIDTH                560u
#define ATN_ACTIVE_HEIGHT               192u
#define ATN_BORDER_H_CYCLES             2u
#define ATN_BORDER_H_PIXELS             (ATN_BORDER_H_CYCLES * 14u)
#define ATN_BORDER_V_LINES              16u
#define ATN_SCRATCH_WIDTH               (ATN_ACTIVE_WIDTH + (2u * ATN_BORDER_H_PIXELS))
#define ATN_SCRATCH_HEIGHT              (ATN_ACTIVE_HEIGHT + (2u * ATN_BORDER_V_LINES))
#define ATN_ACTIVE_X                    ATN_BORDER_H_PIXELS
#define ATN_ACTIVE_Y                    ATN_BORDER_V_LINES
#define ATN_SCRATCH_ROW_PIXELS          (ATN_SCRATCH_LEFT_BORDER_PIXELS + \
                                         ATN_SCRATCH_WIDTH + \
                                         ATN_SCRATCH_RIGHT_BORDER_PIXELS)
#define ATN_SCRATCH_PIXELS              (ATN_SCRATCH_ROW_PIXELS * ATN_SCRATCH_HEIGHT)
#define ATN_SCRATCH_BYTES               (ATN_SCRATCH_PIXELS * 4u)

/* Apple //e scanner constants (NTSC, US). */
#define ATN_SCANNER_MAX_HORZ              65u
#define ATN_SCANNER_MAX_VERT_NTSC         262u
#define ATN_SCANNER_MAX_VERT_PAL          312u
#define ATN_SCANNER_MAX_VERT              ATN_SCANNER_MAX_VERT_NTSC
#define ATN_SCANNER_HORZ_COLORBURST_BEG   12u
#define ATN_SCANNER_HORZ_COLORBURST_END   16u
#define ATN_SCANNER_HORZ_START            25u
#define ATN_SCANNER_Y_MIXED               160u
#define ATN_SCANNER_Y_DISPLAY             192u

/* Chroma-table dimensions. */
#define ATN_NUM_PHASES     4
#define ATN_NUM_SEQUENCES  4096   /* 12-bit signal-bit window */

/* Per-pixel state used by the dot-emission helpers. The renderer-side
 * step_*() functions write these directly. */
extern uint32_t *g_pVideoAddress;     /* points into the scratch buffer */
extern int       g_nColorBurstPixels;
extern int       g_nVideoClockVert;
extern int       g_nVideoClockHorz;
extern int       g_nColorPhaseNTSC;
extern int       g_nSignalBitsNTSC;
extern uint16_t  g_nLastColumnPixelNTSC;

/* Flash + char-set state used by step_text40 / step_text80. */
extern int       g_nVideoCharSet;     /* 0 = primary, 1 = alt */
extern uint16_t  g_nTextFlashMask;    /* 0x0000 or 0xFFFF */

/* Lookup tables. */
extern uint16_t  g_aPixelDoubleMaskHGR[256];

/* Chroma color tables. Each entry is BGRA32 packed into a uint32_t. */
typedef struct {
    uint8_t b, g, r, a;
} atn_bgra_t;

extern atn_bgra_t g_aHueMonitor[ATN_NUM_PHASES][ATN_NUM_SEQUENCES];
extern atn_bgra_t g_aHueColorTV[ATN_NUM_PHASES][ATN_NUM_SEQUENCES];
extern atn_bgra_t g_aBnWMonitor[ATN_NUM_SEQUENCES];
extern atn_bgra_t g_aBnwColorTV[ATN_NUM_SEQUENCES];
extern const atn_bgra_t g_aAppleWinBaseColors[16];

/* Active scratch buffer (the BGRA32 scratch used by g_pVideoAddress).
 * Owned by the renderer (allocated outside this module); set via
 * appletini_ntsc_set_framebuffer() at init. */
extern uint32_t *g_atn_framebuffer;

/* Init tables. Idempotent; safe to call once at boot. ~20 ms on
 * Cortex-A9 due to the floating-point chroma filters. */
void appletini_ntsc_init(void);

/* Runtime video style. mono_enable forces all emitted dots through the
 * tinted monochrome path; color_mode selects the active color lookup
 * when Apple colorburst is active. */
void appletini_ntsc_set_video_output(uint8_t mono_enable,
                                     uint8_t mono_color,
                                     uint8_t color_mode);

/* Bind the scratch buffer used for dot writes. Pass a pointer to a
 * uint32_t array of size ATN_SCRATCH_PIXELS. */
void appletini_ntsc_set_framebuffer(uint32_t *fb);

/* Pixel emit: shift `signal` into the 12-bit window, look up via the
 * active color/mono table, write one BGRA32 to *g_pVideoAddress, advance
 * pointer by 1. */
void atn_emit_color(uint16_t signal);
void atn_emit_mono(uint16_t signal);
void atn_emit_blank_pixels(uint8_t count);

/* Per-mode helper from NTSC.cpp:603. Shifts 14 dots out (or 7 dots when
 * color burst is off), tracking g_nLastColumnPixelNTSC. */
void atn_updatePixels(uint16_t bits);

/* Color-burst helper. True when color is active (g_nColorBurstPixels >= 2). */
static inline int atn_get_color_burst(void) {
    return g_nColorBurstPixels >= 2;
}

#endif /* APPLETINI_NTSC_H */
