#ifndef VIDEO_GLOW_H
#define VIDEO_GLOW_H

#include <stdint.h>

#define APPLETINI_VIDEO_GLOW_OFF    0U
#define APPLETINI_VIDEO_GLOW_LIGHT  1U
#define APPLETINI_VIDEO_GLOW_MEDIUM 2U
#define APPLETINI_VIDEO_GLOW_STRONG 3U
#define APPLETINI_VIDEO_GLOW_MAX    APPLETINI_VIDEO_GLOW_STRONG

static inline uint8_t appletini_video_glow_clamp(uint8_t strength)
{
    return (strength > APPLETINI_VIDEO_GLOW_MAX) ?
        APPLETINI_VIDEO_GLOW_MAX : strength;
}

static inline const char *appletini_video_glow_name(uint8_t strength)
{
    switch (appletini_video_glow_clamp(strength)) {
    case APPLETINI_VIDEO_GLOW_LIGHT:
        return "Light";
    case APPLETINI_VIDEO_GLOW_MEDIUM:
        return "Medium";
    case APPLETINI_VIDEO_GLOW_STRONG:
        return "Strong";
    case APPLETINI_VIDEO_GLOW_OFF:
    default:
        return "Off";
    }
}

#endif /* VIDEO_GLOW_H */
