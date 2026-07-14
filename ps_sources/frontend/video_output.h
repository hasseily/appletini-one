#ifndef VIDEO_OUTPUT_H
#define VIDEO_OUTPUT_H

#include <stdint.h>

#define APPLE_VIDEO_MONO_BLACK 0U
#define APPLE_VIDEO_MONO_WHITE 1U
#define APPLE_VIDEO_MONO_AMBER 2U
#define APPLE_VIDEO_MONO_GREEN 3U

#define APPLE_VIDEO_COLOR_IDEALIZED          0U
#define APPLE_VIDEO_COLOR_RGB                1U
#define APPLE_VIDEO_COLOR_COMPOSITE_MONITOR  2U
#define APPLE_VIDEO_COLOR_TV                 3U
#define APPLE_VIDEO_COLOR_PAL_ACCURATE_COMPOSITE 4U
#define APPLE_VIDEO_COLOR_PAL_ACCURATE_TV        5U
#define APPLE_VIDEO_COLOR_COUNT              6U

#define APPLE_VIDEO_IIGS_BORDER_COLOR_COUNT 16U
#define APPLE_VIDEO_IIGS_BORDER_DEFAULT      6U

#define APPLE_VIDEO_SETTINGS_MONO_ENABLE_SHIFT 0U
#define APPLE_VIDEO_SETTINGS_MONO_COLOR_SHIFT  1U
#define APPLE_VIDEO_SETTINGS_COLOR_MODE_SHIFT  4U
#define APPLE_VIDEO_SETTINGS_COLOR_MODE_MASK   0xFU
#define APPLE_VIDEO_SETTINGS_VIDEO7_AUTO_MONO_SHIFT 8U
#define APPLE_VIDEO_SETTINGS_BORDER_ENABLE_SHIFT 9U
#define APPLE_VIDEO_SETTINGS_BORDER_FLOOD_SHIFT 10U
#define APPLE_VIDEO_SETTINGS_CLEAN_PHASE_SHIFT 12U
#define APPLE_VIDEO_SETTINGS_PAL_PHASE_SHIFT   20U
#define APPLE_VIDEO_SETTINGS_PHASE_MASK        0x7FU
#define APPLE_VIDEO_SETTINGS_BORDER_COLOR_SHIFT 27U
#define APPLE_VIDEO_SETTINGS_BORDER_COLOR_MASK  0xFU

#define APPLE_VIDEO_TIMING_PHASE_MIN (-64)
#define APPLE_VIDEO_TIMING_PHASE_MAX 63
#define APPLE_VIDEO_TIMING_PHASE_BIAS 64

#define APPLE_VIDEO_DEFAULT_CLEAN_PHASE_CYCLES 0
#define APPLE_VIDEO_DEFAULT_PAL_PHASE_CYCLES   0

#define APPLE_VIDEO_SETTINGS_DEFAULT \
    (((uint32_t)APPLE_VIDEO_MONO_WHITE << APPLE_VIDEO_SETTINGS_MONO_COLOR_SHIFT) | \
     ((uint32_t)APPLE_VIDEO_COLOR_COMPOSITE_MONITOR << APPLE_VIDEO_SETTINGS_COLOR_MODE_SHIFT) | \
     ((uint32_t)1U << APPLE_VIDEO_SETTINGS_VIDEO7_AUTO_MONO_SHIFT) | \
     ((uint32_t)APPLE_VIDEO_IIGS_BORDER_DEFAULT << APPLE_VIDEO_SETTINGS_BORDER_COLOR_SHIFT) | \
     ((uint32_t)(APPLE_VIDEO_DEFAULT_CLEAN_PHASE_CYCLES + APPLE_VIDEO_TIMING_PHASE_BIAS) << \
      APPLE_VIDEO_SETTINGS_CLEAN_PHASE_SHIFT) | \
     ((uint32_t)(APPLE_VIDEO_DEFAULT_PAL_PHASE_CYCLES + APPLE_VIDEO_TIMING_PHASE_BIAS) << \
      APPLE_VIDEO_SETTINGS_PAL_PHASE_SHIFT))

static inline uint8_t apple_video_mono_color_clamp(uint8_t color)
{
    switch (color) {
    case APPLE_VIDEO_MONO_BLACK:
    case APPLE_VIDEO_MONO_WHITE:
    case APPLE_VIDEO_MONO_AMBER:
    case APPLE_VIDEO_MONO_GREEN:
        return color;
    default:
        return APPLE_VIDEO_MONO_WHITE;
    }
}

static inline uint8_t apple_video_color_mode_clamp(uint8_t mode)
{
    return (mode < APPLE_VIDEO_COLOR_COUNT) ?
        mode : APPLE_VIDEO_COLOR_COMPOSITE_MONITOR;
}

static inline uint8_t apple_video_color_mode_is_pal_accurate(uint8_t mode)
{
    mode = apple_video_color_mode_clamp(mode);
    return (uint8_t)((mode == APPLE_VIDEO_COLOR_PAL_ACCURATE_COMPOSITE ||
                     mode == APPLE_VIDEO_COLOR_PAL_ACCURATE_TV) ? 1U : 0U);
}

static inline uint8_t apple_video_iigs_border_color_clamp(uint8_t color)
{
    return (uint8_t)(color & 0x0FU);
}

static inline uint32_t apple_video_iigs_border_bgra(uint8_t color)
{
    static const uint16_t palette[APPLE_VIDEO_IIGS_BORDER_COLOR_COUNT] = {
        0x000U, 0xD03U, 0x009U, 0xD2DU,
        0x072U, 0x555U, 0x22FU, 0x6AFU,
        0x850U, 0xF60U, 0xAAAU, 0xF98U,
        0x0D0U, 0xFF0U, 0x4F9U, 0xFFFU
    };
    const uint16_t rgb = palette[apple_video_iigs_border_color_clamp(color)];
    const uint32_t r = (uint32_t)((rgb >> 8) & 0x0FU) * 17U;
    const uint32_t g = (uint32_t)((rgb >> 4) & 0x0FU) * 17U;
    const uint32_t b = (uint32_t)(rgb & 0x0FU) * 17U;

    return 0xFF000000U | (r << 16) | (g << 8) | b;
}

static inline int8_t apple_video_timing_phase_clamp(int8_t cycles)
{
    if (cycles < (int8_t)APPLE_VIDEO_TIMING_PHASE_MIN) {
        return (int8_t)APPLE_VIDEO_TIMING_PHASE_MIN;
    }
    if (cycles > (int8_t)APPLE_VIDEO_TIMING_PHASE_MAX) {
        return (int8_t)APPLE_VIDEO_TIMING_PHASE_MAX;
    }
    return cycles;
}

static inline uint32_t apple_video_timing_phase_pack(int8_t cycles)
{
    const int32_t clamped = (int32_t)apple_video_timing_phase_clamp(cycles);
    return (uint32_t)(clamped + APPLE_VIDEO_TIMING_PHASE_BIAS) &
           APPLE_VIDEO_SETTINGS_PHASE_MASK;
}

static inline int8_t apple_video_timing_phase_unpack(uint32_t settings,
                                                    uint32_t shift)
{
    const int32_t raw =
        (int32_t)((settings >> shift) & APPLE_VIDEO_SETTINGS_PHASE_MASK);
    return apple_video_timing_phase_clamp(
        (int8_t)(raw - APPLE_VIDEO_TIMING_PHASE_BIAS));
}

static inline uint32_t apple_video_settings_pack_border_full(
    uint8_t mono_enable,
    uint8_t mono_color,
    uint8_t color_mode,
    uint8_t video7_auto_mono_enable,
    int8_t clean_phase_cycles,
    int8_t pal_phase_cycles,
    uint8_t border_enable,
    uint8_t border_color,
    uint8_t border_flood)
{
    return ((uint32_t)((mono_enable != 0U) ? 1U : 0U) << APPLE_VIDEO_SETTINGS_MONO_ENABLE_SHIFT) |
           ((uint32_t)apple_video_mono_color_clamp(mono_color) << APPLE_VIDEO_SETTINGS_MONO_COLOR_SHIFT) |
           ((uint32_t)apple_video_color_mode_clamp(color_mode) << APPLE_VIDEO_SETTINGS_COLOR_MODE_SHIFT) |
           ((uint32_t)((video7_auto_mono_enable != 0U) ? 1U : 0U) <<
            APPLE_VIDEO_SETTINGS_VIDEO7_AUTO_MONO_SHIFT) |
           ((uint32_t)((border_enable != 0U) ? 1U : 0U) <<
            APPLE_VIDEO_SETTINGS_BORDER_ENABLE_SHIFT) |
           ((uint32_t)((border_flood != 0U) ? 1U : 0U) <<
            APPLE_VIDEO_SETTINGS_BORDER_FLOOD_SHIFT) |
           (apple_video_timing_phase_pack(clean_phase_cycles) <<
            APPLE_VIDEO_SETTINGS_CLEAN_PHASE_SHIFT) |
           (apple_video_timing_phase_pack(pal_phase_cycles) <<
            APPLE_VIDEO_SETTINGS_PAL_PHASE_SHIFT) |
           ((uint32_t)apple_video_iigs_border_color_clamp(border_color) <<
            APPLE_VIDEO_SETTINGS_BORDER_COLOR_SHIFT);
}

static inline uint32_t apple_video_settings_pack_full(uint8_t mono_enable,
                                                      uint8_t mono_color,
                                                      uint8_t color_mode,
                                                      uint8_t video7_auto_mono_enable,
                                                      int8_t clean_phase_cycles,
                                                      int8_t pal_phase_cycles)
{
    return apple_video_settings_pack_border_full(
        mono_enable,
        mono_color,
        color_mode,
        video7_auto_mono_enable,
        clean_phase_cycles,
        pal_phase_cycles,
        0U,
        APPLE_VIDEO_IIGS_BORDER_DEFAULT,
        0U);
}

static inline uint32_t apple_video_settings_pack_ex(uint8_t mono_enable,
                                                    uint8_t mono_color,
                                                    uint8_t color_mode,
                                                    uint8_t video7_auto_mono_enable)
{
    return apple_video_settings_pack_full(mono_enable,
                                          mono_color,
                                          color_mode,
                                          video7_auto_mono_enable,
                                          (int8_t)APPLE_VIDEO_DEFAULT_CLEAN_PHASE_CYCLES,
                                          (int8_t)APPLE_VIDEO_DEFAULT_PAL_PHASE_CYCLES);
}

static inline uint32_t apple_video_settings_pack(uint8_t mono_enable,
                                                 uint8_t mono_color,
                                                 uint8_t color_mode)
{
    return apple_video_settings_pack_ex(mono_enable, mono_color, color_mode, 1U);
}

static inline uint8_t apple_video_settings_mono_enabled(uint32_t settings)
{
    return (uint8_t)((settings >> APPLE_VIDEO_SETTINGS_MONO_ENABLE_SHIFT) & 0x1U);
}

static inline uint8_t apple_video_settings_mono_color(uint32_t settings)
{
    return apple_video_mono_color_clamp(
        (uint8_t)((settings >> APPLE_VIDEO_SETTINGS_MONO_COLOR_SHIFT) & 0x3U));
}

static inline uint8_t apple_video_settings_color_mode(uint32_t settings)
{
    return apple_video_color_mode_clamp(
        (uint8_t)((settings >> APPLE_VIDEO_SETTINGS_COLOR_MODE_SHIFT) &
                  APPLE_VIDEO_SETTINGS_COLOR_MODE_MASK));
}

static inline uint8_t apple_video_settings_video7_auto_mono_enabled(uint32_t settings)
{
    return (uint8_t)((settings >> APPLE_VIDEO_SETTINGS_VIDEO7_AUTO_MONO_SHIFT) & 0x1U);
}

static inline uint8_t apple_video_settings_border_enabled(uint32_t settings)
{
    return (uint8_t)((settings >> APPLE_VIDEO_SETTINGS_BORDER_ENABLE_SHIFT) & 0x1U);
}

static inline uint8_t apple_video_settings_border_color(uint32_t settings)
{
    return apple_video_iigs_border_color_clamp(
        (uint8_t)((settings >> APPLE_VIDEO_SETTINGS_BORDER_COLOR_SHIFT) &
                  APPLE_VIDEO_SETTINGS_BORDER_COLOR_MASK));
}

static inline uint8_t apple_video_settings_border_flood(uint32_t settings)
{
    return (uint8_t)((settings >> APPLE_VIDEO_SETTINGS_BORDER_FLOOD_SHIFT) & 0x1U);
}

static inline int8_t apple_video_settings_clean_phase_cycles(uint32_t settings)
{
    return apple_video_timing_phase_unpack(settings,
                                           APPLE_VIDEO_SETTINGS_CLEAN_PHASE_SHIFT);
}

static inline int8_t apple_video_settings_pal_phase_cycles(uint32_t settings)
{
    return apple_video_timing_phase_unpack(settings,
                                           APPLE_VIDEO_SETTINGS_PAL_PHASE_SHIFT);
}

static inline uint32_t apple_video_settings_normalize(uint32_t settings)
{
    return apple_video_settings_pack_border_full(
        apple_video_settings_mono_enabled(settings),
        apple_video_settings_mono_color(settings),
        apple_video_settings_color_mode(settings),
        apple_video_settings_video7_auto_mono_enabled(settings),
        apple_video_settings_clean_phase_cycles(settings),
        apple_video_settings_pal_phase_cycles(settings),
        apple_video_settings_border_enabled(settings),
        apple_video_settings_border_color(settings),
        apple_video_settings_border_flood(settings));
}

#endif /* VIDEO_OUTPUT_H */
