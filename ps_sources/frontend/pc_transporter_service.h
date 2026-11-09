#ifndef PC_TRANSPORTER_SERVICE_H
#define PC_TRANSPORTER_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "pc_transporter_memory.h"

void pc_transporter_service_init(uint32_t uart_base);
void pc_transporter_service_set_enabled(bool enabled);
void pc_transporter_service_set_maximum_speed(bool maximum);
void pc_transporter_service_poll(void);
void pc_transporter_service_video_poll(void);
void pc_transporter_service_i2c_poll(void);
void pc_transporter_service_uart_status(uint32_t uart_base);
bool pc_transporter_service_guest_running(void);
/* Return zero when the compositor should show Apple video. */
uint32_t pc_transporter_service_cga_state(void);
/* Return true when the Transporter takes exclusive USB keyboard input. */
bool pc_transporter_service_keyboard_report(uint8_t slot, uint8_t modifiers,
                                            const uint8_t *keys, uint32_t count,
                                            bool capture);

#endif
