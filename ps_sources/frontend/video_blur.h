#ifndef VIDEO_BLUR_H
#define VIDEO_BLUR_H

#include <stdint.h>

#define APPLETINI_VIDEO_BLUR_OFF    0U
#define APPLETINI_VIDEO_BLUR_LIGHT  1U
#define APPLETINI_VIDEO_BLUR_MEDIUM 2U
#define APPLETINI_VIDEO_BLUR_STRONG 3U
#define APPLETINI_VIDEO_BLUR_MAX    APPLETINI_VIDEO_BLUR_STRONG

static inline uint8_t appletini_video_blur_clamp(uint8_t strength)
{
    return (strength > APPLETINI_VIDEO_BLUR_MAX) ?
        APPLETINI_VIDEO_BLUR_MAX : strength;
}

static inline const char *appletini_video_blur_name(uint8_t strength)
{
    switch (appletini_video_blur_clamp(strength)) {
    case APPLETINI_VIDEO_BLUR_LIGHT:
        return "Light";
    case APPLETINI_VIDEO_BLUR_MEDIUM:
        return "Medium";
    case APPLETINI_VIDEO_BLUR_STRONG:
        return "Strong";
    case APPLETINI_VIDEO_BLUR_OFF:
    default:
        return "Off";
    }
}

#endif /* VIDEO_BLUR_H */
