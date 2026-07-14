#ifndef VIDEO_GHOSTING_H
#define VIDEO_GHOSTING_H

#include <stdint.h>

#define APPLETINI_VIDEO_GHOSTING_OFF    0U
#define APPLETINI_VIDEO_GHOSTING_LIGHT  1U
#define APPLETINI_VIDEO_GHOSTING_MEDIUM 2U
#define APPLETINI_VIDEO_GHOSTING_STRONG 3U
#define APPLETINI_VIDEO_GHOSTING_MAX    APPLETINI_VIDEO_GHOSTING_STRONG

static inline uint8_t appletini_video_ghosting_clamp(uint8_t strength)
{
    return (strength > APPLETINI_VIDEO_GHOSTING_MAX) ?
        APPLETINI_VIDEO_GHOSTING_MAX : strength;
}

static inline const char *appletini_video_ghosting_name(uint8_t strength)
{
    switch (appletini_video_ghosting_clamp(strength)) {
    case APPLETINI_VIDEO_GHOSTING_LIGHT:
        return "Light";
    case APPLETINI_VIDEO_GHOSTING_MEDIUM:
        return "Medium";
    case APPLETINI_VIDEO_GHOSTING_STRONG:
        return "Strong";
    case APPLETINI_VIDEO_GHOSTING_OFF:
    default:
        return "Off";
    }
}

#endif /* VIDEO_GHOSTING_H */
