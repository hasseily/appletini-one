#include "video_rom_service.h"

#include <stddef.h>
#include <string.h>

#include "apple_fb_handoff.h"
#include "compositor_layout.h"
#include "xil_cache.h"

#ifndef VIDEO_ROM_SERVICE_SHARED_BUFFER
#define VIDEO_ROM_SERVICE_SHARED_BUFFER \
    ((uint8_t *)(uintptr_t)APPLE_VIDEO_ROM_OVERRIDE_ADDR)
#endif

static uint8_t s_menu_rom[VIDEO_ROM_SERVICE_BYTES];
static uint8_t s_pending_rom[VIDEO_ROM_SERVICE_BYTES];
static uint8_t s_initialized;
static uint8_t s_menu_override;
static uint8_t s_pending;
static uint8_t s_pending_override;
static uint8_t s_pending_custom;
static uint8_t s_published_custom;
static uint8_t s_active_custom;
static uint32_t s_published_generation;
static uint32_t s_active_generation;

void video_rom_service_init(void)
{
    s_menu_override = 0U;
    s_pending = 0U;
    s_pending_override = 0U;
    s_pending_custom = 0U;
    s_published_custom = 0U;
    s_active_custom = 0U;
    s_published_generation = apple_fb_video_rom_gen_get();
    s_active_generation = apple_fb_video_rom_ack_get();
    s_initialized = 1U;
}

void video_rom_service_poll(void)
{
    uint32_t acknowledged;
    uint32_t generation;

    if (s_initialized == 0U) {
        return;
    }
    acknowledged = apple_fb_video_rom_ack_get();
    generation = apple_fb_video_rom_gen_get();
    if (acknowledged == s_published_generation) {
        s_active_generation = acknowledged;
        s_active_custom = s_published_custom;
    }
    if (s_pending == 0U || acknowledged != generation) {
        return;
    }

    /* CPU1 has copied the previous image and rebuilt its private tables.
     * Keep this buffer unchanged from publication until that ACK arrives. */
    if (s_pending_override != 0U) {
        memcpy(VIDEO_ROM_SERVICE_SHARED_BUFFER, s_pending_rom,
               VIDEO_ROM_SERVICE_BYTES);
        Xil_DCacheFlushRange((INTPTR)VIDEO_ROM_SERVICE_SHARED_BUFFER,
                             (INTPTR)VIDEO_ROM_SERVICE_BYTES);
    }
    generation++;
    if (generation == 0U) {
        generation = 1U;
    }
    s_published_generation = generation;
    s_published_custom = s_pending_custom;
    s_pending = 0U;
    apple_fb_video_rom_publish(generation,
        s_pending_override != 0U ? APPLE_FB_VIDEO_ROM_OVERRIDE :
                                  APPLE_FB_VIDEO_ROM_BUILT_IN);
}

uint8_t video_rom_service_apply(const uint8_t *rom)
{
    if (s_initialized == 0U) {
        return VIDEO_ROM_SERVICE_UNAVAILABLE;
    }
    video_rom_service_poll();
    if (s_pending != 0U || s_active_generation != s_published_generation) {
        return VIDEO_ROM_SERVICE_BUSY;
    }
    s_pending_override = rom != NULL ? 1U : s_menu_override;
    s_pending_custom = rom != NULL ? 1U : 0U;
    if (s_pending_override != 0U) {
        memcpy(s_pending_rom, rom != NULL ? rom : s_menu_rom,
               VIDEO_ROM_SERVICE_BYTES);
    }
    s_pending = 1U;
    video_rom_service_poll();
    return VIDEO_ROM_SERVICE_OK;
}

void video_rom_service_set_menu(const uint8_t *rom)
{
    if (s_initialized == 0U) {
        return;
    }
    s_menu_override = rom != NULL ? 1U : 0U;
    if (rom != NULL) {
        memcpy(s_menu_rom, rom, VIDEO_ROM_SERVICE_BYTES);
        memcpy(s_pending_rom, rom, VIDEO_ROM_SERVICE_BYTES);
    }
    s_pending_override = s_menu_override;
    s_pending_custom = 0U;
    s_pending = 1U;
    video_rom_service_poll();
}

void video_rom_service_status(uint16_t *flags, uint32_t *generation)
{
    uint16_t value = 0U;

    video_rom_service_poll();
    if (s_initialized != 0U) {
        value = VIDEO_ROM_SERVICE_AVAILABLE;
        if (s_active_custom != 0U) {
            value |= VIDEO_ROM_SERVICE_CUSTOM;
        }
        if (s_pending != 0U || s_active_generation != s_published_generation) {
            value |= VIDEO_ROM_SERVICE_PENDING;
        }
    }
    if (flags != NULL) {
        *flags = value;
    }
    if (generation != NULL) {
        *generation = s_initialized != 0U ? s_active_generation : 0U;
    }
}
