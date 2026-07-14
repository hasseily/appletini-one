/*
 * fb16.c -- See fb16.h.
 *
 * RGB565 drawing primitives. Takes an explicit framebuffer pointer so
 * the compositor can target whichever output slot it has chosen.
 *
 * Performance: 1920x1080 RGB565 = 4.15 MB/frame. Output slots live in
 * non-cacheable DDR (fb_reader on AXI HP0 must see PS writes
 * immediately), so every byte written here is an uncached store --
 * pixel width is the direct cost multiplier. fb16_clear of a full frame
 * is the worst-case write; the 2x expansion blits are the steady-state
 * cost. Both write 64-bit-wide whenever alignment allows.
 *
 * The 2x blits take BGRA32 sources (the Apple frame ring keeps the
 * renderer's 8:8:8 chroma precision) and narrow to 565 in-register
 * during the expansion, so the only precision drop in the whole
 * pipeline is at the final store -- the same bits the DVI pins carry.
 */

#include <string.h>
#include <stdint.h>

#include "fb16.h"

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#define FB16_BLIT_2X_MAX_SRC_W (FB16_WIDTH / 2)

/* Cacheable scratch for one 2x-expanded output row (src_w*2 565 pixels).
 * Sized for the widest supported source and 32-byte aligned so the NEON
 * 128-bit stores and the row memcpy stay aligned. */
static uint16_t s_blit_2x_row[FB16_BLIT_2X_MAX_SRC_W * 2]
    __attribute__((aligned(32)));

/* Expand one BGRA32 source row into `dst` with 2x horizontal pixel
 * doubling and BGRA32 -> 565 narrowing:
 * dst[2*i] == dst[2*i+1] == narrow(src[i]).
 *
 * `src` is the Apple FB slot, which lives in non-cacheable DDR. The
 * NEON path reads eight pixels per vld4 burst (which also deinterleaves
 * the B/G/R/A byte planes for free), packs 565 with two shift-inserts,
 * and doubles in-register with vzipq -- one uncached burst per eight
 * source pixels instead of eight scalar reads. Requires -mfpu=neon;
 * without it the scalar fallback runs -- correct, just slower. */
void fb16_expand_2x_row_bgra32src(uint16_t *dst, const uint32_t *src,
                                  int src_w)
{
    int x = 0;
#if defined(__ARM_NEON)
    for (; x + 8 <= src_w; x += 8) {
        /* val[0]=B, val[1]=G, val[2]=R, val[3]=A for 8 pixels. */
        const uint8x8x4_t p = vld4_u8((const uint8_t *)(src + x));
        /* r << 8, then insert g at bit 5, then b at bit 0:
         * [15:11]=r[7:3], [10:5]=g[7:2], [4:0]=b[7:3]. */
        uint16x8_t px = vshll_n_u8(p.val[2], 8);
        px = vsriq_n_u16(px, vshll_n_u8(p.val[1], 8), 5);
        px = vsriq_n_u16(px, vshll_n_u8(p.val[0], 8), 11);
        /* Double each pixel: {p0,p0,p1,p1,...}. */
        const uint16x8x2_t z = vzipq_u16(px, px);
        vst1q_u16(dst + 2 * x,     z.val[0]);
        vst1q_u16(dst + 2 * x + 8, z.val[1]);
    }
#endif
    for (; x < src_w; ++x) {
        const uint16_t v = fb16_from_bgra32(src[x]);
        dst[2 * x]     = v;
        dst[2 * x + 1] = v;
    }
}

/* ---------- Embedded 7x8 font (ASCII 32-127) ---------- */

static const uint8_t font7x8[96][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x08,0x08,0x08,0x08,0x08,0x00,0x08,0x00},
    {0x14,0x14,0x14,0x00,0x00,0x00,0x00,0x00},
    {0x14,0x14,0x3E,0x14,0x3E,0x14,0x14,0x00},
    {0x08,0x3C,0x0A,0x1C,0x28,0x1E,0x08,0x00},
    {0x06,0x26,0x10,0x08,0x04,0x32,0x30,0x00},
    {0x04,0x0A,0x0A,0x04,0x2A,0x12,0x2C,0x00},
    {0x08,0x08,0x08,0x00,0x00,0x00,0x00,0x00},
    {0x08,0x04,0x02,0x02,0x02,0x04,0x08,0x00},
    {0x08,0x10,0x20,0x20,0x20,0x10,0x08,0x00},
    {0x08,0x2A,0x1C,0x08,0x1C,0x2A,0x08,0x00},
    {0x00,0x08,0x08,0x3E,0x08,0x08,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x08,0x08,0x04,0x00},
    {0x00,0x00,0x00,0x3E,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x08,0x00},
    {0x00,0x20,0x10,0x08,0x04,0x02,0x00,0x00},
    {0x1C,0x22,0x32,0x2A,0x26,0x22,0x1C,0x00},
    {0x08,0x0C,0x08,0x08,0x08,0x08,0x1C,0x00},
    {0x1C,0x22,0x20,0x18,0x04,0x02,0x3E,0x00},
    {0x3E,0x20,0x10,0x18,0x20,0x22,0x1C,0x00},
    {0x10,0x18,0x14,0x12,0x3E,0x10,0x10,0x00},
    {0x3E,0x02,0x1E,0x20,0x20,0x22,0x1C,0x00},
    {0x38,0x04,0x02,0x1E,0x22,0x22,0x1C,0x00},
    {0x3E,0x20,0x10,0x08,0x04,0x04,0x04,0x00},
    {0x1C,0x22,0x22,0x1C,0x22,0x22,0x1C,0x00},
    {0x1C,0x22,0x22,0x3C,0x20,0x10,0x0E,0x00},
    {0x00,0x00,0x08,0x00,0x08,0x00,0x00,0x00},
    {0x00,0x00,0x08,0x00,0x08,0x08,0x04,0x00},
    {0x10,0x08,0x04,0x02,0x04,0x08,0x10,0x00},
    {0x00,0x00,0x3E,0x00,0x3E,0x00,0x00,0x00},
    {0x04,0x08,0x10,0x20,0x10,0x08,0x04,0x00},
    {0x1C,0x22,0x10,0x08,0x08,0x00,0x08,0x00},
    {0x1C,0x22,0x2A,0x3A,0x1A,0x02,0x3C,0x00},
    {0x08,0x14,0x22,0x22,0x3E,0x22,0x22,0x00},
    {0x1E,0x22,0x22,0x1E,0x22,0x22,0x1E,0x00},
    {0x1C,0x22,0x02,0x02,0x02,0x22,0x1C,0x00},
    {0x1E,0x22,0x22,0x22,0x22,0x22,0x1E,0x00},
    {0x3E,0x02,0x02,0x1E,0x02,0x02,0x3E,0x00},
    {0x3E,0x02,0x02,0x1E,0x02,0x02,0x02,0x00},
    {0x3C,0x02,0x02,0x02,0x32,0x22,0x3C,0x00},
    {0x22,0x22,0x22,0x3E,0x22,0x22,0x22,0x00},
    {0x1C,0x08,0x08,0x08,0x08,0x08,0x1C,0x00},
    {0x20,0x20,0x20,0x20,0x20,0x22,0x1C,0x00},
    {0x22,0x12,0x0A,0x06,0x0A,0x12,0x22,0x00},
    {0x02,0x02,0x02,0x02,0x02,0x02,0x3E,0x00},
    {0x22,0x36,0x2A,0x2A,0x22,0x22,0x22,0x00},
    {0x22,0x22,0x26,0x2A,0x32,0x22,0x22,0x00},
    {0x1C,0x22,0x22,0x22,0x22,0x22,0x1C,0x00},
    {0x1E,0x22,0x22,0x1E,0x02,0x02,0x02,0x00},
    {0x1C,0x22,0x22,0x22,0x2A,0x12,0x2C,0x00},
    {0x1E,0x22,0x22,0x1E,0x0A,0x12,0x22,0x00},
    {0x1C,0x22,0x02,0x1C,0x20,0x22,0x1C,0x00},
    {0x3E,0x08,0x08,0x08,0x08,0x08,0x08,0x00},
    {0x22,0x22,0x22,0x22,0x22,0x22,0x1C,0x00},
    {0x22,0x22,0x22,0x22,0x22,0x14,0x08,0x00},
    {0x22,0x22,0x22,0x2A,0x2A,0x36,0x22,0x00},
    {0x22,0x22,0x14,0x08,0x14,0x22,0x22,0x00},
    {0x22,0x22,0x14,0x08,0x08,0x08,0x08,0x00},
    {0x3E,0x20,0x10,0x08,0x04,0x02,0x3E,0x00},
    {0x3E,0x06,0x06,0x06,0x06,0x06,0x3E,0x00},
    {0x00,0x02,0x04,0x08,0x10,0x20,0x00,0x00},
    {0x3E,0x30,0x30,0x30,0x30,0x30,0x3E,0x00},
    {0x00,0x00,0x08,0x14,0x22,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x7F},
    {0x04,0x08,0x10,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x1C,0x20,0x3C,0x22,0x3C,0x00},
    {0x02,0x02,0x1E,0x22,0x22,0x22,0x1E,0x00},
    {0x00,0x00,0x3C,0x02,0x02,0x02,0x3C,0x00},
    {0x20,0x20,0x3C,0x22,0x22,0x22,0x3C,0x00},
    {0x00,0x00,0x1C,0x22,0x3E,0x02,0x3C,0x00},
    {0x18,0x24,0x04,0x1E,0x04,0x04,0x04,0x00},
    {0x00,0x00,0x1C,0x22,0x22,0x3C,0x20,0x1C},
    {0x02,0x02,0x1E,0x22,0x22,0x22,0x22,0x00},
    {0x08,0x00,0x0C,0x08,0x08,0x08,0x1C,0x00},
    {0x10,0x00,0x18,0x10,0x10,0x10,0x12,0x0C},
    {0x02,0x02,0x22,0x12,0x0E,0x12,0x22,0x00},
    {0x0C,0x08,0x08,0x08,0x08,0x08,0x1C,0x00},
    {0x00,0x00,0x36,0x2A,0x2A,0x2A,0x22,0x00},
    {0x00,0x00,0x1E,0x22,0x22,0x22,0x22,0x00},
    {0x00,0x00,0x1C,0x22,0x22,0x22,0x1C,0x00},
    {0x00,0x00,0x1E,0x22,0x22,0x1E,0x02,0x02},
    {0x00,0x00,0x3C,0x22,0x22,0x3C,0x20,0x20},
    {0x00,0x00,0x3A,0x06,0x02,0x02,0x02,0x00},
    {0x00,0x00,0x3C,0x02,0x1C,0x20,0x1E,0x00},
    {0x04,0x04,0x1E,0x04,0x04,0x24,0x18,0x00},
    {0x00,0x00,0x22,0x22,0x22,0x32,0x2C,0x00},
    {0x00,0x00,0x22,0x22,0x22,0x14,0x08,0x00},
    {0x00,0x00,0x22,0x22,0x2A,0x2A,0x36,0x00},
    {0x00,0x00,0x22,0x14,0x08,0x14,0x22,0x00},
    {0x00,0x00,0x22,0x22,0x22,0x3C,0x20,0x1C},
    {0x00,0x00,0x3E,0x10,0x08,0x04,0x3E,0x00},
    {0x38,0x0C,0x0C,0x06,0x0C,0x0C,0x38,0x00},
    {0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08},
    {0x0E,0x18,0x18,0x30,0x18,0x18,0x0E,0x00},
    {0x2C,0x1A,0x00,0x00,0x00,0x00,0x00,0x00},
};

/* ---------- Internal helpers ---------- */

static inline int clip_axis(int *coord, int *extent, int limit)
{
    if (*extent <= 0) return 0;
    if (*coord < 0) {
        *extent += *coord;
        *coord = 0;
    }
    if (*coord >= limit) return 0;
    if (*coord + *extent > limit) {
        *extent = limit - *coord;
    }
    return *extent > 0;
}

/* Replicate a 565 color across a 64-bit word (4 pixels). */
static inline uint64_t color_x4(uint16_t color)
{
    const uint32_t d = ((uint32_t)color << 16) | color;
    return ((uint64_t)d << 32) | d;
}

/* ---------- Solid fills ---------- */

void fb16_clear(uint16_t *fb, uint16_t color)
{
    if (color == 0u) {
        memset(fb, 0, (size_t)FB16_WIDTH * FB16_HEIGHT * FB16_BPP);
        return;
    }

    /* 64-bit quadrupled writes, unrolled 4x. Pointer-based iteration
     * (an index-based loop trips GCC's aggressive loop optimizer into
     * pointer-wraparound reasoning and a UB warning). */
    const uint64_t qword = color_x4(color);
    uint64_t *p64 = (uint64_t *)fb;
    uint64_t *end = p64 + ((size_t)FB16_WIDTH * (size_t)FB16_HEIGHT / 4u);

    for (; p64 + 4 <= end; p64 += 4) {
        p64[0] = qword;
        p64[1] = qword;
        p64[2] = qword;
        p64[3] = qword;
    }
    for (; p64 < end; ++p64) {
        *p64 = qword;
    }
}

void fb16_pixel(uint16_t *fb, int x, int y, uint16_t color)
{
    if ((unsigned)x < (unsigned)FB16_WIDTH &&
        (unsigned)y < (unsigned)FB16_HEIGHT) {
        fb[y * FB16_WIDTH + x] = color;
    }
}

void fb16_fill_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t color)
{
    if (!clip_axis(&x, &w, FB16_WIDTH))  return;
    if (!clip_axis(&y, &h, FB16_HEIGHT)) return;

    const uint64_t qword = color_x4(color);
    for (int row = 0; row < h; ++row) {
        uint16_t *p16 = &fb[(y + row) * FB16_WIDTH + x];
        int n = w;

        /* Align to 64-bit (up to three leading pixels). */
        while (((uintptr_t)p16 & 0x7u) != 0u && n > 0) {
            *p16++ = color;
            --n;
        }

        uint64_t *p64 = (uint64_t *)p16;
        int n64 = n >> 2;
        int i = 0;
        for (; i + 3 < n64; i += 4) {
            p64[i + 0] = qword;
            p64[i + 1] = qword;
            p64[i + 2] = qword;
            p64[i + 3] = qword;
        }
        for (; i < n64; ++i) {
            p64[i] = qword;
        }
        p16 = (uint16_t *)(p64 + n64);
        for (int t = n & 3; t > 0; --t) {
            *p16++ = color;
        }
    }
}

void fb16_hline(uint16_t *fb, int x, int y, int w, uint16_t color)
{
    fb16_fill_rect(fb, x, y, w, 1, color);
}

void fb16_vline(uint16_t *fb, int x, int y, int h, uint16_t color)
{
    fb16_fill_rect(fb, x, y, 1, h, color);
}

void fb16_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t color)
{
    fb16_hline(fb, x, y, w, color);
    fb16_hline(fb, x, y + h - 1, w, color);
    fb16_vline(fb, x, y, h, color);
    fb16_vline(fb, x + w - 1, y, h, color);
}

/* ---------- Lines and circles ---------- */

void fb16_line(uint16_t *fb, int x0, int y0, int x1, int y1, uint16_t color)
{
    int dx = x1 - x0;
    int dy = y1 - y0;
    int sx = (dx > 0) ? 1 : -1;
    int sy = (dy > 0) ? 1 : -1;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;

    int err = dx - dy;
    for (;;) {
        fb16_pixel(fb, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

void fb16_circle(uint16_t *fb, int cx, int cy, int r, uint16_t color)
{
    int x = r, y = 0, d = 1 - r;
    while (x >= y) {
        fb16_pixel(fb, cx + x, cy + y, color);
        fb16_pixel(fb, cx - x, cy + y, color);
        fb16_pixel(fb, cx + x, cy - y, color);
        fb16_pixel(fb, cx - x, cy - y, color);
        fb16_pixel(fb, cx + y, cy + x, color);
        fb16_pixel(fb, cx - y, cy + x, color);
        fb16_pixel(fb, cx + y, cy - x, color);
        fb16_pixel(fb, cx - y, cy - x, color);
        y++;
        if (d <= 0) {
            d += 2 * y + 1;
        } else {
            x--;
            d += 2 * (y - x) + 1;
        }
    }
}

void fb16_fill_circle(uint16_t *fb, int cx, int cy, int r, uint16_t color)
{
    int x = r, y = 0, d = 1 - r;
    while (x >= y) {
        fb16_hline(fb, cx - x, cy + y, 2 * x + 1, color);
        fb16_hline(fb, cx - x, cy - y, 2 * x + 1, color);
        fb16_hline(fb, cx - y, cy + x, 2 * y + 1, color);
        fb16_hline(fb, cx - y, cy - x, 2 * y + 1, color);
        y++;
        if (d <= 0) {
            d += 2 * y + 1;
        } else {
            x--;
            d += 2 * (y - x) + 1;
        }
    }
}

/* ---------- Text drawing ---------- */

void fb16_char(uint16_t *fb, int x, int y, char c, uint16_t fg, uint16_t bg)
{
    int idx = (c >= 32) ? (c - 32) : 0;
    const uint8_t *glyph = font7x8[idx];
    for (int row = 0; row < FB16_BUILTIN_FONT_HEIGHT; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < FB16_BUILTIN_FONT_DRAW_WIDTH; col++) {
            fb16_pixel(fb, x + col, y + row, (bits & (1 << col)) ? fg : bg);
        }
    }
}

void fb16_string(uint16_t *fb, int x, int y, const char *s, uint16_t fg, uint16_t bg)
{
    while (*s) {
        fb16_char(fb, x, y, *s++, fg, bg);
        x += FB16_BUILTIN_FONT_ADVANCE_X;
    }
}

void fb16_char_scaled(uint16_t *fb, int x, int y, char c, uint16_t fg, uint16_t bg, int scale)
{
    if (scale < 1) scale = 1;
    int idx = (c >= 32 && c <= 127) ? (c - 32) : 0;
    const uint8_t *glyph = font7x8[idx];
    for (int row = 0; row < FB16_BUILTIN_FONT_HEIGHT; row++) {
        const uint8_t bits = glyph[row];
        for (int col = 0; col < FB16_BUILTIN_FONT_DRAW_WIDTH; col++) {
            const uint16_t color = (bits & (1 << col)) ? fg : bg;
            for (int sy = 0; sy < scale; sy++) {
                for (int sx = 0; sx < scale; sx++) {
                    fb16_pixel(fb, x + col * scale + sx, y + row * scale + sy, color);
                }
            }
        }
    }
}

void fb16_char_scaled_xy(uint16_t *fb, int x, int y, char c, uint16_t fg, uint16_t bg, int scale_x, int scale_y)
{
    if (scale_x < 1) scale_x = 1;
    if (scale_y < 1) scale_y = 1;
    int idx = (c >= 32 && c <= 127) ? (c - 32) : 0;
    const uint8_t *glyph = font7x8[idx];
    for (int row = 0; row < FB16_BUILTIN_FONT_HEIGHT; row++) {
        const uint8_t bits = glyph[row];
        for (int col = 0; col < FB16_BUILTIN_FONT_DRAW_WIDTH; col++) {
            const uint16_t color = (bits & (1 << col)) ? fg : bg;
            for (int sy = 0; sy < scale_y; sy++) {
                for (int sx = 0; sx < scale_x; sx++) {
                    fb16_pixel(fb, x + col * scale_x + sx, y + row * scale_y + sy, color);
                }
            }
        }
    }
}

void fb16_string_scaled(uint16_t *fb, int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale)
{
    if (scale < 1) scale = 1;
    while (*s) {
        fb16_char_scaled(fb, x, y, *s++, fg, bg, scale);
        x += FB16_BUILTIN_FONT_ADVANCE_X * scale;
    }
}

void fb16_string_scaled_xy(uint16_t *fb, int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale_x, int scale_y)
{
    if (scale_x < 1) scale_x = 1;
    if (scale_y < 1) scale_y = 1;
    while (*s) {
        fb16_char_scaled_xy(fb, x, y, *s++, fg, bg, scale_x, scale_y);
        x += FB16_BUILTIN_FONT_ADVANCE_X * scale_x;
    }
}

void fb16_char_bitmap(uint16_t *fb, int x, int y, char c, const fb16_bitmap_font_t *font, uint16_t fg, uint16_t bg)
{
    if (font == NULL || font->data == NULL || font->bytes_per_glyph == 0U) {
        return;
    }

    uint32_t ch = (uint8_t)c;
    if (ch < font->first_char || ch > font->last_char) {
        ch = (uint8_t)'?';
        if (ch < font->first_char || ch > font->last_char) {
            ch = font->first_char;
        }
    }

    uint32_t glyph_idx = ch - font->first_char;
    const uint8_t *glyph = &font->data[glyph_idx * font->bytes_per_glyph];
    uint32_t row_bytes = (font->width + 7U) / 8U;

    for (uint32_t row = 0; row < font->height; row++) {
        for (uint32_t col = 0; col < font->width; col++) {
            const uint32_t byte_idx = row * row_bytes + (col >> 3U);
            const uint8_t bit_mask = (uint8_t)(0x80U >> (col & 7U));
            const uint16_t color = (glyph[byte_idx] & bit_mask) ? fg : bg;
            fb16_pixel(fb, x + (int)col, y + (int)row, color);
        }
    }
}

void fb16_string_bitmap(uint16_t *fb, int x, int y, const char *s, const fb16_bitmap_font_t *font, uint16_t fg, uint16_t bg)
{
    if (font == NULL) return;
    while (*s) {
        fb16_char_bitmap(fb, x, y, *s++, font, fg, bg);
        x += font->width;
    }
}

/* ---------- Blits ---------- */

/* Shared destination clip for the plain copy blits. Returns 0 when
 * nothing is visible; otherwise updates x/y, the source start offsets,
 * and the drawable extent. */
static int blit_clip(int *x, int *y, int w, int h,
                     int *src_x0, int *src_y0, int *draw_w, int *draw_h)
{
    *src_x0 = 0;
    *src_y0 = 0;
    *draw_w = w;
    *draw_h = h;

    if (*x < 0) { *src_x0 = -*x; *draw_w += *x; *x = 0; }
    if (*y < 0) { *src_y0 = -*y; *draw_h += *y; *y = 0; }
    if (*x >= FB16_WIDTH || *y >= FB16_HEIGHT) return 0;
    if (*x + *draw_w > FB16_WIDTH)  *draw_w = FB16_WIDTH  - *x;
    if (*y + *draw_h > FB16_HEIGHT) *draw_h = FB16_HEIGHT - *y;
    return (*draw_w > 0 && *draw_h > 0);
}

void fb16_blit_565(uint16_t *fb, int x, int y, int w, int h,
                   const uint16_t *src)
{
    int src_x0, src_y0, draw_w, draw_h;

    if (src == NULL || w <= 0 || h <= 0) return;
    if (!blit_clip(&x, &y, w, h, &src_x0, &src_y0, &draw_w, &draw_h)) return;

    for (int row = 0; row < draw_h; ++row) {
        uint16_t       *dst_row = &fb[(y + row) * FB16_WIDTH + x];
        const uint16_t *src_row = &src[(src_y0 + row) * w + src_x0];
        memcpy(dst_row, src_row, (size_t)draw_w * FB16_BPP);
    }
}

void fb16_blit_bgra32src(uint16_t *fb, int x, int y, int w, int h,
                         const uint32_t *src)
{
    int src_x0, src_y0, draw_w, draw_h;

    if (src == NULL || w <= 0 || h <= 0) return;
    if (!blit_clip(&x, &y, w, h, &src_x0, &src_y0, &draw_w, &draw_h)) return;

    for (int row = 0; row < draw_h; ++row) {
        uint16_t       *dst = &fb[(y + row) * FB16_WIDTH + x];
        const uint32_t *srp = &src[(src_y0 + row) * w + src_x0];
        int i = 0;
#if defined(__ARM_NEON)
        for (; i + 8 <= draw_w; i += 8) {
            const uint8x8x4_t p = vld4_u8((const uint8_t *)(srp + i));
            uint16x8_t px = vshll_n_u8(p.val[2], 8);
            px = vsriq_n_u16(px, vshll_n_u8(p.val[1], 8), 5);
            px = vsriq_n_u16(px, vshll_n_u8(p.val[0], 8), 11);
            vst1q_u16(dst + i, px);
        }
#endif
        for (; i < draw_w; ++i) {
            dst[i] = fb16_from_bgra32(srp[i]);
        }
    }
}

void fb16_blit_2x4(uint16_t *fb, int dst_x, int dst_y,
                   const uint32_t *src, int src_w, int src_h,
                   int src_stride)
{
    fb16_blit_2x4_scanlines(fb, dst_x, dst_y, src, src_w, src_h,
                            src_stride, 0U);
}

void fb16_blit_2x4_scanlines(uint16_t *fb, int dst_x, int dst_y,
                             const uint32_t *src, int src_w, int src_h,
                             int src_stride, uint8_t scanline_mode)
{
    if (src == NULL || src_w <= 0 || src_h <= 0) return;
    if (src_stride <= 0) src_stride = src_w;
    if (scanline_mode > 3U) scanline_mode = 0U;

    /* Caller is responsible for ensuring the destination rect (src_w*2 by
     * src_h*4) fits inside the framebuffer; this primitive does not
     * clip. The compositor's Apple subwindow is fixed-geometry inside
     * 1920x1080, so no clipping is required here. */
    for (int sy = 0; sy < src_h; ++sy) {
        const uint32_t *srow = src + sy * src_stride;
        uint16_t       *drow0 = fb + (dst_y + sy * 4) * FB16_WIDTH + dst_x;
        uint16_t       *drow1 = drow0 + FB16_WIDTH;
        uint16_t       *drow2 = drow0 + 2 * FB16_WIDTH;
        uint16_t       *drow3 = drow0 + 3 * FB16_WIDTH;

        const size_t row_bytes = (size_t)src_w * 2u * FB16_BPP;

        /* Expand + narrow once into cacheable scratch, then copy that
         * scratch to each active output row. Output slots are noncached
         * DDR; keeping destination writes row-contiguous gives the
         * store buffer a far better pattern than interleaving four
         * distant rows. */
        if (src_w <= FB16_BLIT_2X_MAX_SRC_W) {
            fb16_expand_2x_row_bgra32src(s_blit_2x_row, srow, src_w);

            memcpy(drow0, s_blit_2x_row, row_bytes);
            if (scanline_mode >= 3U) {
                memset(drow1, 0, row_bytes);
            } else {
                memcpy(drow1, s_blit_2x_row, row_bytes);
            }
            if (scanline_mode >= 2U) {
                memset(drow2, 0, row_bytes);
            } else {
                memcpy(drow2, s_blit_2x_row, row_bytes);
            }
            if (scanline_mode >= 1U) {
                memset(drow3, 0, row_bytes);
            } else {
                memcpy(drow3, s_blit_2x_row, row_bytes);
            }
        } else {
            for (int x = 0; x < src_w; ++x) {
                const uint16_t v = fb16_from_bgra32(srow[x]);
                const uint32_t vv = ((uint32_t)v << 16) | v;
                ((uint32_t *)drow0)[x] = vv;
                if (scanline_mode < 3U) {
                    ((uint32_t *)drow1)[x] = vv;
                }
                if (scanline_mode < 2U) {
                    ((uint32_t *)drow2)[x] = vv;
                }
                if (scanline_mode < 1U) {
                    ((uint32_t *)drow3)[x] = vv;
                }
            }
            if (scanline_mode >= 3U) {
                memset(drow1, 0, row_bytes);
            }
            if (scanline_mode >= 2U) {
                memset(drow2, 0, row_bytes);
            }
            if (scanline_mode >= 1U) {
                memset(drow3, 0, row_bytes);
            }
        }
    }
}

void fb16_blit_2x2_scanlines(uint16_t *fb, int dst_x, int dst_y,
                             const uint32_t *src, int src_w, int src_h,
                             int src_stride, uint8_t scanline_mode)
{
    if (src == NULL || src_w <= 0 || src_h <= 0) return;
    if (src_stride <= 0) src_stride = src_w;
    if (scanline_mode > 3U) scanline_mode = 0U;

    for (int sy = 0; sy < src_h; ++sy) {
        const uint32_t *srow = src + sy * src_stride;
        uint16_t       *drow0 = fb + (dst_y + sy * 2) * FB16_WIDTH + dst_x;
        uint16_t       *drow1 = drow0 + FB16_WIDTH;

        const size_t row_bytes = (size_t)src_w * 2u * FB16_BPP;

        if (src_w <= FB16_BLIT_2X_MAX_SRC_W) {
            fb16_expand_2x_row_bgra32src(s_blit_2x_row, srow, src_w);
            memcpy(drow0, s_blit_2x_row, row_bytes);
            if (scanline_mode >= 2U) {
                memset(drow1, 0, row_bytes);
            } else {
                memcpy(drow1, s_blit_2x_row, row_bytes);
            }
        } else {
            for (int x = 0; x < src_w; ++x) {
                const uint16_t v = fb16_from_bgra32(srow[x]);
                const uint32_t vv = ((uint32_t)v << 16) | v;
                ((uint32_t *)drow0)[x] = vv;
                if (scanline_mode < 2U) {
                    ((uint32_t *)drow1)[x] = vv;
                }
            }
            if (scanline_mode >= 2U) {
                memset(drow1, 0, row_bytes);
            }
        }
    }
}
