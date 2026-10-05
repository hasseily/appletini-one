#ifndef VIDEO_FILTER_CONFIG_H
#define VIDEO_FILTER_CONFIG_H

#include "video_blur.h"
#include "video_mono.h"

#define VIDEO_FILTER_AXIS_H 1U
#define VIDEO_FILTER_AXIS_V 2U
#define VIDEO_FILTER_EXPLICIT_BLUR 1U
#define VIDEO_FILTER_EXPLICIT_DOT 2U

/* F1.2.4 restores the original Blur/Dot bleed controls. Explicit original
 * keys win regardless of order. F1.2.3 axis settings have no exact equivalent:
 * color H maps to Light (Strong stays Strong); V requires at least Medium.
 * Mono H maps to Dot bleed. Missing axis keys retain the original defaults. */
static inline void video_filter_migrate(uint8_t *blur, uint8_t *dot,
    uint8_t explicit_filters, uint8_t axes, uint8_t horizontal,
    uint8_t vertical, uint8_t mono)
{
    horizontal = appletini_video_blur_clamp(horizontal);
    vertical = appletini_video_blur_clamp(vertical);
    if (axes && !(explicit_filters & VIDEO_FILTER_EXPLICIT_BLUR)) {
        *blur = !mono && horizontal ?
            (horizontal == APPLETINI_VIDEO_BLUR_STRONG ?
                APPLETINI_VIDEO_BLUR_STRONG : APPLETINI_VIDEO_BLUR_LIGHT) :
            APPLETINI_VIDEO_BLUR_OFF;
        if (vertical && *blur < APPLETINI_VIDEO_BLUR_MEDIUM) {
            *blur = APPLETINI_VIDEO_BLUR_MEDIUM;
        }
    }
    if (axes && !(explicit_filters & VIDEO_FILTER_EXPLICIT_DOT)) {
        *dot = mono ? horizontal : APPLETINI_VIDEO_DOT_BLEED_OFF;
    }
    *blur = appletini_video_blur_clamp(*blur);
    *dot = appletini_video_dot_bleed_clamp(*dot);
}

#endif
