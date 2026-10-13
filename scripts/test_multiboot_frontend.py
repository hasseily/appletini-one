#!/usr/bin/env python3
"""Run real menu/switch/sync/CPU1 helpers with simulated flash and bus failures."""

import os
import re
import shutil
import subprocess
from pathlib import Path

from test_joystick_config_menu import function
from test_amp_startup import main as test_amp_startup

ROOT = Path(__file__).resolve().parents[1]
FRONT = ROOT / "ps_sources/frontend"
OUT = ROOT / "build/multiboot_frontend_test"


def main():
    menu = (FRONT / "config_menu.c").read_text()
    header = (FRONT / "config_menu.h").read_text()
    runtime = (FRONT / "main.c").read_text()
    tabs = (FRONT / "config_menu_main_tabs.c").read_text()
    amp = (ROOT / "ps_sources/lib/amp.c").read_text()
    smartport = (FRONT / "smartport_service.c").read_text()
    assert "CONFIG_MENU_BOOT_STARTUP_FIRMWARE_ITEM 3U" in header
    assert "CONFIG_MENU_BOOT_SWITCH_FIRMWARE_ITEM 4U" in header
    assert "CONFIG_MENU_BOOT_USB_BIND_RESET_ITEM 5U" in header
    activate = function(menu, "config_menu_activate_item")
    assert "CONFIG_MENU_BOOT_STARTUP_FIRMWARE_ITEM" in activate
    assert "CONFIG_MENU_BOOT_SWITCH_FIRMWARE_ITEM" in activate
    assert "config_menu_cycle_startup_firmware(menu);" in activate
    assert "config_menu_switch_firmware(menu);" in activate
    assert "config_menu_cycle_startup_firmware(menu);" in function(menu, "config_menu_adjust_focused_value")
    assert "startup_firmware" not in function(menu, "config_menu_save_settings_to_path")
    boot_draw = function(tabs, "config_menu_draw_boot_settings")
    assert '"Startup firmware"' in boot_draw and '"Switch to Multitini"' in boot_draw
    assert "hgr_draw_value_item_dimmed" in boot_draw and '"Not installed"' in boot_draw
    assert "y + (row_h * 5)" in boot_draw
    assert "control_service_firmware_switch(&config_menu);" in runtime
    assert "g_firmware_switch_active != 0U" in function(runtime, "cherryusb_background_poll")
    assert "g_firmware_switch_active != 0U" in function(runtime, "usb0_priority_checkpoint")
    release = function(amp, "amp_release_core1")
    assert "amp_hold_core1()" not in release and "amp_resume_held_core1" not in amp
    assert release.index("core1_launch_attempted = 1U") < release.index("memcpy(dst")
    assert release.index("Xil_Out32(AMP_CPU1_START_ADDR") < release.index("amp_wake_core1();")

    code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
enum { MB_OK=0, MB_PRODUCT_NONE=0, MB_PRODUCT_APPLETINI=0x100,
       MB_PRODUCT_MULTITINI=0x200, MB_GOLDEN_OFFSET=0 };
typedef uint64_t XTime;
typedef struct { uint8_t golden_available, appletini_present, multitini_present;
                 uint32_t default_product, request_product; uint8_t legacy_present; } mb_platform_status_t;
typedef struct {
    void *ctx;
    uint8_t (*get_multiboot_status)(void *, uint8_t *, uint32_t *);
    int (*set_startup_firmware)(void *, uint32_t);
    int (*request_firmware_switch)(void *, uint32_t);
} config_menu_platform_t;
typedef struct {
    config_menu_platform_t platform;
    uint8_t multiboot_ready, multiboot_present_mask, session_only;
    uint8_t onee_persist_write_failed, status_warning, applicard_slot5_enabled;
    uint32_t startup_firmware;
    char status[96];
} config_menu_t;
#define APPLETINI_CFG_PATH "0:/appletini_cfg.txt"
#define AT_DMA_CTRL_REG 1U
#define CARD_CTRL_FEATURE_ENABLE_REG 2U
#define CARD_CTRL_SSC_STATUS_REG 3U
#define CARD_CTRL_ONEE_MODE_REG 4U
#define CARD_CTRL_VTW_CTRL_REG 5U
#define AT_DMA_CTRL_HOLD_REQ_BIT 1U
#define AT_DMA_CTRL_HOLD_ACTIVE_BIT 16U
#define UART0_BASE 0U
static mb_platform_status_t live_status, g_multiboot_status;
static uint8_t g_multiboot_status_valid, g_firmware_switch_active, g_firmware_switch_fault;
static uint32_t g_firmware_switch_request, g_card_slot_enable_mask;
static unsigned status_rc, golden_rc, request_rc, default_rc, menu_request_rc;
static unsigned clear_rc, multiboot_fail, multiboot_restore_fail, multiboot_writes, hw_multiboot;
static unsigned screenshot_busy, printer_busy, printer_queued, usb_remote, ftp_remote;
static unsigned isolated, host_allowed, hold_timeout, pause_ok;
static unsigned disk_rc, settings_ok, sync_error, pending_count;
static unsigned default_calls, menu_request_calls, requests, clears, resets, holds;
static unsigned disk_calls, sync_calls, settings_calls, golden_checks;
static unsigned stop_calls, worker_calls, hw_dma, hw_features, hw_onee, hw_vtw;
static unsigned isolation_fail, isolation_hazard;
static unsigned cpu1_hold_error, restart_calls;
static XTime time_now;
static char events[256]; static unsigned event_count;
static void event(char value) { assert(event_count < sizeof(events)-1); events[event_count++]=value; events[event_count]=0; }
static int mb_platform_status(uint32_t product, mb_platform_status_t *out) {
    assert(product==MB_PRODUCT_APPLETINI); event('V'); *out=live_status; return (int)status_rc;
}
static int mb_platform_set_default(uint32_t product) {
    ++default_calls; if (!default_rc) live_status.default_product=product; return (int)default_rc;
}
static int mb_platform_golden_offset(uint32_t *offset) { ++golden_checks; event('G'); *offset=0x100000; return (int)golden_rc; }
static int mb_platform_request(uint32_t product) { assert(product==MB_PRODUCT_MULTITINI); ++requests; event('R'); return (int)request_rc; }
static int mb_platform_clear_request(void) { ++clears; return (int)clear_rc; }
static void config_menu_set_status(config_menu_t *menu, uint8_t warning, const char *text) {
    menu->status_warning=warning; snprintf(menu->status,sizeof(menu->status),"%s",text);
}
static uint8_t config_menu_save_settings_to_path(config_menu_t *menu, const char *path, const char *message) {
    (void)menu; (void)message; assert(strcmp(path,APPLETINI_CFG_PATH)==0); ++settings_calls; event('C'); return (uint8_t)settings_ok;
}
static uint8_t screenshot_service_busy(void) { return (uint8_t)screenshot_busy; }
static uint8_t printer_service_job_active(void) { return (uint8_t)printer_busy; }
static uint8_t config_menu_usb0_sd_remote_active(const config_menu_t *menu) { (void)menu; return (uint8_t)usb_remote; }
static uint8_t config_menu_ethernet_ftp_sd_remote_active(const config_menu_t *menu) { (void)menu; return (uint8_t)ftp_remote; }
static uint8_t control_onee_isolated(void) { return (uint8_t)isolated; }
static uint8_t boot_menu_service_host_bus_master_allowed(void) { return (uint8_t)host_allowed; }
static uint8_t vtw_service_onee_set_paused(uint8_t paused) { assert(paused==1); return (uint8_t)pause_ok; }
static unsigned reg_read(unsigned address) {
    if(address==0xF800702CU) return hw_multiboot;
    if (address==CARD_CTRL_SSC_STATUS_REG) return printer_queued;
    if (address==CARD_CTRL_VTW_CTRL_REG) return hw_vtw;
    if (address==CARD_CTRL_ONEE_MODE_REG) {
        if(hw_onee & CARD_CTRL_ONEE_CTRL_REQUEST_BIT) {
            if(isolation_hazard) return hw_onee|CARD_CTRL_ONEE_STATUS_ACTIVITY_BIT;
            if(!isolation_fail) return hw_onee|CARD_CTRL_ONEE_STATUS_ISOLATED_BIT;
        }
        return hw_onee;
    }
    if (address==CARD_CTRL_FEATURE_ENABLE_REG) return hw_features;
    assert(address==AT_DMA_CTRL_REG);
    return hw_dma | ((!hold_timeout && (hw_dma & AT_DMA_CTRL_HOLD_REQ_BIT)) ? AT_DMA_CTRL_HOLD_ACTIVE_BIT : 0U);
}
static void reg_write(unsigned address, unsigned value) {
    if(address==0xF8000008U || address==0xF8000004U) return;
    if(address==0xF800702CU) {
        ++multiboot_writes; event('P');
        if((multiboot_writes==1 && multiboot_fail) || (multiboot_writes==2 && multiboot_restore_fail)) return;
        hw_multiboot=value & 0x1fffU; return;
    }
    if(address==CARD_CTRL_ONEE_MODE_REG) {
        hw_onee=(hw_onee & ~CARD_CTRL_ONEE_CTRL_REQUEST_BIT) | (value & CARD_CTRL_ONEE_CTRL_REQUEST_BIT);
        event(value?'I':'i'); return;
    }
    if (address==CARD_CTRL_FEATURE_ENABLE_REG) { hw_features=value; return; }
    assert(address==AT_DMA_CTRL_REG); hw_dma=value;
    if(value & AT_DMA_CTRL_HOLD_REQ_BIT) { ++holds; event('H'); }
}
#define REG_READ(a) reg_read(a)
#define REG_WRITE(a,v) reg_write(a,v)
static void XTime_GetTime(XTime *now) { time_now+=50000; *now=time_now; }
static uint64_t ticks_to_us(XTime elapsed) { return elapsed; }
static void card_control_write_slot_mask(uint32_t value) { g_card_slot_enable_mask=value|0x80; event(value?'s':'S'); }
static void control_apply_slot5_service(uint8_t enabled) { if(!enabled) ++stop_calls; }
static uint8_t smartport_service_has_pending(void) { return pending_count!=0; }
static void smartport_service_poll(void) { if(pending_count) --pending_count; }
static int disk2_service_flush_dirty_now(void) { ++disk_calls; event('D'); return (int)disk_rc; }
typedef int FRESULT;
enum { FR_OK=0, SP_MAX_DEVICES=4 };
typedef struct { uint8_t image_open, is_ram, read_only; unsigned image_file; } sp_device_t;
static sp_device_t g_devices[SP_MAX_DEVICES];
static void smartport_devices_ensure_defaults(void) {}
static FRESULT f_sync(unsigned *file) { assert(*file==7); ++sync_calls; event('F'); return (int)sync_error; }
static void compositor_set_paused(uint8_t paused) { assert(paused==1); }
static void vtw_service_onee_stop(void) { ++stop_calls; }
static void vtw_service_set_enabled(uint8_t enabled) { assert(!enabled); ++stop_calls; }
static void usb_hid_service_stop(void) { ++stop_calls; }
static void usb_sdd_service_stop(void) { ++stop_calls; }
static uint8_t usb_storage_service_disconnect(void) { ++stop_calls; return 0; }
static int amp_hold_core1(void) { ++worker_calls; event('W'); return (int)cpu1_hold_error; }
static uint32_t amp_release_core1(void) { ++restart_calls; return 0U; } /* Hold attempts require reboot. */
static void compositor_request_full_refresh(void) {}
static void Xil_DCacheFlush(void) {}
static void uart_puts(uint32_t uart, const char *text) { (void)uart; (void)text; }
static void control_reboot_preselected_golden(void) { assert(hw_multiboot==32); ++resets; event('B'); }
'''
    control = (FRONT / "card_control_regs.h").read_text()
    definitions = re.findall(r"^#define CARD_CTRL_ONEE_(?:STATUS|CTRL)_[A-Z_]+.*$", control, re.M)
    definitions += re.findall(r"^#define CARD_CTRL_SSC_STATUS_COUNT_MASK.*$", control, re.M)
    definitions += re.findall(r"^#define CARD_CTRL_VTW_CTRL_CORE_RUN_BIT.*$", control, re.M)
    code = "\n".join(definitions) + "\n" + code
    for name in ("control_write_multiboot_checked", "control_prepare_golden_boot",
                 "control_refresh_multiboot", "menu_platform_get_multiboot_status",
                 "menu_platform_set_startup_firmware", "menu_platform_request_firmware_switch"):
        body = re.sub(r'__asm__ volatile \(.*?\);', '(void)0;', function(runtime, name), flags=re.S)
        code += body + "\n"
    for name in ("config_menu_flush_settings", "config_menu_refresh_multiboot",
                 "config_menu_cycle_startup_firmware", "config_menu_switch_firmware"):
        code += function(menu, name) + "\n"
    code += function(smartport, "smartport_service_sync_sd") + "\n"
    code += function(runtime, "control_hold_quiet_connector") + "\n"
    code += function(runtime, "control_service_firmware_switch") + "\n"
    code += r'''
static int menu_request(void *ctx, uint32_t product) { (void)ctx; assert(product==MB_PRODUCT_MULTITINI); ++menu_request_calls; return (int)menu_request_rc; }
static config_menu_t prepare(void) {
    config_menu_t menu={0};
    memset(&live_status,0,sizeof(live_status));
    live_status.golden_available=live_status.appletini_present=live_status.multitini_present=1;
    live_status.default_product=MB_PRODUCT_APPLETINI;
    status_rc=golden_rc=request_rc=default_rc=menu_request_rc=0;
    clear_rc=multiboot_fail=multiboot_restore_fail=multiboot_writes=0; hw_multiboot=64;
    screenshot_busy=printer_busy=printer_queued=usb_remote=ftp_remote=hold_timeout=0;
    isolated=host_allowed=pause_ok=settings_ok=1;
    disk_rc=sync_error=pending_count=0;
    default_calls=menu_request_calls=requests=clears=resets=holds=disk_calls=sync_calls=settings_calls=golden_checks=stop_calls=worker_calls=0;
    g_firmware_switch_fault=g_firmware_switch_active=0; g_firmware_switch_request=MB_PRODUCT_MULTITINI;
    g_card_slot_enable_mask=0xB0; hw_features=7; hw_dma=0; time_now=0;
    hw_onee=hw_vtw=isolation_fail=isolation_hazard=0;
    cpu1_hold_error=restart_calls=0;
    event_count=0; events[0]=0;
    memset(g_devices,0,sizeof(g_devices));
    g_devices[0].image_open=1; g_devices[0].image_file=7;
    g_devices[1].image_open=1; g_devices[1].is_ram=1;
    g_devices[2].image_open=1; g_devices[2].read_only=1;
    menu.session_only=1; menu.applicard_slot5_enabled=1;
    menu.platform.get_multiboot_status=menu_platform_get_multiboot_status;
    menu.platform.set_startup_firmware=menu_platform_set_startup_firmware;
    menu.platform.request_firmware_switch=menu_request;
    control_refresh_multiboot(); config_menu_refresh_multiboot(&menu);
    return menu;
}
static void cancelled(const config_menu_t *menu) {
    assert(!resets && !worker_calls && !g_firmware_switch_active);
    assert(stop_calls==0);
    assert(g_card_slot_enable_mask==0xB0 && hw_features==7 && hw_dma==0);
    assert(hw_multiboot==64);
    assert((hw_onee & CARD_CTRL_ONEE_CTRL_REQUEST_BIT)==0);
    assert(menu->status_warning && g_firmware_switch_request==MB_PRODUCT_NONE);
}
int main(void) {
    config_menu_t menu=prepare();
    config_menu_cycle_startup_firmware(&menu);
    assert(default_calls==1 && menu.startup_firmware==MB_PRODUCT_MULTITINI && !menu.status_warning);
    assert(live_status.default_product==MB_PRODUCT_MULTITINI);
    config_menu_cycle_startup_firmware(&menu);
    assert(default_calls==2 && menu.startup_firmware==MB_PRODUCT_APPLETINI);
    menu=prepare(); live_status.multitini_present=0; control_refresh_multiboot();
    config_menu_cycle_startup_firmware(&menu); assert(!default_calls && menu.status_warning);
    config_menu_switch_firmware(&menu); assert(!menu_request_calls && menu.status_warning);
    menu=prepare(); live_status.golden_available=0; control_refresh_multiboot();
    config_menu_cycle_startup_firmware(&menu); config_menu_switch_firmware(&menu);
    assert(!default_calls && !menu_request_calls && menu.status_warning);
    menu=prepare(); default_rc=1; config_menu_cycle_startup_firmware(&menu);
    assert(default_calls==1 && menu.startup_firmware==MB_PRODUCT_APPLETINI && menu.status_warning);
    menu=prepare(); config_menu_switch_firmware(&menu);
    assert(menu_request_calls==1 && !menu.status_warning && live_status.default_product==MB_PRODUCT_APPLETINI);
    menu_request_rc=1; config_menu_switch_firmware(&menu); assert(menu.status_warning);
    menu=prepare(); g_firmware_switch_request=MB_PRODUCT_NONE;
    assert(menu_platform_request_firmware_switch(NULL,MB_PRODUCT_MULTITINI)==0);
    assert(menu_platform_request_firmware_switch(NULL,MB_PRODUCT_MULTITINI)!=0);
    assert(menu_platform_request_firmware_switch(NULL,MB_PRODUCT_APPLETINI)!=0);

    /* UI presence goes stale: the live target and golden decide every switch. */
    menu=prepare(); live_status.multitini_present=0;
    control_service_firmware_switch(&menu); cancelled(&menu); assert(!disk_calls && !requests);
    menu=prepare(); status_rc=1;
    control_service_firmware_switch(&menu); cancelled(&menu); assert(!requests);
    menu=prepare(); golden_rc=1;
    control_service_firmware_switch(&menu); cancelled(&menu); assert(!requests && !disk_calls);
    for(unsigned mode=0;mode<5;++mode) {
        menu=prepare();
        if(mode==0) screenshot_busy=1; if(mode==1) printer_busy=1;
        if(mode==2) usb_remote=1; if(mode==3) ftp_remote=1;
        if(mode==4) printer_queued=1;
        control_service_firmware_switch(&menu); cancelled(&menu); assert(!requests && !disk_calls);
    }
    menu=prepare(); isolated=host_allowed=0;
    control_service_firmware_switch(&menu); cancelled(&menu); assert(!holds && !disk_calls);
    menu=prepare(); isolated=0; hold_timeout=1;
    control_service_firmware_switch(&menu); cancelled(&menu); assert(holds==1 && !disk_calls);
    menu=prepare(); pause_ok=0;
    control_service_firmware_switch(&menu); cancelled(&menu); assert(!disk_calls);
    menu=prepare(); pending_count=65;
    control_service_firmware_switch(&menu); cancelled(&menu); assert(pending_count==1 && !disk_calls);
    menu=prepare(); disk_rc=1;
    control_service_firmware_switch(&menu); cancelled(&menu); assert(disk_calls==64 && !settings_calls && !requests);
    menu=prepare(); settings_ok=0;
    control_service_firmware_switch(&menu); cancelled(&menu); assert(settings_calls==1 && !sync_calls && !requests);
    menu=prepare(); sync_error=3;
    control_service_firmware_switch(&menu); cancelled(&menu); assert(sync_calls==1 && !requests);
    menu=prepare(); request_rc=1;
    control_service_firmware_switch(&menu); cancelled(&menu); assert(requests==1 && clears==1);
    menu=prepare(); request_rc=clear_rc=1;
    control_service_firmware_switch(&menu); cancelled(&menu);
    assert(requests==1 && clears==1 && strstr(menu.status,"CLEAR FAILED"));
    menu=prepare(); multiboot_fail=1;
    control_service_firmware_switch(&menu); cancelled(&menu);
    assert(requests==1 && clears==1 && !worker_calls && multiboot_writes==2);
    menu=prepare(); cpu1_hold_error=1;
    control_service_firmware_switch(&menu);
    assert(!resets && worker_calls==1 && restart_calls==1 && !stop_calls && clears==1);
    assert(g_firmware_switch_active && g_firmware_switch_fault && menu.status_warning);
    assert(g_card_slot_enable_mask==0xB0 && hw_features==7 && !hw_dma);
    menu=prepare(); isolated=0; cpu1_hold_error=1;
    control_service_firmware_switch(&menu);
    assert(!resets && restart_calls==1 && !stop_calls && clears==1);
    assert(g_firmware_switch_active && g_firmware_switch_fault && (hw_dma&1));
    assert(g_card_slot_enable_mask==0xB0 && hw_features==7 && menu.status_warning);
    menu=prepare(); isolated=0; cpu1_hold_error=multiboot_restore_fail=1;
    control_service_firmware_switch(&menu);
    assert(!resets && restart_calls==1 && !stop_calls && clears==1);
    assert(g_firmware_switch_active && g_firmware_switch_fault && (hw_dma&1));
    assert(hw_multiboot==32 && strstr(menu.status,"RECOVERY FAILED"));
    menu=prepare(); pending_count=3;
    control_service_firmware_switch(&menu);
    assert(resets==1 && worker_calls==1 && requests==1 && !pending_count);
    assert(settings_calls==1 && sync_calls==1 && disk_calls==1);
    assert(strchr(events,'D') < strchr(events,'C') && strchr(events,'C') < strchr(events,'F'));
    assert(strchr(events,'F') < strchr(events,'R') && strchr(events,'R') < strchr(events,'W') && strchr(events,'W') < strchr(events,'B'));
    assert(live_status.default_product==MB_PRODUCT_APPLETINI);
    menu=prepare(); isolated=0;
    control_service_firmware_switch(&menu); assert(resets==1 && holds==1);
    menu=prepare(); menu.session_only=0;
    control_service_firmware_switch(&menu); assert(resets==1 && !settings_calls);
    /* A quiet unattached connector can enter hardware isolation without starting ONE//e. */
    uint32_t quiet=(CARD_CTRL_ONEE_STATUS_SIGNATURE<<CARD_CTRL_ONEE_STATUS_SIGNATURE_SHIFT) |
        CARD_CTRL_ONEE_STATUS_HDL_PRESENT_BIT | CARD_CTRL_ONEE_STATUS_QUIET_BIT |
        CARD_CTRL_ONEE_STATUS_RESELECT_ARMED_BIT | CARD_CTRL_ONEE_STATUS_OUTPUTS_OFF_BIT;
    menu=prepare(); isolated=host_allowed=0; hw_onee=quiet;
    control_service_firmware_switch(&menu); assert(resets==1 && !holds && hw_onee&1);
    assert(hw_vtw==0 && live_status.default_product==MB_PRODUCT_APPLETINI);
    for(unsigned fault=0;fault<9;++fault) {
        menu=prepare(); isolated=host_allowed=0; hw_onee=quiet;
        if(fault==0) hw_onee|=CARD_CTRL_ONEE_STATUS_ACTIVITY_BIT;
        if(fault==1) hw_onee|=CARD_CTRL_ONEE_STATUS_LOCKOUT_BIT;
        if(fault==2) hw_onee|=CARD_CTRL_ONEE_STATUS_APPLE_POWER_BIT;
        if(fault==3) hw_onee &= ~CARD_CTRL_ONEE_STATUS_QUIET_BIT;
        if(fault==4) hw_onee &= ~CARD_CTRL_ONEE_STATUS_HDL_PRESENT_BIT;
        if(fault==5) hw_vtw=CARD_CTRL_VTW_CTRL_CORE_RUN_BIT;
        if(fault==6) isolation_fail=1;
        if(fault==7) isolation_hazard=1;
        if(fault==8) disk_rc=1;
        control_service_firmware_switch(&menu); cancelled(&menu); assert(!requests);
    }
    puts("PASS: menu presence, startup persistence, one-shot switch, save failures and quiet-connector isolation cleanup");
    return 0;
}
'''
    OUT.mkdir(parents=True, exist_ok=True)
    compiler = shutil.which("gcc") or shutil.which("clang")
    if not compiler:
        compiler = "C:/msys64/ucrt64/bin/gcc.exe"
    env = dict(os.environ, PATH=str(Path(compiler).parent) + os.pathsep + os.environ.get("PATH", ""))
    source = OUT / "frontend.c"
    exe = OUT / "frontend.exe"
    source.write_text(code)
    subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-misleading-indentation", str(source), "-o", str(exe)], check=True, env=env)
    subprocess.run([str(exe)], check=True, env=env)

    # Compile the complete production helper, replacing only ARM operations and MMIO.
    test_amp_startup()

    draw_code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define CMUI_ROW_H 34
#define CMUI_BODY_SCALE 2
#define HGR_TEXT_SCALE_X 2
#define HGR_SCALE 1
#define HGR_WHITE 1U
#define CMUI_COLOR_ACCENT 1U
#define CMUI_COLOR_BG 0U
#define CMUI_COLOR_BORDER_SOFT 2U
#define FB16_BUILTIN_FONT_ADVANCE_X 7
#define CONFIG_MENU_USB_BIND_ACTION_COUNT 14U
#define MB_PRODUCT_MULTITINI 0x200U
'''
    draw_code += header[header.index("#define CONFIG_MENU_BOOT_ONEE_ITEM"):
                        header.index("#define CONFIG_MENU_PROFILE_ITEM_COUNT")]
    draw_code += re.search(r"typedef enum \{\s+CONFIG_MENU_ONEE_MODE_OFF.*?\} config_menu_onee_mode_state_t;", header, re.S).group(0) + "\n"
    draw_code += re.search(r"typedef enum \{\s+CONFIG_MENU_USB_BIND_ACTION_UP.*?\} config_menu_usb_bind_action_t;", header, re.S).group(0) + "\n"
    draw_code += re.search(r"static const uint8_t k_boot_usb_binding_action_order\[.*?\] = \{.*?\n\};", menu, re.S).group(0) + "\n"
    draw_code += r'''
typedef struct {
    uint8_t fixed, usb_bindings_editable, usb_binding_capture;
    uint8_t boot_timeout_mode, boot_device, onee_mode_state;
    uint8_t multiboot_ready, multiboot_present_mask;
    uint32_t startup_firmware, item_focus;
    uint8_t usb_menu_bindings[14];
} config_menu_t;
static unsigned screen_w, help_y, focus_count, saw_startup, saw_switch, switch_dim;
static uint8_t config_menu_onee_fixed_bindings_active(const config_menu_t *menu) { return menu->fixed; }
static const char *config_menu_boot_timeout_text(unsigned value) { (void)value; return "3 seconds"; }
static const char *config_menu_boot_device_text(unsigned value) { (void)value; return "SmartPort"; }
static const char *config_menu_onee_mode_text(const config_menu_t *menu) { (void)menu; return "OFF"; }
static const char *config_menu_onee_video_standard_text(const config_menu_t *menu) { (void)menu; return "NTSC"; }
static const char *mb_product_name(unsigned product) { return product==MB_PRODUCT_MULTITINI ? "Multitini" : "Appletini"; }
static const char *config_menu_usb_binding_source_text(unsigned source) { (void)source; return "Space"; }
static unsigned config_menu_usb_open_close_binding_source(const config_menu_t *menu) { (void)menu; return 0; }
static void record_item(int x, int y, int w, uint8_t focus, uint8_t dimmed, const char *text) {
    assert(x>=0 && x+w<=(int)screen_w);
    assert(y>=154 && y+CMUI_ROW_H <= (int)help_y-4);
    assert((int)strlen(text)*14+48<=w);
    focus_count+=focus!=0;
    if(strstr(text,"Startup firmware")) ++saw_startup;
    if(strstr(text,"Switch to Multitini")) { ++saw_switch; switch_dim=dimmed; }
}
static void hgr_draw_item(uint16_t *fb,int x,int y,int w,uint8_t focus,const char *text,uint32_t color) {
    (void)fb; (void)color; record_item(x,y,w,focus,0,text);
}
static void hgr_draw_item_dimmed(uint16_t *fb,int x,int y,int w,uint8_t focus,const char *text) {
    (void)fb; record_item(x,y,w,focus,1,text);
}
static void value(uint16_t *fb,int x,int y,int w,uint8_t focus,const char *label,const char *text,uint8_t dimmed) {
    char combined[128]; (void)fb; snprintf(combined,sizeof(combined),"%s: %s",label,text);
    record_item(x,y,w,focus,dimmed,combined);
}
static void hgr_draw_value_item(uint16_t *fb,int x,int y,int w,uint8_t focus,const char *label,const char *text) {
    value(fb,x,y,w,focus,label,text,0);
}
static void hgr_draw_value_item_dimmed(uint16_t *fb,int x,int y,int w,uint8_t focus,const char *label,const char *text) {
    value(fb,x,y,w,focus,label,text,1);
}
static void cmui_text(uint16_t *fb,int x,int y,const char *text,uint32_t fg,uint32_t bg,int scale) {
    (void)fb; (void)fg; (void)bg;
    assert(x+(int)strlen(text)*7*scale<=(int)screen_w && y+8*scale<(int)help_y);
}
static void fb16_fill_rect(uint16_t *fb,int x,int y,int w,int h,uint32_t color) {
    (void)fb; (void)color; assert(x+w<=(int)screen_w && y+h<(int)help_y);
}
'''
    for name in ("config_menu_boot_usb_binding_item_for_action",):
        draw_code += function(menu, name) + "\n"
    for name in ("usb_binding_draw_label", "hgr_draw_usb_binding_item", "config_menu_draw_boot_settings"):
        draw_code += function(tabs, name) + "\n"
    draw_code += r'''
int main(void) {
    unsigned checks=0;
    for(unsigned mode=0;mode<2;++mode) {
        screen_w=mode?1920:1680;
        unsigned height=mode?1080:1050;
        help_y=height-38-58-18-210;
        for(unsigned fixed=0;fixed<2;++fixed) for(unsigned ready=0;ready<2;++ready)
        for(unsigned present=0;present<4;++present) for(unsigned focus=0;focus<20;++focus) {
            config_menu_t menu={0}; menu.fixed=(uint8_t)fixed;
            menu.usb_bindings_editable=1; menu.usb_binding_capture=0xff;
            menu.multiboot_ready=(uint8_t)ready; menu.multiboot_present_mask=(uint8_t)present;
            menu.startup_firmware=0x100; menu.item_focus=focus;
            focus_count=saw_startup=saw_switch=0;
            config_menu_draw_boot_settings(NULL,&menu,380,154,(int)screen_w-428);
            assert(focus_count==1 && saw_startup==1 && saw_switch==1);
            assert(switch_dim==(!ready || !(present&2)));
            ++checks;
        }
    }
    printf("PASS: %u Boot Settings focus/presence layouts fit above help at 1680x1050 and 1920x1080\n",checks);
    return 0;
}
'''
    source = OUT / "layout.c"
    exe = OUT / "layout.exe"
    source.write_text(draw_code)
    subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                    str(source), "-o", str(exe)], check=True, env=env)
    subprocess.run([str(exe)], check=True, env=env)


if __name__ == "__main__":
    main()
