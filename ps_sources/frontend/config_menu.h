#ifndef CONFIG_MENU_H
#define CONFIG_MENU_H

#include <stddef.h>
#include <stdint.h>

#include "../lib/rtc_pcf8563.h"
#include "uart_control.h"
#include "uthernet2_control.h"
#include "usb_hid_service.h"

#define CONFIG_MENU_STATUS_LEN 96U
#define CONFIG_MENU_PATH_LEN 128U
#define CONFIG_MENU_USB_BIND_ACTION_COUNT 10U
#define CONFIG_MENU_USB_BIND_CAPTURE_NONE 0xFFU
#define CONFIG_MENU_BOOT_USB_BIND_RESET_ITEM 2U
#define CONFIG_MENU_BOOT_USB_BIND_FIRST_ITEM (CONFIG_MENU_BOOT_USB_BIND_RESET_ITEM + 1U)
#define CONFIG_MENU_BOOT_ITEM_COUNT \
    (CONFIG_MENU_BOOT_USB_BIND_FIRST_ITEM + CONFIG_MENU_USB_BIND_ACTION_COUNT)
#define CONFIG_MENU_PROFILE_ITEM_COUNT 5U
#define CONFIG_MENU_ETHERNET_ADDRESS_STATIC 0U
#define CONFIG_MENU_ETHERNET_ADDRESS_DHCP 1U

typedef enum {
    CONFIG_MENU_USB_BIND_ACTION_UP = 0,
    CONFIG_MENU_USB_BIND_ACTION_DOWN,
    CONFIG_MENU_USB_BIND_ACTION_LEFT,
    CONFIG_MENU_USB_BIND_ACTION_RIGHT,
    CONFIG_MENU_USB_BIND_ACTION_TAB_UP,
    CONFIG_MENU_USB_BIND_ACTION_TAB_DOWN,
    CONFIG_MENU_USB_BIND_ACTION_SCREENSHOT_A2,
    CONFIG_MENU_USB_BIND_ACTION_SCREENSHOT_1080P,
    CONFIG_MENU_USB_BIND_ACTION_OK,
    CONFIG_MENU_USB_BIND_ACTION_BACK
} config_menu_usb_bind_action_t;

typedef struct {
    void *ctx;
    void (*set_scanlines)(void *ctx, uint8_t mode);
    uint8_t (*get_scanlines)(void *ctx);
    void (*set_video_ghosting)(void *ctx, uint8_t strength);
    uint8_t (*get_video_ghosting)(void *ctx);
    void (*set_border)(void *ctx, uint8_t enabled, uint8_t color, uint8_t flood);
    uint8_t (*get_border_enabled)(void *ctx);
    uint8_t (*get_border_color)(void *ctx);
    uint8_t (*get_border_flood)(void *ctx);
    void (*set_video_output)(void *ctx,
                             uint8_t mono_enable,
                             uint8_t mono_color,
                             uint8_t color_mode,
                             uint8_t video7_auto_mono_enable,
                             int8_t clean_phase_cycles,
                             int8_t pal_phase_cycles);
    uint8_t (*get_video_output_mono)(void *ctx);
    uint8_t (*get_video_output_mono_color)(void *ctx);
    uint8_t (*get_video_output_color_mode)(void *ctx);
    uint8_t (*get_video7_auto_mono_enabled)(void *ctx);
    int8_t (*get_clean_video_phase_cycles)(void *ctx);
    int8_t (*get_pal_video_phase_cycles)(void *ctx);
    uint8_t (*is_apple_video_50hz)(void *ctx);
    void (*set_boot_timeout_ticks)(void *ctx, uint32_t ticks);
    void (*set_boot_handoff)(void *ctx, uint8_t handoff);
    void (*set_clock_enabled)(void *ctx, uint8_t enable);
    void (*set_supersprite_enabled)(void *ctx, uint8_t enable);
    void (*set_sdd_stream_enabled)(void *ctx, uint8_t enable);
    void (*set_usb0_sd_remote_mount)(void *ctx, uint8_t enable);
    void (*set_slot_enabled)(void *ctx, uint8_t slot, uint8_t enable);
    uint8_t (*get_slot_enabled)(void *ctx, uint8_t slot);
    void (*set_applicard_resource_max)(void *ctx, uint8_t maximum);
    void (*set_phasor_pan)(void *ctx, uint32_t pan_lo, uint32_t pan_hi);
    void (*set_phasor_audio)(void *ctx,
                             int8_t bass,
                             int8_t mid,
                             int8_t treble,
                             int8_t warmth,
                             int8_t volume,
                             uint8_t psg_ay_mode,
                             uint8_t mockingboard_only);
    void (*set_mouse_sensitivity)(void *ctx, uint8_t sensitivity);
    void (*set_disk2_sound_volume)(void *ctx, uint8_t volume);
    void (*play_disk2_sound_event)(void *ctx, uint8_t event);
    int (*set_smartport_image_path)(void *ctx, uint8_t device, const char *path);
    int (*reset_smartport_media)(void *ctx, uint8_t device);
    int (*set_disk2_image_path)(void *ctx, uint8_t drive, const char *path);
    int (*reset_disk2_media)(void *ctx, uint8_t drive);
    uint8_t (*get_disk2_image_read_only)(void *ctx, uint8_t drive);
    int (*set_bezel_path)(void *ctx, const char *path);
    int (*read_rtc)(void *ctx, rtc_pcf8563_time_t *time);
    int (*write_rtc)(void *ctx, const rtc_pcf8563_time_t *time);
    int (*ethernet_read_config)(void *ctx, uthernet2_network_config_t *config);
    int (*ethernet_write_config)(void *ctx,
                                 const uthernet2_network_config_t *config);
    int (*ethernet_test)(void *ctx, uthernet2_test_result_t *result);
    int (*ethernet_dhcp_acquire)(void *ctx,
                                 const uint8_t mac[UTHERNET2_MAC_LEN],
                                 uthernet2_network_config_t *lease,
                                 char *detail,
                                 size_t detail_len);
} config_menu_platform_t;

typedef struct {
    uint8_t active;
    uint32_t tab;
    uint32_t item_focus;
    uint8_t boot_timeout_mode;
    uint8_t boot_device;
    uint8_t scanlines_mode;
    uint8_t video_output_mono;
    uint8_t video_mono_color;
    uint8_t video_color_mode;
    uint8_t video7_auto_mono_enabled;
    uint8_t video_ghosting_strength;
    uint8_t border_enabled;
    uint8_t border_color;
    uint8_t border_flood;
    int8_t clean_video_phase_cycles;
    int8_t pal_video_phase_cycles;
    char video_rom_path[CONFIG_MENU_PATH_LEN];  /* SD override; "" = built-in */
    uint8_t show_debugging;
    uint8_t show_bezel;
    char bezel_path[CONFIG_MENU_PATH_LEN];
    uint8_t smartport_slots[8];
    char smartport_disk_paths[8][CONFIG_MENU_PATH_LEN];
    uint8_t disk2_slot6_enabled;
    uint8_t disk2_activity_visible;
    uint8_t disk2_sound_volume;
    uint8_t disk2_slots[2][4];
    char disk2_disk_paths[2][CONFIG_MENU_PATH_LEN];
    uint8_t mouse_slot2_enabled;
    uint8_t mouse_sensitivity;
    uint8_t applicard_slot5_enabled; /* PCPI Applicard (Z80) in slot 5 */
    uint8_t applicard_resource_max;  /* 0 = standard CPU share, 1 = maximum */
    uint8_t supersprite_enabled;   /* SuperSprite VDP in slot 7 (excl. SmartPort) */
    uint8_t sdd_stream_enabled;    /* USB0 bus-event stream for SuperDuperDisplay */
    uint8_t usb0_sd_remote_active; /* modal USB0 SD-card mass-storage bridge */
    uint8_t mockingboard_slot4_enabled;
    uint8_t mockingboard_pan[12];
    int8_t phasor_bass;
    int8_t phasor_mid;
    int8_t phasor_treble;
    int8_t phasor_warmth;
    int8_t phasor_volume;
    uint8_t phasor_psg_ay_mode;
    uint8_t phasor_mockingboard_only;  /* lock card to Mockingboard mode */
    uint8_t ethernet_slot1_enabled;
    uint8_t ethernet_config_enabled;
    uint8_t ethernet_address_mode;
    uint8_t ethernet_edit_index;
    uthernet2_network_config_t ethernet_config;
    uint8_t clock_enabled;
    uint8_t ram_enabled;
    uint8_t ramworks_enabled;
    uint8_t sp_ramdisk_enabled;
    uint8_t settings_loaded;
    uint8_t session_only;
    usb_hid_menu_source_t usb_menu_bindings[CONFIG_MENU_USB_BIND_ACTION_COUNT];
    uint8_t usb_binding_capture;
    uint8_t usb_bindings_editable;
    uint8_t usb_owned;
    uint8_t status_warning;
    uint8_t browser_active;
    uint8_t browser_target;
    uint16_t browser_selected;
    uint16_t browser_top;
    uint16_t browser_count;
    char browser_dir[CONFIG_MENU_PATH_LEN];
    uint8_t profile_carousel_active;
    uint16_t profile_selected;
    uint16_t profile_count;
    char profile_dir[CONFIG_MENU_PATH_LEN];
    char profile_source_dir[CONFIG_MENU_PATH_LEN];
    uint8_t profile_name_editor_active;
    uint8_t profile_name_editor_mode;
    uint8_t profile_name_editor_virtual;
    uint8_t profile_name_editor_vk_index;
    char profile_name_editor_text[CONFIG_MENU_PATH_LEN];
    char profile_name_editor_target_dir[CONFIG_MENU_PATH_LEN];
    rtc_pcf8563_time_t clock_time;
    config_menu_platform_t platform;
    char status[CONFIG_MENU_STATUS_LEN];
} config_menu_t;

void config_menu_init(config_menu_t *menu);
void config_menu_bind_platform(config_menu_t *menu, const config_menu_platform_t *platform);
void config_menu_apply_boot_runtime(config_menu_t *menu);
void config_menu_apply_runtime(config_menu_t *menu);
void config_menu_apply_startup_assets(config_menu_t *menu);
void config_menu_retry_settings_if_needed(config_menu_t *menu);
void config_menu_set_sdd_stream(config_menu_t *menu, uint8_t enable);
void config_menu_set_applicard_enabled(config_menu_t *menu, uint8_t enable);
void config_menu_stop_usb0_sd_remote(config_menu_t *menu);
void config_menu_usb0_sd_remote_host_ejected(config_menu_t *menu);
uint8_t config_menu_usb0_sd_remote_active(const config_menu_t *menu);
uint8_t config_menu_is_active(const config_menu_t *menu);
uint8_t config_menu_storage_activity_page_visible(const config_menu_t *menu);
void config_menu_set_active(config_menu_t *menu, uint8_t active);
void config_menu_toggle(config_menu_t *menu);
void config_menu_set_usb_bindings_editable(config_menu_t *menu, uint8_t editable);
void config_menu_set_usb_owned(config_menu_t *menu, uint8_t usb_owned);
uint8_t config_menu_handle_input(config_menu_t *menu, ui_input_t input);
const char *config_menu_usb_binding_action_text(uint32_t action);
const char *config_menu_usb_binding_source_text(usb_hid_menu_source_t source);
uint8_t config_menu_usb_binding_capture_action(const config_menu_t *menu);
uint8_t config_menu_capture_usb_binding(config_menu_t *menu,
                                        usb_hid_menu_source_t source);
ui_key_t config_menu_translate_usb_binding(const config_menu_t *menu,
                                           usb_hid_menu_source_t source);
usb_hid_menu_source_t config_menu_usb_ok_binding_source(const config_menu_t *menu);
usb_hid_menu_source_t config_menu_usb_open_close_binding_source(const config_menu_t *menu);
usb_hid_menu_source_t config_menu_usb_screenshot_a2_binding_source(
    const config_menu_t *menu);
usb_hid_menu_source_t config_menu_usb_screenshot_1080p_binding_source(
    const config_menu_t *menu);
void config_menu_draw(uint16_t *fb, const config_menu_t *menu, uint8_t usb_owned);

#endif
