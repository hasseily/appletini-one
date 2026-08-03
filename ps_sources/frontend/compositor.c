/*
 * compositor.c -- See compositor.h.
 *
 * Owns the output FB triple buffer. Reads from the Apple FB ring
 * (apple_cycle_renderer.c) and renders the UI overlay around it.
 *
 * Synchronization with PL:
 *   - PS writes FB_BASE_ADDR_REG to request a slot for the next frame.
 *   - PL fb_reader latches the value at vblank-start, increments the
 *     vblank frame counter (FB_STATUS_REG) and snapshots the latched
 *     base into FB_LAST_LATCHED_REG.
 *   - PS waits for FB_STATUS_REG to advance past s_published_counter
 *     before reusing a slot. If FB_LAST_LATCHED_REG matches a slot
 *     base, that slot is being scanned out -- never reuse it.
 *
 * Synchronization with the Apple renderer uses apple_fb_handoff. The
 * compositor claims one published slot per frame; the three-slot ownership
 * protocol keeps that slot stable until the compositor finishes reading.
 *
 * UI drawing is delegated to a callback (compositor_ui_draw_fn) that
 * main.c registers via compositor_init(). Its base pass paints the bezel,
 * then the compositor blits the Apple subwindow, and its overlay pass paints
 * status UI above the Apple border.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#include "xil_cache.h"
#include "xil_mmu.h"
#include "xiltimer.h"

#include "../lib/common.h"
#include "../lib/fb16.h"
#include "../lib/framebuffer.h"
#include "../lib/uart.h"

#include "apple_cycle_renderer.h"
#include "apple_fb_handoff.h"
#include "card_control_regs.h"
#include "compositor.h"
#include "compositor_layout.h"
#include "scanlines.h"
#include "supersprite_vdp.h"
#include "video_blur.h"
#include "video_ghosting.h"
#include "video_glow.h"

/* ---------- Public counters ---------- */

volatile uint32_t g_compositor_frames_published     = 0u;
volatile uint32_t g_compositor_frames_skipped       = 0u;
volatile uint32_t g_compositor_apple_frames_drawn   = 0u;
volatile uint32_t g_compositor_last_apple_slot      = 0xFFu;
volatile uint32_t g_compositor_last_apple_mode      = APPLE_FB_DISPLAY_MODE_LEGACY;
volatile uint32_t g_compositor_last_ui_us           = 0u;
volatile uint32_t g_compositor_last_apple_us        = 0u;
volatile uint32_t g_compositor_last_sync_us         = 0u;
volatile uint32_t g_compositor_last_total_us        = 0u;
volatile uint32_t g_compositor_last_apple_drawn     = 0u;
volatile uint32_t g_compositor_last_suppress_apple  = 0u;

/* ---------- Private state ---------- */

/* 0xFF = no slot picked yet (the very first composite has no prior
 * publish to coordinate against). */
static uint8_t  s_writer_idx        = 0xFFu;
static uint8_t  s_published_idx     = 0xFFu;
static uint32_t s_composited_apple_seq = 0u;

/* Counter value we expect FB_STATUS_REG to reach (or exceed) before our
 * last publish is considered latched by the PL. Initialized below in
 * compositor_init(). */
static uint32_t s_published_counter = 0u;

/* UI draw callback + context (set by main.c at boot). */
static compositor_ui_draw_fn s_ui_draw   = NULL;
static const void           *s_ui_state  = NULL;
static const void           *s_ui_config = NULL;
static uint8_t               s_scanlines_mode = APPLETINI_SCANLINES_OFF;
static uint8_t               s_video_ghosting_strength = APPLETINI_VIDEO_GHOSTING_OFF;
static uint8_t               s_video_blur_strength = APPLETINI_VIDEO_BLUR_OFF;
static uint8_t               s_video_glow_strength = APPLETINI_VIDEO_GLOW_OFF;
static uint8_t               s_format_badge_enabled = 0u;
static uint8_t               s_border_enabled = 0u;
static uint8_t               s_border_flood = 0u;
static volatile uint8_t      s_force_full_refresh = 0u;
static volatile uint8_t      s_paused = 0u;

/* Debug/measurement: when set, compositor_tick() composites and publishes
 * on every call, bypassing the vblank + fresh-Apple-frame pacing. This
 * measures the raw composite-and-publish ceiling independent of the 50/60 Hz
 * cap (a PAL machine + 50 Hz HDMI pins both the renderer and vblank to 50).
 * Default off; leave off for normal use -- uncapped publishing produces
 * frames the PL never scans out and can tear. */
static volatile uint8_t      s_uncapped = 0u;

static uint32_t ticks_to_us(XTime ticks)
{
    uint64_t us;

    if (ticks == 0U || COUNTS_PER_SECOND == 0U) {
        return 0U;
    }

    us = ((uint64_t)ticks * 1000000ULL) / (uint64_t)COUNTS_PER_SECOND;
    return (us > UINT32_MAX) ? UINT32_MAX : (uint32_t)us;
}

/* ---------- Apple subwindow ghosting ---------- */

#define EFFECT_HISTORY_STRIDE COMP_APPLE_SHR_WIDTH
#define EFFECT_HISTORY_PIXELS (COMP_APPLE_SHR_WIDTH * COMP_APPLE_SHR_HEIGHT)
#define EFFECT_WEIGHT_FULL    64U

/* Decay numerators (per frame, in 64ths) -- THE tuning knobs for the
 * phosphor feel. BRIGHT = initial knock-down from full brightness,
 * KNEE = the hand-off band, TAIL = release rate (closer to 64 = longer
 * tail). Shared by the scalar and NEON blend paths via the
 * k_effect_numer_* tables below, indexed by ghosting strength
 * (OFF / LIGHT / MEDIUM / STRONG). */
#define EFFECT_NUMER_BRIGHT_LIGHT  14U
#define EFFECT_NUMER_BRIGHT_MEDIUM 18U
#define EFFECT_NUMER_BRIGHT_STRONG  24U
#define EFFECT_NUMER_KNEE_LIGHT    40U
#define EFFECT_NUMER_KNEE_MEDIUM   48U
#define EFFECT_NUMER_KNEE_STRONG    56U
#define EFFECT_NUMER_TAIL_LIGHT    58U
#define EFFECT_NUMER_TAIL_MEDIUM   60U
#define EFFECT_NUMER_TAIL_STRONG    61U

static const uint8_t k_effect_numer_bright[APPLETINI_VIDEO_GHOSTING_MAX + 1U] = {
    0U, EFFECT_NUMER_BRIGHT_LIGHT, EFFECT_NUMER_BRIGHT_MEDIUM,
    EFFECT_NUMER_BRIGHT_STRONG
};
static const uint8_t k_effect_numer_knee[APPLETINI_VIDEO_GHOSTING_MAX + 1U] = {
    0U, EFFECT_NUMER_KNEE_LIGHT, EFFECT_NUMER_KNEE_MEDIUM,
    EFFECT_NUMER_KNEE_STRONG
};
static const uint8_t k_effect_numer_tail[APPLETINI_VIDEO_GHOSTING_MAX + 1U] = {
    0U, EFFECT_NUMER_TAIL_LIGHT, EFFECT_NUMER_TAIL_MEDIUM,
    EFFECT_NUMER_TAIL_STRONG
};

static uint32_t s_effect_history[EFFECT_HISTORY_PIXELS]
    __attribute__((aligned(32)));
static uint32_t s_effect_row[COMP_APPLE_SHR_WIDTH]
    __attribute__((aligned(32)));
static uint16_t s_effect_2x_row[COMP_APPLE_SHR_WIDTH * 2U]
    __attribute__((aligned(32)));
static uint16_t s_effect_history_w = 0U;
static uint16_t s_effect_history_h = 0U;

/* Scale each RGB channel by numer/64. numer <= 64 keeps the pairwise
 * field trick overflow-free: 0xFF * 64 = 0x3FC0 stays clear of the
 * neighboring channel's field before the >> 6. */
static inline uint32_t effect_scale_64(uint32_t p, uint32_t numer)
{
    if (numer >= EFFECT_WEIGHT_FULL) {
        return p;
    }
    if (numer == 0U) {
        return 0U;
    }

    return 0xFF000000U |
        ((((p & 0x00FF00FFU) * numer) >> 6) & 0x00FF00FFU) |
        ((((p & 0x0000FF00U) * numer) >> 6) & 0x0000FF00U);
}

/* Phosphor decay envelope, per frame, in 64ths. Three brightness bands
 * give the CRT feel: a SHARP initial drop from full brightness, then a
 * LONG slow release through the dim tail, then a snap to fully off so
 * near-black remnants don't dither forever.
 *
 * Tail persistence (frames from just-below-mid 0x1F down to the snap-off
 * floor): Light ~21, Strong ~43. To retune, the >=0x80 band is the initial
 * knock-down, the >=0x20 band is
 * the knee, the final band is the release rate (closer to 64 = longer). */
static inline uint8_t effect_decay_numer(uint32_t p, uint8_t strength)
{
    strength = appletini_video_ghosting_clamp(strength);
    if (strength == APPLETINI_VIDEO_GHOSTING_OFF) {
        return 0U;
    }

    if ((p & 0x00FCFCFCU) == 0U) {
        return 0U;      /* all channels < 4/255: off */
    }
    if ((p & 0x00808080U) != 0U) {
        return k_effect_numer_bright[strength];
    }
    if ((p & 0x00202020U) != 0U) {
        return k_effect_numer_knee[strength];
    }
    return k_effect_numer_tail[strength];
}

static inline uint32_t effect_max_rgb(uint32_t a, uint32_t b)
{
#if defined(__arm__)
    uint32_t tmp;
    uint32_t out;

    __asm__ volatile (
        "usub8 %0, %2, %3\n\t"
        "sel %1, %2, %3\n\t"
        : "=&r"(tmp), "=&r"(out)
        : "r"(a), "r"(b)
        : "cc");
    (void)tmp;
    return out | 0xFF000000U;
#else
    const uint32_t ar = (a >> 16) & 0xFFU;
    const uint32_t ag = (a >> 8) & 0xFFU;
    const uint32_t ab = a & 0xFFU;
    const uint32_t br = (b >> 16) & 0xFFU;
    const uint32_t bg = (b >> 8) & 0xFFU;
    const uint32_t bb = b & 0xFFU;
    const uint32_t r = (ar > br) ? ar : br;
    const uint32_t g = (ag > bg) ? ag : bg;
    const uint32_t bl = (ab > bb) ? ab : bb;

    return 0xFF000000U | (r << 16) | (g << 8) | bl;
#endif
}


/* ---------- Phosphor blur (spatial spot glow) ----------
 *
 * Display-only separable tent filter over the post-ghosting 8:8:8 rows,
 * applied at Apple source resolution before the 2x doubling. History keeps
 * the unblurred pixel, so persistence and glow never feedback-accumulate.
 *
 * The taps are SWAR halves/quarters/eighths on the 0x00RRGGBB fields; the
 * masked terms sum to at most 0xFD per channel, so no cross-field carry.
 * Truncation loses up to ~2 LSB per channel -- invisible next to the 565
 * pack that follows.
 *
 * Strengths: LIGHT = horizontal 1/4,1/2,1/4. MEDIUM = LIGHT plus the
 * 1/4,1/2,1/4 kernel vertically (the vertical tap is what softens the
 * scanline gap; the three-row ring below delays emission by one source
 * row so it stays symmetric). STRONG = the wider 1/8,1/4,1/4,1/4,1/8
 * horizontal tap plus the same vertical tent. */

static inline uint32_t effect_rgb_half(uint32_t p)
{
    return (p >> 1) & 0x007F7F7FU;
}

static inline uint32_t effect_rgb_quarter(uint32_t p)
{
    return (p >> 2) & 0x003F3F3FU;
}

static inline uint32_t effect_rgb_eighth(uint32_t p)
{
    return (p >> 3) & 0x001F1F1FU;
}

static uint32_t s_effect_blur_ring[3U][COMP_APPLE_SHR_WIDTH]
    __attribute__((aligned(32)));

/* ---------- Phosphor glow (additive halo) ----------
 *
 * SDD-style bloom, independent of blur: the emitted pixel is the base
 * (sharp, or blurred when blur is enabled) plus a scaled copy of the
 * full separable tent of the same content, saturating per channel so
 * brights bleed toward white instead of wrapping. Strength picks the
 * halo weight in 64ths; the halo kernel is the MEDIUM tent whenever
 * blur itself is off. */
#define EFFECT_GLOW_NUMER_LIGHT   8U
#define EFFECT_GLOW_NUMER_MEDIUM 16U
#define EFFECT_GLOW_NUMER_STRONG 32U

static const uint8_t k_effect_glow_numer[APPLETINI_VIDEO_GLOW_MAX + 1U] = {
    0U, EFFECT_GLOW_NUMER_LIGHT, EFFECT_GLOW_NUMER_MEDIUM,
    EFFECT_GLOW_NUMER_STRONG
};

/* Sharp (pre-blur) rows for the glow-over-sharp base when blur is off,
 * plus scratch for the assembled base and halo rows at emit time. */
static uint32_t s_effect_sharp_ring[3U][COMP_APPLE_SHR_WIDTH]
    __attribute__((aligned(32)));
static uint32_t s_effect_halo_row[COMP_APPLE_SHR_WIDTH]
    __attribute__((aligned(32)));

/* Saturating per-channel add in the same pairwise-field style as
 * effect_scale_64: RB spread across 0x00FF00FF and G at 0x0000FF00
 * give every channel headroom, and the per-field overflow bit turns
 * into a 0xFF clamp without cross-field borrows. Alpha is ignored;
 * the 565 pack never reads it. */
static inline uint32_t effect_rgb_sat_add(uint32_t p, uint32_t q)
{
    uint32_t rb = (p & 0x00FF00FFu) + (q & 0x00FF00FFu);
    uint32_t g  = (p & 0x0000FF00u) + (q & 0x0000FF00u);
    const uint32_t sat_rb = (rb >> 8) & 0x00010001u;
    const uint32_t sat_g  = (g >> 16) & 0x1u;

    rb = (rb & 0x00FF00FFu) | ((sat_rb << 8) - sat_rb);
    g  = (g & 0x0000FF00u) | (0x0000FF00u & (0u - sat_g));
    return rb | g;
}

/* Vertical stage over the three H-filtered ring rows: the 1/4,1/2,1/4
 * tent for STRONG blur and for every glow halo; pass-through otherwise. */
static void effect_blur_v_row(const uint32_t *up, const uint32_t *mid,
                              const uint32_t *dn, uint32_t *dst, int w,
                              int tent)
{
    if (!tent) {
        memcpy(dst, mid, (size_t)w * sizeof(uint32_t));
        return;
    }
    for (int x = 0; x < w; ++x) {
        dst[x] = effect_rgb_half(mid[x]) + effect_rgb_quarter(up[x]) +
                 effect_rgb_quarter(dn[x]);
    }
}

static void effect_blur_h_row(const uint32_t *src,
                              uint32_t *dst,
                              int w,
                              uint8_t blur)
{
    if (blur == APPLETINI_VIDEO_BLUR_OFF || w < 4) {
        memcpy(dst, src, (size_t)w * sizeof(uint32_t));
        return;
    }

    if (blur == APPLETINI_VIDEO_BLUR_STRONG) {
        /* Wide 1/8,1/4,1/4,1/4,1/8 tap. Edge pixels clamp. */
        for (int x = 0; x < w; ++x) {
            const uint32_t c  = src[x];
            const uint32_t l1 = (x > 0) ? src[x - 1] : c;
            const uint32_t l2 = (x > 1) ? src[x - 2] : l1;
            const uint32_t r1 = (x + 1 < w) ? src[x + 1] : c;
            const uint32_t r2 = (x + 2 < w) ? src[x + 2] : r1;
            dst[x] = effect_rgb_quarter(c) +
                     effect_rgb_quarter(l1) + effect_rgb_quarter(r1) +
                     effect_rgb_eighth(l2) + effect_rgb_eighth(r2);
        }
        return;
    }

    /* LIGHT and MEDIUM share the 1/4,1/2,1/4 horizontal tap. */
    {
        uint32_t left = src[0];
        for (int x = 0; x < w; ++x) {
            const uint32_t c = src[x];
            const uint32_t r = (x + 1 < w) ? src[x + 1] : c;
            dst[x] = effect_rgb_half(c) +
                     effect_rgb_quarter(left) + effect_rgb_quarter(r);
            left = c;
        }
    }
}

/* Assemble one output row from the rings, apply the optional glow
 * halo, and emit the 565 pack + horizontal doubling into
 * s_effect_2x_row -- one pass, same contract as the ghosting emit. */
static void effect_emit_2x_row(const uint32_t *sharp,
                               const uint32_t *up,
                               const uint32_t *mid,
                               const uint32_t *dn,
                               int w,
                               uint8_t blur,
                               uint8_t glow)
{
    const uint32_t *base;

    /* The vertical tent serves double duty: MEDIUM/STRONG blur's
     * vertical stage and (always) the glow halo's shape. */
    if (glow != APPLETINI_VIDEO_GLOW_OFF ||
        blur >= APPLETINI_VIDEO_BLUR_MEDIUM) {
        effect_blur_v_row(up, mid, dn, s_effect_halo_row, w, 1);
    }

    if (blur == APPLETINI_VIDEO_BLUR_OFF) {
        base = sharp;
    } else if (blur == APPLETINI_VIDEO_BLUR_LIGHT) {
        base = mid;
    } else {
        base = s_effect_halo_row;
    }

    if (glow != APPLETINI_VIDEO_GLOW_OFF) {
        const uint32_t numer = k_effect_glow_numer[glow];
        for (int x = 0; x < w; ++x) {
            const uint32_t p = effect_rgb_sat_add(
                base[x], effect_scale_64(s_effect_halo_row[x], numer));
            const uint32_t v = fb16_from_bgra32(p);
            ((uint32_t *)s_effect_2x_row)[x] = (v << 16) | v;
        }
        return;
    }

    for (int x = 0; x < w; ++x) {
        const uint32_t p = base[x];
        const uint32_t v = fb16_from_bgra32(p);
        ((uint32_t *)s_effect_2x_row)[x] = (v << 16) | v;
    }
}

static uint8_t effect_scanline_blank(uint8_t phase,
                                     uint8_t scale_y,
                                     uint8_t scanline_mode)
{
    scanline_mode = appletini_scanlines_clamp(scanline_mode);
    if (scale_y >= 4U) {
        return (uint8_t)(((phase == 1U && scanline_mode >= 3U) ||
                          (phase == 2U && scanline_mode >= 2U) ||
                          (phase == 3U && scanline_mode >= 1U)) ? 1U : 0U);
    }
    return (uint8_t)((phase == 1U && scanline_mode >= 2U) ? 1U : 0U);
}

static void effect_clear_history(void)
{
    memset(s_effect_history, 0, sizeof(s_effect_history));
    s_effect_history_w = 0U;
    s_effect_history_h = 0U;
}

static void blit_apple_ghosting_2x(uint16_t *fb,
                                   int dst_x,
                                   int dst_y,
                                   const uint32_t *src,
                                   int src_w,
                                   int src_h,
                                   int src_stride,
                                   uint8_t scale_y,
                                   uint8_t scanline_mode,
                                   uint8_t strength,
                                   uint8_t blur,
                                   uint8_t glow)
{
    const size_t row_pixels = (size_t)src_w * 2U;

    if (src == NULL || src_w <= 0 || src_h <= 0 ||
        src_w > (int)COMP_APPLE_SHR_WIDTH ||
        src_h > (int)COMP_APPLE_SHR_HEIGHT) {
        return;
    }
    if (src_stride <= 0) {
        src_stride = src_w;
    }
    if (scale_y != 2U && scale_y != 4U) {
        return;
    }

    blur = appletini_video_blur_clamp(blur);
    glow = appletini_video_glow_clamp(glow);
    if (appletini_video_ghosting_clamp(strength) == APPLETINI_VIDEO_GHOSTING_OFF &&
        blur == APPLETINI_VIDEO_BLUR_OFF &&
        glow == APPLETINI_VIDEO_GLOW_OFF) {
        return;
    }

    if (s_effect_history_w != (uint16_t)src_w ||
        s_effect_history_h != (uint16_t)src_h) {
        effect_clear_history();
        s_effect_history_w = (uint16_t)src_w;
        s_effect_history_h = (uint16_t)src_h;
    }

    if (blur != APPLETINI_VIDEO_BLUR_OFF || glow != APPLETINI_VIDEO_GLOW_OFF) {
        /* Blur/glow path: same ghosting blend, but the row is kept in
         * 8:8:8, horizontally filtered into a three-row ring, and packed
         * to 565 one source row late so the vertical tap sees both
         * neighbors. With glow but no blur, the ring still carries the
         * LIGHT horizontal tap purely as the halo source, and the sharp
         * ring keeps the unfiltered base. With ghosting OFF the decay
         * numerator is 0 and the blend degenerates to pass-through
         * (history writes are then redundant but harmless). The loop
         * runs one extra iteration to flush the delayed final row. */
        const uint8_t ring_h_kernel = (blur != APPLETINI_VIDEO_BLUR_OFF)
            ? blur : (uint8_t)APPLETINI_VIDEO_BLUR_LIGHT;

        for (int sy = 0; sy <= src_h; ++sy) {
            if (sy < src_h) {
                const uint32_t *srow = src + sy * src_stride;
                const uint32_t hist_base = (uint32_t)sy * EFFECT_HISTORY_STRIDE;

                memcpy(s_effect_row, srow, (size_t)src_w * sizeof(uint32_t));
                for (int x = 0; x < src_w; ++x) {
                    uint32_t p = s_effect_row[x];
                    uint32_t *hist = &s_effect_history[hist_base + (uint32_t)x];
                    const uint8_t decay_numer = effect_decay_numer(*hist, strength);

                    p = effect_max_rgb(p, effect_scale_64(*hist, decay_numer));
                    *hist = p;
                    s_effect_row[x] = p;
                }
                if (blur == APPLETINI_VIDEO_BLUR_OFF) {
                    memcpy(s_effect_sharp_ring[sy % 3], s_effect_row,
                           (size_t)src_w * sizeof(uint32_t));
                }
                effect_blur_h_row(s_effect_row, s_effect_blur_ring[sy % 3],
                                  src_w, ring_h_kernel);
            }
            if (sy < 1) {
                continue;
            }
            {
                const int ey = sy - 1;
                const uint32_t *up =
                    s_effect_blur_ring[((ey > 0) ? (ey - 1) : 0) % 3];
                const uint32_t *mid = s_effect_blur_ring[ey % 3];
                const uint32_t *dn =
                    s_effect_blur_ring[((sy < src_h) ? sy : ey) % 3];

                effect_emit_2x_row(s_effect_sharp_ring[ey % 3], up, mid, dn,
                                   src_w, blur, glow);
                for (uint8_t phase = 0U; phase < scale_y; ++phase) {
                    uint16_t *drow = fb +
                        (dst_y + ey * (int)scale_y + (int)phase) * FB16_WIDTH +
                        dst_x;
                    if (effect_scanline_blank(phase, scale_y,
                                              scanline_mode) != 0U) {
                        memset(drow, 0, row_pixels * sizeof(uint16_t));
                    } else {
                        memcpy(drow, s_effect_2x_row,
                               row_pixels * sizeof(uint16_t));
                    }
                }
            }
        }
        return;
    }

    for (int sy = 0; sy < src_h; ++sy) {
        const uint32_t *srow = src + sy * src_stride;
        const uint32_t hist_base = (uint32_t)sy * EFFECT_HISTORY_STRIDE;

        /* Pull the uncached row into cached scratch with burst reads before
         * blending. Reading NORM_NONCACHE source pixels in the inner loop
         * would require one DDR round trip per pixel. */
        memcpy(s_effect_row, srow, (size_t)src_w * sizeof(uint32_t));

        /* Blend in 8:8:8 (history keeps full phosphor precision) and
         * emit the 2x-doubled 565 output pair in the same pass: the
         * scalar loop already owns every pixel, so narrowing here
         * costs three shifts and saves a whole row re-read and
         * expansion pass. Keep this loop scalar and self-contained. */
        for (int x = 0; x < src_w; ++x) {
            uint32_t p = s_effect_row[x];
            uint32_t *hist = &s_effect_history[hist_base + (uint32_t)x];
            const uint8_t decay_numer = effect_decay_numer(*hist, strength);

            p = effect_max_rgb(p, effect_scale_64(*hist, decay_numer));
            *hist = p;
            {
                const uint32_t v = fb16_from_bgra32(p);
                ((uint32_t *)s_effect_2x_row)[x] = (v << 16) | v;
            }
        }

        for (uint8_t phase = 0U; phase < scale_y; ++phase) {
            uint16_t *drow =
                fb + (dst_y + sy * (int)scale_y + (int)phase) * FB16_WIDTH + dst_x;
            if (effect_scanline_blank(phase, scale_y, scanline_mode) != 0U) {
                memset(drow, 0, row_pixels * sizeof(uint16_t));
            } else {
                memcpy(drow, s_effect_2x_row, row_pixels * sizeof(uint16_t));
            }
        }
    }
}

/* ---------- Format badge ----------
 *
 * CPU1 derives this from the exact frame it rendered and publishes it
 * atomically with the framebuffer slot. CPU0 must never inspect CPU1's
 * cached main/AUX shadows: invalidating CPU0's cache cannot make CPU1's
 * dirty lines reach DDR, so that old scheme reported stale HGR/SHR labels. */
static const char *format_badge_page_suffix(uint32_t detail)
{
    switch ((detail & APPLE_FB_FORMAT_PAGE_MASK) >>
            APPLE_FB_FORMAT_PAGE_SHIFT) {
    case APPLE_FB_FORMAT_PAGE_INTERLACE:  return "i";
    case APPLE_FB_FORMAT_PAGE_FLIP_MERGE: return "p";
    default:                               return "";
    }
}

static const char *format_badge_shr4_selector(uint32_t detail)
{
    const uint32_t selectors =
        (detail & APPLE_FB_FORMAT_SELECTORS_MASK) >>
        APPLE_FB_FORMAT_SELECTORS_SHIFT;

    switch (selectors) {
    case APPLE_FB_FORMAT_SELECTOR_STD:    return "STD";
    case APPLE_FB_FORMAT_SELECTOR_RGGB:   return "RGGB";
    case APPLE_FB_FORMAT_SELECTOR_PAL256: return "PAL256";
    case APPLE_FB_FORMAT_SELECTOR_R4G4B4: return "R4G4B4";
    default:                               return "MIXED";
    }
}

static const char *format_badge_label(void)
{
    static char label[32];
    const uint32_t detail = apple_fb_reader_format_detail();
    const uint32_t base = detail & APPLE_FB_FORMAT_BASE_MASK;
    const char *suffix = format_badge_page_suffix(detail);
    const char *name = NULL;
    const char *geometry = NULL;

    switch (base) {
    case APPLE_FB_FORMAT_TEXT: name = "TEXT"; break;
    case APPLE_FB_FORMAT_GR:   name = "GR";   break;
    case APPLE_FB_FORMAT_DGR:  name = "DGR";  break;
    case APPLE_FB_FORMAT_HGR:  name = "HGR";  break;
    case APPLE_FB_FORMAT_DHGR: name = "DHGR"; break;
    case APPLE_FB_FORMAT_SHR_320: name = "SHR 320"; break;
    case APPLE_FB_FORMAT_SHR_640: name = "SHR 640"; break;
    case APPLE_FB_FORMAT_SHR_MIXED: name = "SHR MIXED"; break;
    case APPLE_FB_FORMAT_SHR4_320: geometry = "320"; break;
    case APPLE_FB_FORMAT_SHR4_640: geometry = "640"; break;
    case APPLE_FB_FORMAT_SHR4_MIXED: geometry = "MIXED"; break;
    case APPLE_FB_FORMAT_SHR3200_320: name = "SHR-3200 320"; break;
    case APPLE_FB_FORMAT_SHR3200_640: name = "SHR-3200 640"; break;
    case APPLE_FB_FORMAT_SHR3200_MIXED: name = "SHR-3200 MIXED"; break;
    case APPLE_FB_FORMAT_SHR_EXT_MIXED: name = "SHR EXT MIXED"; break;
    default:
        name = (g_compositor_last_apple_mode == APPLE_FB_DISPLAY_MODE_SHR) ?
               "SHR ?" : "VIDEO ?";
        break;
    }

    if (geometry != NULL) {
        snprintf(label, sizeof(label), "SHR4 %s %s%s",
                 format_badge_shr4_selector(detail), geometry, suffix);
    } else {
        snprintf(label, sizeof(label), "%s%s", name, suffix);
    }
    return label;
}

static void draw_format_badge(uint16_t *fb, int x, int y, int w_px)
{
    const char *label = format_badge_label();
    const int scale = 2;
    const int text_w =
        (int)strlen(label) * FB16_BUILTIN_FONT_ADVANCE_X * scale;
    const int badge_w = text_w + 16;
    const int badge_h = 8 * scale + 10;
    const int bx = x + w_px - badge_w - 10;
    const int by = y + 10;

    fb16_fill_rect(fb, bx, by, badge_w, badge_h, FB16_RGB(0x10, 0x14, 0x1C));
    fb16_rect(fb, bx, by, badge_w, badge_h, FB16_RGB(0x34, 0x48, 0x5C));
    fb16_string_scaled_xy(fb, bx + 8, by + 5, label,
                          FB16_RGB(0x62, 0xD8, 0xE8),
                          FB16_RGB(0x10, 0x14, 0x1C), scale, scale);
}

/* Nonzero when the Apple subwindow must route through the effects blit
 * (ghosting history blend and/or phosphor blur) instead of the fast
 * fb16 NEON path. */
static inline int compositor_apple_effects_active(void)
{
    return (s_video_ghosting_strength != APPLETINI_VIDEO_GHOSTING_OFF) ||
           (s_video_blur_strength != APPLETINI_VIDEO_BLUR_OFF) ||
           (s_video_glow_strength != APPLETINI_VIDEO_GLOW_OFF);
}

/* ---------- Slot picking ---------- */

static uint8_t pick_safe_slot(uint8_t avoid_a, uint8_t avoid_b)
{
    /* Return any slot in [0, COMP_OUT_SLOT_COUNT) that does not equal
     * avoid_a or avoid_b. With 3 slots and at most 2 unsafe, exactly
     * one or two slots are safe; we pick the lowest-numbered. */
    for (uint8_t i = 0u; i < COMP_OUT_SLOT_COUNT; ++i) {
        if (i != avoid_a && i != avoid_b) {
            return i;
        }
    }
    return 0u;  /* unreachable when COMP_OUT_SLOT_COUNT >= 3 */
}

static uint8_t fb_last_latched_slot(void)
{
    uint32_t addr = REG_READ(FB_LAST_LATCHED_REG);
    return comp_out_addr_to_slot(addr);   /* 0xFF if no match */
}

/* ---------- DDR setup ---------- */

static void fb_mark_noncached(uintptr_t base, uint32_t size)
{
    const uint32_t section_size = 0x00100000U;
    const uint32_t bytes = (size + section_size - 1U) & ~(section_size - 1U);
    for (uint32_t off = 0; off < bytes; off += section_size) {
        Xil_SetTlbAttributes(base + off, NORM_NONCACHE);
    }
}

/* ---------- Apple subwindow ---------- */

static void fill_border_flood_rect(uint16_t *fb,
                                   int x,
                                   int y,
                                   int w,
                                   int h,
                                   uint16_t color,
                                   uint8_t scanline_mode)
{
    if (scanline_mode == APPLETINI_SCANLINES_OFF) {
        fb16_fill_rect(fb, x, y, w, h, color);
        return;
    }

    for (int row = 0; row < h; ++row) {
        const uint8_t phase = (uint8_t)((uint32_t)
            (y + row - (int)COMP_BORDER_Y_OFF) & 3U);
        const uint16_t row_color =
            (effect_scanline_blank(phase, 4U, scanline_mode) != 0U) ? 0U : color;

        fb16_fill_rect(fb, x, y + row, w, 1, row_color);
    }
}

static void draw_border_flood(uint16_t *fb,
                              uint16_t color,
                              uint8_t scanline_mode)
{
    scanline_mode = appletini_scanlines_clamp(scanline_mode);
    fill_border_flood_rect(fb, 0, 0,
                           (int)COMP_OUT_WIDTH, (int)COMP_BORDER_Y_OFF,
                           color, scanline_mode);
    fill_border_flood_rect(
        fb, 0, (int)(COMP_BORDER_Y_OFF + COMP_BORDER_HEIGHT),
        (int)COMP_OUT_WIDTH,
        (int)(COMP_OUT_HEIGHT - COMP_BORDER_Y_OFF - COMP_BORDER_HEIGHT),
        color, scanline_mode);
    fill_border_flood_rect(fb, 0, (int)COMP_BORDER_Y_OFF,
                           (int)COMP_BORDER_X_OFF, (int)COMP_BORDER_HEIGHT,
                           color, scanline_mode);
    fill_border_flood_rect(
        fb, (int)(COMP_BORDER_X_OFF + COMP_BORDER_WIDTH),
        (int)COMP_BORDER_Y_OFF,
        (int)(COMP_OUT_WIDTH - COMP_BORDER_X_OFF - COMP_BORDER_WIDTH),
        (int)COMP_BORDER_HEIGHT, color, scanline_mode);
}

static int draw_apple_subwindow(uint16_t *fb)
{
    /* Atomically claim the freshest Apple frame the renderer has
     * published. If no frame has been published yet, skip the
     * subwindow blit -- the previous compositor pass painted
     * whatever UI is in this region. If the renderer hasn't produced
     * anything new since our last claim, we keep our current slot
     * and re-blit it (which is what we want when DVI is faster than
     * the renderer: hold the last good frame). */
    uint8_t slot = apple_fb_reader_claim();
    if (slot == APPLE_FB_NO_SLOT) {
        return 0;
    }
    const uint32_t display_mode = apple_fb_reader_display_mode();
    const uint8_t border_color = apple_fb_reader_border_color();
    g_compositor_last_apple_slot = slot;
    g_compositor_last_apple_mode = display_mode;

    /* Slot is non-cacheable on both cores (CPU1 writer side in
     * apple_cycle_renderer_init, CPU0 reader side in
     * compositor_init below). Reads here go straight to DDR --
     * slower than cached reads but small at 25 MB/s and we avoid
     * the L1+L2 invalidate-on-every-blit cost which on CPU0
     * (with full BSP, no USE_AMP) walks the L2 line-by-line and
     * stalled the main loop to ~25 sec/iter. */
    /* Skip the leading invisible guard pixels. The renderer also keeps
     * right-side guard pixels in the row stride so shifted composite
     * writes at the right edge cannot spill into the next row. */
    const uint32_t *src_base =
        (const uint32_t *)(uintptr_t)comp_apple_slot_addr[slot];
    if (display_mode == APPLE_FB_DISPLAY_MODE_SHR) {
        if (compositor_apple_effects_active()) {
            blit_apple_ghosting_2x(fb,
                                   (int)COMP_SUBWIN_SHR_X_OFF,
                                   (int)COMP_SUBWIN_SHR_Y_OFF,
                                   src_base,
                                   (int)COMP_APPLE_SHR_WIDTH,
                                   (int)COMP_APPLE_SHR_HEIGHT,
                                   (int)COMP_APPLE_SHR_ROW_PIXELS,
                                   2U,
                                   s_scanlines_mode,
                                   s_video_ghosting_strength,
                                   s_video_blur_strength,
                                   s_video_glow_strength);
        } else {
            fb16_blit_2x2_scanlines(fb,
                                    (int)COMP_SUBWIN_SHR_X_OFF,
                                    (int)COMP_SUBWIN_SHR_Y_OFF,
                                    src_base,
                                    (int)COMP_APPLE_SHR_WIDTH,
                                    (int)COMP_APPLE_SHR_HEIGHT,
                                    (int)COMP_APPLE_SHR_ROW_PIXELS,
                                    s_scanlines_mode);
        }
        if (s_format_badge_enabled != 0u) {
            draw_format_badge(fb, (int)COMP_SUBWIN_SHR_X_OFF,
                              (int)COMP_SUBWIN_SHR_Y_OFF,
                              (int)(COMP_APPLE_SHR_WIDTH * 2u));
        }
        return 1;
    }

    if (display_mode == APPLE_FB_DISPLAY_MODE_LEGACY_I) {
        /* Woven 384-row frame at the legacy stride, starting at slot
         * row 0 with no border data: blit 2x2 into the same rect the
         * legacy 2x4 path fills. The IIgs border is unavailable while
         * interlaced. */
        const uint32_t *srcw = src_base + COMP_APPLE_ACTIVE_X;

        if (compositor_apple_effects_active()) {
            blit_apple_ghosting_2x(fb,
                                   (int)COMP_SUBWIN_X_OFF,
                                   (int)COMP_SUBWIN_Y_OFF,
                                   srcw,
                                   (int)COMP_APPLE_WIDTH,
                                   384,
                                   (int)COMP_APPLE_ROW_PIXELS,
                                   2U,
                                   s_scanlines_mode,
                                   s_video_ghosting_strength,
                                   s_video_blur_strength,
                                   s_video_glow_strength);
        } else {
            fb16_blit_2x2_scanlines(fb,
                                    (int)COMP_SUBWIN_X_OFF,
                                    (int)COMP_SUBWIN_Y_OFF,
                                    srcw,
                                    (int)COMP_APPLE_WIDTH,
                                    384,
                                    (int)COMP_APPLE_ROW_PIXELS,
                                    s_scanlines_mode);
        }
        if (s_format_badge_enabled != 0u) {
            draw_format_badge(fb, (int)COMP_SUBWIN_X_OFF,
                              (int)COMP_SUBWIN_Y_OFF,
                              (int)(COMP_APPLE_WIDTH * 2u));
        }
        return 1;
    }

    const uint32_t *src;
    int dst_x;
    int dst_y;
    int src_w;
    int src_h;

    if (s_border_enabled != 0u) {
        if (s_border_flood != 0u) {
            draw_border_flood(fb,
                              fb16_from_bgra32(
                                  apple_video_iigs_border_bgra(border_color)),
                              s_scanlines_mode);
        }
        src = src_base + COMP_APPLE_LEFT_BORDER_PIXELS;
        dst_x = (int)COMP_BORDER_X_OFF;
        dst_y = (int)COMP_BORDER_Y_OFF;
        src_w = (int)COMP_APPLE_VISIBLE_WIDTH;
        src_h = (int)COMP_APPLE_VISIBLE_HEIGHT;
    } else {
        src = src_base + (COMP_APPLE_ACTIVE_Y * COMP_APPLE_ROW_PIXELS) +
              COMP_APPLE_ACTIVE_X;
        dst_x = (int)COMP_SUBWIN_X_OFF;
        dst_y = (int)COMP_SUBWIN_Y_OFF;
        src_w = (int)COMP_APPLE_WIDTH;
        src_h = (int)COMP_APPLE_HEIGHT;
    }
    if (compositor_apple_effects_active()) {
        blit_apple_ghosting_2x(fb,
                               dst_x,
                               dst_y,
                               src,
                               src_w,
                               src_h,
                               (int)COMP_APPLE_ROW_PIXELS,
                               4U,
                               s_scanlines_mode,
                               s_video_ghosting_strength,
                               s_video_blur_strength,
                               s_video_glow_strength);
    } else {
        fb16_blit_2x4_scanlines(fb,
                                dst_x,
                                dst_y,
                                src,
                                src_w,
                                src_h,
                                (int)COMP_APPLE_ROW_PIXELS,
                                s_scanlines_mode);
    }
    if (s_format_badge_enabled != 0u) {
        draw_format_badge(fb, dst_x, dst_y, src_w * 2);
    }
    return 1;
}

/* ---------- SuperSprite (TMS9918) overlay ---------- */

/* Render the software VDP and black-key it over the Apple subwindow: only
 * non-black VDP pixels are written, so the Apple video shows through where the
 * VDP is transparent/black (the SuperSprite's black-level overlay). The
 * 256x192 VDP image is nearest-neighbor scaled into the subwindow rect. */
static void draw_supersprite_overlay(uint16_t *fb)
{
    const ss_vdp_frame_t *f = supersprite_vdp_render();
    if (f == NULL || !f->active || f->pixels == NULL) {
        return;
    }

    for (int sy = 0; sy < SS_VDP_HEIGHT; ++sy) {
        const int oy0 = (int)COMP_SUBWIN_Y_OFF +
            (sy * (int)COMP_SUBWIN_HEIGHT) / SS_VDP_HEIGHT;
        const int oy1 = (int)COMP_SUBWIN_Y_OFF +
            ((sy + 1) * (int)COMP_SUBWIN_HEIGHT) / SS_VDP_HEIGHT;
        const uint32_t *srow = f->pixels + sy * SS_VDP_WIDTH;
        for (int sx = 0; sx < SS_VDP_WIDTH; ++sx) {
            const uint32_t p = srow[sx];
            if ((p & 0x00FFFFFFu) == 0u) {
                continue;  /* black => transparent, keep Apple pixel */
            }
            const uint16_t p565 = fb16_from_bgra32(p);
            const int ox0 = (int)COMP_SUBWIN_X_OFF +
                (sx * (int)COMP_SUBWIN_WIDTH) / SS_VDP_WIDTH;
            const int ox1 = (int)COMP_SUBWIN_X_OFF +
                ((sx + 1) * (int)COMP_SUBWIN_WIDTH) / SS_VDP_WIDTH;
            for (int oy = oy0; oy < oy1; ++oy) {
                uint16_t *drow = fb + oy * FB16_WIDTH;
                for (int ox = ox0; ox < ox1; ++ox) {
                    drow[ox] = p565;
                }
            }
        }
    }
}

/* ---------- Public API ---------- */

void compositor_init(compositor_ui_draw_fn draw_fn)
{
    /* USB/SD DMA and full-menu repaints share DDR bandwidth with HP0 scanout.
     * The full-screen UI throttle limits repaint bursts to 30 Hz. */

    /* Mark every output slot non-cacheable so fb_reader (AXI HP0)
     * sees PS writes immediately. The slots aren't necessarily
     * contiguous (slot 2 lives past 0x3F800000), so mark each one
     * with its own MMU section walk. */
    for (uint32_t i = 0u; i < COMP_OUT_SLOT_COUNT; ++i) {
        fb_mark_noncached(comp_out_slot_addr[i], COMP_OUT_BYTES);
        memset((void *)(uintptr_t)comp_out_slot_addr[i], 0, COMP_OUT_BYTES);
    }

    /* Apple FB slots: non-cacheable on BOTH cores. CPU1 writes
     * uncached (set up in apple_cycle_renderer_init), CPU0 reads
     * uncached (here). 25 MB/s of pixel data at 60 Hz; even at
     * uncached read cost (~50 ns each) the blit's ~107 K reads
     * total ~5 ms and remains bounded. */
    for (uint32_t i = 0u; i < COMP_APPLE_SLOT_COUNT; ++i) {
        fb_mark_noncached(comp_apple_slot_addr[i], COMP_APPLE_BYTES);
    }

    __asm__ volatile ("dsb sy");

    s_ui_draw   = draw_fn;
    s_ui_state  = NULL;
    s_ui_config = NULL;

    /* Round-trip the FB control registers to sanity-check the AXI
     * register decode is wired correctly. If the bitstream is stale
     * or the register offsets are wrong, the read-back doesn't
     * match the write and the boot UART log makes that visible
     * before we spend cycles staring at a garbled screen.
     *
     * Use a benign magic value (0x3E000000 = comp_out_slot_addr[0])
     * so even if the read-back fails or fb_reader latches this
     * value before compositor_tick() fires, scanout points at our
     * own slot 0 (which we just memset to zero -> visible black,
     * not random PS RAM). */
    {
        uint32_t magic = comp_out_slot_addr[0];
        REG_WRITE(FB_BASE_ADDR_REG, magic);
        uint32_t back = REG_READ(FB_BASE_ADDR_REG);
        char line[96];
        snprintf(line, sizeof(line),
                 "compositor: FB_BASE_ADDR_REG round-trip: wrote 0x%08lX, "
                 "read 0x%08lX %s\r\n",
                 (unsigned long)magic, (unsigned long)back,
                 (back == magic) ? "[OK]" : "[FAIL: stale PL?]");
        uart_puts(UART0_BASE, line);

    }

    /* The diagnostic round-trip above already published slot 0
     * (FB_BASE_ADDR_REG = comp_out_slot_addr[0]). Treat that as the
     * compositor's first publish so compositor_tick() avoids drawing
     * into slot 0 while PL might be scanning it. The counter check
     * gates the next compose on FB_STATUS_REG advancing -- the first
     * tick will wait until PL latches the init-time publish, then
     * pick a different slot to draw into. */
    s_writer_idx        = 0u;
    s_published_idx     = 0u;
    s_composited_apple_seq = 0u;
    s_published_counter = REG_READ(FB_STATUS_REG);

    g_compositor_frames_published   = 0u;
    g_compositor_frames_skipped     = 0u;
    g_compositor_apple_frames_drawn = 0u;
    g_compositor_last_apple_slot    = 0xFFu;
    g_compositor_last_apple_mode    = APPLE_FB_DISPLAY_MODE_LEGACY;
    g_compositor_last_ui_us         = 0u;
    g_compositor_last_apple_us      = 0u;
    g_compositor_last_sync_us       = 0u;
    g_compositor_last_total_us      = 0u;
    g_compositor_last_apple_drawn   = 0u;
    g_compositor_last_suppress_apple = 0u;

    uart_puts(UART0_BASE,
        "compositor_init: 3-slot output FB ring at 0x3E000000/0x3E400000/"
        "0x3E800000, 1920x1080 RGB565\r\n");

    /* Report which Apple-subwindow blit path the frontend was compiled
     * with. fb16.c shares these flags, so this is a faithful proxy for
     * whether its NEON burst-read path is active. If this says "scalar"
     * but you expected NEON, the -mfpu=neon flag didn't reach the build. */
#if defined(__ARM_NEON)
    uart_puts(UART0_BASE, "compositor: Apple blit = NEON (vld1q/vzipq burst reads)\r\n");
#else
    uart_puts(UART0_BASE, "compositor: Apple blit = scalar (no __ARM_NEON; build -mfpu=neon)\r\n");
#endif
}

void compositor_set_draw_context(const void *ui_state, const void *config_menu)
{
    s_ui_state  = ui_state;
    s_ui_config = config_menu;
}

void compositor_set_scanlines(uint8_t mode)
{
    s_scanlines_mode = appletini_scanlines_clamp(mode);
}

void compositor_set_video_ghosting(uint8_t strength)
{
    strength = appletini_video_ghosting_clamp(strength);
    if (s_video_ghosting_strength != strength) {
        s_video_ghosting_strength = strength;
        effect_clear_history();
    }
    s_force_full_refresh = 1u;
}

uint8_t compositor_video_ghosting(void)
{
    return s_video_ghosting_strength;
}

void compositor_set_video_blur(uint8_t strength)
{
    strength = appletini_video_blur_clamp(strength);
    s_video_blur_strength = strength;
    /* Display-only: no history reset needed, but the subwindow must be
     * recomposited so a strength change lands without waiting for the
     * next fresh Apple frame. */
    s_force_full_refresh = 1u;
}

uint8_t compositor_video_blur(void)
{
    return s_video_blur_strength;
}

void compositor_set_video_glow(uint8_t strength)
{
    strength = appletini_video_glow_clamp(strength);
    s_video_glow_strength = strength;
    /* Display-only, like blur: recomposite so the change lands without
     * waiting for the next fresh Apple frame. */
    s_force_full_refresh = 1u;
}

uint8_t compositor_video_glow(void)
{
    return s_video_glow_strength;
}

void compositor_set_format_badge(uint8_t enabled)
{
    s_format_badge_enabled = (enabled != 0u) ? 1u : 0u;
    s_force_full_refresh = 1u;
}

uint8_t compositor_format_badge(void)
{
    return s_format_badge_enabled;
}

void compositor_set_border(uint8_t enabled, uint8_t flood)
{
    enabled = (enabled != 0u) ? 1u : 0u;
    flood = (flood != 0u) ? 1u : 0u;
    if (s_border_enabled != enabled || s_border_flood != flood) {
        s_border_enabled = enabled;
        s_border_flood = flood;
        effect_clear_history();
        s_force_full_refresh = 1u;
    }
}

void compositor_set_paused(uint8_t paused)
{
    const uint8_t next = (paused != 0U) ? 1U : 0U;

    if (s_paused == next) {
        return;
    }

    s_paused = next;
    if (s_paused == 0U) {
        s_force_full_refresh = 1u;
    }
}

const uint16_t *compositor_latched_framebuffer(uint8_t *slot_out)
{
    uint8_t slot = fb_last_latched_slot();

    if (slot >= COMP_OUT_SLOT_COUNT) {
        if (s_published_idx < COMP_OUT_SLOT_COUNT) {
            slot = s_published_idx;
        } else if (s_writer_idx < COMP_OUT_SLOT_COUNT) {
            slot = s_writer_idx;
        } else {
            return NULL;
        }
    }

    if (slot_out != NULL) {
        *slot_out = slot;
    }
    return (const uint16_t *)(uintptr_t)comp_out_slot_addr[slot];
}

void compositor_request_full_refresh(void)
{
    s_force_full_refresh = 1u;
}

uint8_t compositor_full_refresh_active(void)
{
    return (s_force_full_refresh != 0u) ? 1u : 0u;
}

void compositor_set_uncapped(uint8_t uncapped)
{
    s_uncapped = (uncapped != 0U) ? 1U : 0U;
}

uint8_t compositor_uncapped(void)
{
    return s_uncapped;
}

int compositor_tick(void)
{
    if (s_paused != 0u) {
        return 0;
    }

    /* UI-only updates wait for FB_STATUS_REG to confirm the prior publish.
     * A fresh Apple frame may supersede a not-yet-latched output frame, which
     * preserves slot ownership while giving it a chance to catch the next
     * vblank. */
    const uint32_t now = REG_READ(FB_STATUS_REG);
    const int prior_publish_latched =
        (s_published_idx == 0xFFu) ||
        ((int32_t)(now - s_published_counter) > 0);
    const uint32_t apple_seq = apple_fb_reader_publish_seq();
    const int fresh_apple_frame =
        (apple_seq != 0u) && (apple_seq != s_composited_apple_seq);
    const int force_full_refresh = (s_force_full_refresh != 0u);

    if (s_published_idx != 0xFFu && s_uncapped == 0u) {
        if (!prior_publish_latched && !fresh_apple_frame &&
            !force_full_refresh) {
            return 0;
        }
    }

    /* Full-screen-UI throttle: when the previous composite suppressed
     * the Apple subwindow (config menu / boot menu owns
     * the screen), halve the repaint rate to ~30 Hz. These modes fully
     * repaint every composite (~10 MB/frame of uncached writes), and
     * that traffic stacked on USB0 mass-storage DMA is what pushed DDR
     * arbitration into scanout underruns (blue flashes in the menu).
     * A settings menu doesn't need 60 Hz; input latency worst-case is
     * one throttle period. Full refreshes and uncapped measurement
     * bypass the throttle. */
    if (g_compositor_last_suppress_apple != 0u &&
        s_uncapped == 0u && !force_full_refresh) {
        static XTime s_last_fullscreen_ui_compose;
        XTime tnow;

        XTime_GetTime(&tnow);
        if (s_last_fullscreen_ui_compose != 0U &&
            (tnow - s_last_fullscreen_ui_compose) <
                ((XTime)COUNTS_PER_SECOND / 30U)) {
            return 0;
        }
        s_last_fullscreen_ui_compose = tnow;
    }

    XTime total_start = 0U;
    XTime ui_start = 0U;
    XTime ui_base_end = 0U;
    XTime ui_overlay_start = 0U;
    XTime ui_end = 0U;
    XTime apple_start = 0U;
    XTime apple_end = 0U;
    XTime sync_start = 0U;
    XTime sync_end = 0U;
    XTime total_end = 0U;
    XTime_GetTime(&total_start);

    /* Pick a slot that is neither published-but-unlatched nor
     * currently scanning. */
    uint8_t latched = fb_last_latched_slot();    /* 0xFF if unrecognized */
    s_writer_idx = pick_safe_slot(latched, s_published_idx);

    uint16_t *fb =
        (uint16_t *)(uintptr_t)comp_out_slot_addr[s_writer_idx];

    /* Paint the static background first. The base callback returns non-zero
     * when the boot/config menu owns the whole screen and the Apple subwindow
     * must be suppressed. */
    int suppress_apple = 0;
    int apple_drawn = 0;
    XTime_GetTime(&ui_start);
    if (s_ui_draw != NULL) {
        suppress_apple = s_ui_draw(fb,
                                   s_ui_state,
                                   s_ui_config,
                                   COMPOSITOR_UI_PHASE_BASE);
    } else {
        fb16_clear(fb, FB16_COLOR_BLACK);
    }
    XTime_GetTime(&ui_base_end);
    if (suppress_apple) {
        s_composited_apple_seq = apple_seq;
    } else {
        XTime_GetTime(&apple_start);
        apple_drawn = draw_apple_subwindow(fb);
        draw_supersprite_overlay(fb);
        XTime_GetTime(&apple_end);
    }

    /* Foreground UI is always last so status text, the disk-access view, and
     * menus cannot be covered by the Apple border or its outer flood. */
    XTime_GetTime(&ui_overlay_start);
    if (s_ui_draw != NULL) {
        (void)s_ui_draw(fb,
                        s_ui_state,
                        s_ui_config,
                        COMPOSITOR_UI_PHASE_OVERLAY);
    }
    XTime_GetTime(&ui_end);
    if (apple_drawn) {
        g_compositor_apple_frames_drawn++;
        s_composited_apple_seq = apple_seq;
    }

    /* Slots are non-cached; no flush needed. dsb makes sure all writes
     * retire in DDR before we tell the PL where to look. */
    XTime_GetTime(&sync_start);
    __asm__ volatile ("dsb sy");

    REG_WRITE(FB_BASE_ADDR_REG, comp_out_slot_addr[s_writer_idx]);
    XTime_GetTime(&sync_end);

    /* Counter we expect to see (or exceed) before the next publish:
     * the value we just observed plus one. We read FB_STATUS_REG
     * AFTER the base-addr write so any vblank that fires concurrently
     * with our write will already be reflected in the counter. */
    s_published_counter = REG_READ(FB_STATUS_REG);
    s_published_idx     = s_writer_idx;
    s_force_full_refresh = 0u;
    g_compositor_frames_published++;
    XTime_GetTime(&total_end);

    g_compositor_last_ui_us = ticks_to_us(
        (ui_base_end - ui_start) + (ui_end - ui_overlay_start));
    g_compositor_last_apple_us = ticks_to_us(apple_end - apple_start);
    g_compositor_last_sync_us = ticks_to_us(sync_end - sync_start);
    g_compositor_last_total_us = ticks_to_us(total_end - total_start);
    g_compositor_last_apple_drawn = (uint32_t)((apple_drawn != 0) ? 1U : 0U);
    g_compositor_last_suppress_apple =
        (uint32_t)((suppress_apple != 0) ? 1U : 0U);
    return 1;
}
