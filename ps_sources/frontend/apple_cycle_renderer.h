/*
 * apple_cycle_renderer.h -- Per-cycle Apple //e renderer driving
 * AppleWin's NTSC pixel-emission core (appletini_ntsc.c). Ported from
 * AppleWin source/NTSC.cpp; behavior matches that emulator at the
 * cycle level.
 *
 * apple_cycle_egress.c calls apple_cycle_renderer_on_record(rec) once per
 * record consumed from the FIFO, in order. The renderer maintains its
 * own per-cycle scan state (g_nVideoClockVert/Horz, g_pVideoAddress,
 * signal-bit register) inside appletini_ntsc.c globals.
 *
 * The renderer writes 616x224 Apple border-ring frames or 640x400 SHR
 * frames into comp_apple_slot_addr[] (see compositor_layout.h). On every clean
 * Apple frame edge it publishes the just-finished slot via the
 * atomic 3-slot handoff in apple_fb_handoff.[ch]. The compositor
 * claims the freshest published slot whenever it composes an output
 * frame; the handoff guarantees writer/reader ownership cannot
 * collide so no tearing is possible.
 *
 * License: GPLv2 (inherited from AppleWin).
 */

#ifndef APPLE_CYCLE_RENDERER_H
#define APPLE_CYCLE_RENDERER_H

#include <stdint.h>

/* Init: builds tables (~20 ms), zeros buffers, sets MMU attrs. Call once
 * from main(), AFTER apple_cycle_egress_init(). Returns 0 on success. */
int apple_cycle_renderer_init(void);

/* Apple RESET# resets the PL soft-switch manager, but the PS-side renderer
 * keeps local VidHD/C029 state. Call this when the Apple reset sequence
 * changes so the local renderer state comes back as TEXT/C051. */
void apple_cycle_renderer_reset_local_video_state(void);

/* Hook called by 2b.1 once per consumed FIFO record. rec == 0 is a
 * gap marker; otherwise this carries an AppleCycleRecord (see
 * apple_cycle_egress.h for bit layout).
 *
 * Defined as a weak symbol so 2b.1 can call it whether or not 2b.2 is
 * linked in. Default-weak version is a no-op. */
void apple_cycle_renderer_on_record(uint64_t rec);

/* Compositor reads the published Apple frame via apple_fb_reader_claim()
 * (see apple_fb_handoff.h). The renderer publishes via
 * apple_fb_writer_publish() at on_frame_end. Frame indices and the
 * "fresh frame waiting" flag live in a single 32-bit atomic word so the
 * Apple-frame and DVI-frame rates can run independently with frame
 * skip / frame hold semantics. */

/* Diagnostic counters (read-only from outside this module). */
extern volatile uint32_t g_acr_frames_complete;
extern volatile uint32_t g_acr_resyncs_cleared;
extern volatile uint32_t g_acr_records_seen;
extern volatile uint32_t g_acr_cycles_rendered;
extern volatile uint32_t g_acr_unknown_modes;
extern volatile uint32_t g_acr_frame_edges_seen;
extern volatile uint32_t g_acr_last_frame_records;

#endif /* APPLE_CYCLE_RENDERER_H */
