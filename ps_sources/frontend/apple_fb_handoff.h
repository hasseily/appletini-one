/*
 * apple_fb_handoff.h -- 3-slot async handoff between the Apple
 * cycle renderer and the compositor.
 *
 * The renderer (writer) and the compositor (reader) run at
 * independent rates: Apple frames are 50/60 Hz with jitter while DVI
 * scanout has its own cadence. The handoff lets each side run without
 * blocking:
 *
 *   - CPU1 publishes a writer-owned slot and monotonically advances
 *     a sequence number after pixel writes are visible.
 *   - CPU0 claims a fresh sequence when available, otherwise it
 *     keeps drawing the previously claimed slot.
 *   - CPU0 publishes reader_active so CPU1 can avoid the slot that
 *     the compositor is currently reading.
 *
 * The published slot word also carries APPLE_FB_DISPLAY_MODE_* and the
 * frame-end border color so the compositor chooses the Apple border-ring
 * path or VidHD SHR
 * 640x400 path from the same coherent read as the slot index.
 */

#ifndef APPLE_FB_HANDOFF_H
#define APPLE_FB_HANDOFF_H

#include <stdint.h>

#include "video_output.h"

#define APPLE_FB_NO_SLOT  0xFFu

#define APPLE_FB_DISPLAY_MODE_LEGACY 0U
#define APPLE_FB_DISPLAY_MODE_SHR    1U

/* Writer-side: returns the slot the renderer should paint into. */
uint8_t apple_fb_writer_slot(void);

/* Writer-side: publish the just-painted writer slot. Caller is
 * responsible for any dcache flush of the slot's contents BEFORE
 * calling -- this routine just performs the atomic ownership swap
 * and a release-store ordering barrier. */
void apple_fb_writer_publish(void);
void apple_fb_writer_publish_mode(uint32_t display_mode);
void apple_fb_writer_publish_frame(uint32_t display_mode, uint8_t border_color);

/* Reader-side: claim the freshest frame the writer has published,
 * if any. Returns the slot index to read (0..2). If the writer has
 * not yet published a first frame, returns APPLE_FB_NO_SLOT and the
 * caller should skip the Apple subwindow blit.
 *
 * If no new frame is available since the last call, returns the
 * same slot the previous call returned (i.e. the reader keeps its
 * current frame). */
uint8_t apple_fb_reader_claim(void);
uint32_t apple_fb_reader_display_mode(void);
uint8_t apple_fb_reader_border_color(void);
uint32_t apple_fb_reader_publish_seq(void);
uint32_t apple_fb_reader_published_display_mode(void);

/* CPU0/shared init: reset shared handoff/control words to their boot defaults.
 * Call once at boot from CPU0. */
void apple_fb_handoff_init(void);

/* CPU1/local init: apply the shared OCM mapping and reset only this core's
 * local handoff cache. Does not overwrite CPU0-published controls. */
void apple_fb_handoff_secondary_init(void);

/* Diagnostic: snapshot of the current state word. Race-free read
 * for logging; callers should not interpret across writes. */
uint32_t apple_fb_handoff_state(void);

/* Shared CPU0->CPU1 renderer controls. The value is packed with
 * apple_video_settings_pack() from video_output.h. */
void apple_fb_video_settings_set(uint32_t settings);
uint32_t apple_fb_video_settings_get(void);

/* Video character-ROM override generation (CPU0 writes, CPU1 reads). CPU0
 * bumps this after copying a validated ROM into APPLE_VIDEO_ROM_OVERRIDE_ADDR
 * (compositor_layout.h); 0 means "no override -> baked Enhanced US ROM". CPU1
 * rebuilds csbits whenever the value it sees changes. */
void apple_fb_video_rom_gen_set(uint32_t gen);
uint32_t apple_fb_video_rom_gen_get(void);

#endif /* APPLE_FB_HANDOFF_H */
