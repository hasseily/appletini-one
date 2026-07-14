#include "apple_pal_video_timing.h"

#include <string.h>

#include "../lib/common.h"

#include "apple_cycle_egress.h"
#include "appletini_csbits.h"
#include "appletini_ntsc.h"
#include "video_output.h"

#define PAL_LINE_CYCLES          65u
#define PAL_VISIBLE_LINES        192u
#define PAL_LINE_RING            200u  /* render-queue depth; >= one frame of lines */
#define PAL_PUMP_MAX_LINES       4u    /* lines per main-loop pump call -- small so the
                                        * egress drain is re-serviced often, keeping the
                                        * capture ring empty enough that the frame-end
                                        * flush below fits without overflowing the FIFO */
#define PAL_FLUSH_MAX_LINES      64u   /* bounded frame-edge render budget; sized below
                                        * the egress ring headroom so exact frame work can
                                        * catch up without starving capture too long */
#define PAL_BYTES_PER_LINE       40u
#define PAL_SCANNER_HBL_CYCLES   25u
#define PAL_RENDER_SAMPLES       ATN_ACTIVE_WIDTH
/* Left-edge pipeline latency, in 14 MHz samples, skipped from the front of the
 * rendered line so the first VISIBLE dot lands at the left edge. It is the LS166
 * load-to-output delay, which is a fixed number of *shift clocks*:
 *   - single resolution: the shift register clocks at 7 MHz (one shift = two
 *     samples) -> PAL_OUTPUT_LATENCY_SAMPLES samples of latency.
 *   - double resolution (80-col / DLORES / DHGR): it clocks at 14 MHz (one
 *     shift = one sample), so the same delay is HALF as many samples ->
 *     PAL_OUTPUT_LATENCY_DOUBLE. Using the single-res value in double res
 *     over-skips: the image shifts left by ~one column and a column is
 *     duplicated at the right edge. pal_copy_visible_output's callers pick the
 *     per-line value from the start-of-line 80-col state.
 * These two values are the knobs to tune if the left edge is off on hardware. */
#define PAL_OUTPUT_LATENCY_SAMPLES 14u   /* single res; also the max, used for sizing */
#define PAL_OUTPUT_LATENCY_DOUBLE  7u    /* double res (14 MHz shift) = half */
#define PAL_RENDER_WORK_SAMPLES  (PAL_RENDER_SAMPLES + PAL_OUTPUT_LATENCY_SAMPLES)
#define PAL_RENDER_TICKS         (PAL_RENDER_WORK_SAMPLES * 2u)
/* sw_by_byte must cover the largest tick-table soft-switch index used by the
 * slow path, which is idx_load44 = (i + 44) / 28 at the final render tick
 * (i = (PAL_RENDER_WORK_SAMPLES - 1) * 2). Derive it from the work-sample count
 * so it tracks PAL_OUTPUT_LATENCY_SAMPLES automatically. It evaluates to 43
 * at 574 work samples and 44 at 588; a fixed 43 would overflow the latter. */
#define PAL_SW_LOOKAHEAD_BYTES \
    ((((PAL_RENDER_WORK_SAMPLES - 1u) * 2u + 44u) / 28u) + 1u)
#define PAL_RAM_SAMPLE_MASK      ((1ULL << PAL_BYTES_PER_LINE) - 1ULL)
#define PAL_MONO_COLOR_COUNT     4u

#define PAL_PROFILE_TMR_COUNT_LO 0xF8F00200U
#define PAL_RENDER_TIMING_PROFILE 0u

#define PAL_SW_GR       0u
#define PAL_SW_MIXED    1u
#define PAL_SW_AN3      2u
#define PAL_SW_TEXT     3u
#define PAL_SW_80COL    4u
#define PAL_SW_HIRES    5u
#define PAL_SW_ALTCHAR  6u
#define PAL_SW_PAGE2    7u

#define PAL_FLAG_COLOUR_BURST 1u

/* The PAL composite simulation produces a 1-bit composite stream; the NTSC
 * colorizer (appletini_ntsc) turns that into artifact colour exactly the way
 * the standard composite modes do: each dot is shifted into a 12-bit signal
 * window and looked up in g_aHue*[phase][window]. Those chroma tables are built
 * assuming the first *visible* dot lands on colour phase 2 (the standard
 * renderer emits two left-border dots first). The PAL stream starts at the first
 * visible dot, so we seed the phase here. If colours come out hue-rotated on
 * hardware, this is the single knob to turn -- each +1 rotates every hue by one
 * colour clock (90 degrees). Value 2 put the decoder one colour clock behind the
 * signal (HGR blue decoded as violet, orange as green -- the high-bit palette
 * looked inverted); 3 advances the phase to align it. The four values cover the
 * four rotations, so if hues are still off, step this and rebuild. */
#define PAL_NTSC_PHASE_OFFSET 3u

typedef struct {
    uint8_t pal_sw[PAL_LINE_CYCLES];
    uint32_t ace_sw[PAL_LINE_CYCLES];
    uint8_t scanned_bytes[PAL_BYTES_PER_LINE * 2u];
    uint64_t cycle_mask_lo;
    uint64_t ram_sample_mask;
    uint32_t fallback_ace_sw;
    uint8_t fallback_pal_sw;
    uint8_t valid;
    uint8_t max_cycle_seen;
    uint8_t cycle64_seen;
    uint32_t y;
} pal_line_buffer_t;

typedef struct {
    uint8_t value;
    int future_time;
} pal_delay_t;

static uint32_t pal_pack_bgra(uint8_t r, uint8_t g, uint8_t b);
static uint32_t pal_pack_atn_bgra(atn_bgra_t c);
static uint32_t pal_apply_mono_color(uint8_t mono_color,
                                     uint8_t r,
                                     uint8_t g,
                                     uint8_t b);
static void pal_init_packed_tables(void);
static void pal_clear_pipeline(void);

static uint32_t *s_framebuffer;
static uint8_t s_mono_enable;
static uint8_t s_mono_color = APPLE_VIDEO_MONO_WHITE;
static uint8_t s_color_mode = APPLE_VIDEO_COLOR_COMPOSITE_MONITOR;

static pal_line_buffer_t s_current_line;
static pal_line_buffer_t s_preroll_line;
static pal_line_buffer_t s_line_ring[PAL_LINE_RING];
static uint32_t s_ring_head;
static uint32_t s_ring_tail;
static uint32_t s_ring_count;
static uint32_t s_last_sw;
static uint8_t s_have_last_sw;
static uint8_t s_frame_failed;
static uint8_t s_capture_enabled;
static uint8_t s_capture_frame_open;
static uint8_t s_render_frame_open;
static uint8_t s_frame_ready_pending;
static uint32_t s_frame_lines_queued;
static uint32_t s_frame_lines_rendered;

/* One scanline of finished BGRA pixels, rendered here in cacheable DDR and then
 * bulk-copied to the non-cacheable framebuffer slot once per line. Scattered
 * per-pixel stores straight to the non-cacheable slot do not write-combine and
 * cost a large share of the per-line render time; a single memcpy bursts. */
static uint32_t s_render_line[PAL_RENDER_WORK_SAMPLES];

static uint32_t s_pal_hue_monitor_packed[ATN_NUM_PHASES][ATN_NUM_SEQUENCES];
static uint32_t s_pal_hue_color_tv_packed[ATN_NUM_PHASES][ATN_NUM_SEQUENCES];
static uint32_t s_pal_bw_monitor_packed[ATN_NUM_SEQUENCES];
static uint32_t s_pal_bw_color_tv_packed[ATN_NUM_SEQUENCES];
static uint32_t s_pal_mono_monitor_packed[PAL_MONO_COLOR_COUNT][ATN_NUM_SEQUENCES];
static uint8_t s_pal_packed_tables_ready;

volatile uint32_t g_pal_frames_published = 0u;
volatile uint32_t g_pal_frames_dropped = 0u;
volatile uint32_t g_pal_lines_rendered = 0u;
volatile uint32_t g_pal_lines_incomplete = 0u;
volatile uint32_t g_pal_queue_overflows = 0u;
volatile uint32_t g_pal_queue_max = 0u;
volatile uint32_t g_pal_fast_lines = 0u;
volatile uint32_t g_pal_slow_lines = 0u;
volatile uint32_t g_pal_render_ticks_total = 0u;
volatile uint32_t g_pal_render_ticks_max = 0u;
volatile uint32_t g_pal_end_frame_count = 0u;
volatile uint32_t g_pal_end_queue_total = 0u;
volatile uint32_t g_pal_end_queue_max = 0u;
volatile uint32_t g_pal_end_lines_drained = 0u;

#if PAL_RENDER_TIMING_PROFILE
static uint32_t pal_profile_timer_read32(void)
{
    return REG_READ(PAL_PROFILE_TMR_COUNT_LO);
}

static void pal_profile_record_render_ticks(uint32_t start_ticks)
{
    const uint32_t delta =
        (uint32_t)(pal_profile_timer_read32() - start_ticks);

    g_pal_render_ticks_total += delta;
    if (delta > g_pal_render_ticks_max) {
        g_pal_render_ticks_max = delta;
    }
}
#endif

uint8_t apple_pal_video_mode_is_active(uint8_t color_mode)
{
    return apple_video_color_mode_is_pal_accurate(color_mode);
}

void apple_pal_video_set_framebuffer(uint32_t *fb)
{
    s_framebuffer = fb;
}

void apple_pal_video_set_video_output(uint8_t mono_enable,
                                      uint8_t mono_color,
                                      uint8_t color_mode)
{
    const uint8_t new_mono_enable = (mono_enable != 0u) ? 1u : 0u;
    const uint8_t new_mono_color = apple_video_mono_color_clamp(mono_color);
    const uint8_t new_color_mode = apple_video_color_mode_clamp(color_mode);
    const uint8_t changed =
        (uint8_t)(s_mono_enable != new_mono_enable ||
                  s_mono_color != new_mono_color ||
                  s_color_mode != new_color_mode);

    s_mono_enable = new_mono_enable;
    s_mono_color = new_mono_color;
    s_color_mode = new_color_mode;
    if (changed != 0u) {
        pal_clear_pipeline();
    }
}

static uint8_t ace_sw_bit(uint32_t sw, uint32_t bit)
{
    return (uint8_t)((sw >> bit) & 1u);
}

static uint8_t ace_text(uint32_t sw)    { return ace_sw_bit(sw, ACE_SWB_TEXT_BIT); }
static uint8_t ace_mixed(uint32_t sw)   { return ace_sw_bit(sw, ACE_SWB_MIXED_BIT); }
static uint8_t ace_hires(uint32_t sw)   { return ace_sw_bit(sw, ACE_SWB_HIRES_BIT); }
static uint8_t ace_page2(uint32_t sw)   { return ace_sw_bit(sw, ACE_SWB_PAGE2_BIT); }
static uint8_t ace_80col(uint32_t sw)   { return ace_sw_bit(sw, ACE_SWB_80COL_BIT); }
static uint8_t ace_an3(uint32_t sw)
{
    /* Accurapple's PAL shader models AN3 polarity, while our captured
     * soft-switch word stores DHIRES polarity: C05E sets DHIRES and clears
     * AN3, C05F clears DHIRES and sets AN3. */
    return (uint8_t)(ace_sw_bit(sw, ACE_SWB_DHIRES_BIT) == 0u);
}
static uint8_t ace_altchar(uint32_t sw) { return ace_sw_bit(sw, ACE_SWB_ALTCHARSET_BIT); }
static uint8_t ace_80store(uint32_t sw) { return ace_sw_bit(sw, ACE_SWB_80STORE_BIT); }

static uint8_t ace_is_graphics(uint32_t sw, uint32_t y)
{
    return (uint8_t)((ace_text(sw) == 0u &&
                     !(y >= ATN_SCANNER_Y_MIXED && ace_mixed(sw) != 0u)) ? 1u : 0u);
}

static uint8_t pal_flags_from_ace(uint32_t sw, uint32_t y)
{
    uint8_t flags = 0u;

    if (ace_is_graphics(sw, y) != 0u) flags |= (uint8_t)(1u << PAL_SW_GR);
    if (ace_mixed(sw) != 0u)          flags |= (uint8_t)(1u << PAL_SW_MIXED);
    if (ace_an3(sw) != 0u)            flags |= (uint8_t)(1u << PAL_SW_AN3);
    if (ace_text(sw) != 0u)           flags |= (uint8_t)(1u << PAL_SW_TEXT);
    if (ace_80col(sw) != 0u)          flags |= (uint8_t)(1u << PAL_SW_80COL);
    if (ace_hires(sw) != 0u)          flags |= (uint8_t)(1u << PAL_SW_HIRES);
    if (ace_altchar(sw) != 0u)        flags |= (uint8_t)(1u << PAL_SW_ALTCHAR);
    if (ace_page2(sw) != 0u)          flags |= (uint8_t)(1u << PAL_SW_PAGE2);

    return flags;
}

static inline uint8_t pal_is_set(uint8_t flags, uint32_t bit)
{
    return (uint8_t)(((flags >> bit) & 1u) != 0u);
}

static inline int pal_bnot(int b)
{
    return 1 - b;
}

static inline uint8_t pal_is_graphics_flags(uint8_t flags, uint32_t y)
{
    const int text = pal_is_set(flags, PAL_SW_TEXT) ? 1 : 0;
    const int mixed = pal_is_set(flags, PAL_SW_MIXED) ? 1 : 0;
    const int on_mix_line = (y >= ATN_SCANNER_Y_MIXED) ? 1 : 0;
    return (uint8_t)((pal_bnot(text) & pal_bnot(on_mix_line & mixed)) != 0);
}

static inline uint8_t pal_compute_gr2p_hal(uint8_t flags, uint32_t y)
{
    if (pal_is_graphics_flags(flags, y) != 0u) {
        return (uint8_t)(pal_is_set(flags, PAL_SW_AN3) == 0u);
    }
    return 1u;
}

static inline int pal_compute_va(int y) { return y & 1; }
static inline int pal_compute_vb(int y) { return (y & 2) >> 1; }
static inline int pal_compute_vc(int y) { return (y & 4) >> 2; }

static inline uint8_t pal_compute_sega(int graphics_time, int va, uint8_t h0)
{
    return (uint8_t)((graphics_time != 0) ? (h0 == 0u) : (va == 1));
}

static inline uint8_t pal_compute_segb(int graphics_time, int vb, int hires)
{
    return (uint8_t)((graphics_time != 0) ? (pal_bnot(hires) == 1) : (vb == 1));
}

static inline uint8_t pal_compute_segc(int vc)
{
    return (uint8_t)(vc == 1);
}

static uint16_t pal_text_address(uint32_t y)
{
    const uint32_t text_y = y / 8u;
    uint16_t ofs = 0u;

    if (text_y >= 8u && text_y <= 15u) {
        ofs = 0x28u;
    } else if (text_y >= 16u) {
        ofs = 0x50u;
    }

    return (uint16_t)(ofs + (uint16_t)((text_y & 7u) * 0x80u));
}

static uint16_t pal_hgr_address(uint32_t y)
{
    uint16_t ofs = 0u;
    const uint32_t y64 = y % 64u;
    const uint32_t i = y64 >> 3u;
    const uint32_t j = y64 & 7u;

    if (y >= 64u && y <= 127u) {
        ofs = 0x28u;
    } else if (y >= 128u) {
        ofs = 0x50u;
    }

    return (uint16_t)(ofs + (uint16_t)(0x80u * i) + (uint16_t)(0x400u * j));
}

static uint8_t pal_line_pal_sw_at(const pal_line_buffer_t *line, int index)
{
    if (line == NULL || line->valid == 0u) {
        return pal_flags_from_ace(s_last_sw, 0u);
    }
    if (index < 0) {
        index = 0;
    } else if (index >= (int)PAL_LINE_CYCLES) {
        return line->fallback_pal_sw;
    }
    return line->pal_sw[index];
}

static uint32_t pal_line_ace_sw_at(const pal_line_buffer_t *line, int index)
{
    if (line == NULL || line->valid == 0u) {
        return s_last_sw;
    }
    if (index < 0) {
        index = 0;
    } else if (index >= (int)PAL_LINE_CYCLES) {
        return line->fallback_ace_sw;
    }
    return line->ace_sw[index];
}

static inline uint8_t pal_read_rom_byte(uint8_t flags,
                                        int gr_time,
                                        int sega,
                                        int segb,
                                        int segc,
                                        uint8_t current_byte)
{
    const int alt_charset = pal_is_set(flags, PAL_SW_ALTCHAR) ? 1 : 0;
    const int flash = (g_nTextFlashMask != 0u) ? 1 : 0;
    const int vid05 = current_byte & 63;
    const int vid6 = (current_byte & 64u) >> 6;
    const int vid7 = (current_byte & 128u) >> 7;
    const int gr2 = gr_time;
    const int ra9 = vid6 & (vid7 | gr2 | alt_charset);
    const int ra10 = vid7 | (pal_bnot(gr2) & vid6 & flash & pal_bnot(alt_charset));
    const int ra11 = gr2;
    const uint16_t addr = (uint16_t)((sega + (segb << 1) + (segc << 2)) |
                                     (vid05 << 3) |
                                     ((ra9 << 9) + (ra10 << 10) + (ra11 << 11)));

    return appletini_video_rom_read(addr);
}

static inline uint8_t pal_ror(uint8_t x)
{
    return (uint8_t)(((x >> 1u) & 127u) | ((x & 1u) << 7u));
}

static uint16_t pal_scanned_byte_address(const pal_line_buffer_t *line,
                                         uint32_t x)
{
    const uint32_t y = line->y;
    const int display_index = (int)PAL_SCANNER_HBL_CYCLES + (int)x;
    const uint32_t sw = pal_line_ace_sw_at(line, display_index);
    const int gr_index = (int)PAL_SCANNER_HBL_CYCLES + ((x <= 1u) ? 0 : ((int)x - 1));
    const uint32_t gr_sw = pal_line_ace_sw_at(line, gr_index);
    const uint8_t page2 = (uint8_t)((ace_page2(sw) != 0u && ace_80store(sw) == 0u) ? 1u : 0u);
    const uint8_t gr1 = ace_is_graphics(gr_sw, y);
    const uint8_t hires_time_gr1 = (uint8_t)((gr1 != 0u && ace_hires(sw) != 0u) ? 1u : 0u);
    const uint16_t page_base = hires_time_gr1 ?
        (uint16_t)(page2 ? 0x4000u : 0x2000u) :
        (uint16_t)(page2 ? 0x0800u : 0x0400u);

    return (uint16_t)(page_base +
        (hires_time_gr1 ? pal_hgr_address(y) : pal_text_address(y)) +
        (uint16_t)x);
}

static void pal_sample_scanned_ram_for_cycle(pal_line_buffer_t *line,
                                             uint32_t cycle)
{
    uint32_t x;
    uint16_t addr;

    if (line == NULL ||
        line->valid == 0u ||
        cycle < PAL_SCANNER_HBL_CYCLES ||
        cycle >= PAL_LINE_CYCLES) {
        return;
    }

    /* Accurapple's read_scanned_ram consumes byte_offset 0..39 on the
     * 28 MHz grid. Those byte_offsets correspond to scanner cycles 25..64
     * after HBL. Latch both motherboard and aux bytes here, at that cycle,
     * so deferred rendering cannot accidentally read a later shadow-bank
     * state after Apple code has already modified video memory. */
    x = cycle - PAL_SCANNER_HBL_CYCLES;
    addr = pal_scanned_byte_address(line, x);
    line->scanned_bytes[(x * 2u) + 0u] = g_main_bank[addr];
    line->scanned_bytes[(x * 2u) + 1u] = g_aux_bank[addr];
    line->ram_sample_mask |= (1ULL << x);
}

static uint8_t pal_colour_burst_for_line(const pal_line_buffer_t *line)
{
    uint32_t i;
    uint8_t gr_flags = 0u;

    for (i = 13u; i < 17u; ++i) {
        if (pal_is_graphics_flags(pal_line_pal_sw_at(line, (int)i), line->y) != 0u) {
            gr_flags++;
        }
    }

    return (uint8_t)((gr_flags >= 1u) ? PAL_FLAG_COLOUR_BURST : 0u);
}

static void pal_build_sw_by_byte(const pal_line_buffer_t *line,
                                 const pal_line_buffer_t *next,
                                 uint8_t *sw_by_byte)
{
    const uint8_t fallback =
        (line != NULL && line->valid != 0u) ?
        line->fallback_pal_sw : pal_flags_from_ace(s_last_sw, 0u);
    uint32_t b;

    for (b = 0u; b < PAL_SW_LOOKAHEAD_BYTES; ++b) {
        const uint32_t index = b + PAL_SCANNER_HBL_CYCLES;

        if (index < PAL_LINE_CYCLES) {
            sw_by_byte[b] = line->pal_sw[index];
        } else if (next != NULL && next->valid != 0u) {
            uint32_t next_index = index - PAL_LINE_CYCLES;
            if (next_index >= PAL_LINE_CYCLES) {
                sw_by_byte[b] = next->fallback_pal_sw;
                continue;
            }
            sw_by_byte[b] = next->pal_sw[next_index];
        } else {
            sw_by_byte[b] = fallback;
        }
    }
}

/* The per-28 MHz-tick control signals (byte_offset, phase0, SIG_7M, RASRISE1,
 * LDPS_CHECK, the soft-switch sampling indices, ...) are pure functions of the
 * tick index i and are identical for every scanline and frame. Precomputing
 * them avoids repeated integer division in CPU1's egress-drain loop and keeps
 * the per-line hot path to table reads. The equations are documented below. */
typedef struct {
    uint16_t time;           /* original 28 MHz tick index i */
    uint8_t byte_offset;     /* i / 28 */
    uint8_t ram_index;       /* byte_offset * 2 + phase0 */
    uint8_t ram_valid;       /* byte_offset is inside the 40 scanned bytes */
    uint8_t phase0;          /* 1 - (i / 14) % 2 */
    uint8_t sig_7m;          /* (i >> 1) & 1 */
    uint8_t rasrise1;        /* (i % 28) == 26 */
    uint8_t load_sw;         /* (i % 28) == 12 */
    uint8_t clr_ref;         /* (i / 4) % 2 == 0 */
    uint8_t h0;              /* ((i - 1) / 28) % 2 == 0 */
    uint8_t ldps_check;      /* ((i - 2) % 28) == 22 */
    uint8_t ldps_check_p01;  /* LDPS_CHECK or ((i - 2) % 28) == 8 */
    uint8_t ldps_hgr_check;  /* ((i - 2) % 28) == 24 */
    uint8_t idx_old;         /* sw_by_byte index ((i > 0) ? i - 1 : 0) / 28 */
    uint8_t idx_old3;        /* sw_by_byte index (i - 1 + 38) / 28 */
    uint8_t idx_new3;        /* sw_by_byte index (i + 38) / 28 */
    uint8_t idx_load44;      /* soft-switch sample index (i + 44) / 28 */
    uint8_t idx_load16;      /* 80COL sample index (i + 16) / 28 */
    uint8_t byte_edge;       /* idx_old != byte_offset */
    uint8_t an3_edge;        /* idx_old3 != idx_new3 */
} pal_tick_t;

static pal_tick_t s_tick[PAL_RENDER_WORK_SAMPLES];
static uint8_t s_tick_ready;

static void pal_init_tick_table(void)
{
    uint32_t sample;

    if (s_tick_ready != 0u) {
        return;
    }
    for (sample = 0u; sample < PAL_RENDER_WORK_SAMPLES; ++sample) {
        const int i = (int)(sample * 2u);
        pal_tick_t *t = &s_tick[sample];
        const int d3 = 38;

        t->time           = (uint16_t)i;
        t->byte_offset    = (uint8_t)(i / 28);
        t->phase0         = (uint8_t)(1 - ((i / 14) % 2));
        t->ram_valid      = (uint8_t)(t->byte_offset < PAL_BYTES_PER_LINE);
        t->ram_index      = (uint8_t)((t->byte_offset * 2u) + t->phase0);
        t->sig_7m         = (uint8_t)((((uint32_t)i >> 1u) & 1u) != 0u);
        t->rasrise1       = (uint8_t)((i % 28) == 26);
        t->load_sw        = (uint8_t)((i % 28) == 12);
        t->clr_ref        = (uint8_t)(((i / 4) % 2) == 0);
        t->h0             = (uint8_t)((((i - 1) / 28) % 2) == 0);
        t->ldps_check     = (uint8_t)(((i - 2) % 28) == 22);
        t->ldps_check_p01 = (uint8_t)((((i - 2) % 28) == 22) ||
                                      (((i - 2) % 28) == ((22 + 14) % 28)));
        t->ldps_hgr_check = (uint8_t)(((i - 2) % 28) == ((14 - 2) * 2));
        t->idx_old        = (uint8_t)((((i > 0) ? (i - 1) : 0)) / 28);
        t->idx_old3       = (uint8_t)((((i - 1 + d3) > 0) ? (i - 1 + d3) : 0) / 28);
        t->idx_new3       = (uint8_t)((i + d3) / 28);
        t->idx_load44     = (uint8_t)((i + 44) / 28);
        t->idx_load16     = (uint8_t)((i + 16) / 28);
        t->byte_edge      = (uint8_t)(t->idx_old != t->byte_offset);
        t->an3_edge       = (uint8_t)(t->idx_old3 != t->idx_new3);
    }
    s_tick_ready = 1u;
}

static uint8_t pal_sw_lookahead_uniform(const uint8_t *sw_by_byte)
{
    const uint8_t first = sw_by_byte[0];
    uint32_t i;

    for (i = 1u; i < PAL_SW_LOOKAHEAD_BYTES; ++i) {
        if (sw_by_byte[i] != first) {
            return 0u;
        }
    }
    return 1u;
}

/* latency = left-edge samples to skip: PAL_OUTPUT_LATENCY_SAMPLES in single
 * resolution, PAL_OUTPUT_LATENCY_DOUBLE in double resolution. Both leave the
 * 560-sample visible window inside s_render_line[PAL_RENDER_WORK_SAMPLES]. */
static void pal_copy_visible_output(uint32_t *dst, uint32_t latency)
{
    if (dst != NULL) {
        memcpy(dst,
               &s_render_line[latency],
               PAL_RENDER_SAMPLES * sizeof(uint32_t));
    }
}

static void pal_prepare_render_output(const pal_line_buffer_t *line,
                                      uint32_t **dst,
                                      const uint32_t *phase_table[ATN_NUM_PHASES])
{
    const uint8_t burst_flags = pal_colour_burst_for_line(line);
    const uint8_t line_colour_burst =
        (uint8_t)((burst_flags & PAL_FLAG_COLOUR_BURST) != 0u);
    const uint8_t line_colourise =
        (uint8_t)(line_colour_burst != 0u && s_mono_enable == 0u);

    *dst = NULL;

    if (s_framebuffer != NULL) {
        *dst = &s_framebuffer[
            ((line->y + ATN_ACTIVE_Y) * ATN_SCRATCH_ROW_PIXELS) +
            ATN_SCRATCH_LEFT_BORDER_PIXELS + ATN_ACTIVE_X];
    }

    pal_init_packed_tables();
    if (line_colourise != 0u) {
        const uint32_t (*hue)[ATN_NUM_SEQUENCES] =
            (s_color_mode == APPLE_VIDEO_COLOR_PAL_ACCURATE_TV) ?
            s_pal_hue_color_tv_packed : s_pal_hue_monitor_packed;
        phase_table[0] = hue[0];
        phase_table[1] = hue[1];
        phase_table[2] = hue[2];
        phase_table[3] = hue[3];
    } else {
        const uint32_t *bw;

        if (s_mono_enable != 0u) {
            bw = s_pal_mono_monitor_packed[
                apple_video_mono_color_clamp(s_mono_color)];
        } else if (s_color_mode == APPLE_VIDEO_COLOR_PAL_ACCURATE_TV) {
            bw = s_pal_bw_color_tv_packed;
        } else {
            bw = s_pal_bw_monitor_packed;
        }
        phase_table[0] = bw;
        phase_table[1] = bw;
        phase_table[2] = bw;
        phase_table[3] = bw;
    }
}

static void pal_render_composite_line_uniform(const pal_line_buffer_t *line,
                                              uint8_t soft_switches)
{
    uint32_t *dst = NULL;
    const uint32_t *ntsc_phase_table[ATN_NUM_PHASES];
    const int yl = (int)line->y;
    const int va = pal_compute_va(yl);
    const int vb = pal_compute_vb(yl);
    const int vc = pal_compute_vc(yl);
    const uint8_t segc = pal_compute_segc(vc);
    const uint8_t ssw80col = pal_is_set(soft_switches, PAL_SW_80COL);
    const uint8_t gr_time = pal_is_graphics_flags(soft_switches, line->y);
    const uint8_t gr2p = pal_compute_gr2p_hal(soft_switches, line->y);
    const uint8_t lohi = pal_is_set(soft_switches, PAL_SW_HIRES);
    const uint8_t segb = pal_compute_segb(gr_time, vb, lohi);
    const uint8_t sega0 = pal_compute_sega(gr_time, va, 0u);
    const uint8_t sega1 = pal_compute_sega(gr_time, va, 1u);
    const uint8_t not_gr2p = (uint8_t)(gr2p ^ 1u);
    const uint8_t not_segb = (uint8_t)(segb ^ 1u);
    const uint8_t vid7m_base =
        (uint8_t)((not_gr2p & segb) | (gr2p & ssw80col));
    uint8_t vid7mp = 1u;
    uint8_t shift_register = 255u;
    uint16_t ntsc_window = 0u;
    uint32_t byte_group = 0u;
    uint32_t sample;

    pal_prepare_render_output(line,
                              &dst,
                              ntsc_phase_table);
    if (dst == NULL) {
        return;
    }

    sample = 0u;
    while (sample < PAL_RENDER_WORK_SAMPLES) {
        const uint8_t ram_valid = (uint8_t)(byte_group < PAL_BYTES_PER_LINE);
        const uint8_t ram_phase1 = (ram_valid != 0u) ?
            line->scanned_bytes[(byte_group * 2u) + 1u] : 0u;
        const uint8_t ram_phase0 = (ram_valid != 0u) ?
            line->scanned_bytes[(byte_group * 2u) + 0u] : 0u;
        const uint8_t h0_rest =
            (uint8_t)(((byte_group & 1u) == 0u) ? 1u : 0u);
        const uint8_t h0_pos0 =
            (byte_group == 0u) ? 1u :
            (uint8_t)((((byte_group - 1u) & 1u) == 0u) ? 1u : 0u);
        uint32_t pos;

        for (pos = 0u; pos < 14u && sample < PAL_RENDER_WORK_SAMPLES; ++pos, ++sample) {
            const uint8_t h0 = (pos == 0u) ? h0_pos0 : h0_rest;
            const uint8_t composite =
                (uint8_t)((shift_register ^ 1u) & 1u);
            uint8_t ram_byte = 0u;
            uint8_t ldps_s = 0u;
            uint8_t vid7m_s = vid7m_base;

            ntsc_window =
                (uint16_t)(((ntsc_window << 1) | composite) & 0x0FFFu);
            s_render_line[sample] =
                ntsc_phase_table[(PAL_NTSC_PHASE_OFFSET + sample) & 3u]
                                [ntsc_window];

            if ((pos & 1u) == 0u && gr2p != 0u) {
                vid7m_s = 1u;
            }

            if (pos == 12u) {
                const uint8_t vid7 =
                    (uint8_t)((ram_phase0 & 0x80u) != 0u);
                const uint8_t not_vid7 = (uint8_t)(vid7 ^ 1u);

                ram_byte = ram_phase0;
                if (not_vid7 != 0u) {
                    vid7m_s = 1u;
                }
                ldps_s = (uint8_t)(gr2p | segb | not_vid7);
            } else {
                if (vid7mp == 0u) {
                    vid7m_s = 1u;
                }
                if (pos == 5u) {
                    ram_byte = ram_phase1;
                    ldps_s = (uint8_t)(ssw80col & gr2p);
                } else if (pos == 13u) {
                    const uint8_t vid7 =
                        (uint8_t)((ram_phase0 & 0x80u) != 0u);

                    ram_byte = ram_phase0;
                    ldps_s = (uint8_t)(vid7 & not_segb & not_gr2p);
                }
            }

            vid7mp = vid7m_s;

            if (vid7m_s != 0u) {
                shift_register = pal_ror(shift_register);
            }
            if (ldps_s != 0u && ram_valid != 0u) {
                shift_register = pal_read_rom_byte(
                    soft_switches,
                    gr_time,
                    (h0 != 0u) ? sega1 : sega0,
                    segb,
                    segc,
                    ram_byte);
            }
        }
        byte_group++;
    }

    /* Double-res (80-col) shifts at 14 MHz -> half the left-edge latency. */
    pal_copy_visible_output(dst,
        ssw80col ? PAL_OUTPUT_LATENCY_DOUBLE : PAL_OUTPUT_LATENCY_SAMPLES);
}

static void pal_render_composite_line(const pal_line_buffer_t *line,
                                      const pal_line_buffer_t *next)
{
    uint32_t *dst = NULL;
    uint8_t sw_by_byte[PAL_SW_LOOKAHEAD_BYTES];
    uint8_t soft_switches;
    uint8_t ssw80col;
    uint8_t current_gr_time;
    uint8_t current_gr2p_hal;
    uint8_t current_lohi;
    uint8_t current_sega;
    uint8_t current_segb;
    pal_delay_t delayed_gr2p_hal;
    pal_delay_t delayed_gr_time;
    pal_delay_t delayed_lohi;
    pal_delay_t delayed_sega;
    pal_delay_t delayed_segb;
    uint8_t vid7mp = 1u;
    uint8_t shift_register = 255u;
    uint32_t sample;
    /* VA/VB/VC and SEGC are pure functions of the scanline number, which is
     * constant for this whole call. Compute them once instead of re-deriving
     * them on every tick inside the delay scheduler and the per-byte ROM load. */
    const int yl = (int)line->y;
    const int va = pal_compute_va(yl);
    const int vb = pal_compute_vb(yl);
    const int vc = pal_compute_vc(yl);
    const uint8_t segc = pal_compute_segc(vc);
    /* NTSC artifact-colour serializer state for this scanline. The composite
     * stream is shifted bit-by-bit into ntsc_window and looked up in the same
     * chroma tables the standard composite/TV modes use. PAL Accurate Composite
     * maps to the monitor hue table, PAL Accurate TV to the colour-TV table;
     * text/blanking lines (no colourburst) and mono mode fall back to the
     * matching B/W table. Phase and window reset per line (NTSC locks chroma to
     * colourburst every scanline). */
    uint16_t ntsc_window = 0u;
    uint8_t ntsc_phase = (uint8_t)PAL_NTSC_PHASE_OFFSET;
    const uint32_t *ntsc_phase_table[ATN_NUM_PHASES];

    pal_build_sw_by_byte(line, next, sw_by_byte);
    if (pal_sw_lookahead_uniform(sw_by_byte) != 0u) {
        g_pal_fast_lines++;
        pal_render_composite_line_uniform(line, sw_by_byte[0]);
        return;
    }
    g_pal_slow_lines++;

    soft_switches = sw_by_byte[0];
    ssw80col = pal_is_set(sw_by_byte[0], PAL_SW_80COL);
    /* Left-edge latency uses start-of-line resolution. Preserve it before the
     * loop mutates ssw80col for later cycles. */
    const uint8_t line_double_res = ssw80col;

    current_gr_time = pal_is_graphics_flags(soft_switches, line->y);
    current_gr2p_hal = pal_compute_gr2p_hal(soft_switches, line->y);
    current_lohi = pal_is_set(soft_switches, PAL_SW_HIRES);
    current_sega = pal_compute_sega(current_gr_time, va, 0u);
    current_segb = pal_compute_segb(current_gr_time, vb, current_lohi);

    delayed_gr2p_hal.value = current_gr2p_hal; delayed_gr2p_hal.future_time = 0;
    delayed_gr_time.value = current_gr_time;   delayed_gr_time.future_time = 0;
    delayed_lohi.value = current_lohi;         delayed_lohi.future_time = 0;
    delayed_sega.value = current_sega;         delayed_sega.future_time = 0;
    delayed_segb.value = current_segb;         delayed_segb.future_time = 0;

    pal_prepare_render_output(line,
                              &dst,
                              ntsc_phase_table);
    if (dst == NULL) {
        return;
    }
    pal_init_tick_table();

    /* Step the 28 MHz grid two ticks at a time. A composite sample is emitted
     * only on even ticks (sample = i >> 1), and every event that can change the
     * sampled output also lands on an even tick:
     *   - byte boundaries (ssw_old vs ssw_new) at i = 28*b,
     *   - the AN3 look-ahead edge at i = 28*k + 18,
     *   - LOAD_SOFT_SWITCHES at i % 28 == 12,
     *   - all scheduled delays are even (AN3 +24, TEXT +30/32/34, HIRES +2),
     *     so every delayed future_time is even,
     *   - the shift register and VID7Mp mutate only inside the even block.
     * Odd ticks therefore contribute nothing observable, so the loop walks
     * samples directly for roughly half the overhead. Any change to tick
     * stepping must re-check these parity invariants. */
    for (sample = 0u; sample < PAL_RENDER_WORK_SAMPLES; ++sample) {
        const pal_tick_t *t = &s_tick[sample];
        const int i = (int)t->time;
        const int byte_offset = (int)t->byte_offset;
        const uint8_t h0 = t->h0;
        const uint8_t ram_byte = (t->ram_valid != 0u) ?
            line->scanned_bytes[t->ram_index] : 0u;

        if (t->an3_edge != 0u) {
            const uint8_t ssw_old3 = sw_by_byte[t->idx_old3];
            const uint8_t ssw_new3 = sw_by_byte[t->idx_new3];

            if (((ssw_old3 ^ ssw_new3) & (uint8_t)(1u << PAL_SW_AN3)) != 0u) {
                delayed_gr2p_hal.value = pal_compute_gr2p_hal(ssw_new3, line->y);
                delayed_gr2p_hal.future_time = i + 24;
            }
        }

        if (t->byte_edge != 0u) {
            const uint8_t ssw_old = sw_by_byte[t->idx_old];
            const uint8_t ssw_new = sw_by_byte[t->byte_offset];
            const uint8_t sw_delta = (uint8_t)(ssw_old ^ ssw_new);

            if ((sw_delta & (uint8_t)(1u << PAL_SW_TEXT)) != 0u) {
                const uint8_t text_old = pal_is_set(ssw_old, PAL_SW_TEXT);
                const uint8_t text_new = pal_is_set(ssw_new, PAL_SW_TEXT);
                int delay_segb;
                int delay_gr_time_p;

                if (text_old == 0u && text_new != 0u) {
                    delay_segb = pal_is_set(ssw_old, PAL_SW_HIRES) ? 32 : 30;
                    delay_gr_time_p = delay_segb + 2;
                } else {
                    delay_segb = 32;
                    delay_gr_time_p = delay_segb;
                }

                delayed_gr2p_hal.value = pal_compute_gr2p_hal(ssw_new, line->y);
                delayed_gr2p_hal.future_time = i + delay_gr_time_p;
                delayed_gr_time.value = pal_is_graphics_flags(ssw_new, line->y);
                delayed_gr_time.future_time = i + delay_gr_time_p;
                delayed_sega.value = pal_compute_sega(
                    pal_is_graphics_flags(ssw_new, line->y),
                    va,
                    h0);
                delayed_sega.future_time = i + delay_segb;
                delayed_segb.value = pal_compute_segb(
                    pal_is_graphics_flags(ssw_new, line->y),
                    vb,
                    pal_is_set(ssw_new, PAL_SW_HIRES));
                delayed_segb.future_time = i + delay_segb;
            }

            if ((sw_delta & (uint8_t)(1u << PAL_SW_HIRES)) != 0u) {
                delayed_segb.value = pal_compute_segb(
                    pal_is_graphics_flags(ssw_new, line->y),
                    vb,
                    pal_is_set(ssw_new, PAL_SW_HIRES));
                delayed_segb.future_time = i + 2;
            }
        }

        if (i >= delayed_gr2p_hal.future_time) current_gr2p_hal = delayed_gr2p_hal.value;
        if (i >= delayed_gr_time.future_time)   current_gr_time = delayed_gr_time.value;
        if (i >= delayed_lohi.future_time)      current_lohi = delayed_lohi.value;
        if (i >= delayed_segb.future_time)      current_segb = delayed_segb.value;
        if (i >= delayed_sega.future_time)      current_sega = delayed_sega.value;

        (void)current_lohi;
        (void)current_sega;

        if (t->load_sw != 0u) {
            soft_switches = sw_by_byte[t->idx_load44];
            ssw80col = pal_is_set(sw_by_byte[t->idx_load16], PAL_SW_80COL);
        }

        const uint8_t gr2p = current_gr2p_hal;
        const uint8_t segb = current_segb;
        const uint8_t vid7 = (uint8_t)((ram_byte & 0x80u) != 0u);
        const uint8_t not_vid7 = (uint8_t)(vid7 ^ 1u);
        const uint8_t not_gr2p = (uint8_t)(gr2p ^ 1u);
        const uint8_t not_segb = (uint8_t)(segb ^ 1u);
        const uint8_t not_h0 = (uint8_t)(h0 ^ 1u);
        const uint8_t sig_7m = t->sig_7m;
        const uint8_t not_sig_7m = (uint8_t)(sig_7m ^ 1u);
        const uint8_t clr_ref = t->clr_ref;
        const uint8_t ldps_check = t->ldps_check;
        const uint8_t not_ldps_check = (uint8_t)(ldps_check ^ 1u);
        const uint8_t ldps_check_phase_0_and_1 = t->ldps_check_p01;
        const uint8_t ldps_hgr_check = t->ldps_hgr_check;
        const uint8_t ldps_s1 = (uint8_t)(ldps_check_phase_0_and_1 & ssw80col & gr2p);
        const uint8_t ldps_s2 = (uint8_t)(ldps_check & gr2p);
        const uint8_t ldps_s3 = (uint8_t)(ldps_check & segb);
        const uint8_t ldps_s4 = (uint8_t)(ldps_check & not_vid7);
        const uint8_t ldps_s5 = (uint8_t)(ldps_check & clr_ref & not_h0);
        const uint8_t ldps_s6 = (uint8_t)(ldps_hgr_check & vid7 & not_segb & not_gr2p);
        const uint8_t vid7m_s1 = (uint8_t)(not_gr2p & segb);
        const uint8_t vid7m_s2 = (uint8_t)(gr2p & ssw80col);
        const uint8_t vid7m_s3 = (uint8_t)(gr2p & not_sig_7m);
        const uint8_t vid7m_s4 = (uint8_t)(ldps_check & not_vid7);
        const uint8_t vid7m_s5 = (uint8_t)(ldps_check & clr_ref & not_h0);
        const uint8_t vid7m_t123 = (uint8_t)(not_ldps_check & (uint8_t)(vid7mp ^ 1u));
        const uint8_t lss_out = (uint8_t)(shift_register & 1u);
        const uint8_t vid7m_s =
            (uint8_t)(vid7m_s1 | vid7m_s2 | vid7m_s3 |
                      vid7m_s4 | vid7m_s5 | vid7m_t123);
        const uint8_t ldps_s =
            (uint8_t)(ldps_s1 | ldps_s2 | ldps_s3 |
                      ldps_s4 | ldps_s5 | ldps_s6);
        const uint8_t vid7m = (uint8_t)(vid7m_s ^ 1u);
        const uint8_t composite = (uint8_t)(1u - lss_out);

        vid7mp = vid7m_s;

        /* Shift the composite dot into the NTSC signal window and colourise
         * via the shared AppleWin chroma tables, exactly as the standard
         * composite path does (atn_color_table_lookup). */
        ntsc_window =
            (uint16_t)(((ntsc_window << 1) | composite) & 0x0FFFu);
        s_render_line[sample] =
            ntsc_phase_table[ntsc_phase & 3u][ntsc_window];
        ntsc_phase = (uint8_t)((ntsc_phase + 1u) & 3u);

        if (vid7m == 0u) {
            shift_register = pal_ror(shift_register);
        }
        if (ldps_s != 0u && byte_offset < (int)PAL_BYTES_PER_LINE) {
            shift_register = pal_read_rom_byte(
                soft_switches,
                current_gr_time,
                pal_compute_sega(current_gr_time, va, h0),
                segb,
                segc,
                ram_byte);
        }
    }

    /* Double-res (80-col) shifts at 14 MHz -> half the left-edge latency. */
    pal_copy_visible_output(dst,
        line_double_res ? PAL_OUTPUT_LATENCY_DOUBLE : PAL_OUTPUT_LATENCY_SAMPLES);
}

static uint32_t pal_pack_bgra(uint8_t r, uint8_t g, uint8_t b)
{
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static uint32_t pal_pack_atn_bgra(atn_bgra_t c)
{
    return ((uint32_t)c.b) |
           ((uint32_t)c.g << 8) |
           ((uint32_t)c.r << 16) |
           ((uint32_t)c.a << 24);
}

static uint32_t pal_apply_mono_color(uint8_t mono_color,
                                     uint8_t r,
                                     uint8_t g,
                                     uint8_t b)
{
    uint8_t y = (uint8_t)(((uint32_t)r * 30u + (uint32_t)g * 59u +
                           (uint32_t)b * 11u) / 100u);

    switch (apple_video_mono_color_clamp(mono_color)) {
    case APPLE_VIDEO_MONO_GREEN:
        return pal_pack_bgra((uint8_t)(y / 16u),
                             (uint8_t)((uint32_t)y * 72u / 100u),
                             (uint8_t)((uint32_t)y * 32u / 100u));
    case APPLE_VIDEO_MONO_AMBER:
        return pal_pack_bgra(y, (uint8_t)((uint32_t)y * 70u / 100u), (uint8_t)(y / 8u));
    case APPLE_VIDEO_MONO_BLACK:
        return pal_pack_bgra(0u, 0u, 0u);
    case APPLE_VIDEO_MONO_WHITE:
    default:
        return pal_pack_bgra(y, y, y);
    }
}

static void pal_init_packed_tables(void)
{
    uint32_t phase;
    uint32_t seq;
    uint32_t mono;

    if (s_pal_packed_tables_ready != 0u) {
        return;
    }

    for (phase = 0u; phase < ATN_NUM_PHASES; ++phase) {
        for (seq = 0u; seq < ATN_NUM_SEQUENCES; ++seq) {
            s_pal_hue_monitor_packed[phase][seq] =
                pal_pack_atn_bgra(g_aHueMonitor[phase][seq]);
            s_pal_hue_color_tv_packed[phase][seq] =
                pal_pack_atn_bgra(g_aHueColorTV[phase][seq]);
        }
    }

    for (seq = 0u; seq < ATN_NUM_SEQUENCES; ++seq) {
        const atn_bgra_t monitor = g_aBnWMonitor[seq];
        const atn_bgra_t color_tv = g_aBnwColorTV[seq];

        s_pal_bw_monitor_packed[seq] = pal_pack_atn_bgra(monitor);
        s_pal_bw_color_tv_packed[seq] = pal_pack_atn_bgra(color_tv);
        for (mono = 0u; mono < PAL_MONO_COLOR_COUNT; ++mono) {
            s_pal_mono_monitor_packed[mono][seq] =
                pal_apply_mono_color((uint8_t)mono,
                                     monitor.r,
                                     monitor.g,
                                     monitor.b);
        }
    }

    s_pal_packed_tables_ready = 1u;
}

static void pal_render_line(const pal_line_buffer_t *line,
                            const pal_line_buffer_t *next)
{
#if PAL_RENDER_TIMING_PROFILE
    uint32_t start_ticks;
#endif

    if (line == NULL || line->valid == 0u || line->y >= PAL_VISIBLE_LINES) {
        return;
    }
#if PAL_RENDER_TIMING_PROFILE
    start_ticks = pal_profile_timer_read32();
#endif
    pal_render_composite_line(line, next);
#if PAL_RENDER_TIMING_PROFILE
    pal_profile_record_render_ticks(start_ticks);
#endif
}

static void pal_init_line(pal_line_buffer_t *line, uint32_t y, uint32_t sw)
{
    uint32_t i;
    const uint8_t pal_flags = pal_flags_from_ace(sw, y);

    memset(line, 0, sizeof(*line));
    line->valid = 1u;
    line->y = y;
    line->fallback_ace_sw = sw;
    line->fallback_pal_sw = pal_flags;
    for (i = 0u; i < PAL_LINE_CYCLES; ++i) {
        line->ace_sw[i] = sw;
        line->pal_sw[i] = pal_flags;
    }
}

static void pal_mark_cycle_seen(pal_line_buffer_t *line, uint32_t cycle)
{
    if (cycle < 64u) {
        line->cycle_mask_lo |= (1ULL << cycle);
    } else if (cycle == 64u) {
        line->cycle64_seen = 1u;
    }
}

static uint8_t pal_line_complete(const pal_line_buffer_t *line)
{
    return (uint8_t)(line != NULL &&
                     line->valid != 0u &&
                     line->y < PAL_VISIBLE_LINES &&
                     line->cycle_mask_lo == 0xFFFFFFFFFFFFFFFFULL &&
                     line->cycle64_seen != 0u &&
                     line->ram_sample_mask == PAL_RAM_SAMPLE_MASK);
}

static void pal_capture_line_cycle(pal_line_buffer_t *line,
                                   uint32_t y,
                                   uint32_t cycle,
                                   uint32_t softswitch_bits)
{
    const uint8_t pal_flags = pal_flags_from_ace(softswitch_bits, y);

    if (line == NULL || cycle >= PAL_LINE_CYCLES) {
        return;
    }
    if (line->valid == 0u || line->y != y) {
        pal_init_line(line, y, softswitch_bits);
    }

    line->ace_sw[cycle] = softswitch_bits;
    line->pal_sw[cycle] = pal_flags;
    line->fallback_ace_sw = softswitch_bits;
    line->fallback_pal_sw = pal_flags;
    pal_mark_cycle_seen(line, cycle);
    if (cycle > line->max_cycle_seen) {
        line->max_cycle_seen = (uint8_t)cycle;
    }
    pal_sample_scanned_ram_for_cycle(line, cycle);
}

static void pal_clear_pipeline(void)
{
    memset(&s_current_line, 0, sizeof(s_current_line));
    memset(&s_preroll_line, 0, sizeof(s_preroll_line));
    s_ring_head = 0u;
    s_ring_tail = 0u;
    s_ring_count = 0u;
    s_frame_failed = 0u;
    s_capture_enabled = 0u;
    s_capture_frame_open = 0u;
    s_render_frame_open = 0u;
    s_frame_ready_pending = 0u;
    s_frame_lines_queued = 0u;
    s_frame_lines_rendered = 0u;
}

static void pal_drop_current_frame(void)
{
    pal_clear_pipeline();
    g_pal_frames_dropped++;
}

static void pal_finish_frame_if_complete(void)
{
    if (s_render_frame_open == 0u ||
        s_capture_frame_open != 0u ||
        s_ring_count != 0u ||
        s_current_line.valid != 0u ||
        s_frame_ready_pending != 0u) {
        return;
    }

    if (s_frame_failed != 0u ||
        s_frame_lines_queued != PAL_VISIBLE_LINES ||
        s_frame_lines_rendered != PAL_VISIBLE_LINES) {
        if (s_frame_failed == 0u) {
            g_pal_lines_incomplete++;
        }
        pal_drop_current_frame();
        return;
    }

    s_render_frame_open = 0u;
    s_frame_ready_pending = 1u;
}

/* Decoupled render queue. apple_pal_video_on_cycle() (egress drain path) only
 * buffers cycles and hands completed lines to this queue with a cheap copy; the
 * heavy per-line render runs off the drain in apple_pal_video_pump() (CPU1 main
 * loop). A PAL frame may finish rendering after its capture frame has ended;
 * until then later Apple frames are skipped whole. That keeps every published
 * PAL frame exact without stale-line caches or capture-FIFO stalls.
 */
static void pal_ring_push(const pal_line_buffer_t *line)
{
    if (s_ring_count >= PAL_LINE_RING) {
        g_pal_queue_overflows++;
        pal_drop_current_frame();
        return;
    }
    s_line_ring[s_ring_head] = *line;
    s_ring_head = (s_ring_head + 1u) % PAL_LINE_RING;
    s_ring_count++;
    s_frame_lines_queued++;
    if (s_ring_count > g_pal_queue_max) {
        g_pal_queue_max = s_ring_count;
    }
}

/* Render up to max_lines queued lines. Each is rendered with the next queued
 * line as its one-line soft-switch look-ahead, so the newest queued line is
 * held back until its successor arrives. Once capture for this frame is closed,
 * the final line is rendered with its captured fallback soft-switch state. */
static uint32_t pal_ring_drain(uint32_t max_lines)
{
    const uint32_t hold_last = (s_capture_frame_open != 0u) ? 1u : 0u;
    uint32_t drained = 0u;

    while (s_frame_ready_pending == 0u &&
           max_lines > 0u &&
           s_ring_count > hold_last) {
        const pal_line_buffer_t *next =
            (s_ring_count >= 2u) ?
            &s_line_ring[(s_ring_tail + 1u) % PAL_LINE_RING] : NULL;

        if (s_frame_failed == 0u) {
            if (pal_line_complete(&s_line_ring[s_ring_tail]) == 0u) {
                s_frame_failed = 1u;
                g_pal_lines_incomplete++;
                pal_drop_current_frame();
                return drained;
            } else {
                pal_render_line(&s_line_ring[s_ring_tail], next);
                g_pal_lines_rendered++;
                s_frame_lines_rendered++;
            }
        }
        s_ring_tail = (s_ring_tail + 1u) % PAL_LINE_RING;
        s_ring_count--;
        max_lines--;
        drained++;
    }

    pal_finish_frame_if_complete();
    return drained;
}

void apple_pal_video_reset(void)
{
    pal_clear_pipeline();
    s_last_sw = (1u << ACE_SWB_TEXT_BIT);
    s_have_last_sw = 1u;
}

void apple_pal_video_resync(void)
{
    const uint8_t had_frame_state =
        (uint8_t)((s_capture_enabled != 0u ||
                   s_capture_frame_open != 0u ||
                   s_render_frame_open != 0u ||
                   s_frame_ready_pending != 0u ||
                   s_ring_count != 0u ||
                   s_current_line.valid != 0u ||
                   s_preroll_line.valid != 0u) ? 1u : 0u);

    pal_clear_pipeline();
    s_have_last_sw = 0u;
    if (apple_pal_video_mode_is_active(s_color_mode) != 0u &&
        had_frame_state != 0u) {
        g_pal_frames_dropped++;
    }
}

void apple_pal_video_begin_frame(void)
{
    if (apple_pal_video_mode_is_active(s_color_mode) == 0u) {
        s_capture_enabled = 0u;
        s_capture_frame_open = 0u;
        memset(&s_preroll_line, 0, sizeof(s_preroll_line));
        return;
    }

    /* Accept a new PAL capture only when the previous accepted frame has been
     * fully rendered and published. If CPU1 is still rendering or waiting for
     * the handoff publish, skip this Apple frame whole; every eventual PAL
     * publish is therefore a complete exact frame, never a stale-line mix. */
    if (s_frame_ready_pending != 0u ||
        s_render_frame_open != 0u ||
        s_ring_count != 0u ||
        s_current_line.valid != 0u) {
        s_capture_enabled = 0u;
        s_capture_frame_open = 0u;
        memset(&s_preroll_line, 0, sizeof(s_preroll_line));
        g_pal_frames_dropped++;
        return;
    }

    memset(&s_current_line, 0, sizeof(s_current_line));
    if (s_preroll_line.valid != 0u) {
        s_current_line = s_preroll_line;
        memset(&s_preroll_line, 0, sizeof(s_preroll_line));
    }
    s_ring_head = 0u;
    s_ring_tail = 0u;
    s_ring_count = 0u;
    s_frame_failed = 0u;
    s_capture_enabled = 1u;
    s_capture_frame_open = 1u;
    s_render_frame_open = 1u;
    s_frame_ready_pending = 0u;
    s_frame_lines_queued = 0u;
    s_frame_lines_rendered = 0u;
}

void apple_pal_video_preroll_line0_cycle(uint32_t cycle, uint32_t softswitch_bits)
{
    if (apple_pal_video_mode_is_active(s_color_mode) == 0u ||
        cycle >= PAL_LINE_CYCLES) {
        return;
    }
    if (cycle == 0u) {
        memset(&s_preroll_line, 0, sizeof(s_preroll_line));
    }
    pal_capture_line_cycle(&s_preroll_line, 0u, cycle, softswitch_bits);
    s_last_sw = softswitch_bits;
    s_have_last_sw = 1u;
}

uint8_t apple_pal_video_end_frame(void)
{
    /* Close any accepted capture, render a small bounded amount of remaining
     * work, and report whether a previously accepted PAL frame is now complete
     * and ready for the normal framebuffer handoff. */
    if (apple_pal_video_mode_is_active(s_color_mode) == 0u) {
        return 1u;
    }
    if (s_capture_enabled != 0u && s_current_line.valid != 0u) {
        pal_ring_push(&s_current_line);
        memset(&s_current_line, 0, sizeof(s_current_line));
    }
    if (s_capture_enabled != 0u) {
        s_capture_enabled = 0u;
        s_capture_frame_open = 0u;
    }

    g_pal_end_frame_count++;
    g_pal_end_queue_total += s_ring_count;
    if (s_ring_count > g_pal_end_queue_max) {
        g_pal_end_queue_max = s_ring_count;
    }
    g_pal_end_lines_drained += pal_ring_drain(PAL_FLUSH_MAX_LINES);
    if (s_frame_ready_pending != 0u) {
        s_frame_ready_pending = 0u;
        g_pal_frames_published++;
        return 1u;
    }
    return 0u;
}

/* Render queued lines off the drain path. Called once per CPU1 main-loop
 * iteration after apple_cycle_egress_poll(). A no-op when the queue is empty
 * (any non-PAL mode), so it is safe to call unconditionally. */
void apple_pal_video_pump(void)
{
    if (apple_pal_video_mode_is_active(s_color_mode) == 0u) {
        return;
    }
    (void)pal_ring_drain(PAL_PUMP_MAX_LINES);
}

void apple_pal_video_on_cycle(uint32_t line, uint32_t cycle, uint32_t softswitch_bits)
{
    if (line >= PAL_VISIBLE_LINES || cycle >= PAL_LINE_CYCLES) {
        return;
    }

    if (s_have_last_sw == 0u) {
        s_last_sw = softswitch_bits;
        s_have_last_sw = 1u;
    }
    if (s_capture_enabled == 0u || s_frame_ready_pending != 0u) {
        s_last_sw = softswitch_bits;
        return;
    }

    if (s_current_line.valid == 0u) {
        pal_init_line(&s_current_line, line, softswitch_bits);
    } else if (s_current_line.y != line) {
        /* Line complete: hand it to the render queue (cheap copy) and start the
         * next one. The render itself happens off the drain path in
         * apple_pal_video_pump() / end_frame(), so this never stalls the egress
         * drain. */
        pal_ring_push(&s_current_line);
        if (s_capture_enabled == 0u) {
            s_last_sw = softswitch_bits;
            return;
        }
        pal_init_line(&s_current_line, line, softswitch_bits);
    }

    pal_capture_line_cycle(&s_current_line, line, cycle, softswitch_bits);
    s_last_sw = softswitch_bits;
}
