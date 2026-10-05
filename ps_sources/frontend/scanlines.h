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

/* At 4x retain the original 1/2/3 black rows. At 2x keep every source
 * row and dim its second copy to 3/4, 1/2, or 1/4. At 1x never erase detail. */
static inline uint8_t appletini_scanlines_keep_quarters(unsigned phase, unsigned scale_y,
                                                        uint8_t mode)
{
    mode = appletini_scanlines_clamp(mode);
    if (!mode || scale_y == 1U) {
        return 4U;
    }
    if (scale_y == 4U) {
        return phase >= 4U - mode ? 0U : 4U;
    }
    return scale_y == 2U && phase == 1U ? (uint8_t)(4U - mode) : 4U;
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
