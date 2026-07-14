/*
 * supersprite_vdp.h -- software TMS9918A renderer for the SuperSprite card.
 *
 * The PL front end (supersprite_card.sv) owns the real-time TMS9918 register
 * and VRAM interface at slot 7 ($C0Fx) and exposes VRAM + registers + status
 * to the PS through the card-control register window (CARD_CTRL_SS_*). This
 * module (runs on CPU0, the compositor side) reads that state and renders the
 * VDP picture into a 256x192 BGRA32 buffer. The compositor black-keys the
 * result over the Apple subwindow: a VDP pixel that is black lets the Apple
 * video show through (the SuperSprite's black-level overlay behavior).
 */

#ifndef SUPERSPRITE_VDP_H
#define SUPERSPRITE_VDP_H

#include <stdint.h>

#define SS_VDP_WIDTH   256
#define SS_VDP_HEIGHT  192

typedef struct {
    uint8_t         active;       /* card enabled and overlay on */
    uint8_t         apple_video;  /* 1 = Apple video visible under the overlay */
    uint8_t         changed;      /* 1 if re-rendered this call (VDP frame advanced) */
    const uint32_t *pixels;       /* SS_VDP_WIDTH*SS_VDP_HEIGHT BGRA32, or NULL */
} ss_vdp_frame_t;

/* Sample the VDP state and re-render (rate-limited by a wall clock). Returns
 * the current frame descriptor. Cheap to call every compositor tick: it
 * early-outs when the card is disabled, when the wall-clock interval since the
 * last render has not elapsed, or while the display is blanked (holding the
 * last frame). */
const ss_vdp_frame_t *supersprite_vdp_render(void);

/* Debug override: when on, force the overlay active regardless of the VDP
 * overlay switch ($C0n6) and re-render every call (bypass the rate limit).
 * This distinguishes switch/gate failures from renderer failures. Default off. */
void supersprite_vdp_set_force_active(uint8_t on);
uint8_t supersprite_vdp_get_force_active(void);

#endif /* SUPERSPRITE_VDP_H */
