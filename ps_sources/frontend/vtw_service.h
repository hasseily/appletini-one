#ifndef VTW_SERVICE_H
#define VTW_SERVICE_H

/* Virtual TransWarp service (CPU0).
 *
 * Thin by design (README_VIRTUAL_TRANSWARP.md): the PL owns the 65C02 core,
 * shadow memory, and bus engine. This service sequences the session --
 * waiting for a positive //e identification, taking the bus, copying the
 * motherboard ROM into the shadow over real bus cycles, then releasing the
 * core -- and exposes configuration plus `vtw status` diagnostics.
 *
 * The ROM copy runs incrementally from vtw_service_poll() so USB/audio
 * polling never stalls; a full copy takes a few dozen poll passes.
 *
 * Disabling the session releases /DMA at a PHI1 boundary, but the frozen
 * motherboard 6502 resumes from stale context: the user must CTRL-RESET,
 * exactly like the real TransWarp's off-until-reset semantics. */

#include <stdint.h>

void vtw_service_init(uint32_t uart_base);

/* Configuration intent (persisted by the config menu). The session engages
 * only once the boot ROM has positively identified a //e. */
void vtw_service_set_enabled(uint8_t enable);
uint8_t vtw_service_is_enabled(void);

/* speed_mode: CARD_CTRL_VTW_SPEED_*; pace_divider: fabric clocks per core
 * cycle in divided mode (min 2 = full rate; 37 ~= 3.6 MHz-equivalent). */
void vtw_service_set_speed(uint8_t speed_mode, uint8_t pace_divider);
uint8_t vtw_service_speed_mode(void);
uint8_t vtw_service_pace_divider(void);

/* Ignore all software writes to $C074, including fast, 1 MHz, and disable.
 * Enabling it live clears any value that software latched earlier. */
void vtw_service_set_ignore_c074(uint8_t ignore);

/* Force virtual Disk II accesses through the original physical 1 MHz path. */
void vtw_service_set_disk2_accel_disabled(uint8_t disable);

/* Per-region slowdown (TransWarp DIP block 2): region_mask bits are
 * CARD_CTRL_VTW_SLOWDOWN_* (slots 1-7, floating-bus/video timing, paddle);
 * cycles is the 1 MHz window per region access (0 disables). */
void vtw_service_set_slowdown(uint16_t region_mask, uint16_t cycles);

/* Runtime speed overrides (USB keymap actions; $C074-style, never
 * persisted, cleared by a configured speed change). */
void vtw_service_speed_toggle(void);   /* 1 MHz <-> configured        */
void vtw_service_speed_step(int8_t dir); /* preset ladder, +1/-1      */
void vtw_service_slug_toggle(void);    /* 0.05 MHz <-> configured     */
/* Slug key arming (TransWarp tab, default off). Disarming while slug
 * is active restores the configured speed. */
void vtw_service_set_slug_enabled(uint8_t enable);
/* One-line result of the last speed action, for the display overlay. */
const char *vtw_service_last_action_text(void);

/* 1 while the vTW owns the Apple bus (session active past takeover). */
uint8_t vtw_service_session_active(void);

void vtw_service_poll(void);
void vtw_service_uart_status(uint32_t uart_base);

/* Hex-dump `len` bytes of the shadow (18-bit physical space: $00000 main,
 * $10000 aux, $20000 ROM copy at $C000-based offsets) over the UART.
 * Reads via shadow port B; safe while the core runs. */
void vtw_service_uart_dump(uint32_t uart_base, uint32_t phys, uint32_t len);

#endif /* VTW_SERVICE_H */
