#ifndef CONFIG_MENU_INTERNAL_H
#define CONFIG_MENU_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "ff.h"

#include "config_menu.h"
#include "config_menu_ui.h"
#include "../lib/fb16.h"

/* Coordinate and palette aliases used by the 1080p menu pages. */
#define HGR_SCALE 1
#define HGR_TEXT_SCALE_X CMUI_BODY_SCALE
#define HGR_TEXT_SCALE_Y CMUI_BODY_SCALE
#define HGR_TEXT_MAX_CHARS 240
#define HGR_X 0
#define HGR_Y 0
#define HGR_FB_W ((int)FB16_WIDTH)
#define HGR_FB_H ((int)FB16_HEIGHT)
#define HGR_W (HGR_FB_W / HGR_SCALE)
#define HGR_H (HGR_FB_H / HGR_SCALE)

#define HGR_BLACK CMUI_COLOR_BG
#define HGR_WHITE CMUI_COLOR_TEXT
#define HGR_GREEN CMUI_COLOR_SUCCESS
#define HGR_PURPLE CMUI_COLOR_ACCENT_2
#define HGR_BLUE CMUI_COLOR_ACCENT
#define HGR_ORANGE CMUI_COLOR_WARN
#define HGR_DIMMED CMUI_COLOR_DIM

#define CONFIG_MENU_BOOT_DEVICE_DISK2 1U
#define MOCKINGBOARD_CONTROL_SLOT 4U

/* Config-menu tabs. Shared here (rather than living in config_menu.c) so
 * the centralized help text in config_menu_help.c can name tabs by their
 * enum value. menu->tab holds one of these (stored as a uint32_t). */
typedef enum {
    CONFIG_TAB_PROFILES = 0,
    CONFIG_TAB_BOOT_SETTINGS,
    CONFIG_TAB_VIDEO,
    CONFIG_TAB_SMARTPORT,
    CONFIG_TAB_DISK2,
    CONFIG_TAB_APPLICARD,
    CONFIG_TAB_MOCKINGBOARD,
    CONFIG_TAB_MOUSE,
    CONFIG_TAB_ETHERNET,
    CONFIG_TAB_TRANSWARP,
    CONFIG_TAB_CLOCK,
    CONFIG_TAB_RAM,
    CONFIG_TAB_USB,
    CONFIG_TAB_PRINTING,
    CONFIG_TAB_ABOUT,
    CONFIG_TAB_COUNT
} config_tab_t;

#define SMARTPORT_DEVICE_COUNT 8U
#define CONFIG_ETHERNET_ITEM_SLOT 0U
#define CONFIG_ETHERNET_ITEM_CONFIG_ENABLED 1U
#define CONFIG_ETHERNET_ITEM_MODE 2U
#define CONFIG_ETHERNET_ITEM_MAC 3U
#define CONFIG_ETHERNET_ITEM_IP 4U
#define CONFIG_ETHERNET_ITEM_SUBNET 5U
#define CONFIG_ETHERNET_ITEM_GATEWAY 6U
#define CONFIG_ETHERNET_ITEM_READ 7U
#define CONFIG_ETHERNET_ITEM_WRITE 8U
#define CONFIG_ETHERNET_ITEM_DHCP 9U
#define CONFIG_ETHERNET_ITEM_TEST 10U
#define CONFIG_ETHERNET_ITEM_FTP_SD 11U
#define CONFIG_ETHERNET_ITEM_COUNT 12U
#define CONFIG_VIDEO_ITEM_OUTPUT       0U
#define CONFIG_VIDEO_ITEM_VARIANT      1U
#define CONFIG_VIDEO_ITEM_SCANLINES    2U
#define CONFIG_VIDEO_ITEM_BLUR         3U
#define CONFIG_VIDEO_ITEM_GLOW         4U
#define CONFIG_VIDEO_ITEM_GHOSTING     5U
#define CONFIG_VIDEO_ITEM_BORDER       6U
#define CONFIG_VIDEO_ITEM_VIDEO7       7U
#define CONFIG_VIDEO_ITEM_COL140M      8U
#define CONFIG_VIDEO_ITEM_BORDER_COLOR 9U
#define CONFIG_VIDEO_ITEM_BORDER_FLOOD 10U
#define CONFIG_VIDEO_ITEM_ROM          11U
#define CONFIG_VIDEO_ITEM_SHOW_BEZEL   12U
#define CONFIG_VIDEO_ITEM_BEZEL        13U
#define CONFIG_VIDEO_ITEM_DEBUG        14U
#define CONFIG_VIDEO_ITEM_BADGE        15U
#define CONFIG_VIDEO_ITEM_COUNT        16U
/* This is a help-table key, not a selectable row. The live ONE//e standard
 * shares the normal boot-menu reset index, whose help remains distinct. */
#define CONFIG_MENU_BOOT_ONEE_STANDARD_HELP_ITEM CONFIG_MENU_BOOT_ITEM_COUNT
#define CONFIG_TRANSWARP_ITEM_ENABLE       0U
#define CONFIG_TRANSWARP_ITEM_SPEED        1U
#define CONFIG_TRANSWARP_ITEM_IGNORE_C074  2U
#define CONFIG_TRANSWARP_ITEM_DISABLE_D2   3U
#define CONFIG_TRANSWARP_ITEM_FLOATBUS     4U
#define CONFIG_TRANSWARP_ITEM_PADDLE       5U
#define CONFIG_TRANSWARP_ITEM_SLUG         6U
#define CONFIG_TRANSWARP_ITEM_SLOT_FIRST   7U
#define CONFIG_TRANSWARP_SLOT_COUNT        7U
#define CONFIG_TRANSWARP_ITEM_SLOT_LAST \
    (CONFIG_TRANSWARP_ITEM_SLOT_FIRST + CONFIG_TRANSWARP_SLOT_COUNT - 1U)
#define CONFIG_TRANSWARP_ITEM_WINDOW \
    (CONFIG_TRANSWARP_ITEM_SLOT_LAST + 1U)
#define CONFIG_TRANSWARP_ITEM_COUNT \
    (CONFIG_TRANSWARP_ITEM_WINDOW + 1U)
#define MOCKINGBOARD_CHANNEL_COUNT 12U
#define MOCKINGBOARD_LEGACY_CHANNEL_COUNT 6U
#define PHASOR_AUDIO_CONTROL_COUNT 4U
#define PHASOR_AUDIO_CONTROL_MIN (-8)
#define PHASOR_AUDIO_CONTROL_MAX 8
#define PHASOR_WARMTH_DEFAULT 8
#define PHASOR_PSG_MODE_YM2149 0U
#define PHASOR_PSG_MODE_AY8913 1U

typedef enum {
    PHASOR_AUDIO_CONTROL_BASS = 0,
    PHASOR_AUDIO_CONTROL_MID,
    PHASOR_AUDIO_CONTROL_TREBLE,
    PHASOR_AUDIO_CONTROL_VOLUME
} phasor_audio_control_t;

#define PHASOR_SLOT_FOCUS 0U
#define PHASOR_MOCKINGBOARD_ONLY_FOCUS 1U
#define PHASOR_PAN_FOCUS_BASE 2U
#define PHASOR_AUDIO_FOCUS_BASE (PHASOR_PAN_FOCUS_BASE + MOCKINGBOARD_CHANNEL_COUNT)
#define PHASOR_PSG_MODE_FOCUS (PHASOR_AUDIO_FOCUS_BASE + PHASOR_AUDIO_CONTROL_COUNT)

uint8_t config_menu_appendf(char *buffer,
                            size_t buffer_len,
                            int *len,
                            const char *fmt,
                            ...);
void config_menu_set_status(config_menu_t *menu, uint8_t warning, const char *text);
void config_menu_save_settings(config_menu_t *menu);
uint8_t config_menu_save_settings_to_path(config_menu_t *menu,
                                          const char *path,
                                          const char *success_status);
uint8_t config_menu_load_profile_settings(config_menu_t *menu,
                                          const char *profile_dir);
uint8_t config_menu_save_profile_settings(config_menu_t *menu,
                                          const char *profile_dir);
void config_menu_refresh_smartport_media_after_menu_sd(config_menu_t *menu);
uint8_t config_menu_pan_clamp(uint32_t pan);
const char *config_menu_basename(const char *path);
const char *config_menu_boot_timeout_text(uint8_t mode);
const char *config_menu_boot_device_text(uint8_t device);
const char *config_menu_onee_mode_text(const config_menu_t *menu);
uint8_t config_menu_onee_fixed_bindings_active(const config_menu_t *menu);
const char *config_menu_onee_video_standard_text(const config_menu_t *menu);
const char *config_menu_video_output_text(uint8_t mono);
const char *config_menu_video7_auto_mono_text(uint8_t enabled);
const char *config_menu_border_color_text(uint8_t color);
const char *config_menu_border_outside_text(uint8_t flood);
const char *config_menu_video_variant_label(const config_menu_t *menu);
const char *config_menu_video_variant_text(const config_menu_t *menu);
uint8_t config_menu_video_pal_accurate_help_visible(const config_menu_t *menu);
const char *config_menu_bezel_text(const config_menu_t *menu);
const char *config_menu_video_rom_text(const config_menu_t *menu);
const char *config_menu_ethernet_address_mode_text(uint8_t mode);
void config_menu_format_ipv4(const uint8_t ip[UTHERNET2_IPV4_LEN],
                             char *out,
                             size_t out_len);
void config_menu_format_mac(const uint8_t mac[UTHERNET2_MAC_LEN],
                            char *out,
                            size_t out_len);
uint8_t config_menu_ethernet_selected_index(const config_menu_t *menu,
                                            uint8_t width);
uint32_t config_menu_boot_usb_binding_action_for_item(uint32_t item_focus);
uint32_t config_menu_boot_usb_binding_item_for_action(uint32_t action);

void config_menu_phasor_set_defaults(config_menu_t *menu);
void config_menu_phasor_apply(config_menu_t *menu);
uint32_t config_menu_phasor_item_count(void);
uint8_t config_menu_phasor_parse_setting(config_menu_t *menu,
                                         const char *key,
                                         const char *value);
uint8_t config_menu_phasor_append_settings(const config_menu_t *menu,
                                           char *buffer,
                                           size_t buffer_len,
                                           int *len);
uint8_t config_menu_phasor_adjust(config_menu_t *menu, int8_t delta);
void config_menu_phasor_activate(config_menu_t *menu);
void config_menu_phasor_draw(uint16_t *fb,
                             const config_menu_t *menu,
                             int x,
                             int y,
                             int w);
void config_menu_draw_boot_settings(uint16_t *fb,
                                    const config_menu_t *menu,
                                    int x,
                                    int y,
                                    int w);
void config_menu_draw_video(uint16_t *fb,
                            const config_menu_t *menu,
                            int x,
                            int y,
                            int w);
void config_menu_draw_smartport(uint16_t *fb,
                                const config_menu_t *menu,
                                int x,
                                int y,
                                int w);
void config_menu_draw_disk2(uint16_t *fb,
                            const config_menu_t *menu,
                            int x,
                            int y,
                            int w);
void config_menu_draw_mouse(uint16_t *fb,
                            const config_menu_t *menu,
                            int x,
                            int y,
                            int w);
void config_menu_draw_ethernet(uint16_t *fb,
                               const config_menu_t *menu,
                               int x,
                               int y,
                               int w);
void config_menu_draw_applicard(uint16_t *fb,
                                const config_menu_t *menu,
                                int x,
                                int y,
                                int w);
void config_menu_draw_clock(uint16_t *fb,
                            const config_menu_t *menu,
                            int x,
                            int y,
                            int w);
void config_menu_draw_transwarp(uint16_t *fb,
                                const config_menu_t *menu,
                                int x,
                                int y,
                                int w);
const char *config_menu_vtw_speed_label(const config_menu_t *menu);
void config_menu_draw_ram(uint16_t *fb,
                          const config_menu_t *menu,
                          int x,
                          int y,
                          int w);
void config_menu_draw_usb(uint16_t *fb,
                          const config_menu_t *menu,
                          int x,
                          int y,
                          int w);
/* Printing tab + printout browser file actions (config_menu_printing.c). */
#define CONFIG_PRINTING_ITEM_COUNT 2U
#define CONFIG_PRINTING_ITEM_ENABLE 0U
#define CONFIG_PRINTING_ITEM_BROWSE 1U
void config_menu_draw_printing(uint16_t *fb,
                               const config_menu_t *menu,
                               int x,
                               int y,
                               int w);
void config_menu_printing_toggle_ssc(config_menu_t *menu);
uint8_t config_menu_printing_handle_input(config_menu_t *menu,
                                          ui_input_t input);
void config_menu_printing_draw_overlays(uint16_t *fb,
                                        const config_menu_t *menu,
                                        int x,
                                        int y,
                                        int w);
void config_menu_printing_start_rename(config_menu_t *menu,
                                       const char *name,
                                       const char *path);
void config_menu_printing_start_delete(config_menu_t *menu,
                                       const char *name,
                                       const char *path);
/* Browser plumbing exposed by config_menu.c for the printout actions. */
FRESULT config_menu_mount_sd(void);
void config_menu_browser_reload_after_fileop(config_menu_t *menu);
uint8_t config_menu_browser_selected_file(const config_menu_t *menu,
                                          char *name,
                                          size_t name_len,
                                          char *path,
                                          size_t path_len);

void config_menu_profiles_init(config_menu_t *menu);
uint8_t config_menu_profiles_handle_input(config_menu_t *menu, ui_input_t input);
void config_menu_profiles_open_carousel(config_menu_t *menu);
void config_menu_profiles_save_to_profile(config_menu_t *menu);
void config_menu_profiles_save_as(config_menu_t *menu);
void config_menu_profiles_rename(config_menu_t *menu);
void config_menu_profiles_set_image_from_png(config_menu_t *menu, const char *path);
void config_menu_profiles_draw(uint16_t *fb,
                               const config_menu_t *menu,
                               int x,
                               int y,
                               int w);

void hgr_focus_band(uint16_t *fb, int x, int y, int w, uint8_t focused);
void hgr_draw_check_item(uint16_t *fb,
                         int x,
                         int y,
                         int w,
                         uint8_t focused,
                         uint8_t checked,
                         const char *text);
void hgr_draw_check_item_dimmed(uint16_t *fb,
                                int x,
                                int y,
                                int w,
                                uint8_t focused,
                                uint8_t checked,
                                const char *text);
void hgr_draw_item(uint16_t *fb,
                   int x,
                   int y,
                   int w,
                   uint8_t focused,
                   const char *text,
                   uint32_t color);
void hgr_draw_item_dimmed(uint16_t *fb,
                          int x,
                          int y,
                          int w,
                          uint8_t focused,
                          const char *text);
void hgr_draw_lock_icon(uint16_t *fb,
                        int x,
                        int y,
                        uint8_t locked,
                        uint8_t focused,
                        uint32_t color);
void hgr_draw_item_with_lock(uint16_t *fb,
                             int x,
                             int y,
                             int w,
                             uint8_t focused,
                             const char *text,
                             uint32_t color,
                             uint8_t locked);
void hgr_draw_value_item(uint16_t *fb,
                         int x,
                         int y,
                         int w,
                         uint8_t focused,
                         const char *label,
                         const char *value);
void hgr_draw_value_item_dimmed(uint16_t *fb,
                                int x,
                                int y,
                                int w,
                                uint8_t focused,
                                const char *label,
                                const char *value);
void hgr_draw_mouse_sensitivity_item(uint16_t *fb,
                                     int x,
                                     int y,
                                     int w,
                                     uint8_t focused,
                                     uint8_t sensitivity);
void hgr_draw_video_ghosting_item(uint16_t *fb,
                                  int x,
                                  int y,
                                  int w,
                                  uint8_t focused,
                                  uint8_t strength);
void hgr_draw_video_blur_item(uint16_t *fb,
                              int x,
                              int y,
                              int w,
                              uint8_t focused,
                              uint8_t strength);
void hgr_draw_video_glow_item(uint16_t *fb,
                              int x,
                              int y,
                              int w,
                              uint8_t focused,
                              uint8_t strength);

#endif
