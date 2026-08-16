/*
 * apple_cycle_renderer.h -- Per-cycle Apple //e renderer driving
 * AppleWin's NTSC pixel-emission core (appletini_ntsc.c). Ported from
 * AppleWin source/NTSC.cpp; behavior matches that emulator at the
 * cycle level.
 *
 * apple_cycle_egress.c calls apple_cycle_renderer_on_record_lookahead() once
 * per record consumed from the FIFO, in order. The renderer maintains its
 * own per-cycle scan state (g_nVideoClockVert/Horz, g_pVideoAddress,
 * signal-bit register) inside appletini_ntsc.c globals.
 *
 * The renderer writes 616x224 Apple border-ring frames or 640x400 SHR
 * frames into comp_apple_slot_addr[] (see compositor_layout.h). It publishes
 * completed frames through the atomic 3-slot handoff in apple_fb_handoff.[ch].
 * Static SHR shadows reuse the last published slot. The compositor claims the
 * freshest slot whenever it composes an output frame; writer and reader slots
 * cannot collide.
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

/* CPU1 sets this from the PL's effective vTW 1 MHz status before draining
 * each batch. It selects the vTW capture normalization applied before the
 * common scanner-phase correction. */
void apple_cycle_renderer_set_vtw_1mhz(uint8_t active);

/* CPU1 passes the PL's authoritative fake-SHR capture state each
 * batch; the renderer repairs its local C029 shadow if a capture
 * drop ate the C029 record (screen-freeze self-heal). */
void apple_cycle_renderer_sync_shr_mode(uint8_t pl_shr_active);

/* Egress hook: a captured aux write to $9DF8 (SDD paged-mode ctrl)
 * just landed in the mirror. Opens/closes the vTW main $6000-$9FFF
 * posting window immediately so a second interlace field loaded while
 * SHR is off still reaches the capture. */
void apple_cycle_renderer_note_aux_ctrl_write(uint8_t value);

/* True when a legacy frame record needs two future frame snapshots for its
 * per-switch scanner-phase correction. Fake SHR emits one marker per frame
 * and never takes this path. */
int apple_cycle_renderer_needs_phase_lookahead(uint64_t rec);

/* Hook called by 2b.1 with the current record and up to two consecutive
 * future legacy frame records. A zero future record means unavailable. The
 * future records supply switch bits only; egress has not applied their video
 * memory writes, so the current cycle cannot see future pixel data. */
void apple_cycle_renderer_on_record_lookahead(uint64_t rec,
                                              uint64_t next1,
                                              uint64_t next2);

/* Hook called by 2b.1 once per consumed FIFO record. rec == 0 is a
 * gap marker; otherwise this carries an AppleCycleRecord (see
 * apple_cycle_egress.h for bit layout).
 *
 * Defined as a weak symbol so 2b.1 can call it whether or not 2b.2 is
 * linked in. Default-weak version is a no-op. */
void apple_cycle_renderer_on_record(uint64_t rec);

/* Last phase-corrected switch word. Used by the host timing test and useful
 * for a precise UART diagnostic without exposing renderer internals. */
uint32_t apple_cycle_renderer_debug_phase_switches(void);

/* Compositor reads the published Apple frame via apple_fb_reader_claim()
 * (see apple_fb_handoff.h). Normal legacy video publishes at frame end.
 * Full shadow renders, such as SHR and legacy interlace, publish as soon as
 * that render finishes at frame start. Frame indices and the "fresh frame
 * waiting" flag live in one 32-bit atomic word. */

/* Diagnostic: print one UART0 line with the A2Li hole bytes, gate
 * inputs, and last frame mode, from CPU1's coherent shadow view.
 * Triggered by the CPU0 console command "shadow a2li". */
void apple_cycle_renderer_debug_a2li_line(void);

/* Diagnostic counters (read-only from outside this module). */
extern volatile uint32_t g_acr_frames_complete;
extern volatile uint32_t g_acr_resyncs_cleared;
extern volatile uint32_t g_acr_records_seen;
extern volatile uint32_t g_acr_cycles_rendered;
extern volatile uint32_t g_acr_unknown_modes;
/* Frames rendered with the SHR4 extended decode active (magic at aux
 * $9DFC). Climbing = software is using SDD-style SHR4 modes. */
extern volatile uint32_t g_acr_shr4_frames;
/* Static SHR frame markers skipped and changed SHR frames rebuilt. */
extern volatile uint32_t g_acr_shr_frames_skipped;
extern volatile uint32_t g_acr_shr_cache_rebuilds;
/* Static full-shadow legacy frames skipped and changed frames rebuilt. */
extern volatile uint32_t g_acr_legacy_frames_skipped;
extern volatile uint32_t g_acr_legacy_cache_rebuilds;
extern volatile uint32_t g_acr_frame_edges_seen;
extern volatile uint32_t g_acr_last_frame_records;

#endif /* APPLE_CYCLE_RENDERER_H */
