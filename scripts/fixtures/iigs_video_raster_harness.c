#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#include "apple_cycle_egress.h"
#include "apple_fb_handoff.h"
#include "compositor_layout.h"
#include "video_output.h"

#define ATN_SCANNER_MAX_HORZ 65u
#define ATN_SCANNER_HORZ_START 25u
#define GS_LINE_FETCH_CYCLE ATN_SCANNER_HORZ_START
#define GS_GAP_QUEUE_BOUND 4160u
#define ATN_SCANNER_Y_DISPLAY 192u
#define ATN_SCANNER_MAX_VERT_PAL 312u
#define ATN_SCANNER_MAX_VERT_NTSC 262u

static uint8_t s_vidhd_newvideo, s_gs_active, s_gs_newvideo, s_gs_screen_color;
static uint8_t s_gs_langsel, s_gs_wait_state, s_vidhd_border_color;
static uint16_t s_gs_recovery_records;
static uint32_t g_acr_gs_capture_gaps;
static uint8_t s_border_default_color = 6;
static uint8_t s_shr_cache_invalidate, s_gs_previous_shr_cell;
static uint8_t s_gs_raster_armed, s_gs_raster_time_valid, s_gs_raster_finish;
static uint8_t s_gs_raster_start, s_gs_flash_frames, s_gs_line_scb, s_gs_phase_locked;
static uint32_t s_gs_line_palette[16];
static uint32_t s_current_sw, s_scanner_frame_lines, s_gs_raster_last_time;
static uint32_t s_frame_format_detail, s_frame_display_mode;
static uint8_t s_shr_badge_geometry_mask, s_shr_badge_family_mask, s_cached_writer_slot;
static int g_nVideoClockVert, g_nVideoClockHorz, g_nVideoCharSet;
static uint16_t g_aPixelDoubleMaskHGR[128], g_nTextFlashMask;
static uint32_t canvas[COMP_APPLE_GS_WIDTH * COMP_APPLE_GS_HEIGHT];
static uint32_t *g_atn_framebuffer = canvas, *g_pVideoAddress;
static volatile uint8_t aux_memory[65536], main_memory[65536];
volatile uint8_t *const g_aux_bank = aux_memory, *const g_main_bank = main_memory;
static const volatile uint8_t *s_shr_color_bank;
static volatile uint32_t g_acr_cycles_rendered;
volatile uint32_t g_gap_markers_seen, g_resync_pending, g_bus_writes_seen, g_frame_records_seen;
volatile uint32_t g_video_shadow_generation, g_legacy_video_shadow_generation;
const uint32_t comp_apple_slot_addr[COMP_APPLE_SLOT_COUNT] = {0};
static unsigned publishes;
static void gs_raster_state(uint64_t rec);
static void apply_video_settings_if_changed(void) {}
static void shr_mode_switch_resync(void) {}
static void apply_video_rom_if_changed(void) {}
uint8_t apple_fb_writer_slot(void) { return 0; }
static void appletini_ntsc_set_framebuffer(uint32_t *unused)
{
    (void)unused;
    g_atn_framebuffer = canvas;
}
static void shr_badge_begin(void)
{
    s_shr_badge_geometry_mask = 0;
    s_shr_badge_family_mask = 0;
}
static uint32_t shr_badge_detail(uint8_t page)
{
    return page + s_shr_badge_geometry_mask;
}
static void publish_current_frame(void) { ++publishes; }
static int sw_altcharset(uint32_t sw) { return !!(sw & 4u); }
static int sw_dhires(uint32_t sw) { return !!(sw & 1u); }
static int sw_80col(uint32_t sw) { return !!(sw & 2u); }
enum { MODE_TEXT40, MODE_TEXT80, MODE_HGR, MODE_DHGR, MODE_LORES, MODE_DLORES };
static int pick_mode(uint32_t sw, int line) { (void)sw; (void)line; return MODE_TEXT40; }
static uint16_t scanner_addr_hgr(int line, int cycle, uint32_t sw)
{ (void)line; (void)cycle; (void)sw; return 0x2000; }
static void step_text40(uint32_t sw)
{
    (void)sw;
    for (unsigned i = 0; i < 14; ++i)
        *g_pVideoAddress++ = apple_video_iigs_border_bgra(s_gs_screen_color >> 4);
}
static void step_text80(uint32_t sw) { step_text40(sw); }
static void step_hgr_rgb(uint32_t sw) { step_text40(sw); }
static void step_dhgr_rgb(uint32_t sw) { step_text40(sw); }
static void step_lores_crisp(uint32_t sw) { step_text40(sw); }
static void step_dlores_crisp(uint32_t sw) { step_text40(sw); }
static void linear_text_overlay_capture_on_gap(void) {}
static unsigned overlay_writes;
static void linear_text_overlay_capture_on_write(uint32_t a, uint8_t d)
{ (void)a; (void)d; ++overlay_writes; }
static void linear_text_overlay_capture_on_command(uint8_t d) { (void)d; }
static uint8_t post_wide;
static void shr_update_post_wide(uint8_t want) { post_wide = want; }
/* RENDERER */

static void apply_record(uint64_t rec, uint64_t next1, uint64_t next2)
{
    /* EGRESS */
}
static uint64_t cycle_record(unsigned line, unsigned cycle)
{
    return (1ULL << ACE_BIT_FRAME_EN) |
           ((uint64_t)line << ACE_BIT_LINE_IN_FRAME_LO) |
           ((uint64_t)cycle << ACE_BIT_CYCLE_IN_LINE_LO);
}
static uint64_t io_record(unsigned line, unsigned cycle, uint16_t addr, uint8_t data)
{
    return ((uint64_t)ACE_RECORD_KIND_GS_IO << ACE_BIT_RECORD_KIND_LO) |
           ((uint64_t)line << ACE_BIT_IO_LINE_LO) |
           ((uint64_t)cycle << ACE_BIT_IO_CYCLE_LO) |
           ((uint64_t)addr << ACE_BIT_IO_ADDR_LO) |
           ((uint64_t)data << ACE_BIT_IO_DATA_LO);
}
static void feed_record(uint64_t record) { apply_record(record, 0, 0); }
static void write_aux(uint64_t tick, uint16_t addr, uint8_t data)
{
    feed_record(tick | ((uint64_t)(0x10000u | addr) << ACE_BIT_ADDR_DECODE_LO) |
         (1ULL << ACE_BIT_ADDR_DECODE_EN) | data);
}
void apple_cycle_renderer_on_record_lookahead(uint64_t rec, uint64_t next1, uint64_t next2)
{
    (void)next1;
    (void)next2;
    if (!rec) {
        gs_raster_gap();
    } else {
        (void)dispatch_gs_record(rec);
    }
}

static double host_seconds(void)
{
#ifdef _WIN32
    LARGE_INTEGER value, frequency;
    assert(QueryPerformanceFrequency(&frequency));
    assert(QueryPerformanceCounter(&value));
    return (double)value.QuadPart / (double)frequency.QuadPart;
#else
    return (double)clock() / CLOCKS_PER_SEC;
#endif
}

static void benchmark(uint64_t snapshot, unsigned write_every_tick)
{
    enum { FRAMES = 120, LINES = 262, CYCLES = 65 };
    const unsigned records = FRAMES * (1 + LINES * CYCLES * (1 + write_every_tick)) + 2u;
    const unsigned initial_publishes = publishes;
    const unsigned initial_writes = g_bus_writes_seen;

    feed_record(0);
    feed_record(snapshot);
    for (unsigned i = 0; i < GS_GAP_QUEUE_BOUND; ++i)
        feed_record(cycle_record(i / CYCLES, i % CYCLES));
    const double before = host_seconds();
    for (unsigned frame = 0; frame < FRAMES; ++frame) {
        feed_record(snapshot);
        for (unsigned line = 0; line < LINES; ++line) {
            for (unsigned c = 0; c < CYCLES; ++c) {
                const uint64_t tick = cycle_record(line, c);
                feed_record(tick);
                if (write_every_tick)
                    write_aux(tick, (uint16_t)(0x2000u + (line * CYCLES + c) % 32000u),
                              (uint8_t)(frame + c));
            }
        }
    }
    feed_record(snapshot);
    feed_record(cycle_record(0, 1));
    const double seconds = host_seconds() - before;
    assert(seconds > 0 && publishes - initial_publishes == FRAMES);
    assert(g_bus_writes_seen - initial_writes == FRAMES * LINES * CYCLES * write_every_tick);
    printf("Host -O2 %s: %u records in %.3f s, %.2f M records/s, %.1f selected frames/s\n",
           write_every_tick ? "ticks + separate write each tick" : "ticks only",
           records, seconds, records / seconds / 1000000.0, FRAMES / seconds);
}

int main(int argc, char **argv)
{
    const uint64_t snapshot =
        ((uint64_t)ACE_RECORD_KIND_GS_STATE << ACE_BIT_RECORD_KIND_LO) |
        (1ULL << ACE_BIT_FRAME_EN) | (0x6AULL << ACE_BIT_GS_C022_LO) |
        (0xA0ULL << ACE_BIT_GS_C029_LO) | (7ULL << ACE_BIT_GS_BORDER_LO) |
        (1ULL << ACE_BIT_GS_PHASE_LOCKED) | (1ULL << ACE_BIT_GS_FRAME_SELECTED);
    const uint32_t border = COMP_APPLE_GS_ACTIVE_Y * COMP_APPLE_GS_WIDTH;
    const uint32_t first = border + COMP_APPLE_GS_ACTIVE_X;

    const uint64_t pre_id = (uint64_t)ACE_RECORD_KIND_GS_SHADOW << ACE_BIT_RECORD_KIND_LO;
    for (unsigned i = 0; i < 5; ++i) {
        static const uint8_t signature[] = {0xC1, 0xB2, 0xCC, 0xE9, 0xFF};
        feed_record(pre_id | ((uint64_t)(0x0878u + i) << ACE_BIT_ADDR_DECODE_LO) |
                    (1ULL << ACE_BIT_ADDR_DECODE_EN) | signature[i]);
    }
    write_aux(pre_id, 0x9DF8, 1);
    write_aux(pre_id, 0x2000, 0x11);
    assert(!s_gs_active && !overlay_writes && !post_wide);
    assert(g_main_bank[0x087C] == 0xFF && g_aux_bank[0x9DF8] == 1 && g_aux_bank[0x2000] == 0x11);
    assert(dispatch_gs_record(pre_id) && !apple_cycle_renderer_needs_phase_lookahead(pre_id));

    feed_record(snapshot & ~(1ULL << ACE_BIT_GS_PHASE_LOCKED));
    feed_record(cycle_record(0, 0));
    feed_record(cycle_record(0, 1));
    assert(!s_gs_raster_armed && publishes == 0);
    assert(!apple_cycle_renderer_needs_phase_lookahead(cycle_record(0, 2)));

    for (unsigned i = 0; i < sizeof(canvas) / sizeof(canvas[0]); ++i)
        canvas[i] = 0xDEADBEEF;
    write_aux(0, 0x9DF8, 1);
    write_aux(0, 0x9DFC, 0xD3);
    write_aux(0, 0x9DFD, 0xC8);
    write_aux(0, 0x9DFE, 0xD2);
    write_aux(0, 0x9DFF, 0xB4);
    assert(!post_wide);

    /* Prepare AUX while SHR is still off. */
    write_aux(0, 0x2004, 0x11);
    write_aux(0, 0x2008, 0x22);
    write_aux(0, 0x9E02, 0x12);
    write_aux(0, 0x9E04, 0x34);
    feed_record(snapshot);
    for (unsigned c = 0; c < 25; ++c) {
        feed_record(cycle_record(0, c));
        if (c == 23) feed_record(io_record(0, 23, 0xC034, 9));
    }
    assert(canvas[border] == apple_video_iigs_border_bgra(7));
    assert(canvas[border + 16] == apple_video_iigs_border_bgra(9));

    /* The same-timestamp palette and pixel writes follow the rendered cell. */
    s_shr_color_bank = g_aux_bank;
    assert(shr_read_palette_color(0x9E00, 1) == 0xFF001122u);
    const uint32_t old = shr_read_palette_color(0x9E00, 1);
    write_aux(cycle_record(0, 25), 0x9E02, 0x56);
    assert(canvas[first] == old && g_aux_bank[0x9E02] == 0x56);
    const uint32_t updated = shr_read_palette_color(0x9E00, 1);
    assert(updated != old);
    write_aux(cycle_record(0, 26), 0x2004, 0x22);
    assert(canvas[first + 16] == old);
    feed_record(cycle_record(0, 27));
    feed_record(io_record(0, 27, 0xC029, 0));
    feed_record(cycle_record(0, 28));
    assert(canvas[first + 32] == shr_read_palette_color(0x9E00, 2));
    assert(canvas[first + 48] == apple_video_iigs_border_bgra(6));

    feed_record(io_record(0, 28, 0xC029, 0x80));
    write_aux(cycle_record(0, 29), 0x9D01, 0x80);
    write_aux(cycle_record(0, 30), 0x9E10, 0x78);
    for (unsigned c = 31; c < 65; ++c) feed_record(cycle_record(0, c));
    for (unsigned c = 0; c <= 25; ++c) feed_record(cycle_record(1, c));
    assert(canvas[first + COMP_APPLE_GS_WIDTH] == shr_read_palette_color(0x9E00, 8));
    assert(s_gs_line_palette[1] == updated);
    for (unsigned c = 26; c < 65; ++c) feed_record(cycle_record(1, c));
    for (unsigned line = 2; line < 262; ++line)
        for (unsigned c = 0; c < 65; ++c) feed_record(cycle_record(line, c));
    assert(publishes == 0);
    feed_record(snapshot);
    feed_record(cycle_record(0, 0));
    assert(publishes == 0);
    feed_record(cycle_record(0, 1));
    assert(publishes == 1 && s_frame_display_mode == APPLE_FB_DISPLAY_MODE_GS);
    for (unsigned i = 0; i < sizeof(canvas) / sizeof(canvas[0]); ++i)
        assert(canvas[i] != 0xDEADBEEF);
    assert(g_main_bank[0x087C] == 0xFF);

    feed_record(0);
    assert(!s_gs_raster_armed && s_gs_wait_state);
    feed_record(io_record(0, 2, 0xC029, 0));
    assert(vidhd_shr_enabled());
    /* An older queued zero, an intervening reset snapshot and then a
     * partial old frame cannot arm or publish while recovering. */
    feed_record(snapshot);
    feed_record(snapshot);
    feed_record(snapshot | (1ULL << ACE_BIT_GS_RESET));
    feed_record(cycle_record(0, 0));
    feed_record(cycle_record(0, 1));
    assert(!s_gs_raster_armed && s_gs_wait_state && publishes == 1);
    for (unsigned i = 2; i < GS_GAP_QUEUE_BOUND + 2; ++i)
        feed_record(cycle_record(i / 65, i % 65));
    feed_record(snapshot & ~(1ULL << ACE_BIT_GS_PHASE_LOCKED));
    feed_record(snapshot);
    feed_record(cycle_record(0, 0));
    feed_record(cycle_record(0, 1));
    assert(!s_gs_raster_armed && s_gs_wait_state && publishes == 1);
    for (unsigned i = 2; i < 262u * 65u; ++i)
        feed_record(cycle_record(i / 65, i % 65));
    feed_record(snapshot);
    feed_record(cycle_record(0, 1));
    assert(s_gs_raster_armed && !s_gs_wait_state && publishes == 1);

    /* A cadence change at the boundary can publish the completed old frame.
     * A rephase that truncates a PAL frame beyond line 262 cannot. */
    const uint64_t pal_snapshot = snapshot | (0x10ULL << ACE_BIT_GS_C02B_LO);
    for (unsigned line = 0; line < 262; ++line)
        for (unsigned c = (line == 0 ? 2 : 0); c < 65; ++c) feed_record(cycle_record(line, c));
    feed_record(io_record(261, 64, 0xC02B, 0x10));
    feed_record(pal_snapshot);
    feed_record(cycle_record(0, 1));
    assert(publishes == 2 && s_scanner_frame_lines == 312 && s_gs_raster_armed);

    for (unsigned line = 0; line <= 280; ++line)
        for (unsigned c = (line == 0 ? 2 : 0); c < 65; ++c) feed_record(cycle_record(line, c));
    feed_record(io_record(280, 64, 0xC02B, 0));
    feed_record(snapshot);
    feed_record(cycle_record(0, 1));
    assert(publishes == 2 && s_scanner_frame_lines == 262 && s_gs_raster_armed);
    const uint64_t legacy_snapshot = snapshot & ~(0x80ULL << ACE_BIT_GS_C029_LO);
    feed_record(legacy_snapshot & ~(1ULL << ACE_BIT_GS_PHASE_LOCKED));
    feed_record(legacy_snapshot);
    for (unsigned line = 0; line < 262; ++line)
        for (unsigned c = 0; c < 65; ++c) feed_record(cycle_record(line, c));
    feed_record(legacy_snapshot);
    feed_record(cycle_record(0, 1));
    assert(publishes == 3 && s_frame_format_detail == APPLE_FB_FORMAT_UNKNOWN);
    puts("Native before-write raster replay, border, SCB, mode changes and gap recovery");
    if (argc == 2 && strcmp(argv[1], "--benchmark") == 0) {
        printf("Host compiler %s\n", __VERSION__);
        benchmark(snapshot, 0);
        benchmark(snapshot, 1);
    }
    return 0;
}
