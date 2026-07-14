#ifndef APPLE_PAL_VIDEO_TIMING_H
#define APPLE_PAL_VIDEO_TIMING_H

#include <stdint.h>

uint8_t apple_pal_video_mode_is_active(uint8_t color_mode);
void apple_pal_video_set_framebuffer(uint32_t *fb);
void apple_pal_video_set_video_output(uint8_t mono_enable,
                                      uint8_t mono_color,
                                      uint8_t color_mode);
void apple_pal_video_reset(void);
void apple_pal_video_resync(void);
void apple_pal_video_begin_frame(void);
uint8_t apple_pal_video_end_frame(void);
void apple_pal_video_preroll_line0_cycle(uint32_t cycle,
                                         uint32_t softswitch_bits);
void apple_pal_video_on_cycle(uint32_t line,
                              uint32_t cycle,
                              uint32_t softswitch_bits);

/* Render queued PAL work off the egress drain path. Call once per CPU1
 * main-loop iteration, after apple_cycle_egress_poll(). No-op outside PAL
 * accurate modes, so it is safe to call unconditionally. */
void apple_pal_video_pump(void);

extern volatile uint32_t g_pal_frames_published;
extern volatile uint32_t g_pal_frames_dropped;
extern volatile uint32_t g_pal_lines_rendered;
extern volatile uint32_t g_pal_lines_incomplete;
extern volatile uint32_t g_pal_queue_overflows;
extern volatile uint32_t g_pal_queue_max;
extern volatile uint32_t g_pal_fast_lines;
extern volatile uint32_t g_pal_slow_lines;
extern volatile uint32_t g_pal_render_ticks_total;
extern volatile uint32_t g_pal_render_ticks_max;
extern volatile uint32_t g_pal_end_frame_count;
extern volatile uint32_t g_pal_end_queue_total;
extern volatile uint32_t g_pal_end_queue_max;
extern volatile uint32_t g_pal_end_lines_drained;

#endif /* APPLE_PAL_VIDEO_TIMING_H */
