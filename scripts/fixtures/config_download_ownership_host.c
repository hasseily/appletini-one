/* The production helpers below run unchanged. Registering any FATFS while
 * downloading would invalidate the mock live file, so every mount is counted. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int FRESULT;
typedef unsigned char BYTE;
typedef struct { int unused; } FATFS;
typedef struct { int unused; } FIL;
typedef struct { uint8_t ip[4]; } uthernet2_network_config_t;
enum { FR_OK, FR_DISK_ERR, FR_NO_FILE, FR_DENIED, FR_NOT_ENABLED,
       FR_LOCKED, FR_INVALID_OBJECT, FR_WRITE_PROTECTED, FR_NOT_READY };
#define CONFIG_SMARTPORT_ALL_DEVICES 255U
#define CONFIG_MENU_STATUS_LEN 96U
#define CONFIG_MENU_ETHERNET_ADDRESS_DHCP 1U
#define CONFIG_MENU_ONEE_MODE_LOCKED 2U
#define CONFIG_TAB_BOOT_SETTINGS 1U
#define CONFIG_TAB_ABOUT 2U
#define CONFIG_MENU_BOOT_ONEE_STANDARD_ITEM 5U
#define CONFIG_MENU_BOOT_ONEE_ITEM 2U
#define CONFIG_MENU_USB_BIND_CAPTURE_NONE 255U
#define ONEE_PERSIST_RETRY_POLL_LIMIT 4096U
#define APPLETINI_CFG_PATH "0:/appletini_cfg.txt"
typedef struct {
    void *ctx;
    int (*reset_smartport_media)(void *, uint8_t);
    void (*ethernet_dhcp_cancel)(void *);
    int (*ethernet_dhcp_poll)(void *, uthernet2_network_config_t *, char *, size_t);
    uint8_t (*get_onee_mode_state)(void *);
    uint32_t (*get_onee_mode_status)(void *);
    uint8_t (*get_onee_mode_persist_update)(void *, uint8_t *);
    void (*ack_onee_mode_persist_update)(void *, uint8_t);
} platform_t;
typedef struct {
    platform_t platform;
    uint8_t active, settings_loaded, ethernet_dhcp_pending, ethernet_dhcp_report;
    uint8_t ethernet_slot1_enabled, ethernet_config_enabled, ethernet_address_mode;
    uint8_t session_only, onee_mode_state, onee_persisted_enabled;
    uint32_t onee_mode_status, onee_persist_retry_polls;
    uint8_t tab, item_focus, ethernet_ftp_sd_remote_active, onee_persist_write_failed;
    uint8_t golden_boot_update_confirm, golden_boot_confirm_update;
    uint8_t joystick_page_active, usb_owned, usb_binding_capture;
    uint8_t profile_carousel_active, profile_name_editor_active;
    uint8_t printout_action_active, printout_editor_active, printout_delete_confirm_active;
    uthernet2_network_config_t ethernet_config;
} config_menu_t;
static FATFS g_config_fs;
static config_menu_t *g_deferred_smartport_refresh;
static unsigned mounts, initializes, opens, resets, dhcp_polls, saves, acks, probes, closes;
static int busy, mounted, pending, save_ok, dhcp_result;
static FRESULT open_result;
static int http_download_active(void) { return busy; }
static FRESULT f_mount(FATFS *fs, const char *path, unsigned immediate)
{ (void)path; (void)immediate; assert(!busy); ++mounts; mounted = fs != NULL; return FR_OK; }
static int disk_initialize(int drive) { (void)drive; ++initializes; return 0; }
static FRESULT f_open(FIL *file, const char *path, BYTE mode)
{ (void)file; (void)path; (void)mode; ++opens; return mounted ? open_result : FR_NOT_ENABLED; }
static int reset_media(void *ctx, uint8_t device)
{ (void)ctx; assert(!busy && device == CONFIG_SMARTPORT_ALL_DEVICES); ++resets; return 0; }
static void dhcp_cancel(void *ctx) { (void)ctx; assert(!busy); }
static int dhcp_poll(void *ctx, uthernet2_network_config_t *lease, char *detail, size_t length)
{ (void)ctx; (void)detail; (void)length; assert(!busy); ++dhcp_polls; lease->ip[0] = 42; return dhcp_result; }
static uint8_t mode_state(void *ctx) { (void)ctx; return 1; }
static uint32_t mode_status(void *ctx) { (void)ctx; return 0x1234; }
static uint8_t persist_update(void *ctx, uint8_t *value)
{ (void)ctx; *value = 1; return (uint8_t)pending; }
static void persist_ack(void *ctx, uint8_t value)
{ (void)ctx; assert(!busy && value == 1); ++acks; pending = 0; }
static uint8_t config_menu_onee_fixed_bindings_active(const config_menu_t *m)
{ (void)m; return 0; }
static void config_menu_set_status(config_menu_t *m, uint8_t error, const char *text)
{ (void)m; (void)error; (void)text; }
static void config_menu_ethernet_set_status_ip(config_menu_t *m, const char *text,
                                              uint8_t error, const uthernet2_network_config_t *ip)
{ (void)m; (void)text; (void)error; (void)ip; }
static uint8_t config_menu_save_settings_to_path(config_menu_t *m, const char *p, const char *s)
{ (void)m; (void)p; (void)s; assert(!busy); ++saves; return (uint8_t)save_ok; }
static void config_menu_save_settings(config_menu_t *m)
{ (void)config_menu_save_settings_to_path(m, APPLETINI_CFG_PATH, NULL); }
static void config_menu_load_settings(config_menu_t *m)
{ assert(!busy); ++probes; m->settings_loaded = 1; }
static void config_menu_restore_onee_intent(config_menu_t *m) { (void)m; assert(!busy); }
static void config_menu_apply_runtime(config_menu_t *m) { (void)m; assert(!busy); }
static int config_menu_apply_ethernet_config(config_menu_t *m, uint8_t save)
{ (void)m; (void)save; assert(!busy); return 0; }
static void config_menu_apply_bezel(config_menu_t *m) { (void)m; assert(!busy); }
static void config_menu_apply_video_rom(config_menu_t *m) { (void)m; assert(!busy); }
static void config_menu_try_read_rtc(config_menu_t *m, uint8_t report)
{ (void)m; (void)report; ++probes; }
static void config_menu_refresh_golden_boot(config_menu_t *m) { (void)m; assert(!busy); ++probes; }
static void config_menu_stop_usb0_sd_remote(config_menu_t *m) { (void)m; }
static void config_menu_browser_close(config_menu_t *m) { (void)m; ++closes; }
#include "config_sd_helpers.inc"

static void reset(config_menu_t *m)
{
    memset(m, 0, sizeof(*m));
    m->platform.reset_smartport_media = reset_media;
    m->platform.ethernet_dhcp_cancel = dhcp_cancel;
    m->platform.ethernet_dhcp_poll = dhcp_poll;
    m->platform.get_onee_mode_state = mode_state;
    m->platform.get_onee_mode_status = mode_status;
    m->platform.get_onee_mode_persist_update = persist_update;
    m->platform.ack_onee_mode_persist_update = persist_ack;
    mounts = initializes = opens = resets = dhcp_polls = saves = acks = probes = closes = 0;
    busy = pending = 0; mounted = save_ok = dhcp_result = 1; open_result = FR_OK;
    g_deferred_smartport_refresh = NULL;
}
int main(void)
{
    config_menu_t m;
    FIL file;
    const FRESULT errors[] = {FR_DISK_ERR, FR_DENIED, FR_NO_FILE, FR_WRITE_PROTECTED, FR_NOT_READY};
    reset(&m); busy = 1;
    config_menu_set_active(&m, 1);
    config_menu_retry_settings_if_needed(&m);
    assert(!m.active && !probes && !resets);
    assert(config_menu_mount_sd() == FR_LOCKED && !mounts && !initializes);
    /* Closing remains possible, and a requested refresh survives that close. */
    m.active = 1;
    config_menu_refresh_smartport_media_after_menu_sd(&m);
    assert(g_deferred_smartport_refresh == &m && !resets);
    config_menu_set_active(&m, 0);
    config_menu_poll_ethernet(&m);
    assert(!m.active && closes == 1 && !resets);
    busy = 0;
    config_menu_poll_ethernet(&m);
    config_menu_poll_ethernet(&m);
    assert(resets == 1 && !g_deferred_smartport_refresh);
    config_menu_set_active(&m, 1);
    assert(m.active && probes == 2 && resets == 2);

    reset(&m); busy = 1;
    m.ethernet_dhcp_pending = m.ethernet_slot1_enabled = m.ethernet_config_enabled = 1;
    m.ethernet_address_mode = CONFIG_MENU_ETHERNET_ADDRESS_DHCP;
    for (unsigned i = 0; i < 3; ++i) config_menu_poll_ethernet(&m);
    assert(m.ethernet_dhcp_pending && !dhcp_polls && !saves);
    busy = 0; config_menu_poll_ethernet(&m);
    assert(!m.ethernet_dhcp_pending && dhcp_polls == 1 && saves == 1 && m.ethernet_config.ip[0] == 42);

    reset(&m); busy = pending = 1;
    for (unsigned i = 0; i < 3; ++i) config_menu_poll_onee_mode(&m);
    assert(pending && !acks && !saves && m.onee_persisted_enabled && m.onee_mode_status == 0x1234);
    busy = 0; save_ok = 0; config_menu_poll_onee_mode(&m);
    assert(pending && !acks && saves == 1 && m.onee_persist_write_failed);
    busy = 1; config_menu_poll_onee_mode(&m);
    assert(m.onee_persist_retry_polls == ONEE_PERSIST_RETRY_POLL_LIMIT && saves == 1);
    busy = 0; save_ok = 1; m.onee_persist_retry_polls = 0;
    config_menu_poll_onee_mode(&m);
    assert(!pending && acks == 1 && saves == 2 && !m.onee_persist_write_failed);

    for (unsigned i = 0; i < sizeof(errors)/sizeof(errors[0]); ++i) {
        reset(&m); busy = 1; open_result = errors[i];
        assert(config_menu_open_path(&file, "0:/appletini_cfg.txt", 1) == errors[i]);
        assert(!mounts && !initializes && opens == 1);
    }
    reset(&m); mounted = 0; busy = 1;
    assert(config_menu_open_path(&file, "0:/appletini_cfg.txt", 1) == FR_LOCKED && !mounts);
    busy = 0;
    assert(config_menu_open_path(&file, "0:/appletini_cfg.txt", 1) == FR_OK && mounts == 1 && opens == 3);
    puts("PASS config SD ownership: menu entry blocked, close allowed, refresh deferred once, DHCP/ONEe pending state saved after download, real open errors never remount");
    return 0;
}
