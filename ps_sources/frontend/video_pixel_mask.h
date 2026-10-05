#ifndef VIDEO_PIXEL_MASK_H
#define VIDEO_PIXEL_MASK_H

#include <stdint.h>

enum {
    APPLETINI_VIDEO_PIXEL_MASK_OFF = 0,
    APPLETINI_VIDEO_PIXEL_MASK_APERTURE,
    APPLETINI_VIDEO_PIXEL_MASK_SHADOW,
    APPLETINI_VIDEO_PIXEL_MASK_LCD,
    APPLETINI_VIDEO_PIXEL_MASK_COUNT
};
#define APPLETINI_VIDEO_PIXEL_MASK_MAX 3U
#define VIDEO_PIXEL_MASK_EXCLUSIONS 8U

typedef struct {
    uint16_t x0, x1, y0, y1;
} video_pixel_mask_rect_t;

/* Metadata belongs to one completed output slot. Rectangles are half-open
 * output coordinates; the pattern origin is the Apple viewport, not a crop. */
typedef struct {
    uint8_t mode, exclusion_count;
    video_pixel_mask_rect_t viewport;
    video_pixel_mask_rect_t excluded[VIDEO_PIXEL_MASK_EXCLUSIONS];
} video_pixel_mask_frame_t;

static inline uint8_t appletini_video_pixel_mask_clamp(uint8_t mode)
{
    return mode < APPLETINI_VIDEO_PIXEL_MASK_COUNT ? mode : APPLETINI_VIDEO_PIXEL_MASK_OFF;
}

static inline const char *appletini_video_pixel_mask_name(uint8_t mode)
{
    static const char *const names[] = {"Off", "Aperture grille", "Shadow mask", "LCD grid"};
    return names[appletini_video_pixel_mask_clamp(mode)];
}

static inline int video_pixel_mask_contains(const video_pixel_mask_rect_t *r, int x, int y)
{
    return x >= r->x0 && x < r->x1 && y >= r->y0 && y < r->y1;
}

static inline int video_pixel_mask_clip_rect(video_pixel_mask_rect_t *r,
    int x, int y, int width, int height, int output_width, int output_height)
{
    *r = (video_pixel_mask_rect_t){0, 0, 0, 0};
    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if (x >= output_width || y >= output_height) return 0;
    if (width > output_width - x) width = output_width - x;
    if (height > output_height - y) height = output_height - y;
    if (width <= 0 || height <= 0) return 0;
    *r = (video_pixel_mask_rect_t){(uint16_t)x, (uint16_t)(x + width),
                                 (uint16_t)y, (uint16_t)(y + height)};
    return 1;
}

/* Match the FPGA: replicate RGB565 to RGB888, attenuate, then truncate.
 * Applying the shift directly to a five/six-bit channel gives different
 * rounding at dark levels. Masks follow software scanlines and blending. */
static inline uint16_t video_pixel_mask_rgb565(uint16_t pixel, uint8_t mode,
                                               unsigned x, unsigned y)
{
    if (mode == APPLETINI_VIDEO_PIXEL_MASK_OFF || mode >= APPLETINI_VIDEO_PIXEL_MASK_COUNT)
        return pixel;
    unsigned r = pixel >> 11, g = (pixel >> 5) & 63U, b = pixel & 31U;
    r = (r << 3) | (r >> 2);
    g = (g << 2) | (g >> 4);
    b = (b << 3) | (b >> 2);
    if (mode == APPLETINI_VIDEO_PIXEL_MASK_LCD) {
        if (x % 3U == 2U || y % 3U == 2U) {
            r -= r >> 2; g -= g >> 2; b -= b >> 2;
        }
    } else {
        const unsigned selected = (x % 3U +
            (mode == APPLETINI_VIDEO_PIXEL_MASK_SHADOW ? y & 1U : 0U)) % 3U;
        if (selected != 0U) r -= r >> 2;
        if (selected != 1U) g -= g >> 2;
        if (selected != 2U) b -= b >> 2;
    }
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static inline uint16_t video_pixel_mask_frame_rgb565(const video_pixel_mask_frame_t *frame,
                                                     uint16_t pixel, int x, int y)
{
    if (!frame || !frame->mode || !video_pixel_mask_contains(&frame->viewport, x, y))
        return pixel;
    for (unsigned i = 0; i < frame->exclusion_count && i < VIDEO_PIXEL_MASK_EXCLUSIONS; ++i) {
        if (video_pixel_mask_contains(&frame->excluded[i], x, y)) return pixel;
    }
    return video_pixel_mask_rgb565(pixel, frame->mode,
        (unsigned)(x - frame->viewport.x0), (unsigned)(y - frame->viewport.y0));
}

#endif
