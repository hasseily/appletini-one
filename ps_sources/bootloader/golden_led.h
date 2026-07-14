#ifndef BOOT_UPDATER_GOLDEN_LED_H
#define BOOT_UPDATER_GOLDEN_LED_H

/*
 * golden_led -- status LED for the golden bootloader.
 *
 * Drives a single active-high status LED on PS MIO0 (bank 500, ball G6 on
 * the respun board: MIO0 -> series R -> LED -> GND). MIO0 is muxed to GPIO
 * by ps7_init (PCW config), so this module only touches the PS GPIO
 * controller, not the SLCR MIO mux.
 *
 * The golden updater is single-threaded and fully blocking, so there is no
 * background blinker: callers drive the LED explicitly around each phase
 * (solid on for erase/metadata, toggle-per-chunk during program/verify).
 * The error-code blinker is blocking by design -- it runs only at a dead
 * end (failed update before dropping into the serial monitor), where
 * stealing CPU time costs nothing.
 */

#include <stdint.h>

/* Configure MIO0 as a GPIO output and drive the LED off. Call once at
 * boot before using the other functions. */
void golden_led_init(void);

void golden_led_on(void);
void golden_led_off(void);
void golden_led_toggle(void);

/* Blink `code` (an updater error code) as a repeated pattern: `code` short
 * blinks, a long gap, repeated a few times, then return. Blocking. Leaves
 * the LED off. `code` <= 0 is treated as a single blink. */
void golden_led_error_code(int code);

#endif /* BOOT_UPDATER_GOLDEN_LED_H */
