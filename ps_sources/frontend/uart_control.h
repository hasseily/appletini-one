#ifndef UART_CONTROL_H
#define UART_CONTROL_H

#include <stdint.h>

#include "../lib/rtc_pcf8563.h"
#include "../lib/tmp102.h"
#include "scanlines.h"

typedef enum {
    UI_KEY_NONE = 0,
    UI_KEY_UP,
    UI_KEY_DOWN,
    UI_KEY_LEFT,
    UI_KEY_RIGHT,
    UI_KEY_ENTER,
    UI_KEY_BACK,
    UI_KEY_TOGGLE,
    UI_KEY_SCANLINES,
    UI_KEY_TAB,
    UI_KEY_SHIFT_TAB,
    UI_KEY_PAGE_UP,
    UI_KEY_PAGE_DOWN,
    UI_KEY_SPACE,
    UI_KEY_ESC,
    UI_KEY_MENU
} ui_key_t;

typedef struct {
    ui_key_t key;
    uint8_t pressed;
    uint8_t ascii;
} ui_input_t;

typedef struct {
    uint32_t frame_count;
    uint32_t key_count;
    uint8_t config_boot_device;
    uint8_t config_boot_timeout_mode;
    uint8_t config_disk2_slot6_enabled;
    uint8_t config_settings_loaded;
    uint8_t config_session_only;
    uint8_t vsync_enable;
    uint8_t scanlines_mode;
    uint8_t text_mono_fg_color;
    uint8_t text_mono_bg_color;
    uint8_t display_mono_enable;
    uint8_t display_mono_color;
    uint32_t apple_fb_slot;
    uint32_t apple_fb_mode;
    uint8_t apple_video_50hz;
    uint32_t fps_x100;
    uint32_t apple_fps_x100;
    uint32_t apple_fb_blits;
    uint32_t compositor_frames_published;
    uint32_t compositor_frames_skipped;
    uint32_t fb_frame_count;
    uint32_t fb_last_latched;
    uint8_t i2c_ready;
    uint8_t tmp102_ready;
    tmp102_temp_t temp;
    rtc_pcf8563_time_t rtc;
    uint8_t audio_enable;
    uint8_t audio_mute;
    uint32_t audio_tone_hz;
    uint32_t audio_amp;
    uint32_t audio_clkcnt;
    uint32_t audio_q3lvl;
    uint32_t audio_q3tog;
    uint8_t updater_meta_valid;
    char updater_golden_version[32];
    char updater_firmware_version[32];
} uart_control_snapshot_t;

typedef struct {
    void *ctx;
    int (*get_snapshot)(void *ctx, uart_control_snapshot_t *snapshot);
    int (*set_rtc)(void *ctx, const rtc_pcf8563_time_t *time);
    void (*set_vsync)(void *ctx, uint8_t enable);
    void (*set_scanlines)(void *ctx, uint8_t mode);
    void (*set_text_mono_colors)(void *ctx, uint8_t fg_color, uint8_t bg_color);
    void (*set_display_mono)(void *ctx, uint8_t enable, uint8_t color);
    void (*set_audio_enable)(void *ctx, uint8_t enable);
    void (*set_audio_mute)(void *ctx, uint8_t mute);
    void (*set_audio_tone_hz)(void *ctx, uint32_t tone_hz);
    void (*set_audio_amp)(void *ctx, uint32_t amp);
    void (*reboot_system)(void *ctx);
    void (*psram_qpi)(void *ctx);
    void (*psram_qpi_exit)(void *ctx);
    void (*psram_qspi_read)(void *ctx, const char* addr);
    void (*psram_qspi_write)(void *ctx, const char* addr, const char* datahi, const char* datalo);
    void (*psram_spi_read)(void *ctx, const char* addr);
    void (*psram_spi_write)(void *ctx, const char* addr, const char* datahi, const char* datalo);
    void (*psram_reset)(void *ctx);
    void (*psram_toggle_wrap)(void *ctx);
    void (*psram_set_delay)(void *ctx, const char* delay);
    void (*psram_scan_delay)(void *ctx, const char* addr);
    void (*psram_read_id)(void *ctx);
    int (*smartport_reset_media)(void *ctx);
    int (*smartport_read_block)(void *ctx,
                                uint32_t block_num,
                                uint8_t *buffer,
                                uint32_t count,
                                uint32_t *actual_out);
} uart_control_ops_t;

typedef struct {
    ui_input_t input;
    uint8_t request_redraw;
} uart_control_event_t;

typedef struct {
    uint32_t control_uart_base;
    uint32_t debug_uart_base;
    uint8_t cmd_mode;
    uint32_t cmd_len;
    char cmd_buf[128];
} uart_control_t;

void uart_control_init(uart_control_t *control, uint32_t control_uart_base, uint32_t debug_uart_base);

/* Bind the config menu so the sdd command keeps the persisted setting
 * in sync with the live USB0 personality (void* to avoid a circular
 * include; config_menu.h includes this header). */
void uart_control_bind_config_menu(void *menu);
void uart_control_print_help(const uart_control_t *control, const uart_control_ops_t *ops);
uart_control_event_t uart_control_poll(uart_control_t *control, const uart_control_ops_t *ops);
int uart_control_has_pending_input(const uart_control_t *control);
const char *uart_control_key_name(ui_key_t key);

#endif
