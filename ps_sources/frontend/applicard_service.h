/* PCPI Applicard service: PS-side Z80 coprocessor behind the slot-5 PL
 * latches (applicard_card.sv). The Z80 core runs in bounded T-state
 * slices from the CPU0 main loop; the handshake protocol makes the
 * pacing invisible to Apple-side software.
 *
 * Z80 memory lives at APPLICARD_Z80_RAM_BASE in ordinary *cacheable* DDR:
 * no PL master ever touches it, only this emulator, so it needs no TLB
 * marking and no cache maintenance.
 */

#ifndef APPLICARD_SERVICE_H
#define APPLICARD_SERVICE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 2 MB (32 x 64 KB banks) inside the free 0x3F600000+ DDR window (see the
 * region table in README_APPLICARD.md and compositor_layout.h
 * neighbors): 0x3F600000-0x3F7FFFFF. */
#define APPLICARD_Z80_RAM_BASE   0x3F600000U

/* Default Z80Emulate slice budget in T-states per slice. */
#define APPLICARD_DEFAULT_SLICE_TSTATES  20000U

/* Wall-clock cap for back-to-back slices per main-loop poll. Slices are
 * separated by the USB0 priority checkpoint, so this bounds how long the
 * rest of the main loop waits, not how long USB0 waits. The two presets
 * back the config menu's "Resource usage" item; `z80 wall <us>` can set
 * anything in the 200..20000 clamp for experiments. Duty is roughly
 * wall/(wall + main-loop pass): ~80%% at 4 ms, ~86%% at 12 ms. */
#define APPLICARD_WALL_CAP_STANDARD_US   4000U
#define APPLICARD_WALL_CAP_MAX_US        12000U
#define APPLICARD_DEFAULT_WALL_CAP_US    APPLICARD_WALL_CAP_STANDARD_US

void applicard_service_init(uint32_t uart_base);
void applicard_service_poll(void);

/* Arm/disarm the emulator (mirrors the slot-5 enable). Enabling resets
 * the Z80 with the boot ROM mapped. */
void applicard_service_set_enabled(uint8_t enable);
uint8_t applicard_service_is_enabled(void);

void applicard_service_request_reset(void);
void applicard_service_set_budget(uint32_t tstates);
void applicard_service_set_wall_cap(uint32_t us);

/* Called between compute-burst slices so USB0 keeps its service latency
 * (main.c passes usb0_priority_checkpoint). */
void applicard_service_set_checkpoint(void (*checkpoint)(void));

/* UART debug helpers (the `z80` command group). */
void applicard_service_uart_status(uint32_t uart_base);
void applicard_service_uart_dump(uint32_t uart_base, uint32_t addr,
                                 uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* APPLICARD_SERVICE_H */
