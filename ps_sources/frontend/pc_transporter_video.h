#ifndef PC_TRANSPORTER_VIDEO_H
#define PC_TRANSPORTER_VIDEO_H

#include <stdint.h>

#define PCT_CGA_FONT_OFFSET 0xAFA6EU
typedef struct {
    uint16_t start_address;
    uint16_t cursor_address;
    uint8_t cursor_start;
    uint8_t cursor_end;
    uint8_t mode;
    uint8_t color;
    /* Raw CRTC R1, R6 and R9 for graphics. An unset width or row count
     * selects the standard CGA geometry. */
    uint8_t horizontal_displayed;
    uint8_t vertical_displayed;
    uint8_t maximum_scanline;
    uint8_t graphics_linear;
} pc_transporter_video_state_t;

/* Draw into a rectangle that fits the caller's framebuffer. The font is
 * the 128-character, 8-byte-per-character table uploaded with AEPCBIOS.
 * With graphics_linear set, vram contains packed raster rows matching the
 * supplied CRTC geometry, with no start-address offset or bank padding.
 * The CGA frame counter drives text attribute and cursor blink.
 * An optional checkpoint lets the guest run between groups of rows. */
void pc_transporter_video_draw(uint16_t *dest, unsigned stride,
                               unsigned width, unsigned height,
                               const pc_transporter_video_state_t *state,
                               const volatile uint8_t *vram,
                               const volatile uint8_t *font,
                               uint32_t frame, void (*checkpoint)(void));

#endif /* PC_TRANSPORTER_VIDEO_H */
