#ifndef DEBUG_OVERLAY_H
#define DEBUG_OVERLAY_H

#include <stdint.h>

#include "../lib/rtc_pcf8563.h"
#include "../lib/tmp102.h"
#include "disk2_service.h"
#include "smartport_service.h"
#include "usb_hid_service.h"
#include "usb_storage_service.h"

typedef struct {
    int x;
    int y;
    int w;
    int h;
} debug_overlay_rect_t;

typedef struct {
    char firmware_version[32];
    char golden_version[32];
    char bezel_status[160];

    uint8_t metadata_valid;
    uint8_t show_bezel;
    uint8_t usb_menu_owned;
    uint8_t config_menu_active;
    uint8_t boot_device;
    uint8_t settings_loaded;
    uint8_t session_only;

    tmp102_temp_t temp;
    rtc_pcf8563_time_t rtc;

    uint32_t ui_frame_count;
    uint32_t key_count;
    uint32_t fps_x100;
    uint32_t apple_fps_x100;
    uint32_t compositor_fps_x100;
    uint32_t renderer_fps_x100;
    uint32_t draw_us;

    uint8_t apple_mode;
    uint8_t apple_video_50hz;
    uint8_t scanlines_mode;
    uint8_t video_output_mono;
    uint8_t video_mono_color;
    uint8_t video_color_mode;
    uint8_t video7_auto_mono;
    uint8_t video_ghosting_strength;

    uint32_t compositor_frames_published;
    uint32_t compositor_frames_skipped;
    uint32_t compositor_apple_blits;
    uint32_t renderer_publish_seq;
    uint32_t compositor_ui_us;
    uint32_t compositor_apple_us;
    uint32_t compositor_sync_us;
    uint32_t compositor_total_us;
    uint32_t compositor_apple_drawn;
    uint32_t compositor_suppress_apple;
    uint32_t fb_vblank_count;
    uint32_t fb_last_latched;
    uint32_t fb_debug;
    uint32_t fb_debug2;
    uint32_t softswitch_state;

    uint8_t disk2_valid;
    disk2_activity_t disk2_activity;
    uint8_t disk2_read_only[DISK2_DRIVE_COUNT];

    uint8_t smartport_valid;
    smartport_activity_t smartport_activity;

    usb_hid_service_status_t hid_status;
    usb_storage_service_stats_t usb_storage_stats;
    uint8_t usb_storage_attention;
} debug_overlay_snapshot_t;

uint32_t debug_overlay_region_count(void);
debug_overlay_rect_t debug_overlay_region(uint32_t index);
void debug_overlay_draw(uint16_t *fb, const debug_overlay_snapshot_t *snapshot);

#endif /* DEBUG_OVERLAY_H */
