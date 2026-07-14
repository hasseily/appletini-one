/*
 * compositor.h -- PS-driven compositor that produces 1920x1080 RGB565
 * output frames for fb_reader.
 *
 * Each output frame is a composite of:
 *   - The UI overlay (bezel, debug HUD, activity badges, optional menu) when
 *     the boot menu is not full-screen, OR
 *   - The full-screen boot menu / config menu drawing.
 *   - The Apple subwindow (1120x768) scaled from the latest published
 *     Apple FB slot, present only in overlay mode.
 *
 * Triple buffer: 3 slots in DDR (comp_out_slot_addr[]). The compositor
 * picks a slot that is neither currently scanning out (per
 * FB_LAST_LATCHED_REG) nor already pending for scanout, composites into
 * it, and writes FB_BASE_ADDR_REG to publish. The PL latches the latest
 * base at vblank-start, so a newer publish may supersede an older pending
 * frame before it is ever scanned out.
 *
 * Pacing: compositor_tick() is non-blocking. It normally redraws after
 * vblank for UI refresh, but it can also publish immediately when the
 * Apple renderer has produced a fresher frame, reducing one full-frame
 * worth of output latency.
 */

#ifndef COMPOSITOR_H
#define COMPOSITOR_H

#include <stdint.h>

/* UI draw callback. Implemented in main.c (ui_compose_frame). The
 * compositor hands the chosen output slot to this callback once per
 * frame. Forward-declared as opaque pointers because compositor.c
 * doesn't know about ui_state_t / config_menu_t internals.
 *
 * Return value: non-zero tells the compositor to suppress the Apple
 * subwindow blit for this frame, leaving the UI's drawing intact in
 * that region. Used when the boot menu / config menu is active: the
 * menu draws full-screen and would otherwise be clobbered by the
 * Apple subwindow. Zero = compose the Apple subwindow on top as
 * normal. */
typedef int (*compositor_ui_draw_fn)(uint16_t *fb,
                                     const void *ui_state,
                                     const void *config_menu);

/* One-time setup. Marks the output slots non-cacheable and primes the
 * publish-counter handshake. Must be called after the AXI register
 * region has been marked DEVICE_MEMORY and after
 * apple_cycle_renderer_init() (so the Apple FB ring is initialized). */
void compositor_init(compositor_ui_draw_fn draw_fn);

/* Bind the user-state pointers the draw callback receives. Called once
 * after compositor_init() before the main loop starts. */
void compositor_set_draw_context(const void *ui_state,
                                 const void *config_menu);

/* Apple subwindow scanline strength: APPLETINI_SCANLINES_* values. */
void compositor_set_scanlines(uint8_t mode);

/* Apple subwindow phosphor ghosting strength. */
void compositor_set_video_ghosting(uint8_t strength);
uint8_t compositor_video_ghosting(void);

/* IIgs-style border ring and optional outer flood. */
void compositor_set_border(uint8_t enabled, uint8_t flood);

/* Temporarily stop publishing new output frames. The currently latched
 * scanout slot remains on screen, which lets slow operations read from it
 * without the compositor racing ahead. */
void compositor_set_paused(uint8_t paused);

/* Return the 1920x1080 RGB565 output slot currently latched by the PL.
 * If the PL has not reported a recognizable slot yet, this falls back to
 * the last slot published by the compositor. */
const uint16_t *compositor_latched_framebuffer(uint8_t *slot_out);

/* Force the next compositor_tick() to redraw and publish a frame even if
 * neither vblank pacing nor a fresh Apple frame would otherwise do it. */
void compositor_request_full_refresh(void);

/* Debug/measurement toggle. When enabled, compositor_tick() composites and
 * publishes on every call, bypassing the vblank + fresh-Apple-frame pacing.
 * This exposes the raw composite-and-publish ceiling (read it off the
 * "FPS comp" HUD line) independent of the 50/60 Hz vblank+renderer cap --
 * e.g. a PAL machine pins both the renderer and HDMI vblank to 50 Hz.
 * Default off; leave off for normal use (uncapped publishing produces
 * frames the PL never scans out and can tear). */
void compositor_set_uncapped(uint8_t uncapped);
uint8_t compositor_uncapped(void);

/* Nonzero while the current composite is a forced full refresh. UI draw
 * callbacks use this to drop cached static state (per-slot backgrounds)
 * so a full refresh really repaints everything -- otherwise a slot whose
 * background content was ever damaged keeps its stale pixels forever
 * (generation matches, so it is never repainted). */
uint8_t compositor_full_refresh_active(void);

/* Run zero or one composite-and-publish cycles. Non-blocking. Returns
 * non-zero when a frame was actually composited and published. */
int compositor_tick(void);

/* Diagnostic counters. */
extern volatile uint32_t g_compositor_frames_published;
extern volatile uint32_t g_compositor_frames_skipped;     /* PL not ready */
extern volatile uint32_t g_compositor_apple_frames_drawn; /* Apple subwin blits */
extern volatile uint32_t g_compositor_last_apple_slot;    /* 0..2 or 0xFF */
extern volatile uint32_t g_compositor_last_apple_mode;    /* APPLE_FB_DISPLAY_MODE_* */
extern volatile uint32_t g_compositor_last_ui_us;
extern volatile uint32_t g_compositor_last_apple_us;
extern volatile uint32_t g_compositor_last_sync_us;
extern volatile uint32_t g_compositor_last_total_us;
extern volatile uint32_t g_compositor_last_apple_drawn;
extern volatile uint32_t g_compositor_last_suppress_apple;

#endif /* COMPOSITOR_H */
