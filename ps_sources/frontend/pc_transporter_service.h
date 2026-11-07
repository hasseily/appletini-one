#ifndef PC_TRANSPORTER_SERVICE_H
#define PC_TRANSPORTER_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#define PCT_DDR_BASE 0x3F900000U

void pc_transporter_service_init(uint32_t uart_base);
void pc_transporter_service_set_enabled(bool enabled);
void pc_transporter_service_poll(void);
void pc_transporter_service_uart_status(uint32_t uart_base);
bool pc_transporter_service_video_active(void);
bool pc_transporter_service_keyboard_report(uint8_t slot, uint8_t modifiers,
                                            const uint8_t *keys, uint32_t count,
                                            bool capture);

#endif
