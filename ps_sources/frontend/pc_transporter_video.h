#ifndef PC_TRANSPORTER_VIDEO_H
#define PC_TRANSPORTER_VIDEO_H

#include <stdint.h>

#define PCT_CGA_FONT_OFFSET 0xAFA6EU
#define PCT_CGA_VIDEO_ENABLE 0x08U

typedef struct {
    uint16_t start_address;
    uint16_t cursor_address;
    uint8_t cursor_start;
    uint8_t cursor_end;
    uint8_t mode;
    uint8_t color;
} pc_transporter_video_state_t;

/* Draw into a rectangle that fits the caller's framebuffer. The font is
 * the 128-character, 8-byte-per-character table uploaded with AEPCBIOS. */
void pc_transporter_video_draw(uint16_t *dest, unsigned stride,
                               unsigned width, unsigned height,
                               const pc_transporter_video_state_t *state,
                               const volatile uint8_t *vram,
                               const volatile uint8_t *font,
                               uint32_t frame);

#endif /* PC_TRANSPORTER_VIDEO_H */
