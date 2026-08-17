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

/* File-manager "remember last directory" categories. Each feature that
 * picks files gets its own remembered directory (smartport shares one
 * across its slots; disk2 shares one across both drives). */
#define CONFIG_BROWSER_CAT_SMARTPORT 0U
#define CONFIG_BROWSER_CAT_DISK2     1U
#define CONFIG_BROWSER_CAT_BEZEL     2U
#define CONFIG_BROWSER_CAT_ROM       3U
#define CONFIG_BROWSER_CAT_PROFILE   4U
#define CONFIG_BROWSER_CAT_PRINTOUT  5U
#define CONFIG_BROWSER_CAT_COUNT     6U
#define CONFIG_MENU_USB_BIND_ACTION_COUNT 14U
#define CONFIG_MENU_USB_BIND_CAPTURE_NONE 0xFFU
#define CONFIG_MENU_BOOT_ONEE_ITEM 2U
#define CONFIG_MENU_BOOT_USB_BIND_RESET_ITEM 3U
/* The reset row is hidden while ONE//e owns the menu, so its focus index can
 * select the ONE//e-only video-standard row in that mode. */
#define CONFIG_MENU_BOOT_ONEE_STANDARD_ITEM \
    CONFIG_MENU_BOOT_USB_BIND_RESET_ITEM
#define CONFIG_MENU_BOOT_USB_BIND_FIRST_ITEM (CONFIG_MENU_BOOT_USB_BIND_RESET_ITEM + 1U)
#define CONFIG_MENU_BOOT_ITEM_COUNT \
    (CONFIG_MENU_BOOT_USB_BIND_FIRST_ITEM + CONFIG_MENU_USB_BIND_ACTION_COUNT)
#define CONFIG_MENU_PROFILE_ITEM_COUNT 5U
#define CONFIG_MENU_ETHERNET_ADDRESS_STATIC 0U
#define CONFIG_MENU_ETHERNET_ADDRESS_DHCP 1U

typedef enum {
    CONFIG_MENU_ONEE_MODE_OFF = 0,
    CONFIG_MENU_ONEE_MODE_RUNNING,
    CONFIG_MENU_ONEE_MODE_LOCKED
} config_menu_onee_mode_state_t;

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
    CONFIG_MENU_USB_BIND_ACTION_BACK,
    /* Global virtual-TransWarp speed actions (keyboard keys, fire with
     * the menu open or closed; unbound by default). */
    CONFIG_MENU_USB_BIND_ACTION_VTW_SPEED_TOGGLE,
    CONFIG_MENU_USB_BIND_ACTION_VTW_SPEED_UP,
    CONFIG_MENU_USB_BIND_ACTION_VTW_SPEED_DOWN,
    CONFIG_MENU_USB_BIND_ACTION_VTW_SLUG_TOGGLE
} config_menu_usb_bind_action_t;

typedef enum {
    CONFIG_SLOT5_PROCESSOR_Z80 = 0,
    CONFIG_SLOT5_PROCESSOR_AD8088,
    CONFIG_SLOT5_PROCESSOR_COUNT
} config_slot5_processor_t;

typedef struct {
    void *ctx;
    void (*set_scanlines)(void *ctx, uint8_t mode);
    uint8_t (*get_scanlines)(void *ctx);
    void (*set_video_ghosting)(void *ctx, uint8_t strength);
    uint8_t (*get_video_ghosting)(void *ctx);
    void (*set_video_blur)(void *ctx, uint8_t strength);
    uint8_t (*get_video_blur)(void *ctx);
    void (*set_video_glow)(void *ctx, uint8_t strength);
    uint8_t (*get_video_glow)(void *ctx);
    void (*set_format_badge)(void *ctx, uint8_t enabled);
    uint8_t (*get_format_badge)(void *ctx);
    void (*set_border)(void *ctx, uint8_t enabled, uint8_t color, uint8_t flood);
    uint8_t (*get_border_enabled)(void *ctx);
    uint8_t (*get_border_color)(void *ctx);
    uint8_t (*get_border_flood)(void *ctx);
    void (*set_video_output)(void *ctx,
                             uint8_t mono_enable,
                             uint8_t mono_color,
                             uint8_t color_mode,
                             uint8_t video7_auto_mono_enable,
                             uint8_t dhgr_col140m_enable,
                             int8_t clean_phase_cycles,
                             int8_t pal_phase_cycles);
    uint8_t (*get_video_output_mono)(void *ctx);
    uint8_t (*get_video_output_mono_color)(void *ctx);
    uint8_t (*get_video_output_color_mode)(void *ctx);
    uint8_t (*get_video7_auto_mono_enabled)(void *ctx);
    uint8_t (*get_dhgr_col140m_enabled)(void *ctx);
    int8_t (*get_clean_video_phase_cycles)(void *ctx);
    int8_t (*get_pal_video_phase_cycles)(void *ctx);
    uint8_t (*is_apple_video_50hz)(void *ctx);
    void (*set_onee_video_50hz)(void *ctx, uint8_t enable);
    uint8_t (*get_onee_video_50hz)(void *ctx);
    uint8_t (*get_onee_video_active_50hz)(void *ctx);
    void (*set_boot_timeout_ticks)(void *ctx, uint32_t ticks);
    void (*set_boot_handoff)(void *ctx, uint8_t handoff);
    uint8_t (*set_onee_mode)(void *ctx, uint8_t enable);
    void (*restore_onee_mode_intent)(void *ctx, uint8_t enable);
    uint8_t (*get_onee_mode_persist_update)(void *ctx, uint8_t *enable);
    void (*ack_onee_mode_persist_update)(void *ctx, uint8_t enable);
    uint8_t (*get_onee_mode_state)(void *ctx);
    uint32_t (*get_onee_mode_status)(void *ctx);
    void (*set_clock_enabled)(void *ctx, uint8_t enable);
    void (*set_supersprite_enabled)(void *ctx, uint8_t enable);
    void (*set_ssc_enabled)(void *ctx, uint8_t enable);
    void (*set_sdd_stream_enabled)(void *ctx, uint8_t enable);
    void (*set_usb0_sd_remote_mount)(void *ctx, uint8_t enable);
    int (*set_ethernet_ftp_sd_remote)(void *ctx,
                                      uint8_t enable,
                                      char *detail,
                                      size_t detail_len);
    void (*set_slot_enabled)(void *ctx, uint8_t slot, uint8_t enable);
    uint8_t (*get_slot_enabled)(void *ctx, uint8_t slot);
    void (*set_slot5_processor)(void *ctx, uint8_t processor);
    void (*set_applicard_resource_max)(void *ctx, uint8_t maximum);
    void (*set_vtw_config)(void *ctx,
                           uint8_t enable,
                           uint8_t speed_mode,
                           uint8_t pace_divider,
                           uint8_t ignore_c074,
                           uint8_t disable_disk2_accel);
    void (*set_vtw_slug_key_enabled)(void *ctx, uint8_t enable);
    /* Per-region slowdown (TransWarp DIP block 2): region enable mask +
     * 1 MHz window duration in Apple cycles. */
    void (*set_vtw_slowdown)(void *ctx, uint16_t region_mask, uint16_t cycles);
    /* Full USB1 host re-enumeration (stop + start), for devices too slow
     * to enumerate at power-on. */
    void (*refresh_usb1)(void *ctx);
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
    int (*ethernet_dhcp_start)(void *ctx,
                               const uint8_t mac[UTHERNET2_MAC_LEN],
                               char *detail,
                               size_t detail_len);
    int (*ethernet_dhcp_poll)(void *ctx,
                              uthernet2_network_config_t *lease,
                              char *detail,
                              size_t detail_len);
    void (*ethernet_dhcp_cancel)(void *ctx);
} config_menu_platform_t;

typedef struct {
    uint8_t active;
    uint32_t tab;
    uint32_t item_focus;
    uint8_t boot_timeout_mode;
    uint8_t boot_device;
    uint8_t onee_mode_state;     /* live service state */
    uint32_t onee_mode_status;   /* live PL supervisor readback */
    uint8_t onee_persisted_enabled; /* global config only; never profiled */
    uint8_t onee_video_50hz;     /* desired global standard; 0 NTSC, 1 PAL */
    uint8_t onee_persist_write_failed;
    uint32_t onee_persist_retry_polls;
    uint8_t scanlines_mode;
    uint8_t video_output_mono;
    uint8_t video_mono_color;
    uint8_t video_color_mode;
    uint8_t video7_auto_mono_enabled;
    uint8_t dhgr_col140m_enabled;
    uint8_t video_ghosting_strength;
    uint8_t video_blur_strength;
    uint8_t video_glow_strength;
    uint8_t format_badge_enabled;
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
    uint8_t applicard_slot5_enabled; /* selected coprocessor in slot 5 */
    uint8_t slot5_processor;         /* config_slot5_processor_t */
    uint8_t applicard_resource_max;  /* 0 = standard CPU share, 1 = maximum */
    uint8_t vtw_enabled;             /* virtual TransWarp accelerator */
    uint8_t vtw_speed_mode;          /* 0 full, 1 divided, 2 1MHz-locked */
    uint8_t vtw_pace_divider;        /* divided mode: fabric clks per cycle */
    uint8_t vtw_ignore_c074;         /* ignore every $C074 speed-switch write */
    uint8_t vtw_disable_disk2_accel; /* force Disk II onto physical 1MHz path */
    uint8_t vtw_slug_key_enabled;    /* arm the 0.05 MHz slug USB key (def off) */
    uint16_t vtw_slowdown_mask;      /* per-region 1MHz: [6:0] slots, [7] float/video, [8] paddle */
    uint16_t vtw_slowdown_cycles;    /* 1 MHz window per region access (0 = off) */
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
    uint8_t ssc_slot1_enabled;     /* virtual SSC printer, shares slot 1 */
    uint8_t ethernet_config_enabled;
    uint8_t ethernet_address_mode;
    uint8_t ethernet_edit_index;
    uint8_t ethernet_dhcp_pending;
    uint8_t ethernet_dhcp_report;
    uint8_t ethernet_ftp_sd_remote_active;
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
    /* Last-visited browse directory per feature category (see
     * CONFIG_BROWSER_CAT_*). The file manager reopens here next time, or
     * falls back to the feature default / card root if it is unreachable. */
    char browser_last_dir[CONFIG_BROWSER_CAT_COUNT][CONFIG_MENU_PATH_LEN];
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
    /* Printout browser file actions (rename editor + delete confirm). */
    uint8_t printout_editor_active;
    uint8_t printout_editor_virtual;
    uint8_t printout_editor_vk_index;
    uint8_t printout_delete_confirm_active;
    char printout_editor_text[CONFIG_MENU_PATH_LEN];
    char printout_action_name[CONFIG_MENU_PATH_LEN];
    char printout_action_path[CONFIG_MENU_PATH_LEN];
    rtc_pcf8563_time_t clock_time;
    config_menu_platform_t platform;
    char status[CONFIG_MENU_STATUS_LEN];
} config_menu_t;

void config_menu_init(config_menu_t *menu);
void config_menu_bind_platform(config_menu_t *menu, const config_menu_platform_t *platform);
void config_menu_apply_boot_runtime(config_menu_t *menu);
void config_menu_apply_runtime(config_menu_t *menu);
void config_menu_apply_startup_assets(config_menu_t *menu);
void config_menu_start_boot_dhcp(config_menu_t *menu);
void config_menu_poll_onee_mode(config_menu_t *menu);
void config_menu_poll_ethernet(config_menu_t *menu);
void config_menu_retry_settings_if_needed(config_menu_t *menu);
void config_menu_set_sdd_stream(config_menu_t *menu, uint8_t enable);
void config_menu_refresh_vtw_slowdown(config_menu_t *menu);
void config_menu_set_applicard_enabled(config_menu_t *menu, uint8_t enable);
void config_menu_set_vtw_enabled(config_menu_t *menu, uint8_t enable);
void config_menu_set_vtw_speed(config_menu_t *menu,
                               uint8_t speed_mode,
                               uint8_t pace_divider);
void config_menu_stop_usb0_sd_remote(config_menu_t *menu);
void config_menu_usb0_sd_remote_host_ejected(config_menu_t *menu);
uint8_t config_menu_usb0_sd_remote_active(const config_menu_t *menu);
void config_menu_stop_ethernet_ftp_sd_remote(config_menu_t *menu);
uint8_t config_menu_ethernet_ftp_sd_remote_active(const config_menu_t *menu);
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
usb_hid_menu_source_t config_menu_usb_vtw_binding_source(
    const config_menu_t *menu, uint32_t action);
void config_menu_draw(uint16_t *fb, const config_menu_t *menu, uint8_t usb_owned);

#endif
