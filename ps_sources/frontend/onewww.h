#ifndef ONEWWW_H
#define ONEWWW_H

#include <stdint.h>

/* ONEWWW service, SmartPort private selector $88, unit 0.
 *
 * The card gets a web page, lays it out and draws it. It writes the
 * 640 x 400 picture into CPU1's copy of the Apple video memory. The Apple
 * switches Super Hi-Res on ($C029 = $C1) and sends keys and the mouse.
 *
 * CONTROL payload: version (1), operation, then:
 *   OPEN  = 1: one byte, ignored. Shows the home page.
 *   CLOSE = 2: one byte, ignored. Gives the screen back to the Apple.
 *   KEY   = 3: one byte, the Apple key code (the high bit is ignored).
 *   MOUSE = 4: x (LE16, 0..639), y (LE16, 0..399), buttons (bit 0 = down).
 * STATUS returns 16 bytes: "ATNW", version, state, shown generation, 0,
 * page rows (LE32), top row (LE32). State: 0 closed, 1 page, 2 address
 * entry, 3 loading, 4 layout, 5 style sheets. The card closes on the Q key: the Apple
 * reads the state after each key and stops when it is 0. */
#define ONEWWW_SELECTOR 0x88U

uint8_t onewww_control(const uint8_t *payload, uint16_t length);
uint16_t onewww_status(uint8_t *out, uint16_t capacity);
void onewww_poll(void);
/* A page is loading or its layout runs: repaint the screen less often. */
uint8_t onewww_busy(void);
void onewww_reset(void);

#endif
