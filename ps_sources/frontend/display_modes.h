#ifndef DISPLAY_MODES_H
#define DISPLAY_MODES_H

#include <stdint.h>

/* IDs also select the PL timing presets in video_pkg.sv. Keep this order. */
#define DISPLAY_MODE_COUNT    6U
#define DISPLAY_MODE_DEFAULT  4U
#define DISPLAY_MAX_WIDTH     1920U
#define DISPLAY_MAX_HEIGHT    1080U
#define DISPLAY_SIZE_MULTIPLIER_MAX  2U

/* Zero follows the largest integer scale that fits each Apple frame. */
static inline uint8_t display_size_multiplier_clamp(uint8_t multiplier)
{
    return multiplier <= DISPLAY_SIZE_MULTIPLIER_MAX ? multiplier : 0U;
}

typedef struct {
    uint16_t width;
    uint16_t height;
    const char *name;
} display_mode_t;

static inline uint8_t display_mode_clamp(uint8_t mode)
{
    return mode < DISPLAY_MODE_COUNT ? mode : DISPLAY_MODE_DEFAULT;
}

static inline const display_mode_t *display_mode_get(uint8_t mode)
{
    static const display_mode_t modes[DISPLAY_MODE_COUNT] = {
        { 1024U,  768U, "1024x768"  },
        { 1200U,  800U, "1200x800"  },
        { 1280U, 1024U, "1280x1024" },
        { 1680U, 1050U, "1680x1050" },
        { 1920U, 1080U, "1920x1080" },
        { 1360U,  768U, "1360x768"  },
    };
    return &modes[display_mode_clamp(mode)];
}

static inline uint8_t display_mode_max_multiplier(uint8_t mode)
{
    const display_mode_t *output = display_mode_get(mode);
    /* Legacy video has a 560x384 display aspect; SHR can need a lower scale. */
    const uint16_t fit_x = output->width / 560U;
    const uint16_t fit_y = output->height / 384U;
    const uint16_t fit = fit_x < fit_y ? fit_x : fit_y;
    return (uint8_t)(fit < DISPLAY_SIZE_MULTIPLIER_MAX ?
                     fit : DISPLAY_SIZE_MULTIPLIER_MAX);
}

#endif /* DISPLAY_MODES_H */
