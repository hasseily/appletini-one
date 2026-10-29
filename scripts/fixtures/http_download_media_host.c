#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define UART0_BASE 0U
#define CARD_CTRL_SLOT_BIT(n) (1U << (n))
#define CARD_CTRL_SLOT_ETHERNET 3U
#define CARD_CTRL_SLOT_DISK2 6U
#define SMARTPORT_SERVICE_ALL_DEVICES 255U
typedef struct { uint8_t usb, ftp; } config_menu_t;
static uint8_t g_usb0_sd_handover_pending, g_screenshot_media_refresh_pending;
static uint8_t g_ftp_slots_suppressed;
static uint32_t g_card_slot_enable_mask, g_ftp_saved_slot_mask;
static int active, cleanup_failure, connects, disconnects, busy;
static int screenshot_busy, screenshot_cancels, flushes, refreshes;
static int ftp_starts, ftp_running, sdd_running, sdd_stops;
static int http_running, probe_running, bench_running;
static int http_download_active(void) { return active; }
static int http_download_quiesce(void)
{ if (!cleanup_failure) active=0; return cleanup_failure; }
static void http_service_storage_busy(int value) { busy=value; }
static int http_service_active(void) { return http_running; }
static int https_probe_active(void) { return probe_running; }
static int http_bench_active(void) { return bench_running; }
static int screenshot_service_busy(void) { return screenshot_busy; }
static int screenshot_service_cancel(void)
{ screenshot_busy=0; screenshot_cancels++; return 0; }
static int usb_sdd_service_active(void) { return sdd_running; }
static void usb_sdd_service_stop(void) { sdd_running=0; sdd_stops++; }
static int disk2_service_flush_dirty_now(void) { flushes++; return 0; }
static void usb_storage_service_connect(void) { connects++; }
static int usb_storage_service_disconnect(void) { disconnects++; return 0; }
static void uart_puts(uint32_t uart, const char *message) { (void)uart; (void)message; }
static int ftp_sd_service_active(void) { return ftp_running; }
static int ftp_sd_service_start(char *detail, size_t size)
{ (void)detail; (void)size; ftp_starts++; ftp_running=1; return 0; }
static void ftp_sd_service_stop(void) { ftp_running=0; }
static void smartport_service_suspend_sd(void) {}
static void card_control_write_slot_mask(uint32_t mask) { (void)mask; }
static void control_restore_ftp_sd_media(void) {}
static void control_restore_ftp_slots(void) {}
static int smartport_service_reset_media(uint8_t device)
{ assert(device==SMARTPORT_SERVICE_ALL_DEVICES); refreshes++; return 0; }
static int config_menu_usb0_sd_remote_active(const config_menu_t *menu) { return menu->usb; }
static int config_menu_ethernet_ftp_sd_remote_active(const config_menu_t *menu) { return menu->ftp; }

#include "main_download_media.inc"

int main(void)
{
    config_menu_t menu={0};
    char detail[96];
    active=1; cleanup_failure=1; screenshot_busy=1;
    control_set_usb0_sd_remote_mount(NULL,1);
    assert(busy && g_usb0_sd_handover_pending && !connects && !flushes);
    assert(screenshot_cancels==1);
    control_poll_usb0_sd_handover();
    assert(!connects && g_usb0_sd_handover_pending);
    cleanup_failure=0;
    control_poll_usb0_sd_handover();
    assert(connects==1 && flushes==1 && !g_usb0_sd_handover_pending && !active);
    control_poll_usb0_sd_handover();
    assert(connects==1);
    control_set_usb0_sd_remote_mount(NULL,0);
    assert(!busy && disconnects==1);

    active=1; cleanup_failure=1;
    control_set_usb0_sd_remote_mount(NULL,1);
    control_set_usb0_sd_remote_mount(NULL,0);
    cleanup_failure=0;
    control_poll_usb0_sd_handover();
    assert(connects==1 && !g_usb0_sd_handover_pending);

    active=1;
    ui_screenshot_sd_write_complete(NULL);
    ui_poll_screenshot_media_refresh(&menu);
    assert(!refreshes && g_screenshot_media_refresh_pending);
    active=0; menu.usb=1;
    ui_poll_screenshot_media_refresh(&menu); assert(!refreshes);
    menu.usb=0; menu.ftp=1;
    ui_poll_screenshot_media_refresh(&menu); assert(!refreshes);
    menu.ftp=0; sdd_running=1;
    ui_poll_screenshot_media_refresh(&menu); assert(!refreshes);
    sdd_running=0;
    ui_poll_screenshot_media_refresh(&menu);
    assert(refreshes==1 && !g_screenshot_media_refresh_pending);
    ui_poll_screenshot_media_refresh(&menu); assert(refreshes==1);

    active=1;
    assert(control_set_ethernet_ftp_sd_remote(NULL,1,detail,sizeof detail)==-1);
    assert(!ftp_starts && strstr(detail,"HTTP/S"));
    active=0;
    assert(control_set_ethernet_ftp_sd_remote(NULL,1,detail,sizeof detail)==0);
    assert(ftp_starts==1 && g_ftp_slots_suppressed);
    puts("PASS: actual main callbacks defer SD handover/refresh, retry cleanup, cancel pending USB, and block FTP during downloads");
    return 0;
}
