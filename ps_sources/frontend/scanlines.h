#ifndef SCANLINES_H
#define SCANLINES_H

#include <stdint.h>

#define APPLETINI_SCANLINES_OFF    0U
#define APPLETINI_SCANLINES_LIGHT  1U
#define APPLETINI_SCANLINES_MEDIUM 2U
#define APPLETINI_SCANLINES_STRONG 3U
#define APPLETINI_SCANLINES_COUNT  4U

static inline uint8_t appletini_scanlines_clamp(uint8_t mode)
{
    return (mode < APPLETINI_SCANLINES_COUNT) ? mode : APPLETINI_SCANLINES_OFF;
}

static inline const char *appletini_scanlines_name(uint8_t mode)
{
    switch (appletini_scanlines_clamp(mode)) {
    case APPLETINI_SCANLINES_LIGHT:  return "Light";
    case APPLETINI_SCANLINES_MEDIUM: return "Medium";
    case APPLETINI_SCANLINES_STRONG: return "Strong";
    default:                         return "Off";
    }
}

#endif /* SCANLINES_H */
