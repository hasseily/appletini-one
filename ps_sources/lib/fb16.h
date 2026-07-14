/*
 * fb16.h -- RGB565 drawing primitives.
 *
 * Single drawing API for the compositor pipeline. All primitives take an
 * explicit framebuffer base pointer (uint16_t * to a 1920x1080 RGB565
 * surface) so the compositor can write into whichever output slot it
 * has selected for the current frame, without globals.
 *
 * Color format: RGB565 (bits [15:11] = R, [10:5] = G, [4:0] = B). This
 * matches the DVI output pins bit-for-bit -- the PL scans these pixels
 * out with no conversion stage. Sixteen bits per pixel is the designed
 * output depth of the whole video path: half the DDR traffic of a
 * 32-bit surface on the scan-out, the compose-time stores, and the
 * bezel reads, on a memory bus shared with everything else.
 *
 * The Apple frame ring stays BGRA32 (the cycle renderer's chroma
 * pipeline is 8:8:8-native); the *_bgra32src blits narrow to 565 while
 * expanding, so precision is only dropped at the final store.
 *
 * Most primitives clip to FB16_WIDTH x FB16_HEIGHT (1920x1080); the
 * exceptions are the 2x expansion blits, which are performance-critical
 * and assume the caller has placed them inside the surface. The
 * caller's `fb` pointer must point at the start of a 1920x1080 RGB565
 * surface (i.e. one of the comp_out_slot_addr[] slots).
 */

#ifndef FB16_H
#define FB16_H

#include <stdint.h>

#define FB16_WIDTH         1920
#define FB16_HEIGHT        1080
#define FB16_BPP           2
#define FB16_STRIDE_BYTES  (FB16_WIDTH * FB16_BPP)

/* Construct an RGB565 value from 8-bit channels. */
#define FB16_RGB(r, g, b)  \
    ((uint16_t)((((uint16_t)(r) & 0xF8u) << 8) | \
                (((uint16_t)(g) & 0xFCu) << 3) | \
                (((uint16_t)(b)) >> 3)))

/* Common colors (RGB565). */
#define FB16_COLOR_BLACK       FB16_RGB(0x00, 0x00, 0x00)
#define FB16_COLOR_WHITE       FB16_RGB(0xFF, 0xFF, 0xFF)
#define FB16_COLOR_RED         FB16_RGB(0xFF, 0x00, 0x00)
#define FB16_COLOR_GREEN       FB16_RGB(0x00, 0xFF, 0x00)
#define FB16_COLOR_BLUE        FB16_RGB(0x00, 0x00, 0xFF)
#define FB16_COLOR_YELLOW      FB16_RGB(0xFF, 0xFF, 0x00)
#define FB16_COLOR_CYAN        FB16_RGB(0x00, 0xFF, 0xFF)
#define FB16_COLOR_MAGENTA     FB16_RGB(0xFF, 0x00, 0xFF)
#define FB16_COLOR_ORANGE      FB16_RGB(0xFF, 0x80, 0x00)
#define FB16_COLOR_DARK_GRAY   FB16_RGB(0x40, 0x40, 0x40)
#define FB16_COLOR_PINK        FB16_RGB(0xFF, 0x50, 0x80)
#define FB16_COLOR_PURPLE      FB16_RGB(0x80, 0x00, 0x80)
#define FB16_COLOR_TEAL        FB16_RGB(0x00, 0x80, 0x80)
#define FB16_COLOR_LIME        FB16_RGB(0x80, 0xFF, 0x00)
#define FB16_COLOR_MAROON      FB16_RGB(0x60, 0x00, 0x00)
#define FB16_COLOR_NAVY        FB16_RGB(0x00, 0x00, 0x60)
#define FB16_COLOR_GOLD        FB16_RGB(0xFF, 0xC0, 0x00)
#define FB16_COLOR_SKY         FB16_RGB(0x50, 0xC0, 0xFF)

/* Narrow one BGRA32 pixel (little-endian 0xAARRGGBB word) to RGB565. */
static inline uint16_t fb16_from_bgra32(uint32_t p) {
    return (uint16_t)(((p >> 8) & 0xF800u) |
                      ((p >> 5) & 0x07E0u) |
                      ((p >> 3) & 0x001Fu));
}

/* Widen RGB565 to a BGRA32 word (replicate-high), for consumers that
 * need 8:8:8 again (e.g. PNG screenshot encode). */
static inline uint32_t fb16_to_bgra32(uint16_t v) {
    uint32_t r5 = (v >> 11) & 0x1Fu;
    uint32_t g6 = (v >> 5)  & 0x3Fu;
    uint32_t b5 = (v)       & 0x1Fu;
    uint32_t r8 = (r5 << 3) | (r5 >> 2);
    uint32_t g8 = (g6 << 2) | (g6 >> 4);
    uint32_t b8 = (b5 << 3) | (b5 >> 2);
    return 0xFF000000u | (r8 << 16) | (g8 << 8) | b8;
}

/* Built-in fixed font geometry (font7x8[] in fb16.c). */
#define FB16_BUILTIN_FONT_HEIGHT      8
#define FB16_BUILTIN_FONT_DRAW_WIDTH  7
#define FB16_BUILTIN_FONT_ADVANCE_X   FB16_BUILTIN_FONT_DRAW_WIDTH

/* Bitmap font descriptor (used by apple_font24.c and other variable-
 * size fonts). */
typedef struct {
    uint8_t  width;
    uint8_t  height;
    uint8_t  first_char;
    uint8_t  last_char;
    uint16_t bytes_per_glyph;
    const uint8_t *data;
} fb16_bitmap_font_t;

/* ---------- Primitives ---------- */

/* Fill the entire 1920x1080 frame with `color`. Optimized for color==0
 * (memset path) and aligned 64-bit writes otherwise. */
void fb16_clear(uint16_t *fb, uint16_t color);

/* Set one pixel; clipped. */
void fb16_pixel(uint16_t *fb, int x, int y, uint16_t color);

/* Horizontal/vertical 1px lines; clipped. */
void fb16_hline(uint16_t *fb, int x, int y, int w, uint16_t color);
void fb16_vline(uint16_t *fb, int x, int y, int h, uint16_t color);

/* Filled rectangle, clipped. */
void fb16_fill_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t color);

/* Outline rectangle (4 sides, 1 px each). */
void fb16_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t color);

/* Bresenham's line algorithm; clipped. */
void fb16_line(uint16_t *fb, int x0, int y0, int x1, int y1, uint16_t color);

/* Outline / filled circle. */
void fb16_circle(uint16_t *fb, int cx, int cy, int r, uint16_t color);
void fb16_fill_circle(uint16_t *fb, int cx, int cy, int r, uint16_t color);

/* Built-in 7x8 font drawing. */
void fb16_char(uint16_t *fb, int x, int y, char c, uint16_t fg, uint16_t bg);
void fb16_string(uint16_t *fb, int x, int y, const char *s, uint16_t fg, uint16_t bg);
void fb16_char_scaled(uint16_t *fb, int x, int y, char c, uint16_t fg, uint16_t bg, int scale);
void fb16_string_scaled(uint16_t *fb, int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale);
void fb16_char_scaled_xy(uint16_t *fb, int x, int y, char c, uint16_t fg, uint16_t bg, int scale_x, int scale_y);
void fb16_string_scaled_xy(uint16_t *fb, int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale_x, int scale_y);

/* Variable-size bitmap font drawing. */
void fb16_char_bitmap(uint16_t *fb, int x, int y, char c, const fb16_bitmap_font_t *font, uint16_t fg, uint16_t bg);
void fb16_string_bitmap(uint16_t *fb, int x, int y, const char *s, const fb16_bitmap_font_t *font, uint16_t fg, uint16_t bg);

/* Copy a w x h RGB565 source into the output frame at (x, y). Source is
 * assumed contiguous (stride = w). Clipped at the destination. Used for
 * assets converted to 565 at load time (bezel, logo). */
void fb16_blit_565(uint16_t *fb, int x, int y, int w, int h,
                   const uint16_t *src);

/* Copy a w x h BGRA32 source into the output frame at (x, y), narrowing
 * to 565 per pixel. Clipped at the destination. For 32-bit-native
 * sources drawn straight to the output (no cached 565 copy). */
void fb16_blit_bgra32src(uint16_t *fb, int x, int y, int w, int h,
                         const uint32_t *src);

/* 2x horizontal, 4x vertical nearest-neighbor replication blit from a
 * BGRA32 source (the Apple frame ring), narrowing to 565 in the
 * expansion. The destination rect is (dst_x, dst_y) sized (src_w*2 by
 * src_h*4). Caller must ensure the destination fits; this primitive
 * does not clip.
 *
 * src_stride is the source row stride in pixels. Pass <= 0 to mean
 * "tightly packed" (stride == src_w). The Apple FB has leading guard
 * pixels per row so its visible-pixels-per-row (src_w) is smaller than
 * its actual row stride; src_stride captures that. */
void fb16_blit_2x4(uint16_t *fb, int dst_x, int dst_y,
                   const uint32_t *src, int src_w, int src_h,
                   int src_stride);

/* Same as fb16_blit_2x4, with scanline mode 0..3. Mode 1 blanks output
 * repeat row 3, mode 2 blanks rows 2 and 3, mode 3 blanks rows 1..3. */
void fb16_blit_2x4_scanlines(uint16_t *fb, int dst_x, int dst_y,
                             const uint32_t *src, int src_w, int src_h,
                             int src_stride, uint8_t scanline_mode);

/* 2x horizontal, 2x vertical nearest-neighbor replication blit. Used by
 * VidHD SHR, whose AppleWin-compatible source is already 640x400 and
 * only needs a modest output scale for the 1080p compositor surface. */
void fb16_blit_2x2_scanlines(uint16_t *fb, int dst_x, int dst_y,
                             const uint32_t *src, int src_w, int src_h,
                             int src_stride, uint8_t scanline_mode);

/* Expand one BGRA32 source row into a 565 destination with 2x
 * horizontal doubling: dst[2i] == dst[2i+1] == narrow(src[i]). Exposed
 * for the compositor's ghosting path, which blends in 8:8:8 scratch and
 * narrows only at this final expansion. */
void fb16_expand_2x_row_bgra32src(uint16_t *dst, const uint32_t *src,
                                  int src_w);

#endif /* FB16_H */
