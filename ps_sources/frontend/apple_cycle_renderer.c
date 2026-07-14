/*
 * apple_cycle_renderer.c -- See apple_cycle_renderer.h.
 *
 * Per-cycle Apple //e renderer driving AppleWin's NTSC core. Behavior
 * matches AppleWin (source/NTSC.cpp) at the bus-cycle level.
 *
 * Per-cycle dispatch is invoked by apple_cycle_egress.c via the weak
 * callback apple_cycle_renderer_on_record. Records carry a per-cycle
 * snapshot of soft-switches; we redo mode dispatch every cycle so
 * mid-line mode changes are honored.
 *
 * Output: the renderer rotates among COMP_APPLE_SLOT_COUNT (3) BGRA32
 * Apple border-ring slots in DDR (comp_apple_slot_addr[]), with per-row
 * guard pixels for shifted NTSC writes. Slot ownership is
 * managed by the atomic 3-slot handoff in apple_fb_handoff.[ch] so
 * the renderer (writer) and compositor (reader) can run at independent
 * frame rates: writer-faster-than-reader silently drops frames, reader
 * -faster-than-writer holds the last good frame. At the start of each
 * Apple frame on_frame_start() reads the writer-slot index from the
 * handoff and points the NTSC core's framebuffer at it; on frame end
 * it flushes the slot and calls apple_fb_writer_publish() which atomic-
 * ally promotes the slot to idle and rotates the writer to the freed
 * slot.
 *
 * License: GPLv2 (inherited from AppleWin).
 */

#include <stdint.h>
#include <string.h>

#include "xil_cache.h"
#include "xil_mmu.h"

#include "../lib/common.h"
#include "../lib/uart.h"

#include "apple_cycle_egress.h"
#include "apple_cycle_renderer.h"
#include "apple_fb_handoff.h"
#include "apple_pal_video_timing.h"
#include "appletini_csbits.h"
#include "appletini_ntsc.h"
#include "compositor_layout.h"

/* ---------- Public state ---------- */

volatile uint32_t g_acr_frames_complete   = 0u;
volatile uint32_t g_acr_resyncs_cleared   = 0u;
volatile uint32_t g_acr_records_seen      = 0u;
volatile uint32_t g_acr_cycles_rendered   = 0u;
volatile uint32_t g_acr_unknown_modes     = 0u;
volatile uint32_t g_acr_frame_edges_seen  = 0u;

/* Diagnostic: records dispatched between on_frame_start and on_frame_end
 * for the most-recently-completed frame. A clean NTSC frame is ~17030
 * cycles (262 lines * 65 cycles); much less indicates a partial frame. */
volatile uint32_t g_acr_last_frame_records = 0u;
static   uint32_t s_records_in_frame       = 0u;

/* ---------- Private state ---------- */

/* Per-record state to detect frame boundaries from line wrap-around. */
static int      s_prev_valid     = 0;
static uint32_t s_prev_line      = 0;
static uint8_t  s_frame_end_pending = 0u;
static uint32_t s_scanner_frame_lines = ATN_SCANNER_MAX_VERT_NTSC;
static uint32_t s_pending_line0_sw[ATN_BORDER_H_CYCLES];
static uint8_t  s_pending_line0_mask = 0u;

/* Tracks the previous line for per-line chroma-state reset. Initialised
 * to a sentinel that doesn't match any valid line (line is 9-bit ->
 * 0..511 valid range), so the first record after init forces a reset. */
static uint32_t s_chroma_prev_line = 0xFFFFFFFFu;

/* Render-armed: 0 until we see the first clean frame edge after init or
 * after a gap-marker resync. */
static int      s_render_armed   = 0;
static int      s_just_resynced  = 0;

/* Latest soft-switch state -- updated per record so per-mode step_*()
 * functions see the per-cycle state. */
static uint32_t s_current_sw     = 0u;

/* 3-slot ring ownership lives in apple_fb_handoff.c. The renderer
 * caches the writer-slot index here at on_frame_start time so the
 * per-record dispatch path can hand g_pVideoAddress a row pointer
 * without a CAS. */
static uint8_t s_cached_writer_slot = 0u;
static uint32_t s_video_settings_seen = 0xFFFFFFFFu;
static uint32_t s_video_rom_gen_seen = 0u;   /* applied video-ROM override gen */
static uint8_t s_render_mono_enable = 0u;
static uint8_t s_render_color_mode = APPLE_VIDEO_COLOR_COMPOSITE_MONITOR;
static uint8_t s_border_enabled = 0u;
static uint8_t s_border_default_color = APPLE_VIDEO_IIGS_BORDER_DEFAULT;
static uint8_t s_video7_rgb_flags = 0u;
static uint8_t s_video7_rgb_mode = 0u;
static uint8_t s_video7_prev_an3_addr = 0u;
static uint8_t s_video7_rgb_mode_seen = 0xFFu;

/*
 * PL timestamps are locked to the motherboard-visible VBL edge. The clean
 * AppleWin path and the PAL-accurate path have independent PS-side phase
 * calibrations because PL timing changes can move that capture origin. Keep
 * raw timestamps for frame-boundary/resync handling and translate only the
 * renderer coordinates.
 */
static int8_t s_clean_capture_phase_cycles =
    (int8_t)APPLE_VIDEO_DEFAULT_CLEAN_PHASE_CYCLES;
static int8_t s_pal_capture_phase_cycles =
    (int8_t)APPLE_VIDEO_DEFAULT_PAL_PHASE_CYCLES;

static inline void capture_to_scanner_phase(uint32_t raw_line,
                                            uint32_t raw_cycle,
                                            int8_t phase_cycles,
                                            uint32_t *scan_line,
                                            uint32_t *scan_cycle) {
    int32_t line = (int32_t)raw_line;
    int32_t cycle = (int32_t)raw_cycle + (int32_t)phase_cycles;

    while (cycle >= (int32_t)ATN_SCANNER_MAX_HORZ) {
        cycle -= ATN_SCANNER_MAX_HORZ;
        line += 1;
    }
    while (cycle < 0) {
        cycle += ATN_SCANNER_MAX_HORZ;
        line -= 1;
    }

    *scan_line = (line < 0) ? UINT32_MAX : (uint32_t)line;
    *scan_cycle = (uint32_t)cycle;
}

static inline uint8_t pal_positive_phase_preroll_cycle(uint32_t raw_line,
                                                       uint32_t raw_cycle,
                                                       int8_t phase_cycles,
                                                       uint32_t *scan_cycle)
{
    const int32_t shifted_cycle = (int32_t)raw_cycle + (int32_t)phase_cycles;

    if (phase_cycles <= 0 ||
        raw_line < (uint32_t)ATN_SCANNER_Y_DISPLAY ||
        shifted_cycle < (int32_t)ATN_SCANNER_MAX_HORZ) {
        return 0u;
    }
    *scan_cycle = (uint32_t)(shifted_cycle - (int32_t)ATN_SCANNER_MAX_HORZ);
    return 1u;
}

#define SHR_WIDTH  640u
#define SHR_HEIGHT 400u
#define SHR_LOGICAL_HEIGHT 200u

static uint8_t  s_vidhd_screen_color = 0u;
static uint8_t  s_vidhd_newvideo = 0u;
static uint8_t  s_vidhd_border_color = APPLE_VIDEO_IIGS_BORDER_DEFAULT;
static uint8_t  s_vidhd_shadow = 0u;
static uint8_t  s_vidhd_bw_force_seen = 0xFFu;
static uint32_t s_frame_display_mode = APPLE_FB_DISPLAY_MODE_LEGACY;
static uint32_t s_previous_frame_display_mode = APPLE_FB_DISPLAY_MODE_LEGACY;

static uint32_t s_left_border_colors[ATN_BORDER_H_CYCLES];

/* ---------- AppleWin scanner address tables (NTSC, //e only) ---------- *
 * Verbatim from NTSC.cpp:180-213 (the _DEBUG static tables that match the
 * runtime-generated versions for NTSC).                                    */

static const uint16_t g_kClockVertOffsetsHGR[262] = {
    0x0000,0x0400,0x0800,0x0C00,0x1000,0x1400,0x1800,0x1C00,0x0080,0x0480,0x0880,0x0C80,0x1080,0x1480,0x1880,0x1C80,
    0x0100,0x0500,0x0900,0x0D00,0x1100,0x1500,0x1900,0x1D00,0x0180,0x0580,0x0980,0x0D80,0x1180,0x1580,0x1980,0x1D80,
    0x0200,0x0600,0x0A00,0x0E00,0x1200,0x1600,0x1A00,0x1E00,0x0280,0x0680,0x0A80,0x0E80,0x1280,0x1680,0x1A80,0x1E80,
    0x0300,0x0700,0x0B00,0x0F00,0x1300,0x1700,0x1B00,0x1F00,0x0380,0x0780,0x0B80,0x0F80,0x1380,0x1780,0x1B80,0x1F80,
    0x0000,0x0400,0x0800,0x0C00,0x1000,0x1400,0x1800,0x1C00,0x0080,0x0480,0x0880,0x0C80,0x1080,0x1480,0x1880,0x1C80,
    0x0100,0x0500,0x0900,0x0D00,0x1100,0x1500,0x1900,0x1D00,0x0180,0x0580,0x0980,0x0D80,0x1180,0x1580,0x1980,0x1D80,
    0x0200,0x0600,0x0A00,0x0E00,0x1200,0x1600,0x1A00,0x1E00,0x0280,0x0680,0x0A80,0x0E80,0x1280,0x1680,0x1A80,0x1E80,
    0x0300,0x0700,0x0B00,0x0F00,0x1300,0x1700,0x1B00,0x1F00,0x0380,0x0780,0x0B80,0x0F80,0x1380,0x1780,0x1B80,0x1F80,
    0x0000,0x0400,0x0800,0x0C00,0x1000,0x1400,0x1800,0x1C00,0x0080,0x0480,0x0880,0x0C80,0x1080,0x1480,0x1880,0x1C80,
    0x0100,0x0500,0x0900,0x0D00,0x1100,0x1500,0x1900,0x1D00,0x0180,0x0580,0x0980,0x0D80,0x1180,0x1580,0x1980,0x1D80,
    0x0200,0x0600,0x0A00,0x0E00,0x1200,0x1600,0x1A00,0x1E00,0x0280,0x0680,0x0A80,0x0E80,0x1280,0x1680,0x1A80,0x1E80,
    0x0300,0x0700,0x0B00,0x0F00,0x1300,0x1700,0x1B00,0x1F00,0x0380,0x0780,0x0B80,0x0F80,0x1380,0x1780,0x1B80,0x1F80,
    0x0000,0x0400,0x0800,0x0C00,0x1000,0x1400,0x1800,0x1C00,0x0080,0x0480,0x0880,0x0C80,0x1080,0x1480,0x1880,0x1C80,
    0x0100,0x0500,0x0900,0x0D00,0x1100,0x1500,0x1900,0x1D00,0x0180,0x0580,0x0980,0x0D80,0x1180,0x1580,0x1980,0x1D80,
    0x0200,0x0600,0x0A00,0x0E00,0x1200,0x1600,0x1A00,0x1E00,0x0280,0x0680,0x0A80,0x0E80,0x1280,0x1680,0x1A80,0x1E80,
    0x0300,0x0700,0x0B00,0x0F00,0x1300,0x1700,0x1B00,0x1F00,0x0380,0x0780,0x0B80,0x0F80,0x1380,0x1780,0x1B80,0x1F80,
    0x0B80,0x0F80,0x1380,0x1780,0x1B80,0x1F80
};

static const uint16_t g_kClockVertOffsetsTXT[33] = {
    0x0000,0x0080,0x0100,0x0180,0x0200,0x0280,0x0300,0x0380,
    0x0000,0x0080,0x0100,0x0180,0x0200,0x0280,0x0300,0x0380,
    0x0000,0x0080,0x0100,0x0180,0x0200,0x0280,0x0300,0x0380,
    0x0000,0x0080,0x0100,0x0180,0x0200,0x0280,0x0300,0x0380,
    0x380
};

/* APPLE_IIE_HORZ_CLOCK_OFFSET[5][65] -- 5 = ceil(262/64). Table from
 * NTSC.cpp:263-309. */
static const uint16_t kAPPLE_IIE_HORZ_CLOCK_OFFSET[5][65] = {
    {0x0068,0x0068,0x0069,0x006A,0x006B,0x006C,0x006D,0x006E,0x006F,
     0x0070,0x0071,0x0072,0x0073,0x0074,0x0075,0x0076,0x0077,
     0x0078,0x0079,0x007A,0x007B,0x007C,0x007D,0x007E,0x007F,
     0x0000,0x0001,0x0002,0x0003,0x0004,0x0005,0x0006,0x0007,
     0x0008,0x0009,0x000A,0x000B,0x000C,0x000D,0x000E,0x000F,
     0x0010,0x0011,0x0012,0x0013,0x0014,0x0015,0x0016,0x0017,
     0x0018,0x0019,0x001A,0x001B,0x001C,0x001D,0x001E,0x001F,
     0x0020,0x0021,0x0022,0x0023,0x0024,0x0025,0x0026,0x0027},
    {0x0010,0x0010,0x0011,0x0012,0x0013,0x0014,0x0015,0x0016,0x0017,
     0x0018,0x0019,0x001A,0x001B,0x001C,0x001D,0x001E,0x001F,
     0x0020,0x0021,0x0022,0x0023,0x0024,0x0025,0x0026,0x0027,
     0x0028,0x0029,0x002A,0x002B,0x002C,0x002D,0x002E,0x002F,
     0x0030,0x0031,0x0032,0x0033,0x0034,0x0035,0x0036,0x0037,
     0x0038,0x0039,0x003A,0x003B,0x003C,0x003D,0x003E,0x003F,
     0x0040,0x0041,0x0042,0x0043,0x0044,0x0045,0x0046,0x0047,
     0x0048,0x0049,0x004A,0x004B,0x004C,0x004D,0x004E,0x004F},
    {0x0038,0x0038,0x0039,0x003A,0x003B,0x003C,0x003D,0x003E,0x003F,
     0x0040,0x0041,0x0042,0x0043,0x0044,0x0045,0x0046,0x0047,
     0x0048,0x0049,0x004A,0x004B,0x004C,0x004D,0x004E,0x004F,
     0x0050,0x0051,0x0052,0x0053,0x0054,0x0055,0x0056,0x0057,
     0x0058,0x0059,0x005A,0x005B,0x005C,0x005D,0x005E,0x005F,
     0x0060,0x0061,0x0062,0x0063,0x0064,0x0065,0x0066,0x0067,
     0x0068,0x0069,0x006A,0x006B,0x006C,0x006D,0x006E,0x006F,
     0x0070,0x0071,0x0072,0x0073,0x0074,0x0075,0x0076,0x0077},
    {0x0060,0x0060,0x0061,0x0062,0x0063,0x0064,0x0065,0x0066,0x0067,
     0x0068,0x0069,0x006A,0x006B,0x006C,0x006D,0x006E,0x006F,
     0x0070,0x0071,0x0072,0x0073,0x0074,0x0075,0x0076,0x0077,
     0x0078,0x0079,0x007A,0x007B,0x007C,0x007D,0x007E,0x007F,
     0x0000,0x0001,0x0002,0x0003,0x0004,0x0005,0x0006,0x0007,
     0x0008,0x0009,0x000A,0x000B,0x000C,0x000D,0x000E,0x000F,
     0x0010,0x0011,0x0012,0x0013,0x0014,0x0015,0x0016,0x0017,
     0x0018,0x0019,0x001A,0x001B,0x001C,0x001D,0x001E,0x001F},
    {0x0060,0x0060,0x0061,0x0062,0x0063,0x0064,0x0065,0x0066,0x0067,
     0x0068,0x0069,0x006A,0x006B,0x006C,0x006D,0x006E,0x006F,
     0x0070,0x0071,0x0072,0x0073,0x0074,0x0075,0x0076,0x0077,
     0x0078,0x0079,0x007A,0x007B,0x007C,0x007D,0x007E,0x007F,
     0x0000,0x0001,0x0002,0x0003,0x0004,0x0005,0x0006,0x0007,
     0x0008,0x0009,0x000A,0x000B,0x000C,0x000D,0x000E,0x000F,
     0x0010,0x0011,0x0012,0x0013,0x0014,0x0015,0x0016,0x0017,
     0x0018,0x0019,0x001A,0x001B,0x001C,0x001D,0x001E,0x001F}
};

/* g_aPixelMaskGR for getLoResBits. NTSC.cpp:1182-1183:
 *   for color 0..15: g_aPixelMaskGR[color] = (c<<12)|(c<<8)|(c<<4)|c
 * 16-bit pattern, replicated nibble. */
static uint16_t g_aPixelMaskGR[16];
static int      s_pixel_mask_gr_init = 0;

static void init_pixel_mask_gr(void) {
    for (uint16_t color = 0; color < 16; color++) {
        g_aPixelMaskGR[color] = (uint16_t)((color << 12) | (color << 8) | (color << 4) | color);
    }
    s_pixel_mask_gr_init = 1;
}

/* ---------- Soft-switch helpers ---------- */

#define SW_BIT(name) (1u << ACE_SWB_##name##_BIT)

static inline int sw_text(uint32_t sw)        { return !!(sw & SW_BIT(TEXT)); }
static inline int sw_mixed(uint32_t sw)       { return !!(sw & SW_BIT(MIXED)); }
static inline int sw_hires(uint32_t sw)       { return !!(sw & SW_BIT(HIRES)); }
static inline int sw_page2(uint32_t sw)       { return !!(sw & SW_BIT(PAGE2)); }
static inline int sw_80col(uint32_t sw)       { return !!(sw & SW_BIT(80COL)); }
static inline int sw_dhires(uint32_t sw)      { return !!(sw & SW_BIT(DHIRES)); }
static inline int sw_80store(uint32_t sw)     { return !!(sw & SW_BIT(80STORE)); }
static inline int sw_altcharset(uint32_t sw)  { return !!(sw & SW_BIT(ALTCHARSET)); }

/* ---------- Address decode (transliterated from NTSC.cpp) ---------- */

static inline uint16_t scanner_addr_txt(int line, int cycle, uint32_t sw) {
    /* The vert/horz offset tables are within a $400-aligned text page;
     * the actual Apple text page lives at $0400 (TEXT1) or $0800
     * (TEXT2). Add the page base before returning. */
    uint16_t a = (uint16_t)(g_kClockVertOffsetsTXT[line / 8]
                          + kAPPLE_IIE_HORZ_CLOCK_OFFSET[line / 64][cycle]);
    a += 0x400;
    /* PAGE2 selects $0800 page when 80STORE is clear; with 80STORE set,
     * PAGE2 routes to AUX bank instead of swapping page (caller must
     * read from g_aux_bank in that case -- step_text40 is main-only,
     * which is correct). */
    if (sw_page2(sw) && !sw_80store(sw)) a += 0x400;
    return a;
}

static inline uint16_t scanner_addr_hgr(int line, int cycle, uint32_t sw) {
    uint16_t a = (uint16_t)(g_kClockVertOffsetsHGR[line]
                          + kAPPLE_IIE_HORZ_CLOCK_OFFSET[line / 64][cycle]);
    /* HGR base is $2000; PAGE2 selects $4000 (when 80STORE clear). */
    a += 0x2000;
    if (sw_page2(sw) && !sw_80store(sw)) a += 0x2000;
    return a;
}

static inline int vidhd_shr_enabled(void) {
    return (s_vidhd_newvideo & 0xC0u) == 0xC0u;
}

static inline int vidhd_bw_forced(void) {
    return (s_vidhd_newvideo & 0x20u) != 0u;
}

static inline uint16_t shr_scanline_addr(uint32_t y, uint32_t x) {
    return (uint16_t)(0x2000u + 160u * y + 4u * x);
}

static inline uint32_t shr_pack_bgra(uint8_t r, uint8_t g, uint8_t b) {
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static inline uint32_t shr_apply_c029_bw(uint32_t bgra) {
    if (!vidhd_bw_forced()) return bgra;

    const uint8_t b = (uint8_t)(bgra & 0xFFu);
    const uint8_t g = (uint8_t)((bgra >> 8) & 0xFFu);
    const uint8_t r = (uint8_t)((bgra >> 16) & 0xFFu);
    const uint8_t y = (uint8_t)(((uint32_t)r * 77u +
                                 (uint32_t)g * 150u +
                                 (uint32_t)b * 29u) >> 8);
    return shr_pack_bgra(y, y, y);
}

static inline uint32_t shr_palette_color(uint16_t palette_base, uint8_t idx) {
    const uint16_t a = (uint16_t)(palette_base + ((uint16_t)(idx & 0x0Fu) * 2u));
    const uint16_t raw =
        (uint16_t)g_aux_bank[a] |
        (uint16_t)((uint16_t)g_aux_bank[(uint16_t)(a + 1u)] << 8);
    const uint8_t b = (uint8_t)((raw & 0x000Fu) * 16u);
    const uint8_t g = (uint8_t)(((raw >> 4) & 0x000Fu) * 16u);
    const uint8_t r = (uint8_t)(((raw >> 8) & 0x000Fu) * 16u);
    return shr_apply_c029_bw(shr_pack_bgra(r, g, b));
}

/* ---------- Mode pick ---------- */

typedef enum {
    MODE_TEXT40,
    MODE_TEXT80,
    MODE_HGR,
    MODE_DHGR,
    MODE_LORES,
    MODE_DLORES,
} render_mode_t;

static render_mode_t pick_mode(uint32_t sw, int vert) {
    int text   = sw_text(sw);
    int mixed  = sw_mixed(sw);
    int hires  = sw_hires(sw);
    int col80  = sw_80col(sw);
    int dhires = sw_dhires(sw);

    int effective_text = text || (mixed && vert >= (int)ATN_SCANNER_Y_MIXED);
    if (effective_text) return col80 ? MODE_TEXT80 : MODE_TEXT40;
    if (hires)          return (col80 && dhires) ? MODE_DHGR : MODE_HGR;
    if (col80 && dhires) return MODE_DLORES;
    if (col80)           return MODE_DLORES;
    return MODE_LORES;
}

/* ---------- Char-set bits accessor (mirrors AppleWin's getCharSetBits) ---------- */

static inline uint8_t get_char_set_bits(uint8_t ch) {
    return csbits_enhanced2e[g_nVideoCharSet][ch][g_nVideoClockVert & 7];
}

static inline uint16_t get_lores_bits(uint8_t b) {
    return g_aPixelMaskGR[(b >> (g_nVideoClockVert & 4)) & 0xF];
}

/* ---------- AppleWin Idealized/RGB cell paths ---------- *
 * Ported from AppleWin source/RGBMonitor.cpp. Idealized uses
 * UpdateHiResCell/UpdateDHiResCell/UpdateLoResCell/UpdateDLoResCell.
 * RGB uses the default Apple RGB-card paths: RGB mode 0, which means
 * HGR RGB and DHGR 140-color RGB. */

enum {
    AW_HGR_BLACK = 0,
    AW_HGR_WHITE,
    AW_HGR_BLUE,
    AW_HGR_ORANGE,
    AW_HGR_GREEN,
    AW_HGR_VIOLET,
    AW_HGR_GREY1,
    AW_HGR_GREY2,
    AW_HGR_YELLOW,
    AW_HGR_AQUA,
    AW_HGR_PURPLE,
    AW_HGR_PINK,
    AW_BLACK,
    AW_DEEP_RED,
    AW_DARK_BLUE,
    AW_MAGENTA,
    AW_DARK_GREEN,
    AW_DARK_GRAY,
    AW_BLUE,
    AW_LIGHT_BLUE,
    AW_BROWN,
    AW_ORANGE,
    AW_LIGHT_GRAY,
    AW_PINK,
    AW_GREEN,
    AW_YELLOW,
    AW_AQUA,
    AW_WHITE
};

enum {
    AW_CM_VIOLET = 0,
    AW_CM_BLUE,
    AW_CM_GREEN,
    AW_CM_ORANGE,
    AW_CM_BLACK,
    AW_CM_WHITE
};

static const uint8_t k_aw_hires_to_pal_index[6] = {
    AW_HGR_VIOLET,
    AW_HGR_BLUE,
    AW_HGR_GREEN,
    AW_HGR_ORANGE,
    AW_HGR_BLACK,
    AW_HGR_WHITE
};

static const uint8_t k_aw_double_hires_pal_index[16] = {
    AW_BLACK,    AW_DARK_BLUE, AW_DARK_GREEN, AW_BLUE,
    AW_BROWN,   AW_LIGHT_GRAY, AW_GREEN,      AW_AQUA,
    AW_DEEP_RED,AW_MAGENTA,    AW_DARK_GRAY,  AW_LIGHT_BLUE,
    AW_ORANGE,  AW_PINK,       AW_YELLOW,     AW_WHITE
};

enum {
    AW_DHIRES_LOOKUP_WIDTH = 10,
    AW_DHIRES_LOOKUP_BYTES = 256 * 256 * AW_DHIRES_LOOKUP_WIDTH
};

static uint32_t s_aw_base_packed[16];
static uint8_t s_aw_dhires_lookup[AW_DHIRES_LOOKUP_BYTES];

static inline uint32_t aw_pack_bgra(atn_bgra_t v) {
    return ((uint32_t)v.b)
         | ((uint32_t)v.g << 8)
         | ((uint32_t)v.r << 16)
         | ((uint32_t)v.a << 24);
}

static inline uint32_t aw_bgr(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)b) | ((uint32_t)g << 8) | ((uint32_t)r << 16) | 0xFF000000u;
}

static uint32_t aw_palette_color(uint8_t index) {
    if (index >= AW_BLACK && index <= AW_WHITE) {
        return aw_pack_bgra(g_aAppleWinBaseColors[index - AW_BLACK]);
    }

    switch (index) {
    case AW_HGR_BLACK:  return aw_pack_bgra(g_aAppleWinBaseColors[0]);
    case AW_HGR_WHITE:  return aw_pack_bgra(g_aAppleWinBaseColors[15]);
    case AW_HGR_BLUE:   return aw_pack_bgra(g_aAppleWinBaseColors[6]);
    case AW_HGR_ORANGE: return aw_pack_bgra(g_aAppleWinBaseColors[9]);
    case AW_HGR_GREEN:  return aw_pack_bgra(g_aAppleWinBaseColors[12]);
    case AW_HGR_VIOLET: return aw_pack_bgra(g_aAppleWinBaseColors[3]);
    case AW_HGR_GREY1:  return aw_bgr(0x80, 0x80, 0x80);
    case AW_HGR_GREY2:  return aw_bgr(0x80, 0x80, 0x80);
    case AW_HGR_YELLOW: return aw_bgr(0x9E, 0x9E, 0x00);
    case AW_HGR_AQUA:   return aw_bgr(0x00, 0xCD, 0x4A);
    case AW_HGR_PURPLE: return aw_bgr(0x61, 0x61, 0xFF);
    case AW_HGR_PINK:   return aw_bgr(0xFF, 0x32, 0xB5);
    default:            return aw_pack_bgra(g_aAppleWinBaseColors[0]);
    }
}

static inline void aw_emit_color_index(uint8_t palette_index) {
    *g_pVideoAddress++ = aw_palette_color(palette_index);
}

static inline void aw_emit_base_index(uint8_t base_index) {
    *g_pVideoAddress++ = s_aw_base_packed[base_index & 0x0F];
}

static inline uint8_t aw_rol_nib(uint8_t x) {
    return (uint8_t)((((uint8_t)(x << 1)) & 0x0FU) | ((x >> 3) & 0x01U));
}

static void aw_build_dhires_lookup_row(uint8_t *dst, uint8_t column, uint8_t byteval)
{
    enum { OFFSET = 3, SIZE = 10 };
    uint8_t color[SIZE];
    memset(color, 0, sizeof(color));

    const uint32_t pattern = (uint32_t)byteval | ((uint32_t)column << 8);
    for (int pixel = 1; pixel < 15; ++pixel) {
        if ((pattern & (1u << pixel)) == 0u) {
            continue;
        }

        const uint8_t pixelcolor = (uint8_t)(1u << ((pixel - OFFSET) & 3));
        if ((pixel >= OFFSET + 2) && (pixel < SIZE + OFFSET + 2) &&
            (pattern & (0x7u << (pixel - 4)))) {
            color[pixel - (OFFSET + 2)] |= pixelcolor;
        }
        if ((pixel >= OFFSET + 1) && (pixel < SIZE + OFFSET + 1) &&
            (pattern & (0xFu << (pixel - 4)))) {
            color[pixel - (OFFSET + 1)] |= pixelcolor;
        }
        if ((pixel >= OFFSET + 0) && (pixel < SIZE + OFFSET + 0)) {
            color[pixel - (OFFSET + 0)] |= pixelcolor;
        }
        if ((pixel >= OFFSET - 1) && (pixel < SIZE + OFFSET - 1) &&
            (pattern & (0xFu << (pixel + 1)))) {
            color[pixel - (OFFSET - 1)] |= pixelcolor;
        }
        if ((pixel >= OFFSET - 2) && (pixel < SIZE + OFFSET - 2) &&
            (pattern & (0x7u << (pixel + 2)))) {
            color[pixel - (OFFSET - 2)] |= pixelcolor;
        }
    }

    for (uint8_t x = 0; x < SIZE; ++x) {
        dst[x] = (uint8_t)(k_aw_double_hires_pal_index[color[x] & 0x0F] - AW_BLACK);
    }
}

static void aw_init_crisp_lookup_tables(void)
{
    for (uint8_t i = 0; i < 16; ++i) {
        s_aw_base_packed[i] = aw_pack_bgra(g_aAppleWinBaseColors[i]);
    }

    for (uint32_t column = 0; column < 256u; ++column) {
        for (uint32_t byteval = 0; byteval < 256u; ++byteval) {
            const uint32_t offs =
                ((column * 256u) + byteval) * (uint32_t)AW_DHIRES_LOOKUP_WIDTH;
            aw_build_dhires_lookup_row(&s_aw_dhires_lookup[offs],
                                       (uint8_t)column,
                                       (uint8_t)byteval);
        }
    }
}

static void aw_emit_dhires_source(uint32_t value, uint8_t start_x, uint8_t count)
{
    const uint8_t byteval = (uint8_t)(value & 0xFFU);
    const uint8_t column = (uint8_t)((value >> 8) & 0xFFU);
    const uint32_t offs =
        ((((uint32_t)column * 256u) + (uint32_t)byteval) *
         (uint32_t)AW_DHIRES_LOOKUP_WIDTH) + (uint32_t)start_x;
    const uint8_t *src = &s_aw_dhires_lookup[offs];

    for (uint8_t i = 0; i < count; ++i) {
        aw_emit_base_index(src[i]);
    }
}

static void step_hgr_idealized(uint32_t sw) {
    const int x = g_nVideoClockHorz - (int)ATN_SCANNER_HORZ_START;
    uint16_t addr = scanner_addr_hgr(g_nVideoClockVert, g_nVideoClockHorz, sw);
    uint8_t byteval1 = (x > 0) ? g_main_bank[(uint16_t)(addr - 1u)] : 0u;
    uint8_t byteval2 = g_main_bank[addr & 0xFFFFu];
    uint8_t byteval3 = (x < 39) ? g_main_bank[(uint16_t)(addr + 1u)] : 0u;

    if (sw_dhires(sw) && !sw_80col(sw)) {
        byteval1 &= 0x7FU;
        byteval2 &= 0x7FU;
        byteval3 &= 0x7FU;
    }

    uint8_t source[32];
    memset(source, AW_HGR_BLACK, sizeof(source));

    const uint8_t column = (uint8_t)(((byteval1 & 0xE0U) >> 3) | (byteval3 & 0x03U));
    const int prev_high_bit = (column >= 16U) ? 1 : 0;
    int pixels[11] = {0};
    pixels[0] = column & 4U;
    pixels[1] = column & 8U;
    pixels[9] = column & 1U;
    pixels[10] = column & 2U;

    for (int i = 2, mask = 1; i < 9; ++i) {
        pixels[i] = ((byteval2 & mask) != 0U) ? 1 : 0;
        mask <<= 1;
    }

    const int curr_high_bit = (byteval2 >> 7) & 1;
    if (curr_high_bit != 0) {
        if (pixels[1]) {
            if (pixels[2] || pixels[0]) {
                source[0] = AW_HGR_WHITE;
                source[16] = AW_HGR_WHITE;
            } else {
                source[0] = (prev_high_bit == 0) ? AW_HGR_BLACK : AW_HGR_ORANGE;
                source[16] = AW_HGR_BLUE;
            }
        } else if (pixels[0] && pixels[2]) {
            source[0] = AW_HGR_BLUE;
            source[16] = AW_HGR_ORANGE;
        }
    }

    int out_x = curr_high_bit;
    for (int odd = 0; odd < 2; ++odd) {
        if (odd != 0) {
            out_x = 16 + curr_high_bit;
        }

        for (int i = 2; i < 9; ++i) {
            int color = AW_CM_BLACK;
            if (pixels[i]) {
                color = AW_CM_WHITE;
                if (!(pixels[i - 1] || pixels[i + 1])) {
                    color = ((odd ^ (i & 1)) << 1) | curr_high_bit;
                }
            } else if (pixels[i - 1] && pixels[i + 1]) {
                color = ((odd ^ !(i & 1)) << 1) | curr_high_bit;
            }

            source[out_x] = k_aw_hires_to_pal_index[color];
            source[out_x + 1] = k_aw_hires_to_pal_index[color];
            out_x += 2;
        }
    }

    const uint8_t start = (uint8_t)((x & 1) * 16);
    for (uint8_t i = 0; i < 14; ++i) {
        aw_emit_color_index(source[start + i]);
    }
}

static void step_dhgr_idealized(uint32_t sw) {
    const int x = g_nVideoClockHorz - (int)ATN_SCANNER_HORZ_START;
    const int xpixel = x * 14;
    const uint16_t addr = scanner_addr_hgr(g_nVideoClockVert, g_nVideoClockHorz, sw);

    const uint8_t byteval1 = (x > 0) ? g_main_bank[(uint16_t)(addr - 1u)] : 0u;
    const uint8_t byteval2 = g_aux_bank[addr & 0xFFFFu];
    const uint8_t byteval3 = g_main_bank[addr & 0xFFFFu];
    const uint8_t byteval4 = (x < 39) ? g_aux_bank[(uint16_t)(addr + 1u)] : 0u;

    const uint32_t dwordval = (byteval1 & 0x70U) |
                              ((uint32_t)(byteval2 & 0x7FU) << 7) |
                              ((uint32_t)(byteval3 & 0x7FU) << 14) |
                              ((uint32_t)(byteval4 & 0x07U) << 21);

    uint8_t color = (uint8_t)((xpixel + 0) & 3);
    uint32_t value = dwordval >> (4 + 0 - color);
    aw_emit_dhires_source(value, color, 7);

    color = (uint8_t)((xpixel + 7) & 3);
    value = dwordval >> (4 + 7 - color);
    aw_emit_dhires_source(value, color, 7);
}

static void step_hgr_rgb(uint32_t sw) {
    const int x = g_nVideoClockHorz - (int)ATN_SCANNER_HORZ_START;
    int xoffset = x & 1;
    uint16_t addr = (uint16_t)(scanner_addr_hgr(g_nVideoClockVert, g_nVideoClockHorz, sw) - xoffset);

    const uint8_t byteval1 = (x < 2) ? 0u : g_main_bank[(uint16_t)(addr - 1u)];
    const uint8_t byteval2 = g_main_bank[addr & 0xFFFFu];
    const uint8_t byteval3 = g_main_bank[(uint16_t)(addr + 1u)];
    const uint8_t byteval4 = (x >= 38) ? 0u : g_main_bank[(uint16_t)(addr + 2u)];

    uint32_t dwordval = (byteval1 & 0x7FU) |
                        ((uint32_t)(byteval2 & 0x7FU) << 7) |
                        ((uint32_t)(byteval3 & 0x7FU) << 14) |
                        ((uint32_t)(byteval4 & 0x7FU) << 21);

    uint32_t colors[14];
    uint32_t dwordval_tmp = dwordval >> 7;
    int offset = (byteval2 & 0x80U) ? 1 : 0;
    for (int i = 0; i < 14; ++i) {
        if (i == 7) {
            offset = (byteval3 & 0x80U) ? 1 : 0;
        }
        const int color = dwordval_tmp & 0x3;
        colors[i] = offset ? aw_palette_color((uint8_t)(1 + color)) :
                             aw_palette_color((uint8_t)(6 - color));
        if ((i & 1) != 0) {
            dwordval_tmp >>= 2;
        }
    }

    const uint32_t bw[2] = {
        aw_palette_color(AW_HGR_BLACK),
        aw_palette_color(AW_HGR_WHITE)
    };

    const uint32_t mask = 0x01C0U;
    const uint32_t chck1 = 0x0140U;
    const uint32_t chck2 = 0x0080U;

    if (xoffset != 0) {
        dwordval >>= 7;
        xoffset = 7;
    }

    for (int i = xoffset; i < xoffset + 7; ++i) {
        uint32_t out;
        if (((dwordval & mask) == chck1) || ((dwordval & mask) == chck2)) {
            out = colors[i];
        } else {
            out = bw[(dwordval & chck2) ? 1 : 0];
        }
        *g_pVideoAddress++ = out;
        *g_pVideoAddress++ = out;
        dwordval >>= 1;
    }
}

static void step_dhgr_rgb(uint32_t sw) {
    const int x = g_nVideoClockHorz - (int)ATN_SCANNER_HORZ_START;
    const int xoffset = x & 1;
    const uint16_t addr = (uint16_t)(scanner_addr_hgr(g_nVideoClockVert,
                                                       g_nVideoClockHorz,
                                                       sw) - xoffset);

    const uint8_t byteval1 = g_aux_bank[addr & 0xFFFFu];
    const uint8_t byteval2 = g_main_bank[addr & 0xFFFFu];
    const uint8_t byteval3 = g_aux_bank[(uint16_t)(addr + 1u)];
    const uint8_t byteval4 = g_main_bank[(uint16_t)(addr + 1u)];

    uint32_t dwordval = (byteval1 & 0x7FU) |
                        ((uint32_t)(byteval2 & 0x7FU) << 7) |
                        ((uint32_t)(byteval3 & 0x7FU) << 14) |
                        ((uint32_t)(byteval4 & 0x7FU) << 21);

    uint32_t colors[7];
    for (int i = 0; i < 7; ++i) {
        const uint8_t bits = (uint8_t)(dwordval & 0x0FU);
        const uint8_t color = (uint8_t)(((bits & 7U) << 1) | ((bits & 8U) >> 3));
        colors[i] = aw_pack_bgra(g_aAppleWinBaseColors[color]);
        dwordval >>= 4;
    }

    static const uint8_t even_cells[14] = { 0,0,0,0, 1,1,1,1, 2,2,2,2, 3,3 };
    static const uint8_t odd_cells[14]  = { 3,3, 4,4,4,4, 5,5,5,5, 6,6,6,6 };
    const uint8_t *cells = (xoffset == 0) ? even_cells : odd_cells;
    for (uint8_t i = 0; i < 14; ++i) {
        *g_pVideoAddress++ = colors[cells[i]];
    }
}

static void step_lores_crisp(uint32_t sw) {
    const uint16_t addr = scanner_addr_txt(g_nVideoClockVert, g_nVideoClockHorz, sw);
    const uint8_t val = g_main_bank[addr & 0xFFFFu];
    const uint8_t color = (uint8_t)((val >> (g_nVideoClockVert & 4)) & 0x0F);

    for (uint8_t i = 0; i < 14; ++i) {
        aw_emit_base_index(color);
    }
}

static void step_dlores_crisp(uint32_t sw) {
    const uint16_t addr = scanner_addr_txt(g_nVideoClockVert, g_nVideoClockHorz, sw);
    uint8_t auxval = g_aux_bank[addr & 0xFFFFu];
    const uint8_t mainval = g_main_bank[addr & 0xFFFFu];

    auxval = (uint8_t)((aw_rol_nib(auxval >> 4) << 4) | aw_rol_nib(auxval & 0x0F));

    const uint8_t shift = (uint8_t)(g_nVideoClockVert & 4);
    const uint8_t aux_color = (uint8_t)((auxval >> shift) & 0x0F);
    const uint8_t main_color = (uint8_t)((mainval >> shift) & 0x0F);

    for (uint8_t i = 0; i < 7; ++i) {
        aw_emit_base_index(aux_color);
    }
    for (uint8_t i = 0; i < 7; ++i) {
        aw_emit_base_index(main_color);
    }
}

static void aw_emit_duochrome_bits(uint8_t bits, uint8_t width, uint8_t double_pixels) {
    for (uint8_t x = 0; x < width; x += double_pixels ? 2U : 1U) {
        const uint32_t color = aw_pack_bgra(g_aAppleWinBaseColors[(bits & 1U) ? 15 : 0]);
        bits >>= 1;
        *g_pVideoAddress++ = color;
        if (double_pixels) {
            *g_pVideoAddress++ = color;
        }
    }
}

static void atn_updatePixels_force_mono(uint16_t bits) {
    for (int i = 0; i < 13; i++) {
        atn_emit_mono(bits & 1);
        bits >>= 1;
    }
    atn_emit_mono(bits & 1);
    g_nLastColumnPixelNTSC = bits & 1;
}

/* ---------- Per-cycle step bodies (one cycle each, AppleWin parity) ---------- */

static inline int render_applewin_crisp_color(void) {
    return s_render_mono_enable == 0u &&
           (s_render_color_mode == APPLE_VIDEO_COLOR_IDEALIZED ||
            s_render_color_mode == APPLE_VIDEO_COLOR_RGB);
}

static int applewin_visible_left_shift(render_mode_t mode) {
    if (!atn_get_color_burst() || render_applewin_crisp_color()) {
        return 0;
    }

    switch (mode) {
        case MODE_TEXT80:
        case MODE_DHGR:
        case MODE_DLORES:
            return 3;
        default:
            return 2;
    }
}

static void position_video_address(render_mode_t mode) {
    /* Render directly into the non-cacheable writer slot. */
    if (g_nVideoClockHorz == (int)ATN_SCANNER_HORZ_START
        && g_nVideoClockVert < (int)ATN_SCANNER_Y_DISPLAY) {
        /* Bind to the visible-first-pixel position on this row.
         * Each row in the scratch buffer is ATN_SCRATCH_ROW_PIXELS
         * wide; the first ATN_SCRATCH_LEFT_BORDER_PIXELS are the
         * invisible left guard AppleWin's per-mode offset writes need
         * (NTSC.cpp:768-784), and the row has a matching right guard
         * for shifted end-of-row writes. The compositor blit already
         * skips the left guard via its source-stride param. */
        g_pVideoAddress =
            &g_atn_framebuffer[(g_nVideoClockVert + (int)ATN_ACTIVE_Y) *
                                   (int)ATN_SCRATCH_ROW_PIXELS
                               + (int)ATN_SCRATCH_LEFT_BORDER_PIXELS
                               + (int)ATN_ACTIVE_X];

        /* Mirror AppleWin's per-mode pixel-offset adjustments. These
         * are load-bearing for chroma correctness: each emit advances
         * g_nColorPhaseNTSC by 1 (mod 4), and the chroma tables are
         * built assuming the first *visible* dot lands at phase 2
         * (because AppleWin shifts -=2 into a left border, putting
         * 2 padding dots at phases 0 and 1 before the first visible
         * dot). Without these offsets the visible content lands at
         * phase 0 and colours come out rotated.
         *
         *  - NTSC color-burst-on modes get -=2 (NTSC.cpp:768-769).
         *    AppleWin explicitly excludes VT_COLOR_IDEALIZED and
         *    VT_COLOR_VIDEOCARD_RGB from this path; those modes use
         *    RGBMonitor.cpp cell renderers at the unshifted address.
         *    text40 emits with color burst off, so the gate excludes
         *    pure text rendering.
         *  - DHGR / DLORES / TEXT80 additionally get -=1 (NTSC.cpp:776-783)
         *    to align the 14M and 7M pixel grids.
         */
        const int left_shift = applewin_visible_left_shift(mode);
        if (left_shift != 0) {
            g_pVideoAddress -= left_shift;
        }
    }
}

static void emit_shifted_right_edge_pixels(render_mode_t mode) {
    if (g_nVideoClockVert >= (int)ATN_SCANNER_Y_DISPLAY ||
        g_nVideoClockHorz != (int)(ATN_SCANNER_MAX_HORZ - 1)) {
        return;
    }

    const int left_shift = applewin_visible_left_shift(mode);
    if (left_shift > 0) {
        atn_emit_blank_pixels((uint8_t)left_shift);
    }
}

static uint8_t border_row_for_line(uint32_t line, uint32_t *row)
{
    const uint32_t bottom_end = ATN_ACTIVE_HEIGHT + ATN_BORDER_V_LINES;
    const uint32_t top_start = s_scanner_frame_lines - ATN_BORDER_V_LINES;

    if (line < bottom_end) {
        *row = line + ATN_BORDER_V_LINES;
        return 1u;
    }
    if (line >= top_start && line < s_scanner_frame_lines) {
        *row = line - top_start;
        return 1u;
    }
    return 0u;
}

static void border_fill(uint32_t row, uint32_t x, uint32_t color)
{
    uint32_t *dst = &g_atn_framebuffer[
        row * ATN_SCRATCH_ROW_PIXELS + ATN_SCRATCH_LEFT_BORDER_PIXELS + x];

    for (uint32_t i = 0u; i < 14u; ++i) {
        dst[i] = color;
    }
}

static void emit_border_cycle(uint32_t line, uint32_t cycle)
{
    uint32_t row;
    uint32_t x;
    uint32_t left_index = 0u;
    uint8_t save_left = 0u;

    if (s_border_enabled == 0u || g_atn_framebuffer == NULL) {
        return;
    }

    if (cycle < ATN_BORDER_H_CYCLES) {
        const uint32_t previous_line = (line == 0u) ?
            (s_scanner_frame_lines - 1u) : (line - 1u);

        if (border_row_for_line(previous_line, &row) == 0u) {
            return;
        }
        x = ATN_BORDER_H_PIXELS + ATN_ACTIVE_WIDTH + (cycle * 14u);
    } else {
        if (border_row_for_line(line, &row) == 0u) {
            return;
        }
        if (cycle >= (ATN_SCANNER_HORZ_START - ATN_BORDER_H_CYCLES) &&
            cycle < ATN_SCANNER_HORZ_START) {
            left_index = cycle -
                (ATN_SCANNER_HORZ_START - ATN_BORDER_H_CYCLES);
            save_left = (line < ATN_ACTIVE_HEIGHT) ? 1u : 0u;
            x = left_index * 14u;
        } else if (line >= ATN_ACTIVE_HEIGHT &&
                   cycle >= ATN_SCANNER_HORZ_START &&
                   cycle < ATN_SCANNER_MAX_HORZ) {
            x = ATN_BORDER_H_PIXELS +
                ((cycle - ATN_SCANNER_HORZ_START) * 14u);
        } else {
            return;
        }
    }

    const uint32_t color = apple_video_iigs_border_bgra(s_vidhd_border_color);
    if (save_left != 0u) {
        s_left_border_colors[left_index] = color;
    }
    border_fill(row, x, color);
}

static void restore_left_border(uint32_t line, uint32_t cycle)
{
    if (s_border_enabled == 0u || line >= ATN_ACTIVE_HEIGHT ||
        cycle != (ATN_SCANNER_MAX_HORZ - 1u)) {
        return;
    }

    for (uint32_t i = 0u; i < ATN_BORDER_H_CYCLES; ++i) {
        border_fill(line + ATN_BORDER_V_LINES,
                    i * 14u,
                    s_left_border_colors[i]);
    }
}

/* TEXT40: uses g_aPixelDoubleMaskHGR + flash mask. */
static void step_text40(uint32_t sw) {
    uint16_t addr = scanner_addr_txt(g_nVideoClockVert, g_nVideoClockHorz, sw);

    if (g_nVideoClockHorz < (int)ATN_SCANNER_HORZ_COLORBURST_END
        && g_nVideoClockHorz >= (int)ATN_SCANNER_HORZ_COLORBURST_BEG) {
        if (g_nColorBurstPixels > 0) g_nColorBurstPixels--;
    } else if (g_nVideoClockVert < (int)ATN_SCANNER_Y_DISPLAY
               && g_nVideoClockHorz >= (int)ATN_SCANNER_HORZ_START) {
        uint8_t  m    = g_main_bank[addr & 0xFFFFu];
        uint8_t  c    = get_char_set_bits(m);
        if (g_nVideoCharSet == 0 && (m & 0xC0) == 0x40) {
            c ^= (uint8_t)g_nTextFlashMask;
        }
        if (s_render_mono_enable == 0u &&
            s_render_color_mode == APPLE_VIDEO_COLOR_RGB) {
            aw_emit_duochrome_bits(c, 14, 1);
        } else {
            uint16_t bits = g_aPixelDoubleMaskHGR[c & 0x7F];
            atn_updatePixels(bits);
        }
    }
}

/* TEXT80: aux byte + main byte, 14-bit packed bits. */
static void step_text80(uint32_t sw) {
    uint16_t addr = scanner_addr_txt(g_nVideoClockVert, g_nVideoClockHorz, sw);

    if (g_nVideoClockHorz < (int)ATN_SCANNER_HORZ_COLORBURST_END
        && g_nVideoClockHorz >= (int)ATN_SCANNER_HORZ_COLORBURST_BEG) {
        if (g_nColorBurstPixels > 0) g_nColorBurstPixels--;
    } else if (g_nVideoClockVert < (int)ATN_SCANNER_Y_DISPLAY
               && g_nVideoClockHorz >= (int)ATN_SCANNER_HORZ_START) {
        uint8_t m = g_main_bank[addr & 0xFFFFu];
        uint8_t a = g_aux_bank[addr & 0xFFFFu];
        uint16_t main = get_char_set_bits(m);
        uint16_t aux  = get_char_set_bits(a);
        if (g_nVideoCharSet == 0 && (m & 0xC0) == 0x40) main ^= g_nTextFlashMask;
        if (g_nVideoCharSet == 0 && (a & 0xC0) == 0x40) aux  ^= g_nTextFlashMask;
        uint16_t bits = (uint16_t)((main << 7) | (aux & 0x7F));
        if (s_render_mono_enable == 0u &&
            s_render_color_mode == APPLE_VIDEO_COLOR_RGB) {
            aw_emit_duochrome_bits((uint8_t)aux, 7, 0);
            aw_emit_duochrome_bits((uint8_t)main, 7, 0);
            g_nLastColumnPixelNTSC = (uint16_t)((bits >> 14) & 1);
            return;
        }
        if (!render_applewin_crisp_color()) {
            bits = (uint16_t)((bits << 1) | g_nLastColumnPixelNTSC);
        }
        atn_updatePixels(bits);
        /* TEXT80 (and DHGR/DLORES) pre-shift the pattern, so the
         * dot held over for the next column is bit 14 of the
         * shifted value -- not the bit 13 that atn_updatePixels
         * just saved. Override here so the next column's pre-shift
         * chains correctly. Without this, alternating columns'
         * first pixel gets the wrong carry-in, producing visibly
         * uneven white-pixel widths. AppleWin skips this pre-shift
         * for VT_COLOR_IDEALIZED and VT_COLOR_VIDEOCARD_RGB. */
        g_nLastColumnPixelNTSC = (uint16_t)((bits >> 14) & 1);
    }
}

/* HGR (single hires): main only, with high-bit half-shift. */
static void step_hgr(uint32_t sw) {
    uint16_t addr = scanner_addr_hgr(g_nVideoClockVert, g_nVideoClockHorz, sw);

    if (g_nVideoClockVert < (int)ATN_SCANNER_Y_DISPLAY) {
        if (g_nVideoClockHorz < (int)ATN_SCANNER_HORZ_COLORBURST_END
            && g_nVideoClockHorz >= (int)ATN_SCANNER_HORZ_COLORBURST_BEG) {
            g_nColorBurstPixels = 1024;
        } else if (g_nVideoClockHorz >= (int)ATN_SCANNER_HORZ_START) {
            if (s_render_mono_enable == 0u &&
                s_render_color_mode == APPLE_VIDEO_COLOR_IDEALIZED) {
                step_hgr_idealized(sw);
                return;
            }
            if (s_render_mono_enable == 0u &&
                s_render_color_mode == APPLE_VIDEO_COLOR_RGB &&
                !(sw_dhires(sw) && !sw_80col(sw))) {
                step_hgr_rgb(sw);
                return;
            }
            uint8_t  m    = g_main_bank[addr & 0xFFFFu];
            uint16_t bits = g_aPixelDoubleMaskHGR[m & 0x7F];
            if (m & 0x80) {
                bits = (uint16_t)((bits << 1) | g_nLastColumnPixelNTSC);
            }
            if (s_render_mono_enable == 0u &&
                s_render_color_mode == APPLE_VIDEO_COLOR_RGB) {
                atn_updatePixels_force_mono(bits);
            } else {
                atn_updatePixels(bits);
            }
            /* Do not replace g_nLastColumnPixelNTSC with bits>>14.
             * AppleWin's HGR path (NTSC.cpp:1632) retains the bit-13 value
             * saved by atn_updatePixels for the next column's half-shift
             * carry-in. position_video_address owns pixel-offset alignment. */
            if (g_nVideoClockHorz == (int)(ATN_SCANNER_MAX_HORZ - 1)) {
                g_nLastColumnPixelNTSC = 0;
            }
        }
    }
}

/* DHGR (double hires 80): aux + main, 14-bit packed with last-column carry. */
static void step_dhgr(uint32_t sw) {
    uint16_t addr = scanner_addr_hgr(g_nVideoClockVert, g_nVideoClockHorz, sw);

    if (g_nVideoClockVert < (int)ATN_SCANNER_Y_DISPLAY) {
        if (g_nVideoClockHorz < (int)ATN_SCANNER_HORZ_COLORBURST_END
            && g_nVideoClockHorz >= (int)ATN_SCANNER_HORZ_COLORBURST_BEG) {
            g_nColorBurstPixels = 1024;
        } else if (g_nVideoClockHorz >= (int)ATN_SCANNER_HORZ_START) {
            if (s_render_mono_enable == 0u &&
                s_render_color_mode == APPLE_VIDEO_COLOR_IDEALIZED) {
                step_dhgr_idealized(sw);
                return;
            }
            if (s_render_mono_enable == 0u &&
                s_render_color_mode == APPLE_VIDEO_COLOR_RGB) {
                step_dhgr_rgb(sw);
                return;
            }
            uint8_t m = g_main_bank[addr & 0xFFFFu];
            uint8_t a = g_aux_bank[addr & 0xFFFFu];
            uint16_t bits = (uint16_t)(((m & 0x7F) << 7) | (a & 0x7F));
            bits = (uint16_t)((bits << 1) | g_nLastColumnPixelNTSC);
            atn_updatePixels(bits);
            g_nLastColumnPixelNTSC = (uint16_t)((bits >> 14) & 1);
        }
    }
}

/* LORES (single lores 40): main only, 4-bit nibble per row half. */
static void step_lores(uint32_t sw) {
    uint16_t addr = scanner_addr_txt(g_nVideoClockVert, g_nVideoClockHorz, sw);

    if (g_nVideoClockVert < (int)ATN_SCANNER_Y_DISPLAY) {
        if (g_nVideoClockHorz < (int)ATN_SCANNER_HORZ_COLORBURST_END
            && g_nVideoClockHorz >= (int)ATN_SCANNER_HORZ_COLORBURST_BEG) {
            g_nColorBurstPixels = 1024;
        } else if (g_nVideoClockHorz >= (int)ATN_SCANNER_HORZ_START) {
            if (render_applewin_crisp_color()) {
                step_lores_crisp(sw);
                return;
            }
            uint8_t  m    = g_main_bank[addr & 0xFFFFu];
            uint16_t lo   = get_lores_bits(m);
            uint16_t bits = (uint16_t)(lo >> ((1 - (g_nVideoClockHorz & 1)) * 2));
            atn_updatePixels(bits);
        }
    }
}

/* DLORES (double lores 80): aux + main, 7-bit halves combined. */
static void step_dlores(uint32_t sw) {
    uint16_t addr = scanner_addr_txt(g_nVideoClockVert, g_nVideoClockHorz, sw);

    if (g_nVideoClockVert < (int)ATN_SCANNER_Y_DISPLAY) {
        if (g_nVideoClockHorz < (int)ATN_SCANNER_HORZ_COLORBURST_END
            && g_nVideoClockHorz >= (int)ATN_SCANNER_HORZ_COLORBURST_BEG) {
            g_nColorBurstPixels = 1024;
        } else if (g_nVideoClockHorz >= (int)ATN_SCANNER_HORZ_START) {
            if (render_applewin_crisp_color()) {
                step_dlores_crisp(sw);
                return;
            }
            uint8_t m = g_main_bank[addr & 0xFFFFu];
            uint8_t a = g_aux_bank[addr & 0xFFFFu];
            uint16_t lo = get_lores_bits(m);
            uint16_t hi = get_lores_bits(a);
            uint16_t main = (uint16_t)(lo >> (((1 - (g_nVideoClockHorz & 1)) * 2) + 3));
            uint16_t aux  = (uint16_t)(hi >> (((1 - (g_nVideoClockHorz & 1)) * 2) + 3));
            uint16_t bits = (uint16_t)((main << 7) | (aux & 0x7F));
            atn_updatePixels(bits);
            g_nLastColumnPixelNTSC = (uint16_t)((bits >> 14) & 1);
        }
    }
}

static void shr_render_cell_320(uint32_t *row0, uint32_t x,
                                uint32_t a, uint16_t palette_base,
                                int color_fill)
{
    uint32_t *dst = row0 + x * 16u;

    for (uint32_t i = 0u; i < 4u; ++i) {
        const uint8_t byte = (uint8_t)(a & 0xFFu);
        const uint8_t pixel1 = (uint8_t)((byte >> 4) & 0x0Fu);
        uint32_t color1 = shr_palette_color(palette_base, pixel1);
        if (color_fill && pixel1 == 0u) {
            color1 = (dst != row0) ? *(dst - 1) : 0u;
        }
        *dst++ = color1;
        *dst++ = color1;

        const uint8_t pixel2 = (uint8_t)(byte & 0x0Fu);
        uint32_t color2 = shr_palette_color(palette_base, pixel2);
        if (color_fill && pixel2 == 0u) color2 = color1;
        *dst++ = color2;
        *dst++ = color2;

        a >>= 8;
    }
}

static void shr_render_cell_640(uint32_t *row0, uint32_t x,
                                uint32_t a, uint16_t palette_base)
{
    uint32_t *dst = row0 + x * 16u;

    for (uint32_t i = 0u; i < 4u; ++i) {
        const uint8_t byte = (uint8_t)(a & 0xFFu);
        const uint8_t pixel1 = (uint8_t)((byte >> 6) & 0x03u);
        *dst++ = shr_palette_color(palette_base, (uint8_t)(0x8u + pixel1));

        const uint8_t pixel2 = (uint8_t)((byte >> 4) & 0x03u);
        *dst++ = shr_palette_color(palette_base, (uint8_t)(0xCu + pixel2));

        const uint8_t pixel3 = (uint8_t)((byte >> 2) & 0x03u);
        *dst++ = shr_palette_color(palette_base, (uint8_t)(0x0u + pixel3));

        const uint8_t pixel4 = (uint8_t)(byte & 0x03u);
        *dst++ = shr_palette_color(palette_base, (uint8_t)(0x4u + pixel4));

        a >>= 8;
    }
}

static void render_shr_cell(uint32_t y, uint32_t x)
{
    const uint16_t addr = shr_scanline_addr(y, x);
    const uint32_t a =
        (uint32_t)g_aux_bank[addr] |
        ((uint32_t)g_aux_bank[(uint16_t)(addr + 1u)] << 8) |
        ((uint32_t)g_aux_bank[(uint16_t)(addr + 2u)] << 16) |
        ((uint32_t)g_aux_bank[(uint16_t)(addr + 3u)] << 24);
    const uint8_t control = g_aux_bank[0x9D00u + (uint16_t)y];
    const uint16_t palette_base = (uint16_t)(0x9E00u + ((uint16_t)(control & 0x0Fu) * 32u));
    const int is_640 = (control & 0x80u) != 0u;
    const int color_fill = (control & 0x20u) != 0u;
    uint32_t *row0 = &g_atn_framebuffer[(y * 2u) * SHR_WIDTH];
    uint32_t *row1 = row0 + SHR_WIDTH;

    if (is_640) {
        shr_render_cell_640(row0, x, a, palette_base);
    } else {
        shr_render_cell_320(row0, x, a, palette_base, color_fill);
    }
    memcpy(row1 + x * 16u, row0 + x * 16u, 16u * sizeof(uint32_t));
}

static void render_shr_frame_full(void)
{
    if (g_atn_framebuffer == NULL) {
        return;
    }

    for (uint32_t y = 0u; y < SHR_LOGICAL_HEIGHT; ++y) {
        for (uint32_t x = 0u; x < 40u; ++x) {
            render_shr_cell(y, x);
        }
    }
}

/* ---------- Frame end / start ---------- */

/* Rebuild csbits from the SD video-ROM override buffer when CPU0 bumps the
 * handoff generation. gen 0 = no override -> baked Enhanced US ROM. CPU0 has
 * already validated the buffer (size/sanity) before bumping the generation. */
static void apply_video_rom_if_changed(void) {
    const uint32_t gen = apple_fb_video_rom_gen_get();
    if (gen == s_video_rom_gen_seen) {
        return;
    }
    s_video_rom_gen_seen = gen;
    if (gen != 0u) {
        appletini_video_rom_use_override(
            (const uint8_t *)(uintptr_t)APPLE_VIDEO_ROM_OVERRIDE_ADDR);
    } else {
        appletini_video_rom_use_baked();
    }
    apple_pal_video_reset();
}

static void apply_video_settings_if_changed(void) {
    apply_video_rom_if_changed();
    const uint32_t settings = apple_fb_video_settings_get();
    const uint8_t border_default =
        apple_video_settings_border_color(settings);
    const uint8_t bw_force = vidhd_bw_forced() ? 1u : 0u;
    const int8_t clean_phase =
        apple_video_settings_clean_phase_cycles(settings);
    const int8_t pal_phase =
        apple_video_settings_pal_phase_cycles(settings);
    const int8_t old_pal_phase = s_pal_capture_phase_cycles;

    if (settings != s_video_settings_seen) {
        s_border_enabled = apple_video_settings_border_enabled(settings);
        if (border_default != s_border_default_color) {
            s_vidhd_border_color = border_default;
        }
        s_border_default_color = border_default;
    }

    if (settings == s_video_settings_seen &&
        bw_force == s_vidhd_bw_force_seen &&
        s_video7_rgb_mode == s_video7_rgb_mode_seen) {
        return;
    }

    const uint8_t user_mono = apple_video_settings_mono_enabled(settings);
    const uint8_t video7_auto_mono =
        ((user_mono == 0u) &&
         (apple_video_settings_video7_auto_mono_enabled(settings) != 0u) &&
         (s_video7_rgb_mode == 3u)) ? 1u : 0u;
    const uint8_t effective_mono =
        ((user_mono != 0u) || (bw_force != 0u) || (video7_auto_mono != 0u)) ? 1u : 0u;
    const uint8_t mono_color = ((bw_force != 0u) || (video7_auto_mono != 0u)) ?
        APPLE_VIDEO_MONO_WHITE : apple_video_settings_mono_color(settings);

    appletini_ntsc_set_video_output(
        effective_mono,
        mono_color,
        apple_video_settings_color_mode(settings));
    apple_pal_video_set_video_output(
        effective_mono,
        mono_color,
        apple_video_settings_color_mode(settings));
    if (old_pal_phase != pal_phase) {
        apple_pal_video_reset();
    }
    s_render_mono_enable = effective_mono;
    s_render_color_mode = apple_video_settings_color_mode(settings);
    s_clean_capture_phase_cycles = clean_phase;
    s_pal_capture_phase_cycles = pal_phase;
    s_video_settings_seen = settings;
    s_vidhd_bw_force_seen = bw_force;
    s_video7_rgb_mode_seen = s_video7_rgb_mode;
}

void apple_cycle_renderer_reset_local_video_state(void)
{
    const uint32_t text_sw = SW_BIT(TEXT);
    const uint32_t settings = apple_fb_video_settings_get();

    s_prev_valid          = 0;
    s_prev_line           = 0;
    s_frame_end_pending   = 0u;
    s_scanner_frame_lines = ATN_SCANNER_MAX_VERT_NTSC;
    s_pending_line0_mask  = 0u;
    s_chroma_prev_line    = 0xFFFFFFFFu;
    s_render_armed        = 0;
    s_just_resynced       = 1;
    s_current_sw          = text_sw;
    s_records_in_frame    = 0u;

    s_vidhd_screen_color  = 0u;
    s_vidhd_newvideo      = 0u;
    s_border_enabled      = apple_video_settings_border_enabled(settings);
    s_border_default_color = apple_video_settings_border_color(settings);
    s_vidhd_border_color  = s_border_default_color;
    s_vidhd_shadow        = 0u;
    s_vidhd_bw_force_seen = 0xFFu;
    s_video7_rgb_flags    = 0u;
    s_video7_rgb_mode     = 0u;
    s_video7_prev_an3_addr = 0u;
    s_video7_rgb_mode_seen = 0xFFu;

    s_frame_display_mode = APPLE_FB_DISPLAY_MODE_LEGACY;
    s_previous_frame_display_mode = APPLE_FB_DISPLAY_MODE_LEGACY;

    apple_pal_video_reset();
    g_resync_pending = 1u;
    apply_video_settings_if_changed();
}

static void handle_video7_softswitch_record(uint64_t rec)
{
    const uint16_t addr = ace_softswitch_access_addr(rec);
    const uint8_t low = (uint8_t)(addr & 0xFFu);

    if ((low & 0xFEu) != 0x5Eu) {
        return;
    }

    if (low == 0x5Fu && s_video7_prev_an3_addr == 0x5Eu) {
        const uint8_t old_mode = s_video7_rgb_mode;

        s_video7_rgb_flags = (uint8_t)((s_video7_rgb_flags << 1) & 0x03u);
        s_video7_rgb_flags |= sw_80col(ace_softswitch_bits(rec)) ? 0u : 1u;
        s_video7_rgb_mode = s_video7_rgb_flags;

        if (s_video7_rgb_mode != old_mode) {
            apply_video_settings_if_changed();
        }
    }

    s_video7_prev_an3_addr = low;
}

static void handle_vidhd_io_record(uint64_t rec)
{
    const uint16_t addr = ace_io_addr(rec);
    const uint8_t data = ace_io_data(rec);
    const int was_shr = vidhd_shr_enabled();

    switch ((uint32_t)addr) {
    case 0xC022U:
        s_vidhd_screen_color = data;
        break;
    case 0xC029U:
        s_vidhd_newvideo = data;
        apply_video_settings_if_changed();
        break;
    case 0xC034U:
        s_vidhd_border_color = apple_video_iigs_border_color_clamp(data);
        break;
    case 0xC035U:
        s_vidhd_shadow = data;
        break;
    default:
        break;
    }

    if (was_shr != vidhd_shr_enabled()) {
        s_frame_display_mode = vidhd_shr_enabled() ?
            APPLE_FB_DISPLAY_MODE_SHR : APPLE_FB_DISPLAY_MODE_LEGACY;
    }
}

static void on_frame_end(void) {
    const uint8_t pal_frame_ready = apple_pal_video_end_frame();

    g_acr_frame_edges_seen++;

    /* Slot region is mapped non-cacheable on both cores (see
     * apple_cycle_renderer_init() and compositor_init()), so all
     * pixel stores from this core have already gone straight to
     * DDR. No cache flush needed; the dsb makes the writes visible
     * before the publish CAS happens. */
    __asm__ volatile ("dsb sy");

    /* Atomic handoff: claim the current idle slot as our next writer
     * slot, demote our just-finished slot to idle, set idle_ready=1.
     * Reader (compositor) picks up the new idle next time it polls.
     *
     * PAL accurate rendering is line-deferred. CPU1 may spend later Apple
     * frames finishing one accepted exact PAL capture; only publish when that
     * delayed frame is complete, never while a writer slot contains partial
     * PAL scanlines. */
    if (pal_frame_ready != 0u) {
        apple_fb_writer_publish_frame(s_frame_display_mode,
                                      s_vidhd_border_color);
        g_acr_frames_complete++;
    }
    g_acr_last_frame_records = s_records_in_frame;
    s_records_in_frame = 0u;
}

static void on_frame_start(void) {
    /* Bind the NTSC core to the fresh writer slot selected by the handoff
     * before the first cycle of the frame writes pixels. */
    s_cached_writer_slot = apple_fb_writer_slot();

    uint32_t *slot_addr =
        (uint32_t *)(uintptr_t)comp_apple_slot_addr[s_cached_writer_slot];
    s_frame_display_mode = vidhd_shr_enabled() ?
        APPLE_FB_DISPLAY_MODE_SHR : APPLE_FB_DISPLAY_MODE_LEGACY;

    /* Resync zero: when a gap kicked us out of arming and we are
     * picking back up, the writer slot may hold a partial paint from
     * the previous (interrupted) attempt -- zero it so any unpainted
     * region shows black instead of stale pixels. The atomic handoff
     * guarantees the writer slot is *not* the reader slot, so this
     * memset is safe with respect to the compositor.
     *
     * Outside the resync path the writer slot still holds 2-frames-
     * ago content, but since we are about to paint a complete frame
     * over it (on_frame_end only fires after a full line-wrap), every
     * visible pixel will be overwritten before we publish. */
    if (s_just_resynced || s_frame_display_mode != s_previous_frame_display_mode) {
        memset(slot_addr, 0, COMP_APPLE_BYTES);
        s_just_resynced = 0;
    }
    s_previous_frame_display_mode = s_frame_display_mode;

    /* Reset per-frame record counter so g_acr_last_frame_records
     * tells us how many cycles got dispatched into this slot before
     * its on_frame_end published. << 17000 indicates a bogus wrap
     * detection fired on a partial paint. */
    s_records_in_frame = 0u;

    /* Flash counter: AppleWin uses 16-frame period. */
    if ((g_acr_frames_complete % 16u) == 0u) {
        g_nTextFlashMask = (uint16_t)(g_nTextFlashMask ^ 0xFFFFu);
    }

    /* Re-bind the NTSC core's destination. position_video_address()
     * during cycle dispatch recomputes g_pVideoAddress per row from
     * g_atn_framebuffer. */
    appletini_ntsc_set_framebuffer(slot_addr);
    apple_pal_video_set_framebuffer(slot_addr);
    apply_video_settings_if_changed();
    apple_pal_video_begin_frame();
    if (s_frame_display_mode == APPLE_FB_DISPLAY_MODE_SHR) {
        render_shr_frame_full();
    }
    /* Do NOT reinitialise g_nColorBurstPixels here. Resetting the
     * counter at every frame start gave a chroma-warmup artifact
     * on the first ~4 scanlines (mono rendering until the counter
     * decrements through its initial value during colorburst
     * cycles). The counter is initialised once at renderer boot
     * and the modes themselves drive it: step_hgr/step_dhgr/etc
     * set it back to 1024 during their colorburst cycles, so it
     * stays at the right value as long as the relevant mode is
     * actually running. */
}

/* ---------- Init ---------- */

int apple_cycle_renderer_init(void) {
    /* Mark the apple FB slot region non-cacheable on the calling
     * core (CPU1 in AMP, CPU0 in single-core builds). The slots
     * are written by the renderer here and read by the compositor
     * on the other core; if either side caches them we have to
     * flush/invalidate explicitly, which on USE_AMP=1 BSPs cannot
     * touch L2 (xil_cache.c L2 ops are stripped from libxil under
     * USE_AMP). Marking the region uncached on both cores makes
     * the pixel writes go straight to DDR and the compositor's
     * reads come straight from DDR. The cost is store coalescing
     * (no write-back buffer combining), which is acceptable here:
     * the compositor's 1080p output ring is the dominant DDR
     * bandwidth consumer, not the small Apple source slots.
     *
     * Section size for Xil_SetTlbAttributes is 1 MB, and the three
     * slots are each 1 MB aligned, so we mark all three sections
     * individually. */
    for (uint32_t i = 0u; i < COMP_APPLE_SLOT_COUNT; ++i) {
        Xil_SetTlbAttributes(comp_apple_slot_addr[i], NORM_NONCACHE);
    }

    /* Zero the slots after applying the non-cacheable mapping so writes
     * reach DDR directly. */
    for (uint32_t i = 0u; i < COMP_APPLE_SLOT_COUNT; ++i) {
        void *slot = (void *)(uintptr_t)comp_apple_slot_addr[i];
        memset(slot, 0, COMP_APPLE_BYTES);
    }
    __asm__ volatile ("dsb sy");

    /* Initialize NTSC chroma tables and the pixel-double mask. */
    appletini_csbits_init();
    appletini_ntsc_init();
    init_pixel_mask_gr();
    aw_init_crisp_lookup_tables();

    /* CPU0 owns the shared handoff/control reset. CPU1 only needs the
     * matching OCM mapping and its local writer cache initialized. */
    apple_fb_handoff_secondary_init();
    s_cached_writer_slot = apple_fb_writer_slot();

    /* Bind the initial writer slot so g_atn_framebuffer is non-NULL
     * before the first frame edge. on_frame_start() re-binds on every
     * frame boundary. */
    appletini_ntsc_set_framebuffer(
        (uint32_t *)(uintptr_t)comp_apple_slot_addr[s_cached_writer_slot]);
    apple_pal_video_set_framebuffer(
        (uint32_t *)(uintptr_t)comp_apple_slot_addr[s_cached_writer_slot]);

    /* Reset per-record state to the same TEXT/C051 baseline used after
     * Apple-side reset. */
    apple_cycle_renderer_reset_local_video_state();
    s_just_resynced = 0;
    g_resync_pending = 0u;

    __asm__ volatile ("dsb sy");

    uart_puts(UART0_BASE,
        "apple_cycle_renderer_init: 3-slot Apple FB ring at 0x3F300000+, "
        "legacy 616x224 / VidHD SHR 640x400 BGRA32\r\n");
    return 0;
}

/* ---------- Per-record dispatch ---------- */

void apple_cycle_renderer_on_record(uint64_t rec) {
    g_acr_records_seen++;

    /* Gap marker: 2b.1 already set g_resync_pending. We hold output
     * until the next clean frame edge. */
    if (rec == 0ULL) {
        s_render_armed = 0;
        s_frame_end_pending = 0u;
        s_pending_line0_mask = 0u;
        apple_pal_video_resync();
        return;
    }

    if (ace_record_kind(rec) == ACE_RECORD_KIND_IO_WRITE) {
        handle_vidhd_io_record(rec);
        return;
    }

    if (ace_record_kind(rec) == ACE_RECORD_KIND_SOFTSW_ACCESS) {
        handle_video7_softswitch_record(rec);
        return;
    }

    /* Rule-1 only: shadow already updated by 2b.1. Nothing to render. */
    if (!ace_frame_en(rec)) return;

    uint32_t line  = ace_line_in_frame(rec);
    uint32_t cycle = ace_cycle_in_line(rec);
    uint32_t sw    = ace_softswitch_bits(rec);
    const int shr_active = vidhd_shr_enabled();
    const int shr_frame_marker =
        shr_active && line == 0u && cycle == 0u;

    if (line >= ATN_SCANNER_MAX_VERT_NTSC) {
        s_scanner_frame_lines = ATN_SCANNER_MAX_VERT_PAL;
    }

    /* Frame boundary detection.
     *
     * A NTSC frame wraps from line 261 (the last line of vblank)
     * back to line 0. The legitimate end-of-frame signature is
     * therefore "line == 0 and we just saw a line >= 200". The
     * earlier broader rule "any backward line move" caught spurious
     * mid-frame counter resets that the PL apple_timing_gen issues
     * during initial calibration -- and republished a partial slot.
     * When fake-SHR is active the PL suppresses the per-cycle stream and
     * emits only one line-0/cycle-0 marker per frame; treat each marker
     * after the first as a complete frame edge. */
    if (s_prev_valid && line == 0u &&
        (s_prev_line >= 200u || (shr_frame_marker && s_render_armed))) {
        if (s_render_armed) {
            if (s_prev_line >= 200u) {
                s_scanner_frame_lines =
                    (s_prev_line >= 300u) ? ATN_SCANNER_MAX_VERT_PAL :
                                            ATN_SCANNER_MAX_VERT_NTSC;
            }
            if (shr_frame_marker) {
                on_frame_end();
                on_frame_start();
            } else {
                s_frame_end_pending = 1u;
                s_pending_line0_mask = 0u;
            }
        }
    }
    s_prev_line  = line;
    s_prev_valid = 1;

    /* Resync handshake. */
    if (g_resync_pending) {
        if (line == 0u && cycle == 0u) {
            g_resync_pending = 0u;
            g_acr_resyncs_cleared++;
            s_just_resynced = 1;
            s_render_armed  = 1;
            on_frame_start();
        } else {
            return;
        }
    }
    if (!s_render_armed) {
        /* First-frame arming: arm at the next clean line==0 cycle==0. */
        if (line == 0u && cycle == 0u) {
            s_render_armed = 1;
            on_frame_start();
        } else {
            return;
        }
    }

    /* Cycles 0 and 1 draw the right border of the previous scanner line.
     * Keep the same writer slot through those two cycles, then publish it and
     * open the new frame before line 0 reaches any active pixels. */
    if (s_frame_end_pending != 0u) {
        if (line == 0u && cycle < ATN_BORDER_H_CYCLES) {
            uint32_t border_line;
            uint32_t border_cycle;
            const int8_t phase =
                (apple_pal_video_mode_is_active(s_render_color_mode) != 0u) ?
                s_pal_capture_phase_cycles : s_clean_capture_phase_cycles;

            capture_to_scanner_phase(line, cycle, phase,
                                     &border_line, &border_cycle);
            emit_border_cycle(border_line, border_cycle);
            s_pending_line0_sw[cycle] = sw;
            s_pending_line0_mask |= (uint8_t)(1u << cycle);
            g_acr_cycles_rendered++;
            s_records_in_frame++;
            return;
        }

        on_frame_end();
        on_frame_start();
        s_frame_end_pending = 0u;
        if (apple_pal_video_mode_is_active(s_render_color_mode) != 0u) {
            for (uint32_t i = 0u; i < ATN_BORDER_H_CYCLES; ++i) {
                if ((s_pending_line0_mask & (uint8_t)(1u << i)) != 0u) {
                    apple_pal_video_on_cycle(0u, i, s_pending_line0_sw[i]);
                }
            }
        }
        s_pending_line0_mask = 0u;
    }

    if (shr_active) {
        /* C029 SHR owns the frame geometry and pixel decode while active.
         * on_frame_start() renders the complete AUX-shadow frame; sparse
         * frame markers only publish the finished slot. */
        g_acr_cycles_rendered++;
        s_records_in_frame++;
        return;
    }

    if (apple_pal_video_mode_is_active(s_render_color_mode) != 0u) {
        uint32_t pal_line;
        uint32_t pal_cycle;
        uint32_t pal_preroll_cycle;

        /* The Accurapple PAL model consumes raw 65-cycle lines where cycles
         * 0..24 are HBL and 25..64 are visible memory scan. Apply only the
         * PAL calibration here; frame-boundary handling above still uses raw
         * PL line/cycle timestamps. Positive phase pulls the first shifted
         * line-0 cycles from the tail of the raw VBL frame, so capture those
         * as preroll before the raw line-0 frame edge opens the next slot. */
        if (pal_positive_phase_preroll_cycle(line,
                                             cycle,
                                             s_pal_capture_phase_cycles,
                                             &pal_preroll_cycle) != 0u) {
            apple_pal_video_preroll_line0_cycle(pal_preroll_cycle, sw);
        }
        capture_to_scanner_phase(line,
                                 cycle,
                                 s_pal_capture_phase_cycles,
                                 &pal_line,
                                 &pal_cycle);
        emit_border_cycle(pal_line, pal_cycle);
        if (pal_line >= (uint32_t)ATN_SCANNER_Y_DISPLAY ||
            pal_cycle >= (uint32_t)ATN_SCANNER_MAX_HORZ) {
            s_records_in_frame++;
            return;
        }
        s_current_sw = sw;
        apple_pal_video_on_cycle(pal_line, pal_cycle, sw);
        g_acr_cycles_rendered++;
        s_records_in_frame++;
        return;
    }

    uint32_t render_line;
    uint32_t render_cycle;
    capture_to_scanner_phase(line,
                             cycle,
                             s_clean_capture_phase_cycles,
                             &render_line,
                             &render_cycle);
    emit_border_cycle(render_line, render_cycle);

    /* Only vblank lines may bypass dispatch. NTSC chroma state advances on
     * every visible cycle; the step_*() functions handle their own no-op
     * intervals without skipping that state progression. */
    const uint32_t visible_lines = (uint32_t)ATN_SCANNER_Y_DISPLAY;
    if (render_line >= visible_lines) {
        s_records_in_frame++;
        return;
    }

    /* Real NTSC hardware locks chroma phase to colorburst at each scanline,
     * so reset chroma phase, signal history, and last-column carry once per
     * line. Otherwise HGR/DHGR color depends on accumulated state from other
     * lines. s_prev_line supplies the transition already used for frame-edge
     * detection. */
    if (s_chroma_prev_line != render_line) {
        g_nColorPhaseNTSC      = 0;
        g_nLastColumnPixelNTSC = 0;
        g_nSignalBitsNTSC      = 0;
        s_chroma_prev_line     = render_line;
    }

    g_nVideoClockVert = (int)render_line;
    g_nVideoClockHorz = (int)render_cycle;
    s_current_sw      = sw;

    /* Map ALTCHARSET soft-switch to g_nVideoCharSet (0/1). */
    g_nVideoCharSet = sw_altcharset(sw) ? 1 : 0;

    const render_mode_t mode = pick_mode(sw, g_nVideoClockVert);
    position_video_address(mode);

    switch (mode) {
        case MODE_TEXT40: step_text40(sw); break;
        case MODE_TEXT80: step_text80(sw); break;
        case MODE_HGR:    step_hgr(sw);    break;
        case MODE_DHGR:   step_dhgr(sw);   break;
        case MODE_LORES:  step_lores(sw);  break;
        case MODE_DLORES: step_dlores(sw); break;
        default:          g_acr_unknown_modes++; break;
    }
    emit_shifted_right_edge_pixels(mode);
    restore_left_border(render_line, render_cycle);

    g_acr_cycles_rendered++;
    s_records_in_frame++;
}
