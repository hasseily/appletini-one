#ifndef VIDEO_MONO_H
#define VIDEO_MONO_H

#include <stdint.h>

#include "../lib/fb16.h"
#include "video_output.h"

/* Dot bleed: how far each monochrome dot spreads sideways during the
 * compositor's 2x row expansion, like the spot of a CRT. Off keeps the
 * plain RGB expansion. Light shapes only the two output columns of each
 * dot. Medium and Strong widen the spot into the neighbouring dots. */
#define APPLETINI_VIDEO_DOT_BLEED_OFF    0U
#define APPLETINI_VIDEO_DOT_BLEED_LIGHT  1U
#define APPLETINI_VIDEO_DOT_BLEED_MEDIUM 2U
#define APPLETINI_VIDEO_DOT_BLEED_STRONG 3U
#define APPLETINI_VIDEO_DOT_BLEED_MAX    APPLETINI_VIDEO_DOT_BLEED_STRONG

static inline uint8_t appletini_video_dot_bleed_clamp(uint8_t level)
{
    return (level > APPLETINI_VIDEO_DOT_BLEED_MAX) ?
        APPLETINI_VIDEO_DOT_BLEED_MAX : level;
}

static inline const char *appletini_video_dot_bleed_name(uint8_t level)
{
    switch (appletini_video_dot_bleed_clamp(level)) {
    case APPLETINI_VIDEO_DOT_BLEED_LIGHT:
        return "Light";
    case APPLETINI_VIDEO_DOT_BLEED_MEDIUM:
        return "Medium";
    case APPLETINI_VIDEO_DOT_BLEED_STRONG:
        return "Strong";
    case APPLETINI_VIDEO_DOT_BLEED_OFF:
    default:
        return "Off";
    }
}

/* The renderer already tints its BGRA pixels. Use the strongest channel as
 * brightness so green keeps its 0..181 range, and white/amber keep 0..255.
 * Build this small tint table only when the frame's mono color changes. */
static inline unsigned video_mono_channel_shift(uint8_t color)
{
    return (color == APPLE_VIDEO_MONO_GREEN) ? 8U : 16U;
}

static inline void video_mono_build_tint(uint16_t tint[256], uint8_t color)
{
    for (unsigned y = 0U; y < 256U; ++y) {
        unsigned r = y, g = y, b = y;

        switch (color) {
        case APPLE_VIDEO_MONO_BLACK:
            r = g = b = 0U;
            break;
        case APPLE_VIDEO_MONO_GREEN:
            r = y * 0x08U / 0xB5U;
            b = y * 0x52U / 0xB5U;
            break;
        case APPLE_VIDEO_MONO_AMBER:
            g = y * 0x80U / 0xFFU;
            b = y * 0x01U / 0xFFU;
            break;
        default:
            break;
        }
        tint[y] = FB16_RGB(r, g, b);
    }
}

static inline unsigned video_mono_level(const uint32_t *src, int x,
                                        unsigned channel_shift)
{
    return (src[x] >> channel_shift) & 0xFFU;
}

/* Every level makes two output samples per dot, centered on the dot. All
 * weights sum to 4 or 32, so a solid fill keeps its brightness. Row ends
 * repeat the end dot, so they never sample a colored border.
 *
 * Light: two taps. Single dots stay sharp; only the gaps soften.
 *     left  = (previous + 3*current)/4
 *     right = (3*current + next)/4 */
static inline void video_mono_expand_row_light(uint16_t *dst,
                                               const uint32_t *src,
                                               int width,
                                               unsigned channel_shift,
                                               const uint16_t tint[256])
{
    unsigned current = video_mono_level(src, 0, channel_shift);
    unsigned previous = current;
    for (int x = 0; x < width; ++x) {
        const unsigned next = (x + 1 < width)
            ? video_mono_level(src, x + 1, channel_shift) : current;
        const unsigned center = 3U * current + 2U;

        dst[2 * x] = tint[(previous + center) >> 2];
        dst[2 * x + 1] = tint[(center + next) >> 2];
        previous = current;
        current = next;
    }
}

/* Medium: three taps, a spot about 1.25x wider than Light.
 *     left  = (10*previous + 20*current + 2*next)/32
 *     right = (2*previous + 20*current + 10*next)/32 */
static inline void video_mono_expand_row_medium(uint16_t *dst,
                                                const uint32_t *src,
                                                int width,
                                                unsigned channel_shift,
                                                const uint16_t tint[256])
{
    unsigned current = video_mono_level(src, 0, channel_shift);
    unsigned previous = current;
    for (int x = 0; x < width; ++x) {
        const unsigned next = (x + 1 < width)
            ? video_mono_level(src, x + 1, channel_shift) : current;
        const unsigned center = 20U * current + 16U;

        dst[2 * x] = tint[(10U * previous + center + 2U * next) >> 5];
        dst[2 * x + 1] = tint[(2U * previous + center + 10U * next) >> 5];
        previous = current;
        current = next;
    }
}

/* Strong: four taps, a spot about 1.7x wider than Light. Each sample also
 * sees the second dot behind it.
 *     left  = (2*previous2 + 10*previous + 15*current + 5*next)/32
 *     right = (5*previous + 15*current + 10*next + 2*next2)/32 */
static inline void video_mono_expand_row_strong(uint16_t *dst,
                                                const uint32_t *src,
                                                int width,
                                                unsigned channel_shift,
                                                const uint16_t tint[256])
{
    unsigned current = video_mono_level(src, 0, channel_shift);
    unsigned previous = current;
    unsigned previous2 = current;
    unsigned next = (width > 1)
        ? video_mono_level(src, 1, channel_shift) : current;
    for (int x = 0; x < width; ++x) {
        const unsigned next2 = (x + 2 < width)
            ? video_mono_level(src, x + 2, channel_shift) : next;
        const unsigned center = 15U * current + 16U;

        dst[2 * x] = tint[(2U * previous2 + 10U * previous + center +
                           5U * next) >> 5];
        dst[2 * x + 1] = tint[(5U * previous + center + 10U * next +
                               2U * next2) >> 5];
        previous2 = previous;
        previous = current;
        current = next;
        next = next2;
    }
}

/* Shape one cached source row into its 2x RGB565 output row. src must be
 * a cached row (the compositor already copies each DDR row) and must not
 * overlap dst. No frame-sized intermediate is needed. Off falls back to
 * the plain RGB expansion so a caller never has to special-case it. */
static inline void video_mono_expand_row(uint16_t *dst, const uint32_t *src,
                                          int width, unsigned channel_shift,
                                          const uint16_t tint[256],
                                          uint8_t bleed)
{
    if (width <= 0) {
        return;
    }
    switch (appletini_video_dot_bleed_clamp(bleed)) {
    case APPLETINI_VIDEO_DOT_BLEED_LIGHT:
        video_mono_expand_row_light(dst, src, width, channel_shift, tint);
        break;
    case APPLETINI_VIDEO_DOT_BLEED_MEDIUM:
        video_mono_expand_row_medium(dst, src, width, channel_shift, tint);
        break;
    case APPLETINI_VIDEO_DOT_BLEED_STRONG:
        video_mono_expand_row_strong(dst, src, width, channel_shift, tint);
        break;
    case APPLETINI_VIDEO_DOT_BLEED_OFF:
    default:
        fb16_expand_2x_row_bgra32src(dst, src, width);
        break;
    }
}

#endif /* VIDEO_MONO_H */
