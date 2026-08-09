/* PS-side ALF AD8088 Plus service behind applicard_card.sv. */

#ifndef AD8088_SERVICE_H
#define AD8088_SERVICE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Dedicated 1 MB DDR arena. The Z80 personality owns 0x3F600000-0x3F7FFFFF;
 * keeping the AD8088 at 0x3F800000 prevents live UI changes from exposing
 * one processor's stale RAM through the other processor's memory map. */
#define AD8088_RAM_BASE                 0x3F800000U
#define AD8088_SLICE_INSTRUCTIONS       256U
#define AD8088_WALL_CAP_STANDARD_US     4000U
#define AD8088_WALL_CAP_MAX_US          12000U

void ad8088_service_init(uint32_t uart_base);
void ad8088_service_poll(void);
void ad8088_service_set_enabled(uint8_t enable);
uint8_t ad8088_service_is_enabled(void);
void ad8088_service_set_wall_cap(uint32_t us);
/* Trace mode: one UART line per dispatched monitor command with its parsed
 * parameters, plus reset/abort servicing. Default off. */
void ad8088_service_set_trace(uint8_t enable);
void ad8088_service_set_checkpoint(void (*checkpoint)(void));
void ad8088_service_uart_status(uint32_t uart_base);
/* Last paddle measurements: guest-visible poll count vs wall-clock us. */
void ad8088_service_uart_paddle(uint32_t uart_base);
void ad8088_service_uart_dump(uint32_t uart_base, uint32_t addr,
                              uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* AD8088_SERVICE_H */
