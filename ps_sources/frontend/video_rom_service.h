#ifndef VIDEO_ROM_SERVICE_H
#define VIDEO_ROM_SERVICE_H

#include <stdint.h>

#define VIDEO_ROM_SERVICE_BYTES        4096U
#define VIDEO_ROM_SERVICE_OK           0x00U
#define VIDEO_ROM_SERVICE_BUSY         0x66U
#define VIDEO_ROM_SERVICE_UNAVAILABLE  0x60U
#define VIDEO_ROM_SERVICE_AVAILABLE   0x0001U
#define VIDEO_ROM_SERVICE_CUSTOM       0x0002U
#define VIDEO_ROM_SERVICE_PENDING      0x0004U

/* CPU0 only. Call after apple_fb_handoff_init(), before loading menu settings. */
void video_rom_service_init(void);
void video_rom_service_poll(void);

/* Copy a complete software ROM; NULL restores the cached menu ROM. A BUSY
 * result leaves both the request and the active ROM unchanged. The renderer
 * applies accepted requests at a frame boundary. */
uint8_t video_rom_service_apply(const uint8_t *rom);

/* Cache the menu selection independently from software uploads. NULL selects
 * the built-in ROM. Menu changes replace any queued software request, but
 * never overwrite a shared buffer that CPU1 has not acknowledged. */
void video_rom_service_set_menu(const uint8_t *rom);

/* CUSTOM means an acknowledged software upload, not an SD menu override.
 * Generation is the renderer's last acknowledged generation. */
void video_rom_service_status(uint16_t *flags, uint32_t *generation);

#endif
