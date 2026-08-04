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
 * frame rates: writer-faster-than-reader silently drops frames, while a
 * faster reader holds the last good frame. Normal legacy video publishes
 * at frame end. Full shadow renders publish when their frame is complete;
 * unchanged SHR shadows keep the last published slot without a new decode.
 *
 * License: GPLv2 (inherited from AppleWin).
 */

#include <stdint.h>
#include <string.h>

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

/* Data barrier before the publish CAS. Compiles away on the host
 * regression harness (scripts/host_render_harness), which executes
 * this file single-threaded on x86. */
#if defined(__GNUC__) && defined(__arm__)
#define ACR_DSB() __asm__ volatile ("dsb sy")
#else
#define ACR_DSB() ((void)0)
#endif

#include "xil_cache.h"
#include "xil_mmu.h"

#include "../lib/common.h"
#include "../lib/uart.h"

#include "apple_cycle_egress.h"
#include "apple_cycle_renderer.h"
#include "apple_fb_handoff.h"
#include "card_control_regs.h"
#include "xil_io.h"
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

/*
 * A 1 MHz vTW core learns its next bus address immediately after the prior
 * Apple data phase, while the serialized physical C0xx access is captured
 * in the following record. VIDSYNC observes that as a one-cycle-late mode
 * transition. Hold one vTW frame record so the following record can supply
 * only the affected next-cycle video bits. ALTCHARSET is deliberately not
 * in the mask: hardware validation shows its glyph path is already aligned.
 */
#define VTW_PHASE_SW_MASK ( \
    (1U << ACE_SWB_80STORE_BIT) | \
    (1U << ACE_SWB_TEXT_BIT) | \
    (1U << ACE_SWB_MIXED_BIT) | \
    (1U << ACE_SWB_PAGE2_BIT) | \
    (1U << ACE_SWB_HIRES_BIT) | \
    (1U << ACE_SWB_80COL_BIT) | \
    (1U << ACE_SWB_DHIRES_BIT))

static uint8_t  s_vtw_1mhz_active = 0u;
static uint8_t  s_vtw_pending_valid = 0u;
static uint64_t s_vtw_pending_record = 0ULL;

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
static uint8_t s_render_video7_mono_enable = 0u;
static uint8_t s_render_dhgr_col140m_enable = 1u;
static uint8_t s_dhgr_col140m_right_mono = 0u;
static uint8_t s_border_enabled = 0u;
static uint8_t s_border_default_color = APPLE_VIDEO_IIGS_BORDER_DEFAULT;
static uint8_t s_video7_rgb_flags = 0u;
static uint8_t s_video7_rgb_mode = 0u;
static uint8_t s_video7_an3_sequence = 0u;
static uint8_t s_video7_rgb_mode_seen = 0xFFu;
static uint8_t s_shr_mono_gate_seen = 0xFFu;

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
/* After an Apple reset, CPU0 clears the PL's fake-SHR state with a
 * DMA write of $01 to $C029. Until that lands, the PL still reports
 * SHR active; hold off adopting it so the stale image cannot flash
 * back. A real C029 record always releases the hold. */
static uint8_t  s_shr_sync_hold = 0u;
static uint8_t  s_vidhd_border_color = APPLE_VIDEO_IIGS_BORDER_DEFAULT;
static uint8_t  s_vidhd_shadow = 0u;
static uint8_t  s_vidhd_bw_force_seen = 0xFFu;
static uint32_t s_frame_display_mode = APPLE_FB_DISPLAY_MODE_LEGACY;
static uint32_t s_previous_frame_display_mode = APPLE_FB_DISPLAY_MODE_LEGACY;
static uint32_t s_frame_format_detail = APPLE_FB_FORMAT_UNKNOWN;

/* SHR decode cache. Full RGGB decode can take longer than one Apple frame.
 * A captured shadow write advances g_shr_shadow_generation. Static frames
 * therefore keep the last published slot without decoding or publishing a
 * duplicate. A rebuild publishes at once, on the same frame marker. */
static uint8_t  s_shr_cache_valid = 0u;
static uint8_t  s_shr_cache_invalidate = 1u;
static uint32_t s_shr_cache_generation = 0u;
volatile uint32_t g_acr_shr_frames_skipped = 0u;
volatile uint32_t g_acr_shr_cache_rebuilds = 0u;

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

static const volatile uint8_t *s_shr_color_bank; /* set per field pass */

static inline uint32_t shr_palette_color(uint16_t palette_base, uint8_t idx) {
    const volatile uint8_t *bank = s_shr_color_bank;
    const uint16_t a = (uint16_t)(palette_base + ((uint16_t)(idx & 0x0Fu) * 2u));
    const uint16_t raw =
        (uint16_t)bank[a] |
        (uint16_t)((uint16_t)bank[(uint16_t)(a + 1u)] << 8);
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

static uint8_t dhgr_col140m_mode_is_mono(uint16_t row_addr, int dot)
{
    const int pair_dot = dot % 28;
    const int pair = dot / 28;
    const int mode_byte = (pair_dot < 8) ? 0 :
                          (pair_dot < 16) ? 1 :
                          (pair_dot < 24) ? 2 : 3;
    const int byte_index = pair * 4 + mode_byte;
    const uint16_t addr = (uint16_t)(row_addr + (uint16_t)(byte_index >> 1));
    const uint8_t value = ((byte_index & 1) != 0) ?
        g_main_bank[addr] : g_aux_bank[addr];

    return (uint8_t)(((value & 0x80u) == 0u) ? 1u : 0u);
}

static void dhgr_col140m_apply_crisp(uint32_t *dst, uint16_t addr, int x)
{
    const uint16_t row_addr = (uint16_t)(addr - (uint16_t)x);
    const uint16_t bits = (uint16_t)((g_aux_bank[addr] & 0x7fu) |
                                    ((uint16_t)(g_main_bank[addr] & 0x7fu) << 7));
    const int first_dot = x * 14;

    for (int i = 0; i < 14; ++i) {
        const int dot = first_dot + i;
        if (dhgr_col140m_mode_is_mono(row_addr, dot) != 0u) {
            dst[i] = s_aw_base_packed[((bits >> i) & 1u) ? 15u : 0u];
        }
    }
}

static void atn_updatePixels_dhgr_col140m(uint16_t bits,
                                          uint16_t row_addr,
                                          int x)
{
    /* Composite DHGR starts three scratch pixels before the visible edge. */
    const int first_dot = x * 14 - 3;

    for (int i = 0; i < 14; ++i) {
        const int dot = first_dot + i;
        if (dot >= 0 && dot < (int)ATN_ACTIVE_WIDTH &&
            dhgr_col140m_mode_is_mono(row_addr, dot) != 0u) {
            atn_emit_mono(bits & 1u);
        } else {
            atn_emit_color(bits & 1u);
        }
        bits >>= 1;
    }
    g_nLastColumnPixelNTSC = bits & 1u;
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
        if (mode == MODE_DHGR &&
            s_render_mono_enable == 0u &&
            s_render_dhgr_col140m_enable != 0u) {
            for (int i = 0; i < left_shift; ++i) {
                if (s_dhgr_col140m_right_mono != 0u) {
                    atn_emit_mono(0u);
                } else {
                    atn_emit_color(0u);
                }
            }
            g_nLastColumnPixelNTSC = 0u;
        } else {
            atn_emit_blank_pixels((uint8_t)left_shift);
        }
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
            const int x = g_nVideoClockHorz - (int)ATN_SCANNER_HORZ_START;
            uint32_t *const output = g_pVideoAddress;

            if (s_render_mono_enable == 0u &&
                s_render_color_mode == APPLE_VIDEO_COLOR_IDEALIZED) {
                step_dhgr_idealized(sw);
                if (s_render_dhgr_col140m_enable != 0u) {
                    dhgr_col140m_apply_crisp(output, addr, x);
                }
                return;
            }
            if (s_render_mono_enable == 0u &&
                s_render_color_mode == APPLE_VIDEO_COLOR_RGB) {
                step_dhgr_rgb(sw);
                if (s_render_dhgr_col140m_enable != 0u) {
                    dhgr_col140m_apply_crisp(output, addr, x);
                }
                return;
            }
            uint8_t m = g_main_bank[addr & 0xFFFFu];
            uint8_t a = g_aux_bank[addr & 0xFFFFu];
            uint16_t bits = (uint16_t)(((m & 0x7F) << 7) | (a & 0x7F));
            bits = (uint16_t)((bits << 1) | g_nLastColumnPixelNTSC);
            if (s_render_mono_enable == 0u &&
                s_render_dhgr_col140m_enable != 0u) {
                const uint16_t row_addr = (uint16_t)(addr - (uint16_t)x);
                atn_updatePixels_dhgr_col140m(bits, row_addr, x);
                if (x == 39) {
                    s_dhgr_col140m_right_mono =
                        dhgr_col140m_mode_is_mono(
                            row_addr, (int)ATN_ACTIVE_WIDTH - 1);
                }
            } else {
                atn_updatePixels(bits);
            }
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

/* ==================================================================== */
/* SHR4 extended modes (SuperDuperDisplay-compatible).                  */
/*                                                                      */
/* Armed by the magic bytes 'SHR4' (high-ASCII $D3 $C8 $D2 $B4) at aux  */
/* $9DFC. Each palette entry's SECOND byte carries a mode nibble in its */
/* top 4 bits (unused by stock SHR):                                    */
/*   $0RGB standard SHR   $1ggg RGGB Bayer demosaic                    */
/*   $2RGB PAL256 flat 256-color palette   $3xxx R4G4B4 direct color   */
/* Reference: SDD shaders/a2video_beam_shr_raw.frag. The frame renders  */
/* from the mirror at the frame marker, so PAL256 palette snapshots are */
/* frame-accurate rather than beam-accurate -- identical for static     */
/* art, approximate for beam-racing art (documented difference).       */
/* ==================================================================== */

#define SHR_MAGIC_ADDR  0x9DFCu
#define SHR_CTRL_ADDR   0x9DF8u

/* Per-FIELD mode state: SDD re-evaluates magic/ctrl per field bank, so a
 * double-mode second field carries its own mode data in main memory. */
static uint8_t  s_shr4_frame_active;
static uint8_t  s_shr3200_frame_active;
static uint8_t  s_shr3200_bank;        /* 0 = main, 1 = aux */
static uint16_t s_shr3200_pal_start;   /* base of 200 x 32-byte palettes */
static const volatile uint8_t *s_f_bank;   /* current field's pixel bank */
static uint8_t  s_shr_interlace_mode;  /* SDD $9DF8 interlace enabled */
static uint8_t  s_shr_interlaced;      /* rendering an interlaced frame  */
/* Format facts gathered from the exact pixels rendered into this frame.
 * They become frame-coherent badge metadata in the OCM handoff. */
static uint8_t  s_shr_badge_family_mask;   /* 1=SHR, 2=SHR4, 4=3200 */
static uint8_t  s_shr_badge_geometry_mask; /* 1=320, 2=640 */
static uint8_t  s_shr_badge_selector_mask; /* SHR4 selector bits 0..3 */
static uint32_t s_f_out_row;           /* output row of the line in flight */
static uint8_t  s_post_wide_last = 0xFFu;

/* Diagnostic: frames rendered with any SHR4/3200 mode active. */
volatile uint32_t g_acr_shr4_frames = 0u;

static void shr_badge_begin(void)
{
    s_shr_badge_family_mask = 0u;
    s_shr_badge_geometry_mask = 0u;
    s_shr_badge_selector_mask = 0u;
}

static uint32_t shr_badge_detail(uint8_t page_mode)
{
    uint32_t base;

    switch (s_shr_badge_family_mask) {
    case 1u:
        base = (s_shr_badge_geometry_mask == 1u) ? APPLE_FB_FORMAT_SHR_320 :
               (s_shr_badge_geometry_mask == 2u) ? APPLE_FB_FORMAT_SHR_640 :
                                                   APPLE_FB_FORMAT_SHR_MIXED;
        break;
    case 2u:
        base = (s_shr_badge_geometry_mask == 1u) ? APPLE_FB_FORMAT_SHR4_320 :
               (s_shr_badge_geometry_mask == 2u) ? APPLE_FB_FORMAT_SHR4_640 :
                                                   APPLE_FB_FORMAT_SHR4_MIXED;
        break;
    case 4u:
        base = (s_shr_badge_geometry_mask == 1u) ? APPLE_FB_FORMAT_SHR3200_320 :
               (s_shr_badge_geometry_mask == 2u) ? APPLE_FB_FORMAT_SHR3200_640 :
                                                   APPLE_FB_FORMAT_SHR3200_MIXED;
        break;
    default:
        base = APPLE_FB_FORMAT_SHR_EXT_MIXED;
        break;
    }

    return APPLE_FB_FORMAT_DETAIL(base, s_shr_badge_selector_mask,
                                  page_mode);
}

static inline void shr_badge_note_selector(uint8_t selector)
{
    if (selector <= 3u) {
        s_shr_badge_selector_mask |= (uint8_t)(1u << selector);
    } else {
        /* Unknown values render through the standard fallback. Mark all
         * selectors so the badge says MIXED instead of making a false claim. */
        s_shr_badge_selector_mask = 0x0Fu;
    }
}

static inline int shr4_magic_present(const volatile uint8_t *bank) {
    return bank[SHR_MAGIC_ADDR + 0u] == 0xD3u &&   /* 'S' | $80 */
           bank[SHR_MAGIC_ADDR + 1u] == 0xC8u &&   /* 'H' | $80 */
           bank[SHR_MAGIC_ADDR + 2u] == 0xD2u &&   /* 'R' | $80 */
           bank[SHR_MAGIC_ADDR + 3u] == 0xB4u;     /* '4' | $80 */
}

static inline int shr3200_magic_present(const volatile uint8_t *bank) {
    return bank[SHR_MAGIC_ADDR + 0u] == 0xB3u &&   /* '3' | $80 */
           bank[SHR_MAGIC_ADDR + 1u] == 0xB2u &&   /* '2' | $80 */
           bank[SHR_MAGIC_ADDR + 2u] == 0xB0u &&   /* '0' | $80 */
           bank[SHR_MAGIC_ADDR + 3u] == 0xB0u;     /* '0' | $80 */
}

/* Direct RGB444 -> BGRA, matching ConvertIIgs2RGB (channel * 16). */
static inline uint32_t shr4_rgb444(uint8_t r4, uint8_t g4, uint8_t b4) {
    return shr_apply_c029_bw(shr_pack_bgra((uint8_t)(r4 * 16u),
                                           (uint8_t)(g4 * 16u),
                                           (uint8_t)(b4 * 16u)));
}

/* RGGB neighborhood sample: the 4-bit palette INDEX of pixel (px, py)  */
/* in 320x200 space, 0 outside the content area (matches the shader's   */
/* bounds behavior).                                                    */
/* row is in OUTPUT space: 0..199 normally (one field), 0..399 when the
 * frame is interlaced -- even output rows sample the aux field, odd rows
 * the main field, exactly like the shader's 320x400 interlace path. */
static inline int32_t shr4_rggb_sample(int32_t px, int32_t row) {
    const int32_t rows = s_shr_interlaced ? 400 : 200;
    if (px < 0 || px >= 320 || row < 0 || row >= rows) return 0;
    const volatile uint8_t *bank = s_f_bank;
    int32_t py = row;
    if (s_shr_interlaced) {
        bank = (row & 1) ? g_main_bank : g_aux_bank;
        py = row >> 1;
    }
    const uint16_t a = (uint16_t)(0x2000u + 160u * (uint32_t)py +
                                  ((uint32_t)px >> 1));
    const uint8_t byte = bank[a];
    return (px & 1) ? (int32_t)(byte & 0x0Fu) : (int32_t)(byte >> 4);
}

/* 640-mode variant: the sample is the raw 2-bit pixel value (0..3) at
 * full 640 horizontal resolution, matching the shader's 640 CFA path. */
static inline int32_t shr4_rggb_sample640(int32_t px, int32_t row) {
    const int32_t rows = s_shr_interlaced ? 400 : 200;
    if (px < 0 || px >= 640 || row < 0 || row >= rows) return 0;
    const volatile uint8_t *bank = s_f_bank;
    int32_t py = row;
    if (s_shr_interlaced) {
        bank = (row & 1) ? g_main_bank : g_aux_bank;
        py = row >> 1;
    }
    const uint16_t a = (uint16_t)(0x2000u + 160u * (uint32_t)py +
                                  ((uint32_t)px >> 2));
    return (int32_t)((bank[a] >> (6 - 2 * (px & 3))) & 0x03u);
}

/* Malvar 5x5 demosaic weights over the shader's 13-sample layout:      */
/*   s0 (x,y-2); s1..s3 (x-1..x+1, y-1); s4..s8 (x-2..x+2, y);          */
/*   s9..s11 (x-1..x+1, y+1); s12 (x,y+2).                              */
/* Weights are doubled so the 0.5/1.5 entries stay integral; the final  */
/* divisor is therefore 240 (= 15 * 8 * 2).                             */
static const int8_t k_rggb_g[13]   = { -2,  0,  4,  0,
                                       -2,  4,  8,  4, -2,
                                        0,  4,  0, -2 };
static const int8_t k_rggb_xg[13]  = {  1, -2,  0, -2,
                                       -2,  8, 10,  8, -2,
                                       -2,  0, -2,  1 };
static const int8_t k_rggb_xgx[13] = { -2, -2,  8, -2,
                                        1,  0, 10,  0,  1,
                                       -2,  8, -2, -2 };
static const int8_t k_rggb_rb[13]  = { -3,  4,  0,  4,
                                       -3,  0, 12,  0, -3,
                                        4,  0,  4, -3 };

/* max_sample is 15 in 320 mode, 3 in 640 mode; the divisor follows the
 * shader's "filter gives x8" normalization (doubled weights -> x16). */
/* Scalar reference; the NEON row path below is bit-exact to it and is
 * used instead whenever __ARM_NEON is available. */
static uint32_t __attribute__((unused))
shr4_rggb_filter(const int32_t *s, const int8_t *w,
                                 int32_t max_sample) {
    int32_t acc = 0;
    for (int i = 0; i < 13; ++i) acc += s[i] * (int32_t)w[i];
    acc = (acc * 255) / (max_sample * 16);
    if (acc < 0) acc = 0;
    if (acc > 255) acc = 255;
    return (uint32_t)acc;
}

/* Demosaic one pixel in 320- or 640-space. Bayer cell: (even x, even  */
/* y) = R, (odd x, even y) = G, (even x, odd y) = G, (odd x, odd y) = B.*/
static uint32_t __attribute__((unused))
shr4_rggb_pixel_at(int32_t px, int32_t py, int is640) {
    int32_t (*const sample)(int32_t, int32_t) =
        is640 ? shr4_rggb_sample640 : shr4_rggb_sample;
    const int32_t maxs = is640 ? 3 : 15;
    int32_t s[13];
    s[0]  = sample(px,     py - 2);
    s[1]  = sample(px - 1, py - 1);
    s[2]  = sample(px,     py - 1);
    s[3]  = sample(px + 1, py - 1);
    s[4]  = sample(px - 2, py);
    s[5]  = sample(px - 1, py);
    s[6]  = sample(px,     py);
    s[7]  = sample(px + 1, py);
    s[8]  = sample(px + 2, py);
    s[9]  = sample(px - 1, py + 1);
    s[10] = sample(px,     py + 1);
    s[11] = sample(px + 1, py + 1);
    s[12] = sample(px,     py + 2);

    const uint32_t own = (uint32_t)((s[6] * 255) / maxs);
    uint32_t r, g, b;
    if (((px & 1) == 0) && ((py & 1) == 0)) {          /* R site */
        r = own;
        g = shr4_rggb_filter(s, k_rggb_g, maxs);
        b = shr4_rggb_filter(s, k_rggb_rb, maxs);
    } else if (((px & 1) == 1) && ((py & 1) == 0)) {   /* G site, R row */
        r = shr4_rggb_filter(s, k_rggb_xg, maxs);
        g = own;
        b = shr4_rggb_filter(s, k_rggb_xgx, maxs);
    } else if (((px & 1) == 0) && ((py & 1) == 1)) {   /* G site, B row */
        r = shr4_rggb_filter(s, k_rggb_xgx, maxs);
        g = own;
        b = shr4_rggb_filter(s, k_rggb_xg, maxs);
    } else {                                           /* B site */
        r = shr4_rggb_filter(s, k_rggb_rb, maxs);
        g = shr4_rggb_filter(s, k_rggb_g, maxs);
        b = own;
    }
    return shr_apply_c029_bw(shr_pack_bgra((uint8_t)r, (uint8_t)g,
                                           (uint8_t)b));
}

#if defined(__ARM_NEON)
/* ---------- NEON RGGB row demosaic ----------
 *
 * The scalar path gathers 13 samples and runs a 13-tap filter per
 * pixel: several frame budgets per frame at 640 or double-field
 * resolution. This path demosaics a whole line at a time, 8 pixels
 * per vector, bit-exact to the scalar filter:
 *
 *   (acc * 255) / 240 == (acc * 17) >> 4    (320 mode, 255/240 = 17/16)
 *   (acc * 255) /  48 == (acc * 85) >> 4    (640 mode, 255/48  = 85/16)
 *
 * for every acc the weights can produce (|acc| <= 645, |acc*85| <=
 * 10965: int16-safe), with vqmovun_s16 supplying exactly the scalar's
 * 0..255 clamp (negatives -> 0). Sample rows are unpacked once into a
 * ring of padded int16 rows (2 zero samples each side reproduce the
 * scalar's out-of-bounds behavior) and shared across the five output
 * rows that reference them; the generation counter invalidates the
 * ring and the line cache at each full-frame decode. $C029 B&W is
 * applied at lookup, not baked into the cached line. */
static int16_t  s_rggb_srows[8][648] __attribute__((aligned(16)));
static int32_t  s_rggb_srow_id[8];
static uint8_t  s_rggb_srow_is640[8];
static uint32_t s_rggb_srow_gen[8];
/* Bank identity per cached row: SHR4 page-flip merging renders the
 * SAME row index from two different field banks within one frame, so
 * the row index alone cannot key the cache. */
static const volatile uint8_t *s_rggb_srow_bank[8];
static uint32_t s_rggb_gen = 1u;

static uint32_t s_rggb_line_bgra[640] __attribute__((aligned(16)));
static int32_t  s_rggb_line_id = -3;
static uint8_t  s_rggb_line_is640;
static const volatile uint8_t *s_rggb_line_bank;
static uint32_t s_rggb_line_gen = 0u;

/* Unpack (or fetch cached) one sample row in unified output-row space.
 * Returns the padded buffer; sample x lives at [2 + x]. Bank selection
 * matches shr4_rggb_sample exactly: interlaced rows alternate banks,
 * otherwise the current field bank serves the whole frame. */
static const int16_t *rggb_sample_row(int32_t row, int is640)
{
    const int32_t rows = s_shr_interlaced ? 400 : 200;
    const uint32_t slot = (uint32_t)(row + 2) & 7u;
    const int32_t width = is640 ? 640 : 320;
    int16_t *buf = s_rggb_srows[slot];
    const volatile uint8_t *bank = s_f_bank;
    int32_t py = row;

    if (s_shr_interlaced) {
        bank = (row & 1) ? g_main_bank : g_aux_bank;
        py = row >> 1;
    }

    if (s_rggb_srow_gen[slot] == s_rggb_gen &&
        s_rggb_srow_id[slot] == row &&
        s_rggb_srow_is640[slot] == (uint8_t)is640 &&
        s_rggb_srow_bank[slot] == bank) {
        return buf;
    }

    memset(buf, 0, (size_t)(width + 4) * sizeof(int16_t));
    if (row >= 0 && row < rows) {
        const uint16_t base = (uint16_t)(0x2000u + 160u * (uint32_t)py);
        int16_t *out = buf + 2;
        if (is640) {
            for (uint32_t i = 0u; i < 160u; ++i) {
                const uint8_t byte = bank[(uint16_t)(base + i)];
                out[0] = (int16_t)((byte >> 6) & 0x03u);
                out[1] = (int16_t)((byte >> 4) & 0x03u);
                out[2] = (int16_t)((byte >> 2) & 0x03u);
                out[3] = (int16_t)(byte & 0x03u);
                out += 4;
            }
        } else {
            for (uint32_t i = 0u; i < 160u; ++i) {
                const uint8_t byte = bank[(uint16_t)(base + i)];
                out[0] = (int16_t)(byte >> 4);
                out[1] = (int16_t)(byte & 0x0Fu);
                out += 2;
            }
        }
    }
    s_rggb_srow_gen[slot] = s_rggb_gen;
    s_rggb_srow_id[slot] = row;
    s_rggb_srow_is640[slot] = (uint8_t)is640;
    s_rggb_srow_bank[slot] = bank;
    return buf;
}

static void rggb_build_line_neon(int32_t row, int is640)
{
    const int32_t width = is640 ? 640 : 320;
    const int16_t *rm2 = rggb_sample_row(row - 2, is640) + 2;
    const int16_t *rm1 = rggb_sample_row(row - 1, is640) + 2;
    const int16_t *rc  = rggb_sample_row(row,     is640) + 2;
    const int16_t *rp1 = rggb_sample_row(row + 1, is640) + 2;
    const int16_t *rp2 = rggb_sample_row(row + 2, is640) + 2;
    const int16_t scale = is640 ? 85 : 17;
    static const uint16_t k_even_lanes[8] =
        { 0xFFFFu, 0u, 0xFFFFu, 0u, 0xFFFFu, 0u, 0xFFFFu, 0u };
    const uint16x8_t even = vld1q_u16(k_even_lanes);
    const int row_odd = (int)(row & 1);

    for (int32_t px = 0; px < width; px += 8) {
        const int16x8_t sm2  = vld1q_s16(rm2 + px);
        const int16x8_t sm1l = vld1q_s16(rm1 + px - 1);
        const int16x8_t sm1c = vld1q_s16(rm1 + px);
        const int16x8_t sm1r = vld1q_s16(rm1 + px + 1);
        const int16x8_t s0l2 = vld1q_s16(rc + px - 2);
        const int16x8_t s0l1 = vld1q_s16(rc + px - 1);
        const int16x8_t s0c  = vld1q_s16(rc + px);
        const int16x8_t s0r1 = vld1q_s16(rc + px + 1);
        const int16x8_t s0r2 = vld1q_s16(rc + px + 2);
        const int16x8_t sp1l = vld1q_s16(rp1 + px - 1);
        const int16x8_t sp1c = vld1q_s16(rp1 + px);
        const int16x8_t sp1r = vld1q_s16(rp1 + px + 1);
        const int16x8_t sp2  = vld1q_s16(rp2 + px);

        /* k_rggb_g: -2*s0 +4*(s2,s5,s7,s10) +8*s6 -2*(s4,s8,s12) */
        int16x8_t gf = vmulq_n_s16(s0c, 8);
        gf = vmlaq_n_s16(gf, sm1c, 4);
        gf = vmlaq_n_s16(gf, s0l1, 4);
        gf = vmlaq_n_s16(gf, s0r1, 4);
        gf = vmlaq_n_s16(gf, sp1c, 4);
        gf = vmlaq_n_s16(gf, sm2, -2);
        gf = vmlaq_n_s16(gf, s0l2, -2);
        gf = vmlaq_n_s16(gf, s0r2, -2);
        gf = vmlaq_n_s16(gf, sp2, -2);

        /* k_rggb_xg: +1*(s0,s12) -2*(s1,s3,s4,s8,s9,s11) +8*(s5,s7) +10*s6 */
        int16x8_t xg = vmulq_n_s16(s0c, 10);
        xg = vmlaq_n_s16(xg, s0l1, 8);
        xg = vmlaq_n_s16(xg, s0r1, 8);
        xg = vaddq_s16(xg, sm2);
        xg = vaddq_s16(xg, sp2);
        xg = vmlaq_n_s16(xg, sm1l, -2);
        xg = vmlaq_n_s16(xg, sm1r, -2);
        xg = vmlaq_n_s16(xg, s0l2, -2);
        xg = vmlaq_n_s16(xg, s0r2, -2);
        xg = vmlaq_n_s16(xg, sp1l, -2);
        xg = vmlaq_n_s16(xg, sp1r, -2);

        /* k_rggb_xgx: -2*(s0,s1,s3,s9,s11,s12) +8*(s2,s10) +1*(s4,s8) +10*s6 */
        int16x8_t xgx = vmulq_n_s16(s0c, 10);
        xgx = vmlaq_n_s16(xgx, sm1c, 8);
        xgx = vmlaq_n_s16(xgx, sp1c, 8);
        xgx = vaddq_s16(xgx, s0l2);
        xgx = vaddq_s16(xgx, s0r2);
        xgx = vmlaq_n_s16(xgx, sm2, -2);
        xgx = vmlaq_n_s16(xgx, sm1l, -2);
        xgx = vmlaq_n_s16(xgx, sm1r, -2);
        xgx = vmlaq_n_s16(xgx, sp1l, -2);
        xgx = vmlaq_n_s16(xgx, sp1r, -2);
        xgx = vmlaq_n_s16(xgx, sp2, -2);

        /* k_rggb_rb: -3*(s0,s4,s8,s12) +4*(s1,s3,s9,s11) +12*s6 */
        int16x8_t rb = vmulq_n_s16(s0c, 12);
        rb = vmlaq_n_s16(rb, sm1l, 4);
        rb = vmlaq_n_s16(rb, sm1r, 4);
        rb = vmlaq_n_s16(rb, sp1l, 4);
        rb = vmlaq_n_s16(rb, sp1r, 4);
        rb = vmlaq_n_s16(rb, sm2, -3);
        rb = vmlaq_n_s16(rb, s0l2, -3);
        rb = vmlaq_n_s16(rb, s0r2, -3);
        rb = vmlaq_n_s16(rb, sp2, -3);

        gf  = vshrq_n_s16(vmulq_n_s16(gf, scale), 4);
        xg  = vshrq_n_s16(vmulq_n_s16(xg, scale), 4);
        xgx = vshrq_n_s16(vmulq_n_s16(xgx, scale), 4);
        rb  = vshrq_n_s16(vmulq_n_s16(rb, scale), 4);
        const int16x8_t own = vmulq_n_s16(s0c, scale);

        /* Bayer site map (even x, even y)=R, G, G, (odd, odd)=B. */
        int16x8_t rv, gv, bv;
        if (row_odd == 0) {
            rv = vbslq_s16(even, own, xg);
            gv = vbslq_s16(even, gf, own);
            bv = vbslq_s16(even, rb, xgx);
        } else {
            rv = vbslq_s16(even, xgx, rb);
            gv = vbslq_s16(even, own, gf);
            bv = vbslq_s16(even, xg, own);
        }

        uint8x8x4_t out;
        out.val[0] = vqmovun_s16(bv);
        out.val[1] = vqmovun_s16(gv);
        out.val[2] = vqmovun_s16(rv);
        out.val[3] = vdup_n_u8(0xFFu);
        vst4_u8((uint8_t *)&s_rggb_line_bgra[px], out);
    }
}
#endif /* __ARM_NEON */

/* Per-pixel RGGB entry point for the cell renderers: NEON builds (and
 * caches) the whole line on first touch; scalar builds fall through to
 * the reference per-pixel filter. */
static inline uint32_t shr4_rggb_pixel_lookup(int32_t px, int32_t row,
                                              int is640)
{
#if defined(__ARM_NEON)
    if (s_rggb_line_gen != s_rggb_gen || s_rggb_line_id != row ||
        s_rggb_line_is640 != (uint8_t)is640 ||
        s_rggb_line_bank != s_f_bank) {
        rggb_build_line_neon(row, is640);
        s_rggb_line_gen = s_rggb_gen;
        s_rggb_line_id = row;
        s_rggb_line_is640 = (uint8_t)is640;
        s_rggb_line_bank = s_f_bank;
    }
    return shr_apply_c029_bw(s_rggb_line_bgra[px]);
#else
    return shr4_rggb_pixel_at(px, row, is640);
#endif
}

/* R4G4B4: 3-byte groups hold two direct-color pixels, each spanning    */
/* three 320-space pixels: bytes AB CD EF -> colors (A,B,C) and (D,E,F).*/
static uint32_t shr4_r4g4b4_pixel(uint32_t y, int32_t px) {
    const uint32_t group = (uint32_t)px / 6u;
    const uint16_t base = (uint16_t)(0x2000u + 160u * y + group * 3u);
    const uint8_t b0 = s_f_bank[base];
    /* A 160-byte line holds 53 full triplets plus a 2-pixel tail whose
     * later bytes would live past the line. SDD's shader texel-fetches
     * out of its VRAM texture there and reads back zero; match that
     * instead of spilling into the next row's first byte (which showed
     * as a colored tint down the last 320-mode column). */
    const uint8_t b1 = (group * 3u + 1u < 160u)
        ? s_f_bank[(uint16_t)(base + 1u)] : 0u;
    const uint8_t b2 = (group * 3u + 2u < 160u)
        ? s_f_bank[(uint16_t)(base + 2u)] : 0u;
    if (((uint32_t)px % 6u) < 3u) {
        return shr4_rgb444((uint8_t)(b0 >> 4), (uint8_t)(b0 & 0x0Fu),
                           (uint8_t)(b1 >> 4));
    }
    return shr4_rgb444((uint8_t)(b1 & 0x0Fu), (uint8_t)(b2 >> 4),
                       (uint8_t)(b2 & 0x0Fu));
}

/* PAL256: the SHR byte is an index into the flat 512-byte palette area */
/* at aux $9E00 (all 16 palettes as one 256-color table). Both pixels   */
/* of the byte show the same color.                                     */
static inline uint32_t shr4_pal256_color(uint8_t idx) {
    const uint16_t a = (uint16_t)(0x9E00u + ((uint16_t)idx * 2u));
    const uint16_t raw =
        (uint16_t)s_f_bank[a] |
        (uint16_t)((uint16_t)s_f_bank[(uint16_t)(a + 1u)] << 8);
    return shr_apply_c029_bw(shr_pack_bgra(
        (uint8_t)(((raw >> 8) & 0x0Fu) * 16u),
        (uint8_t)(((raw >> 4) & 0x0Fu) * 16u),
        (uint8_t)((raw & 0x0Fu) * 16u)));
}

/* SHR-3200 ("Brooks"): magic '3200' at $9DFC; ctrl bytes at $9DF8 are  */
/* {paged mode, bank, palette lo, palette hi}. One 32-byte palette per  */
/* line, in EITHER bank, and the palette index is REVERSED (0 = last    */
/* entry) -- both per the SDD reference.                                */
static inline uint32_t shr3200_color(uint32_t y, uint8_t idx) {
    const uint16_t a = (uint16_t)(s_shr3200_pal_start + y * 32u +
                                  ((uint16_t)(15u - (idx & 0x0Fu)) * 2u));
    const uint8_t lo = s_shr3200_bank ? g_aux_bank[a] : g_main_bank[a];
    const uint8_t hi = s_shr3200_bank ? g_aux_bank[(uint16_t)(a + 1u)]
                                      : g_main_bank[(uint16_t)(a + 1u)];
    const uint16_t raw = (uint16_t)lo | ((uint16_t)hi << 8);
    return shr_apply_c029_bw(shr_pack_bgra(
        (uint8_t)(((raw >> 8) & 0x0Fu) * 16u),
        (uint8_t)(((raw >> 4) & 0x0Fu) * 16u),
        (uint8_t)((raw & 0x0Fu) * 16u)));
}

static void shr3200_render_cell_320(uint32_t *row0, uint32_t y, uint32_t x)
{
    uint32_t *dst = row0 + x * 16u;
    const uint16_t addr = shr_scanline_addr(y, x);

    for (uint32_t i = 0u; i < 4u; ++i) {
        const uint8_t byte = s_f_bank[(uint16_t)(addr + i)];
        const uint32_t c1 = shr3200_color(y, (uint8_t)(byte >> 4));
        *dst++ = c1;
        *dst++ = c1;
        const uint32_t c2 = shr3200_color(y, (uint8_t)(byte & 0x0Fu));
        *dst++ = c2;
        *dst++ = c2;
    }
}

/* SHR4 320-mode cell: 4 bytes -> 8 doubled pixels, each pixel routed   */
/* by ITS palette entry's mode nibble (submodes mix freely on a line).  */
static void shr4_render_cell_320(uint32_t *row0, uint32_t y, uint32_t x,
                                 uint16_t palette_base)
{
    uint32_t *dst = row0 + x * 16u;
    const uint16_t addr = shr_scanline_addr(y, x);

    for (uint32_t i = 0u; i < 4u; ++i) {
        const uint8_t byte = s_f_bank[(uint16_t)(addr + i)];
        for (uint32_t half = 0u; half < 2u; ++half) {
            const uint8_t idx = (half == 0u) ? (uint8_t)(byte >> 4)
                                             : (uint8_t)(byte & 0x0Fu);
            const int32_t px = (int32_t)(x * 8u + i * 2u + half);
            const uint8_t b2 =
                s_f_bank[(uint16_t)(palette_base + idx * 2u + 1u)];
            const uint8_t selector = (uint8_t)(b2 >> 4);
            uint32_t color;
            shr_badge_note_selector(selector);
            switch (selector) {
            case 1u:
                color = shr4_rggb_pixel_lookup(px, (int32_t)s_f_out_row, 0);
                break;
            case 2u:
                color = shr4_pal256_color(byte);
                break;
            case 3u:
                color = shr4_r4g4b4_pixel(y, px);
                break;
            default:
                color = shr_palette_color(palette_base, idx);
                break;
            }
            *dst++ = color;
            *dst++ = color;
        }
    }
}

/* SHR4 640-mode cell: 4 bytes -> 16 single-width pixels. Each 2-bit
 * pixel selects a palette entry from its position's quadrant (the
 * standard 640-mode remap); that entry's mode nibble routes the pixel.
 * RGGB uses the raw 2-bit value as the Bayer sample at full 640
 * resolution; every other submode renders as standard 640 color. */
static const uint8_t k_shr640_quad[4] = { 8u, 12u, 0u, 4u };

static void shr4_render_cell_640(uint32_t *row0, uint32_t y, uint32_t x,
                                 uint16_t palette_base)
{
    uint32_t *dst = row0 + x * 16u;
    const uint16_t addr = shr_scanline_addr(y, x);
    (void)y;

    for (uint32_t i = 0u; i < 4u; ++i) {
        const uint8_t byte = s_f_bank[(uint16_t)(addr + i)];
        for (uint32_t lp = 0u; lp < 4u; ++lp) {
            const uint8_t v = (uint8_t)((byte >> (6u - 2u * lp)) & 0x03u);
            const uint8_t idx = (uint8_t)(k_shr640_quad[lp] + v);
            const uint8_t b2 =
                s_f_bank[(uint16_t)(palette_base + idx * 2u + 1u)];
            const uint8_t selector = (uint8_t)(b2 >> 4);
            shr_badge_note_selector(selector);
            if (selector == 1u) {
                const int32_t px = (int32_t)(x * 16u + i * 4u + lp);
                *dst++ = shr4_rggb_pixel_lookup(px, (int32_t)s_f_out_row, 1);
            } else {
                *dst++ = shr_palette_color(palette_base, idx);
            }
        }
    }
}

/* Evaluate the extended-mode state for one field bank: magic, 3200 ctrl,
 * and (aux only) the paged-mode byte. Mirrors SDD's per-bank re-run. */
static void shr_eval_field_modes(const volatile uint8_t *bank)
{
    s_f_bank = bank;
    s_shr_color_bank = bank;
    s_shr4_frame_active = (uint8_t)shr4_magic_present(bank);
    s_shr3200_frame_active = 0u;
    if (!s_shr4_frame_active && shr3200_magic_present(bank)) {
        const uint16_t pal_start =
            (uint16_t)bank[SHR_CTRL_ADDR + 2u] |
            (uint16_t)((uint16_t)bank[SHR_CTRL_ADDR + 3u] << 8);
        /* Palettes must fit below the 64K mirror: 200 lines * 32 bytes. */
        if (pal_start < (uint16_t)(0xFFFFu - 200u * 32u)) {
            s_shr3200_frame_active = 1u;
            s_shr3200_bank = (bank[SHR_CTRL_ADDR + 1u] == 1u) ? 1u : 0u;
            s_shr3200_pal_start = pal_start;
        }
    }
}

/* Render one SHR line of the current field into `row`. Callers set
 * s_f_out_row (the RGGB sample-space row) beforehand. */
static void render_shr_line_to(uint32_t *row, uint32_t y)
{
    const uint8_t control = s_f_bank[0x9D00u + (uint16_t)y];
    const uint16_t palette_base =
        (uint16_t)(0x9E00u + ((uint16_t)(control & 0x0Fu) * 32u));
    const int is_640 = (control & 0x80u) != 0u;
    const int color_fill = (control & 0x20u) != 0u;

    s_shr_badge_geometry_mask |= is_640 ? 2u : 1u;
    s_shr_badge_family_mask |= s_shr4_frame_active ? 2u :
                               (s_shr3200_frame_active ? 4u : 1u);

    for (uint32_t x = 0u; x < 40u; ++x) {
        if (s_shr4_frame_active) {
            if (is_640) {
                shr4_render_cell_640(row, y, x, palette_base);
            } else {
                shr4_render_cell_320(row, y, x, palette_base);
            }
        } else if (s_shr3200_frame_active && !is_640) {
            shr3200_render_cell_320(row, y, x);
        } else {
            const uint16_t addr = shr_scanline_addr(y, x);
            const uint32_t a =
                (uint32_t)s_f_bank[addr] |
                ((uint32_t)s_f_bank[(uint16_t)(addr + 1u)] << 8) |
                ((uint32_t)s_f_bank[(uint16_t)(addr + 2u)] << 16) |
                ((uint32_t)s_f_bank[(uint16_t)(addr + 3u)] << 24);
            if (is_640) {
                shr_render_cell_640(row, x, a, palette_base);
            } else {
                shr_render_cell_320(row, x, a, palette_base, color_fill);
            }
        }
    }
}

/* CPU1 fallback: tell the PL whether vTW must post main $6000-$9FFF for a
 * second SHR field. The vTW core also tracks its own aux $9DF8 writes before
 * they reach the physical capture. Write-on-change only. */
static void shr_update_post_wide(uint8_t want)
{
    if (want == s_post_wide_last) return;
    s_post_wide_last = want;
    Xil_Out32(CARD_CTRL_VIDEO_POST_WIDE_REG, (uint32_t)want);
}

/* Called by the egress the moment a captured aux write to $9DF8 (the
 * SDD paged-mode ctrl byte) lands in the mirror. Under vTW, main
 * $6000-$9FFF writes are only posted while the wide window is open,
 * and a loader delivers the ctrl byte (via its aux field copy) right
 * BEFORE it reads the second interlace field into main -- with SHR
 * held off during the load, no rendered frame can open the window in
 * time, and the whole second field would bypass the mirror (observed:
 * an interlaced image showing the previous image's staging leftovers
 * in $6000-$9FFF whenever it loads after a non-interlaced one).
 * Render-time evaluation still narrows the window after
 * non-interlaced SHR frames. */
void apple_cycle_renderer_note_aux_ctrl_write(uint8_t value)
{
    /* Both paged types keep their second field in MAIN $2000-$9FFF:
     * 1 = interlace (spatial), 2 = page flip (temporal). */
    shr_update_post_wide((value == 1u || value == 2u) ? 1u : 0u);
}

/* Exact per-byte floor average, used to merge page-flip fields on
 * sub-120 Hz output: alternating pages at 50/60 Hz would strobe, so
 * both fields blend 50/50 into every frame instead. */
static inline uint32_t bgra_avg(uint32_t a, uint32_t b)
{
    return (a & b) + (((a ^ b) & 0xFEFEFEFEu) >> 1);
}

static uint32_t s_shr_merge_row_a[SHR_WIDTH] __attribute__((aligned(16)));
static uint32_t s_shr_merge_row_b[SHR_WIDTH] __attribute__((aligned(16)));

static void render_shr_frame_full(void)
{
    uint8_t page_mode = APPLE_FB_FORMAT_PAGE_NONE;

    if (g_atn_framebuffer == NULL) {
        return;
    }

    shr_badge_begin();

#if defined(__ARM_NEON)
    /* Invalidate the RGGB sample-row ring and line cache: the shadow
     * may have changed since the previous decode. */
    s_rggb_gen++;
#endif

    /* Mode/paged decisions come from the AUX control plane, re-read
     * every frame (mode-exit hygiene: clearing $9DFC drops the extended
     * decode on the next frame). SHR4 wins over 3200, per SDD. The
     * creator declares the paged TYPE in ctrl byte 0: 1 = interlace
     * (even rows aux, odd rows main; double vertical resolution),
     * 2 = page flip (two full-rate frames; merged below 120 Hz). */
    shr_eval_field_modes(g_aux_bank);
    s_shr_interlace_mode = 0u;
    uint8_t flip_merge = 0u;
    if (s_shr4_frame_active || s_shr3200_frame_active) {
        const uint8_t paged = g_aux_bank[SHR_CTRL_ADDR + 0u];
        if (paged == 1u) {
            s_shr_interlace_mode = 1u;
            page_mode = APPLE_FB_FORMAT_PAGE_INTERLACE;
        } else if (paged == 2u) {
            flip_merge = 1u;
            page_mode = APPLE_FB_FORMAT_PAGE_FLIP_MERGE;
        }
        g_acr_shr4_frames++;
    }
    shr_update_post_wide((s_shr_interlace_mode || flip_merge) ? 1u : 0u);

    if (s_shr_interlace_mode != 0u) {
        /* Interlace: even output rows from aux, odd rows from main; each
         * field carries its own mode data, SCBs, and palettes. */
        s_shr_interlaced = 1u;
        for (uint32_t field = 0u; field < 2u; ++field) {
            shr_eval_field_modes(field ? g_main_bank : g_aux_bank);
            for (uint32_t y = 0u; y < SHR_LOGICAL_HEIGHT; ++y) {
                s_f_out_row = y * 2u + field;
                render_shr_line_to(
                    &g_atn_framebuffer[(y * 2u + field) * SHR_WIDTH], y);
            }
        }
        s_shr_interlaced = 0u;
        s_frame_format_detail = shr_badge_detail(page_mode);
        return;
    }

    if (flip_merge != 0u) {
        /* Page flip on sub-120 Hz output: render the same line from
         * both field banks (each self-described per SDD) and blend
         * 50/50, doubled vertically like a progressive frame. */
        s_shr_interlaced = 0u;
        for (uint32_t y = 0u; y < SHR_LOGICAL_HEIGHT; ++y) {
            uint32_t *out = &g_atn_framebuffer[(y * 2u) * SHR_WIDTH];

            s_f_out_row = y;
            shr_eval_field_modes(g_aux_bank);
            render_shr_line_to(s_shr_merge_row_a, y);
            shr_eval_field_modes(g_main_bank);
            render_shr_line_to(s_shr_merge_row_b, y);
            for (uint32_t x = 0u; x < SHR_WIDTH; ++x) {
                out[x] = bgra_avg(s_shr_merge_row_a[x],
                                  s_shr_merge_row_b[x]);
            }
            memcpy(out + SHR_WIDTH, out, SHR_WIDTH * sizeof(uint32_t));
        }
        s_frame_format_detail = shr_badge_detail(page_mode);
        return;
    }

    s_shr_interlaced = 0u;
    for (uint32_t y = 0u; y < SHR_LOGICAL_HEIGHT; ++y) {
        uint32_t *out = &g_atn_framebuffer[(y * 2u) * SHR_WIDTH];

        s_f_out_row = y;
        render_shr_line_to(out, y);
        memcpy(out + SHR_WIDTH, out, SHR_WIDTH * sizeof(uint32_t));
    }
    s_frame_format_detail = shr_badge_detail(page_mode);
}

/* ---------- Legacy interlace weave ----------
 *
 * The A2Li in-band signal (SDD docs/LEGACY_PAGED_VIDEO_MODES.md):
 * hi-ASCII 'A2Li' plus a mode byte in PAGE 2's first screen hole,
 * main bank -- $4078-$407C for the hires pair, $0878-$087C for the
 * lores pair, following the Fotofile precedent of metadata at page
 * offset +$78. Mode $01 = interlace, $02 = page flip, anything else
 * disarms. Both hole locations sit inside the FPGA capture windows
 * (main $0400-$0BFF / $2000-$9FFF), so the signal always reaches the
 * shadow. Level-sampled at every frame start; honored only while a
 * graphics mode is selected (TEXT off), reading the hole of the
 * family the soft switches pick (HIRES on -> hires pair).
 *
 * When armed, the per-cycle legacy pipeline is bypassed and the frame
 * is synthesized shadow-side like SHR: 384 rows fetched from both
 * pages (80STORE forced clear so PAGE2 swaps pages), each row
 * rendered by the normal mode steppers with the per-line chroma
 * reset the live path applies -- so every color mode, mono included,
 * renders through its usual pixel pipeline. The hires pair weaves at
 * scanline parity (even rows PAGE1, odd rows PAGE2); the lores pair
 * weaves at nibble-row bands (96 four-scanline bands, page by band
 * parity) so a 40x96 image shows 96 crisp rows instead of striped
 * blends. The woven frame starts at slot row 0 with no border data
 * (384 rows at the legacy stride exceed the bordered layout; the
 * IIgs border is unavailable while interlaced). */
/* Returns the armed paged TYPE, declared by the content:
 * 0 = off, 1 = interlace (spatial fields), 2 = page flip (temporal:
 * PAGE2 is the alternate frame; merged 50/50 below 120 Hz output,
 * where true flipping would strobe). */
static uint8_t legacy_paged_mode(void)
{
    if (sw_text(s_current_sw)) {
        return 0u;
    }
    {
        const uint16_t hole = sw_hires(s_current_sw) ? 0x4078u : 0x0878u;

        if (g_main_bank[hole]      != 0xC1u ||    /* 'A' | $80 */
            g_main_bank[hole + 1u] != 0xB2u ||    /* '2' | $80 */
            g_main_bank[hole + 2u] != 0xCCu ||    /* 'L' | $80 */
            g_main_bank[hole + 3u] != 0xE9u) {    /* 'i' | $80 */
            return 0u;
        }
        {
            const uint8_t mode = g_main_bank[hole + 4u];
            return (mode == 1u || mode == 2u) ? mode : 0u;
        }
    }
}

static uint32_t legacy_format_detail(uint8_t page_mode)
{
    uint32_t base;
    uint32_t page = APPLE_FB_FORMAT_PAGE_NONE;
    uint32_t video7 = APPLE_FB_FORMAT_LEGACY_VIDEO7_NONE;

    if (sw_text(s_current_sw)) {
        base = APPLE_FB_FORMAT_TEXT;
    } else {
        const uint8_t dbl =
            (sw_80col(s_current_sw) && sw_dhires(s_current_sw)) ? 1u : 0u;

        if (sw_hires(s_current_sw)) {
            base = dbl ? APPLE_FB_FORMAT_DHGR : APPLE_FB_FORMAT_HGR;
        } else {
            base = dbl ? APPLE_FB_FORMAT_DGR : APPLE_FB_FORMAT_GR;
        }
    }

    if (page_mode == 1u) {
        page = APPLE_FB_FORMAT_PAGE_INTERLACE;
    } else if (page_mode == 2u) {
        page = APPLE_FB_FORMAT_PAGE_FLIP_MERGE;
    }
    if (s_render_video7_mono_enable != 0u) {
        video7 = APPLE_FB_FORMAT_LEGACY_VIDEO7_MONO;
    } else if (base == APPLE_FB_FORMAT_DHGR &&
               s_render_dhgr_col140m_enable != 0u) {
        video7 = APPLE_FB_FORMAT_LEGACY_VIDEO7_MIX;
    }
    return APPLE_FB_FORMAT_DETAIL(base, video7, page);
}

/* Nonzero while frames render as legacy flip merges (published with
 * plain LEGACY geometry, but synthesized shadow-side like the weave). */
static uint8_t s_legacy_flip_q = 0u;
static uint8_t s_prev_legacy_flip_q = 0u;

static void render_legacy_weave_frame_full(void)
{
    if (g_atn_framebuffer == NULL) {
        return;
    }

    const uint32_t sw0 =
        s_current_sw & ~(SW_BIT(PAGE2) | SW_BIT(80STORE));
    const uint8_t hires_pair = sw_hires(s_current_sw) ? 1u : 0u;

    for (uint32_t row = 0u; row < 384u; ++row) {
        int y;
        uint32_t sw;

        if (hires_pair != 0u) {
            /* Scanline fields: even output rows PAGE1, odd PAGE2. */
            y  = (int)(row >> 1);
            sw = (row & 1u) ? (sw0 | SW_BIT(PAGE2)) : sw0;
        } else {
            /* Lores nibble-row bands: output band row>>2, page by
             * band parity, source scanline within the band's nibble
             * row (four scanlines per band). */
            y  = (int)(((row >> 3) << 2) | (row & 3u));
            sw = ((row >> 2) & 1u) ? (sw0 | SW_BIT(PAGE2)) : sw0;
        }
        const render_mode_t mode = pick_mode(sw, y);
        const int left_shift = applewin_visible_left_shift(mode);

        /* Same per-scanline chroma lock the live path applies. The
         * synthesized row skips the real colorburst cycles, so assert the
         * burst latch here before the active HGR/DHGR cells are emitted. */
        g_nColorBurstPixels   = 1024;
        g_nColorPhaseNTSC      = 0;
        g_nLastColumnPixelNTSC = 0;
        g_nSignalBitsNTSC      = 0;

        g_nVideoClockVert = y;
        g_nVideoCharSet = sw_altcharset(sw) ? 1 : 0;
        g_pVideoAddress =
            &g_atn_framebuffer[row * ATN_SCRATCH_ROW_PIXELS
                               + ATN_SCRATCH_LEFT_BORDER_PIXELS
                               + ATN_ACTIVE_X] - left_shift;

        for (uint32_t c = 0u; c < 40u; ++c) {
            g_nVideoClockHorz = (int)(ATN_SCANNER_HORZ_START + c);
            switch (mode) {
                case MODE_TEXT40: step_text40(sw); break;
                case MODE_TEXT80: step_text80(sw); break;
                case MODE_HGR:    step_hgr(sw);    break;
                case MODE_DHGR:   step_dhgr(sw);   break;
                case MODE_LORES:  step_lores(sw);  break;
                case MODE_DLORES: step_dlores(sw); break;
                default: break;
            }
        }
        g_nVideoClockHorz = (int)(ATN_SCANNER_MAX_HORZ - 1u);
        emit_shifted_right_edge_pixels(mode);
    }
}

/* Page-flip merge: both pages are complete 192-line frames meant to
 * alternate at 120 Hz. Below 120 Hz output, render each line from
 * PAGE1 directly into the slot at the normal legacy position, render
 * the PAGE2 line into a scratch row, and blend 50/50. Published as
 * plain LEGACY geometry (borders stay black; the slot is cleared on
 * entry via the flip-state change). */
static uint32_t s_legacy_merge_row[ATN_SCRATCH_ROW_PIXELS]
    __attribute__((aligned(16)));

static void render_legacy_flip_merge_frame_full(void)
{
    if (g_atn_framebuffer == NULL) {
        return;
    }

    const uint32_t sw0 =
        s_current_sw & ~(SW_BIT(PAGE2) | SW_BIT(80STORE));

    for (int y = 0; y < 192; ++y) {
        const render_mode_t mode = pick_mode(sw0, y);
        const int left_shift = applewin_visible_left_shift(mode);
        uint32_t *fb_row =
            &g_atn_framebuffer[(uint32_t)(y + (int)ATN_ACTIVE_Y) *
                               ATN_SCRATCH_ROW_PIXELS];

        g_nVideoClockVert = y;
        g_nVideoCharSet = sw_altcharset(sw0) ? 1 : 0;

        /* PAGE1 pass, straight into the slot row. */
        g_nColorBurstPixels   = 1024;
        g_nColorPhaseNTSC      = 0;
        g_nLastColumnPixelNTSC = 0;
        g_nSignalBitsNTSC      = 0;
        g_pVideoAddress = fb_row + ATN_SCRATCH_LEFT_BORDER_PIXELS +
                          ATN_ACTIVE_X - left_shift;
        for (uint32_t c = 0u; c < 40u; ++c) {
            g_nVideoClockHorz = (int)(ATN_SCANNER_HORZ_START + c);
            switch (mode) {
                case MODE_TEXT40: step_text40(sw0); break;
                case MODE_TEXT80: step_text80(sw0); break;
                case MODE_HGR:    step_hgr(sw0);    break;
                case MODE_DHGR:   step_dhgr(sw0);   break;
                case MODE_LORES:  step_lores(sw0);  break;
                case MODE_DLORES: step_dlores(sw0); break;
                default: break;
            }
        }
        g_nVideoClockHorz = (int)(ATN_SCANNER_MAX_HORZ - 1u);
        emit_shifted_right_edge_pixels(mode);

        /* PAGE2 pass into the scratch row, then blend. */
        {
            const uint32_t sw2 = sw0 | SW_BIT(PAGE2);

            memset(s_legacy_merge_row, 0, sizeof(s_legacy_merge_row));
            g_nColorBurstPixels   = 1024;
            g_nColorPhaseNTSC      = 0;
            g_nLastColumnPixelNTSC = 0;
            g_nSignalBitsNTSC      = 0;
            g_pVideoAddress = s_legacy_merge_row +
                              ATN_SCRATCH_LEFT_BORDER_PIXELS +
                              ATN_ACTIVE_X - left_shift;
            for (uint32_t c = 0u; c < 40u; ++c) {
                g_nVideoClockHorz = (int)(ATN_SCANNER_HORZ_START + c);
                switch (mode) {
                    case MODE_TEXT40: step_text40(sw2); break;
                    case MODE_TEXT80: step_text80(sw2); break;
                    case MODE_HGR:    step_hgr(sw2);    break;
                    case MODE_DHGR:   step_dhgr(sw2);   break;
                    case MODE_LORES:  step_lores(sw2);  break;
                    case MODE_DLORES: step_dlores(sw2); break;
                    default: break;
                }
            }
            g_nVideoClockHorz = (int)(ATN_SCANNER_MAX_HORZ - 1u);
            emit_shifted_right_edge_pixels(mode);

            for (uint32_t x = 0u; x < ATN_SCRATCH_ROW_PIXELS; ++x) {
                fb_row[x] = bgra_avg(fb_row[x], s_legacy_merge_row[x]);
            }
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
    s_shr_cache_invalidate = 1u;
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

    const uint8_t shr_now = vidhd_shr_enabled() ? 1u : 0u;
    if (settings == s_video_settings_seen &&
        bw_force == s_vidhd_bw_force_seen &&
        s_video7_rgb_mode == s_video7_rgb_mode_seen &&
        shr_now == s_shr_mono_gate_seen) {
        return;
    }
    /* Settings, mono gating, and ROM selection can change a decoded frame
     * without touching shadow RAM. Rebuild the next SHR frame. */
    s_shr_cache_invalidate = 1u;
    const uint8_t user_mono = apple_video_settings_mono_enabled(settings);
    /* Video-7 auto-mono is a legacy-video mode; it must never
     * grayscale an SHR frame (a stale RGB-mode latch could otherwise
     * flip a whole SHR slideshow to monochrome). SHR's own $C029 B&W
     * control (bw_force) remains honored. */
    const uint8_t video7_mono =
        ((shr_now == 0u) &&
         (apple_video_settings_video7_auto_mono_enabled(settings) != 0u) &&
         (s_video7_rgb_mode == 3u)) ? 1u : 0u;
    const uint8_t video7_auto_mono =
        ((user_mono == 0u) && (video7_mono != 0u)) ? 1u : 0u;
    const uint8_t video7_mix =
        ((shr_now == 0u) &&
         (apple_video_settings_dhgr_col140m_enabled(settings) != 0u) &&
         (s_video7_rgb_mode == 2u)) ? 1u : 0u;
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
    s_render_video7_mono_enable = video7_mono;
    s_render_dhgr_col140m_enable = video7_mix;
    s_clean_capture_phase_cycles = clean_phase;
    s_pal_capture_phase_cycles = pal_phase;
    s_video_settings_seen = settings;
    s_vidhd_bw_force_seen = bw_force;
    s_video7_rgb_mode_seen = s_video7_rgb_mode;
    s_shr_mono_gate_seen = shr_now;
}

void apple_cycle_renderer_reset_local_video_state(void)
{
    s_shr_sync_hold = 1u;
    const uint32_t text_sw = SW_BIT(TEXT);
    const uint32_t settings = apple_fb_video_settings_get();

    s_prev_valid          = 0;
    s_prev_line           = 0;
    s_frame_end_pending   = 0u;
    s_scanner_frame_lines = ATN_SCANNER_MAX_VERT_NTSC;
    s_pending_line0_mask  = 0u;
    s_vtw_1mhz_active     = 0u;
    s_vtw_pending_valid   = 0u;
    s_vtw_pending_record  = 0ULL;
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
    s_video7_an3_sequence = 0u;
    s_video7_rgb_mode_seen = 0xFFu;
    s_shr_mono_gate_seen  = 0xFFu;
    s_shr_cache_valid     = 0u;
    s_shr_cache_invalidate = 1u;
    s_shr_cache_generation = 0u;
    s_legacy_flip_q       = 0u;
    s_prev_legacy_flip_q  = 0u;

    s_frame_display_mode = APPLE_FB_DISPLAY_MODE_LEGACY;
    s_previous_frame_display_mode = APPLE_FB_DISPLAY_MODE_LEGACY;
    s_frame_format_detail = APPLE_FB_FORMAT_UNKNOWN;

    apple_pal_video_reset();
    g_resync_pending = 1u;
    apply_video_settings_if_changed();
}

static void handle_video7_softswitch_record(uint64_t rec)
{
    const uint16_t addr = ace_softswitch_access_addr(rec);
    const uint8_t low = (uint8_t)(addr & 0xFFu);
    const uint32_t sw = ace_softswitch_bits(rec);
    const uint8_t expected =
        ((s_video7_an3_sequence & 1u) == 0u) ? 0x5Fu : 0x5Eu;

    /* Video-7 selects a two-bit mode with five AN3 accesses while MIXED
     * is off: OFF-ON-OFF-ON-OFF. The first ON supplies bit 0 from
     * !80COL; the second supplies bit 1. */
    if ((low & 0xFEu) != 0x5Eu || sw_mixed(sw)) {
        s_video7_an3_sequence = 0u;
        return;
    }

    if (low != expected) {
        /* A fresh OFF can be the first access of a new sequence. */
        s_video7_an3_sequence = (low == 0x5Fu) ? 1u : 0u;
        s_video7_rgb_flags = 0u;
        return;
    }

    if (s_video7_an3_sequence == 0u) {
        s_video7_rgb_flags = 0u;
    }
    s_video7_an3_sequence++;

    if (s_video7_an3_sequence == 2u) {
        s_video7_rgb_flags = sw_80col(sw) ? 0u : 1u;
    } else if (s_video7_an3_sequence == 4u) {
        s_video7_rgb_flags = (uint8_t)(
            (s_video7_rgb_flags & 0x01u) |
            (sw_80col(sw) ? 0u : 0x02u));
    } else if (s_video7_an3_sequence == 5u) {
        const uint8_t old_mode = s_video7_rgb_mode;

        s_video7_an3_sequence = 0u;
        s_video7_rgb_mode = s_video7_rgb_flags;
        if (s_video7_rgb_mode != old_mode) {
            apply_video_settings_if_changed();
        }
    }
}

/*
 * C029 can change in the middle of a frame. Abandon the current
 * writer rather than publishing a buffer containing both legacy and
 * SHR geometry. The next line-0 marker starts a complete frame in the
 * newly selected mode; the compositor keeps showing its last coherent
 * frame until then.
 *
 * This also drops any in-flight PAL-accurate capture. SHR uses its
 * normal 640x400 renderer regardless of the configured legacy color
 * mode, and PAL capture resumes from a clean frame after SHR exits.
 */
static void shr_mode_switch_resync(void)
{
    s_render_armed = 0;
    s_frame_end_pending = 0u;
    s_pending_line0_mask = 0u;
    s_shr_cache_valid = 0u;
    s_shr_cache_invalidate = 1u;
    apple_pal_video_resync();
    g_resync_pending = 1u;
}

/* Diagnostic: PL-vs-local C029 SHR disagreements repaired. */
volatile uint32_t g_acr_shr_mode_resyncs = 0u;

/* CPU1 calls this each drain batch with the PL's authoritative
 * fake-SHR capture state. The local C029 shadow normally tracks it
 * through I/O records; if a capture drop eats the C029 record itself,
 * the two diverge and the renderer waits forever for frame markers
 * that never come (or ignores the ones arriving). Repair by adopting
 * the PL's state -- it is the side that actually gates the stream. */
void apple_cycle_renderer_sync_shr_mode(uint8_t pl_shr_active)
{
    const int local = vidhd_shr_enabled();

    if (s_shr_sync_hold) {
        if (!pl_shr_active) {
            s_shr_sync_hold = 0u;
        }
        return;
    }
    if (pl_shr_active && !local) {
        s_vidhd_newvideo |= 0xC0u;
    } else if (!pl_shr_active && local) {
        s_vidhd_newvideo &= (uint8_t)~0x40u;
    } else {
        return;
    }
    g_acr_shr_mode_resyncs++;
    apply_video_settings_if_changed();
    shr_mode_switch_resync();
}

static void handle_vidhd_io_record(uint64_t rec)
{
    const uint16_t addr = ace_io_addr(rec);
    const uint8_t data = ace_io_data(rec);
    const int was_shr = vidhd_shr_enabled();

    switch ((uint32_t)addr) {
    case 0xC022U:
        if (s_vidhd_screen_color != data) {
            s_vidhd_screen_color = data;
            s_shr_cache_invalidate = 1u;
        }
        break;
    case 0xC029U:
        s_vidhd_newvideo = data;
        s_shr_sync_hold = 0u;       /* real write beats the reset hold */
        apply_video_settings_if_changed();
        break;
    case 0xC034U:
        {
            const uint8_t color = apple_video_iigs_border_color_clamp(data);
            if (s_vidhd_border_color != color) {
                s_vidhd_border_color = color;
                s_shr_cache_invalidate = 1u;
            }
        }
        break;
    case 0xC035U:
        if (s_vidhd_shadow != data) {
            s_vidhd_shadow = data;
            s_shr_cache_invalidate = 1u;
        }
        break;
    default:
        break;
    }

    if (was_shr != vidhd_shr_enabled()) {
        shr_mode_switch_resync();
    }
}

static void publish_current_frame(void)
{
    /* Slot memory is non-cacheable, but order all completed pixel stores
     * before exposing the slot and its matching format metadata to CPU0. */
    ACR_DSB();
    apple_fb_writer_publish_frame_detail(s_frame_display_mode,
                                         s_frame_format_detail,
                                         s_vidhd_border_color);
    g_acr_frames_complete++;
}

static void on_frame_end(void) {
    /*
     * Full shadow modes publish at on_frame_start(), as soon as their frame
     * is complete. Only the normal legacy path waits for PAL completion here.
     */
    const uint8_t synthesized =
        (s_frame_display_mode == APPLE_FB_DISPLAY_MODE_SHR ||
         s_frame_display_mode == APPLE_FB_DISPLAY_MODE_LEGACY_I ||
         s_legacy_flip_q != 0u) ? 1u : 0u;
    const uint8_t frame_ready =
        synthesized ? 0u : apple_pal_video_end_frame();

    g_acr_frame_edges_seen++;

    /* Atomic handoff: claim the current idle slot as our next writer
     * slot, demote our just-finished slot to idle, set idle_ready=1.
     * Reader (compositor) picks up the new idle next time it polls.
     *
     * PAL accurate rendering is line-deferred. CPU1 may spend later Apple
     * frames finishing one accepted exact PAL capture; only publish when that
     * delayed frame is complete, never while a writer slot contains partial
     * PAL scanlines. */
    if (frame_ready != 0u) {
        publish_current_frame();
    }
    g_acr_last_frame_records = s_records_in_frame;
    s_records_in_frame = 0u;
}

static void on_frame_start(void) {
    /* Apply CPU0 settings before deriving this frame's format metadata, so
     * the badge tag and the pixels always describe the same frame. */
    apply_video_settings_if_changed();

    /* Bind the NTSC core to the fresh writer slot selected by the handoff
     * before the first cycle of the frame writes pixels. */
    s_cached_writer_slot = apple_fb_writer_slot();

    uint32_t *slot_addr =
        (uint32_t *)(uintptr_t)comp_apple_slot_addr[s_cached_writer_slot];
    {
        const uint8_t legacy_paged =
            vidhd_shr_enabled() ? 0u : legacy_paged_mode();

        s_legacy_flip_q = (legacy_paged == 2u) ? 1u : 0u;
        s_frame_display_mode = vidhd_shr_enabled()
            ? APPLE_FB_DISPLAY_MODE_SHR
            : (legacy_paged == 1u ? APPLE_FB_DISPLAY_MODE_LEGACY_I
                                  : APPLE_FB_DISPLAY_MODE_LEGACY);
        if (!vidhd_shr_enabled()) {
            s_frame_format_detail = legacy_format_detail(legacy_paged);
        }
    }

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
    if (s_just_resynced ||
        s_frame_display_mode != s_previous_frame_display_mode ||
        s_legacy_flip_q != s_prev_legacy_flip_q) {
        memset(slot_addr, 0, COMP_APPLE_BYTES);
        s_just_resynced = 0;
    }
    s_previous_frame_display_mode = s_frame_display_mode;
    s_prev_legacy_flip_q = s_legacy_flip_q;

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
    if (s_frame_display_mode == APPLE_FB_DISPLAY_MODE_SHR) {
        const uint32_t generation = g_shr_shadow_generation;
        const uint8_t rebuild =
            (s_shr_cache_valid == 0u ||
             s_shr_cache_invalidate != 0u ||
             generation != s_shr_cache_generation) ? 1u : 0u;

        if (rebuild != 0u) {
            render_shr_frame_full();
            publish_current_frame();
            s_shr_cache_generation = g_shr_shadow_generation;
            s_shr_cache_valid = 1u;
            s_shr_cache_invalidate = 0u;
            g_acr_shr_cache_rebuilds++;
        } else {
            g_acr_shr_frames_skipped++;
        }
    } else if (s_frame_display_mode == APPLE_FB_DISPLAY_MODE_LEGACY_I) {
        render_legacy_weave_frame_full();
        publish_current_frame();
    } else if (s_legacy_flip_q != 0u) {
        render_legacy_flip_merge_frame_full();
        publish_current_frame();
    } else {
        apple_pal_video_begin_frame();
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
    ACR_DSB();

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

    ACR_DSB();

    uart_puts(UART0_BASE,
        "apple_cycle_renderer_init: 3-slot Apple FB ring at 0x3F300000+, "
        "legacy 616x224 / VidHD SHR 640x400 BGRA32\r\n");
    return 0;
}

/* ---------- Per-record dispatch ---------- */

static void apple_cycle_renderer_dispatch_record(uint64_t rec) {
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
                /* Close the old marker, then either rebuild and publish the
                 * changed shadow or keep the last static frame published. */
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
            /* A finishing woven or flip-merged frame has no border
             * geometry to complete; emitting here would scribble on
             * synthesized rows. */
            if (s_frame_display_mode != APPLE_FB_DISPLAY_MODE_LEGACY_I &&
                s_legacy_flip_q == 0u) {
                uint32_t border_line;
                uint32_t border_cycle;
                const int8_t phase =
                    (apple_pal_video_mode_is_active(s_render_color_mode) != 0u) ?
                    s_pal_capture_phase_cycles : s_clean_capture_phase_cycles;

                capture_to_scanner_phase(line, cycle, phase,
                                         &border_line, &border_cycle);
                emit_border_cycle(border_line, border_cycle);
            }
            s_pending_line0_sw[cycle] = sw;
            s_pending_line0_mask |= (uint8_t)(1u << cycle);
            g_acr_cycles_rendered++;
            s_records_in_frame++;
            return;
        }

        {
            const int was_weave =
                (s_frame_display_mode == APPLE_FB_DISPLAY_MODE_LEGACY_I) ||
                (s_legacy_flip_q != 0u);
            on_frame_end();
            on_frame_start();
            s_frame_end_pending = 0u;
            if (!was_weave &&
                apple_pal_video_mode_is_active(s_render_color_mode) != 0u) {
                for (uint32_t i = 0u; i < ATN_BORDER_H_CYCLES; ++i) {
                    if ((s_pending_line0_mask & (uint8_t)(1u << i)) != 0u) {
                        apple_pal_video_on_cycle(0u, i, s_pending_line0_sw[i]);
                    }
                }
            }
        }
        s_pending_line0_mask = 0u;
    }

    if (shr_active ||
        s_frame_display_mode == APPLE_FB_DISPLAY_MODE_LEGACY_I ||
        s_legacy_flip_q != 0u) {
        /* C029 SHR, the legacy interlace weave, and the legacy flip
         * merge own the frame geometry and pixel decode while active.
         * on_frame_start() renders the complete shadow frame; the
         * stream only drives frame edges. The soft-switch snapshot
         * MUST keep tracking here: legacy_paged_mode() gates on
         * s_current_sw at every frame start, and without this update
         * a weave stayed latched after the machine dropped to TEXT
         * (caught by the host harness, T2). */
        s_current_sw = sw;
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

static int vtw_record_has_video_state(uint64_t rec)
{
    if (rec == 0ULL) {
        return 0;
    }
    if (ace_record_kind(rec) == ACE_RECORD_KIND_SOFTSW_ACCESS) {
        return 1;
    }
    return ace_record_kind(rec) == ACE_RECORD_KIND_LEGACY &&
           ace_frame_en(rec);
}

static int vtw_records_are_consecutive(uint64_t prior, uint64_t next)
{
    const uint32_t prior_line = ace_line_in_frame(prior);
    const uint32_t prior_cycle = ace_cycle_in_line(prior);
    const uint32_t next_line = ace_line_in_frame(next);
    const uint32_t next_cycle = ace_cycle_in_line(next);

    if (prior_cycle < (ATN_SCANNER_MAX_HORZ - 1u)) {
        return next_line == prior_line && next_cycle == prior_cycle + 1u;
    }
    if (next_cycle != 0u) {
        return 0;
    }
    if (next_line == prior_line + 1u) {
        return 1;
    }
    return next_line == 0u &&
           (prior_line == (ATN_SCANNER_MAX_VERT_NTSC - 1u) ||
            prior_line == (ATN_SCANNER_MAX_VERT_PAL - 1u));
}

static uint64_t vtw_record_with_advanced_video_state(uint64_t prior,
                                                      uint64_t next)
{
    const uint64_t packed_mask =
        (uint64_t)VTW_PHASE_SW_MASK << ACE_BIT_SW_DHIRES;
    const uint64_t next_bits =
        (uint64_t)(ace_softswitch_bits(next) & VTW_PHASE_SW_MASK)
        << ACE_BIT_SW_DHIRES;

    return (prior & ~packed_mask) | next_bits;
}

void apple_cycle_renderer_set_vtw_1mhz(uint8_t active)
{
    active = active != 0u ? 1u : 0u;
    if (s_vtw_1mhz_active != 0u && active == 0u &&
        s_vtw_pending_valid != 0u) {
        apple_cycle_renderer_dispatch_record(s_vtw_pending_record);
        s_vtw_pending_valid = 0u;
    }
    s_vtw_1mhz_active = active;
}

void apple_cycle_renderer_on_next_record(uint64_t rec)
{
    uint64_t pending;

    if (s_vtw_pending_valid == 0u) {
        return;
    }

    pending = s_vtw_pending_record;
    if (s_vtw_1mhz_active != 0u &&
        vtw_record_has_video_state(rec) &&
        vtw_records_are_consecutive(pending, rec)) {
        pending = vtw_record_with_advanced_video_state(pending, rec);
    }

    /*
     * apple_cycle_egress calls this before applying rec's shadow-memory
     * write, so the prior cycle sees next-cycle mode bits without seeing
     * next-cycle RAM contents.
     */
    apple_cycle_renderer_dispatch_record(pending);
    s_vtw_pending_valid = 0u;
}

/* Diagnostic: one-line A2Li/SHR gate dump over UART0, printed from
 * CPU1's coherent view of the shadow (CPU0 must not read the banks;
 * see apple_fb_debug_dump_set). Triggered by the CPU0 console command
 * "shadow a2li". Answers, on live hardware: did the A2Li signature
 * reach the shadow, what do the gate inputs say, and what mode did
 * the last frame select. */
static void acr_put_hex8(uint8_t v)
{
    static const char hex[] = "0123456789ABCDEF";

    uart_putc(UART0_BASE, hex[v >> 4]);
    uart_putc(UART0_BASE, hex[v & 0x0Fu]);
}

static void acr_put_hex16(uint16_t v)
{
    acr_put_hex8((uint8_t)(v >> 8));
    acr_put_hex8((uint8_t)v);
}

static void acr_put_bytes(const char *tag, const volatile uint8_t *bank,
                          uint16_t addr, uint32_t count)
{
    uart_puts(UART0_BASE, tag);
    for (uint32_t i = 0u; i < count; ++i) {
        acr_put_hex8(bank[(uint16_t)(addr + i)]);
        if (i + 1u < count) uart_putc(UART0_BASE, ' ');
    }
}

void apple_cycle_renderer_debug_a2li_line(void)
{
    uart_puts(UART0_BASE, "[cpu1] a2li:");
    acr_put_bytes(" hgr@4078=", g_main_bank, 0x4078u, 5u);
    acr_put_bytes(" lgr@0878=", g_main_bank, 0x0878u, 5u);
    uart_puts(UART0_BASE, " text=");
    uart_putc(UART0_BASE, sw_text(s_current_sw) ? '1' : '0');
    uart_puts(UART0_BASE, " hires=");
    uart_putc(UART0_BASE, sw_hires(s_current_sw) ? '1' : '0');
    uart_puts(UART0_BASE, " 80store=");
    uart_putc(UART0_BASE, sw_80store(s_current_sw) ? '1' : '0');
    uart_puts(UART0_BASE, " 80col=");
    uart_putc(UART0_BASE, sw_80col(s_current_sw) ? '1' : '0');
    uart_puts(UART0_BASE, " dhires=");
    uart_putc(UART0_BASE, sw_dhires(s_current_sw) ? '1' : '0');
    uart_puts(UART0_BASE, " paged=");
    uart_putc(UART0_BASE, (char)('0' + legacy_paged_mode()));
    uart_puts(UART0_BASE, " dispmode=");
    uart_putc(UART0_BASE, (char)('0' + s_frame_display_mode));
    uart_puts(UART0_BASE, " pubmode=");
    uart_putc(UART0_BASE,
              (char)('0' + apple_fb_reader_published_display_mode()));
    uart_puts(UART0_BASE, " flipq=");
    uart_putc(UART0_BASE, (char)('0' + s_legacy_flip_q));
    uart_puts(UART0_BASE, " nv=");
    acr_put_hex8(s_vidhd_newvideo);
    acr_put_bytes(" auxctrl@9DF8=", g_aux_bank, 0x9DF8u, 8u);
    uart_puts(UART0_BASE, " localfmt=");
    acr_put_hex16((uint16_t)s_frame_format_detail);
    uart_puts(UART0_BASE, " pubfmt=");
    acr_put_hex16((uint16_t)apple_fb_reader_published_format_detail());
    uart_puts(UART0_BASE, "\r\n");
}

void apple_cycle_renderer_on_record(uint64_t rec)
{
    if (s_vtw_1mhz_active != 0u &&
        rec != 0ULL &&
        ace_record_kind(rec) == ACE_RECORD_KIND_LEGACY &&
        ace_frame_en(rec)) {
        s_vtw_pending_record = rec;
        s_vtw_pending_valid = 1u;
        return;
    }

    apple_cycle_renderer_dispatch_record(rec);
}
