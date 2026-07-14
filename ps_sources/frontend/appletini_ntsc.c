/*
 * appletini_ntsc.c -- See appletini_ntsc.h.
 *
 * Algorithm and table-init math ported from AppleWin source/NTSC.cpp
 * (Copyright (C) 2010-2011, William S Simms;
 *  (C) 2014-2016, Michael Pohoreski, Tom Charlesworth; GPLv2).
 *
 * Implements the enhanced //e NTSC color-monitor pipeline. PAL refresh,
 * IIgs SHR, VidHD, RGB-card modes, debugger hooks, save/load state,
 * deferred mode changes, other Apple models, and TV-style scanline blending
 * are outside this module.
 *
 * The chroma-table init (initChromaPhaseTables) does floating-point
 * Butterworth-filter math at boot. ~20 ms on Cortex-A9.
 */

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "appletini_ntsc.h"

/* ===== Public state ============================================== */

uint32_t  *g_pVideoAddress       = NULL;
int        g_nColorBurstPixels   = 0;
int        g_nVideoClockVert     = 0;
int        g_nVideoClockHorz     = 0;
int        g_nColorPhaseNTSC     = 0;
int        g_nSignalBitsNTSC     = 0;
uint16_t   g_nLastColumnPixelNTSC = 0;
int        g_nVideoCharSet       = 0;
uint16_t   g_nTextFlashMask      = 0;
uint32_t  *g_atn_framebuffer     = NULL;

uint16_t   g_aPixelDoubleMaskHGR[256];

atn_bgra_t g_aHueMonitor[ATN_NUM_PHASES][ATN_NUM_SEQUENCES];
atn_bgra_t g_aHueColorTV[ATN_NUM_PHASES][ATN_NUM_SEQUENCES];
atn_bgra_t g_aBnWMonitor[ATN_NUM_SEQUENCES];
atn_bgra_t g_aBnwColorTV[ATN_NUM_SEQUENCES];

static uint8_t s_video_mono_enable = 0U;
static uint8_t s_video_mono_color = APPLE_VIDEO_MONO_WHITE;
static uint8_t s_video_color_mode = APPLE_VIDEO_COLOR_COMPOSITE_MONITOR;

#define ATN_BGR(r, g, b) { (uint8_t)(b), (uint8_t)(g), (uint8_t)(r), 255U }

/* AppleWin NTSC.cpp GenerateBaseColors(), fed from g_aHueColorTV.
 * Order matches RGBMonitor.cpp's base color range: BLACK..WHITE. */
const atn_bgra_t g_aAppleWinBaseColors[16] = {
    ATN_BGR(0x00, 0x00, 0x00), /* black */
    ATN_BGR(0x93, 0x0B, 0x7C), /* deep red */
    ATN_BGR(0x1F, 0x35, 0xD3), /* dark blue */
    ATN_BGR(0xBB, 0x36, 0xFF), /* purple */
    ATN_BGR(0x00, 0x76, 0x0C), /* dark green */
    ATN_BGR(0x7E, 0x7E, 0x7E), /* dark gray */
    ATN_BGR(0x07, 0xA8, 0xE1), /* medium blue */
    ATN_BGR(0x9D, 0xAC, 0xFF), /* light blue */
    ATN_BGR(0x62, 0x4C, 0x00), /* brown */
    ATN_BGR(0xF9, 0x56, 0x1D), /* orange */
    ATN_BGR(0x7E, 0x7E, 0x7E), /* gray 2 */
    ATN_BGR(0xFF, 0x81, 0xEC), /* pink */
    ATN_BGR(0x43, 0xC8, 0x00), /* green */
    ATN_BGR(0xDC, 0xCD, 0x16), /* yellow */
    ATN_BGR(0x5D, 0xF7, 0x85), /* aquamarine */
    ATN_BGR(0xFF, 0xFF, 0xFF)  /* white */
};

#undef ATN_BGR

/* Convert the current 4-bit signal window plus NTSC phase back to the
 * AppleWin base-color index. This keeps repeated lores/DHGR nibbles solid
 * in Idealized/RGB instead of rotating through phase-colored entries. */
static const uint8_t g_aAppleWinPhaseColorIndex[ATN_NUM_PHASES][16] = {
    { 0U, 1U, 8U, 9U, 4U, 5U, 12U, 13U, 2U, 3U, 10U, 11U, 6U, 7U, 14U, 15U },
    { 0U, 2U, 1U, 3U, 8U, 10U, 9U, 11U, 4U, 6U, 5U, 7U, 12U, 14U, 13U, 15U },
    { 0U, 4U, 2U, 6U, 1U, 5U, 3U, 7U, 8U, 12U, 10U, 14U, 9U, 13U, 11U, 15U },
    { 0U, 8U, 4U, 12U, 2U, 10U, 6U, 14U, 1U, 9U, 5U, 13U, 3U, 11U, 7U, 15U }
};

/* ===== Filter constants ========================================== */

#define ATN_PI                  3.14159265358979323846
#define ATN_DEG_TO_RAD(x)       (ATN_PI*(x)/180.0)
#define ATN_RAD_45              (ATN_PI*0.25)
#define ATN_RAD_90              (ATN_PI*0.5)
#define ATN_CYCLESTART          ATN_DEG_TO_RAD(45)

#define CHROMA_GAIN             7.438011255
#define CHROMA_0               -0.7318893645
#define CHROMA_1                1.2336442711

#define LUMA_GAIN               13.71331570
#define LUMA_0                 -0.3961075449
#define LUMA_1                  1.1044202472

#define SIGNAL_GAIN             7.614490548
#define SIGNAL_0               -0.2718798058
#define SIGNAL_1                0.7465656072

#define I_TO_R                  0.956
#define I_TO_G                 -0.272
#define I_TO_B                 -1.105

#define Q_TO_R                  0.621
#define Q_TO_G                 -0.647
#define Q_TO_B                  1.702

/* AppleWin's color-table de-ringing tweaks (NTSC.cpp:38-40). */
#define ATN_REMOVE_WHITE_RINGING   1
#define ATN_REMOVE_BLACK_GHOSTING  1
#define ATN_REMOVE_GRAY_CHROMA     1

/* ===== Filter helpers ============================================ */

static double clamp01(double x) {
    if (x < 0.0) return 0.0;
    if (x > 1.0) return 1.0;
    return x;
}

/* Butterworth-style biquads. Each holds its own static state across
 * calls. State is reset by re-running initChromaPhaseTables (which
 * walks every (phase, sequence) pair freshly so state alignment
 * matches AppleWin). */
static double initFilterChroma(double z) {
    static double x[3] = {0,0,0};
    static double y[3] = {0,0,0};
    x[0] = x[1]; x[1] = x[2]; x[2] = z / CHROMA_GAIN;
    y[0] = y[1]; y[1] = y[2];
    y[2] = -x[0] + x[2] + (CHROMA_0*y[0]) + (CHROMA_1*y[1]);
    return y[2];
}

static double initFilterLuma0(double z) {
    static double x[3] = {0,0,0};
    static double y[3] = {0,0,0};
    x[0] = x[1]; x[1] = x[2]; x[2] = z / LUMA_GAIN;
    y[0] = y[1]; y[1] = y[2];
    y[2] = x[0] + x[2] + (2.0*x[1]) + (LUMA_0*y[0]) + (LUMA_1*y[1]);
    return y[2];
}

static double initFilterLuma1(double z) {
    static double x[3] = {0,0,0};
    static double y[3] = {0,0,0};
    x[0] = x[1]; x[1] = x[2]; x[2] = z / LUMA_GAIN;
    y[0] = y[1]; y[1] = y[2];
    y[2] = x[0] + x[2] + (2.0*x[1]) + (LUMA_0*y[0]) + (LUMA_1*y[1]);
    return y[2];
}

static double initFilterSignal(double z) {
    static double x[3] = {0,0,0};
    static double y[3] = {0,0,0};
    x[0] = x[1]; x[1] = x[2]; x[2] = z / SIGNAL_GAIN;
    y[0] = y[1]; y[1] = y[2];
    y[2] = x[0] + x[2] + (2.0*x[1]) + (SIGNAL_0*y[0]) + (SIGNAL_1*y[1]);
    return y[2];
}

/* ===== Pixel-double mask init ==================================== */

static void initPixelDoubleMasks(void) {
    /* g_aPixelDoubleMaskHGR: 7-bit mono -> 14-bit double-pixel pattern.
     *   0x001 -> 0x0003
     *   0x002 -> 0x000C
     *   0x004 -> 0x0030
     *   ...
     *   0x100 -> 0x4000 (high bit = half-pixel shift; pre-optimisation)
     * Optimisation: only fill 0..0x7F; second 128 entries are mirror of
     * the first 128 for the high-bit-shift case. */
    memset(g_aPixelDoubleMaskHGR, 0, sizeof(g_aPixelDoubleMaskHGR));
    for (uint8_t b = 0; b < 0x80; b++) {
        for (uint8_t bit = 0; bit < 7; bit++) {
            if (b & (1 << bit))
                g_aPixelDoubleMaskHGR[b] |= (uint16_t)(3u << (bit * 2));
        }
    }

}

/* ===== Chroma table init ========================================= */

static void initChromaPhaseTables(void) {
    for (int phase = 0; phase < ATN_NUM_PHASES; phase++) {
        double phi = (phase * ATN_RAD_90) + ATN_CYCLESTART;

        for (int s = 0; s < ATN_NUM_SEQUENCES; s++) {
            int t = s;
            double y0 = 0, y1 = 0, c = 0, i = 0, q = 0;
            double zz = 0, z_last = 0;

            for (int n = 0; n < 12; n++) {
                z_last = (t & 0x800) ? 1.0 : 0.0;
                t = t << 1;

                for (int k = 0; k < 2; k++) {
                    zz = initFilterSignal(z_last);
                    c  = initFilterChroma(zz);
                    y0 = initFilterLuma0(zz);
                    y1 = initFilterLuma1(zz - c);

                    c = c * 2.0;
                    i = i + (c * cos(phi) - i) / 8.0;
                    q = q + (c * sin(phi) - q) / 8.0;

                    phi += ATN_RAD_45;
                }
            }

            double brightness;

            /* B&W monitor: pure brightness from final z_last. */
            brightness = clamp01(z_last);
            g_aBnWMonitor[s].b = (uint8_t)(brightness * 255);
            g_aBnWMonitor[s].g = (uint8_t)(brightness * 255);
            g_aBnWMonitor[s].r = (uint8_t)(brightness * 255);
            g_aBnWMonitor[s].a = 255;

            /* B&W color TV: brightness from y1 (filtered luma). */
            brightness = clamp01(y1);
            g_aBnwColorTV[s].b = (uint8_t)(brightness * 255);
            g_aBnwColorTV[s].g = (uint8_t)(brightness * 255);
            g_aBnwColorTV[s].r = (uint8_t)(brightness * 255);
            g_aBnwColorTV[s].a = 255;

            int color = s & 15;

            /* Color monitor: y0 + IQ -> RGB. */
            double r64 = y0 + (I_TO_R * i) + (Q_TO_R * q);
            double g64 = y0 + (I_TO_G * i) + (Q_TO_G * q);
            double b64 = y0 + (I_TO_B * i) + (Q_TO_B * q);
            double r32 = clamp01(r64), g32 = clamp01(g64), b32 = clamp01(b64);

#if ATN_REMOVE_WHITE_RINGING
            if (color == 15) { r32 = 1; g32 = 1; b32 = 1; }
#endif
#if ATN_REMOVE_BLACK_GHOSTING
            if (color == 0)  { r32 = 0; g32 = 0; b32 = 0; }
#endif
#if ATN_REMOVE_GRAY_CHROMA
            if (color == 5) {
                double g = (double)0x83 / (double)0xFF;
                r32 = g; g32 = g; b32 = g;
            }
            if (color == 10) {
                double g = (double)0x78 / (double)0xFF;
                r32 = g; g32 = g; b32 = g;
            }
#endif
            g_aHueMonitor[phase][s].b = (uint8_t)(b32 * 255);
            g_aHueMonitor[phase][s].g = (uint8_t)(g32 * 255);
            g_aHueMonitor[phase][s].r = (uint8_t)(r32 * 255);
            g_aHueMonitor[phase][s].a = 255;

            /* Color TV: y1 + IQ -> RGB. */
            r64 = y1 + (I_TO_R * i) + (Q_TO_R * q);
            g64 = y1 + (I_TO_G * i) + (Q_TO_G * q);
            b64 = y1 + (I_TO_B * i) + (Q_TO_B * q);
            r32 = clamp01(r64); g32 = clamp01(g64); b32 = clamp01(b64);
#if ATN_REMOVE_WHITE_RINGING
            if (color == 15) { r32 = 1; g32 = 1; b32 = 1; }
#endif
#if ATN_REMOVE_BLACK_GHOSTING
            if (color == 0)  { r32 = 0; g32 = 0; b32 = 0; }
#endif
            g_aHueColorTV[phase][s].b = (uint8_t)(b32 * 255);
            g_aHueColorTV[phase][s].g = (uint8_t)(g32 * 255);
            g_aHueColorTV[phase][s].r = (uint8_t)(r32 * 255);
            g_aHueColorTV[phase][s].a = 255;
        }
    }
}

/* ===== Init / framebuffer bind =================================== */

void appletini_ntsc_init(void) {
    initPixelDoubleMasks();
    initChromaPhaseTables();
    g_nColorPhaseNTSC = 0;
    g_nSignalBitsNTSC = 0;
    g_nLastColumnPixelNTSC = 0;
    g_nColorBurstPixels = 0;
    g_nTextFlashMask = 0;
    g_nVideoCharSet = 0;
    appletini_ntsc_set_video_output(0U,
                                    APPLE_VIDEO_MONO_WHITE,
                                    APPLE_VIDEO_COLOR_COMPOSITE_MONITOR);
}

void appletini_ntsc_set_video_output(uint8_t mono_enable,
                                     uint8_t mono_color,
                                     uint8_t color_mode) {
    s_video_mono_enable = (mono_enable != 0U) ? 1U : 0U;
    s_video_mono_color = apple_video_mono_color_clamp(mono_color);
    s_video_color_mode = apple_video_color_mode_clamp(color_mode);
}

void appletini_ntsc_set_framebuffer(uint32_t *fb) {
    g_atn_framebuffer = fb;
    g_pVideoAddress   = fb;
}

/* ===== Per-pixel emit ============================================ */

static inline uint32_t atn_pack_bgra(atn_bgra_t v) {
    return ((uint32_t)v.b)
         | ((uint32_t)v.g << 8)
         | ((uint32_t)v.r << 16)
         | ((uint32_t)v.a << 24);
}

static inline uint8_t atn_current_applewin_base_color_index(void) {
    return g_aAppleWinPhaseColorIndex[(uint8_t)g_nColorPhaseNTSC & 3U]
                                     [(uint8_t)g_nSignalBitsNTSC & 0x0FU];
}

static inline atn_bgra_t atn_tint_mono(atn_bgra_t v) {
    const uint32_t y = v.r;
    atn_bgra_t out;

    out.a = 255U;
    switch (s_video_mono_color) {
    case APPLE_VIDEO_MONO_BLACK:
        out.r = 0U;
        out.g = 0U;
        out.b = 0U;
        break;
    case APPLE_VIDEO_MONO_AMBER:
        out.r = (uint8_t)y;
        out.g = (uint8_t)((y * 0x80U) / 0xFFU);
        out.b = (uint8_t)((y * 0x01U) / 0xFFU);
        break;
    case APPLE_VIDEO_MONO_GREEN:
        out.r = (uint8_t)((y * 0x08U) / 0xFFU);
        out.g = (uint8_t)((y * 0xB5U) / 0xFFU);
        out.b = (uint8_t)((y * 0x52U) / 0xFFU);
        break;
    case APPLE_VIDEO_MONO_WHITE:
    default:
        out.r = (uint8_t)y;
        out.g = (uint8_t)y;
        out.b = (uint8_t)y;
        break;
    }

    return out;
}

static inline uint32_t atn_color_table_lookup(uint16_t signal) {
    g_nSignalBitsNTSC = ((g_nSignalBitsNTSC << 1) | (signal & 1)) & 0xFFF;
    switch (s_video_color_mode) {
    case APPLE_VIDEO_COLOR_IDEALIZED:
    case APPLE_VIDEO_COLOR_RGB:
        return atn_pack_bgra(g_aAppleWinBaseColors[
            atn_current_applewin_base_color_index()]);
    case APPLE_VIDEO_COLOR_TV:
        return atn_pack_bgra(g_aHueColorTV[g_nColorPhaseNTSC][g_nSignalBitsNTSC]);
    case APPLE_VIDEO_COLOR_COMPOSITE_MONITOR:
    default:
        return atn_pack_bgra(g_aHueMonitor[g_nColorPhaseNTSC][g_nSignalBitsNTSC]);
    }
}

static inline uint32_t atn_mono_table_lookup(uint16_t signal) {
    g_nSignalBitsNTSC = ((g_nSignalBitsNTSC << 1) | (signal & 1)) & 0xFFF;
    atn_bgra_t v = (s_video_mono_enable == 0U &&
                    s_video_color_mode == APPLE_VIDEO_COLOR_TV) ?
        g_aBnwColorTV[g_nSignalBitsNTSC] :
        g_aBnWMonitor[g_nSignalBitsNTSC];

    return (s_video_mono_enable != 0U) ?
        atn_pack_bgra(atn_tint_mono(v)) :
        atn_pack_bgra(v);
}

static inline void atn_advance_phase(void) {
    g_nColorPhaseNTSC = (g_nColorPhaseNTSC + 1) & 3;
}

void atn_emit_color(uint16_t signal) {
    *g_pVideoAddress++ = atn_color_table_lookup(signal);
    atn_advance_phase();
}

void atn_emit_mono(uint16_t signal) {
    *g_pVideoAddress++ = atn_mono_table_lookup(signal);
    atn_advance_phase();
}

void atn_emit_blank_pixels(uint8_t count) {
    for (uint8_t i = 0U; i < count; ++i) {
        if (!atn_get_color_burst() || s_video_mono_enable != 0U) {
            atn_emit_mono(0U);
        } else {
            atn_emit_color(0U);
        }
    }
    g_nLastColumnPixelNTSC = 0U;
}

/* updatePixels: shift 14 dots out (color burst on) or 7 dots (off),
 * tracking g_nLastColumnPixelNTSC. Bit-for-bit compatible with
 * AppleWin's NTSC.cpp:603-655.
 *
 * Color burst on: 14 dots. Bits ordered LSB-first in `bits[13:0]`.
 * Color burst off: 7 mono dots from bits[6:0]; high 7 bits ignored. */
void atn_updatePixels(uint16_t bits) {
    if (!atn_get_color_burst() || s_video_mono_enable != 0U) {
        for (int i = 0; i < 13; i++) {
            atn_emit_mono(bits & 1);
            bits >>= 1;
        }
        atn_emit_mono(bits & 1);
        g_nLastColumnPixelNTSC = bits & 1;
    } else {
        for (int i = 0; i < 13; i++) {
            atn_emit_color(bits & 1);
            bits >>= 1;
        }
        atn_emit_color(bits & 1);
        g_nLastColumnPixelNTSC = bits & 1;
    }
}
