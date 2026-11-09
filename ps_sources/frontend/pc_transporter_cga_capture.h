#ifndef PC_TRANSPORTER_CGA_CAPTURE_H
#define PC_TRANSPORTER_CGA_CAPTURE_H

#include <stdint.h>

/* CPU0 clears the handoff before releasing CPU1. */
void pc_transporter_cga_capture_reset(void);

/* CPU1 maps the shared section, then polls it from its foreground loop. */
void pc_transporter_cga_capture_init(void);
void pc_transporter_cga_capture_poll(void);

/* CPU0 owns a successful claim until release. Each row is packed in display
 * order with two bytes per horizontal displayed CRTC character. */
const volatile uint8_t *pc_transporter_cga_capture_claim(uint32_t state,
                                                        uint32_t geometry,
                                                        uint32_t crtc_low);
void pc_transporter_cga_capture_release(void);

/* Counts observed frames since the CPU0 handoff reset. */
void pc_transporter_cga_capture_get_stats(uint32_t *completed, uint32_t *dropped);

#endif
