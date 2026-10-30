#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "config_menu.h"
#include "card_control_regs.h"
#define APPLETINI_HTTP_SERVICE 1

typedef struct { uint32_t input_seq; } ui_state_t;
typedef uint64_t XTime;
#define COUNTS_PER_SECOND 1000000U / 1U
static XTime clock_ticks;
static uint32_t update_cost, http_cost;
static uint32_t last_http_budget;
static unsigned http_calls, refresh_calls;
static uint8_t force_refresh, lose_safe_after_http;
enum { HTTP_OWNER_NONE, HTTP_OWNER_NET, HTTP_OWNER_UPDATE };
static uint8_t http_owner;
static uint8_t http_service_getowner(void) { return http_owner; }
static unsigned ready_after_polls;
static void XTime_GetTime(XTime *now) { *now=clock_ticks; }
static void compositor_request_full_refresh(void) { ++refresh_calls;force_refresh=1; }
static uint8_t compositor_full_refresh_active(void) { return force_refresh; }
static uint8_t g_online_update_approved_kind = ONLINE_UPDATE_KIND_COUNT;
static uint8_t g_golden_boot_update_request, g_firmware_switch_active, g_firmware_switch_fault;
static uint32_t g_firmware_switch_request;
static online_update_snapshot_t live;
static uint8_t allowed, service_active, poll_allowed, poll_busy, disconnected;
static uint8_t boot_start, boot_handoff, http_busy, screenshot_busy, printer_busy;
static uint8_t sdd_busy, usb_remote, ftp_remote;
static uint32_t ssc_pending;
static int service_result;
static unsigned check_calls, start_calls, cancel_calls, poll_calls, finish_calls;
static uint8_t started_kind;
static const char *ready_path;
static XTime g_ethernet_dhcp_started;
static unsigned nic_reads,nic_tests,nic_writes,dhcp_starts,dhcp_polls,dhcp_cancels,timing_logs;
static uint32_t g_card_slot_enable_mask,g_card_slot_effective_mask,slot_register,filter_seen;
static uint8_t onee_isolated,filter_hold;
static uint8_t control_onee_isolated(void) { return onee_isolated; }
static uint8_t boot_menu_service_slot_allowed(uint8_t slot) { (void)slot;return 1; }
static uint32_t http_service_filter_slot_mask(uint32_t mask) {
    filter_seen=mask;return filter_hold?mask&~(1U<<1):mask;
}
static void write_register(uint32_t address,uint32_t value) {
    assert(address==CARD_CTRL_SLOT_ENABLE_REG);slot_register=value;
}
#define REG_WRITE(address,value) write_register(address,value)
static uint8_t config_menu_onee_fixed_bindings_active(const config_menu_t *menu) { (void)menu;return 0; }
int uthernet2_read_network_config(uthernet2_network_config_t *config) { (void)config;++nic_reads;return 0; }
int uthernet2_write_network_config(const uthernet2_network_config_t *config) { (void)config;++nic_writes;return 0; }
int uthernet2_test(uthernet2_test_result_t *result) { (void)result;++nic_tests;return 0; }
int uthernet2_dhcp_start(const uint8_t mac[UTHERNET2_MAC_LEN],char *detail,size_t size) {
    (void)mac;(void)detail;(void)size;++dhcp_starts;return 0;
}
int uthernet2_dhcp_poll(uthernet2_network_config_t *config,char *detail,size_t size) {
    (void)config;(void)detail;(void)size;++dhcp_polls;return 0;
}
void uthernet2_dhcp_cancel(void) { ++dhcp_cancels; }
static void boot_timing_log(const char *text,XTime started) { (void)text;(void)started;++timing_logs; }

static uint8_t control_golden_boot_update_allowed(void) { return allowed; }
static void http_service_poll_budget(uint32_t max_us) {
    assert(max_us && max_us<=2000);last_http_budget=max_us;
    ++http_calls;clock_ticks+=http_cost<max_us?http_cost:max_us;
    if(lose_safe_after_http) boot_handoff=1;
}
uint8_t config_menu_is_active(const config_menu_t *menu) { return menu->active; }
static uint8_t vtw_service_boot_start_pending(void) { return boot_start; }
static uint8_t vtw_service_boot_handoff_pending(void) { return boot_handoff; }
static uint8_t http_download_active(void) { return http_busy; }
static uint8_t screenshot_service_busy(void) { return screenshot_busy; }
static uint8_t printer_service_job_active(void) { return printer_busy; }
static uint8_t usb_sdd_service_active(void) { return sdd_busy; }
uint8_t config_menu_usb0_sd_remote_active(const config_menu_t *menu) { (void)menu; return usb_remote; }
uint8_t config_menu_ethernet_ftp_sd_remote_active(const config_menu_t *menu) { (void)menu; return ftp_remote; }
static uint32_t read_register(uint32_t address)
{ assert(address == CARD_CTRL_SSC_STATUS_REG); return ssc_pending; }
#define REG_READ(address) read_register(address)
static void online_update_status(online_update_snapshot_t *status) { *status = live; }
static int online_update_check(void) { ++check_calls; return service_result; }
static int online_update_start(uint8_t kind) { ++start_calls; started_kind = kind; return service_result; }
static void online_update_cancel(void) { ++cancel_calls; }
static uint8_t online_update_active(void) { return service_active; }
static const char *online_update_ready_path(void) { return ready_path; }
static void online_update_poll(uint8_t safe, uint8_t busy)
{
    ++poll_calls; poll_allowed = safe; poll_busy = busy;
    clock_ticks+=update_cost;
    if(ready_after_polls && poll_calls>=ready_after_polls) {
        live.state=ONLINE_UPDATE_READY;service_active=0;
    }
    if (disconnected) { live.state = ONLINE_UPDATE_FAILED; live.result = -9984; }
}
static void config_menu_set_status(config_menu_t *menu, uint8_t warning, const char *text)
{
    menu->status_warning = warning;
    snprintf(menu->status, sizeof(menu->status), "%s", text);
    if (warning) ++finish_calls;
}
void config_menu_refresh_golden_boot(config_menu_t *menu) { menu->golden_boot_update_allowed = allowed; }

/* PRODUCTION_FUNCTIONS */

static void reset(config_menu_t *menu, ui_state_t *ui)
{
    memset(menu, 0, sizeof(*menu)); memset(ui, 0, sizeof(*ui)); memset(&live, 0, sizeof(live));
    menu->active = allowed = 1;
    live.state = ONLINE_UPDATE_AVAILABLE; live.available_mask = 3;
    g_online_update_approved_kind = ONLINE_UPDATE_KIND_COUNT;
    g_golden_boot_update_request = g_firmware_switch_active = g_firmware_switch_fault = 0;
    g_firmware_switch_request = MB_PRODUCT_NONE;
    service_active = poll_allowed = poll_busy = disconnected = 0;
    boot_start = boot_handoff = http_busy = screenshot_busy = printer_busy = 0;
    sdd_busy = usb_remote = ftp_remote = 0; ssc_pending = 0;
    service_result = 0; check_calls = start_calls = cancel_calls = poll_calls = finish_calls = 0;
    ready_path = "0:/FIRMWARE.BIN";
    menu->platform.get_online_update_status = menu_platform_get_online_update_status;
    (void)ui_online_update_composite_due(ui,menu); /* reset the private pacing state in inactive mode */
    clock_ticks=update_cost=http_cost=http_calls=refresh_calls=force_refresh=lose_safe_after_http=ready_after_polls=0;
    last_http_budget=0;
    http_owner=HTTP_OWNER_UPDATE;
    g_ethernet_dhcp_started=0;
    nic_reads=nic_tests=nic_writes=dhcp_starts=dhcp_polls=dhcp_cancels=timing_logs=0;
    g_card_slot_enable_mask=g_card_slot_effective_mask=slot_register=filter_seen=0;
    onee_isolated=filter_hold=0;
}
static void no_install(void)
{
    assert(g_firmware_switch_request == MB_PRODUCT_NONE && !g_golden_boot_update_request);
}
static void ready(uint8_t kind)
{
    live.state = ONLINE_UPDATE_READY; live.kind = kind;
    ready_path = kind == ONLINE_UPDATE_BOOT ? "0:/BOOT.BIN" : "0:/FIRMWARE.BIN";
}

int main(void)
{
    config_menu_t menu;
    ui_state_t ui;
    reset(&menu, &ui);
    assert(menu_platform_request_online_update_check(NULL) == 0 && check_calls == 1);
    allowed = 0;
    assert(menu_platform_request_online_update_check(NULL) != 0 && check_calls == 1);
    assert(menu_platform_request_online_update_download(NULL, ONLINE_UPDATE_BOOT) != 0 && !start_calls);
    allowed = 1;
    assert(menu_platform_request_online_update_download(NULL, ONLINE_UPDATE_KIND_COUNT) != 0 && !start_calls);
    service_result = -1;
    assert(menu_platform_request_online_update_download(NULL, ONLINE_UPDATE_FIRMWARE) != 0 && start_calls == 1);
    assert(g_online_update_approved_kind == ONLINE_UPDATE_KIND_COUNT);
    puts("PASS queued check/download callbacks reject unsafe, invalid and failed starts without approval");

    for (uint8_t kind = 0; kind < ONLINE_UPDATE_KIND_COUNT; ++kind) {
        reset(&menu, &ui); ready(kind);
        control_service_online_update(&ui, &menu); no_install();
        assert(menu_platform_request_online_update_download(NULL, kind) == 0 && started_kind == kind);
        const uint8_t waiting[] = {ONLINE_UPDATE_DOWNLOADING, ONLINE_UPDATE_VERIFYING,
                                  ONLINE_UPDATE_CHECKING, ONLINE_UPDATE_AVAILABLE};
        for (unsigned i = 0; i < sizeof(waiting); ++i) {
            live.state = waiting[i]; control_service_online_update(&ui, &menu); no_install();
            assert(g_online_update_approved_kind == kind);
        }
        ready((uint8_t)(1U - kind)); control_service_online_update(&ui, &menu); no_install();
        ready(kind); control_service_online_update(&ui, &menu);
        assert(menu.online_update_install_pending && g_online_update_approved_kind == ONLINE_UPDATE_KIND_COUNT);
        assert(kind == ONLINE_UPDATE_BOOT ? g_golden_boot_update_request == 1 :
               g_firmware_switch_request == MB_PRODUCT_APPLETINI);
        assert(ui.input_seq && poll_calls && poll_allowed && !poll_busy);
    }
    puts("PASS only approved matching READY queues the correct firmware/boot installer; no install while downloading/verifying");

    for (unsigned cause = 0; cause < 5; ++cause) {
        reset(&menu, &ui);
        assert(!menu_platform_request_online_update_download(NULL, ONLINE_UPDATE_FIRMWARE));
        ready(ONLINE_UPDATE_FIRMWARE);
        if (cause == 0) menu_platform_cancel_online_update(NULL);
        if (cause == 1) live.state = ONLINE_UPDATE_FAILED;
        if (cause == 2) live.state = ONLINE_UPDATE_CANCELLED;
        if (cause == 3) allowed = 0;
        if (cause == 4) disconnected = 1;
        control_service_online_update(&ui, &menu); no_install();
        assert(g_online_update_approved_kind == ONLINE_UPDATE_KIND_COUNT);
        allowed = 1; disconnected = 0; ready(ONLINE_UPDATE_FIRMWARE);
        control_service_online_update(&ui, &menu); no_install();
    }
    puts("PASS cancel, failed verification/HTTP disconnect and safety loss revoke automatic install permanently");

    for (unsigned cause = 0; cause < 8; ++cause) {
        reset(&menu, &ui);
        assert(!menu_platform_request_online_update_download(NULL, ONLINE_UPDATE_FIRMWARE));
        ready(ONLINE_UPDATE_FIRMWARE);
        if (cause == 0) http_busy = 1;
        if (cause == 1) screenshot_busy = 1;
        if (cause == 2) printer_busy = 1;
        if (cause == 3) sdd_busy = 1;
        if (cause == 4) menu.ethernet_dhcp_pending = 1;
        if (cause == 5) ssc_pending = CARD_CTRL_SSC_STATUS_COUNT_MASK;
        if (cause == 6) usb_remote = 1;
        if (cause == 7) ftp_remote = 1;
        control_service_online_update(&ui, &menu); no_install();
        assert(poll_busy && g_online_update_approved_kind == ONLINE_UPDATE_KIND_COUNT);
    }
    for (unsigned cause = 0; cause < 8; ++cause) {
        reset(&menu, &ui);
        assert(!menu_platform_request_online_update_download(NULL, ONLINE_UPDATE_FIRMWARE));
        ready(ONLINE_UPDATE_FIRMWARE);
        if (cause == 0) menu.active = 0;
        if (cause == 1) allowed = 0;
        if (cause == 2) g_firmware_switch_active = 1;
        if (cause == 3) g_firmware_switch_fault = 1;
        if (cause == 4) g_golden_boot_update_request = 1;
        if (cause == 5) g_firmware_switch_request = MB_PRODUCT_MULTITINI;
        if (cause == 6) boot_start = 1;
        if (cause == 7) boot_handoff = 1;
        control_service_online_update(&ui, &menu);
        assert(!poll_allowed && !menu.online_update_install_pending &&
               g_online_update_approved_kind == ONLINE_UPDATE_KIND_COUNT);
        if (cause != 4 && cause != 5) no_install();
    }
    puts("PASS all 16 storage/service/boot/handoff conflicts reach the poll gate and revoke approval without starting installation");

    for (uint8_t kind = 0; kind < ONLINE_UPDATE_KIND_COUNT; ++kind) {
        for (unsigned cause = 0; cause < 10; ++cause) {
            reset(&menu, &ui); ready(kind);
            if (cause == 0) live.state = ONLINE_UPDATE_DOWNLOADING;
            if (cause == 1) live.kind = (uint8_t)(1U - kind);
            if (cause == 2) ready_path = NULL;
            if (cause == 3) ready_path = "0:/downloads/FIRMWARE.BIN";
            if (cause == 4) allowed = 0;
            if (cause == 5) g_firmware_switch_request = MB_PRODUCT_MULTITINI;
            if (cause == 6) g_golden_boot_update_request = 1;
            if (cause == 7) g_firmware_switch_active = 1;
            if (cause == 8) g_firmware_switch_fault = 1;
            if (cause == 9) service_active = 1;
            assert(menu_platform_request_online_update_install(NULL, kind) != 0);
            if (cause != 5 && cause != 6) no_install();
        }
        reset(&menu, &ui); ready(kind);
        assert(!menu_platform_request_online_update_install(NULL, kind));
        assert(kind == ONLINE_UPDATE_BOOT ? g_golden_boot_update_request == 1 :
               g_firmware_switch_request == MB_PRODUCT_APPLETINI);
    }
    puts("PASS staged retry requires exact verified root path, matching kind, idle owner and safe phase for both installers");

    reset(&menu, &ui);
    assert(!menu_platform_request_online_update_download(NULL, ONLINE_UPDATE_FIRMWARE));
    ready(ONLINE_UPDATE_FIRMWARE); ready_path = "0:/wrong.bin";
    control_service_online_update(&ui, &menu); no_install();
    assert(g_online_update_approved_kind == ONLINE_UPDATE_KIND_COUNT && menu.status_warning &&
           strstr(menu.status, "INSTALL NOT AVAILABLE") && !menu.online_update_install_pending);
    puts("PASS late ready-path mismatch reports failure, clears approval and leaves the menu usable");
    reset(&menu,&ui);service_active=1;live.state=ONLINE_UPDATE_DOWNLOADING;
    update_cost=http_cost=10;control_service_online_update(&ui,&menu);
    assert(poll_calls==8 && http_calls==7 && clock_ticks==150);
    reset(&menu,&ui);service_active=1;live.state=ONLINE_UPDATE_DOWNLOADING;
    update_cost=http_cost=300;control_service_online_update(&ui,&menu);
    assert(poll_calls==4 && http_calls==3 && clock_ticks==2100);
    assert(last_http_budget==500);
    reset(&menu,&ui);service_active=1;update_cost=300;http_cost=2000;
    control_service_online_update(&ui,&menu);
    assert(poll_calls==1 && http_calls==1 && last_http_budget==1700 && clock_ticks==2000);
    for(unsigned conflict=0;conflict<2;++conflict) {
        reset(&menu,&ui);service_active=1;
        if(conflict) http_busy=1;else allowed=0;
        control_service_online_update(&ui,&menu);assert(poll_calls==1 && !http_calls);
    }
    reset(&menu,&ui);control_service_online_update(&ui,&menu);assert(poll_calls==1 && !http_calls);
    reset(&menu,&ui);service_active=1;http_owner=HTTP_OWNER_NET;
    control_service_online_update(&ui,&menu);assert(poll_calls==8 && !http_calls);
    reset(&menu,&ui);service_active=1;lose_safe_after_http=1;
    control_service_online_update(&ui,&menu);assert(poll_calls==2 && http_calls==1 && !poll_allowed);
    reset(&menu,&ui);service_active=1;live.kind=ONLINE_UPDATE_FIRMWARE;ready_after_polls=2;
    g_online_update_approved_kind=ONLINE_UPDATE_FIRMWARE;
    control_service_online_update(&ui,&menu);
    assert(poll_calls==2 && http_calls==1 && g_firmware_switch_request==MB_PRODUCT_APPLETINI);
    puts("PASS bounded foreground pump:8-pass cap,2ms cooperative budget, no foreign/unsafe/busy HTTP pumping, late handoff and immediate READY dispatch");

    reset(&menu,&ui);live.state=ONLINE_UPDATE_DOWNLOADING;
    assert(config_menu_poll_online_update(&menu));
    for(unsigned n=1;n<=100;++n) {
        live.received=n*512;live.total=4761708;live.elapsed_ms=n;live.bytes_per_second=n*1024;live.sd_writes=n/10;
        assert(!config_menu_poll_online_update(&menu));
        assert(menu.online_update.received==n*512 && menu.online_update.elapsed_ms==n);
    }
    strcpy(live.stage,"Image validation");assert(config_menu_poll_online_update(&menu));
    live.state=ONLINE_UPDATE_FAILED;assert(config_menu_poll_online_update(&menu));
    reset(&menu,&ui);service_active=1;unsigned paints=0;
    for(unsigned millisecond=0;millisecond<=1000;++millisecond) {
        clock_ticks=(XTime)millisecond*1000;
        paints+=ui_online_update_composite_due(&ui,&menu);force_refresh=0;
    }
    assert(paints==11); /* first paint, then ten 100ms intervals */
    clock_ticks+=1;++ui.input_seq;assert(ui_online_update_composite_due(&ui,&menu) && force_refresh);
    force_refresh=0;clock_ticks+=1;assert(!ui_online_update_composite_due(&ui,&menu));
    force_refresh=1;assert(ui_online_update_composite_due(&ui,&menu));force_refresh=0;
    ++ui.input_seq;live.state=ONLINE_UPDATE_FAILED;assert(ui_online_update_composite_due(&ui,&menu));force_refresh=0;
    service_active=0;assert(ui_online_update_composite_due(&ui,&menu) && force_refresh);
    force_refresh=0;assert(ui_online_update_composite_due(&ui,&menu));
    puts("PASS fresh progress snapshots without per-chunk invalidation;11 paints over1001 modeled1ms iterations, input/error/full-refresh/completion remain immediate");
    for(unsigned phase=0;phase<3;++phase) {
        reset(&menu,&ui);
        service_active=phase!=1;http_owner=phase!=0?HTTP_OWNER_UPDATE:HTTP_OWNER_NONE;
        g_ethernet_dhcp_started=123;
        char message[64]={0};uthernet2_test_result_t test={0};
        assert(menu_platform_ethernet_write_config(NULL,&menu.ethernet_config)!=0 && !nic_writes && !timing_logs);
        assert(menu_platform_ethernet_dhcp_start(NULL,menu.ethernet_config.mac,message,sizeof(message))!=0);
        assert(!dhcp_starts && strstr(message,"ONLINE UPDATE"));
        assert(menu_platform_ethernet_dhcp_start(NULL,menu.ethernet_config.mac,NULL,0)!=0);
        assert(menu_platform_ethernet_dhcp_poll(NULL,&menu.ethernet_config,message,sizeof(message))==0);
        assert(!dhcp_polls && strstr(message,"ONLINE UPDATE") && g_ethernet_dhcp_started==123);
        menu_platform_ethernet_dhcp_cancel(NULL);assert(!dhcp_cancels && g_ethernet_dhcp_started==123);
        assert(!menu_platform_ethernet_read_config(NULL,&menu.ethernet_config) && nic_reads==1);
        assert(!menu_platform_ethernet_test(NULL,&test) && nic_tests==1);
    }
    reset(&menu,&ui);http_owner=HTTP_OWNER_NONE;clock_ticks=123;
    char message[64]={0};
    assert(!menu_platform_ethernet_write_config(NULL,&menu.ethernet_config) && nic_writes==1 && timing_logs==1);
    assert(!menu_platform_ethernet_dhcp_start(NULL,menu.ethernet_config.mac,message,sizeof(message)) && dhcp_starts==1 && g_ethernet_dhcp_started==123);
    assert(!menu_platform_ethernet_dhcp_poll(NULL,&menu.ethernet_config,message,sizeof(message)) && dhcp_polls==1);
    menu_platform_ethernet_dhcp_cancel(NULL);assert(dhcp_cancels==1 && !g_ethernet_dhcp_started);
    const uint8_t blocked_states[]={ONLINE_UPDATE_CHECKING,ONLINE_UPDATE_DOWNLOADING,ONLINE_UPDATE_VERIFYING};
    menu.ethernet_slot1_enabled=1;
    for(unsigned n=0;n<sizeof(blocked_states);++n) {
        menu.online_update.state=blocked_states[n];
        assert(!config_menu_ethernet_card_access(&menu));
        assert(strstr(menu.status,"FINISH OR CANCEL ONLINE UPDATE"));
    }
    menu.online_update.state=ONLINE_UPDATE_IDLE;assert(config_menu_ethernet_card_access(&menu));
    onee_isolated=1;filter_hold=1;g_card_slot_enable_mask=CARD_CTRL_SLOT_ENABLE_REQUIRED_MASK|(1U<<1)|(1U<<5);
    card_control_apply_slot_policy();
    assert(g_card_slot_effective_mask==g_card_slot_enable_mask && filter_seen==g_card_slot_enable_mask);
    assert(slot_register==(g_card_slot_enable_mask&~(1U<<1)));
    filter_hold=0;card_control_apply_slot_policy();assert(slot_register==g_card_slot_enable_mask);
    puts("PASS all four mutating Ethernet callbacks defer during active UPDATE/restore ownership, read-only access remains, readable UI refusal and configured-vs-filtered slot policy preserved");
    puts("9 online main integration groups passed (native; service/flash mocked)");
    return 0;
}
