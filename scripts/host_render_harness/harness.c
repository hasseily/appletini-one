/*
 * harness.c -- host-side regression harness for the CPU1 render stack.
 *
 * Compiles the REAL apple_cycle_renderer.c + appletini_ntsc.c +
 * appletini_csbits.c for x86 and drives them with synthetic capture
 * records, mimicking apple_cycle_egress.c's shadow-write + dispatch
 * order exactly. Four scenarios, mirroring hardware reports:
 *
 *   T1  dragons.shr4i (interlaced R4G4B4): full-frame decode dumped
 *       for pixel-exact comparison against scripts/render_shr4.py.
 *   T2  legacy A2Li weave: A2IMGVIEW-style flow (text menu -> HGR
 *       pair with the in-band signature) must publish LEGACY_I; then
 *       disarm paths (mode byte zero, drop to text).
 *   T3  A2IMGVIEW no-gap SHR transitions: beach.3200 -> eye320a
 *       (RGGB) -> fluidart (PAL256 interlace), loads replayed the way
 *       load_shr streams them while SHR stays on. After each load the
 *       published frame and the shadow banks are dumped; the checker
 *       re-decodes the banks and compares.
 *   T4  Video-7 MIX (COL140M): state 10 and the UI gate must select
 *       monochrome and color across 8, 8, 8, and 4 dots per 28 dots.
 *
 * Usage: harness.exe <repo_root> <out_dir>
 * Then:  python scripts/host_render_harness/check_output.py <out_dir>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "apple_cycle_egress.h"
#include "apple_cycle_renderer.h"
#include "apple_fb_handoff.h"
#include "appletini_ntsc.h"
#include "compositor_layout.h"
#include "video_output.h"

/* ---------- fake hardware ---------- */

unsigned char g_fake_card_regs[8192];

static uint8_t s_main_mem[0x10000];
static uint8_t s_aux_mem[0x10000];
volatile uint8_t *const g_main_bank = s_main_mem;
volatile uint8_t *const g_aux_bank  = s_aux_mem;
volatile uint32_t g_resync_pending  = 0u;
volatile uint32_t g_shr_shadow_generation = 1u;

static uint8_t s_slot_mem[COMP_APPLE_SLOT_COUNT][COMP_APPLE_SLOT_BYTES];
const uint32_t comp_apple_slot_addr[COMP_APPLE_SLOT_COUNT] = {
    (uint32_t)(uintptr_t)s_slot_mem[0],
    (uint32_t)(uintptr_t)s_slot_mem[1],
    (uint32_t)(uintptr_t)s_slot_mem[2],
};

/* ---------- handoff stubs ---------- */

static uint8_t  s_writer_slot = 0u;
static uint32_t s_settings;
static uint32_t s_pub_mode = 0xFFFFu;
static uint32_t s_pub_detail = APPLE_FB_FORMAT_UNKNOWN;
static uint8_t  s_pub_slot = 0xFFu;
static uint8_t  s_pub_border;
static uint32_t s_pub_count = 0u;

uint8_t apple_fb_writer_slot(void) { return s_writer_slot; }

void apple_fb_writer_publish_frame(uint32_t display_mode,
                                   uint8_t border_color)
{
    apple_fb_writer_publish_frame_detail(display_mode,
                                         APPLE_FB_FORMAT_UNKNOWN,
                                         border_color);
}

void apple_fb_writer_publish_frame_detail(uint32_t display_mode,
                                          uint32_t format_detail,
                                          uint8_t border_color)
{
    s_pub_mode = display_mode;
    s_pub_detail = format_detail;
    s_pub_border = border_color;
    s_pub_slot = s_writer_slot;
    s_pub_count++;
    s_writer_slot = (uint8_t)((s_writer_slot + 1u) % COMP_APPLE_SLOT_COUNT);
}

void apple_fb_handoff_secondary_init(void) {}
uint32_t apple_fb_video_settings_get(void) { return s_settings; }
uint32_t apple_fb_video_rom_gen_get(void) { return 0u; }
uint32_t apple_fb_reader_published_display_mode(void) { return s_pub_mode; }
uint32_t apple_fb_reader_published_format_detail(void) { return s_pub_detail; }

/* ---------- PAL-accurate model stubs (harness runs non-PAL modes) --- */

uint8_t apple_pal_video_mode_is_active(uint8_t color_mode)
{
    return (uint8_t)((color_mode == APPLE_VIDEO_COLOR_PAL_ACCURATE_COMPOSITE ||
                      color_mode == APPLE_VIDEO_COLOR_PAL_ACCURATE_TV) ? 1u : 0u);
}
void apple_pal_video_set_framebuffer(uint32_t *fb) { (void)fb; }
void apple_pal_video_set_video_output(uint8_t a, uint8_t b, uint8_t c)
{ (void)a; (void)b; (void)c; }
void apple_pal_video_reset(void) {}
void apple_pal_video_resync(void) {}
void apple_pal_video_begin_frame(void) {}
uint8_t apple_pal_video_end_frame(void) { return 1u; }
void apple_pal_video_preroll_line0_cycle(uint32_t cycle, uint32_t sw)
{ (void)cycle; (void)sw; }
void apple_pal_video_on_cycle(uint32_t line, uint32_t cycle, uint32_t sw)
{ (void)line; (void)cycle; (void)sw; }
void apple_pal_video_pump(void) {}

/* ---------- uart stubs ---------- */

void uart_puts(uintptr_t base, const char *s) { (void)base; (void)s; }
void uart_putc(uintptr_t base, char c) { (void)base; (void)c; }

/* ---------- record builders (apple_cycle_egress.h layout) ---------- */

static uint64_t rec_frame(uint32_t line, uint32_t cycle, uint32_t sw11)
{
    return ((uint64_t)ACE_RECORD_KIND_LEGACY << ACE_BIT_RECORD_KIND_LO) |
           (1ULL << ACE_BIT_FRAME_EN) |
           ((uint64_t)(line & 0x1FFu) << ACE_BIT_LINE_IN_FRAME_LO) |
           ((uint64_t)(cycle & 0x7Fu) << ACE_BIT_CYCLE_IN_LINE_LO) |
           ((uint64_t)(sw11 & 0x7FFu) << ACE_BIT_SW_DHIRES);
}

static uint64_t rec_write(uint32_t addr24, uint8_t data)
{
    return ((uint64_t)ACE_RECORD_KIND_LEGACY << ACE_BIT_RECORD_KIND_LO) |
           ((uint64_t)(addr24 & 0xFFFFFFu) << ACE_BIT_ADDR_DECODE_LO) |
           (1ULL << ACE_BIT_ADDR_DECODE_EN) |
           (uint64_t)data;
}

static uint64_t rec_io(uint16_t addr, uint8_t data)
{
    return ((uint64_t)ACE_RECORD_KIND_IO_WRITE << ACE_BIT_RECORD_KIND_LO) |
           ((uint64_t)addr << ACE_BIT_IO_ADDR_LO) |
           ((uint64_t)data << ACE_BIT_IO_DATA_LO);
}

static uint64_t rec_softswitch(uint16_t addr, uint32_t sw11)
{
    return ((uint64_t)ACE_RECORD_KIND_SOFTSW_ACCESS << ACE_BIT_RECORD_KIND_LO) |
           ((uint64_t)(sw11 & 0x7FFu) << ACE_BIT_SW_DHIRES) |
           ((uint64_t)addr << ACE_BIT_ADDR_DECODE_LO);
}

/* Soft-switch words. */
#define SW_TEXT_ONLY  (1u << ACE_SWB_TEXT_BIT)
#define SW_HGR        (1u << ACE_SWB_HIRES_BIT)
#define SW_DHGR       ((1u << ACE_SWB_HIRES_BIT) | (1u << ACE_SWB_80COL_BIT) | \
                       (1u << ACE_SWB_DHIRES_BIT))

/* ---------- egress mimic (apple_cycle_egress.c drain body) ---------- */

static void feed(uint64_t rec)
{
    if (rec == 0ULL) {
        g_resync_pending = 1u;
    } else if (ace_record_kind(rec) == ACE_RECORD_KIND_LEGACY &&
               ace_addr_decode_en(rec)) {
        uint32_t a = ace_addr_decode(rec);
        uint8_t  d = ace_data(rec);
        if ((a & 0x010000u) != 0u) {
            g_aux_bank[a & 0xFFFFu] = d;
            if ((a & 0xFFFFu) == 0x9DF8u) {
                apple_cycle_renderer_note_aux_ctrl_write(d);
            }
        } else {
            g_main_bank[a & 0xFFFFu] = d;
        }
        if ((uint16_t)(a & 0xFFFFu) >= 0x2000u &&
            (uint16_t)(a & 0xFFFFu) <= 0x9FFFu) {
            g_shr_shadow_generation++;
        }
    }
    apple_cycle_renderer_on_record(rec);
}

/* One full legacy frame sweep: enough cycles per line to exercise the
 * frame-edge machinery (cycles 0/1 pend the edge, cycle 2 flushes it)
 * without simulating all 65 cycles. */
static void legacy_frame(uint32_t sw11)
{
    for (uint32_t line = 0u; line < 262u; ++line) {
        feed(rec_frame(line, 0u, sw11));
        feed(rec_frame(line, 1u, sw11));
        feed(rec_frame(line, 2u, sw11));
        feed(rec_frame(line, 30u, sw11));
        feed(rec_frame(line, 64u, sw11));
    }
}

static void legacy_full_frame(uint32_t sw11)
{
    for (uint32_t line = 0u; line < 262u; ++line) {
        for (uint32_t cycle = 0u; cycle < 65u; ++cycle) {
            feed(rec_frame(line, cycle, sw11));
        }
    }
}

static void shr_marker(void)
{
    feed(rec_frame(0u, 0u, SW_TEXT_ONLY));
}

static void video7_select_mode(uint8_t mode)
{
    uint32_t sw;

    /* OFF-ON-OFF-ON-OFF. The first ON clocks mode bit 0 from !80COL;
     * the second ON clocks mode bit 1. */
    feed(rec_softswitch(0xC05Fu, 0u));
    sw = ((mode & 0x01u) != 0u) ? 0u : (1u << ACE_SWB_80COL_BIT);
    feed(rec_softswitch(0xC05Eu, sw));
    feed(rec_softswitch(0xC05Fu, sw));
    sw = ((mode & 0x02u) != 0u) ? 0u : (1u << ACE_SWB_80COL_BIT);
    feed(rec_softswitch(0xC05Eu, sw));
    feed(rec_softswitch(0xC05Fu, sw));
}

/* ---------- file + dump helpers ---------- */

static char s_repo[1024];
static char s_out[1024];

static uint8_t *load_file(const char *rel, size_t *out_len)
{
    char path[2048];
    FILE *f;
    uint8_t *buf;
    long n;

    snprintf(path, sizeof path, "%s/%s", s_repo, rel);
    f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (uint8_t *)malloc((size_t)n);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { exit(2); }
    fclose(f);
    *out_len = (size_t)n;
    return buf;
}

static void dump(const char *name, const void *data, size_t len)
{
    char path[2048];
    FILE *f;

    snprintf(path, sizeof path, "%s/%s", s_out, name);
    f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); exit(2); }
    fwrite(data, 1, len, f);
    fclose(f);
}

static void dump_published_shr(const char *name)
{
    /* SHR frames: 400 rows x 640 BGRA at the SHR stride from row 0. */
    dump(name, s_slot_mem[s_pub_slot], 640u * 400u * 4u);
}

static void dump_banks(const char *main_name, const char *aux_name)
{
    dump(main_name, s_main_mem, 0x10000u);
    dump(aux_name, s_aux_mem, 0x10000u);
}

/* ---------- A2IMGVIEW load_shr replay (no text gap, SHR stays on) --- */

static void viewer_load_shr(const uint8_t *file, size_t len)
{
    uint8_t magic[4];
    size_t i;
    uint32_t tick = 0u;

    /* Gap disarm of the legacy holes (main writes). */
    feed(rec_write(0x00407Cu, 0u));
    feed(rec_write(0x00087Cu, 0u));

    /* load_shr: neutralize stale aux ctrl+magic first (aux writes). */
    for (i = 0u; i < 8u; ++i) {
        feed(rec_write(0x019DF8u + (uint32_t)i, 0u));
    }

    /* Stage first 32K into main $2000-$9FFF; markers interleave the
     * way frames keep running during a real load. */
    for (i = 0u; i < 0x8000u && i < len; ++i) {
        feed(rec_write(0x002000u + (uint32_t)i, file[i]));
        if ((++tick & 0x0FFFu) == 0u) shr_marker();
    }

    /* Hold back the mode magic while the AUX page paints in. */
    for (i = 0u; i < 4u; ++i) {
        magic[i] = s_main_mem[0x9DFCu + i];
        feed(rec_write(0x009DFCu + (uint32_t)i, 0u));
    }
    feed(rec_io(0xC029u, 0xC1u));

    /* copy_aux_range: main $2000-$9FFF -> aux (RAMWRT writes). */
    for (i = 0u; i < 0x8000u && i < len; ++i) {
        const uint32_t addr = 0x002000u + (uint32_t)i;
        feed(rec_write(0x010000u + addr, s_main_mem[addr]));
        if ((++tick & 0x0FFFu) == 0u) shr_marker();
    }
    /* Viewer zeroes MAIN $9DF8-$9DFF after staging. */
    for (i = 0u; i < 8u; ++i) {
        feed(rec_write(0x009DF8u + (uint32_t)i, 0u));
    }
    /* Remaining payload straight into main $2000+. */
    for (i = 0x8000u; i < len; ++i) {
        feed(rec_write(0x002000u + (uint32_t)(i - 0x8000u), file[i]));
        if ((++tick & 0x0FFFu) == 0u) shr_marker();
    }

    /* Commit magic last. The next marker rebuilds the completed image;
     * later unchanged markers must reuse that published frame. */
    for (i = 0u; i < 4u; ++i) {
        feed(rec_write(0x019DFCu + (uint32_t)i, magic[i]));
    }
    for (i = 0u; i < 6u; ++i) shr_marker();
}

/* ---------- tests ---------- */

static int s_failures = 0;

static void expect(int cond, const char *what)
{
    printf("%-58s %s\n", what, cond ? "PASS" : "FAIL");
    if (!cond) s_failures++;
}

static void t1_dragons(void)
{
    size_t len;
    uint8_t *f = load_file("software/shr4_demo_images/dragons.shr4i", &len);
    size_t i;

    printf("--- T1 dragons.shr4i (interlaced R4G4B4) ---\n");
    for (i = 0u; i < 0x8000u; ++i) {
        s_aux_mem[0x2000u + i] = f[i];
        s_main_mem[0x2000u + i] = f[0x8000u + i];
    }
    feed(rec_io(0xC029u, 0xC1u));
    for (i = 0u; i < 6u; ++i) shr_marker();

    expect(s_pub_mode == APPLE_FB_DISPLAY_MODE_SHR, "T1 published mode SHR");
    expect(s_pub_detail == APPLE_FB_FORMAT_DETAIL(
               APPLE_FB_FORMAT_SHR4_320,
               APPLE_FB_FORMAT_SELECTOR_R4G4B4,
               APPLE_FB_FORMAT_PAGE_INTERLACE),
           "T1 publishes SHR4 R4G4B4 320i detail");
    dump_published_shr("t1_dragons_frame.bin");
    dump_banks("t1_main.bin", "t1_aux.bin");
    free(f);
}

static void t2_legacy_weave(void)
{
    size_t len;
    uint8_t *f = load_file("software/legacy_demo_images/eye.hgri", &len);
    size_t i;
    uint32_t weave_probe;
    int distinct_field_rows = 0;
    int colored_pixels = 0;

    printf("--- T2 legacy A2Li weave (eye.hgri) ---\n");

    /* Leave SHR: viewer's show_hgr writes NEWVIDEO=$01. */
    feed(rec_io(0xC029u, 0x01u));

    /* Text menu frames first (A2IMGVIEW inherits a text screen). */
    legacy_frame(SW_TEXT_ONLY);
    legacy_frame(SW_TEXT_ONLY);

    /* Load the pair as bus writes: page 1 then staged page 2. The
     * signature lands with page 2, but its mode byte stays zero until
     * the loader commits the complete pair. */
    feed(rec_write(0x00407Cu, 0u));          /* gap disarm */
    feed(rec_write(0x00087Cu, 0u));
    for (i = 0u; i < 0x2000u; ++i) {
        feed(rec_write(0x002000u + (uint32_t)i, f[i]));
    }
    for (i = 0u; i < 0x2000u; ++i) {
        const uint8_t d = (i == 0x7Cu) ? 0u : f[0x2000u + i];
        feed(rec_write(0x004000u + (uint32_t)i, d));
    }
    expect(g_main_bank[0x4078u] == 0xC1u && g_main_bank[0x407Cu] == 0x00u,
           "T2 A2Li signature staged with mode held off");

    legacy_frame(SW_HGR);
    legacy_frame(SW_HGR);
    legacy_frame(SW_HGR);
    expect(s_pub_mode == APPLE_FB_DISPLAY_MODE_LEGACY,
           "T2 remains ordinary HGR before final mode commit");

    feed(rec_write(0x00407Cu, f[0x207Cu]));

    /* show_hgr restores all HGR switches after the MLI close. */
    legacy_frame(SW_HGR);
    expect(s_pub_mode == APPLE_FB_DISPLAY_MODE_LEGACY_I,
           "T2 publishes the completed weave at its first frame start");
    legacy_frame(SW_HGR);
    legacy_frame(SW_HGR);
    expect(s_pub_mode == APPLE_FB_DISPLAY_MODE_LEGACY_I,
           "T2 published mode LEGACY_I while armed");
    expect(s_pub_detail == APPLE_FB_FORMAT_DETAIL(
               APPLE_FB_FORMAT_HGR, 0u,
               APPLE_FB_FORMAT_PAGE_INTERLACE),
           "T2 publishes HGRi detail with the woven slot");

    /* Weave paints 384 rows from slot row 0; probe row 383. */
    memcpy(&weave_probe,
           s_slot_mem[s_pub_slot] +
               (383u * ATN_SCRATCH_ROW_PIXELS +
                ATN_SCRATCH_LEFT_BORDER_PIXELS + ATN_ACTIVE_X + 100u) * 4u,
           4u);
    expect(weave_probe != 0u, "T2 woven row 383 painted");
    for (i = 0u; i < 192u; ++i) {
        const uint8_t *even_row = s_slot_mem[s_pub_slot] +
            ((2u * i) * ATN_SCRATCH_ROW_PIXELS +
             ATN_SCRATCH_LEFT_BORDER_PIXELS + ATN_ACTIVE_X) * 4u;
        const uint8_t *odd_row = s_slot_mem[s_pub_slot] +
            ((2u * i + 1u) * ATN_SCRATCH_ROW_PIXELS +
             ATN_SCRATCH_LEFT_BORDER_PIXELS + ATN_ACTIVE_X) * 4u;

        if (memcmp(even_row, odd_row, ATN_ACTIVE_WIDTH * 4u) != 0) {
            distinct_field_rows++;
        }
    }
    expect(distinct_field_rows > 32,
           "T2 even and odd field rows come from distinct pages");

    /* Composite HGR must contain artifact color. The synthesized path does
     * not run horizontal blanking/colorburst cycles, so this catches a
     * missing burst latch that silently sends every dot through mono. */
    for (uint32_t row = 0u; row < 384u; ++row) {
        const uint32_t *pixels = (const uint32_t *)s_slot_mem[s_pub_slot] +
            row * ATN_SCRATCH_ROW_PIXELS +
            ATN_SCRATCH_LEFT_BORDER_PIXELS + ATN_ACTIVE_X;
        for (uint32_t x = 0u; x < ATN_ACTIVE_WIDTH; ++x) {
            const uint32_t p = pixels[x];
            const uint8_t b = (uint8_t)p;
            const uint8_t g = (uint8_t)(p >> 8);
            const uint8_t r = (uint8_t)(p >> 16);
            if (r != g || g != b) {
                colored_pixels++;
            }
        }
    }
    expect(colored_pixels > 100,
           "T2 HGRi weave retains composite artifact color");

    /* Drop to text (BASIC prompt): weave must suspend. Three frames:
     * the switch state propagates during frame 1, frame 2 selects
     * LEGACY at its start, and frame 3's edge publishes it. */
    legacy_frame(SW_TEXT_ONLY);
    legacy_frame(SW_TEXT_ONLY);
    legacy_frame(SW_TEXT_ONLY);
    expect(s_pub_mode == APPLE_FB_DISPLAY_MODE_LEGACY,
           "T2 weave suspends when TEXT comes back");

    /* HGR again: resumes. */
    legacy_frame(SW_HGR);
    legacy_frame(SW_HGR);
    legacy_frame(SW_HGR);
    expect(s_pub_mode == APPLE_FB_DISPLAY_MODE_LEGACY_I,
           "T2 weave resumes when HGR returns");

    /* Mode-byte disarm. */
    feed(rec_write(0x00407Cu, 0u));
    legacy_frame(SW_HGR);
    legacy_frame(SW_HGR);
    legacy_frame(SW_HGR);
    expect(s_pub_mode == APPLE_FB_DISPLAY_MODE_LEGACY,
           "T2 weave disarms when the hole mode byte clears");

    free(f);
}

static void t3_shr_transitions(void)
{
    size_t len_beach, len_eye, len_fluid;
    uint8_t *beach = load_file("software/shr4_demo_images/beach.3200", &len_beach);
    uint8_t *eye   = load_file("software/shr4_demo_images/eye320.shr4", &len_eye);
    uint8_t *fluid = load_file("software/shr4_demo_images/fluidart.shr4i", &len_fluid);
    uint32_t published;
    uint32_t skipped;
    uint32_t rebuilt;
    uint8_t old_pixel;

    printf("--- T3 A2IMGVIEW no-gap SHR transitions ---\n");

    feed(rec_io(0xC029u, 0xC1u));

    viewer_load_shr(beach, len_beach);
    expect(s_pub_mode == APPLE_FB_DISPLAY_MODE_SHR, "T3 beach published as SHR");
    expect(s_pub_detail == APPLE_FB_FORMAT_DETAIL(
               APPLE_FB_FORMAT_SHR3200_320, 0u,
               APPLE_FB_FORMAT_PAGE_NONE),
           "T3 beach publishes SHR-3200 320 detail");
    dump_published_shr("t3_beach_frame.bin");
    dump_banks("t3_beach_main.bin", "t3_beach_aux.bin");

    viewer_load_shr(eye, len_eye);
    expect(s_pub_mode == APPLE_FB_DISPLAY_MODE_SHR, "T3 eye320 published as SHR");
    expect(s_pub_detail == APPLE_FB_FORMAT_DETAIL(
               APPLE_FB_FORMAT_SHR4_320,
               APPLE_FB_FORMAT_SELECTOR_RGGB,
               APPLE_FB_FORMAT_PAGE_NONE),
           "T3 eye publishes SHR4 RGGB 320 detail");
    dump_published_shr("t3_eye_frame.bin");
    dump_banks("t3_eye_main.bin", "t3_eye_aux.bin");

    viewer_load_shr(fluid, len_fluid);
    expect(s_pub_mode == APPLE_FB_DISPLAY_MODE_SHR, "T3 fluidart published as SHR");
    expect(s_pub_detail == APPLE_FB_FORMAT_DETAIL(
               APPLE_FB_FORMAT_SHR4_320,
               APPLE_FB_FORMAT_SELECTOR_PAL256,
               APPLE_FB_FORMAT_PAGE_INTERLACE),
           "T3 fluid publishes SHR4 PAL256 320i detail");
    dump_published_shr("t3_fluid_frame.bin");
    dump_banks("t3_fluid_main.bin", "t3_fluid_aux.bin");

    published = s_pub_count;
    skipped = g_acr_shr_frames_skipped;
    shr_marker();
    shr_marker();
    expect(s_pub_count == published,
           "T3 static SHR markers do not republish cached frame");
    expect(g_acr_shr_frames_skipped == skipped + 2u,
           "T3 static SHR markers count two cache skips");

    old_pixel = g_aux_bank[0x2000u];
    rebuilt = g_acr_shr_cache_rebuilds;
    feed(rec_write(0x012000u, (uint8_t)(old_pixel ^ 0x01u)));
    shr_marker();
    expect(s_pub_count == published + 1u,
           "T3 one shadow write rebuilds and publishes at next marker");
    expect(g_acr_shr_cache_rebuilds == rebuilt + 1u,
           "T3 one shadow write counts one cache rebuild");

    published = s_pub_count;
    shr_marker();
    expect(s_pub_count == published,
           "T3 rebuilt static frame returns to cache-hit state");

    free(beach); free(eye); free(fluid);
}

static void t4_dhgr_col140m(void)
{
    uint32_t color_pixels[28];
    uint32_t mixed_pixels[28];
    uint32_t disabled_pixels[28];
    uint32_t mixed_detail;
    uint32_t disabled_detail;
    const uint32_t row = 80u + ATN_ACTIVE_Y;
    int mono_changed = 0;
    int mono_is_bw = 1;
    int color_unchanged = 1;

    printf("--- T4 Video-7 MIX (COL140M) state gate and 8+8+8+4 alignment ---\n");
    feed(rec_io(0xC029u, 0x01u));
    memset(&s_aux_mem[0x2000u], 0x55, 0x2000u);
    memset(&s_main_mem[0x2000u], 0xAA, 0x2000u);

    s_settings = apple_video_settings_pack_border_full(
        0u, 0u, APPLE_VIDEO_COLOR_RGB, 1u, 1u, 0, 0, 0u, 0u, 0u);
    legacy_full_frame(SW_DHGR);
    legacy_full_frame(SW_DHGR);
    memcpy(color_pixels,
           s_slot_mem[s_pub_slot] +
               (row * ATN_SCRATCH_ROW_PIXELS +
                ATN_SCRATCH_LEFT_BORDER_PIXELS + ATN_ACTIVE_X) * 4u,
           sizeof(color_pixels));

    video7_select_mode(2u);
    legacy_full_frame(SW_DHGR);
    legacy_full_frame(SW_DHGR);
    memcpy(mixed_pixels,
           s_slot_mem[s_pub_slot] +
               (row * ATN_SCRATCH_ROW_PIXELS +
                ATN_SCRATCH_LEFT_BORDER_PIXELS + ATN_ACTIVE_X) * 4u,
           sizeof(mixed_pixels));
    mixed_detail = s_pub_detail;

    s_settings = apple_video_settings_pack_border_full(
        0u, 0u, APPLE_VIDEO_COLOR_RGB, 1u, 0u, 0, 0, 0u, 0u, 0u);
    legacy_full_frame(SW_DHGR);
    legacy_full_frame(SW_DHGR);
    memcpy(disabled_pixels,
           s_slot_mem[s_pub_slot] +
               (row * ATN_SCRATCH_ROW_PIXELS +
                ATN_SCRATCH_LEFT_BORDER_PIXELS + ATN_ACTIVE_X) * 4u,
           sizeof(disabled_pixels));
    disabled_detail = s_pub_detail;

    for (int i = 0; i < 28; ++i) {
        const int mono = (i < 8 || (i >= 16 && i < 24));
        const uint32_t p = mixed_pixels[i];
        const uint8_t b = (uint8_t)p;
        const uint8_t g = (uint8_t)(p >> 8);
        const uint8_t r = (uint8_t)(p >> 16);

        if (mono) {
            if (p != color_pixels[i]) {
                mono_changed++;
            }
            if (r != g || g != b) {
                mono_is_bw = 0;
            }
        } else if (p != color_pixels[i]) {
            color_unchanged = 0;
        }
    }

    expect(mono_changed > 0,
           "T4 AUX-controlled 8-dot spans switch to monochrome");
    expect(mono_is_bw,
           "T4 monochrome spans contain only neutral pixels");
    expect(color_unchanged,
           "T4 MAIN controls 8 dots then the final 4 dots");
    expect((mixed_detail & APPLE_FB_FORMAT_SELECTORS_MASK) ==
               (APPLE_FB_FORMAT_LEGACY_VIDEO7_MIX <<
                APPLE_FB_FORMAT_SELECTORS_SHIFT),
           "T4 DHGR badge metadata carries Video-7 MIX");
    expect(memcmp(disabled_pixels, color_pixels, sizeof(color_pixels)) == 0,
           "T4 checkbox off ignores latched Video-7 state 10");
    expect((disabled_detail & APPLE_FB_FORMAT_SELECTORS_MASK) == 0u,
           "T4 checkbox off removes the Video-7 MIX badge tag");

    s_settings = apple_video_settings_pack_border_full(
        0u, 0u, APPLE_VIDEO_COLOR_RGB, 1u, 1u, 0, 0, 0u, 0u, 0u);
    video7_select_mode(3u);
    legacy_full_frame(SW_HGR);
    legacy_full_frame(SW_HGR);
    legacy_full_frame(SW_HGR);
    expect((s_pub_detail & APPLE_FB_FORMAT_BASE_MASK) == APPLE_FB_FORMAT_HGR &&
           (s_pub_detail & APPLE_FB_FORMAT_SELECTORS_MASK) ==
               (APPLE_FB_FORMAT_LEGACY_VIDEO7_MONO <<
                APPLE_FB_FORMAT_SELECTORS_SHIFT),
           "T4 HGR badge metadata carries Video-7 mono");

    video7_select_mode(2u);
    s_main_mem[0x4078u] = 0xC1u;
    s_main_mem[0x4079u] = 0xB2u;
    s_main_mem[0x407Au] = 0xCCu;
    s_main_mem[0x407Bu] = 0xE9u;
    s_main_mem[0x407Cu] = 1u;
    legacy_full_frame(SW_DHGR);
    legacy_full_frame(SW_DHGR);
    legacy_full_frame(SW_DHGR);
    expect((s_pub_detail & APPLE_FB_FORMAT_BASE_MASK) == APPLE_FB_FORMAT_DHGR &&
           (s_pub_detail & APPLE_FB_FORMAT_SELECTORS_MASK) ==
               (APPLE_FB_FORMAT_LEGACY_VIDEO7_MIX <<
                APPLE_FB_FORMAT_SELECTORS_SHIFT) &&
           (s_pub_detail & APPLE_FB_FORMAT_PAGE_MASK) ==
               (APPLE_FB_FORMAT_PAGE_INTERLACE <<
                APPLE_FB_FORMAT_PAGE_SHIFT),
           "T4 DHGRi badge metadata carries Video-7 MIX");

    s_main_mem[0x407Cu] = 2u;
    legacy_full_frame(SW_DHGR);
    legacy_full_frame(SW_DHGR);
    legacy_full_frame(SW_DHGR);
    expect((s_pub_detail & APPLE_FB_FORMAT_BASE_MASK) == APPLE_FB_FORMAT_DHGR &&
           (s_pub_detail & APPLE_FB_FORMAT_SELECTORS_MASK) ==
               (APPLE_FB_FORMAT_LEGACY_VIDEO7_MIX <<
                APPLE_FB_FORMAT_SELECTORS_SHIFT) &&
           (s_pub_detail & APPLE_FB_FORMAT_PAGE_MASK) ==
               (APPLE_FB_FORMAT_PAGE_FLIP_MERGE <<
                APPLE_FB_FORMAT_PAGE_SHIFT),
           "T4 DHGRp badge metadata carries Video-7 MIX");
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: harness <repo_root> <out_dir>\n");
        return 2;
    }
    snprintf(s_repo, sizeof s_repo, "%s", argv[1]);
    snprintf(s_out, sizeof s_out, "%s", argv[2]);

    s_settings = apple_video_settings_pack_border_full(
        0u, 0u, APPLE_VIDEO_COLOR_COMPOSITE_MONITOR, 1u, 1u,
        0u, 0u, 0, 0, 0u);

    if (apple_cycle_renderer_init() != 0) {
        fprintf(stderr, "renderer init failed\n");
        return 2;
    }

    t1_dragons();
    t2_legacy_weave();
    t3_shr_transitions();
    t4_dhgr_col140m();

    printf("harness: %d failure(s)\n", s_failures);
    return s_failures ? 1 : 0;
}
