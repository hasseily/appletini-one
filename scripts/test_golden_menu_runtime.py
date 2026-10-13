#!/usr/bin/env python3
"""Exercise the production deferred Golden update with injected failures."""
import os
from pathlib import Path
import shutil
import subprocess

from test_joystick_config_menu import function

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/golden-menu-runtime-test"

PREAMBLE = r'''
#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "card_control_regs.h"
#include "../lib/framebuffer.h"
#include "../lib/psdma.h"
typedef uint64_t XTime;
typedef struct { unsigned pending; char status[128]; } config_menu_t;
typedef struct { int updated,verified,reset_required; uint32_t boot_offset; char error_msg[96]; } golden_self_update_result_t;
enum { UART0_BASE=0,UART1_BASE=1 };
/* Match the BSP's deliberately unparenthesized expression. */
#define COUNTS_PER_SECOND 100000000U / 2U
#define GOLDEN_SELF_UPDATE_TRIAL_OFFSET 0x100000U
static unsigned fault,events,gsu_calls,holds,stops,finished,rebooted,prepare,restores,refreshes;
static unsigned reset_polls,reset_syncs,legacy_reads,legacy_writes,smartport_polls;
static unsigned handed_off,pending_count,dma_claims,dma_releases,dma_busy,dma_granted;
static unsigned reset_cleanups;
static psdma_owner_t dma_owner;
static uint8_t g_golden_boot_update_request,g_firmware_switch_active,g_firmware_switch_fault;
static uint32_t vtw_ctrl,onee,boot_register,reset_status;
static XTime ticks;
static jmp_buf reset_jump;
static char order[128];
static void event(char c) { order[events++]=c; order[events]=0; }
static uint32_t read_reg(unsigned reg) {
    if(reg==CARD_CTRL_SSC_STATUS_REG) return fault==3 ? 1U:0U;
    /* This old diagnostic address is unmapped in the retained FPGA. */
    if(reg==AT_DMA_CTRL_REG) { ++legacy_reads; return 0; }
    if(reg==CARD_CTRL_VTW_CTRL_REG) return vtw_ctrl;
    assert(reg==CARD_CTRL_APPLE_RESET_STATUS_REG);
    ++reset_polls;
    if((vtw_ctrl&CARD_CTRL_VTW_CTRL_APPLE_RES_BIT)&&fault!=5 &&
       (fault!=25 || reset_polls>=3)) reset_status &= ~CARD_CTRL_APPLE_RESET_RES_BIT;
    return reset_status;
}
static void write_reg(unsigned reg,uint32_t value) {
    if(reg==AT_DMA_CTRL_REG) { ++legacy_writes; return; }
    if(reg==CARD_CTRL_VTW_CTRL_REG) {
        if(!(value&CARD_CTRL_VTW_CTRL_APPLE_RES_BIT)&&strchr(order,'d')) {
            assert(reset_cleanups==((reset_status&CARD_CTRL_APPLE_RESET_RES_BIT)==0));
        }
        if(fault!=26) vtw_ctrl=value;
        if(value&CARD_CTRL_VTW_CTRL_APPLE_RES_BIT) {
            if(fault==5||fault==25) reset_status |= CARD_CTRL_APPLE_RESET_RES_BIT;
            if(fault==32) pending_count=1;
        }
        event('d');
    }
    else if(reg==CARD_CTRL_ONEE_MODE_REG) { onee=value; event('i'); }
    else { assert(reg==CARD_CTRL_FEATURE_ENABLE_REG && value==0); event('f'); }
}
#define REG_READ(a) read_reg(a)
#define REG_WRITE(a,v) write_reg(a,v)
static unsigned screenshot_service_busy(void) { return fault==1; }
static unsigned printer_service_job_active(void) { return fault==2; }
static unsigned config_menu_usb0_sd_remote_active(config_menu_t *m) { (void)m;return fault==4; }
static unsigned config_menu_ethernet_ftp_sd_remote_active(config_menu_t *m) { (void)m;return fault==21; }
static unsigned menu_platform_get_golden_boot_file_present(void *p) { assert(p==NULL);return fault!=6; }
static unsigned control_onee_isolated(void) { return fault==19 || fault==20; }
static unsigned boot_menu_service_host_bus_master_allowed(void) { return fault!=18 && fault!=22; }
static unsigned boot_menu_service_initial_menu_active(void) { return !handed_off; }
static int control_hold_quiet_connector(uint32_t *saved,uint8_t *requested) {
    *saved=onee;onee=1;*requested=1;event('i');return fault==18 ? -1:0;
}
static void XTime_GetTime(XTime *value) { ticks+=1250000;*value=ticks; }
static void card_control_sync_apple_reset_seq(void) {
    assert(!(read_reg(CARD_CTRL_APPLE_RESET_STATUS_REG)&CARD_CTRL_APPLE_RESET_RES_BIT));
    ++reset_syncs; event('a');
}
static unsigned vtw_service_onee_set_paused(unsigned paused) { assert(paused==1);event('p');return fault!=19; }
static unsigned smartport_service_has_pending(void) { return pending_count!=0; }
static void smartport_service_poll(void) {
    /* Reset clears the hardware command: draining afterwards is stale. */
    assert(!(vtw_ctrl&CARD_CTRL_VTW_CTRL_APPLE_RES_BIT));
    if(fault!=7 && pending_count) --pending_count;
    if(fault==28) handed_off=1;
    ++smartport_polls; event('s');
}
static void smartport_service_apple_reset(void) {
    assert(strchr(order,'d') && !holds); pending_count=0; ++reset_cleanups; event('k');
}
static int disk2_service_flush_dirty_now(void) { return fault==8 ? -1:0; }
static unsigned config_menu_flush_settings(config_menu_t *m) { assert(m->pending);event('c');return fault!=9; }
static int smartport_service_sync_sd(void) { event('s');return fault==10 ? -1:0; }
psdma_result_t psdma_acquire(psdma_owner_t owner) {
    assert(owner==PSDMA_OWNER_FIRMWARE_UPDATE);++dma_claims;
    assert(!pending_count && !strchr(order,'d') && !strchr(order,'p'));
    if(dma_owner!=PSDMA_OWNER_NONE) return PSDMA_ERR_OWNED;
    if(dma_busy) return PSDMA_ERR_BUSY;
    dma_owner=owner;dma_granted=1;event('q');return PSDMA_OK;
}
void psdma_release(psdma_owner_t owner) {
    assert(owner==PSDMA_OWNER_FIRMWARE_UPDATE && dma_owner==owner);
    dma_owner=PSDMA_OWNER_NONE;++dma_releases;event('l');
}
static void compositor_request_full_refresh(void) { ++refreshes; }
static int compositor_tick(void) { event('v');return 1; }
static int golden_self_update_run(const char *path,unsigned uart,unsigned mirror,golden_self_update_result_t *r) {
    assert(!strcmp(path,"0:/BOOT.BIN")&&uart==UART0_BASE&&mirror==UART1_BASE);
    assert(g_firmware_switch_active && strchr(order,'c') && strchr(order,'s'));
    assert(dma_owner==PSDMA_OWNER_FIRMWARE_UPDATE);
    assert(!holds&&!prepare&&!stops);++gsu_calls;event('g');
    r->updated=r->verified=r->reset_required=1;r->boot_offset=GOLDEN_SELF_UPDATE_TRIAL_OFFSET;
    if(fault==11) { strcpy(r->error_msg,"source validation failed");return -1; }
    if(fault==12) r->verified=0;
    if(fault==13) r->boot_offset=0;
    if(fault==23) r->updated=0;
    if(fault==24) r->reset_required=0;
    return 0;
}
static int control_prepare_golden_boot(uint32_t offset,uint32_t *saved) {
    assert(gsu_calls==1 && offset==GOLDEN_SELF_UPDATE_TRIAL_OFFSET && !holds);
    *saved=boot_register;boot_register=offset/0x8000U;++prepare;event('b');
    return fault==14 || fault==17 ? -1:0;
}
static int control_write_multiboot_checked(uint32_t value) { ++restores;event('r');boot_register=value;return fault==17 ? -1:0; }
static int amp_hold_core1(void) { assert(prepare==1 && gsu_calls==1);++holds;event('h');return fault==15 ? -1:0; }
static void card_control_write_slot_mask(uint32_t mask) { assert(mask==0 && holds==1);event('m'); }
static void control_apply_slot5_service(unsigned value) { assert(value==0); }
static void compositor_set_paused(unsigned value) { assert(value==1); }
static void vtw_service_onee_stop(void) {}
static void vtw_service_set_enabled(unsigned value) { assert(value==0); vtw_ctrl=0; event('t'); }
static void usb_hid_service_stop(void) { ++stops;event('u'); }
static void usb_sdd_service_stop(void) { ++stops; }
static int usb_storage_service_disconnect(void) { ++stops;return 0; }
static void Xil_DCacheFlush(void) { event('x'); }
static void uart_puts(unsigned uart,const char *text) { assert(uart==UART0_BASE && text); }
static void control_reboot_preselected_golden(void) {
    assert(boot_register==GOLDEN_SELF_UPDATE_TRIAL_OFFSET/0x8000U && holds==1 && stops==3);
    assert(strchr(order,'g')<strchr(order,'b') && strchr(order,'b')<strchr(order,'h'));
    if(fault!=20 && fault!=22) {
        assert(vtw_ctrl&CARD_CTRL_VTW_CTRL_APPLE_RES_BIT);
        assert(!(read_reg(CARD_CTRL_APPLE_RESET_STATUS_REG)&CARD_CTRL_APPLE_RESET_RES_BIT));
        assert(vtw_ctrl==CARD_CTRL_VTW_CTRL_APPLE_RES_BIT && !strchr(order,'t'));
    }
    ++rebooted;event('z');longjmp(reset_jump,1);
}
static void config_menu_golden_boot_update_finished(config_menu_t *m,const char *error) {
    assert(error && *error);++finished;m->pending=0;snprintf(m->status,sizeof(m->status),"%s",error);
}
'''

SCENARIOS = r'''
int main(void) {
    config_menu_t menu={0};
    assert(ticks_to_us(0)==0 && ticks_to_us(1)==0);
    assert(ticks_to_us(4999999)==99999 && ticks_to_us(5000000)==100000);
    assert(ticks_to_us(5000001)==100000 && ticks_to_us(5000050)==100001);
    assert(ticks_to_us(100000000000ULL)==2000000000U);
    write_reg(AT_DMA_CTRL_REG,AT_DMA_CTRL_HOLD_REQ_BIT);
    assert(read_reg(AT_DMA_CTRL_REG)==0 && legacy_reads==1 && legacy_writes==1);
    /* An already asserted reset still needs both request readback and the
     * physical pin status; keep the complete original control word. */
    uint32_t original=0x123400E0U|CARD_CTRL_VTW_CTRL_APPLE_RES_BIT, saved=0;
    uint8_t requested=0;
    vtw_ctrl=original; reset_status=0x55U; fault=0;
    assert(control_hold_apple_reset(&saved,&requested)==0);
    assert(saved==original && requested && vtw_ctrl==original && reset_syncs==1);
    control_service_golden_boot_update(&menu);
    assert(!gsu_calls&&!holds&&!finished);
    for(fault=0;fault<=32;++fault) {
        events=gsu_calls=holds=stops=finished=rebooted=prepare=restores=refreshes=0;
        reset_polls=reset_syncs=legacy_reads=legacy_writes=smartport_polls=0;
        dma_claims=dma_releases=dma_granted=reset_cleanups=0; handed_off=fault==27;
        dma_owner=fault==29 ? PSDMA_OWNER_SMARTPORT:PSDMA_OWNER_NONE;
        dma_busy=fault==30;
        pending_count=fault==7 ? 65:fault==31 ? 3:fault==28 ? 1:0;
        g_golden_boot_update_request=1;g_firmware_switch_active=g_firmware_switch_fault=0;
        menu.pending=1;menu.status[0]=0;order[0]=0;ticks=0;
        vtw_ctrl=0x123400E0U;reset_status=CARD_CTRL_APPLE_RESET_RES_BIT|0x55U;onee=0x40;boot_register=0x123;
        if(!setjmp(reset_jump)) control_service_golden_boot_update(&menu);
        assert(!g_golden_boot_update_request);
        assert(!legacy_reads&&!legacy_writes);
        if(fault==0||fault==16||fault==20||fault==25||fault==31) {
            assert(rebooted==1&&holds==1&&gsu_calls==1&&stops==3&&!finished);
            assert(menu.pending&&g_firmware_switch_active&&!g_firmware_switch_fault);
            assert(dma_claims==1&&!dma_releases&&dma_owner==PSDMA_OWNER_FIRMWARE_UPDATE);
            assert(!reset_cleanups);
        } else {
            assert(!rebooted&&!stops&&finished==1&&!menu.pending&&menu.status[0]);
            assert(holds==(fault==15 ? 1U:0U));
            if(fault==15||fault==17) {
                assert(g_firmware_switch_fault&&g_firmware_switch_active);
                assert(vtw_ctrl&CARD_CTRL_VTW_CTRL_APPLE_RES_BIT);
                assert(dma_owner==PSDMA_OWNER_FIRMWARE_UPDATE && !dma_releases);
                assert(!reset_cleanups);
            } else {
                assert(!g_firmware_switch_fault&&!g_firmware_switch_active&&vtw_ctrl==0x123400E0U&&onee==0x40);
                assert(dma_owner==(fault==29 ? PSDMA_OWNER_SMARTPORT:PSDMA_OWNER_NONE));
                assert(dma_releases==dma_granted);
                assert(reset_cleanups==(strchr(order,'d')!=NULL &&
                    (reset_status&CARD_CTRL_APPLE_RESET_RES_BIT)==0));
            }
            if(fault==14||fault==15||fault==17) assert(boot_register==0x123 && restores==1);
            else assert(!prepare&&!restores&&boot_register==0x123);
            if(fault<11||fault==18||fault==19||fault==21||fault==22||fault>=26) assert(!gsu_calls);
            else assert(gsu_calls==1);
            if(fault==11) assert(strstr(menu.status,"source validation failed"));
            if(fault==27||fault==28) assert(!reset_syncs&&smartport_polls==(fault==28) && !strchr(order,'d'));
            if(fault==27) assert(!dma_claims);
            if(fault==29||fault==30) assert(dma_claims==1 && !dma_granted && !strchr(order,'d') && !reset_syncs && !gsu_calls);
            if(fault==32) assert(!smartport_polls&&dma_claims==1&&dma_releases==1);
        }
    }
    puts("PASS: production deferred Golden update and physical-reset acknowledgement/readback/delay, unsupported legacy DMA, original-control recovery, and retained reset on safe halt");
    puts("PASS: real BSP frequency/2 expression and exact 100ms conversion boundary");
    return 0;
}
'''


def main():
    source = (ROOT / "ps_sources/frontend/main.c").read_text()
    service = function(source, "control_service_golden_boot_update")
    assert "amp_release_core1" not in service
    assert "mb_platform_request" not in service
    assert service.index("smartport_service_sync_sd()") < service.index("golden_self_update_run(")
    assert service.index("result.verified == 0") < service.index("amp_hold_core1()")
    assert "AT_DMA_CTRL_REG" not in service
    helpers = function(source, "ticks_to_us") + function(source, "control_golden_boot_update_allowed") + function(source, "control_hold_apple_reset")
    OUT.mkdir(parents=True, exist_ok=True)
    harness = OUT / "runtime.c"
    harness.write_text(PREAMBLE + helpers + service + SCENARIOS)
    compiler = shutil.which("gcc") or "C:/msys64/ucrt64/bin/gcc.exe"
    env = dict(os.environ, PATH=str(Path(compiler).parent) + os.pathsep + os.environ.get("PATH", ""))
    executable = OUT / "runtime.exe"
    subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O0",
                    "-I",str(ROOT / "ps_sources/frontend"),
                    str(harness), "-o", str(executable)], check=True, env=env)
    subprocess.run([str(executable)], check=True, env=env)


if __name__ == "__main__":
    main()
