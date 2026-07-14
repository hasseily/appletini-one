#include "config_menu.h"
#include "config_menu_internal.h"
#include "config_menu_help.h"

#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "diskio.h"
#include "ff.h"

#include "apple_fb_handoff.h"
#include "compositor_layout.h"
#include "profile_manager.h"
#include "../image_versions.h"
#include "xil_cache.h"
#include "usb_hid_service.h"
#include "video_ghosting.h"
#include "video_output.h"

extern uint8_t boot_menu_service_aux_card_present(void);

#define APPLETINI_CFG_PATH "0:/appletini_cfg.txt"
#define APPLETINI_CFG_MAX 8192U
#define APPLETINI_CFG_VERSION 103U
#define ETHERNET_CONTROL_SLOT 1U
#define DISK2_CONTROL_SLOT 6U
#define MOUSE_CONTROL_SLOT 2U
#define APPLICARD_CONTROL_SLOT 5U

#define CONFIG_DEFAULT_BOOT_TIMEOUT_MODE CONFIG_BOOT_TIMEOUT_3S
#define CONFIG_DEFAULT_BOOT_DEVICE CONFIG_BOOT_DEVICE_SMARTPORT
#define CONFIG_DEFAULT_SCANLINES_MODE APPLETINI_SCANLINES_OFF
#define CONFIG_DEFAULT_VIDEO_OUTPUT_MONO 0U
#define CONFIG_DEFAULT_VIDEO_MONO_COLOR APPLE_VIDEO_MONO_WHITE
#define CONFIG_DEFAULT_VIDEO_COLOR_MODE APPLE_VIDEO_COLOR_COMPOSITE_MONITOR
#define CONFIG_DEFAULT_VIDEO7_AUTO_MONO_ENABLED 1U
#define CONFIG_DEFAULT_VIDEO_GHOSTING_STRENGTH APPLETINI_VIDEO_GHOSTING_OFF
#define CONFIG_DEFAULT_BORDER_ENABLED 0U
#define CONFIG_DEFAULT_BORDER_COLOR APPLE_VIDEO_IIGS_BORDER_DEFAULT
#define CONFIG_DEFAULT_BORDER_FLOOD 0U
#define CONFIG_DEFAULT_CLEAN_VIDEO_PHASE_CYCLES APPLE_VIDEO_DEFAULT_CLEAN_PHASE_CYCLES
#define CONFIG_DEFAULT_PAL_VIDEO_PHASE_CYCLES APPLE_VIDEO_DEFAULT_PAL_PHASE_CYCLES
#define CONFIG_DEFAULT_SHOW_DEBUGGING 0U
#define CONFIG_DEFAULT_SHOW_BEZEL 1U
#define CONFIG_DEFAULT_SMARTPORT_DISK1_ENABLED 1U
#define CONFIG_DEFAULT_DISK2_SLOT6_ENABLED 1U
#define CONFIG_DEFAULT_APPLICARD_SLOT5_ENABLED 0U
#define CONFIG_DEFAULT_DISK2_ACTIVITY_VISIBLE 1U
#define CONFIG_DEFAULT_DISK2_SOUND_VOLUME 5U
#define CONFIG_MAX_DISK2_SOUND_VOLUME 10U
#define CONFIG_DISK2_SOUND_EVENT_DOOR_OPEN 4U
#define CONFIG_DISK2_SOUND_EVENT_DOOR_CLOSE 5U
#define CONFIG_DEFAULT_MOUSE_SLOT2_ENABLED 1U
#define CONFIG_DEFAULT_MOUSE_SENSITIVITY 100U
#define CONFIG_DEFAULT_MOCKINGBOARD_SLOT4_ENABLED 1U
#define CONFIG_DEFAULT_ETHERNET_SLOT1_ENABLED 1U
#define CONFIG_DEFAULT_ETHERNET_CONFIG_ENABLED 0U
#define CONFIG_DEFAULT_ETHERNET_ADDRESS_MODE CONFIG_MENU_ETHERNET_ADDRESS_STATIC
#define CONFIG_DEFAULT_CLOCK_ENABLED 1U
#define CONFIG_DEFAULT_RAM_ENABLED 1U
#define CONFIG_DEFAULT_SP_RAMDISK_ENABLED 0U
#define CONFIG_USB_KEY_USAGE_A 0x04U
#define CONFIG_USB_KEY_USAGE_1 0x1EU
#define CONFIG_USB_KEY_USAGE_0 0x27U
#define CONFIG_USB_KEY_USAGE_ENTER 0x28U
#define CONFIG_USB_KEY_USAGE_BACKSPACE 0x2AU
#define CONFIG_USB_KEY_USAGE_TAB 0x2BU
#define CONFIG_USB_KEY_USAGE_SPACE 0x2CU
#define CONFIG_USB_KEY_USAGE_F1 0x3AU
#define CONFIG_USB_KEY_USAGE_F12 0x45U
#define CONFIG_USB_KEY_USAGE_F13 0x68U
#define CONFIG_USB_KEY_USAGE_F24 0x73U
#define CONFIG_USB_KEY_USAGE_PRINTSCN 0x46U
#define CONFIG_USB_KEY_USAGE_ESCAPE 0x29U
#define CONFIG_USB_KEY_SOURCE(usage) \
    ((usb_hid_menu_source_t)(USB_HID_MENU_SOURCE_KEY_BASE | (usage)))

#define CONFIG_SMARTPORT_ALL_DEVICES 0xFFU
#define CONFIG_DISK2_PO_IMAGE_BYTES 143360U
#define CONFIG_SMARTPORT_140K_PO_IMAGE_BYTES 143360U
#define CONFIG_SMARTPORT_800K_PO_IMAGE_BYTES 819200U

static uint8_t config_menu_str_ieq(const char *a, const char *b);

#define BOOT_TIMEOUT_TICKS_3S 399000000U
#define BOOT_TIMEOUT_TICKS_5S 665000000U
#define BOOT_TIMEOUT_TICKS_ALWAYS 0xFFFFFFFFU
#define MOUSE_SENSITIVITY_MIN 3U
#define MOUSE_SENSITIVITY_MAX 150U
#define MOUSE_SENSITIVITY_DEFAULT_INDEX 11U
#define MOUSE_SENSITIVITY_STEP_COUNT 16U

static const uint8_t k_mouse_sensitivity_steps[MOUSE_SENSITIVITY_STEP_COUNT] = {
    3U, 5U, 8U, 12U,
    18U, 25U, 35U, 50U,
    65U, 80U, 90U, 100U,
    112U, 125U, 137U, 150U
};


typedef enum {
    CONFIG_BOOT_TIMEOUT_3S = 0,
    CONFIG_BOOT_TIMEOUT_5S,
    CONFIG_BOOT_TIMEOUT_ALWAYS,
    CONFIG_BOOT_TIMEOUT_COUNT
} config_boot_timeout_t;

typedef enum {
    CONFIG_BOOT_DEVICE_SMARTPORT = 0,
    CONFIG_BOOT_DEVICE_DISK2,
    CONFIG_BOOT_DEVICE_COUNT
} config_boot_device_t;

typedef enum {
    CONFIG_BOOT_HANDOFF_SMARTPORT = 1,
    CONFIG_BOOT_HANDOFF_DISK2 = 2
} config_boot_handoff_t;

typedef enum {
    CONFIG_BROWSER_TARGET_NONE = 0,
    CONFIG_BROWSER_TARGET_SMARTPORT_1,
    CONFIG_BROWSER_TARGET_SMARTPORT_2,
    CONFIG_BROWSER_TARGET_SMARTPORT_3,
    CONFIG_BROWSER_TARGET_SMARTPORT_4,
    CONFIG_BROWSER_TARGET_SMARTPORT_5,
    CONFIG_BROWSER_TARGET_SMARTPORT_6,
    CONFIG_BROWSER_TARGET_SMARTPORT_7,
    CONFIG_BROWSER_TARGET_SMARTPORT_8,
    CONFIG_BROWSER_TARGET_DISK2_D1,
    CONFIG_BROWSER_TARGET_DISK2_D2,
    CONFIG_BROWSER_TARGET_BEZEL,
    CONFIG_BROWSER_TARGET_VIDEO_ROM,
    CONFIG_BROWSER_TARGET_PROFILE_IMAGE
} config_browser_target_t;

typedef enum {
    CONFIG_BROWSER_ENTRY_CLOSE = 0,
    CONFIG_BROWSER_ENTRY_EMPTY,
    CONFIG_BROWSER_ENTRY_PARENT,
    CONFIG_BROWSER_ENTRY_DIR,
    CONFIG_BROWSER_ENTRY_FILE
} config_browser_entry_type_t;

typedef struct {
    config_browser_entry_type_t type;
    uint8_t read_only;
    char name[CONFIG_MENU_PATH_LEN];
    char path[CONFIG_MENU_PATH_LEN];
} config_browser_entry_t;

typedef struct {
    uint8_t loaded;
    uint8_t valid;
    char path[CONFIG_MENU_PATH_LEN];
    uint32_t *pixels;
    unsigned w;
    unsigned h;
} config_browser_preview_cache_t;

#define CONFIG_BROWSER_MAX_ENTRIES 96U
#define CONFIG_BROWSER_VISIBLE_ROWS 12U
#define CONFIG_BROWSER_PROFILE_IMAGE_VISIBLE_ROWS 11U
#define CONFIG_BROWSER_PROFILE_PREVIEW_W 118
#define CONFIG_BROWSER_PROFILE_PREVIEW_GAP 8
#define CONFIG_BROWSER_PROFILE_PREVIEW_PAD_X 4
#define CONFIG_BROWSER_PROFILE_PREVIEW_TITLE_H 10
#define CONFIG_BROWSER_PROFILE_PREVIEW_H \
    (CONFIG_BROWSER_PROFILE_PREVIEW_TITLE_H + \
     (((CONFIG_BROWSER_PROFILE_PREVIEW_W - (2 * CONFIG_BROWSER_PROFILE_PREVIEW_PAD_X)) * \
       PROFILE_MANAGER_THUMB_H + PROFILE_MANAGER_THUMB_W - 1U) / PROFILE_MANAGER_THUMB_W))

static FATFS g_config_fs;
static config_browser_entry_t g_browser_entries[CONFIG_BROWSER_MAX_ENTRIES];
static config_browser_preview_cache_t g_browser_preview_cache;

static const char * const k_tab_labels[CONFIG_TAB_COUNT] = {
    "Profiles",
    "Boot Settings",
    "Video",
    "SmartPort",
    "Disk II",
    "Mouse",
    "Phasor",
    "Ethernet",
    "Z80 Applicard",
    "Clock",
    "RAM",
    "USB",
    "About"
};

#define CONFIG_MENU_HELP_H 210
#define CONFIG_MENU_HELP_GAP 24

/* Help-panel text lives in config_menu_help.c (centralized, per-tab with
 * optional per-item overrides). config_menu_draw_help() below fetches it
 * via config_menu_help_resolve(). */

static const char * const k_about_contributors[] = {
    "hardware:   Karl \"KKR75\" Asseily",
    "firmware:   Henri \"Rikkles\" Asseily"
};

static const char * const k_about_versions[] = {
    "Golden boot: " APPLETINI_BOOT_IMAGE_VERSION_FULL,
    "Firmware:    " APPLETINI_FIRMWARE_IMAGE_VERSION_FULL
};

static const char * const k_about_third_party[] = {
    "Project origin and multiple components - John \"Elltwo\" Flanagan",
    "CherryUSB - embedded USB stack",
    "FatFs - FAT filesystem module by ChaN",
    "LodePNG - PNG codec by Lode Vandevenne",
    "WB2AXIP - AXI helper RTL by Gisselquist Technology",
    "Xilinx/AMD Vitis standalone BSP and drivers",
    "A2RetroNet project - Oliver Schmidt",
    "z80emu - Lin Ke-Fong",
    "AppleWin reference - AppleWin emulator by Tom Charlesworth, Michael Pohoreski, and others",
    "Accurate PAL video timing reference - Stephane Champailler",
    "SC-01 Speech chip emulation reference - Olivier Galibert",
    "Mockingboard/Phasor reference - Tom Charlesworth",
    "",
    "And in no particular order: Peter Ferrie, John Brooks, fenarinarsa, Jansky, the Paris a2cp",
    "fatdog, 4am, the Infinitum Slack, the Apple II community discords and so many more..."
};

static const char * const k_usb_binding_config_keys[CONFIG_MENU_USB_BIND_ACTION_COUNT] = {
    "usb.menu.bind.up",
    "usb.menu.bind.down",
    "usb.menu.bind.left",
    "usb.menu.bind.right",
    "usb.menu.bind.tab.up",
    "usb.menu.bind.tab.down",
    "usb.menu.bind.screenshot.a2",
    "usb.menu.bind.screenshot.1080p",
    "usb.menu.bind.ok",
    "usb.menu.bind.back"
};

static const char * const k_usb_binding_action_text[CONFIG_MENU_USB_BIND_ACTION_COUNT] = {
    "Up",
    "Down",
    "Left",
    "Right",
    "Tab up",
    "Tab down",
    "PRTSCR A2",
    "PRTSCR 1080P",
    "OK",
    "Back"
};

static const uint8_t k_boot_usb_binding_action_order[CONFIG_MENU_USB_BIND_ACTION_COUNT] = {
    CONFIG_MENU_USB_BIND_ACTION_UP,
    CONFIG_MENU_USB_BIND_ACTION_DOWN,
    CONFIG_MENU_USB_BIND_ACTION_LEFT,
    CONFIG_MENU_USB_BIND_ACTION_RIGHT,
    CONFIG_MENU_USB_BIND_ACTION_TAB_UP,
    CONFIG_MENU_USB_BIND_ACTION_TAB_DOWN,
    CONFIG_MENU_USB_BIND_ACTION_OK,
    CONFIG_MENU_USB_BIND_ACTION_BACK,
    CONFIG_MENU_USB_BIND_ACTION_SCREENSHOT_A2,
    CONFIG_MENU_USB_BIND_ACTION_SCREENSHOT_1080P
};

static const ui_key_t k_usb_binding_keys[CONFIG_MENU_USB_BIND_ACTION_COUNT] = {
    UI_KEY_PAGE_UP,
    UI_KEY_PAGE_DOWN,
    UI_KEY_LEFT,
    UI_KEY_RIGHT,
    UI_KEY_SHIFT_TAB,
    UI_KEY_TAB,
    UI_KEY_NONE,
    UI_KEY_NONE,
    UI_KEY_ENTER,
    UI_KEY_BACK
};

static const usb_hid_menu_source_t k_usb_binding_defaults[CONFIG_MENU_USB_BIND_ACTION_COUNT] = {
    USB_HID_MENU_ACTION_ITEM_UP,
    USB_HID_MENU_ACTION_ITEM_DOWN,
    USB_HID_MENU_ACTION_LEFT,
    USB_HID_MENU_ACTION_RIGHT,
    USB_HID_MENU_ACTION_PREV_TAB,
    USB_HID_MENU_ACTION_NEXT_TAB,
    CONFIG_USB_KEY_SOURCE(CONFIG_USB_KEY_USAGE_F12),
    CONFIG_USB_KEY_SOURCE(CONFIG_USB_KEY_USAGE_PRINTSCN),
    USB_HID_MENU_ACTION_SELECT,
    CONFIG_USB_KEY_SOURCE(CONFIG_USB_KEY_USAGE_ESCAPE)
};

static const usb_hid_menu_source_t k_usb_binding_source_order[] = {
    USB_HID_MENU_ACTION_NONE,
    USB_HID_MENU_ACTION_LEFT,
    USB_HID_MENU_ACTION_RIGHT,
    USB_HID_MENU_ACTION_SELECT,
    USB_HID_MENU_ACTION_ITEM_DOWN,
    USB_HID_MENU_ACTION_ITEM_UP,
    USB_HID_MENU_ACTION_PREV_TAB,
    USB_HID_MENU_ACTION_NEXT_TAB
};

static const usb_hid_menu_source_t k_usb_binding_button_source_order[] = {
    USB_HID_MENU_ACTION_LEFT,
    USB_HID_MENU_ACTION_RIGHT,
    USB_HID_MENU_ACTION_SELECT,
    USB_HID_MENU_ACTION_ITEM_DOWN,
    USB_HID_MENU_ACTION_ITEM_UP
};

static const usb_hid_menu_source_t k_usb_binding_screenshot_source_order[] = {
    USB_HID_MENU_SOURCE_NONE,
    CONFIG_USB_KEY_SOURCE(CONFIG_USB_KEY_USAGE_F12),
    CONFIG_USB_KEY_SOURCE(CONFIG_USB_KEY_USAGE_PRINTSCN)
};

void config_menu_set_status(config_menu_t *menu, uint8_t warning, const char *text)
{
    if (menu == NULL) {
        return;
    }

    menu->status_warning = (warning != 0U) ? 1U : 0U;
    if (text == NULL) {
        menu->status[0] = '\0';
    } else {
        (void)snprintf(menu->status, sizeof(menu->status), "%s", text);
    }
}

static void config_menu_set_sd_error(config_menu_t *menu, const char *prefix, FRESULT fr)
{
    char text[CONFIG_MENU_STATUS_LEN];

    if (prefix == NULL) {
        prefix = "CONFIG SD ERROR";
    }

    (void)snprintf(text,
                   sizeof(text),
                   "%s FR=%u: SETTINGS ACTIVE FOR THIS SESSION",
                   prefix,
                   (unsigned)fr);
    config_menu_set_status(menu, 1U, text);
}

uint8_t config_menu_appendf(char *buffer,
                            size_t buffer_len,
                            int *len,
                            const char *fmt,
                            ...)
{
    va_list args;
    int written;

    if (buffer == NULL || len == NULL || fmt == NULL ||
        *len < 0 || (size_t)*len >= buffer_len) {
        return 0U;
    }

    va_start(args, fmt);
    written = vsnprintf(buffer + *len, buffer_len - (size_t)*len, fmt, args);
    va_end(args);

    if (written < 0 ||
        (size_t)written >= (buffer_len - (size_t)*len)) {
        return 0U;
    }

    *len += written;
    return 1U;
}

static uint8_t config_menu_bool_text(const char *value)
{
    return (config_menu_str_ieq(value, "on") != 0U) ? 1U : 0U;
}

static const char *config_menu_enabled_text(uint8_t enabled)
{
    return (enabled != 0U) ? "Enabled" : "Disabled";
}

static const char *config_menu_on_off(uint8_t enabled)
{
    return (enabled != 0U) ? "ON" : "OFF";
}

const char *config_menu_ethernet_address_mode_text(uint8_t mode)
{
    return (mode == CONFIG_MENU_ETHERNET_ADDRESS_DHCP) ? "DHCP" : "STATIC";
}

static uint8_t config_menu_ethernet_address_mode_value(const char *value)
{
    return (config_menu_str_ieq(value, "dhcp") != 0U) ?
        CONFIG_MENU_ETHERNET_ADDRESS_DHCP :
        CONFIG_MENU_ETHERNET_ADDRESS_STATIC;
}

void config_menu_format_ipv4(const uint8_t ip[UTHERNET2_IPV4_LEN],
                             char *out,
                             size_t out_len)
{
    if (out == NULL || out_len == 0U) {
        return;
    }
    if (ip == NULL) {
        out[0] = '\0';
        return;
    }
    (void)snprintf(out,
                   out_len,
                   "%u.%u.%u.%u",
                   (unsigned)ip[0],
                   (unsigned)ip[1],
                   (unsigned)ip[2],
                   (unsigned)ip[3]);
}

void config_menu_format_mac(const uint8_t mac[UTHERNET2_MAC_LEN],
                            char *out,
                            size_t out_len)
{
    if (out == NULL || out_len == 0U) {
        return;
    }
    if (mac == NULL) {
        out[0] = '\0';
        return;
    }
    (void)snprintf(out,
                   out_len,
                   "%02X:%02X:%02X:%02X:%02X:%02X",
                   (unsigned)mac[0],
                   (unsigned)mac[1],
                   (unsigned)mac[2],
                   (unsigned)mac[3],
                   (unsigned)mac[4],
                   (unsigned)mac[5]);
}

uint8_t config_menu_ethernet_selected_index(const config_menu_t *menu,
                                            uint8_t width)
{
    if (menu == NULL || width == 0U) {
        return 0U;
    }
    return (uint8_t)(menu->ethernet_edit_index % width);
}

static uint8_t config_menu_parse_dec_byte(const char **cursor, uint8_t *out)
{
    uint32_t value = 0U;
    const char *p;

    if (cursor == NULL || *cursor == NULL || out == NULL) {
        return 0U;
    }
    p = *cursor;
    if (*p < '0' || *p > '9') {
        return 0U;
    }
    while (*p >= '0' && *p <= '9') {
        value = (value * 10U) + (uint32_t)(*p - '0');
        if (value > 255U) {
            return 0U;
        }
        p++;
    }
    *cursor = p;
    *out = (uint8_t)value;
    return 1U;
}

static uint8_t config_menu_parse_ipv4(const char *text,
                                      uint8_t ip[UTHERNET2_IPV4_LEN])
{
    const char *p = text;
    uint8_t parsed[UTHERNET2_IPV4_LEN];

    if (text == NULL || ip == NULL) {
        return 0U;
    }
    for (uint8_t i = 0U; i < UTHERNET2_IPV4_LEN; ++i) {
        if (config_menu_parse_dec_byte(&p, &parsed[i]) == 0U) {
            return 0U;
        }
        if (i < (UTHERNET2_IPV4_LEN - 1U)) {
            if (*p != '.') {
                return 0U;
            }
            p++;
        }
    }
    if (*p != '\0') {
        return 0U;
    }
    memcpy(ip, parsed, sizeof(parsed));
    return 1U;
}

static uint8_t config_menu_hex_value(char c, uint8_t *out)
{
    if (out == NULL) {
        return 0U;
    }
    if (c >= '0' && c <= '9') {
        *out = (uint8_t)(c - '0');
        return 1U;
    }
    if (c >= 'a' && c <= 'f') {
        *out = (uint8_t)(10U + (uint8_t)(c - 'a'));
        return 1U;
    }
    if (c >= 'A' && c <= 'F') {
        *out = (uint8_t)(10U + (uint8_t)(c - 'A'));
        return 1U;
    }
    return 0U;
}

static uint8_t config_menu_parse_hex_byte(const char **cursor, uint8_t *out)
{
    uint8_t hi = 0U;
    uint8_t lo = 0U;
    const char *p;

    if (cursor == NULL || *cursor == NULL || out == NULL) {
        return 0U;
    }
    p = *cursor;
    if (config_menu_hex_value(*p, &hi) == 0U) {
        return 0U;
    }
    p++;
    if (config_menu_hex_value(*p, &lo) != 0U) {
        hi = (uint8_t)((hi << 4) | lo);
        p++;
    }
    *cursor = p;
    *out = hi;
    return 1U;
}

static uint8_t config_menu_parse_mac(const char *text,
                                     uint8_t mac[UTHERNET2_MAC_LEN])
{
    const char *p = text;
    uint8_t parsed[UTHERNET2_MAC_LEN];

    if (text == NULL || mac == NULL) {
        return 0U;
    }
    for (uint8_t i = 0U; i < UTHERNET2_MAC_LEN; ++i) {
        if (config_menu_parse_hex_byte(&p, &parsed[i]) == 0U) {
            return 0U;
        }
        if (i < (UTHERNET2_MAC_LEN - 1U)) {
            if (*p != ':') {
                return 0U;
            }
            p++;
        }
    }
    if (*p != '\0' || uthernet2_mac_is_valid(parsed) == 0U) {
        return 0U;
    }
    memcpy(mac, parsed, sizeof(parsed));
    return 1U;
}

static void config_menu_coerce_ethernet(config_menu_t *menu)
{
    if (menu == NULL) {
        return;
    }
    if (menu->ethernet_address_mode != CONFIG_MENU_ETHERNET_ADDRESS_DHCP) {
        menu->ethernet_address_mode = CONFIG_MENU_ETHERNET_ADDRESS_STATIC;
    }
}

static uint8_t config_menu_wrapped_u8_delta(uint8_t value, int8_t delta)
{
    int32_t next = (int32_t)value + (int32_t)delta;

    while (next < 0) {
        next += 256;
    }
    while (next > 255) {
        next -= 256;
    }
    return (uint8_t)next;
}

static const char *config_menu_scanlines_config(uint8_t mode)
{
    switch (appletini_scanlines_clamp(mode)) {
    case APPLETINI_SCANLINES_LIGHT:
        return "LIGHT";
    case APPLETINI_SCANLINES_MEDIUM:
        return "MEDIUM";
    case APPLETINI_SCANLINES_STRONG:
        return "STRONG";
    case APPLETINI_SCANLINES_OFF:
    default:
        return "OFF";
    }
}

static const char *config_menu_video_ghosting_config(uint8_t strength)
{
    switch (appletini_video_ghosting_clamp(strength)) {
    case APPLETINI_VIDEO_GHOSTING_LIGHT:
        return "LIGHT";
    case APPLETINI_VIDEO_GHOSTING_MEDIUM:
        return "MEDIUM";
    case APPLETINI_VIDEO_GHOSTING_STRONG:
        return "STRONG";
    case APPLETINI_VIDEO_GHOSTING_OFF:
    default:
        return "OFF";
    }
}

static uint8_t config_menu_scanlines_text(const char *value)
{
    if (value == NULL) {
        return APPLETINI_SCANLINES_OFF;
    }
    if (config_menu_str_ieq(value, "light") != 0U) {
        return APPLETINI_SCANLINES_LIGHT;
    }
    if (config_menu_str_ieq(value, "medium") != 0U) {
        return APPLETINI_SCANLINES_MEDIUM;
    }
    if (config_menu_str_ieq(value, "strong") != 0U) {
        return APPLETINI_SCANLINES_STRONG;
    }
    return APPLETINI_SCANLINES_OFF;
}

static uint8_t config_menu_video_ghosting_text(const char *value)
{
    if (value == NULL) {
        return APPLETINI_VIDEO_GHOSTING_OFF;
    }
    if (config_menu_str_ieq(value, "light") != 0U) {
        return APPLETINI_VIDEO_GHOSTING_LIGHT;
    }
    if (config_menu_str_ieq(value, "medium") != 0U) {
        return APPLETINI_VIDEO_GHOSTING_MEDIUM;
    }
    if (config_menu_str_ieq(value, "strong") != 0U) {
        return APPLETINI_VIDEO_GHOSTING_STRONG;
    }
    return APPLETINI_VIDEO_GHOSTING_OFF;
}

static char config_menu_ascii_lower(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return (char)(c + ('a' - 'A'));
    }
    return c;
}

static char config_menu_ascii_upper(char c)
{
    if (c >= 'a' && c <= 'z') {
        return (char)(c - ('a' - 'A'));
    }
    return c;
}

static uint8_t config_menu_str_ieq(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return 0U;
    }
    while (*a != '\0' && *b != '\0') {
        if (config_menu_ascii_lower(*a) != config_menu_ascii_lower(*b)) {
            return 0U;
        }
        a++;
        b++;
    }
    return (*a == '\0' && *b == '\0') ? 1U : 0U;
}

static uint8_t config_menu_str_starts_ieq(const char *text, const char *prefix)
{
    if (text == NULL || prefix == NULL) {
        return 0U;
    }
    while (*prefix != '\0') {
        if (*text == '\0' ||
            config_menu_ascii_lower(*text) != config_menu_ascii_lower(*prefix)) {
            return 0U;
        }
        text++;
        prefix++;
    }
    return 1U;
}

static void config_menu_ascii_lower_in_place(char *text)
{
    if (text == NULL) {
        return;
    }
    while (*text != '\0') {
        *text = config_menu_ascii_lower(*text);
        text++;
    }
}

static uint8_t config_menu_is_space(char c)
{
    return (c == ' ' || c == '\t' || c == '\r' || c == '\n') ? 1U : 0U;
}

static char *config_menu_trim(char *text)
{
    char *end;

    if (text == NULL) {
        return NULL;
    }
    if ((uint8_t)text[0] == 0xEFU &&
        (uint8_t)text[1] == 0xBBU &&
        (uint8_t)text[2] == 0xBFU) {
        text += 3;
    }
    while (config_menu_is_space(*text) != 0U) {
        text++;
    }
    end = text + strlen(text);
    while (end > text && config_menu_is_space(end[-1]) != 0U) {
        end--;
    }
    *end = '\0';
    return text;
}

static char *config_menu_parse_config_line(char *line, char **out_value)
{
    char *hash;
    char *eq;
    char *key;
    char *value;

    if (out_value != NULL) {
        *out_value = NULL;
    }
    if (line == NULL || out_value == NULL) {
        return NULL;
    }

    hash = strchr(line, '#');
    if (hash != NULL) {
        *hash = '\0';
    }
    key = config_menu_trim(line);
    if (key == NULL || key[0] == '\0') {
        return NULL;
    }

    eq = strchr(key, '=');
    if (eq == NULL) {
        return NULL;
    }
    *eq = '\0';
    value = config_menu_trim(eq + 1);
    key = config_menu_trim(key);
    if (key == NULL || value == NULL || key[0] == '\0') {
        return NULL;
    }

    config_menu_ascii_lower_in_place(key);
    *out_value = value;
    return key;
}

static char config_menu_path_eq_char(char c)
{
    if (c == '\\') {
        c = '/';
    }
    return config_menu_ascii_lower(c);
}

static uint8_t config_menu_path_ieq(const char *a, const char *b)
{
    if (a == NULL || b == NULL || a[0] == '\0' || b[0] == '\0') {
        return 0U;
    }
    while (*a != '\0' && *b != '\0') {
        if (config_menu_path_eq_char(*a) != config_menu_path_eq_char(*b)) {
            return 0U;
        }
        a++;
        b++;
    }
    return (*a == '\0' && *b == '\0') ? 1U : 0U;
}

static uint8_t config_menu_smartport_path_in_use(const config_menu_t *menu,
                                                 uint8_t device,
                                                 const char *path)
{
    uint8_t i;

    if (menu == NULL || path == NULL || path[0] == '\0') {
        return 0U;
    }
    for (i = 0U; i < SMARTPORT_DEVICE_COUNT; ++i) {
        if (i != (uint8_t)(device - 1U) &&
            menu->smartport_slots[i] != 0U &&
            config_menu_path_ieq(path, menu->smartport_disk_paths[i]) != 0U) {
            return 1U;
        }
    }
    return 0U;
}

static uint8_t config_menu_smartport_path_used_before(const config_menu_t *menu,
                                                      uint8_t device,
                                                      const char *path)
{
    uint8_t i;
    const uint8_t index = (uint8_t)(device - 1U);

    if (menu == NULL || path == NULL || path[0] == '\0') {
        return 0U;
    }
    for (i = 0U; i < index; ++i) {
        if (menu->smartport_slots[i] != 0U &&
            config_menu_path_ieq(path, menu->smartport_disk_paths[i]) != 0U) {
            return 1U;
        }
    }
    return 0U;
}

const char *config_menu_basename(const char *path)
{
    const char *name = path;

    if (path == NULL) {
        return "";
    }
    while (*path != '\0') {
        if (*path == '/' || *path == '\\' || *path == ':') {
            name = path + 1;
        }
        path++;
    }
    return name;
}

static uint8_t config_menu_has_smartport_ext(const char *name, FSIZE_t size)
{
    const char *dot = NULL;

    if (name == NULL) {
        return 0U;
    }
    while (*name != '\0') {
        if (*name == '.') {
            dot = name;
        }
        name++;
    }
    if (dot == NULL) {
        return 0U;
    }
    return (config_menu_str_ieq(dot, ".hdv") != 0U ||
            config_menu_str_ieq(dot, ".2mg") != 0U ||
            config_menu_str_ieq(dot, ".2img") != 0U ||
            (config_menu_str_ieq(dot, ".po") != 0U &&
             (size == (FSIZE_t)CONFIG_SMARTPORT_140K_PO_IMAGE_BYTES ||
              size == (FSIZE_t)CONFIG_SMARTPORT_800K_PO_IMAGE_BYTES))) ? 1U : 0U;
}

static uint8_t config_menu_has_disk2_ext(const char *name, FSIZE_t size)
{
    const char *dot = NULL;

    if (name == NULL) {
        return 0U;
    }
    while (*name != '\0') {
        if (*name == '.') {
            dot = name;
        }
        name++;
    }
    if (dot == NULL) {
        return 0U;
    }
    return (config_menu_str_ieq(dot, ".woz") != 0U ||
            config_menu_str_ieq(dot, ".nib") != 0U ||
            config_menu_str_ieq(dot, ".dsk") != 0U ||
            config_menu_str_ieq(dot, ".do") != 0U ||
            (config_menu_str_ieq(dot, ".po") != 0U &&
             size == (FSIZE_t)CONFIG_DISK2_PO_IMAGE_BYTES)) ? 1U : 0U;
}

static uint8_t config_menu_has_png_ext(const char *name)
{
    const char *dot = NULL;

    if (name == NULL) {
        return 0U;
    }
    while (*name != '\0') {
        if (*name == '.') {
            dot = name;
        }
        name++;
    }
    return (dot != NULL && config_menu_str_ieq(dot, ".png") != 0U) ? 1U : 0U;
}

static uint8_t config_menu_is_video_rom_file(const char *name, FSIZE_t size)
{
    /* //e video character ROMs are exactly 4 KB (2732) or 8 KB (dual-charset).
     * Match by size so any dump shows regardless of extension (.rom, .bin, or
     * none) -- this is also exactly what config_menu_apply_video_rom accepts. */
    (void)name;
    return (size == 4096U || size == 8192U) ? 1U : 0U;
}

static void config_menu_copy_text(char *dst, size_t dst_len, const char *src)
{
    if (dst == NULL || dst_len == 0U) {
        return;
    }
    if (src == NULL) {
        src = "";
    }
    (void)snprintf(dst, dst_len, "%s", src);
}

static void config_menu_default_smartport_path(uint8_t device, char *dst, size_t dst_len)
{
    if (dst == NULL || dst_len == 0U) {
        return;
    }
    if (device == 0U || device > SMARTPORT_DEVICE_COUNT) {
        dst[0] = '\0';
        return;
    }
    (void)snprintf(dst, dst_len, "0:/DISK%u.hdv", (unsigned)device);
}

uint8_t config_menu_pan_clamp(uint32_t pan)
{
    return (pan > 15U) ? 15U : (uint8_t)pan;
}

static uint8_t config_menu_disk2_sound_volume_clamp(uint32_t volume)
{
    return (volume > CONFIG_MAX_DISK2_SOUND_VOLUME) ?
        CONFIG_MAX_DISK2_SOUND_VOLUME : (uint8_t)volume;
}

static uint8_t config_menu_mouse_sensitivity_clamp(uint32_t sensitivity)
{
    if (sensitivity < MOUSE_SENSITIVITY_MIN) {
        return MOUSE_SENSITIVITY_MIN;
    }
    if (sensitivity > MOUSE_SENSITIVITY_MAX) {
        return MOUSE_SENSITIVITY_MAX;
    }
    return (uint8_t)sensitivity;
}

static uint32_t config_menu_mouse_sensitivity_index(uint8_t sensitivity)
{
    uint32_t best_index = 0U;
    uint32_t best_distance = 0xFFFFFFFFU;

    for (uint32_t i = 0U; i < MOUSE_SENSITIVITY_STEP_COUNT; ++i) {
        const uint32_t step = k_mouse_sensitivity_steps[i];
        const uint32_t distance = (sensitivity > step) ?
            ((uint32_t)sensitivity - step) : (step - (uint32_t)sensitivity);

        if (distance < best_distance) {
            best_distance = distance;
            best_index = i;
        }
    }

    return best_index;
}

static uint8_t config_menu_path_is_root(const char *path)
{
    return (path == NULL ||
            path[0] == '\0' ||
            strcmp(path, "0:") == 0 ||
            strcmp(path, "0:/") == 0) ? 1U : 0U;
}

static uint8_t config_menu_join_path(const char *dir,
                                     const char *name,
                                     char *out,
                                     size_t out_len)
{
    int len;
    size_t dir_len;

    if (name == NULL || out == NULL || out_len == 0U) {
        return 0U;
    }
    if (dir == NULL || dir[0] == '\0') {
        dir = "0:/";
    }

    dir_len = strlen(dir);
    if (dir_len == 0U || dir[dir_len - 1U] == '/' || dir[dir_len - 1U] == '\\') {
        len = snprintf(out, out_len, "%s%s", dir, name);
    } else {
        len = snprintf(out, out_len, "%s/%s", dir, name);
    }
    return (len > 0 && len < (int)out_len) ? 1U : 0U;
}

static void config_menu_parent_path(const char *path, char *out, size_t out_len)
{
    char tmp[CONFIG_MENU_PATH_LEN];
    size_t len;

    if (out == NULL || out_len == 0U) {
        return;
    }
    if (config_menu_path_is_root(path) != 0U) {
        config_menu_copy_text(out, out_len, "0:/");
        return;
    }

    config_menu_copy_text(tmp, sizeof(tmp), path);
    len = strlen(tmp);
    while (len > 3U && (tmp[len - 1U] == '/' || tmp[len - 1U] == '\\')) {
        tmp[--len] = '\0';
    }
    while (len > 3U && tmp[len - 1U] != '/' && tmp[len - 1U] != '\\') {
        len--;
    }
    if (len <= 3U) {
        config_menu_copy_text(out, out_len, "0:/");
    } else {
        tmp[len - 1U] = '\0';
        config_menu_copy_text(out, out_len, tmp);
    }
}

static FRESULT config_menu_mount_sd(void)
{
    FRESULT fr = f_mount(&g_config_fs, "0:/", 1U);

    if (fr == FR_OK) {
        return fr;
    }

    (void)disk_initialize(0);
    (void)f_mount((FATFS *)0, "0:/", 0U);
    return f_mount(&g_config_fs, "0:/", 1U);
}

static FRESULT config_menu_open_path(FIL *file, const char *path, BYTE mode)
{
    FRESULT fr;

    if (file == NULL || path == NULL || path[0] == '\0') {
        return FR_INVALID_OBJECT;
    }

    fr = f_open(file, path, mode);
    if (fr == FR_OK || fr == FR_NO_FILE) {
        return fr;
    }

    fr = config_menu_mount_sd();
    if (fr != FR_OK) {
        return fr;
    }

    return f_open(file, path, mode);
}

static FRESULT config_menu_open_cfg(FIL *file, BYTE mode)
{
    return config_menu_open_path(file, APPLETINI_CFG_PATH, mode);
}

const char *config_menu_boot_timeout_text(uint8_t mode)
{
    switch (mode) {
    case CONFIG_BOOT_TIMEOUT_3S:
        return "3 seconds";
    case CONFIG_BOOT_TIMEOUT_5S:
        return "5 seconds";
    case CONFIG_BOOT_TIMEOUT_ALWAYS:
    default:
        return "Always show";
    }
}

const char *config_menu_boot_device_text(uint8_t device)
{
    switch (device) {
    case CONFIG_BOOT_DEVICE_DISK2:
        return "Disk II";
    case CONFIG_BOOT_DEVICE_SMARTPORT:
    default:
        return "SmartPort";
    }
}

static uint8_t config_menu_usb_binding_source_valid(usb_hid_menu_source_t source)
{
    for (uint32_t i = 0U;
         i < (sizeof(k_usb_binding_source_order) /
              sizeof(k_usb_binding_source_order[0]));
         ++i) {
        if (k_usb_binding_source_order[i] == source) {
            return 1U;
        }
    }
    return usb_hid_menu_source_is_keyboard(source);
}

static uint8_t config_menu_usb_binding_mouse_button_source_valid(
    usb_hid_menu_source_t source)
{
    for (uint32_t i = 0U;
         i < (sizeof(k_usb_binding_button_source_order) /
              sizeof(k_usb_binding_button_source_order[0]));
         ++i) {
        if (k_usb_binding_button_source_order[i] == source) {
            return 1U;
        }
    }
    return 0U;
}

static uint8_t config_menu_usb_binding_ok_source_valid(usb_hid_menu_source_t source)
{
    return (config_menu_usb_binding_mouse_button_source_valid(source) != 0U ||
            usb_hid_menu_source_is_keyboard(source) != 0U) ? 1U : 0U;
}

static uint8_t config_menu_usb_binding_action_is_button(uint32_t action)
{
    return (action == CONFIG_MENU_USB_BIND_ACTION_OK ||
            action == CONFIG_MENU_USB_BIND_ACTION_BACK) ? 1U : 0U;
}

static uint8_t config_menu_usb_binding_action_is_screenshot(uint32_t action)
{
    return (action == CONFIG_MENU_USB_BIND_ACTION_SCREENSHOT_A2 ||
            action == CONFIG_MENU_USB_BIND_ACTION_SCREENSHOT_1080P) ? 1U : 0U;
}

static uint8_t config_menu_usb_binding_source_valid_for_action(
    uint32_t action,
    usb_hid_menu_source_t source)
{
    if (config_menu_usb_binding_action_is_screenshot(action) != 0U) {
        return (source == USB_HID_MENU_SOURCE_NONE ||
                usb_hid_menu_source_is_keyboard(source) != 0U) ? 1U : 0U;
    }

    return config_menu_usb_binding_source_valid(source);
}

static usb_hid_menu_source_t config_menu_usb_binding_source_clamp_for_action(
    uint32_t action,
    uint32_t source)
{
    const usb_hid_menu_source_t value =
        (source > 0xFFFFU) ? USB_HID_MENU_SOURCE_NONE : (usb_hid_menu_source_t)source;

    return (config_menu_usb_binding_source_valid_for_action(action, value) != 0U) ?
        value : USB_HID_MENU_SOURCE_NONE;
}

static uint32_t config_menu_usb_binding_source_index(usb_hid_menu_source_t source)
{
    for (uint32_t i = 0U;
         i < (sizeof(k_usb_binding_source_order) /
              sizeof(k_usb_binding_source_order[0]));
         ++i) {
        if (k_usb_binding_source_order[i] == source) {
            return i;
        }
    }
    return 0U;
}

static uint32_t config_menu_usb_binding_button_source_index(usb_hid_menu_source_t source)
{
    for (uint32_t i = 0U;
         i < (sizeof(k_usb_binding_button_source_order) /
              sizeof(k_usb_binding_button_source_order[0]));
         ++i) {
        if (k_usb_binding_button_source_order[i] == source) {
            return i;
        }
    }
    return 2U;
}

static uint32_t config_menu_usb_binding_screenshot_source_index(
    usb_hid_menu_source_t source)
{
    for (uint32_t i = 0U;
         i < (sizeof(k_usb_binding_screenshot_source_order) /
              sizeof(k_usb_binding_screenshot_source_order[0]));
         ++i) {
        if (k_usb_binding_screenshot_source_order[i] == source) {
            return i;
        }
    }
    return 0U;
}

static usb_hid_menu_source_t config_menu_usb_binding_next_ordered_source(
    const usb_hid_menu_source_t *order,
    uint32_t count,
    uint32_t index,
    int8_t delta)
{
    if (order == NULL || count == 0U) {
        return USB_HID_MENU_SOURCE_NONE;
    }

    if (delta < 0) {
        index = (index == 0U) ? (count - 1U) : (index - 1U);
    } else {
        index = (index + 1U) % count;
    }
    return order[index];
}

static usb_hid_menu_source_t config_menu_usb_binding_next_source(
    uint32_t action,
    usb_hid_menu_source_t source,
    int8_t delta)
{
    if (config_menu_usb_binding_action_is_button(action) != 0U) {
        return config_menu_usb_binding_next_ordered_source(
            k_usb_binding_button_source_order,
            sizeof(k_usb_binding_button_source_order) /
                sizeof(k_usb_binding_button_source_order[0]),
            config_menu_usb_binding_button_source_index(source),
            delta);
    }
    if (config_menu_usb_binding_action_is_screenshot(action) != 0U) {
        return config_menu_usb_binding_next_ordered_source(
            k_usb_binding_screenshot_source_order,
            sizeof(k_usb_binding_screenshot_source_order) /
                sizeof(k_usb_binding_screenshot_source_order[0]),
            config_menu_usb_binding_screenshot_source_index(source),
            delta);
    }

    return config_menu_usb_binding_next_ordered_source(
        k_usb_binding_source_order,
        sizeof(k_usb_binding_source_order) /
            sizeof(k_usb_binding_source_order[0]),
        config_menu_usb_binding_source_index(source),
        delta);
}

static void config_menu_usb_bindings_set_defaults(config_menu_t *menu)
{
    if (menu == NULL) {
        return;
    }

    for (uint32_t i = 0U; i < CONFIG_MENU_USB_BIND_ACTION_COUNT; ++i) {
        menu->usb_menu_bindings[i] = k_usb_binding_defaults[i];
    }
    menu->usb_binding_capture = CONFIG_MENU_USB_BIND_CAPTURE_NONE;
}

static void config_menu_assign_usb_binding(config_menu_t *menu,
                                           uint32_t action,
                                           usb_hid_menu_source_t source)
{
    if (menu == NULL || action >= CONFIG_MENU_USB_BIND_ACTION_COUNT) {
        return;
    }

    source = config_menu_usb_binding_source_clamp_for_action(action, source);
    if (config_menu_usb_binding_action_is_button(action) != 0U &&
        config_menu_usb_binding_ok_source_valid(source) == 0U) {
        source = k_usb_binding_defaults[action];
    }

    if (source != USB_HID_MENU_SOURCE_NONE) {
        for (uint32_t i = 0U; i < CONFIG_MENU_USB_BIND_ACTION_COUNT; ++i) {
            if (i != action && menu->usb_menu_bindings[i] == source) {
                menu->usb_menu_bindings[i] = USB_HID_MENU_SOURCE_NONE;
            }
        }
    }
    menu->usb_menu_bindings[action] = source;
}

static void config_menu_usb_bindings_coerce(config_menu_t *menu)
{
    if (menu == NULL) {
        return;
    }

    for (uint32_t i = 0U; i < CONFIG_MENU_USB_BIND_ACTION_COUNT; ++i) {
        menu->usb_menu_bindings[i] =
            config_menu_usb_binding_source_clamp_for_action(
                i,
                menu->usb_menu_bindings[i]);
    }
    for (uint32_t i = 0U; i < CONFIG_MENU_USB_BIND_ACTION_COUNT; ++i) {
        if (config_menu_usb_binding_action_is_button(i) != 0U &&
            config_menu_usb_binding_ok_source_valid(menu->usb_menu_bindings[i]) == 0U) {
            menu->usb_menu_bindings[i] = k_usb_binding_defaults[i];
        }
    }
}

static const char *config_menu_usb_key_usage_config(uint8_t usage)
{
    static char text[16];

    if (usage >= CONFIG_USB_KEY_USAGE_A &&
        usage < (CONFIG_USB_KEY_USAGE_A + 26U)) {
        text[0] = (char)('A' + (usage - CONFIG_USB_KEY_USAGE_A));
        text[1] = '\0';
        return text;
    }
    if (usage >= CONFIG_USB_KEY_USAGE_1 &&
        usage <= CONFIG_USB_KEY_USAGE_0) {
        text[0] = (usage == CONFIG_USB_KEY_USAGE_0) ?
            '0' : (char)('1' + (usage - CONFIG_USB_KEY_USAGE_1));
        text[1] = '\0';
        return text;
    }
    if (usage >= CONFIG_USB_KEY_USAGE_F1 &&
        usage <= CONFIG_USB_KEY_USAGE_F12) {
        (void)snprintf(text,
                       sizeof(text),
                       "F%u",
                       (unsigned)(usage - CONFIG_USB_KEY_USAGE_F1 + 1U));
        return text;
    }
    if (usage >= CONFIG_USB_KEY_USAGE_F13 &&
        usage <= CONFIG_USB_KEY_USAGE_F24) {
        (void)snprintf(text,
                       sizeof(text),
                       "F%u",
                       (unsigned)(usage - CONFIG_USB_KEY_USAGE_F13 + 13U));
        return text;
    }

    switch (usage) {
    case CONFIG_USB_KEY_USAGE_ENTER:
        return "ENTER";
    case CONFIG_USB_KEY_USAGE_ESCAPE:
        return "ESC";
    case CONFIG_USB_KEY_USAGE_BACKSPACE:
        return "BACKSPACE";
    case CONFIG_USB_KEY_USAGE_TAB:
        return "TAB";
    case CONFIG_USB_KEY_USAGE_SPACE:
        return "SPACE";
    case CONFIG_USB_KEY_USAGE_PRINTSCN:
        return "PRINTSCREEN";
    default:
        (void)snprintf(text, sizeof(text), "0X%02X", (unsigned)usage);
        return text;
    }
}

static const char *config_menu_usb_binding_source_config(
    usb_hid_menu_source_t source)
{
    static char text[24];

    switch (source) {
    case USB_HID_MENU_SOURCE_NONE:
        return "NONE";
    case USB_HID_MENU_ACTION_LEFT:
        return "MOUSE.LEFT";
    case USB_HID_MENU_ACTION_RIGHT:
        return "MOUSE.RIGHT";
    case USB_HID_MENU_ACTION_SELECT:
        return "MOUSE.MIDDLE";
    case USB_HID_MENU_ACTION_ITEM_DOWN:
        return "MOUSE.BUTTON4";
    case USB_HID_MENU_ACTION_ITEM_UP:
        return "MOUSE.BUTTON5";
    case USB_HID_MENU_ACTION_PREV_TAB:
        return "WHEEL.UP";
    case USB_HID_MENU_ACTION_NEXT_TAB:
        return "WHEEL.DOWN";
    default:
        if (usb_hid_menu_source_is_keyboard(source) != 0U) {
            (void)snprintf(
                text,
                sizeof(text),
                "KEY.%s",
                config_menu_usb_key_usage_config(
                    (uint8_t)(source & USB_HID_MENU_SOURCE_KEY_MASK)));
            return text;
        }
        return "NONE";
    }
}

static uint8_t config_menu_usb_key_usage_value(const char *value,
                                               uint8_t *out_usage)
{
    const char *key;
    unsigned long numeric;

    if (value == NULL || out_usage == NULL) {
        return 0U;
    }
    key = value;
    if (config_menu_str_starts_ieq(key, "key.") == 0U) {
        return 0U;
    }
    key += 4;

    if (key[0] != '\0' && key[1] == '\0') {
        const char c = config_menu_ascii_upper(key[0]);
        if (c >= 'A' && c <= 'Z') {
            *out_usage = (uint8_t)(CONFIG_USB_KEY_USAGE_A + (uint8_t)(c - 'A'));
            return 1U;
        }
        if (c >= '1' && c <= '9') {
            *out_usage = (uint8_t)(CONFIG_USB_KEY_USAGE_1 + (uint8_t)(c - '1'));
            return 1U;
        }
        if (c == '0') {
            *out_usage = CONFIG_USB_KEY_USAGE_0;
            return 1U;
        }
    }

    if (config_menu_ascii_upper(key[0]) == 'F' &&
        key[1] >= '0' && key[1] <= '9') {
        numeric = strtoul(key + 1, NULL, 10);
        if (numeric >= 1UL && numeric <= 12UL) {
            *out_usage =
                (uint8_t)(CONFIG_USB_KEY_USAGE_F1 + (uint8_t)(numeric - 1UL));
            return 1U;
        }
        if (numeric >= 13UL && numeric <= 24UL) {
            *out_usage =
                (uint8_t)(CONFIG_USB_KEY_USAGE_F13 + (uint8_t)(numeric - 13UL));
            return 1U;
        }
    }

    if (config_menu_str_ieq(key, "enter") != 0U) {
        *out_usage = CONFIG_USB_KEY_USAGE_ENTER;
        return 1U;
    }
    if (config_menu_str_ieq(key, "esc") != 0U) {
        *out_usage = CONFIG_USB_KEY_USAGE_ESCAPE;
        return 1U;
    }
    if (config_menu_str_ieq(key, "backspace") != 0U) {
        *out_usage = CONFIG_USB_KEY_USAGE_BACKSPACE;
        return 1U;
    }
    if (config_menu_str_ieq(key, "tab") != 0U) {
        *out_usage = CONFIG_USB_KEY_USAGE_TAB;
        return 1U;
    }
    if (config_menu_str_ieq(key, "space") != 0U) {
        *out_usage = CONFIG_USB_KEY_USAGE_SPACE;
        return 1U;
    }
    if (config_menu_str_ieq(key, "printscreen") != 0U) {
        *out_usage = CONFIG_USB_KEY_USAGE_PRINTSCN;
        return 1U;
    }

    if (key[0] == '0' &&
        (key[1] == 'x' || key[1] == 'X')) {
        numeric = strtoul(key + 2, NULL, 16);
    } else {
        numeric = strtoul(key, NULL, 10);
    }
    if (numeric > 0UL && numeric <= 0xFFUL) {
        *out_usage = (uint8_t)numeric;
        return 1U;
    }

    return 0U;
}

static usb_hid_menu_source_t config_menu_usb_binding_source_value(const char *value)
{
    uint8_t usage = 0U;

    if (value == NULL) {
        return USB_HID_MENU_SOURCE_NONE;
    }
    if (config_menu_str_ieq(value, "none") != 0U) {
        return USB_HID_MENU_SOURCE_NONE;
    }
    if (config_menu_str_ieq(value, "mouse.left") != 0U) {
        return USB_HID_MENU_ACTION_LEFT;
    }
    if (config_menu_str_ieq(value, "mouse.right") != 0U) {
        return USB_HID_MENU_ACTION_RIGHT;
    }
    if (config_menu_str_ieq(value, "mouse.middle") != 0U) {
        return USB_HID_MENU_ACTION_SELECT;
    }
    if (config_menu_str_ieq(value, "mouse.button4") != 0U) {
        return USB_HID_MENU_ACTION_ITEM_DOWN;
    }
    if (config_menu_str_ieq(value, "mouse.button5") != 0U) {
        return USB_HID_MENU_ACTION_ITEM_UP;
    }
    if (config_menu_str_ieq(value, "wheel.up") != 0U) {
        return USB_HID_MENU_ACTION_PREV_TAB;
    }
    if (config_menu_str_ieq(value, "wheel.down") != 0U) {
        return USB_HID_MENU_ACTION_NEXT_TAB;
    }
    if (config_menu_usb_key_usage_value(value, &usage) != 0U) {
        return CONFIG_USB_KEY_SOURCE(usage);
    }
    return USB_HID_MENU_SOURCE_NONE;
}

static uint8_t config_menu_parse_usb_binding(config_menu_t *menu,
                                             const char *key,
                                             const char *value)
{
    if (menu == NULL || key == NULL || value == NULL) {
        return 0U;
    }

    for (uint32_t i = 0U; i < CONFIG_MENU_USB_BIND_ACTION_COUNT; ++i) {
        if (strcmp(key, k_usb_binding_config_keys[i]) == 0) {
            menu->usb_menu_bindings[i] =
                config_menu_usb_binding_source_value(value);
            return 1U;
        }
    }
    return 0U;
}

const char *config_menu_usb_binding_action_text(uint32_t action)
{
    if (action >= CONFIG_MENU_USB_BIND_ACTION_COUNT) {
        return "";
    }
    return k_usb_binding_action_text[action];
}

const char *config_menu_usb_binding_source_text(usb_hid_menu_source_t source)
{
    switch (source) {
    case USB_HID_MENU_ACTION_LEFT:
        return "USB L";
    case USB_HID_MENU_ACTION_RIGHT:
        return "USB R";
    case USB_HID_MENU_ACTION_SELECT:
        return "USB Mid";
    case USB_HID_MENU_ACTION_ITEM_DOWN:
        return "USB B4";
    case USB_HID_MENU_ACTION_ITEM_UP:
        return "USB B5";
    case USB_HID_MENU_ACTION_PREV_TAB:
        return "Wheel Up";
    case USB_HID_MENU_ACTION_NEXT_TAB:
        return "Wheel Dn";
    default:
        return usb_hid_menu_source_text(source);
    }
}

uint8_t config_menu_usb_binding_capture_action(const config_menu_t *menu)
{
    if (menu == NULL ||
        menu->usb_binding_capture >= CONFIG_MENU_USB_BIND_ACTION_COUNT) {
        return CONFIG_MENU_USB_BIND_CAPTURE_NONE;
    }
    return menu->usb_binding_capture;
}

uint8_t config_menu_capture_usb_binding(config_menu_t *menu,
                                        usb_hid_menu_source_t source)
{
    const uint8_t action = config_menu_usb_binding_capture_action(menu);
    char text[CONFIG_MENU_STATUS_LEN];

    if (menu == NULL || action == CONFIG_MENU_USB_BIND_CAPTURE_NONE) {
        return 0U;
    }
    if (menu->usb_bindings_editable == 0U) {
        menu->usb_binding_capture = CONFIG_MENU_USB_BIND_CAPTURE_NONE;
        config_menu_set_status(menu, 1U, "USB BINDINGS EDITABLE AT BOOT");
        return 1U;
    }
    if (source == USB_HID_MENU_SOURCE_NONE) {
        return 0U;
    }
    if (config_menu_usb_binding_action_is_screenshot(action) != 0U &&
        usb_hid_menu_source_is_keyboard(source) == 0U) {
        config_menu_set_status(menu, 1U, "SCREENSHOT REQUIRES USB KEY");
        return 1U;
    }
    if (config_menu_usb_binding_source_valid_for_action(action, source) == 0U) {
        return 0U;
    }
    if (config_menu_usb_binding_action_is_button(action) != 0U &&
        config_menu_usb_binding_ok_source_valid(source) == 0U) {
        (void)snprintf(text,
                       sizeof(text),
                       "%s REQUIRES USB INPUT",
                       config_menu_usb_binding_action_text(action));
        config_menu_set_status(menu, 1U, text);
        return 1U;
    }

    config_menu_assign_usb_binding(menu, action, source);
    menu->usb_binding_capture = CONFIG_MENU_USB_BIND_CAPTURE_NONE;
    config_menu_save_settings(menu);
    (void)snprintf(text,
                   sizeof(text),
                   "USB %s: %s",
                   config_menu_usb_binding_action_text(action),
                   config_menu_usb_binding_source_text(source));
    config_menu_set_status(menu, 0U, text);
    return 1U;
}

ui_key_t config_menu_translate_usb_binding(const config_menu_t *menu,
                                           usb_hid_menu_source_t source)
{
    if (menu == NULL ||
        source == USB_HID_MENU_SOURCE_NONE ||
        config_menu_usb_binding_source_valid(source) == 0U) {
        return UI_KEY_NONE;
    }

    if (menu->usb_menu_bindings[CONFIG_MENU_USB_BIND_ACTION_OK] == source) {
        return UI_KEY_ENTER;
    }
    if (menu->usb_menu_bindings[CONFIG_MENU_USB_BIND_ACTION_BACK] == source) {
        return UI_KEY_BACK;
    }
    for (uint32_t i = 0U; i < CONFIG_MENU_USB_BIND_ACTION_COUNT; ++i) {
        if (menu->usb_menu_bindings[i] == source) {
            return k_usb_binding_keys[i];
        }
    }
    return UI_KEY_NONE;
}

usb_hid_menu_source_t config_menu_usb_ok_binding_source(const config_menu_t *menu)
{
    usb_hid_menu_source_t source;

    if (menu == NULL) {
        return USB_HID_MENU_ACTION_SELECT;
    }
    source = menu->usb_menu_bindings[CONFIG_MENU_USB_BIND_ACTION_OK];
    return (config_menu_usb_binding_ok_source_valid(source) != 0U) ?
        source : USB_HID_MENU_ACTION_SELECT;
}

usb_hid_menu_source_t config_menu_usb_open_close_binding_source(const config_menu_t *menu)
{
    return config_menu_usb_ok_binding_source(menu);
}

usb_hid_menu_source_t config_menu_usb_screenshot_a2_binding_source(
    const config_menu_t *menu)
{
    if (menu == NULL) {
        return k_usb_binding_defaults[CONFIG_MENU_USB_BIND_ACTION_SCREENSHOT_A2];
    }
    return config_menu_usb_binding_source_clamp_for_action(
        CONFIG_MENU_USB_BIND_ACTION_SCREENSHOT_A2,
        menu->usb_menu_bindings[CONFIG_MENU_USB_BIND_ACTION_SCREENSHOT_A2]);
}

usb_hid_menu_source_t config_menu_usb_screenshot_1080p_binding_source(
    const config_menu_t *menu)
{
    if (menu == NULL) {
        return k_usb_binding_defaults[CONFIG_MENU_USB_BIND_ACTION_SCREENSHOT_1080P];
    }
    return config_menu_usb_binding_source_clamp_for_action(
        CONFIG_MENU_USB_BIND_ACTION_SCREENSHOT_1080P,
        menu->usb_menu_bindings[CONFIG_MENU_USB_BIND_ACTION_SCREENSHOT_1080P]);
}

const char *config_menu_video_output_text(uint8_t mono)
{
    return (mono != 0U) ? "Monochrome" : "Color";
}

const char *config_menu_video7_auto_mono_text(uint8_t enabled)
{
    return config_menu_enabled_text(enabled);
}

const char *config_menu_border_color_text(uint8_t color)
{
    static const char *const names[APPLE_VIDEO_IIGS_BORDER_COLOR_COUNT] = {
        "Black", "Deep red", "Dark blue", "Purple",
        "Dark green", "Dark gray", "Medium blue", "Light blue",
        "Brown", "Orange", "Light gray", "Pink",
        "Green", "Yellow", "Aqua", "White"
    };

    return names[apple_video_iigs_border_color_clamp(color)];
}

const char *config_menu_border_outside_text(uint8_t flood)
{
    return (flood != 0u) ? "Flood" : "Bezel";
}

static const char *config_menu_video_mono_color_text(uint8_t color)
{
    switch (apple_video_mono_color_clamp(color)) {
    case APPLE_VIDEO_MONO_GREEN:
        return "Green";
    case APPLE_VIDEO_MONO_AMBER:
        return "Amber";
    case APPLE_VIDEO_MONO_WHITE:
    default:
        return "White";
    }
}

static const char *config_menu_video_mono_color_config(uint8_t color)
{
    switch (apple_video_mono_color_clamp(color)) {
    case APPLE_VIDEO_MONO_GREEN:
        return "GREEN";
    case APPLE_VIDEO_MONO_AMBER:
        return "AMBER";
    case APPLE_VIDEO_MONO_WHITE:
    default:
        return "WHITE";
    }
}

static const char *config_menu_video_color_mode_text(uint8_t mode)
{
    switch (apple_video_color_mode_clamp(mode)) {
    case APPLE_VIDEO_COLOR_IDEALIZED:
        return "Idealized";
    case APPLE_VIDEO_COLOR_RGB:
        return "RGB";
    case APPLE_VIDEO_COLOR_TV:
        return "Color TV";
    case APPLE_VIDEO_COLOR_PAL_ACCURATE_COMPOSITE:
        return "PAL Accurate Composite";
    case APPLE_VIDEO_COLOR_PAL_ACCURATE_TV:
        return "PAL Accurate TV";
    case APPLE_VIDEO_COLOR_COMPOSITE_MONITOR:
    default:
        return "Composite Monitor";
    }
}

static const char *config_menu_video_color_mode_config(uint8_t mode)
{
    switch (apple_video_color_mode_clamp(mode)) {
    case APPLE_VIDEO_COLOR_IDEALIZED:
        return "IDEALIZED";
    case APPLE_VIDEO_COLOR_RGB:
        return "RGB";
    case APPLE_VIDEO_COLOR_TV:
        return "COLOR_TV";
    case APPLE_VIDEO_COLOR_PAL_ACCURATE_COMPOSITE:
        return "PAL_ACCURATE_COMPOSITE";
    case APPLE_VIDEO_COLOR_PAL_ACCURATE_TV:
        return "PAL_ACCURATE_TV";
    case APPLE_VIDEO_COLOR_COMPOSITE_MONITOR:
    default:
        return "COMPOSITE_MONITOR";
    }
}

static uint8_t config_menu_pal_accurate_modes_allowed(const config_menu_t *menu)
{
    if (menu != NULL && menu->platform.is_apple_video_50hz != NULL) {
        return (menu->platform.is_apple_video_50hz(menu->platform.ctx) != 0U) ?
            1U : 0U;
    }
    return 0U;
}

static uint8_t config_menu_video_color_mode_allowed(const config_menu_t *menu,
                                                    uint8_t mode)
{
    if (apple_video_color_mode_is_pal_accurate(mode) == 0U) {
        return 1U;
    }
    return config_menu_pal_accurate_modes_allowed(menu);
}

const char *config_menu_video_variant_label(const config_menu_t *menu)
{
    return (menu != NULL && menu->video_output_mono != 0U) ?
        "Mono color" : "Color mode";
}

const char *config_menu_video_variant_text(const config_menu_t *menu)
{
    if (menu != NULL && menu->video_output_mono != 0U) {
        return config_menu_video_mono_color_text(menu->video_mono_color);
    }
    if (menu != NULL &&
        config_menu_video_color_mode_allowed(menu, menu->video_color_mode) == 0U) {
        return config_menu_video_color_mode_text(CONFIG_DEFAULT_VIDEO_COLOR_MODE);
    }
    return config_menu_video_color_mode_text(
        (menu != NULL) ? menu->video_color_mode : CONFIG_DEFAULT_VIDEO_COLOR_MODE);
}

uint8_t config_menu_video_pal_accurate_help_visible(const config_menu_t *menu)
{
    return (menu != NULL &&
            menu->video_output_mono == 0U &&
            apple_video_color_mode_is_pal_accurate(menu->video_color_mode) != 0U &&
            config_menu_pal_accurate_modes_allowed(menu) != 0U) ? 1U : 0U;
}

static uint8_t config_menu_video_output_text_value(const char *value)
{
    if (config_menu_str_ieq(value, "monochrome") != 0U) {
        return 1U;
    }
    return 0U;
}

static uint8_t config_menu_video_mono_color_value(const char *value)
{
    if (config_menu_str_ieq(value, "green") != 0U) {
        return APPLE_VIDEO_MONO_GREEN;
    }
    if (config_menu_str_ieq(value, "amber") != 0U) {
        return APPLE_VIDEO_MONO_AMBER;
    }
    return APPLE_VIDEO_MONO_WHITE;
}

static uint8_t config_menu_video_color_mode_value(const char *value)
{
    if (config_menu_str_ieq(value, "idealized") != 0U) {
        return APPLE_VIDEO_COLOR_IDEALIZED;
    }
    if (config_menu_str_ieq(value, "rgb") != 0U) {
        return APPLE_VIDEO_COLOR_RGB;
    }
    if (config_menu_str_ieq(value, "color_tv") != 0U) {
        return APPLE_VIDEO_COLOR_TV;
    }
    if (config_menu_str_ieq(value, "pal_accurate_composite") != 0U) {
        return APPLE_VIDEO_COLOR_PAL_ACCURATE_COMPOSITE;
    }
    if (config_menu_str_ieq(value, "pal_accurate_tv") != 0U) {
        return APPLE_VIDEO_COLOR_PAL_ACCURATE_TV;
    }
    if (config_menu_str_ieq(value, "composite_monitor") != 0U) {
        return APPLE_VIDEO_COLOR_COMPOSITE_MONITOR;
    }
    return CONFIG_DEFAULT_VIDEO_COLOR_MODE;
}

static uint8_t config_menu_next_mono_color(uint8_t color, int8_t delta)
{
    static const uint8_t colors[3] = {
        APPLE_VIDEO_MONO_WHITE,
        APPLE_VIDEO_MONO_GREEN,
        APPLE_VIDEO_MONO_AMBER
    };
    uint32_t index = 0U;

    color = apple_video_mono_color_clamp(color);
    for (uint32_t i = 0U; i < 3U; ++i) {
        if (colors[i] == color) {
            index = i;
            break;
        }
    }

    if (delta < 0) {
        index = (index == 0U) ? 2U : (index - 1U);
    } else {
        index = (index + 1U) % 3U;
    }
    return colors[index];
}

static uint8_t config_menu_next_color_mode(const config_menu_t *menu,
                                           uint8_t mode,
                                           int8_t delta)
{
    mode = apple_video_color_mode_clamp(mode);
    for (uint32_t i = 0U; i < APPLE_VIDEO_COLOR_COUNT; ++i) {
        if (delta < 0) {
            mode = (mode == 0U) ?
                (APPLE_VIDEO_COLOR_COUNT - 1U) : (uint8_t)(mode - 1U);
        } else {
            mode = (uint8_t)((mode + 1U) % APPLE_VIDEO_COLOR_COUNT);
        }
        if (config_menu_video_color_mode_allowed(menu, mode) != 0U) {
            return mode;
        }
    }
    return CONFIG_DEFAULT_VIDEO_COLOR_MODE;
}

const char *config_menu_bezel_text(const config_menu_t *menu)
{
    if (menu == NULL || menu->bezel_path[0] == '\0') {
        return "Auto 0:/bezel.png, 0:/bezels/bezel.png";
    }
    return menu->bezel_path;
}

const char *config_menu_video_rom_text(const config_menu_t *menu)
{
    if (menu == NULL || menu->video_rom_path[0] == '\0') {
        return "Built-in";
    }
    return config_menu_basename(menu->video_rom_path);
}

static uint32_t config_menu_boot_timeout_ticks(uint8_t mode)
{
    switch (mode) {
    case CONFIG_BOOT_TIMEOUT_3S:
        return BOOT_TIMEOUT_TICKS_3S;
    case CONFIG_BOOT_TIMEOUT_5S:
        return BOOT_TIMEOUT_TICKS_5S;
    case CONFIG_BOOT_TIMEOUT_ALWAYS:
    default:
        return BOOT_TIMEOUT_TICKS_ALWAYS;
    }
}

static void config_menu_coerce_boot_device(config_menu_t *menu)
{
    if (menu != NULL &&
        menu->boot_device == CONFIG_BOOT_DEVICE_DISK2 &&
        menu->disk2_slot6_enabled == 0U) {
        menu->boot_device = CONFIG_BOOT_DEVICE_SMARTPORT;
    }
}

static void config_menu_coerce_video_output(config_menu_t *menu)
{
    if (menu == NULL) {
        return;
    }
    menu->video_output_mono = (menu->video_output_mono != 0U) ? 1U : 0U;
    menu->video_mono_color = apple_video_mono_color_clamp(menu->video_mono_color);
    if (menu->video_mono_color == APPLE_VIDEO_MONO_BLACK) {
        menu->video_mono_color = CONFIG_DEFAULT_VIDEO_MONO_COLOR;
    }
    menu->video_color_mode = apple_video_color_mode_clamp(menu->video_color_mode);
    if (config_menu_video_color_mode_allowed(menu, menu->video_color_mode) == 0U) {
        menu->video_color_mode = CONFIG_DEFAULT_VIDEO_COLOR_MODE;
    }
    menu->video7_auto_mono_enabled =
        (menu->video7_auto_mono_enabled != 0U) ? 1U : 0U;
    menu->clean_video_phase_cycles = CONFIG_DEFAULT_CLEAN_VIDEO_PHASE_CYCLES;
    menu->pal_video_phase_cycles = CONFIG_DEFAULT_PAL_VIDEO_PHASE_CYCLES;
}

static void config_menu_coerce_video_ghosting(config_menu_t *menu)
{
    if (menu == NULL) {
        return;
    }
    menu->video_ghosting_strength =
        appletini_video_ghosting_clamp(menu->video_ghosting_strength);
}

static void config_menu_coerce_border(config_menu_t *menu)
{
    if (menu == NULL) {
        return;
    }
    menu->border_enabled = (menu->border_enabled != 0u) ? 1u : 0u;
    menu->border_color = apple_video_iigs_border_color_clamp(menu->border_color);
    menu->border_flood = (menu->border_flood != 0u) ? 1u : 0u;
    if (menu->border_flood != 0u) {
        menu->show_bezel = 0U;
        menu->show_debugging = 0U;
    }
}

static void config_menu_apply_border(config_menu_t *menu)
{
    if (menu == NULL) {
        return;
    }
    config_menu_coerce_border(menu);
    if (menu->platform.set_border != NULL) {
        menu->platform.set_border(menu->platform.ctx,
                                  menu->border_enabled,
                                  menu->border_color,
                                  menu->border_flood);
    }
}

static void config_menu_apply_video_ghosting(config_menu_t *menu)
{
    if (menu == NULL) {
        return;
    }
    config_menu_coerce_video_ghosting(menu);
    if (menu->platform.set_video_ghosting != NULL) {
        menu->platform.set_video_ghosting(menu->platform.ctx,
                                          menu->video_ghosting_strength);
    }
}

static void config_menu_apply_video_output(config_menu_t *menu)
{
    if (menu == NULL) {
        return;
    }
    config_menu_coerce_video_output(menu);
    if (menu->platform.set_video_output != NULL) {
        menu->platform.set_video_output(menu->platform.ctx,
                                        menu->video_output_mono,
                                        menu->video_mono_color,
                                        menu->video_color_mode,
                                        menu->video7_auto_mono_enabled,
                                        menu->clean_video_phase_cycles,
                                        menu->pal_video_phase_cycles);
    }
}

static void config_menu_apply_bezel(config_menu_t *menu)
{
    int rc;
    char text[CONFIG_MENU_STATUS_LEN];

    if (menu == NULL || menu->platform.set_bezel_path == NULL) {
        return;
    }

    rc = menu->platform.set_bezel_path(menu->platform.ctx, menu->bezel_path);
    if (rc == 0) {
        (void)snprintf(text,
                       sizeof(text),
                       "BEZEL: %.84s",
                       config_menu_bezel_text(menu));
        config_menu_set_status(menu, 0U, text);
    } else {
        (void)snprintf(text,
                       sizeof(text),
                       "BEZEL LOAD FAILED RC=%d",
                       rc);
        config_menu_set_status(menu, 1U, text);
    }
}

/* Load the user-selected video character ROM from the SD card into the shared
 * CPU0->CPU1 override buffer and bump the handoff generation so CPU1's renderer
 * rebuilds csbits from it. Validates size (4096 or 8192; an 8 KB dual-charset
 * dump uses its primary 4 KB bank) and sanity (not all 00/FF). On empty path,
 * missing/unreadable file, or a bad dump, sets generation 0 -> CPU1 falls back
 * to the baked Enhanced US ROM. */
static uint8_t s_video_rom_scratch[8192];

static void config_menu_apply_video_rom(config_menu_t *menu)
{
    FIL file;
    FRESULT fr;
    FSIZE_t fsize;
    UINT got = 0U;
    uint8_t valid = 0U;
    char text[CONFIG_MENU_STATUS_LEN];

    if (menu == NULL) {
        return;
    }
    if (menu->video_rom_path[0] == '\0') {
        apple_fb_video_rom_gen_set(0U);   /* no override -> baked Enhanced US */
        return;
    }
    if (config_menu_mount_sd() != FR_OK ||
        f_open(&file, menu->video_rom_path, FA_READ) != FR_OK) {
        apple_fb_video_rom_gen_set(0U);
        config_menu_set_status(menu, 1U, "VIDEO ROM OPEN FAILED - USING BUILT-IN");
        return;
    }

    fsize = f_size(&file);
    if (fsize == 4096U || fsize == 8192U) {
        fr = f_read(&file, s_video_rom_scratch, (UINT)fsize, &got);
        if (fr == FR_OK && got == (UINT)fsize) {
            uint8_t all00 = 1U, allff = 1U;
            for (uint32_t i = 0U; i < APPLE_VIDEO_ROM_OVERRIDE_BYTES; ++i) {
                if (s_video_rom_scratch[i] != 0x00U) { all00 = 0U; }
                if (s_video_rom_scratch[i] != 0xFFU) { allff = 0U; }
            }
            valid = (uint8_t)((all00 == 0U) && (allff == 0U));
        }
    }
    (void)f_close(&file);

    if (valid != 0U) {
        memcpy((void *)(uintptr_t)APPLE_VIDEO_ROM_OVERRIDE_ADDR,
               s_video_rom_scratch, APPLE_VIDEO_ROM_OVERRIDE_BYTES);
        /* Push the buffer to DDR before CPU1 (which reads it non-cached) sees
         * the new generation, in case this region is cacheable on CPU0. */
        Xil_DCacheFlushRange((INTPTR)APPLE_VIDEO_ROM_OVERRIDE_ADDR,
                             (INTPTR)APPLE_VIDEO_ROM_OVERRIDE_BYTES);
        __asm__ volatile ("dmb sy" ::: "memory");
        uint32_t gen = apple_fb_video_rom_gen_get() + 1U;
        if (gen == 0U) { gen = 1U; }
        apple_fb_video_rom_gen_set(gen);
        (void)snprintf(text, sizeof(text), "VIDEO ROM: %.80s", config_menu_video_rom_text(menu));
        config_menu_set_status(menu, 0U, text);
    } else {
        apple_fb_video_rom_gen_set(0U);   /* bad dump -> baked Enhanced US */
        config_menu_set_status(menu, 1U, "VIDEO ROM INVALID - USING BUILT-IN");
    }
}

static void config_menu_load_platform_defaults(config_menu_t *menu)
{
    if (menu == NULL) {
        return;
    }

    if (menu->platform.get_scanlines != NULL) {
        menu->scanlines_mode =
            appletini_scanlines_clamp(menu->platform.get_scanlines(menu->platform.ctx));
    }
    if (menu->platform.get_video_ghosting != NULL) {
        menu->video_ghosting_strength = appletini_video_ghosting_clamp(
            menu->platform.get_video_ghosting(menu->platform.ctx));
    }
    if (menu->platform.get_border_enabled != NULL) {
        menu->border_enabled =
            (menu->platform.get_border_enabled(menu->platform.ctx) != 0u) ? 1u : 0u;
    }
    if (menu->platform.get_border_color != NULL) {
        menu->border_color = apple_video_iigs_border_color_clamp(
            menu->platform.get_border_color(menu->platform.ctx));
    }
    if (menu->platform.get_border_flood != NULL) {
        menu->border_flood =
            (menu->platform.get_border_flood(menu->platform.ctx) != 0u) ? 1u : 0u;
    }
    if (menu->platform.get_video_output_mono != NULL) {
        menu->video_output_mono =
            (menu->platform.get_video_output_mono(menu->platform.ctx) != 0U) ? 1U : 0U;
    }
    if (menu->platform.get_video_output_mono_color != NULL) {
        menu->video_mono_color = apple_video_mono_color_clamp(
            menu->platform.get_video_output_mono_color(menu->platform.ctx));
    }
    if (menu->platform.get_video_output_color_mode != NULL) {
        menu->video_color_mode = apple_video_color_mode_clamp(
            menu->platform.get_video_output_color_mode(menu->platform.ctx));
    }
    if (menu->platform.get_video7_auto_mono_enabled != NULL) {
        menu->video7_auto_mono_enabled =
            (menu->platform.get_video7_auto_mono_enabled(menu->platform.ctx) != 0U) ?
            1U : 0U;
    }
    if (menu->platform.get_clean_video_phase_cycles != NULL) {
        menu->clean_video_phase_cycles = apple_video_timing_phase_clamp(
            menu->platform.get_clean_video_phase_cycles(menu->platform.ctx));
    }
    if (menu->platform.get_pal_video_phase_cycles != NULL) {
        menu->pal_video_phase_cycles = apple_video_timing_phase_clamp(
            menu->platform.get_pal_video_phase_cycles(menu->platform.ctx));
    }
    if (menu->platform.get_slot_enabled != NULL) {
        menu->ethernet_slot1_enabled =
            menu->platform.get_slot_enabled(menu->platform.ctx, ETHERNET_CONTROL_SLOT);
        menu->disk2_slot6_enabled =
            menu->platform.get_slot_enabled(menu->platform.ctx, DISK2_CONTROL_SLOT);
        menu->mouse_slot2_enabled =
            menu->platform.get_slot_enabled(menu->platform.ctx, MOUSE_CONTROL_SLOT);
        menu->mockingboard_slot4_enabled =
            menu->platform.get_slot_enabled(menu->platform.ctx, MOCKINGBOARD_CONTROL_SLOT);
        menu->applicard_slot5_enabled =
            menu->platform.get_slot_enabled(menu->platform.ctx, APPLICARD_CONTROL_SLOT);
    }
}

static void config_menu_apply_boot_runtime_internal(config_menu_t *menu,
                                                    uint8_t apply_boot_timeout)
{
    if (menu == NULL) {
        return;
    }

    config_menu_coerce_boot_device(menu);
    if (menu->platform.set_slot_enabled != NULL) {
        menu->platform.set_slot_enabled(menu->platform.ctx,
                                        DISK2_CONTROL_SLOT,
                                        menu->disk2_slot6_enabled);
    }
    if (menu->platform.set_boot_handoff != NULL) {
        uint8_t handoff = CONFIG_BOOT_HANDOFF_SMARTPORT;

        if (menu->boot_device == CONFIG_BOOT_DEVICE_DISK2 &&
            menu->disk2_slot6_enabled != 0U) {
            handoff = CONFIG_BOOT_HANDOFF_DISK2;
        }
        menu->platform.set_boot_handoff(menu->platform.ctx, handoff);
    }
    if (apply_boot_timeout != 0U &&
        menu->platform.set_boot_timeout_ticks != NULL) {
        menu->platform.set_boot_timeout_ticks(menu->platform.ctx,
                                             config_menu_boot_timeout_ticks(menu->boot_timeout_mode));
    }
}

static void config_menu_apply_smartport_paths(config_menu_t *menu)
{
    if (menu == NULL || menu->platform.set_smartport_image_path == NULL) {
        return;
    }
    for (uint8_t device = 1U; device <= SMARTPORT_DEVICE_COUNT; ++device) {
        (void)menu->platform.set_smartport_image_path(menu->platform.ctx,
                                                      device,
                                                      "");
    }
    for (uint8_t device = 1U; device <= SMARTPORT_DEVICE_COUNT; ++device) {
        const uint8_t index = (uint8_t)(device - 1U);
        const char *path;
        if (menu->smartport_slots[index] != 0U &&
            config_menu_smartport_path_used_before(
                menu,
                device,
                menu->smartport_disk_paths[index]) != 0U) {
            menu->smartport_slots[index] = 0U;
            menu->smartport_disk_paths[index][0] = '\0';
        }
        path = (menu->smartport_slots[index] != 0U) ?
            menu->smartport_disk_paths[index] : "";
        (void)menu->platform.set_smartport_image_path(menu->platform.ctx,
                                                      device,
                                                      path);
    }
}

void config_menu_refresh_smartport_media_after_menu_sd(config_menu_t *menu)
{
    if (menu == NULL ||
        menu->active == 0U ||
        menu->platform.reset_smartport_media == NULL) {
        return;
    }

    /*
     * FatFs has one mounted object per volume. The config browser/save paths
     * mount their own FATFS, which invalidates SmartPort's long-lived FIL.
     * Refresh immediately while the Apple is still in the boot menu; the ROM
     * handoff path does not wait for the PS close event before jumping to Cn00.
     */
    (void)menu->platform.reset_smartport_media(menu->platform.ctx,
                                               CONFIG_SMARTPORT_ALL_DEVICES);
}

static void config_menu_apply_disk2_paths(config_menu_t *menu)
{
    if (menu == NULL || menu->platform.set_disk2_image_path == NULL) {
        return;
    }
    (void)menu->platform.set_disk2_image_path(menu->platform.ctx,
                                              0U,
                                              menu->disk2_disk_paths[0]);
    (void)menu->platform.set_disk2_image_path(menu->platform.ctx,
                                              1U,
                                              menu->disk2_disk_paths[1]);
}

static void config_menu_apply_disk2_sound(config_menu_t *menu)
{
    if (menu == NULL || menu->platform.set_disk2_sound_volume == NULL) {
        return;
    }
    menu->platform.set_disk2_sound_volume(
        menu->platform.ctx,
        config_menu_disk2_sound_volume_clamp(menu->disk2_sound_volume));
}

static uint8_t config_menu_ethernet_card_access(config_menu_t *menu)
{
    if (menu == NULL) {
        return 0U;
    }
    if (menu->usb_owned != 0U) {
        config_menu_set_status(menu, 1U, "CARD ACCESS ONLY FROM BOOT MENU");
        return 0U;
    }
    return 1U;
}

static void config_menu_ethernet_set_status_ip(
    config_menu_t *menu,
    const char *prefix,
    uint8_t warning,
    const uthernet2_network_config_t *config);

static uint8_t config_menu_acquire_ethernet_dhcp(config_menu_t *menu,
                                                 uint8_t report)
{
    uthernet2_network_config_t lease;
    char detail[CONFIG_MENU_STATUS_LEN];

    if (config_menu_ethernet_card_access(menu) == 0U) {
        return 0U;
    }
    if (menu->platform.ethernet_dhcp_acquire == NULL) {
        if (report != 0U) {
            config_menu_set_status(menu, 1U, "UTHERNET II DHCP UNAVAILABLE");
        }
        return 0U;
    }
    if (menu->platform.ethernet_write_config == NULL ||
        menu->platform.ethernet_write_config(menu->platform.ctx,
                                             &menu->ethernet_config) != 0) {
        if (report != 0U) {
            config_menu_set_status(menu, 1U, "UTHERNET II CONFIG WRITE FAILED");
        }
        return 0U;
    }

    detail[0] = '\0';
    if (report != 0U) {
        config_menu_set_status(menu, 1U, "DHCP REQUEST IN PROGRESS");
    }
    if (menu->platform.ethernet_dhcp_acquire(menu->platform.ctx,
                                             menu->ethernet_config.mac,
                                             &lease,
                                             detail,
                                             sizeof(detail)) != 0) {
        if (report != 0U) {
            config_menu_set_status(menu,
                                   1U,
                                   (detail[0] != '\0') ? detail : "DHCP FAILED");
        }
        return 0U;
    }

    menu->ethernet_config = lease;
    menu->ethernet_address_mode = CONFIG_MENU_ETHERNET_ADDRESS_DHCP;
    menu->ethernet_config_enabled = 1U;
    config_menu_save_settings(menu);
    if (report != 0U && menu->session_only == 0U) {
        config_menu_ethernet_set_status_ip(menu,
                                           "DHCP LEASE IP",
                                           0U,
                                           &menu->ethernet_config);
    }
    return 1U;
}

static uint8_t config_menu_apply_ethernet_config(config_menu_t *menu,
                                                 uint8_t report)
{
    if (menu == NULL || menu->ethernet_config_enabled == 0U) {
        return 1U;
    }
    if (menu->ethernet_address_mode == CONFIG_MENU_ETHERNET_ADDRESS_DHCP) {
        return config_menu_acquire_ethernet_dhcp(menu, report);
    }
    if (config_menu_ethernet_card_access(menu) == 0U) {
        return 0U;
    }
    if (menu->platform.ethernet_write_config == NULL) {
        if (report != 0U) {
            config_menu_set_status(menu, 1U, "UTHERNET II CONFIG WRITE UNAVAILABLE");
        }
        return 0U;
    }
    if (menu->platform.ethernet_write_config(menu->platform.ctx,
                                             &menu->ethernet_config) != 0) {
        if (report != 0U) {
            config_menu_set_status(menu, 1U, "UTHERNET II CONFIG WRITE FAILED");
        }
        return 0U;
    }
    if (report != 0U) {
        config_menu_set_status(menu, 0U, "APPLIED UTHERNET II NETWORK CONFIG");
    }
    return 1U;
}

static void config_menu_apply_runtime_internal(config_menu_t *menu,
                                               uint8_t apply_boot_timeout)
{
    if (menu == NULL) {
        return;
    }

    config_menu_coerce_video_output(menu);

    if (menu->platform.set_scanlines != NULL) {
        menu->platform.set_scanlines(menu->platform.ctx, menu->scanlines_mode);
    }
    config_menu_apply_video_ghosting(menu);
    config_menu_apply_video_output(menu);
    config_menu_apply_border(menu);
    if (menu->platform.set_slot_enabled != NULL) {
        menu->platform.set_slot_enabled(menu->platform.ctx,
                                        ETHERNET_CONTROL_SLOT,
                                        menu->ethernet_slot1_enabled);
        menu->platform.set_slot_enabled(menu->platform.ctx,
                                        MOUSE_CONTROL_SLOT,
                                        menu->mouse_slot2_enabled);
        menu->platform.set_slot_enabled(menu->platform.ctx,
                                        MOCKINGBOARD_CONTROL_SLOT,
                                        menu->mockingboard_slot4_enabled);
        menu->platform.set_slot_enabled(menu->platform.ctx,
                                        APPLICARD_CONTROL_SLOT,
                                        menu->applicard_slot5_enabled);
    }
    if (menu->platform.set_applicard_resource_max != NULL) {
        menu->platform.set_applicard_resource_max(menu->platform.ctx,
                                                  menu->applicard_resource_max);
    }
    if (menu->platform.set_supersprite_enabled != NULL) {
        menu->platform.set_supersprite_enabled(menu->platform.ctx,
                                               menu->supersprite_enabled);
    }
    if (menu->platform.set_sdd_stream_enabled != NULL) {
        menu->platform.set_sdd_stream_enabled(menu->platform.ctx,
                                              menu->sdd_stream_enabled);
    }
    config_menu_apply_boot_runtime_internal(menu, apply_boot_timeout);
    if (menu->platform.set_clock_enabled != NULL) {
        menu->platform.set_clock_enabled(menu->platform.ctx, menu->clock_enabled);
    }
    if (menu->platform.set_mouse_sensitivity != NULL) {
        menu->platform.set_mouse_sensitivity(menu->platform.ctx, menu->mouse_sensitivity);
    }
    config_menu_phasor_apply(menu);
    config_menu_apply_disk2_sound(menu);
    config_menu_apply_smartport_paths(menu);
    config_menu_apply_disk2_paths(menu);
}

void config_menu_apply_boot_runtime(config_menu_t *menu)
{
    config_menu_apply_boot_runtime_internal(menu, 1U);
}

void config_menu_apply_runtime(config_menu_t *menu)
{
    config_menu_apply_runtime_internal(menu, 1U);
}

static uint8_t config_menu_parse_indexed_config_key(const char *key,
                                                    const char *prefix,
                                                    uint32_t min_index,
                                                    uint32_t max_index,
                                                    uint32_t *out_index,
                                                    const char **out_suffix)
{
    const size_t prefix_len = strlen(prefix);
    const char *cursor;
    char *end = NULL;
    unsigned long index;

    if (key == NULL || prefix == NULL ||
        strncmp(key, prefix, prefix_len) != 0) {
        return 0U;
    }

    cursor = key + prefix_len;
    if (*cursor < '0' || *cursor > '9') {
        return 0U;
    }

    index = strtoul(cursor, &end, 10);
    if (end == NULL || end == cursor ||
        index < (unsigned long)min_index ||
        index > (unsigned long)max_index) {
        return 0U;
    }

    if (out_index != NULL) {
        *out_index = (uint32_t)index;
    }
    if (out_suffix != NULL) {
        *out_suffix = end;
    }
    return 1U;
}

static uint8_t config_menu_config_path_is_firmware_default(const char *value)
{
    return (uint8_t)(value == NULL ||
                     value[0] == '\0' ||
                     config_menu_str_ieq(value, "firmware") != 0U);
}

static void config_menu_parse_key_value(config_menu_t *menu, const char *key, const char *value)
{
    uint32_t i;
    const char *suffix;

    if (menu == NULL || key == NULL || value == NULL) {
        return;
    }

    if (strcmp(key, "appletini.config.version") == 0) {
        return;
    } else if (strcmp(key, "boot.menu.seconds") == 0) {
        if (config_menu_str_ieq(value, "always") != 0U) {
            menu->boot_timeout_mode = CONFIG_BOOT_TIMEOUT_ALWAYS;
        } else if (strtoul(value, NULL, 10) == 5UL) {
            menu->boot_timeout_mode = CONFIG_BOOT_TIMEOUT_5S;
        } else {
            menu->boot_timeout_mode = CONFIG_BOOT_TIMEOUT_3S;
        }
    } else if (strcmp(key, "boot.device") == 0) {
        if (config_menu_str_ieq(value, "disk2") != 0U) {
            menu->boot_device = CONFIG_BOOT_DEVICE_DISK2;
        } else {
            menu->boot_device = CONFIG_BOOT_DEVICE_SMARTPORT;
        }
    } else if (strcmp(key, "video.scanlines") == 0) {
        menu->scanlines_mode = config_menu_scanlines_text(value);
    } else if (strcmp(key, "video.output") == 0) {
        menu->video_output_mono = config_menu_video_output_text_value(value);
    } else if (strcmp(key, "video.mono.color") == 0) {
        menu->video_mono_color = config_menu_video_mono_color_value(value);
    } else if (strcmp(key, "video.color.mode") == 0) {
        menu->video_color_mode = config_menu_video_color_mode_value(value);
    } else if (strcmp(key, "video.video7.monochrome") == 0) {
        menu->video7_auto_mono_enabled = config_menu_bool_text(value);
    } else if (strcmp(key, "video.ghosting") == 0) {
        menu->video_ghosting_strength = config_menu_video_ghosting_text(value);
    } else if (strcmp(key, "video.border.enabled") == 0) {
        menu->border_enabled = config_menu_bool_text(value);
    } else if (strcmp(key, "video.border.color") == 0) {
        menu->border_color = apple_video_iigs_border_color_clamp(
            (uint8_t)strtoul(value, NULL, 0));
    } else if (strcmp(key, "video.border.outside") == 0) {
        menu->border_flood =
            (config_menu_str_ieq(value, "flood") != 0u) ? 1u : 0u;
    } else if (strcmp(key, "video.debug.overlay") == 0) {
        menu->show_debugging = config_menu_bool_text(value);
    } else if (strcmp(key, "video.bezel.visible") == 0) {
        menu->show_bezel = config_menu_bool_text(value);
    } else if (strcmp(key, "video.bezel.path") == 0) {
        if (config_menu_config_path_is_firmware_default(value) != 0U) {
            menu->bezel_path[0] = '\0';
        } else {
            config_menu_copy_text(menu->bezel_path,
                                  sizeof(menu->bezel_path),
                                  value);
        }
    } else if (strcmp(key, "video.rom") == 0) {
        if (config_menu_config_path_is_firmware_default(value) != 0U) {
            menu->video_rom_path[0] = '\0';
        } else {
            config_menu_copy_text(menu->video_rom_path,
                                  sizeof(menu->video_rom_path),
                                  value);
        }
    } else if (config_menu_parse_indexed_config_key(
                   key, "smartport.disk.", 1U, SMARTPORT_DEVICE_COUNT,
                   &i, &suffix) != 0U) {
        if (strcmp(suffix, ".path") == 0) {
            config_menu_copy_text(menu->smartport_disk_paths[i - 1U],
                                  sizeof(menu->smartport_disk_paths[i - 1U]),
                                  value);
        } else if (strcmp(suffix, ".enabled") == 0) {
            menu->smartport_slots[i - 1U] = config_menu_bool_text(value);
        }
    } else if (strcmp(key, "applicard.slot5.enabled") == 0) {
        menu->applicard_slot5_enabled = config_menu_bool_text(value);
    } else if (strcmp(key, "applicard.resource.max") == 0) {
        menu->applicard_resource_max = config_menu_bool_text(value);
    } else if (strcmp(key, "disk2.slot6.enabled") == 0) {
        menu->disk2_slot6_enabled = config_menu_bool_text(value);
    } else if (strcmp(key, "disk2.activity.visible") == 0) {
        menu->disk2_activity_visible = config_menu_bool_text(value);
    } else if (strcmp(key, "disk2.sound.volume") == 0) {
        menu->disk2_sound_volume =
            config_menu_disk2_sound_volume_clamp(strtoul(value, NULL, 10));
    } else if (config_menu_parse_indexed_config_key(
                   key, "disk2.drive.", 1U, 2U, &i, &suffix) != 0U) {
        if (strcmp(suffix, ".path") == 0) {
            config_menu_copy_text(menu->disk2_disk_paths[i - 1U],
                                  sizeof(menu->disk2_disk_paths[i - 1U]),
                                  value);
        } else if (strncmp(suffix, ".slot.", 6U) == 0) {
            char *end = NULL;
            unsigned long slot = strtoul(suffix + 6U, &end, 10);
            if (end != NULL &&
                strcmp(end, ".enabled") == 0 &&
                slot >= 1UL && slot <= 4UL) {
                menu->disk2_slots[i - 1U][slot - 1UL] =
                    config_menu_bool_text(value);
            }
        }
    } else if (config_menu_parse_usb_binding(menu, key, value) != 0U) {
        /* Handled by USB menu binding settings. */
    } else if (config_menu_phasor_parse_setting(menu, key, value) != 0U) {
        /* Handled by the Phasor tab module. */
    } else if (strcmp(key, "mouse.slot2.enabled") == 0) {
        menu->mouse_slot2_enabled = config_menu_bool_text(value);
    } else if (strcmp(key, "mouse.sensitivity") == 0) {
        menu->mouse_sensitivity =
            config_menu_mouse_sensitivity_clamp(strtoul(value, NULL, 10));
    } else if (strcmp(key, "smartport.supersprite.enabled") == 0) {
        menu->supersprite_enabled = config_menu_bool_text(value);
    } else if (strcmp(key, "usb.sdd.stream.enabled") == 0) {
        menu->sdd_stream_enabled = config_menu_bool_text(value);
    } else if (strcmp(key, "ethernet.slot1.enabled") == 0) {
        menu->ethernet_slot1_enabled = config_menu_bool_text(value);
    } else if (strcmp(key, "ethernet.config.enabled") == 0) {
        menu->ethernet_config_enabled = config_menu_bool_text(value);
    } else if (strcmp(key, "ethernet.address.mode") == 0) {
        menu->ethernet_address_mode = config_menu_ethernet_address_mode_value(value);
    } else if (strcmp(key, "ethernet.mac") == 0) {
        (void)config_menu_parse_mac(value, menu->ethernet_config.mac);
    } else if (strcmp(key, "ethernet.ip") == 0) {
        (void)config_menu_parse_ipv4(value, menu->ethernet_config.ip);
    } else if (strcmp(key, "ethernet.subnet") == 0) {
        (void)config_menu_parse_ipv4(value, menu->ethernet_config.subnet);
    } else if (strcmp(key, "ethernet.gateway") == 0) {
        (void)config_menu_parse_ipv4(value, menu->ethernet_config.gateway);
    } else if (strcmp(key, "clock.enabled") == 0) {
        menu->clock_enabled = config_menu_bool_text(value);
    } else if (strcmp(key, "ram.enabled") == 0) {
        menu->ram_enabled = config_menu_bool_text(value);
        menu->ramworks_enabled = menu->ram_enabled;
    } else if (strcmp(key, "smartport.ram32.enabled") == 0) {
        menu->sp_ramdisk_enabled = config_menu_bool_text(value);
    }
}

uint8_t config_menu_save_settings_to_path(config_menu_t *menu,
                                          const char *path,
                                          const char *success_status)
{
    FIL file;
    FRESULT fr;
    UINT written = 0U;
    char buffer[APPLETINI_CFG_MAX];
    int len = 0;
    uint8_t ok = 1U;
    char ethernet_mac[18];
    char ethernet_ip[16];
    char ethernet_subnet[16];
    char ethernet_gateway[16];

    if (menu == NULL || path == NULL || path[0] == '\0') {
        return 0U;
    }

    config_menu_format_mac(menu->ethernet_config.mac,
                           ethernet_mac,
                           sizeof(ethernet_mac));
    config_menu_format_ipv4(menu->ethernet_config.ip,
                            ethernet_ip,
                            sizeof(ethernet_ip));
    config_menu_format_ipv4(menu->ethernet_config.subnet,
                            ethernet_subnet,
                            sizeof(ethernet_subnet));
    config_menu_format_ipv4(menu->ethernet_config.gateway,
                            ethernet_gateway,
                            sizeof(ethernet_gateway));

#define APPEND_CFG(...) do { \
        if (ok != 0U) { \
            ok = config_menu_appendf(buffer, sizeof(buffer), &len, __VA_ARGS__); \
        } \
    } while (0)

    APPEND_CFG("appletini.config.version=%u\n"
               "boot.menu.seconds=%s\n"
               "boot.device=%s\n"
               "video.scanlines=%s\n"
               "video.output=%s\n"
               "video.mono.color=%s\n"
               "video.color.mode=%s\n"
               "video.video7.monochrome=%s\n"
               "video.ghosting=%s\n"
               "video.border.enabled=%s\n"
               "video.border.color=%u\n"
               "video.border.outside=%s\n"
               "video.debug.overlay=%s\n"
               "video.bezel.visible=%s\n"
               "video.bezel.path=%s\n",
               (unsigned)APPLETINI_CFG_VERSION,
               (menu->boot_timeout_mode == CONFIG_BOOT_TIMEOUT_ALWAYS) ? "ALWAYS" :
               ((menu->boot_timeout_mode == CONFIG_BOOT_TIMEOUT_5S) ? "5" : "3"),
               (menu->boot_device == CONFIG_BOOT_DEVICE_DISK2) ? "DISK2" : "SMARTPORT",
               config_menu_scanlines_config(menu->scanlines_mode),
               (menu->video_output_mono != 0U) ? "MONOCHROME" : "COLOR",
               config_menu_video_mono_color_config(menu->video_mono_color),
               config_menu_video_color_mode_config(menu->video_color_mode),
               config_menu_on_off(menu->video7_auto_mono_enabled),
               config_menu_video_ghosting_config(menu->video_ghosting_strength),
               config_menu_on_off(menu->border_enabled),
               (unsigned)apple_video_iigs_border_color_clamp(menu->border_color),
               (menu->border_flood != 0u) ? "FLOOD" : "BEZEL",
               config_menu_on_off(menu->show_debugging),
               config_menu_on_off(menu->show_bezel),
               (menu->bezel_path[0] != '\0') ? menu->bezel_path : "FIRMWARE");

    APPEND_CFG("video.rom=%s\n",
               (menu->video_rom_path[0] != '\0') ?
               menu->video_rom_path : "FIRMWARE");

    for (uint32_t device = 0U; device < SMARTPORT_DEVICE_COUNT; ++device) {
        APPEND_CFG("smartport.disk.%u.path=%s\n",
                   (unsigned)(device + 1U),
                   menu->smartport_disk_paths[device]);
    }
    for (uint32_t device = 0U; device < SMARTPORT_DEVICE_COUNT; ++device) {
        APPEND_CFG("smartport.disk.%u.enabled=%s\n",
                   (unsigned)(device + 1U),
                   config_menu_on_off(menu->smartport_slots[device]));
    }

    APPEND_CFG("disk2.slot6.enabled=%s\n"
               "disk2.activity.visible=%s\n"
               "disk2.sound.volume=%u\n"
               "disk2.drive.1.path=%s\n"
               "disk2.drive.2.path=%s\n",
               config_menu_on_off(menu->disk2_slot6_enabled),
               config_menu_on_off(menu->disk2_activity_visible),
               (unsigned)config_menu_disk2_sound_volume_clamp(menu->disk2_sound_volume),
               menu->disk2_disk_paths[0],
               menu->disk2_disk_paths[1]);

    for (uint32_t drive = 0U; drive < 2U; ++drive) {
        for (uint32_t slot = 0U; slot < 4U; ++slot) {
            APPEND_CFG("disk2.drive.%u.slot.%u.enabled=%s\n",
                       (unsigned)(drive + 1U),
                       (unsigned)(slot + 1U),
                       config_menu_on_off(menu->disk2_slots[drive][slot]));
        }
    }

    APPEND_CFG("mouse.slot2.enabled=%s\n"
               "mouse.sensitivity=%u\n"
               "applicard.slot5.enabled=%s\n"
               "applicard.resource.max=%s\n",
               config_menu_on_off(menu->mouse_slot2_enabled),
               (unsigned)menu->mouse_sensitivity,
               config_menu_on_off(menu->applicard_slot5_enabled),
               config_menu_on_off(menu->applicard_resource_max));

    for (uint32_t binding = 0U;
         binding < CONFIG_MENU_USB_BIND_ACTION_COUNT;
         ++binding) {
        APPEND_CFG("%s=%s\n",
                   k_usb_binding_config_keys[binding],
                   config_menu_usb_binding_source_config(
                       config_menu_usb_binding_source_clamp_for_action(
                       binding,
                       menu->usb_menu_bindings[binding])));
    }

    if (ok != 0U) {
        ok = config_menu_phasor_append_settings(menu, buffer, sizeof(buffer), &len);
    }

    APPEND_CFG("smartport.ram32.enabled=%s\n"
               "smartport.supersprite.enabled=%s\n"
               "usb.sdd.stream.enabled=%s\n"
               "ethernet.slot1.enabled=%s\n"
               "ethernet.config.enabled=%s\n"
               "ethernet.address.mode=%s\n"
               "ethernet.mac=%s\n"
               "ethernet.ip=%s\n"
               "ethernet.subnet=%s\n"
               "ethernet.gateway=%s\n"
               "clock.enabled=%s\n"
               "ram.enabled=%s\n",
               config_menu_on_off(menu->sp_ramdisk_enabled),
               config_menu_on_off(menu->supersprite_enabled),
               config_menu_on_off(menu->sdd_stream_enabled),
               config_menu_on_off(menu->ethernet_slot1_enabled),
               config_menu_on_off(menu->ethernet_config_enabled),
               config_menu_ethernet_address_mode_text(menu->ethernet_address_mode),
               ethernet_mac,
               ethernet_ip,
               ethernet_subnet,
               ethernet_gateway,
               config_menu_on_off(menu->clock_enabled),
               config_menu_on_off(menu->ram_enabled));

#undef APPEND_CFG

    if (ok == 0U || len <= 0) {
        menu->session_only = 1U;
        config_menu_set_status(menu, 1U, "CONFIG BUFFER FULL: SETTINGS ACTIVE FOR THIS SESSION");
        return 0U;
    }

    fr = config_menu_open_path(&file, path, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) {
        menu->session_only = 1U;
        config_menu_set_sd_error(menu, "CONFIG WRITE OPEN FAILED", fr);
        return 0U;
    }

    fr = f_write(&file, buffer, (UINT)len, &written);
    (void)f_close(&file);
    if (fr != FR_OK || written != (UINT)len) {
        menu->session_only = 1U;
        config_menu_set_sd_error(menu, "CONFIG WRITE FAILED", fr);
        return 0U;
    }

    menu->settings_loaded = 1U;
    menu->session_only = 0U;
    if (success_status != NULL) {
        config_menu_set_status(menu, 0U, success_status);
    }
    config_menu_refresh_smartport_media_after_menu_sd(menu);
    return 1U;
}

void config_menu_save_settings(config_menu_t *menu)
{
    (void)config_menu_save_settings_to_path(menu,
                                            APPLETINI_CFG_PATH,
                                            "WROTE 0:/appletini_cfg.txt");
}

static void config_menu_load_settings(config_menu_t *menu)
{
    FIL file;
    FRESULT fr;
    UINT bytes_read = 0U;
    char buffer[APPLETINI_CFG_MAX];
    char *line;
    uint32_t cfg_version = 0U;

    if (menu == NULL) {
        return;
    }

    fr = config_menu_open_cfg(&file, FA_READ);
    if (fr == FR_NO_FILE) {
        config_menu_save_settings(menu);
        if (menu->session_only == 0U) {
            menu->settings_loaded = 1U;
            config_menu_set_status(menu, 0U, "CREATED 0:/appletini_cfg.txt");
        }
        return;
    }
    if (fr != FR_OK) {
        menu->settings_loaded = 0U;
        menu->session_only = 1U;
        config_menu_set_sd_error(menu, "CONFIG READ OPEN FAILED", fr);
        return;
    }

    fr = f_read(&file, buffer, sizeof(buffer) - 1U, &bytes_read);
    (void)f_close(&file);
    if (fr != FR_OK) {
        menu->settings_loaded = 0U;
        menu->session_only = 1U;
        config_menu_set_sd_error(menu, "CONFIG READ FAILED", fr);
        return;
    }

    buffer[bytes_read] = '\0';
    line = strtok(buffer, "\r\n");
    while (line != NULL) {
        char *value = NULL;
        char *key = config_menu_parse_config_line(line, &value);
        if (key != NULL) {
            if (strcmp(key, "appletini.config.version") == 0) {
                cfg_version = (uint32_t)strtoul(value, NULL, 10);
            }
            config_menu_parse_key_value(menu, key, value);
        }
        line = strtok(NULL, "\r\n");
    }

    config_menu_coerce_boot_device(menu);
    config_menu_coerce_video_output(menu);
    config_menu_coerce_video_ghosting(menu);
    config_menu_coerce_border(menu);
    config_menu_coerce_ethernet(menu);
    config_menu_usb_bindings_coerce(menu);
    menu->settings_loaded = 1U;
    menu->session_only = 0U;
    if (cfg_version < APPLETINI_CFG_VERSION) {
        config_menu_save_settings(menu);
        if (menu->session_only == 0U) {
            config_menu_set_status(menu, 0U, "UPDATED 0:/appletini_cfg.txt");
        }
    } else {
        config_menu_set_status(menu, 0U, "LOADED 0:/appletini_cfg.txt");
    }
}

/* Retry an unloaded configuration when the menu opens or an SD card is
 * inserted. Either event may make 0:/appletini_cfg.txt readable. */
void config_menu_retry_settings_if_needed(config_menu_t *menu)
{
    if (menu == NULL || menu->settings_loaded != 0U) {
        return;
    }

    config_menu_load_settings(menu);
    config_menu_apply_runtime(menu);
    if (menu->settings_loaded != 0U) {
        (void)config_menu_apply_ethernet_config(menu, 0U);
    }
    config_menu_apply_bezel(menu);
    config_menu_apply_video_rom(menu);
}

static uint8_t config_menu_days_in_month(uint16_t year, uint8_t month)
{
    static const uint8_t days[12] = {
        31U, 28U, 31U, 30U, 31U, 30U,
        31U, 31U, 30U, 31U, 30U, 31U
    };
    const uint8_t leap = (uint8_t)(((year % 4U) == 0U &&
                                    (((year % 100U) != 0U) || ((year % 400U) == 0U))) ? 1U : 0U);

    if (month < 1U || month > 12U) {
        return 31U;
    }
    if (month == 2U && leap != 0U) {
        return 29U;
    }
    return days[month - 1U];
}

static void config_menu_clock_default(config_menu_t *menu)
{
    if (menu == NULL || menu->clock_time.valid != 0U) {
        return;
    }

    menu->clock_time.year = 2026U;
    menu->clock_time.month = 1U;
    menu->clock_time.day = 1U;
    menu->clock_time.hour = 0U;
    menu->clock_time.min = 0U;
    menu->clock_time.sec = 0U;
    menu->clock_time.wday = rtc_pcf8563_weekday_from_ymd(menu->clock_time.year,
                                                         menu->clock_time.month,
                                                         menu->clock_time.day);
    menu->clock_time.valid = 1U;
    menu->clock_time.status = 0U;
}

static void config_menu_clock_clamp(config_menu_t *menu)
{
    uint8_t max_day;

    if (menu == NULL) {
        return;
    }

    config_menu_clock_default(menu);
    if (menu->clock_time.year < 2000U || menu->clock_time.year > 2099U) {
        menu->clock_time.year = 2026U;
    }
    if (menu->clock_time.month < 1U || menu->clock_time.month > 12U) {
        menu->clock_time.month = 1U;
    }
    max_day = config_menu_days_in_month(menu->clock_time.year, menu->clock_time.month);
    if (menu->clock_time.day < 1U || menu->clock_time.day > max_day) {
        menu->clock_time.day = max_day;
    }
    if (menu->clock_time.hour > 23U) {
        menu->clock_time.hour = 0U;
    }
    if (menu->clock_time.min > 59U) {
        menu->clock_time.min = 0U;
    }
    if (menu->clock_time.sec > 59U) {
        menu->clock_time.sec = 0U;
    }
    menu->clock_time.wday = rtc_pcf8563_weekday_from_ymd(menu->clock_time.year,
                                                         menu->clock_time.month,
                                                         menu->clock_time.day);
    menu->clock_time.valid = 1U;
    menu->clock_time.status = 0U;
}

static uint16_t config_menu_adjust_u16_wrap(uint16_t value,
                                            uint16_t min_value,
                                            uint16_t max_value,
                                            int8_t delta)
{
    if (delta < 0) {
        return (value <= min_value) ? max_value : (uint16_t)(value - 1U);
    }
    return (value >= max_value) ? min_value : (uint16_t)(value + 1U);
}

static uint8_t config_menu_adjust_u8_wrap(uint8_t value,
                                          uint8_t min_value,
                                          uint8_t max_value,
                                          int8_t delta)
{
    if (delta < 0) {
        return (value <= min_value) ? max_value : (uint8_t)(value - 1U);
    }
    return (value >= max_value) ? min_value : (uint8_t)(value + 1U);
}

static uint8_t config_menu_adjust_clock_field(config_menu_t *menu, int8_t delta)
{
    uint8_t max_day;

    if (menu == NULL || menu->tab != CONFIG_TAB_CLOCK ||
        menu->item_focus < 2U || menu->item_focus > 7U) {
        return 0U;
    }

    config_menu_clock_clamp(menu);
    switch (menu->item_focus) {
    case 2U:
        menu->clock_time.year =
            config_menu_adjust_u16_wrap(menu->clock_time.year,
                                        2000U,
                                        2099U,
                                        delta);
        break;
    case 3U:
        menu->clock_time.month =
            config_menu_adjust_u8_wrap(menu->clock_time.month, 1U, 12U, delta);
        break;
    case 4U:
        max_day = config_menu_days_in_month(menu->clock_time.year,
                                            menu->clock_time.month);
        menu->clock_time.day =
            config_menu_adjust_u8_wrap(menu->clock_time.day, 1U, max_day, delta);
        break;
    case 5U:
        menu->clock_time.hour =
            config_menu_adjust_u8_wrap(menu->clock_time.hour, 0U, 23U, delta);
        break;
    case 6U:
        menu->clock_time.min =
            config_menu_adjust_u8_wrap(menu->clock_time.min, 0U, 59U, delta);
        break;
    case 7U:
        menu->clock_time.sec =
            config_menu_adjust_u8_wrap(menu->clock_time.sec, 0U, 59U, delta);
        break;
    default:
        return 0U;
    }

    config_menu_clock_clamp(menu);
    return 1U;
}

static void config_menu_reset_settings_only(config_menu_t *menu)
{
    const uint8_t usb_bindings_editable =
        (menu != NULL) ? menu->usb_bindings_editable : 1U;

    if (menu == NULL) {
        return;
    }

    menu->boot_timeout_mode = CONFIG_DEFAULT_BOOT_TIMEOUT_MODE;
    menu->boot_device = CONFIG_DEFAULT_BOOT_DEVICE;
    menu->scanlines_mode = CONFIG_DEFAULT_SCANLINES_MODE;
    menu->video_output_mono = CONFIG_DEFAULT_VIDEO_OUTPUT_MONO;
    menu->video_mono_color = CONFIG_DEFAULT_VIDEO_MONO_COLOR;
    menu->video_color_mode = CONFIG_DEFAULT_VIDEO_COLOR_MODE;
    menu->video7_auto_mono_enabled = CONFIG_DEFAULT_VIDEO7_AUTO_MONO_ENABLED;
    menu->video_ghosting_strength = CONFIG_DEFAULT_VIDEO_GHOSTING_STRENGTH;
    menu->border_enabled = CONFIG_DEFAULT_BORDER_ENABLED;
    menu->border_color = CONFIG_DEFAULT_BORDER_COLOR;
    menu->border_flood = CONFIG_DEFAULT_BORDER_FLOOD;
    menu->clean_video_phase_cycles = CONFIG_DEFAULT_CLEAN_VIDEO_PHASE_CYCLES;
    menu->pal_video_phase_cycles = CONFIG_DEFAULT_PAL_VIDEO_PHASE_CYCLES;
    menu->video_rom_path[0] = '\0';
    menu->show_debugging = CONFIG_DEFAULT_SHOW_DEBUGGING;
    menu->show_bezel = CONFIG_DEFAULT_SHOW_BEZEL;
    menu->bezel_path[0] = '\0';

    memset(menu->smartport_slots, 0, sizeof(menu->smartport_slots));
    memset(menu->smartport_disk_paths, 0, sizeof(menu->smartport_disk_paths));
    for (uint8_t device = 1U; device <= SMARTPORT_DEVICE_COUNT; ++device) {
        config_menu_default_smartport_path(device,
                                           menu->smartport_disk_paths[device - 1U],
                                           sizeof(menu->smartport_disk_paths[device - 1U]));
    }
    menu->smartport_slots[0] = CONFIG_DEFAULT_SMARTPORT_DISK1_ENABLED;

    menu->disk2_slot6_enabled = CONFIG_DEFAULT_DISK2_SLOT6_ENABLED;
    menu->applicard_slot5_enabled = CONFIG_DEFAULT_APPLICARD_SLOT5_ENABLED;
    menu->applicard_resource_max = 0U;
    menu->disk2_activity_visible = CONFIG_DEFAULT_DISK2_ACTIVITY_VISIBLE;
    menu->disk2_sound_volume = CONFIG_DEFAULT_DISK2_SOUND_VOLUME;
    memset(menu->disk2_slots, 0, sizeof(menu->disk2_slots));
    memset(menu->disk2_disk_paths, 0, sizeof(menu->disk2_disk_paths));

    menu->mouse_slot2_enabled = CONFIG_DEFAULT_MOUSE_SLOT2_ENABLED;
    menu->mouse_sensitivity = CONFIG_DEFAULT_MOUSE_SENSITIVITY;
    menu->supersprite_enabled = 0U;
    menu->sdd_stream_enabled = 0U;
    menu->usb0_sd_remote_active = 0U;
    menu->mockingboard_slot4_enabled = CONFIG_DEFAULT_MOCKINGBOARD_SLOT4_ENABLED;
    config_menu_phasor_set_defaults(menu);
    menu->ethernet_slot1_enabled = CONFIG_DEFAULT_ETHERNET_SLOT1_ENABLED;
    menu->ethernet_config_enabled = CONFIG_DEFAULT_ETHERNET_CONFIG_ENABLED;
    menu->ethernet_address_mode = CONFIG_DEFAULT_ETHERNET_ADDRESS_MODE;
    menu->ethernet_edit_index = 0U;
    uthernet2_default_config(&menu->ethernet_config);
    menu->clock_enabled = CONFIG_DEFAULT_CLOCK_ENABLED;
    menu->ram_enabled = CONFIG_DEFAULT_RAM_ENABLED;
    menu->ramworks_enabled = menu->ram_enabled;
    menu->sp_ramdisk_enabled = CONFIG_DEFAULT_SP_RAMDISK_ENABLED;
    config_menu_usb_bindings_set_defaults(menu);
    menu->usb_bindings_editable = usb_bindings_editable;
    menu->usb_binding_capture = CONFIG_MENU_USB_BIND_CAPTURE_NONE;
}

static uint8_t config_menu_read_settings_from_path(config_menu_t *menu,
                                                   const char *path,
                                                   uint8_t reset_first,
                                                   uint32_t *out_cfg_version)
{
    FIL file;
    FRESULT fr;
    UINT bytes_read = 0U;
    char buffer[APPLETINI_CFG_MAX];
    char *line;
    uint32_t cfg_version = 0U;

    if (menu == NULL || path == NULL || path[0] == '\0') {
        return 0U;
    }

    fr = config_menu_open_path(&file, path, FA_READ);
    if (fr != FR_OK) {
        menu->settings_loaded = 0U;
        menu->session_only = 1U;
        config_menu_set_sd_error(menu, "CONFIG READ OPEN FAILED", fr);
        return 0U;
    }

    fr = f_read(&file, buffer, sizeof(buffer) - 1U, &bytes_read);
    (void)f_close(&file);
    if (fr != FR_OK) {
        menu->settings_loaded = 0U;
        menu->session_only = 1U;
        config_menu_set_sd_error(menu, "CONFIG READ FAILED", fr);
        return 0U;
    }

    if (reset_first != 0U) {
        config_menu_reset_settings_only(menu);
    }

    buffer[bytes_read] = '\0';
    line = strtok(buffer, "\r\n");
    while (line != NULL) {
        char *value = NULL;
        char *key = config_menu_parse_config_line(line, &value);
        if (key != NULL) {
            if (strcmp(key, "appletini.config.version") == 0) {
                cfg_version = (uint32_t)strtoul(value, NULL, 10);
            }
            config_menu_parse_key_value(menu, key, value);
        }
        line = strtok(NULL, "\r\n");
    }

    config_menu_coerce_boot_device(menu);
    config_menu_coerce_video_output(menu);
    config_menu_coerce_video_ghosting(menu);
    config_menu_coerce_border(menu);
    config_menu_coerce_ethernet(menu);
    config_menu_usb_bindings_coerce(menu);
    menu->settings_loaded = 1U;
    menu->session_only = 0U;
    if (out_cfg_version != NULL) {
        *out_cfg_version = cfg_version;
    }
    return 1U;
}

uint8_t config_menu_save_profile_settings(config_menu_t *menu,
                                          const char *profile_dir)
{
    char cfg_path[CONFIG_MENU_PATH_LEN];

    if (menu == NULL || profile_dir == NULL || profile_dir[0] == '\0') {
        return 0U;
    }
    if (profile_manager_cfg_path(profile_dir, cfg_path, sizeof(cfg_path)) == 0U) {
        config_menu_set_status(menu, 1U, "PROFILE CONFIG PATH TOO LONG");
        return 0U;
    }

    return config_menu_save_settings_to_path(menu, cfg_path, NULL);
}

static void config_menu_save_active_profile_if_selected(config_menu_t *menu)
{
    if (menu == NULL || menu->profile_source_dir[0] == '\0') {
        return;
    }
    (void)config_menu_save_profile_settings(menu, menu->profile_source_dir);
}

uint8_t config_menu_load_profile_settings(config_menu_t *menu,
                                          const char *profile_dir)
{
    char cfg_path[CONFIG_MENU_PATH_LEN];
    char text[CONFIG_MENU_STATUS_LEN];

    if (menu == NULL || profile_dir == NULL || profile_dir[0] == '\0') {
        return 0U;
    }
    if (profile_manager_cfg_path(profile_dir, cfg_path, sizeof(cfg_path)) == 0U) {
        config_menu_set_status(menu, 1U, "PROFILE CONFIG PATH TOO LONG");
        return 0U;
    }

    if (config_menu_read_settings_from_path(menu, cfg_path, 1U, NULL) == 0U) {
        return 0U;
    }

    config_menu_copy_text(menu->profile_source_dir,
                          sizeof(menu->profile_source_dir),
                          profile_dir);
    config_menu_apply_runtime(menu);
    config_menu_apply_bezel(menu);
    config_menu_apply_video_rom(menu);
    if (config_menu_save_settings_to_path(menu, APPLETINI_CFG_PATH, NULL) == 0U) {
        return 0U;
    }

    (void)snprintf(text,
                   sizeof(text),
                   "LOADED PROFILE %.76s",
                   config_menu_basename(profile_dir));
    config_menu_set_status(menu, 0U, text);
    return 1U;
}

static void config_menu_try_read_rtc(config_menu_t *menu, uint8_t visible_status)
{
    rtc_pcf8563_time_t now;
    int rc;

    if (menu == NULL || menu->platform.read_rtc == NULL) {
        if (visible_status != 0U) {
            config_menu_set_status(menu, 1U, "RTC READ NOT AVAILABLE");
        }
        return;
    }

    memset(&now, 0, sizeof(now));
    rc = menu->platform.read_rtc(menu->platform.ctx, &now);
    if (rc == 0 && now.valid != 0U) {
        menu->clock_time = now;
        if (visible_status != 0U) {
            config_menu_set_status(menu, 0U, "READ PCF8563 CLOCK");
        }
    } else if (visible_status != 0U) {
        char text[CONFIG_MENU_STATUS_LEN];

        if (rc != 0) {
            snprintf(text, sizeof(text), "RTC I2C READ FAILED RC=%d", rc);
        } else if ((now.status & RTC_PCF8563_STATUS_VOLTAGE_LOW) != 0U) {
            snprintf(text, sizeof(text), "RTC VL FLAG SET: WRITE TIME TO CLEAR");
        } else if ((now.status & RTC_PCF8563_STATUS_BAD_FIELD) != 0U) {
            snprintf(text, sizeof(text), "RTC BAD TIME %04u-%02u-%02u %02u:%02u:%02u",
                     (unsigned)now.year, (unsigned)now.month, (unsigned)now.day,
                     (unsigned)now.hour, (unsigned)now.min, (unsigned)now.sec);
        } else {
            snprintf(text, sizeof(text), "RTC TIME INVALID STATUS=0x%02X", (unsigned)now.status);
        }
        config_menu_set_status(menu, 1U, text);
    }
}

void config_menu_apply_startup_assets(config_menu_t *menu)
{
    config_menu_apply_bezel(menu);
    config_menu_apply_video_rom(menu);
    config_menu_try_read_rtc(menu, 0U);
}

/* Single authority for the SuperDuperDisplay stream setting: applies the
 * personality switch AND persists it, so the USB tab checkbox and the uart
 * 'sdd on/off' command can never disagree with what the next boot does. */
void config_menu_set_applicard_enabled(config_menu_t *menu, uint8_t enable)
{
    if (menu == NULL) {
        return;
    }
    menu->applicard_slot5_enabled = enable ? 1U : 0U;
    if (menu->platform.set_slot_enabled != NULL) {
        menu->platform.set_slot_enabled(menu->platform.ctx,
                                        APPLICARD_CONTROL_SLOT,
                                        menu->applicard_slot5_enabled);
    }
    /* Read the live state back so the persisted setting always matches
     * what the Apple actually sees. */
    if (menu->platform.get_slot_enabled != NULL) {
        menu->applicard_slot5_enabled =
            menu->platform.get_slot_enabled(menu->platform.ctx,
                                            APPLICARD_CONTROL_SLOT);
    }
    config_menu_save_settings(menu);
    config_menu_set_status(menu, menu->applicard_slot5_enabled,
                           menu->applicard_slot5_enabled != 0U ?
                               "APPLICARD Z80 ON - SLOT 5" :
                               "APPLICARD Z80 OFF - SLOT 5 EMPTY");
}

void config_menu_set_sdd_stream(config_menu_t *menu, uint8_t enable)
{
    if (menu == NULL) {
        return;
    }
    if (enable != 0U && menu->usb0_sd_remote_active != 0U) {
        if (menu->platform.set_usb0_sd_remote_mount != NULL) {
            menu->platform.set_usb0_sd_remote_mount(menu->platform.ctx, 0U);
        }
        menu->usb0_sd_remote_active = 0U;
    }
    menu->sdd_stream_enabled = enable ? 1U : 0U;
    if (menu->platform.set_sdd_stream_enabled != NULL) {
        menu->platform.set_sdd_stream_enabled(menu->platform.ctx,
                                              menu->sdd_stream_enabled);
    }
    config_menu_save_settings(menu);
    config_menu_set_status(menu, menu->sdd_stream_enabled,
                           menu->sdd_stream_enabled ?
                               "SDD STREAM ON - USB0 STREAMS TO SDD" :
                               "SDD STREAM OFF - USB0 DETACHED");
}

static void config_menu_close_usb0_sd_remote(config_menu_t *menu,
                                             const char *status)
{
    if (menu == NULL || menu->usb0_sd_remote_active == 0U) {
        return;
    }
    if (menu->platform.set_usb0_sd_remote_mount != NULL) {
        menu->platform.set_usb0_sd_remote_mount(menu->platform.ctx, 0U);
    }
    menu->usb0_sd_remote_active = 0U;
    config_menu_set_status(menu,
                           0U,
                           (status != NULL) ?
                               status :
                               "SD CARD REMOTE MOUNTING STOPPED");
}

void config_menu_stop_usb0_sd_remote(config_menu_t *menu)
{
    config_menu_close_usb0_sd_remote(menu,
                                     "SD CARD REMOTE MOUNTING STOPPED");
}

void config_menu_usb0_sd_remote_host_ejected(config_menu_t *menu)
{
    config_menu_close_usb0_sd_remote(menu,
                                     "HOST EJECTED SD REMOTE MOUNT");
}

static void config_menu_start_usb0_sd_remote(config_menu_t *menu)
{
    if (menu == NULL) {
        return;
    }
    if (menu->sdd_stream_enabled != 0U) {
        config_menu_set_status(menu, 1U, "DISABLE SDD STREAM FIRST");
        return;
    }
    if (menu->platform.set_usb0_sd_remote_mount == NULL) {
        config_menu_set_status(menu, 1U, "USB0 SD REMOTE MOUNT UNAVAILABLE");
        return;
    }
    menu->usb0_sd_remote_active = 1U;
    menu->platform.set_usb0_sd_remote_mount(menu->platform.ctx, 1U);
    config_menu_set_status(menu, 1U, "SD CARD REMOTE MOUNTING - ENTER/ESC EXITS");
}

uint8_t config_menu_usb0_sd_remote_active(const config_menu_t *menu)
{
    return (menu != NULL && menu->usb0_sd_remote_active != 0U) ? 1U : 0U;
}

static uint32_t config_menu_tab_item_count(uint32_t tab)
{
    switch (tab) {
    case CONFIG_TAB_BOOT_SETTINGS:
        return CONFIG_MENU_BOOT_ITEM_COUNT;
    case CONFIG_TAB_PROFILES:
        return CONFIG_MENU_PROFILE_ITEM_COUNT;
    case CONFIG_TAB_VIDEO:
        return CONFIG_VIDEO_ITEM_COUNT;
    case CONFIG_TAB_SMARTPORT:
        return SMARTPORT_DEVICE_COUNT + 3U; /* overlay + N slots + ram disk + SuperSprite */
    case CONFIG_TAB_USB:
        return 2U;                          /* SD remote mount + SDD stream */
    case CONFIG_TAB_DISK2:
        return 5U;
    case CONFIG_TAB_MOUSE:
        return 2U;
    case CONFIG_TAB_MOCKINGBOARD:
        return config_menu_phasor_item_count();
    case CONFIG_TAB_ETHERNET:
        return CONFIG_ETHERNET_ITEM_COUNT;
    case CONFIG_TAB_APPLICARD:
        return 2U;
    case CONFIG_TAB_RAM:
        return 1U;
    case CONFIG_TAB_ABOUT:
        return 0U;
    case CONFIG_TAB_CLOCK:
        return 9U;
    default:
        return 1U;
    }
}

uint32_t config_menu_boot_usb_binding_action_for_item(uint32_t item_focus)
{
    const uint32_t index =
        (item_focus >= CONFIG_MENU_BOOT_USB_BIND_FIRST_ITEM) ?
        (item_focus - CONFIG_MENU_BOOT_USB_BIND_FIRST_ITEM) :
        CONFIG_MENU_USB_BIND_ACTION_COUNT;

    if (index >= CONFIG_MENU_USB_BIND_ACTION_COUNT) {
        return CONFIG_MENU_USB_BIND_ACTION_COUNT;
    }
    return k_boot_usb_binding_action_order[index];
}

uint32_t config_menu_boot_usb_binding_item_for_action(uint32_t action)
{
    for (uint32_t i = 0U; i < CONFIG_MENU_USB_BIND_ACTION_COUNT; ++i) {
        if (k_boot_usb_binding_action_order[i] == action) {
            return CONFIG_MENU_BOOT_USB_BIND_FIRST_ITEM + i;
        }
    }
    return CONFIG_MENU_BOOT_ITEM_COUNT;
}

static void config_menu_clamp_item(config_menu_t *menu)
{
    const uint32_t count = (menu != NULL) ? config_menu_tab_item_count(menu->tab) : 0U;

    if (menu == NULL || count == 0U) {
        return;
    }
    if (menu->item_focus >= count) {
        menu->item_focus = count - 1U;
    }
}

static void config_menu_next_tab(config_menu_t *menu)
{
    if (menu == NULL) {
        return;
    }
    menu->tab = (menu->tab + 1U) % CONFIG_TAB_COUNT;
    menu->item_focus = 0U;
    config_menu_clamp_item(menu);
}

static void config_menu_prev_tab(config_menu_t *menu)
{
    if (menu == NULL) {
        return;
    }
    menu->tab = (menu->tab == 0U) ? (CONFIG_TAB_COUNT - 1U) : (menu->tab - 1U);
    menu->item_focus = 0U;
    config_menu_clamp_item(menu);
}

static void config_menu_next_item(config_menu_t *menu)
{
    const uint32_t count = (menu != NULL) ? config_menu_tab_item_count(menu->tab) : 0U;

    if (menu == NULL || count == 0U) {
        return;
    }
    menu->item_focus = (menu->item_focus + 1U) % count;
}

static void config_menu_prev_item(config_menu_t *menu)
{
    const uint32_t count = (menu != NULL) ? config_menu_tab_item_count(menu->tab) : 0U;

    if (menu == NULL || count == 0U) {
        return;
    }
    menu->item_focus = (menu->item_focus == 0U) ? (count - 1U) : (menu->item_focus - 1U);
}

static uint8_t *config_menu_ethernet_edit_target(config_menu_t *menu,
                                                 uint32_t item,
                                                 uint8_t *width)
{
    if (width != NULL) {
        *width = 0U;
    }
    if (menu == NULL) {
        return NULL;
    }
    switch (item) {
    case CONFIG_ETHERNET_ITEM_MAC:
        if (width != NULL) {
            *width = UTHERNET2_MAC_LEN;
        }
        return menu->ethernet_config.mac;
    case CONFIG_ETHERNET_ITEM_IP:
        if (width != NULL) {
            *width = UTHERNET2_IPV4_LEN;
        }
        return menu->ethernet_config.ip;
    case CONFIG_ETHERNET_ITEM_SUBNET:
        if (width != NULL) {
            *width = UTHERNET2_IPV4_LEN;
        }
        return menu->ethernet_config.subnet;
    case CONFIG_ETHERNET_ITEM_GATEWAY:
        if (width != NULL) {
            *width = UTHERNET2_IPV4_LEN;
        }
        return menu->ethernet_config.gateway;
    default:
        return NULL;
    }
}

static const char *config_menu_ethernet_field_name(uint32_t item)
{
    switch (item) {
    case CONFIG_ETHERNET_ITEM_MAC:
        return "MAC BYTE";
    case CONFIG_ETHERNET_ITEM_IP:
        return "IP OCTET";
    case CONFIG_ETHERNET_ITEM_SUBNET:
        return "SUBNET OCTET";
    case CONFIG_ETHERNET_ITEM_GATEWAY:
        return "GATEWAY OCTET";
    default:
        return "ETHERNET FIELD";
    }
}

static void config_menu_ethernet_toggle_slot(config_menu_t *menu)
{
    if (menu == NULL) {
        return;
    }
    menu->ethernet_slot1_enabled = menu->ethernet_slot1_enabled ? 0U : 1U;
    if (menu->platform.set_slot_enabled != NULL) {
        menu->platform.set_slot_enabled(menu->platform.ctx,
                                        ETHERNET_CONTROL_SLOT,
                                        menu->ethernet_slot1_enabled);
    }
    config_menu_save_settings(menu);
    config_menu_set_status(menu,
                           menu->ethernet_slot1_enabled ? 0U : 1U,
                           menu->ethernet_slot1_enabled ?
                               "UTHERNET II ENABLED IN SLOT 1" :
                               "UTHERNET II DISABLED");
}

static void config_menu_ethernet_toggle_saved_config(config_menu_t *menu)
{
    if (menu == NULL) {
        return;
    }
    menu->ethernet_config_enabled =
        (menu->ethernet_config_enabled != 0U) ? 0U : 1U;
    config_menu_save_settings(menu);
    if (menu->session_only != 0U) {
        return;
    }
    config_menu_set_status(menu,
                           0U,
                           menu->ethernet_config_enabled != 0U ?
                               "UTHERNET II BOOT CONFIG ENABLED" :
                               "UTHERNET II SAVED CONFIG NOT AUTO-APPLIED");
}

static void config_menu_ethernet_toggle_mode(config_menu_t *menu)
{
    char text[CONFIG_MENU_STATUS_LEN];

    if (menu == NULL) {
        return;
    }
    menu->ethernet_address_mode =
        (menu->ethernet_address_mode == CONFIG_MENU_ETHERNET_ADDRESS_DHCP) ?
        CONFIG_MENU_ETHERNET_ADDRESS_STATIC :
        CONFIG_MENU_ETHERNET_ADDRESS_DHCP;
    config_menu_save_settings(menu);
    if (menu->session_only != 0U) {
        return;
    }
    (void)snprintf(text,
                   sizeof(text),
                   "ETHERNET ADDRESS MODE %s",
                   config_menu_ethernet_address_mode_text(menu->ethernet_address_mode));
    config_menu_set_status(menu, 0U, text);
}

static uint8_t config_menu_ethernet_adjust_value(config_menu_t *menu,
                                                 int8_t delta)
{
    uint8_t width = 0U;
    uint8_t *target = config_menu_ethernet_edit_target(menu,
                                                       menu != NULL ?
                                                           menu->item_focus : 0U,
                                                       &width);
    uint8_t index;
    char text[CONFIG_MENU_STATUS_LEN];

    if (menu == NULL || target == NULL || width == 0U) {
        return 0U;
    }

    index = config_menu_ethernet_selected_index(menu, width);
    target[index] = config_menu_wrapped_u8_delta(target[index], delta);
    config_menu_save_settings(menu);
    if (menu->session_only != 0U) {
        return 1U;
    }
    (void)snprintf(text,
                   sizeof(text),
                   "%s %u = %u",
                   config_menu_ethernet_field_name(menu->item_focus),
                   (unsigned)(index + 1U),
                   (unsigned)target[index]);
    config_menu_set_status(menu, 0U, text);
    return 1U;
}

static void config_menu_ethernet_cycle_edit_index(config_menu_t *menu)
{
    uint8_t width = 0U;
    const uint8_t *target = config_menu_ethernet_edit_target(menu,
                                                            menu != NULL ?
                                                                menu->item_focus : 0U,
                                                            &width);
    uint8_t index;
    char text[CONFIG_MENU_STATUS_LEN];

    (void)target;
    if (menu == NULL || width == 0U) {
        return;
    }

    index = (uint8_t)((config_menu_ethernet_selected_index(menu, width) + 1U) %
                      width);
    menu->ethernet_edit_index = index;
    (void)snprintf(text,
                   sizeof(text),
                   "EDITING %s %u",
                   config_menu_ethernet_field_name(menu->item_focus),
                   (unsigned)(index + 1U));
    config_menu_set_status(menu, 0U, text);
}

static void config_menu_ethernet_set_status_ip(config_menu_t *menu,
                                               const char *prefix,
                                               uint8_t warning,
                                               const uthernet2_network_config_t *config)
{
    char ip[16];
    char text[CONFIG_MENU_STATUS_LEN];

    if (menu == NULL || prefix == NULL || config == NULL) {
        return;
    }
    config_menu_format_ipv4(config->ip, ip, sizeof(ip));
    (void)snprintf(text, sizeof(text), "%s %s", prefix, ip);
    config_menu_set_status(menu, warning, text);
}

static void config_menu_ethernet_read_from_card(config_menu_t *menu)
{
    uthernet2_network_config_t config;

    if (config_menu_ethernet_card_access(menu) == 0U) {
        return;
    }
    if (menu->platform.ethernet_read_config == NULL) {
        config_menu_set_status(menu, 1U, "UTHERNET II CONFIG READ UNAVAILABLE");
        return;
    }
    if (menu->platform.ethernet_read_config(menu->platform.ctx, &config) != 0) {
        config_menu_set_status(menu, 1U, "UTHERNET II CONFIG READ FAILED");
        return;
    }
    if ((config.ip[0] | config.ip[1] | config.ip[2] | config.ip[3]) == 0U) {
        config_menu_set_status(menu, 1U, "UTHERNET II CARD CONFIG IS EMPTY");
        return;
    }
    if (uthernet2_mac_is_valid(config.mac) == 0U) {
        config_menu_set_status(menu, 1U, "UTHERNET II CARD MAC IS INVALID");
        return;
    }
    menu->ethernet_config = config;
    config_menu_save_settings(menu);
    if (menu->session_only != 0U) {
        return;
    }
    config_menu_ethernet_set_status_ip(menu,
                                       "READ UTHERNET II IP",
                                       0U,
                                       &menu->ethernet_config);
}

static void config_menu_ethernet_write_to_card(config_menu_t *menu)
{
    if (config_menu_ethernet_card_access(menu) == 0U) {
        return;
    }
    if (menu->platform.ethernet_write_config == NULL) {
        config_menu_set_status(menu, 1U, "UTHERNET II CONFIG WRITE UNAVAILABLE");
        return;
    }
    if (menu->platform.ethernet_write_config(menu->platform.ctx,
                                             &menu->ethernet_config) != 0) {
        config_menu_set_status(menu, 1U, "UTHERNET II CONFIG WRITE FAILED");
        return;
    }
    menu->ethernet_address_mode = CONFIG_MENU_ETHERNET_ADDRESS_STATIC;
    menu->ethernet_config_enabled = 1U;
    config_menu_save_settings(menu);
    if (menu->session_only != 0U) {
        return;
    }
    config_menu_ethernet_set_status_ip(menu,
                                       "WROTE UTHERNET II IP",
                                       0U,
                                       &menu->ethernet_config);
}

static void config_menu_ethernet_dhcp(config_menu_t *menu)
{
    (void)config_menu_acquire_ethernet_dhcp(menu, 1U);
}

static void config_menu_ethernet_test(config_menu_t *menu)
{
    uthernet2_test_result_t result;
    char ip[16];
    char text[CONFIG_MENU_STATUS_LEN];

    if (config_menu_ethernet_card_access(menu) == 0U) {
        return;
    }
    if (menu->platform.ethernet_test == NULL) {
        config_menu_set_status(menu, 1U, "UTHERNET II TEST UNAVAILABLE");
        return;
    }
    if (menu->ethernet_address_mode == CONFIG_MENU_ETHERNET_ADDRESS_STATIC) {
        if (menu->platform.ethernet_write_config == NULL) {
            config_menu_set_status(menu, 1U, "UTHERNET II CONFIG WRITE UNAVAILABLE");
            return;
        }
        if (menu->platform.ethernet_write_config(menu->platform.ctx,
                                                 &menu->ethernet_config) != 0) {
            config_menu_set_status(menu, 1U, "UTHERNET II CONFIG WRITE FAILED");
            return;
        }
    }
    if (menu->platform.ethernet_test(menu->platform.ctx, &result) != 0) {
        config_menu_set_status(menu, 1U, "UTHERNET II TEST FAILED");
        return;
    }
    config_menu_format_ipv4(result.config.ip, ip, sizeof(ip));
    (void)snprintf(text,
                   sizeof(text),
                   "W5100S V=0x%02X LINK %s IP %s",
                   (unsigned)result.version,
                   (result.link_up != 0U) ? "UP" : "DOWN",
                   ip);
    config_menu_set_status(menu, (result.link_up != 0U) ? 0U : 1U, text);
}

static uint8_t config_menu_adjust_focused_value(config_menu_t *menu, int8_t delta)
{
    uint32_t index;

    if (menu == NULL) {
        return 0U;
    }

    if (menu->tab == CONFIG_TAB_VIDEO &&
        menu->item_focus == CONFIG_VIDEO_ITEM_SCANLINES) {
        if (delta < 0) {
            menu->scanlines_mode = (menu->scanlines_mode == APPLETINI_SCANLINES_OFF) ?
                APPLETINI_SCANLINES_STRONG : (uint8_t)(menu->scanlines_mode - 1U);
        } else {
            menu->scanlines_mode =
                (uint8_t)((menu->scanlines_mode + 1U) % APPLETINI_SCANLINES_COUNT);
        }
        config_menu_apply_runtime(menu);
        config_menu_save_settings(menu);
        return 1U;
    }

    if (menu->tab == CONFIG_TAB_VIDEO &&
        menu->item_focus == CONFIG_VIDEO_ITEM_VIDEO7) {
        (void)delta;
        menu->video7_auto_mono_enabled =
            (menu->video7_auto_mono_enabled != 0U) ? 0U : 1U;
        config_menu_apply_runtime(menu);
        config_menu_save_settings(menu);
        return 1U;
    }

    if (menu->tab == CONFIG_TAB_VIDEO &&
        menu->item_focus == CONFIG_VIDEO_ITEM_OUTPUT) {
        menu->video_output_mono = menu->video_output_mono ? 0U : 1U;
        config_menu_apply_runtime(menu);
        config_menu_save_settings(menu);
        return 1U;
    }

    if (menu->tab == CONFIG_TAB_VIDEO &&
        menu->item_focus == CONFIG_VIDEO_ITEM_VARIANT) {
        if (menu->video_output_mono != 0U) {
            menu->video_mono_color =
                config_menu_next_mono_color(menu->video_mono_color, delta);
        } else {
            menu->video_color_mode =
                config_menu_next_color_mode(menu, menu->video_color_mode, delta);
        }
        config_menu_apply_runtime(menu);
        config_menu_save_settings(menu);
        return 1U;
    }

    if (menu->tab == CONFIG_TAB_VIDEO &&
        menu->item_focus == CONFIG_VIDEO_ITEM_GHOSTING) {
        int32_t strength =
            (int32_t)menu->video_ghosting_strength + (int32_t)delta;
        if (strength < 0) {
            strength = 0;
        }
        if (strength > (int32_t)APPLETINI_VIDEO_GHOSTING_MAX) {
            strength = (int32_t)APPLETINI_VIDEO_GHOSTING_MAX;
        }
        if (menu->video_ghosting_strength != (uint8_t)strength) {
            menu->video_ghosting_strength = (uint8_t)strength;
            config_menu_apply_runtime(menu);
            config_menu_save_settings(menu);
        }
        return 1U;
    }

    if (menu->tab == CONFIG_TAB_VIDEO &&
        menu->item_focus == CONFIG_VIDEO_ITEM_BORDER) {
        (void)delta;
        menu->border_enabled = (menu->border_enabled != 0u) ? 0u : 1u;
        config_menu_apply_runtime(menu);
        config_menu_save_settings(menu);
        return 1u;
    }

    if (menu->tab == CONFIG_TAB_VIDEO &&
        menu->item_focus == CONFIG_VIDEO_ITEM_BORDER_COLOR) {
        if (delta < 0) {
            menu->border_color = (menu->border_color == 0u) ?
                15u : (uint8_t)(menu->border_color - 1u);
        } else {
            menu->border_color = (uint8_t)((menu->border_color + 1u) & 0x0Fu);
        }
        config_menu_apply_runtime(menu);
        config_menu_save_settings(menu);
        return 1u;
    }

    if (menu->tab == CONFIG_TAB_VIDEO &&
        menu->item_focus == CONFIG_VIDEO_ITEM_BORDER_FLOOD) {
        (void)delta;
        menu->border_flood = (menu->border_flood != 0u) ? 0u : 1u;
        config_menu_apply_runtime(menu);
        config_menu_save_settings(menu);
        return 1u;
    }

    if (menu->tab == CONFIG_TAB_VIDEO &&
        menu->border_flood != 0u &&
        menu->item_focus >= CONFIG_VIDEO_ITEM_SHOW_BEZEL &&
        menu->item_focus <= CONFIG_VIDEO_ITEM_DEBUG) {
        return 1u;
    }

    if (menu->tab == CONFIG_TAB_BOOT_SETTINGS && menu->item_focus == 1U) {
        if (menu->disk2_slot6_enabled == 0U) {
            menu->boot_device = CONFIG_BOOT_DEVICE_SMARTPORT;
            config_menu_apply_runtime(menu);
            config_menu_save_settings(menu);
            config_menu_set_status(menu, 1U, "ENABLE DISK II TO BOOT SLOT 6");
            return 1U;
        }
        if (menu->boot_device == CONFIG_BOOT_DEVICE_SMARTPORT) {
            menu->boot_device = CONFIG_BOOT_DEVICE_DISK2;
        } else {
            menu->boot_device = CONFIG_BOOT_DEVICE_SMARTPORT;
        }
        config_menu_apply_runtime(menu);
        config_menu_save_settings(menu);
        return 1U;
    }

    if (menu->tab == CONFIG_TAB_VIDEO &&
        menu->item_focus == CONFIG_VIDEO_ITEM_DEBUG) {
        (void)delta;
        menu->show_debugging = (menu->show_debugging != 0U) ? 0U : 1U;
        config_menu_save_settings(menu);
        return 1U;
    }

    if (menu->tab == CONFIG_TAB_VIDEO &&
        menu->item_focus == CONFIG_VIDEO_ITEM_SHOW_BEZEL) {
        (void)delta;
        menu->show_bezel = (menu->show_bezel != 0U) ? 0U : 1U;
        config_menu_save_settings(menu);
        config_menu_save_active_profile_if_selected(menu);
        return 1U;
    }

    if (menu->tab == CONFIG_TAB_BOOT_SETTINGS &&
        menu->item_focus >= CONFIG_MENU_BOOT_USB_BIND_FIRST_ITEM &&
        menu->item_focus < CONFIG_MENU_BOOT_ITEM_COUNT) {
        const uint32_t action =
            config_menu_boot_usb_binding_action_for_item(menu->item_focus);

        if (menu->usb_bindings_editable == 0U) {
            config_menu_set_status(menu, 1U, "USB BINDINGS EDITABLE AT BOOT");
            return 1U;
        }
        if (action >= CONFIG_MENU_USB_BIND_ACTION_COUNT) {
            return 1U;
        }
        config_menu_assign_usb_binding(
            menu,
            action,
            config_menu_usb_binding_next_source(action,
                                                menu->usb_menu_bindings[action],
                                                delta));
        config_menu_save_settings(menu);
        return 1U;
    }

    if ((menu->tab == CONFIG_TAB_SMARTPORT && menu->item_focus == 0U) ||
        (menu->tab == CONFIG_TAB_DISK2 && menu->item_focus == 1U)) {
        (void)delta;
        if (menu->tab == CONFIG_TAB_SMARTPORT &&
            menu->supersprite_enabled != 0U) {
            config_menu_set_status(menu, 1U, "SUPERSPRITE DISABLES SMARTPORT");
            return 1U;
        }
        menu->disk2_activity_visible =
            (menu->disk2_activity_visible != 0U) ? 0U : 1U;
        config_menu_save_settings(menu);
        return 1U;
    }

    if (menu->tab == CONFIG_TAB_DISK2 && menu->item_focus == 4U) {
        int32_t volume = (int32_t)menu->disk2_sound_volume + (int32_t)delta;
        if (volume < 0) {
            volume = 0;
        }
        if (volume > (int32_t)CONFIG_MAX_DISK2_SOUND_VOLUME) {
            volume = (int32_t)CONFIG_MAX_DISK2_SOUND_VOLUME;
        }
        if (menu->disk2_sound_volume != (uint8_t)volume) {
            menu->disk2_sound_volume = (uint8_t)volume;
            config_menu_apply_disk2_sound(menu);
            config_menu_save_settings(menu);
        }
        return 1U;
    }

    if (menu->tab == CONFIG_TAB_MOCKINGBOARD) {
        return config_menu_phasor_adjust(menu, delta);
    }

    if (menu->tab == CONFIG_TAB_ETHERNET) {
        if (menu->item_focus == CONFIG_ETHERNET_ITEM_SLOT) {
            (void)delta;
            config_menu_ethernet_toggle_slot(menu);
            return 1U;
        }
        if (menu->item_focus == CONFIG_ETHERNET_ITEM_CONFIG_ENABLED) {
            (void)delta;
            config_menu_ethernet_toggle_saved_config(menu);
            return 1U;
        }
        if (menu->item_focus == CONFIG_ETHERNET_ITEM_MODE) {
            (void)delta;
            config_menu_ethernet_toggle_mode(menu);
            return 1U;
        }
        if (config_menu_ethernet_adjust_value(menu, delta) != 0U) {
            return 1U;
        }
    }

    if (menu->tab == CONFIG_TAB_CLOCK &&
        config_menu_adjust_clock_field(menu, delta) != 0U) {
        return 1U;
    }

    if (menu->tab == CONFIG_TAB_MOUSE && menu->item_focus == 1U) {
        index = config_menu_mouse_sensitivity_index(menu->mouse_sensitivity);
        if (delta < 0) {
            if (index > 0U) {
                --index;
            }
        } else if (index < (MOUSE_SENSITIVITY_STEP_COUNT - 1U)) {
            ++index;
        }
        if (menu->mouse_sensitivity != k_mouse_sensitivity_steps[index]) {
            menu->mouse_sensitivity = k_mouse_sensitivity_steps[index];
            if (menu->platform.set_mouse_sensitivity != NULL) {
                menu->platform.set_mouse_sensitivity(menu->platform.ctx,
                                                     menu->mouse_sensitivity);
            }
            config_menu_save_settings(menu);
        }
        return 1U;
    }

    return 0U;
}

static void config_menu_reload_smartport_device(config_menu_t *menu, uint8_t device)
{
    int rc = 0;
    char text[CONFIG_MENU_STATUS_LEN];
    const uint8_t index = (uint8_t)(device - 1U);

    if (menu == NULL || device == 0U || device > SMARTPORT_DEVICE_COUNT) {
        return;
    }

    if (menu->platform.set_smartport_image_path != NULL) {
        rc = menu->platform.set_smartport_image_path(menu->platform.ctx,
                                                    device,
                                                    menu->smartport_disk_paths[index]);
    }
    if (rc == 0 && menu->platform.reset_smartport_media != NULL) {
        rc = menu->platform.reset_smartport_media(menu->platform.ctx, device);
    }

    if (rc == 0) {
        (void)snprintf(text,
                       sizeof(text),
                       "SMARTPORT SP%u: %.76s",
                       (unsigned)device,
                       config_menu_basename(menu->smartport_disk_paths[index]));
        config_menu_set_status(menu, 0U, text);
    } else {
        (void)snprintf(text,
                       sizeof(text),
                       "SMARTPORT SP%u LOAD FAILED RC=%d",
                       (unsigned)device,
                       rc);
        config_menu_set_status(menu, 1U, text);
    }
}

static void config_menu_clear_smartport_device(config_menu_t *menu, uint8_t device)
{
    int rc = 0;
    char text[CONFIG_MENU_STATUS_LEN];
    const uint8_t index = (uint8_t)(device - 1U);

    if (menu == NULL || device == 0U || device > SMARTPORT_DEVICE_COUNT) {
        return;
    }

    menu->smartport_disk_paths[index][0] = '\0';
    menu->smartport_slots[index] = 0U;
    if (menu->platform.set_smartport_image_path != NULL) {
        rc = menu->platform.set_smartport_image_path(menu->platform.ctx, device, "");
    }
    if (rc == 0 && menu->platform.reset_smartport_media != NULL) {
        rc = menu->platform.reset_smartport_media(menu->platform.ctx, device);
    }
    config_menu_save_settings(menu);
    (void)snprintf(text,
                   sizeof(text),
                   (rc == 0) ? "SMARTPORT SP%u EMPTIED" : "SMARTPORT SP%u EMPTY FAILED",
                   (unsigned)device);
    config_menu_set_status(menu, (rc == 0) ? 0U : 1U, text);
}

static void config_menu_reload_disk2_drive(config_menu_t *menu, uint8_t drive)
{
    int rc = 0;
    char text[CONFIG_MENU_STATUS_LEN];

    if (menu == NULL || drive >= 2U) {
        return;
    }

    if (menu->platform.set_disk2_image_path != NULL) {
        rc = menu->platform.set_disk2_image_path(menu->platform.ctx,
                                                drive,
                                                menu->disk2_disk_paths[drive]);
    }
    if (rc == 0 && menu->platform.reset_disk2_media != NULL) {
        rc = menu->platform.reset_disk2_media(menu->platform.ctx, drive);
    }

    if (rc == 0) {
        if (menu->platform.play_disk2_sound_event != NULL) {
            menu->platform.play_disk2_sound_event(
                menu->platform.ctx,
                CONFIG_DISK2_SOUND_EVENT_DOOR_CLOSE);
        }
        (void)snprintf(text,
                       sizeof(text),
                       "DISK II D%u: %.76s",
                       (unsigned)drive + 1U,
                       config_menu_basename(menu->disk2_disk_paths[drive]));
        config_menu_set_status(menu, 0U, text);
    } else {
        (void)snprintf(text,
                       sizeof(text),
                       "DISK II LOAD FAILED D%u RC=%d",
                       (unsigned)drive + 1U,
                       rc);
        config_menu_set_status(menu, 1U, text);
    }
}

static void config_menu_clear_disk2_drive(config_menu_t *menu, uint8_t drive)
{
    int rc = 0;
    char text[CONFIG_MENU_STATUS_LEN];

    if (menu == NULL || drive >= 2U) {
        return;
    }

    menu->disk2_disk_paths[drive][0] = '\0';
    menu->disk2_slots[drive][0] = 0U;
    if (menu->platform.set_disk2_image_path != NULL) {
        rc = menu->platform.set_disk2_image_path(menu->platform.ctx, drive, "");
    }
    if (rc == 0 && menu->platform.reset_disk2_media != NULL) {
        rc = menu->platform.reset_disk2_media(menu->platform.ctx, drive);
    }
    if (rc == 0 && menu->platform.play_disk2_sound_event != NULL) {
        menu->platform.play_disk2_sound_event(
            menu->platform.ctx,
            CONFIG_DISK2_SOUND_EVENT_DOOR_OPEN);
    }
    config_menu_save_settings(menu);
    (void)snprintf(text,
                   sizeof(text),
                   (rc == 0) ? "DISK II D%u EMPTIED" : "DISK II D%u EMPTY FAILED",
                   (unsigned)drive + 1U);
    config_menu_set_status(menu, (rc == 0) ? 0U : 1U, text);
}

static uint8_t config_menu_browser_is_disk2_target(uint8_t target)
{
    return (target == CONFIG_BROWSER_TARGET_DISK2_D1 ||
            target == CONFIG_BROWSER_TARGET_DISK2_D2) ? 1U : 0U;
}

static uint8_t config_menu_browser_is_smartport_target(uint8_t target)
{
    return (target >= CONFIG_BROWSER_TARGET_SMARTPORT_1 &&
            target <= CONFIG_BROWSER_TARGET_SMARTPORT_8) ? 1U : 0U;
}

static uint8_t config_menu_browser_is_bezel_target(uint8_t target)
{
    return (target == CONFIG_BROWSER_TARGET_BEZEL) ? 1U : 0U;
}

static uint8_t config_menu_browser_target_smartport_device(uint8_t target)
{
    if (config_menu_browser_is_smartport_target(target) == 0U) {
        return 1U;
    }
    return (uint8_t)(target - CONFIG_BROWSER_TARGET_SMARTPORT_1 + 1U);
}

static uint8_t config_menu_browser_entry_is_duplicate_smartport_image(
    const config_menu_t *menu,
    const config_browser_entry_t *entry)
{
    uint8_t device;

    if (menu == NULL || entry == NULL ||
        entry->type != CONFIG_BROWSER_ENTRY_FILE ||
        config_menu_browser_is_smartport_target(menu->browser_target) == 0U) {
        return 0U;
    }

    device = config_menu_browser_target_smartport_device(menu->browser_target);
    return config_menu_smartport_path_in_use(menu, device, entry->path);
}

static const char *config_menu_browser_title(uint8_t target)
{
    static char title[32];

    switch (target) {
    case CONFIG_BROWSER_TARGET_DISK2_D1:
        return "Disk II Slot 6 Drive 1";
    case CONFIG_BROWSER_TARGET_DISK2_D2:
        return "Disk II Slot 6 Drive 2";
    case CONFIG_BROWSER_TARGET_BEZEL:
        return "Bezel PNG";
    case CONFIG_BROWSER_TARGET_VIDEO_ROM:
        return "Video ROM";
    case CONFIG_BROWSER_TARGET_PROFILE_IMAGE:
        return "Profile Image PNG";
    default:
        if (config_menu_browser_is_smartport_target(target) != 0U) {
            (void)snprintf(title,
                           sizeof(title),
                           "SmartPort SP%u",
                           (unsigned)config_menu_browser_target_smartport_device(target));
            return title;
        }
        return "File Browser";
    }
}

static const char *config_menu_browser_empty_text(uint8_t target)
{
    switch (target) {
    case CONFIG_BROWSER_TARGET_DISK2_D1:
        return "[EMPTY DISK 1]";
    case CONFIG_BROWSER_TARGET_DISK2_D2:
        return "[EMPTY DISK 2]";
    case CONFIG_BROWSER_TARGET_BEZEL:
        return "[AUTO BEZEL]";
    case CONFIG_BROWSER_TARGET_VIDEO_ROM:
        return "[FIRMWARE ROM (US Enhanced)]";
    case CONFIG_BROWSER_TARGET_PROFILE_IMAGE:
        return "[CANCEL IMAGE]";
    default:
        if (config_menu_browser_is_smartport_target(target) != 0U) {
            return "[EMPTY SMARTPORT DEVICE]";
        }
        return "[EMPTY]";
    }
}

static uint8_t config_menu_browser_accepts(const config_menu_t *menu,
                                           const FILINFO *info)
{
    if (menu == NULL || info == NULL || info->fname[0] == '\0') {
        return 0U;
    }
    if (strcmp(info->fname, ".") == 0 || strcmp(info->fname, "..") == 0) {
        return 0U;
    }
    if ((info->fattrib & AM_DIR) != 0U) {
        return 1U;
    }
    if (config_menu_browser_is_smartport_target(menu->browser_target) != 0U) {
        return config_menu_has_smartport_ext(info->fname, info->fsize);
    }
    if (config_menu_browser_is_disk2_target(menu->browser_target) != 0U) {
        return config_menu_has_disk2_ext(info->fname, info->fsize);
    }
    if (config_menu_browser_is_bezel_target(menu->browser_target) != 0U) {
        return config_menu_has_png_ext(info->fname);
    }
    if (menu->browser_target == CONFIG_BROWSER_TARGET_VIDEO_ROM) {
        return config_menu_is_video_rom_file(info->fname, info->fsize);
    }
    if (menu->browser_target == CONFIG_BROWSER_TARGET_PROFILE_IMAGE) {
        return config_menu_has_png_ext(info->fname);
    }
    return 0U;
}

static void config_menu_browser_add_entry(config_menu_t *menu,
                                          config_browser_entry_type_t type,
                                          const char *name,
                                          const char *path,
                                          uint8_t read_only)
{
    config_browser_entry_t *entry;

    if (menu == NULL || menu->browser_count >= CONFIG_BROWSER_MAX_ENTRIES) {
        return;
    }

    entry = &g_browser_entries[menu->browser_count++];
    memset(entry, 0, sizeof(*entry));
    entry->type = type;
    entry->read_only = read_only;
    config_menu_copy_text(entry->name, sizeof(entry->name), name);
    config_menu_copy_text(entry->path, sizeof(entry->path), path);
}

static int config_menu_browser_compare(const config_browser_entry_t *a,
                                       const config_browser_entry_t *b)
{
    const char *an;
    const char *bn;

    if (a == NULL || b == NULL) {
        return 0;
    }
    if (a->type == CONFIG_BROWSER_ENTRY_DIR && b->type != CONFIG_BROWSER_ENTRY_DIR) {
        return -1;
    }
    if (a->type != CONFIG_BROWSER_ENTRY_DIR && b->type == CONFIG_BROWSER_ENTRY_DIR) {
        return 1;
    }

    an = a->name;
    bn = b->name;
    while (*an != '\0' && *bn != '\0') {
        const uint8_t ac = config_menu_ascii_lower((uint8_t)*an);
        const uint8_t bc = config_menu_ascii_lower((uint8_t)*bn);
        if (ac != bc) {
            return (ac < bc) ? -1 : 1;
        }
        ++an;
        ++bn;
    }
    if (*an == '\0' && *bn == '\0') {
        return 0;
    }
    return (*an == '\0') ? -1 : 1;
}

static void config_menu_browser_sort(config_menu_t *menu)
{
    uint16_t start;
    uint16_t i;

    if (menu == NULL || menu->browser_count <= 2U) {
        return;
    }
    start = 0U;
    while (start < menu->browser_count &&
           (g_browser_entries[start].type == CONFIG_BROWSER_ENTRY_CLOSE ||
            g_browser_entries[start].type == CONFIG_BROWSER_ENTRY_EMPTY ||
            g_browser_entries[start].type == CONFIG_BROWSER_ENTRY_PARENT)) {
        ++start;
    }
    if ((uint16_t)(start + 1U) >= menu->browser_count) {
        return;
    }
    for (i = (uint16_t)(start + 1U); i < menu->browser_count; ++i) {
        config_browser_entry_t item = g_browser_entries[i];
        uint16_t j = i;

        while (j > start &&
               config_menu_browser_compare(&item, &g_browser_entries[j - 1U]) < 0) {
            g_browser_entries[j] = g_browser_entries[j - 1U];
            --j;
        }
        g_browser_entries[j] = item;
    }
}

static FRESULT config_menu_browser_refresh(config_menu_t *menu)
{
    DIR dir;
    FILINFO info;
    FRESULT fr;

    if (menu == NULL || menu->browser_active == 0U) {
        return FR_INVALID_OBJECT;
    }

    menu->browser_count = 0U;
    if (config_menu_path_is_root(menu->browser_dir) != 0U) {
        config_menu_browser_add_entry(menu, CONFIG_BROWSER_ENTRY_CLOSE, "[CLOSE]", "", 0U);
    } else {
        char parent[CONFIG_MENU_PATH_LEN];

        config_menu_parent_path(menu->browser_dir, parent, sizeof(parent));
        config_menu_browser_add_entry(menu, CONFIG_BROWSER_ENTRY_PARENT, "[..]", parent, 0U);
    }
    config_menu_browser_add_entry(menu,
                                  CONFIG_BROWSER_ENTRY_EMPTY,
                                  config_menu_browser_empty_text(menu->browser_target),
                                  "",
                                  0U);

    fr = f_opendir(&dir, menu->browser_dir);
    if (fr != FR_OK) {
        fr = config_menu_mount_sd();
        if (fr != FR_OK) {
            return fr;
        }
        fr = f_opendir(&dir, menu->browser_dir);
        if (fr != FR_OK) {
            return fr;
        }
    }

    for (;;) {
        fr = f_readdir(&dir, &info);
        if (fr != FR_OK || info.fname[0] == '\0') {
            break;
        }
        if (config_menu_browser_accepts(menu, &info) != 0U) {
            char path[CONFIG_MENU_PATH_LEN];
            config_browser_entry_type_t type = ((info.fattrib & AM_DIR) != 0U) ?
                CONFIG_BROWSER_ENTRY_DIR : CONFIG_BROWSER_ENTRY_FILE;

            if (config_menu_join_path(menu->browser_dir,
                                      info.fname,
                                      path,
                                      sizeof(path)) != 0U) {
                const uint8_t read_only =
                    (type == CONFIG_BROWSER_ENTRY_FILE &&
                     (info.fattrib & AM_RDO) != 0U) ? 1U : 0U;
                config_menu_browser_add_entry(menu, type, info.fname, path, read_only);
            }
        }
    }
    (void)f_closedir(&dir);
    if (fr != FR_OK) {
        return fr;
    }

    config_menu_browser_sort(menu);
    if (menu->browser_count == 0U) {
        menu->browser_selected = 0U;
        menu->browser_top = 0U;
    } else if (menu->browser_selected >= menu->browser_count) {
        menu->browser_selected = (uint16_t)(menu->browser_count - 1U);
    }
    return FR_OK;
}

static FRESULT config_menu_browser_get_entry(const config_menu_t *menu,
                                             uint16_t index,
                                             config_browser_entry_t *out)
{
    if (menu == NULL || out == NULL || menu->browser_active == 0U) {
        return FR_INVALID_OBJECT;
    }
    if (index >= menu->browser_count || index >= CONFIG_BROWSER_MAX_ENTRIES) {
        return FR_NO_FILE;
    }
    *out = g_browser_entries[index];
    return FR_OK;
}

static void config_menu_browser_preview_clear(void)
{
    if (g_browser_preview_cache.pixels != NULL) {
        profile_manager_free_bgra32(g_browser_preview_cache.pixels);
    }
    memset(&g_browser_preview_cache, 0, sizeof(g_browser_preview_cache));
}

static void config_menu_browser_preview_prepare(config_menu_t *menu)
{
    config_browser_entry_t entry;
    char err[80];

    if (menu == NULL ||
        menu->browser_active == 0U ||
        menu->browser_target != CONFIG_BROWSER_TARGET_PROFILE_IMAGE ||
        config_menu_browser_get_entry(menu, menu->browser_selected, &entry) != FR_OK ||
        entry.type != CONFIG_BROWSER_ENTRY_FILE) {
        config_menu_browser_preview_clear();
        return;
    }

    if (g_browser_preview_cache.loaded != 0U &&
        strcmp(g_browser_preview_cache.path, entry.path) == 0) {
        return;
    }

    config_menu_browser_preview_clear();
    g_browser_preview_cache.loaded = 1U;
    config_menu_copy_text(g_browser_preview_cache.path,
                          sizeof(g_browser_preview_cache.path),
                          entry.path);
    err[0] = '\0';
    if (profile_manager_load_thumb_bgra32(entry.path,
                                          &g_browser_preview_cache.pixels,
                                          &g_browser_preview_cache.w,
                                          &g_browser_preview_cache.h,
                                          err,
                                          sizeof(err)) == 0) {
        g_browser_preview_cache.valid = 1U;
    }
    config_menu_refresh_smartport_media_after_menu_sd(menu);
}

static void config_menu_browser_close(config_menu_t *menu)
{
    if (menu == NULL) {
        return;
    }

    menu->browser_active = 0U;
    menu->browser_target = CONFIG_BROWSER_TARGET_NONE;
    config_menu_browser_preview_clear();
}

static void config_menu_browser_set_dir(config_menu_t *menu, const char *dir)
{
    if (menu == NULL) {
        return;
    }
    config_menu_copy_text(menu->browser_dir, sizeof(menu->browser_dir),
                          (dir != NULL && dir[0] != '\0') ? dir : "0:/");
    menu->browser_selected = 0U;
    menu->browser_top = 0U;
    if (config_menu_browser_refresh(menu) != FR_OK) {
        menu->browser_count = 0U;
    }
    config_menu_refresh_smartport_media_after_menu_sd(menu);
    config_menu_browser_preview_prepare(menu);
}

static void config_menu_open_browser(config_menu_t *menu, uint8_t target)
{
    FRESULT fr;

    if (menu == NULL) {
        return;
    }

    config_menu_browser_preview_clear();
    menu->browser_active = 1U;
    menu->browser_target = target;
    menu->browser_selected = 0U;
    menu->browser_top = 0U;
    if (config_menu_browser_is_bezel_target(target) != 0U &&
        menu->bezel_path[0] != '\0') {
        config_menu_parent_path(menu->bezel_path,
                                menu->browser_dir,
                                sizeof(menu->browser_dir));
    } else if (target == CONFIG_BROWSER_TARGET_VIDEO_ROM) {
        if (menu->video_rom_path[0] != '\0') {
            config_menu_parent_path(menu->video_rom_path,
                                    menu->browser_dir,
                                    sizeof(menu->browser_dir));
        } else {
            /* Default to /ROMs; fall back to the card root if it is absent. */
            DIR roms_dir;
            if (f_opendir(&roms_dir, "0:/ROMs") == FR_OK) {
                (void)f_closedir(&roms_dir);
                config_menu_copy_text(menu->browser_dir,
                                      sizeof(menu->browser_dir), "0:/ROMs");
            } else {
                config_menu_copy_text(menu->browser_dir,
                                      sizeof(menu->browser_dir), "0:/");
            }
        }
    } else if (target == CONFIG_BROWSER_TARGET_PROFILE_IMAGE) {
        config_menu_copy_text(menu->browser_dir, sizeof(menu->browser_dir), "0:/");
    } else {
        config_menu_copy_text(menu->browser_dir, sizeof(menu->browser_dir), "0:/");
    }

    fr = config_menu_browser_refresh(menu);
    config_menu_refresh_smartport_media_after_menu_sd(menu);
    if (fr != FR_OK) {
        config_menu_browser_close(menu);
        config_menu_set_sd_error(menu, "FILE BROWSER OPEN FAILED", fr);
    } else {
        config_menu_browser_preview_prepare(menu);
    }
}

static uint8_t config_menu_browser_target_drive(uint8_t target)
{
    return (target == CONFIG_BROWSER_TARGET_DISK2_D2) ? 1U : 0U;
}

static void config_menu_browser_apply_file(config_menu_t *menu, const char *path)
{
    uint8_t drive;
    uint8_t device;

    if (menu == NULL || path == NULL) {
        return;
    }

    if (config_menu_browser_is_smartport_target(menu->browser_target) != 0U) {
        char text[CONFIG_MENU_STATUS_LEN];

        device = config_menu_browser_target_smartport_device(menu->browser_target);
        if (config_menu_smartport_path_in_use(menu, device, path) != 0U) {
            (void)snprintf(text,
                           sizeof(text),
                           "SMARTPORT SP%u DUPLICATE IMAGE",
                           (unsigned)device);
            config_menu_set_status(menu, 1U, text);
            return;
        }
        config_menu_copy_text(menu->smartport_disk_paths[device - 1U],
                              sizeof(menu->smartport_disk_paths[device - 1U]),
                              path);
        menu->smartport_slots[device - 1U] = 1U;
        config_menu_save_settings(menu);
        config_menu_reload_smartport_device(menu, device);
    } else if (config_menu_browser_is_disk2_target(menu->browser_target) != 0U) {
        drive = config_menu_browser_target_drive(menu->browser_target);
        config_menu_copy_text(menu->disk2_disk_paths[drive],
                              sizeof(menu->disk2_disk_paths[drive]),
                              path);
        menu->disk2_slots[drive][0] = 1U;
        config_menu_save_settings(menu);
        config_menu_reload_disk2_drive(menu, drive);
    } else if (config_menu_browser_is_bezel_target(menu->browser_target) != 0U) {
        config_menu_copy_text(menu->bezel_path,
                              sizeof(menu->bezel_path),
                              path);
        config_menu_save_settings(menu);
        config_menu_apply_bezel(menu);
        config_menu_save_active_profile_if_selected(menu);
        config_menu_refresh_smartport_media_after_menu_sd(menu);
    } else if (menu->browser_target == CONFIG_BROWSER_TARGET_VIDEO_ROM) {
        config_menu_copy_text(menu->video_rom_path,
                              sizeof(menu->video_rom_path),
                              path);
        config_menu_save_settings(menu);
        config_menu_apply_video_rom(menu);
        config_menu_refresh_smartport_media_after_menu_sd(menu);
    } else if (menu->browser_target == CONFIG_BROWSER_TARGET_PROFILE_IMAGE) {
        config_menu_profiles_set_image_from_png(menu, path);
    }
}

static void config_menu_browser_apply_empty(config_menu_t *menu)
{
    if (menu == NULL) {
        return;
    }

    if (config_menu_browser_is_smartport_target(menu->browser_target) != 0U) {
        config_menu_clear_smartport_device(
            menu,
            config_menu_browser_target_smartport_device(menu->browser_target));
    } else if (config_menu_browser_is_disk2_target(menu->browser_target) != 0U) {
        config_menu_clear_disk2_drive(menu,
                                      config_menu_browser_target_drive(menu->browser_target));
    } else if (config_menu_browser_is_bezel_target(menu->browser_target) != 0U) {
        menu->bezel_path[0] = '\0';
        config_menu_save_settings(menu);
        config_menu_apply_bezel(menu);
        config_menu_save_active_profile_if_selected(menu);
        config_menu_refresh_smartport_media_after_menu_sd(menu);
    } else if (menu->browser_target == CONFIG_BROWSER_TARGET_VIDEO_ROM) {
        menu->video_rom_path[0] = '\0';
        config_menu_save_settings(menu);
        config_menu_apply_video_rom(menu);   /* -> baked Enhanced US */
        config_menu_refresh_smartport_media_after_menu_sd(menu);
    } else if (menu->browser_target == CONFIG_BROWSER_TARGET_PROFILE_IMAGE) {
        config_menu_set_status(menu, 0U, "PROFILE IMAGE CANCELLED");
    }
}

static void config_menu_browser_select(config_menu_t *menu)
{
    config_browser_entry_t entry;
    FRESULT fr;

    if (menu == NULL || menu->browser_active == 0U) {
        return;
    }

    fr = config_menu_browser_get_entry(menu, menu->browser_selected, &entry);
    if (fr != FR_OK) {
        config_menu_set_sd_error(menu, "FILE BROWSER READ FAILED", fr);
        return;
    }

    if (entry.type == CONFIG_BROWSER_ENTRY_CLOSE) {
        config_menu_browser_close(menu);
    } else if (entry.type == CONFIG_BROWSER_ENTRY_EMPTY) {
        config_menu_browser_apply_empty(menu);
        config_menu_browser_close(menu);
    } else if (entry.type == CONFIG_BROWSER_ENTRY_PARENT ||
               entry.type == CONFIG_BROWSER_ENTRY_DIR) {
        config_menu_browser_set_dir(menu, entry.path);
    } else if (entry.type == CONFIG_BROWSER_ENTRY_FILE) {
        config_menu_browser_apply_file(menu, entry.path);
        config_menu_browser_close(menu);
    }
}

static void config_menu_browser_parent(config_menu_t *menu)
{
    char parent[CONFIG_MENU_PATH_LEN];

    if (menu == NULL || menu->browser_active == 0U) {
        return;
    }
    config_menu_parent_path(menu->browser_dir, parent, sizeof(parent));
    config_menu_browser_set_dir(menu, parent);
}

static uint16_t config_menu_browser_visible_rows(const config_menu_t *menu)
{
    return (menu != NULL &&
            menu->browser_target == CONFIG_BROWSER_TARGET_PROFILE_IMAGE) ?
        CONFIG_BROWSER_PROFILE_IMAGE_VISIBLE_ROWS :
        CONFIG_BROWSER_VISIBLE_ROWS;
}

static void config_menu_browser_move(config_menu_t *menu, int8_t delta)
{
    const uint16_t visible_rows = config_menu_browser_visible_rows(menu);

    if (menu == NULL || menu->browser_active == 0U) {
        return;
    }
    if (menu->browser_count == 0U) {
        return;
    }

    if (delta < 0) {
        menu->browser_selected = (menu->browser_selected == 0U) ?
            (uint16_t)(menu->browser_count - 1U) :
            (uint16_t)(menu->browser_selected - 1U);
    } else {
        menu->browser_selected = (uint16_t)((menu->browser_selected + 1U) %
                                            menu->browser_count);
    }

    if (menu->browser_selected < menu->browser_top) {
        menu->browser_top = menu->browser_selected;
    } else if (menu->browser_selected >=
               (uint16_t)(menu->browser_top + visible_rows)) {
        menu->browser_top =
            (uint16_t)(menu->browser_selected - visible_rows + 1U);
    }
    config_menu_browser_preview_prepare(menu);
}

static void config_menu_activate_item(config_menu_t *menu)
{
    uint32_t slot;

    if (menu == NULL) {
        return;
    }

    switch (menu->tab) {
    case CONFIG_TAB_BOOT_SETTINGS:
        if (menu->item_focus == 0U) {
            menu->boot_timeout_mode = (uint8_t)((menu->boot_timeout_mode + 1U) % CONFIG_BOOT_TIMEOUT_COUNT);
        } else if (menu->item_focus == 1U) {
            if (menu->disk2_slot6_enabled == 0U) {
                menu->boot_device = CONFIG_BOOT_DEVICE_SMARTPORT;
                config_menu_apply_runtime(menu);
                config_menu_save_settings(menu);
                config_menu_set_status(menu, 1U, "ENABLE DISK II TO BOOT SLOT 6");
                break;
            }
            menu->boot_device = (uint8_t)((menu->boot_device + 1U) % CONFIG_BOOT_DEVICE_COUNT);
        } else if (menu->item_focus == CONFIG_MENU_BOOT_USB_BIND_RESET_ITEM) {
            if (menu->usb_bindings_editable == 0U) {
                config_menu_set_status(menu, 1U, "USB BINDINGS EDITABLE AT BOOT");
                break;
            }
            config_menu_usb_bindings_set_defaults(menu);
            config_menu_save_settings(menu);
            config_menu_set_status(menu, 0U, "USB MENU BINDINGS RESET");
            break;
        } else if (menu->item_focus >= CONFIG_MENU_BOOT_USB_BIND_FIRST_ITEM &&
                   menu->item_focus < CONFIG_MENU_BOOT_ITEM_COUNT) {
            const uint32_t action =
                config_menu_boot_usb_binding_action_for_item(menu->item_focus);
            char text[CONFIG_MENU_STATUS_LEN];

            if (menu->usb_bindings_editable == 0U) {
                config_menu_set_status(menu, 1U, "USB BINDINGS EDITABLE AT BOOT");
                break;
            }
            if (action >= CONFIG_MENU_USB_BIND_ACTION_COUNT) {
                break;
            }
            menu->usb_binding_capture = (uint8_t)action;
            (void)snprintf(text,
                           sizeof(text),
                           "PRESS USB INPUT FOR %s",
                           config_menu_usb_binding_action_text(action));
            config_menu_set_status(menu, 0U, text);
            break;
        }
        config_menu_apply_runtime(menu);
        config_menu_save_settings(menu);
        break;

    case CONFIG_TAB_PROFILES:
        if (menu->item_focus == 0U) {
            config_menu_profiles_open_carousel(menu);
        } else if (menu->item_focus == 1U) {
            config_menu_profiles_save_to_profile(menu);
        } else if (menu->item_focus == 2U) {
            config_menu_profiles_save_as(menu);
        } else if (menu->item_focus == 3U) {
            config_menu_profiles_rename(menu);
        } else if (menu->profile_source_dir[0] == '\0') {
            config_menu_set_status(menu, 1U, "SELECT A PROFILE FIRST");
        } else {
            config_menu_open_browser(menu, CONFIG_BROWSER_TARGET_PROFILE_IMAGE);
        }
        break;

    case CONFIG_TAB_VIDEO:
        if (menu->border_flood != 0u &&
            menu->item_focus >= CONFIG_VIDEO_ITEM_SHOW_BEZEL &&
            menu->item_focus <= CONFIG_VIDEO_ITEM_DEBUG) {
            break;
        }
        if (menu->item_focus == CONFIG_VIDEO_ITEM_ROM) {
            /* Video ROM override: open the file picker (defaults to /ROMs). */
            config_menu_open_browser(menu, CONFIG_BROWSER_TARGET_VIDEO_ROM);
            break;
        }
        if (menu->item_focus == CONFIG_VIDEO_ITEM_BEZEL) {
            config_menu_open_browser(menu, CONFIG_BROWSER_TARGET_BEZEL);
            break;
        }
        if (menu->item_focus == CONFIG_VIDEO_ITEM_OUTPUT) {
            menu->video_output_mono = menu->video_output_mono ? 0U : 1U;
        } else if (menu->item_focus == CONFIG_VIDEO_ITEM_VARIANT) {
            if (menu->video_output_mono != 0U) {
                menu->video_mono_color =
                    config_menu_next_mono_color(menu->video_mono_color, 1);
            } else {
                menu->video_color_mode =
                    config_menu_next_color_mode(menu, menu->video_color_mode, 1);
            }
        } else if (menu->item_focus == CONFIG_VIDEO_ITEM_VIDEO7) {
            menu->video7_auto_mono_enabled =
                (menu->video7_auto_mono_enabled != 0U) ? 0U : 1U;
        } else if (menu->item_focus == CONFIG_VIDEO_ITEM_SCANLINES) {
            menu->scanlines_mode =
                (uint8_t)((menu->scanlines_mode + 1U) % APPLETINI_SCANLINES_COUNT);
        } else if (menu->item_focus == CONFIG_VIDEO_ITEM_GHOSTING) {
            menu->video_ghosting_strength =
                (uint8_t)((menu->video_ghosting_strength + 1U) %
                          (APPLETINI_VIDEO_GHOSTING_MAX + 1U));
        } else if (menu->item_focus == CONFIG_VIDEO_ITEM_BORDER) {
            menu->border_enabled = (menu->border_enabled != 0u) ? 0u : 1u;
        } else if (menu->item_focus == CONFIG_VIDEO_ITEM_BORDER_COLOR) {
            menu->border_color = (uint8_t)((menu->border_color + 1u) & 0x0Fu);
        } else if (menu->item_focus == CONFIG_VIDEO_ITEM_BORDER_FLOOD) {
            menu->border_flood = (menu->border_flood != 0u) ? 0u : 1u;
        } else if (menu->item_focus == CONFIG_VIDEO_ITEM_SHOW_BEZEL) {
            menu->show_bezel = (menu->show_bezel != 0U) ? 0U : 1U;
            config_menu_apply_runtime(menu);
            config_menu_save_settings(menu);
            config_menu_save_active_profile_if_selected(menu);
            break;
        } else if (menu->item_focus == CONFIG_VIDEO_ITEM_DEBUG) {
            menu->show_debugging = (menu->show_debugging != 0U) ? 0U : 1U;
        }
        config_menu_apply_runtime(menu);
        config_menu_save_settings(menu);
        break;

    case CONFIG_TAB_SMARTPORT:
        if (menu->item_focus == 0U) {
            if (menu->supersprite_enabled != 0U) {
                config_menu_set_status(menu, 1U, "SUPERSPRITE DISABLES SMARTPORT");
                break;
            }
            menu->disk2_activity_visible =
                (menu->disk2_activity_visible != 0U) ? 0U : 1U;
            config_menu_save_settings(menu);
            break;
        }
        if (menu->item_focus == SMARTPORT_DEVICE_COUNT + 2U) {
            menu->supersprite_enabled = menu->supersprite_enabled ? 0U : 1U;
            if (menu->platform.set_supersprite_enabled != NULL) {
                menu->platform.set_supersprite_enabled(menu->platform.ctx,
                                                       menu->supersprite_enabled);
            }
            config_menu_save_settings(menu);
            config_menu_set_status(menu, menu->supersprite_enabled,
                                   menu->supersprite_enabled ?
                                       "SUPERSPRITE ON - SMARTPORT DISABLED" :
                                       "SUPERSPRITE OFF - SMARTPORT RESTORED");
            break;
        }
        if (menu->supersprite_enabled != 0U) {
            config_menu_set_status(menu, 1U, "SUPERSPRITE DISABLES SMARTPORT");
            break;
        }
        if (menu->item_focus == SMARTPORT_DEVICE_COUNT + 1U) {
            menu->sp_ramdisk_enabled =
                menu->sp_ramdisk_enabled ? 0U : 1U;
            config_menu_set_status(menu, 0U, menu->sp_ramdisk_enabled
                ? "RAM32 32MB RAM DISK ON (VOLATILE)"
                : "RAM DISK OFF - CONTENTS DROPPED");
            config_menu_save_settings(menu);
            break;
        }
        slot = menu->item_focus - 1U;
        if (slot < SMARTPORT_DEVICE_COUNT) {
            config_menu_open_browser(menu,
                                     (uint8_t)(CONFIG_BROWSER_TARGET_SMARTPORT_1 + slot));
        }
        break;

    case CONFIG_TAB_DISK2:
        if (menu->item_focus == 0U) {
            menu->disk2_slot6_enabled = menu->disk2_slot6_enabled ? 0U : 1U;
            config_menu_coerce_boot_device(menu);
            config_menu_apply_runtime(menu);
            config_menu_save_settings(menu);
        } else if (menu->item_focus == 1U) {
            menu->disk2_activity_visible =
                (menu->disk2_activity_visible != 0U) ? 0U : 1U;
            config_menu_save_settings(menu);
        } else if (menu->item_focus == 2U) {
            config_menu_open_browser(menu, CONFIG_BROWSER_TARGET_DISK2_D1);
        } else if (menu->item_focus == 3U) {
            config_menu_open_browser(menu, CONFIG_BROWSER_TARGET_DISK2_D2);
        } else {
            menu->disk2_sound_volume = CONFIG_DEFAULT_DISK2_SOUND_VOLUME;
            config_menu_apply_disk2_sound(menu);
            config_menu_save_settings(menu);
        }
        break;

    case CONFIG_TAB_MOUSE:
        if (menu->item_focus == 0U) {
            menu->mouse_slot2_enabled = menu->mouse_slot2_enabled ? 0U : 1U;
            if (menu->platform.set_slot_enabled != NULL) {
                menu->platform.set_slot_enabled(menu->platform.ctx,
                                                MOUSE_CONTROL_SLOT,
                                                menu->mouse_slot2_enabled);
            }
        } else if (menu->item_focus == 1U) {
            menu->mouse_sensitivity = CONFIG_DEFAULT_MOUSE_SENSITIVITY;
            if (menu->platform.set_mouse_sensitivity != NULL) {
                menu->platform.set_mouse_sensitivity(menu->platform.ctx,
                                                     menu->mouse_sensitivity);
            }
        }
        config_menu_save_settings(menu);
        break;

    case CONFIG_TAB_MOCKINGBOARD:
        config_menu_phasor_activate(menu);
        break;

    case CONFIG_TAB_ETHERNET:
        switch (menu->item_focus) {
        case CONFIG_ETHERNET_ITEM_SLOT:
            config_menu_ethernet_toggle_slot(menu);
            break;
        case CONFIG_ETHERNET_ITEM_CONFIG_ENABLED:
            config_menu_ethernet_toggle_saved_config(menu);
            break;
        case CONFIG_ETHERNET_ITEM_MODE:
            config_menu_ethernet_toggle_mode(menu);
            break;
        case CONFIG_ETHERNET_ITEM_MAC:
        case CONFIG_ETHERNET_ITEM_IP:
        case CONFIG_ETHERNET_ITEM_SUBNET:
        case CONFIG_ETHERNET_ITEM_GATEWAY:
            config_menu_ethernet_cycle_edit_index(menu);
            break;
        case CONFIG_ETHERNET_ITEM_READ:
        case CONFIG_ETHERNET_ITEM_WRITE:
        case CONFIG_ETHERNET_ITEM_DHCP:
        case CONFIG_ETHERNET_ITEM_TEST:
            if (menu->item_focus == CONFIG_ETHERNET_ITEM_READ) {
                config_menu_ethernet_read_from_card(menu);
            } else if (menu->item_focus == CONFIG_ETHERNET_ITEM_WRITE) {
                config_menu_ethernet_write_to_card(menu);
            } else if (menu->item_focus == CONFIG_ETHERNET_ITEM_DHCP) {
                config_menu_ethernet_dhcp(menu);
            } else {
                config_menu_ethernet_test(menu);
            }
            break;
        default:
            break;
        }
        break;

    case CONFIG_TAB_CLOCK:
        config_menu_clock_clamp(menu);
        if (menu->item_focus == 0U) {
            menu->clock_enabled = menu->clock_enabled ? 0U : 1U;
            if (menu->platform.set_clock_enabled != NULL) {
                menu->platform.set_clock_enabled(menu->platform.ctx, menu->clock_enabled);
            }
            config_menu_save_settings(menu);
        } else if (menu->item_focus == 1U) {
            config_menu_try_read_rtc(menu, 1U);
        } else if (menu->item_focus == 2U) {
            (void)config_menu_adjust_clock_field(menu, 1);
        } else if (menu->item_focus == 3U) {
            (void)config_menu_adjust_clock_field(menu, 1);
        } else if (menu->item_focus == 4U) {
            (void)config_menu_adjust_clock_field(menu, 1);
        } else if (menu->item_focus == 5U) {
            (void)config_menu_adjust_clock_field(menu, 1);
        } else if (menu->item_focus == 6U) {
            (void)config_menu_adjust_clock_field(menu, 1);
        } else if (menu->item_focus == 7U) {
            (void)config_menu_adjust_clock_field(menu, 1);
        } else if (menu->platform.write_rtc != NULL &&
                   menu->platform.write_rtc(menu->platform.ctx, &menu->clock_time) == 0) {
            config_menu_set_status(menu, 0U, "WROTE PCF8563 CLOCK");
        } else {
            config_menu_set_status(menu, 1U, "RTC WRITE FAILED");
        }
        config_menu_clock_clamp(menu);
        break;

    case CONFIG_TAB_RAM:
        if (menu->usb_owned != 0U) {
            config_menu_set_status(menu, 1U,
                "RAM CAN ONLY CHANGE FROM BOOT MENU");
            break;
        }
        if (boot_menu_service_aux_card_present() != 0U) {
            config_menu_set_status(menu, 1U,
                "PHYSICAL AUX CARD DETECTED - APPLETINI RAM STAYS OFF");
            break;
        }
        /* One switch, both features: 64K aux + the 8MB RamWorks
         * expansion move together. */
        menu->ram_enabled = menu->ram_enabled ? 0U : 1U;
        menu->ramworks_enabled = menu->ram_enabled;
        config_menu_set_status(menu, 0U, menu->ram_enabled
            ? "APPLETINI PROVIDES 64K AUX + 8MB RAMWORKS"
            : "APPLETINI RAM OFF");
        config_menu_save_settings(menu);
        break;

    case CONFIG_TAB_APPLICARD:
        if (menu->item_focus == 0U) {
            config_menu_set_applicard_enabled(menu,
                menu->applicard_slot5_enabled ? 0U : 1U);
        } else if (menu->item_focus == 1U) {
            menu->applicard_resource_max =
                menu->applicard_resource_max ? 0U : 1U;
            if (menu->platform.set_applicard_resource_max != NULL) {
                menu->platform.set_applicard_resource_max(
                    menu->platform.ctx, menu->applicard_resource_max);
            }
            config_menu_save_settings(menu);
            config_menu_set_status(menu, 0U,
                menu->applicard_resource_max != 0U ?
                    "Z80 RESOURCE USAGE: MAXIMUM" :
                    "Z80 RESOURCE USAGE: STANDARD");
        }
        break;

    case CONFIG_TAB_USB:
        if (menu->item_focus == 0U) {
            config_menu_start_usb0_sd_remote(menu);
        } else if (menu->item_focus == 1U) {
            config_menu_set_sdd_stream(menu, menu->sdd_stream_enabled ? 0U : 1U);
        }
        break;

    case CONFIG_TAB_ABOUT:
        break;

    default:
        break;
    }
}

void config_menu_init(config_menu_t *menu)
{
    if (menu == NULL) {
        return;
    }

    memset(menu, 0, sizeof(*menu));
    menu->boot_timeout_mode = CONFIG_DEFAULT_BOOT_TIMEOUT_MODE;
    menu->boot_device = CONFIG_DEFAULT_BOOT_DEVICE;
    menu->scanlines_mode = CONFIG_DEFAULT_SCANLINES_MODE;
    menu->video_output_mono = CONFIG_DEFAULT_VIDEO_OUTPUT_MONO;
    menu->video_mono_color = CONFIG_DEFAULT_VIDEO_MONO_COLOR;
    menu->video_color_mode = CONFIG_DEFAULT_VIDEO_COLOR_MODE;
    menu->video7_auto_mono_enabled = CONFIG_DEFAULT_VIDEO7_AUTO_MONO_ENABLED;
    menu->video_ghosting_strength = CONFIG_DEFAULT_VIDEO_GHOSTING_STRENGTH;
    menu->border_enabled = CONFIG_DEFAULT_BORDER_ENABLED;
    menu->border_color = CONFIG_DEFAULT_BORDER_COLOR;
    menu->border_flood = CONFIG_DEFAULT_BORDER_FLOOD;
    menu->clean_video_phase_cycles = CONFIG_DEFAULT_CLEAN_VIDEO_PHASE_CYCLES;
    menu->pal_video_phase_cycles = CONFIG_DEFAULT_PAL_VIDEO_PHASE_CYCLES;
    menu->show_debugging = CONFIG_DEFAULT_SHOW_DEBUGGING;
    menu->show_bezel = CONFIG_DEFAULT_SHOW_BEZEL;
    menu->bezel_path[0] = '\0';
    for (uint8_t device = 1U; device <= SMARTPORT_DEVICE_COUNT; ++device) {
        config_menu_default_smartport_path(device,
                                           menu->smartport_disk_paths[device - 1U],
                                           sizeof(menu->smartport_disk_paths[device - 1U]));
    }
    menu->smartport_slots[0] = CONFIG_DEFAULT_SMARTPORT_DISK1_ENABLED;
    menu->disk2_slot6_enabled = CONFIG_DEFAULT_DISK2_SLOT6_ENABLED;
    menu->applicard_slot5_enabled = CONFIG_DEFAULT_APPLICARD_SLOT5_ENABLED;
    menu->applicard_resource_max = 0U;
    menu->disk2_activity_visible = CONFIG_DEFAULT_DISK2_ACTIVITY_VISIBLE;
    menu->disk2_sound_volume = CONFIG_DEFAULT_DISK2_SOUND_VOLUME;
    menu->mouse_slot2_enabled = CONFIG_DEFAULT_MOUSE_SLOT2_ENABLED;
    menu->mouse_sensitivity = CONFIG_DEFAULT_MOUSE_SENSITIVITY;
    menu->supersprite_enabled = 0U;
    menu->sdd_stream_enabled = 0U;
    menu->usb0_sd_remote_active = 0U;
    config_menu_usb_bindings_set_defaults(menu);
    menu->usb_bindings_editable = 1U;
    menu->usb_owned = 0U;
    menu->mockingboard_slot4_enabled = CONFIG_DEFAULT_MOCKINGBOARD_SLOT4_ENABLED;
    config_menu_phasor_set_defaults(menu);
    menu->ethernet_slot1_enabled = CONFIG_DEFAULT_ETHERNET_SLOT1_ENABLED;
    menu->ethernet_config_enabled = CONFIG_DEFAULT_ETHERNET_CONFIG_ENABLED;
    menu->ethernet_address_mode = CONFIG_DEFAULT_ETHERNET_ADDRESS_MODE;
    menu->ethernet_edit_index = 0U;
    uthernet2_default_config(&menu->ethernet_config);
    menu->clock_enabled = CONFIG_DEFAULT_CLOCK_ENABLED;
    menu->ram_enabled = CONFIG_DEFAULT_RAM_ENABLED;
    menu->ramworks_enabled = menu->ram_enabled;
    menu->sp_ramdisk_enabled = CONFIG_DEFAULT_SP_RAMDISK_ENABLED;
    config_menu_clock_default(menu);
    config_menu_profiles_init(menu);
    config_menu_set_status(menu, 0U, "SESSION DEFAULTS");
}

void config_menu_bind_platform(config_menu_t *menu, const config_menu_platform_t *platform)
{
    if (menu == NULL) {
        return;
    }

    if (platform != NULL) {
        menu->platform = *platform;
    } else {
        memset(&menu->platform, 0, sizeof(menu->platform));
    }

    config_menu_load_platform_defaults(menu);
    config_menu_load_settings(menu);
    config_menu_apply_boot_runtime_internal(menu, 1U);
    (void)config_menu_apply_ethernet_config(menu, 0U);
    config_menu_apply_disk2_sound(menu);
    config_menu_apply_smartport_paths(menu);
}

uint8_t config_menu_is_active(const config_menu_t *menu)
{
    return (menu != NULL && menu->active != 0U) ? 1U : 0U;
}

uint8_t config_menu_storage_activity_page_visible(const config_menu_t *menu)
{
    if (!config_menu_is_active(menu) || menu->browser_active != 0U) {
        return 0U;
    }

    if (menu->tab == CONFIG_TAB_SMARTPORT &&
        menu->supersprite_enabled != 0U) {
        return 0U;
    }

    return (menu->tab == CONFIG_TAB_SMARTPORT ||
            menu->tab == CONFIG_TAB_DISK2) ? 1U : 0U;
}

void config_menu_set_active(config_menu_t *menu, uint8_t active)
{
    if (menu == NULL) {
        return;
    }
    menu->active = (active != 0U) ? 1U : 0U;
    if (menu->active != 0U) {
        config_menu_retry_settings_if_needed(menu);
        config_menu_refresh_smartport_media_after_menu_sd(menu);
    } else {
        config_menu_stop_usb0_sd_remote(menu);
        menu->usb_owned = 0U;
        menu->usb_binding_capture = CONFIG_MENU_USB_BIND_CAPTURE_NONE;
        config_menu_browser_close(menu);
        menu->profile_carousel_active = 0U;
        menu->profile_name_editor_active = 0U;
    }
}

void config_menu_toggle(config_menu_t *menu)
{
    if (menu == NULL) {
        return;
    }
    menu->usb_owned = 0U;
    config_menu_set_active(menu, menu->active == 0U);
}

void config_menu_set_usb_bindings_editable(config_menu_t *menu, uint8_t editable)
{
    if (menu == NULL) {
        return;
    }

    menu->usb_bindings_editable = (editable != 0U) ? 1U : 0U;
    if (menu->usb_bindings_editable == 0U) {
        menu->usb_binding_capture = CONFIG_MENU_USB_BIND_CAPTURE_NONE;
    }
}

void config_menu_set_usb_owned(config_menu_t *menu, uint8_t usb_owned)
{
    if (menu == NULL) {
        return;
    }

    menu->usb_owned = (usb_owned != 0U) ? 1U : 0U;
    if (menu->usb_owned != 0U) {
        menu->usb_binding_capture = CONFIG_MENU_USB_BIND_CAPTURE_NONE;
    }
}

uint8_t config_menu_handle_input(config_menu_t *menu, ui_input_t input)
{
    if (menu == NULL || input.pressed == 0U) {
        return 0U;
    }

    if (config_menu_is_active(menu) &&
        menu->usb0_sd_remote_active != 0U) {
        if (input.key == UI_KEY_ENTER ||
            input.key == UI_KEY_BACK ||
            input.key == UI_KEY_ESC ||
            input.key == UI_KEY_MENU) {
            config_menu_stop_usb0_sd_remote(menu);
        }
        return 1U;
    }

    if (input.key == UI_KEY_MENU) {
        config_menu_toggle(menu);
        return 1U;
    }
    if (!config_menu_is_active(menu)) {
        return 0U;
    }

    if (menu->usb_binding_capture != CONFIG_MENU_USB_BIND_CAPTURE_NONE) {
        if (input.key == UI_KEY_ESC || input.key == UI_KEY_BACK) {
            menu->usb_binding_capture = CONFIG_MENU_USB_BIND_CAPTURE_NONE;
            config_menu_set_status(menu, 0U, "USB BIND CANCELLED");
        }
        return 1U;
    }

    if (config_menu_profiles_handle_input(menu, input) != 0U) {
        return 1U;
    }

    if (menu->browser_active != 0U) {
        switch (input.key) {
        case UI_KEY_TAB:
        case UI_KEY_SHIFT_TAB:
            return 1U;
        case UI_KEY_PAGE_DOWN:
        case UI_KEY_DOWN:
            config_menu_browser_move(menu, 1);
            return 1U;
        case UI_KEY_PAGE_UP:
        case UI_KEY_UP:
            config_menu_browser_move(menu, -1);
            return 1U;
        case UI_KEY_LEFT:
            config_menu_browser_parent(menu);
            return 1U;
        case UI_KEY_BACK:
            if (config_menu_path_is_root(menu->browser_dir) != 0U) {
                config_menu_browser_close(menu);
            } else {
                config_menu_browser_parent(menu);
            }
            return 1U;
        case UI_KEY_RIGHT:
        case UI_KEY_ENTER:
            config_menu_browser_select(menu);
            return 1U;
        case UI_KEY_ESC:
            config_menu_browser_close(menu);
            return 1U;
        default:
            break;
        }
        return 1U;
    }

    switch (input.key) {
    case UI_KEY_TAB:
        config_menu_next_tab(menu);
        return 1U;
    case UI_KEY_SHIFT_TAB:
        config_menu_prev_tab(menu);
        return 1U;
    case UI_KEY_PAGE_UP:
    case UI_KEY_UP:
        config_menu_prev_item(menu);
        return 1U;
    case UI_KEY_PAGE_DOWN:
    case UI_KEY_DOWN:
        config_menu_next_item(menu);
        return 1U;
    case UI_KEY_LEFT:
        (void)config_menu_adjust_focused_value(menu, -1);
        return 1U;
    case UI_KEY_RIGHT:
        (void)config_menu_adjust_focused_value(menu, 1);
        return 1U;
    case UI_KEY_ENTER:
        config_menu_activate_item(menu);
        return 1U;
    case UI_KEY_ESC:
    case UI_KEY_BACK:
        config_menu_set_active(menu, 0U);
        return 1U;
    default:
        break;
    }

    return 0U;
}

static int hgr_px(int v)
{
    return v * HGR_SCALE;
}

static void hgr_fill_rect(uint16_t *fb, int x, int y, int w, int h, uint32_t color)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    fb16_fill_rect(fb,
                   HGR_X + hgr_px(x),
                   HGR_Y + hgr_px(y),
                   hgr_px(w),
                   hgr_px(h),
                   color);
}

void hgr_focus_band(uint16_t *fb, int x, int y, int w, uint8_t focused)
{
    hgr_fill_rect(fb, x, y, w, CMUI_ROW_H, focused ? CMUI_COLOR_ROW_ACTIVE :
                  CMUI_COLOR_ROW);
    if (focused != 0U) {
        hgr_fill_rect(fb, x, y, 5, CMUI_ROW_H, CMUI_COLOR_ACCENT);
        fb16_rect(fb, x, y, w, CMUI_ROW_H, CMUI_COLOR_BORDER);
    }
}

void hgr_draw_lock_icon(uint16_t *fb,
                        int x,
                        int y,
                        uint8_t locked,
                        uint8_t focused,
                        uint32_t color)
{
    const uint32_t bg = focused ? CMUI_COLOR_ROW_ACTIVE : CMUI_COLOR_ROW;

    (void)color;
    if (locked == 0U) {
        return;
    }
    cmui_lock(fb, x, y, locked, focused, bg);
}

static void hgr_draw_item_with_lock_ex(uint16_t *fb,
                                       int x,
                                       int y,
                                       int w,
                                       uint8_t focused,
                                       const char *text,
                                       uint32_t color,
                                       uint8_t show_lock,
                                       uint8_t locked,
                                       uint8_t dimmed)
{
    const uint32_t bg = (dimmed != 0U) ? CMUI_COLOR_ROW_DISABLED :
                        (focused ? CMUI_COLOR_ROW_ACTIVE : CMUI_COLOR_ROW);
    int text_x = x + 2;

    if (show_lock != 0U) {
        fb16_fill_rect(fb, x, y, w, CMUI_ROW_H, bg);
        if (focused != 0U) {
            fb16_fill_rect(fb, x, y, 5, CMUI_ROW_H,
                           (dimmed != 0U) ? CMUI_COLOR_DISABLED_EDGE :
                           CMUI_COLOR_ACCENT);
            fb16_rect(fb, x, y, w, CMUI_ROW_H, CMUI_COLOR_BORDER);
        } else if (dimmed != 0U) {
            fb16_fill_rect(fb, x, y, 5, CMUI_ROW_H, CMUI_COLOR_DISABLED_EDGE);
        }
        hgr_draw_lock_icon(fb, x + 16, y, locked, focused, color);
        text_x = x + 46;
        cmui_text_clipped(fb,
                          text_x,
                          y + 11,
                          w - (text_x - x) - 18,
                          text,
                          (dimmed != 0U) ? CMUI_COLOR_DIM :
                          (focused ? CMUI_COLOR_TEXT : color),
                          bg,
                          CMUI_BODY_SCALE);
        return;
    }
    cmui_row(fb, x, y, w, focused, dimmed, text);
}

static void hgr_draw_item_ex(uint16_t *fb,
                             int x,
                             int y,
                             int w,
                             uint8_t focused,
                             const char *text,
                             uint32_t color,
                             uint8_t dimmed)
{
    hgr_draw_item_with_lock_ex(fb,
                               x,
                               y,
                               w,
                               focused,
                               text,
                               color,
                               0U,
                               0U,
                               dimmed);
}

void hgr_draw_item(uint16_t *fb,
                   int x,
                   int y,
                   int w,
                   uint8_t focused,
                   const char *text,
                   uint32_t color)
{
    hgr_draw_item_ex(fb,
                     x,
                     y,
                     w,
                     focused,
                     text,
                     color,
                     (uint8_t)(color == HGR_DIMMED));
}

void hgr_draw_item_dimmed(uint16_t *fb,
                          int x,
                          int y,
                          int w,
                          uint8_t focused,
                          const char *text)
{
    hgr_draw_item_ex(fb, x, y, w, focused, text, HGR_WHITE, 1U);
}

void hgr_draw_item_with_lock(uint16_t *fb,
                             int x,
                             int y,
                             int w,
                             uint8_t focused,
                             const char *text,
                             uint32_t color,
                             uint8_t locked)
{
    hgr_draw_item_with_lock_ex(fb,
                               x,
                               y,
                               w,
                               focused,
                               text,
                               color,
                               1U,
                               locked,
                               0U);
}

void hgr_draw_check_item(uint16_t *fb,
                         int x,
                         int y,
                         int w,
                         uint8_t focused,
                         uint8_t checked,
                         const char *text)
{
    cmui_check_row(fb, x, y, w, focused, checked, text);
}

void hgr_draw_check_item_dimmed(uint16_t *fb,
                                int x,
                                int y,
                                int w,
                                uint8_t focused,
                                uint8_t checked,
                                const char *text)
{
    cmui_check_row_ex(fb, x, y, w, focused, checked, 1U, text);
}

void hgr_draw_value_item(uint16_t *fb,
                         int x,
                         int y,
                         int w,
                         uint8_t focused,
                         const char *label,
                         const char *value)
{
    cmui_value_row(fb, x, y, w, focused, 0U, label, value);
}

void hgr_draw_value_item_dimmed(uint16_t *fb,
                                int x,
                                int y,
                                int w,
                                uint8_t focused,
                                const char *label,
                                const char *value)
{
    cmui_value_row(fb, x, y, w, focused, 1U, label, value);
}

void hgr_draw_mouse_sensitivity_item(uint16_t *fb,
                                     int x,
                                     int y,
                                     int w,
                                     uint8_t focused,
                                     uint8_t sensitivity)
{
    char value[8];
    uint32_t index;

    sensitivity = config_menu_mouse_sensitivity_clamp(sensitivity);
    index = config_menu_mouse_sensitivity_index(sensitivity);
    (void)snprintf(value, sizeof(value), "%u%%", (unsigned)sensitivity);
    cmui_slider(fb,
                x,
                y,
                w,
                focused,
                0U,
                "Sensitivity",
                "3",
                "150",
                index,
                MOUSE_SENSITIVITY_STEP_COUNT - 1U,
                MOUSE_SENSITIVITY_DEFAULT_INDEX,
                value);
}

void hgr_draw_video_ghosting_item(uint16_t *fb,
                                  int x,
                                  int y,
                                  int w,
                                  uint8_t focused,
                                  uint8_t strength)
{
    const uint32_t bg = (focused != 0U) ? CMUI_COLOR_ROW_ACTIVE : CMUI_COLOR_ROW;
    const uint32_t label_fg = (focused != 0U) ? CMUI_COLOR_TEXT :
                              CMUI_COLOR_MUTED;
    const uint32_t value_fg = (focused != 0U) ? CMUI_COLOR_ACCENT :
                              CMUI_COLOR_TEXT;
    const int label_w = (w >= 900) ? CMUI_VALUE_LABEL_W : ((w * 46) / 100);
    const int value_x = x + 18 + label_w + 30;
    const int value_w = w - label_w - 48;
    const char *value;
    const int value_text_w = cmui_text_width(
        appletini_video_ghosting_name(strength), CMUI_BODY_SCALE);
    const int warning_x = value_x + value_text_w + 38;
    const int warning_w = (x + w - 18) - warning_x;

    if (w <= 0) {
        return;
    }
    strength = appletini_video_ghosting_clamp(strength);
    value = appletini_video_ghosting_name(strength);

    fb16_fill_rect(fb, x, y, w, CMUI_ROW_H, bg);
    if (focused != 0U) {
        fb16_fill_rect(fb, x, y, 5, CMUI_ROW_H, CMUI_COLOR_ACCENT);
        fb16_rect(fb, x, y, w, CMUI_ROW_H, CMUI_COLOR_BORDER);
    }
    cmui_text_clipped(fb, x + 18, y + 9, label_w, "Phosphor ghosting",
                      label_fg, bg, CMUI_BODY_SCALE);
    cmui_text_clipped(fb, value_x, y + 9, value_w, value, value_fg, bg,
                      CMUI_BODY_SCALE);
    if ((warning_w > 0) && (strength > APPLETINI_VIDEO_GHOSTING_OFF)) {
        cmui_text_clipped(fb, warning_x, y + 9, warning_w, "(lower FPS)",
                          CMUI_COLOR_WARN, bg, CMUI_BODY_SCALE);
    }
}

static void config_menu_draw_tabs(uint16_t *fb, const config_menu_t *menu,
                                  int x, int y, int w)
{
    uint32_t i;

    if (menu == NULL) {
        return;
    }

    for (i = 0U; i < CONFIG_TAB_COUNT; ++i) {
        const uint8_t active = (uint8_t)(menu->tab == i);
        cmui_rect_t row = {
            x,
            y + (int)(i * (CMUI_ROW_H + 8)),
            w,
            CMUI_ROW_H
        };

        cmui_nav_item(fb, &row, k_tab_labels[i], active, 1U);
    }
}

static void config_menu_draw_about_section(uint16_t *fb,
                                           int x,
                                           int *y,
                                           int w,
                                           int bottom,
                                           const char *title,
                                           const char * const *lines,
                                           uint32_t line_count)
{
    const int line_h = 28;

    if (y == NULL || *y >= bottom) {
        return;
    }

    cmui_text(fb, x, *y, title, CMUI_COLOR_ACCENT, CMUI_COLOR_BG,
              CMUI_BODY_SCALE);
    *y += 38;

    for (uint32_t i = 0U; i < line_count; ++i) {
        if (*y + FB16_BUILTIN_FONT_HEIGHT > bottom) {
            break;
        }
        cmui_text_clipped(fb,
                          x + 18,
                          *y,
                          w - 36,
                          lines[i],
                          CMUI_COLOR_TEXT,
                          CMUI_COLOR_BG,
                          CMUI_SMALL_SCALE);
        *y += line_h;
    }
    *y += 18;
}

static void config_menu_draw_about_bottom_text(uint16_t *fb,
                                               int x,
                                               int y,
                                               int w,
                                               int h)
{
    const config_menu_help_block_t help =
        config_menu_help_resolve(CONFIG_TAB_ABOUT, 0U);
    const int line_h = 28;
    const int total_h = (int)help.count * line_h;
    int text_y = y + h - total_h;

    if (help.count == 0U || h <= 0) {
        return;
    }
    if (text_y < y) {
        text_y = y;
    }

    for (uint32_t i = 0U; i < help.count; ++i) {
        if (text_y + FB16_BUILTIN_FONT_HEIGHT > y + h) {
            break;
        }
        cmui_text_clipped(fb,
                          x + 18,
                          text_y,
                          w - 36,
                          help.lines[i],
                          (i == (help.count - 1U)) ?
                              CMUI_COLOR_MUTED : CMUI_COLOR_TEXT,
                          CMUI_COLOR_BG,
                          CMUI_SMALL_SCALE);
        text_y += line_h;
    }
}

static void config_menu_draw_about(uint16_t *fb,
                                   int x,
                                   int y,
                                   int w,
                                   int h)
{
    int cursor = y + 10;
    const int help_h = 92;
    const int bottom = y + h - help_h - 18;

    config_menu_draw_about_section(
        fb,
        x,
        &cursor,
        w,
        bottom,
        "Versions",
        k_about_versions,
        (uint32_t)(sizeof(k_about_versions) /
                   sizeof(k_about_versions[0])));
    config_menu_draw_about_section(
        fb,
        x,
        &cursor,
        w,
        bottom,
        "Contributors",
        k_about_contributors,
        (uint32_t)(sizeof(k_about_contributors) /
                   sizeof(k_about_contributors[0])));
    config_menu_draw_about_section(
        fb,
        x,
        &cursor,
        w,
        bottom,
        "Third-party codebases and inspiration",
        k_about_third_party,
        (uint32_t)(sizeof(k_about_third_party) /
                   sizeof(k_about_third_party[0])));
    config_menu_draw_about_bottom_text(fb, x, y, w, h);
}

/* Max lines the help panel assembles per frame: the largest centralized
 * block plus a couple of runtime-computed status lines, with headroom. */
#define CONFIG_MENU_HELP_MAX_LINES 16U

static void config_menu_draw_help(uint16_t *fb,
                                  const config_menu_t *menu,
                                  const cmui_rect_t *rect)
{
    const char *lines[CONFIG_MENU_HELP_MAX_LINES];
    config_menu_help_block_t base;
    uint32_t count = 0U;

    if (menu == NULL || rect == NULL) {
        return;
    }

    /* Static text (tab default, or a per-item override) comes from the
     * centralized table in config_menu_help.c. */
    base = config_menu_help_resolve(menu->tab, menu->item_focus);

    switch (menu->tab) {
    case CONFIG_TAB_VIDEO:
        /* Base lines, then a live note when PAL-accurate modes apply. */
        for (uint32_t i = 0U;
             i < base.count && count < CONFIG_MENU_HELP_MAX_LINES; ++i) {
            lines[count++] = base.lines[i];
        }
        if (config_menu_video_pal_accurate_help_visible(menu) != 0U &&
            count < CONFIG_MENU_HELP_MAX_LINES) {
            lines[count++] = "PAL Accurate modes do not support SHR.";
        }
        break;

    case CONFIG_TAB_CLOCK:
        /* base[0], live link-state, then the remaining base lines. */
        if (base.count > 0U) {
            lines[count++] = base.lines[0];
        }
        lines[count++] = (menu->clock_enabled != 0U) ?
            "Current state: no-slot clock link active." :
            "Current state: no-slot clock link disabled.";
        for (uint32_t i = 1U;
             i < base.count && count < CONFIG_MENU_HELP_MAX_LINES; ++i) {
            lines[count++] = base.lines[i];
        }
        break;

    default:
        /* Every other tab: show the resolved block verbatim. Per-item
         * overrides are already applied by resolve(). */
        for (uint32_t i = 0U;
             i < base.count && count < CONFIG_MENU_HELP_MAX_LINES; ++i) {
            lines[count++] = base.lines[i];
        }
        break;
    }

    cmui_help_panel(fb, rect, "Help", lines, count);
}

static void config_menu_blit_scaled_bgra(uint16_t *fb,
                                         int dst_x,
                                         int dst_y,
                                         int dst_w,
                                         int dst_h,
                                         const uint32_t *src,
                                         unsigned src_w,
                                         unsigned src_h)
{
    if (fb == NULL || src == NULL || dst_w <= 0 || dst_h <= 0 ||
        src_w == 0U || src_h == 0U) {
        return;
    }

    for (int y = 0; y < dst_h; ++y) {
        const int out_y = dst_y + y;
        const uint32_t sy = (uint32_t)(((uint64_t)(uint32_t)y * src_h) /
                                      (uint32_t)dst_h);

        if (out_y < 0 || out_y >= FB16_HEIGHT) {
            continue;
        }
        for (int x = 0; x < dst_w; ++x) {
            const int out_x = dst_x + x;
            const uint32_t sx = (uint32_t)(((uint64_t)(uint32_t)x * src_w) /
                                          (uint32_t)dst_w);

            if (out_x < 0 || out_x >= FB16_WIDTH) {
                continue;
            }
            fb[((size_t)out_y * FB16_WIDTH) + (size_t)out_x] =
                fb16_from_bgra32(src[((size_t)sy * src_w) + sx]);
        }
    }
}

static void config_menu_fit_image_rect(unsigned src_w,
                                       unsigned src_h,
                                       int max_w,
                                       int max_h,
                                       int *out_w,
                                       int *out_h)
{
    int dst_w;
    int dst_h;

    if (out_w == NULL || out_h == NULL ||
        src_w == 0U || src_h == 0U || max_w <= 0 || max_h <= 0) {
        if (out_w != NULL) {
            *out_w = 0;
        }
        if (out_h != NULL) {
            *out_h = 0;
        }
        return;
    }

    dst_w = max_w;
    dst_h = (int)(((uint64_t)src_h * (uint64_t)(uint32_t)dst_w) /
                  (uint64_t)src_w);
    if (dst_h > max_h) {
        dst_h = max_h;
        dst_w = (int)(((uint64_t)src_w * (uint64_t)(uint32_t)dst_h) /
                      (uint64_t)src_h);
    }
    if (dst_w <= 0) {
        dst_w = 1;
    }
    if (dst_h <= 0) {
        dst_h = 1;
    }

    *out_w = dst_w;
    *out_h = dst_h;
}

static void config_menu_draw_browser_preview(uint16_t *fb,
                                             const config_menu_t *menu,
                                             int x,
                                             int y,
                                             int w,
                                             int h)
{
    config_browser_entry_t entry;
    const uint8_t has_entry =
        (uint8_t)(config_menu_browser_get_entry(menu, menu->browser_selected, &entry) == FR_OK);
    const uint8_t selected_file =
        (uint8_t)(has_entry != 0U && entry.type == CONFIG_BROWSER_ENTRY_FILE);
    const uint8_t loaded =
        (uint8_t)(selected_file != 0U &&
                  g_browser_preview_cache.loaded != 0U &&
                  strcmp(g_browser_preview_cache.path, entry.path) == 0);
    const int image_x = x + CONFIG_BROWSER_PROFILE_PREVIEW_PAD_X;
    const int image_y = y + CONFIG_BROWSER_PROFILE_PREVIEW_TITLE_H;
    const int image_w = w - (2 * CONFIG_BROWSER_PROFILE_PREVIEW_PAD_X);
    const int image_h = h - CONFIG_BROWSER_PROFILE_PREVIEW_TITLE_H - 2;

    fb16_fill_rect(fb, x, y, w, h, CMUI_COLOR_PANEL_2);
    fb16_rect(fb, x, y, w, h, CMUI_COLOR_BORDER_SOFT);
    cmui_text(fb, x + 16, y + 14, "Preview", CMUI_COLOR_WARN,
              CMUI_COLOR_PANEL_2, CMUI_BODY_SCALE);

    if (selected_file == 0U) {
        cmui_text(fb, x + 16, image_y + 24, "Select PNG",
                  CMUI_COLOR_DIM, CMUI_COLOR_PANEL_2, CMUI_BODY_SCALE);
        return;
    }
    if (loaded == 0U || g_browser_preview_cache.valid == 0U ||
        g_browser_preview_cache.pixels == NULL) {
        cmui_text(fb, x + 16, image_y + 24, "No preview",
                  CMUI_COLOR_DIM, CMUI_COLOR_PANEL_2, CMUI_BODY_SCALE);
        return;
    }

    {
        const int image_fb_x = image_x;
        const int image_fb_y = image_y;
        const int image_fb_w = image_w;
        const int image_fb_h = image_h;
        int dst_w;
        int dst_h;
        int dst_x;
        int dst_y;

        config_menu_fit_image_rect(g_browser_preview_cache.w,
                                   g_browser_preview_cache.h,
                                   image_fb_w,
                                   image_fb_h,
                                   &dst_w,
                                   &dst_h);
        dst_x = image_fb_x + ((image_fb_w - dst_w) / 2);
        dst_y = image_fb_y + ((image_fb_h - dst_h) / 2);

        fb16_fill_rect(fb, image_fb_x, image_fb_y, image_fb_w, image_fb_h,
                       FB16_RGB(0x08, 0x0A, 0x0D));
        config_menu_blit_scaled_bgra(fb,
                                     dst_x,
                                     dst_y,
                                     dst_w,
                                     dst_h,
                                     g_browser_preview_cache.pixels,
                                     g_browser_preview_cache.w,
                                     g_browser_preview_cache.h);
    }
}

static void config_menu_draw_browser(uint16_t *fb,
                                     const config_menu_t *menu,
                                     int x,
                                     int y,
                                     int w)
{
    const uint8_t show_preview =
        (uint8_t)(menu != NULL &&
                  menu->browser_target == CONFIG_BROWSER_TARGET_PROFILE_IMAGE);
    const int box_y = y;
    const int box_bottom = CMUI_SCREEN_H - CMUI_MARGIN_Y - CMUI_FOOTER_H - 24;
    const int box_h = box_bottom - box_y;
    const int row_y = y + 108;
    const int row_h = CMUI_ROW_H + CMUI_ROW_GAP;
    const uint16_t visible_rows = config_menu_browser_visible_rows(menu);
    const int preview_w = show_preview ? 360 : 0;
    const int preview_gap = show_preview ? 24 : 0;
    const int preview_h = show_preview ? 320 : 0;
    const int list_w = w - preview_w - preview_gap;
    char line[180];

    if (menu == NULL || menu->browser_active == 0U) {
        return;
    }

    fb16_fill_rect(fb, x - 20, y - 18, w + 40, box_h + 36,
                   CMUI_COLOR_PANEL);
    fb16_rect(fb, x - 20, y - 18, w + 40, box_h + 36,
              CMUI_COLOR_BORDER);
    cmui_text(fb, x, y + 10, config_menu_browser_title(menu->browser_target),
              CMUI_COLOR_WARN, CMUI_COLOR_PANEL, CMUI_TITLE_SCALE);
    cmui_text_clipped(fb, x, y + 58, w - 300, menu->browser_dir,
                      CMUI_COLOR_MUTED, CMUI_COLOR_PANEL, CMUI_SMALL_SCALE);

    (void)snprintf(line,
                   sizeof(line),
                   "Item %u/%u",
                   (unsigned)((menu->browser_count == 0U) ? 0U : (menu->browser_selected + 1U)),
                   (unsigned)menu->browser_count);
    cmui_text_clipped(fb, x + w - 280, y + 24, 270, line,
                      CMUI_COLOR_TEXT, CMUI_COLOR_PANEL, CMUI_SMALL_SCALE);

    for (uint16_t row = 0U; row < visible_rows; ++row) {
        const uint16_t index = (uint16_t)(menu->browser_top + row);
        config_browser_entry_t entry;
        uint8_t focused;
        uint8_t dimmed;
        uint32_t color;

        if (index >= menu->browser_count) {
            break;
        }
        if (config_menu_browser_get_entry(menu, index, &entry) != FR_OK) {
            break;
        }

        if (entry.type == CONFIG_BROWSER_ENTRY_DIR) {
            (void)snprintf(line, sizeof(line), "[DIR] %.128s", entry.name);
        } else if (entry.type == CONFIG_BROWSER_ENTRY_PARENT) {
            (void)snprintf(line, sizeof(line), "%.128s", entry.name);
        } else {
            (void)snprintf(line, sizeof(line), "%.128s", entry.name);
        }
        focused = (uint8_t)(index == menu->browser_selected);
        dimmed = config_menu_browser_entry_is_duplicate_smartport_image(menu, &entry);
        color = (entry.type == CONFIG_BROWSER_ENTRY_EMPTY) ? HGR_ORANGE :
                ((entry.type == CONFIG_BROWSER_ENTRY_CLOSE) ? HGR_GREEN : HGR_WHITE);
        hgr_draw_item_with_lock_ex(
            fb,
            x,
            row_y + ((int)row * row_h),
            list_w,
            focused,
            line,
            color,
            (uint8_t)(config_menu_browser_is_disk2_target(menu->browser_target) != 0U &&
                      entry.type == CONFIG_BROWSER_ENTRY_FILE),
            entry.read_only,
            dimmed);
    }

    if (show_preview != 0U) {
        config_menu_draw_browser_preview(fb,
                                         menu,
                                         x + list_w + preview_gap,
                                         row_y,
                                         preview_w,
                                         preview_h);
    }
}

static void config_menu_draw_usb0_sd_remote_modal(uint16_t *fb,
                                                  const cmui_rect_t *body,
                                                  uint8_t usb_owned)
{
    static const char * const lines[] = {
        "USB0 is exposing the SD card to the host computer.",
        "Appletini is servicing the USB mass-storage bridge only.",
        "Eject the disk on the host before exiting this mode."
    };
    cmui_rect_t panel;

    if (fb == NULL || body == NULL) {
        return;
    }

    panel.x = body->x;
    panel.y = body->y + 56;
    panel.w = body->w;
    panel.h = 280;
    cmui_panel(fb, &panel, CMUI_COLOR_PANEL);
    cmui_text(fb,
              panel.x + 28,
              panel.y + 28,
              "SD Card Remote Mounting",
              CMUI_COLOR_WARN,
              CMUI_COLOR_PANEL,
              CMUI_TITLE_SCALE);
    for (uint32_t i = 0U; i < (sizeof(lines) / sizeof(lines[0])); ++i) {
        cmui_text_clipped(fb,
                          panel.x + 28,
                          panel.y + 88 + ((int)i * 38),
                          panel.w - 56,
                          lines[i],
                          CMUI_COLOR_TEXT,
                          CMUI_COLOR_PANEL,
                          CMUI_BODY_SCALE);
    }
    // last line gives the response keys depending on whether USB is owned or not
    char* keysRespond = (usb_owned != 0U) ? "Press USB OK, BACK or MENU to detach USB0." :
        "Press Enter, Esc, Back, or Menu to detach USB0.";
    cmui_text_clipped(fb,
                        panel.x + 28,
                        panel.y + 88 + ((int)(sizeof(lines) / sizeof(lines[0])) * 38),
                        panel.w - 56,
                        keysRespond,
                        CMUI_COLOR_ACCENT,
                        CMUI_COLOR_PANEL,
                        CMUI_BODY_SCALE);
    }

static void config_menu_draw_page(uint16_t *fb, const config_menu_t *menu,
                                  int x, int y, int w, int h)
{
    int help_h;
    cmui_rect_t help;
    int content_h;

    if (menu == NULL) {
        return;
    }
    help_h = (h > (CONFIG_MENU_HELP_H + 160)) ?
        CONFIG_MENU_HELP_H : (h / 3);
    help.x = x;
    help.y = y + h - help_h;
    help.w = w;
    help.h = help_h;
    content_h = (menu->tab == CONFIG_TAB_ABOUT) ?
        h : (h - help_h - CONFIG_MENU_HELP_GAP);
    if (content_h < 0) {
        content_h = 0;
    }

    switch (menu->tab) {
    case CONFIG_TAB_BOOT_SETTINGS:
        config_menu_draw_boot_settings(fb, menu, x, y, w);
        break;
    case CONFIG_TAB_PROFILES:
        config_menu_profiles_draw(fb, menu, x, y, w);
        break;
    case CONFIG_TAB_VIDEO:
        config_menu_draw_video(fb, menu, x, y, w);
        break;
    case CONFIG_TAB_SMARTPORT:
        config_menu_draw_smartport(fb, menu, x, y, w);
        break;
    case CONFIG_TAB_DISK2:
        config_menu_draw_disk2(fb, menu, x, y, w);
        break;
    case CONFIG_TAB_MOUSE:
        config_menu_draw_mouse(fb, menu, x, y, w);
        break;
    case CONFIG_TAB_MOCKINGBOARD:
        config_menu_phasor_draw(fb, menu, x, y, w);
        break;
    case CONFIG_TAB_ETHERNET:
        config_menu_draw_ethernet(fb, menu, x, y, w);
        break;
    case CONFIG_TAB_APPLICARD:
        config_menu_draw_applicard(fb, menu, x, y, w);
        break;
    case CONFIG_TAB_CLOCK:
        config_menu_draw_clock(fb, menu, x, y, w);
        break;
    case CONFIG_TAB_RAM:
        config_menu_draw_ram(fb, menu, x, y, w);
        break;
    case CONFIG_TAB_USB:
        config_menu_draw_usb(fb, menu, x, y, w);
        break;
    case CONFIG_TAB_ABOUT:
        config_menu_draw_about(fb, x, y, w, content_h);
        break;
    default:
        break;
    }

    if (menu->tab != CONFIG_TAB_ABOUT) {
        config_menu_draw_help(fb, menu, &help);
    }
}

void config_menu_draw(uint16_t *fb, const config_menu_t *menu, uint8_t usb_owned)
{
    cmui_rect_t nav;
    cmui_rect_t body;
    cmui_rect_t footer;

    if (!config_menu_is_active(menu)) {
        return;
    }

    cmui_screen_rects(&nav, &body, &footer);
    cmui_clear(fb);
    cmui_header(fb,
                "Appletini",
                APPLETINI_FIRMWARE_IMAGE_VERSION_FULL,
                usb_owned);
    fb16_fill_rect(fb,
                   nav.x + nav.w + (CMUI_NAV_GAP / 2),
                   nav.y,
                   1,
                   nav.h,
                   CMUI_COLOR_BORDER_SOFT);

    if (menu->usb0_sd_remote_active != 0U) {
        config_menu_draw_usb0_sd_remote_modal(fb, &body, usb_owned);
    } else {
        config_menu_draw_tabs(fb, menu, nav.x, nav.y, nav.w);
        config_menu_draw_page(fb, menu, body.x, body.y, body.w, body.h);
    }
    cmui_footer(fb, &footer, menu->status, menu->status_warning, usb_owned);
    if (menu->usb0_sd_remote_active == 0U) {
        config_menu_draw_browser(fb, menu, body.x, body.y - 4, body.w);
    }
}
