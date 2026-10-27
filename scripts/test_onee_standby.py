#!/usr/bin/env python3
"""Run the real standby eligibility, compositor helpers and RGB565 painter."""

from pathlib import Path
import subprocess
import textwrap

from test_joystick_config_menu import function
from test_onee_input_service import find_native_c_compiler

ROOT = Path(__file__).resolve().parents[1]
FRONT = ROOT / "ps_sources/frontend"
OUT = ROOT / "build/onee_standby_test"


def native(name: str, code: str, sources: tuple[str, ...] = ()) -> None:
    compiler = find_native_c_compiler()
    if compiler is None:
        raise RuntimeError("Standby checks require a host C compiler")
    path = OUT / (name + ".c")
    executable = OUT / (name + ".exe")
    path.write_text(textwrap.dedent(code), encoding="utf-8")
    # Match ARM's unsigned plain char when compiling the shared font renderer.
    command = [str(compiler), "-std=c11", "-funsigned-char", "-Wall", "-Wextra", "-Werror", "-O2",
               "-I", str(FRONT), str(path)]
    command.extend(str(ROOT / source) for source in sources)
    if "mingw" in str(compiler).lower():
        command.append("-static")
    command.extend(["-o", str(executable)])
    subprocess.run(command, cwd=ROOT, check=True)
    subprocess.run([str(executable)], cwd=OUT, check=True)


def service_test() -> None:
    native("policy", r'''
        #include <assert.h>
        #include <stdint.h>
        #include <stdio.h>
        static uint32_t mode_status, high_writes, writes;
        #define COMMON_H
        #define REG_READ(address) ((void)(address), mode_status)
        #define REG_WRITE(address,value) ((void)(address), ++writes, high_writes += ((value) != 0U))
        #include "onee_service.c"
        static uint8_t start(void *ctx) { (void)ctx; return 1U; }
        static void stop(void *ctx) { (void)ctx; }
        static uint32_t safe_off(void) {
            return (CARD_CTRL_ONEE_STATUS_SIGNATURE << CARD_CTRL_ONEE_STATUS_SIGNATURE_SHIFT) |
                CARD_CTRL_ONEE_STATUS_HDL_PRESENT_BIT | CARD_CTRL_ONEE_STATUS_OUTPUTS_OFF_BIT |
                CARD_CTRL_ONEE_STATUS_QUIET_BIT | CARD_CTRL_ONEE_STATUS_RESELECT_ARMED_BIT |
                (CARD_CTRL_ONEE_INHIBIT_MANUAL_OFF << CARD_CTRL_ONEE_STATUS_INHIBIT_SHIFT);
        }
        int main(void) {
            mode_status=safe_off(); onee_service_init();
            assert(!onee_service_standby_available()); /* No runtime yet. */
            onee_service_bind_runtime(start,stop,stop,start,NULL);
            unsigned before=writes;
            for (unsigned i=0;i<128;++i) assert(onee_service_standby_available());
            assert(writes==before && !high_writes && !g_manual_request && !g_persisted_intent);
            const uint32_t blocked[]={CARD_CTRL_ONEE_STATUS_ACTIVITY_BIT,CARD_CTRL_ONEE_STATUS_APPLE_POWER_BIT,
                CARD_CTRL_ONEE_STATUS_LOCKOUT_BIT,CARD_CTRL_ONEE_STATUS_REQUEST_BIT,
                CARD_CTRL_ONEE_STATUS_EFFECTIVE_BIT,CARD_CTRL_ONEE_STATUS_SELECTED_BIT,
                CARD_CTRL_ONEE_STATUS_ISOLATED_BIT};
            for (unsigned i=0;i<sizeof(blocked)/sizeof(blocked[0]);++i) {
                g_status=safe_off()|blocked[i]; assert(!onee_service_standby_available());
            }
            g_status=safe_off()&~CARD_CTRL_ONEE_STATUS_QUIET_BIT; assert(!onee_service_standby_available());
            g_status=safe_off()&~CARD_CTRL_ONEE_STATUS_RESELECT_ARMED_BIT; assert(!onee_service_standby_available());
            g_status=safe_off()&~CARD_CTRL_ONEE_STATUS_HDL_PRESENT_BIT; assert(!onee_service_standby_available());
            g_status=0U; assert(!onee_service_standby_available());
            g_status=safe_off();
            onee_service_restore_persisted(1U); assert(!onee_service_standby_available());
            onee_service_restore_persisted(0U); assert(onee_service_standby_available());
            assert(onee_service_request_start()); assert(!onee_service_standby_available());
            onee_service_request_stop(); before=high_writes;
            for (unsigned i=0;i<512;++i) { onee_service_poll(); assert(onee_service_standby_available()); }
            assert(high_writes==before && !g_persisted_intent && !g_manual_request);
            assert(onee_service_request_start()); before=high_writes;
            mode_status=safe_off()|CARD_CTRL_ONEE_STATUS_ACTIVITY_BIT;
            onee_service_poll(); assert(!onee_service_standby_available());
            mode_status=safe_off();
            for (unsigned i=0;i<512;++i) onee_service_poll();
            assert(onee_service_standby_available() && high_writes==before && !g_manual_request);
            assert(g_lockout_latched); /* Offering setup must not re-arm a stopped session. */
            puts("PASS standby is read-only, OFF stays OFF, restore waits and safety stops need manual restart");
            return 0;
        }
    ''')


def compositor_test() -> None:
    source = (FRONT / "main.c").read_text(encoding="utf-8")
    compose = function(source, "ui_compose_frame")
    assert compose.index("g_onee_standby_available != 0U && !menu_active") < compose.index("ui_prepare_static_background")
    code = r'''
        #include <assert.h>
        #include <stdint.h>
        #include <stdio.h>
        #include <string.h>
        #define COMP_OUT_SLOT_COUNT 3U
        #define APPLE_FB_DISPLAY_MODE_LEGACY 0U
        #define MB_PRODUCT_NONE 0U
        typedef struct { uint8_t active; } config_menu_t;
        typedef enum { COMPOSITOR_UI_PHASE_BASE, COMPOSITOR_UI_PHASE_OVERLAY } compositor_ui_phase_t;
        typedef struct { int x,y,w,h; } screenshot_service_rect_t;
        static uint8_t g_onee_standby_available,g_output_slot_standby[3];
        static uint32_t g_output_slot_bg_generation[3];
        static uint8_t g_output_slot_bg_show_bezel[3],g_output_slot_apple_mode[3],g_output_slot_debug_dirty[3];
        static uint8_t g_golden_boot_update_request,g_firmware_switch_active,g_firmware_switch_fault;
        static uint32_t g_firmware_switch_request;
        static unsigned eligible=1U,session,start_pending,handoff,refreshes,input_mode,draws,overlays,restore;
        static uint8_t onee_service_standby_available(void) { return eligible; }
        static uint8_t vtw_service_session_active(void) { return session; }
        static uint8_t vtw_service_boot_start_pending(void) { return start_pending; }
        static uint8_t vtw_service_boot_handoff_pending(void) { return handoff; }
        static void compositor_request_full_refresh(void) { ++refreshes; }
        static void usb_hid_service_set_onee_standby_mode(uint8_t mode) { input_mode=mode; }
        static uint8_t comp_out_addr_to_slot(uint32_t address) {
            return address>=0x1000U && address<=0x3000U ? (uint8_t)(address/0x1000U-1U) : 0xFFU;
        }
        static void cmui_onee_standby_draw(uint16_t *fb) { (void)fb; ++draws; }
        static uint8_t screenshot_service_restore_rect_for_frame(uint16_t *fb,screenshot_service_rect_t *rect) {
            (void)fb;(void)rect;unsigned result=restore;restore=0U;return result;
        }
        static void screenshot_service_draw_overlay(uint16_t *fb) { (void)fb; ++overlays; }
    '''
    for name in ("ui_invalidate_static_backgrounds", "ui_mark_slot_dynamic",
                 "ui_sync_onee_standby", "ui_compose_onee_standby"):
        code += function(source, name)
    code += r'''
        int main(void) {
            config_menu_t menu={0};
            ui_sync_onee_standby(&menu); assert(g_onee_standby_available && input_mode && refreshes==1U);
            ui_sync_onee_standby(&menu); assert(refreshes==1U);
            for (unsigned frame=0;frame<9;++frame) {
                uint16_t *fb=(uint16_t *)(uintptr_t)((frame%3U+1U)*0x1000U);
                assert(ui_compose_onee_standby(fb,COMPOSITOR_UI_PHASE_BASE));
                assert(ui_compose_onee_standby(fb,COMPOSITOR_UI_PHASE_OVERLAY));
            }
            assert(draws==3U && overlays==9U); /* Paint once per output slot. */
            restore=1U; ui_compose_onee_standby((uint16_t *)0x1000U,COMPOSITOR_UI_PHASE_BASE);
            assert(draws==4U);
            ui_mark_slot_dynamic((uint16_t *)0x2000U); /* A menu replaced this slot. */
            ui_compose_onee_standby((uint16_t *)0x2000U,COMPOSITOR_UI_PHASE_BASE); assert(draws==5U);
            ui_invalidate_static_backgrounds(); /* Output change/full refresh. */
            ui_compose_onee_standby((uint16_t *)0x1000U,COMPOSITOR_UI_PHASE_BASE); assert(draws==6U);
            unsigned *busy[]={&session,&start_pending,&handoff};
            for (unsigned i=0;i<3;++i) {
                *busy[i]=1U;ui_sync_onee_standby(&menu);assert(!input_mode && !g_onee_standby_available);
                *busy[i]=0U;ui_sync_onee_standby(&menu);assert(input_mode);
            }
            g_golden_boot_update_request=1U;ui_sync_onee_standby(&menu);assert(!input_mode);
            g_golden_boot_update_request=0U;g_firmware_switch_request=2U;ui_sync_onee_standby(&menu);assert(!input_mode);
            g_firmware_switch_request=0U;g_firmware_switch_active=1U;ui_sync_onee_standby(&menu);assert(!input_mode);
            g_firmware_switch_active=0U;g_firmware_switch_fault=1U;ui_sync_onee_standby(&menu);assert(!input_mode);
            g_firmware_switch_fault=0U;eligible=0U;ui_sync_onee_standby(&menu);assert(!input_mode);
            eligible=1U;ui_sync_onee_standby(NULL);assert(!input_mode);
            puts("PASS standby cache, screenshot restore, menu/output cleanup and startup/maintenance exclusions");
            return 0;
        }
    '''
    native("compositor", code)


def render_test() -> None:
    native("render", r'''
        #include <assert.h>
        #include <stdint.h>
        #include <stdio.h>
        #include <stdlib.h>
        #include "config_menu_ui.c"
        static void check_frame(uint16_t *fb,int w,int h) {
            int left=w,right=0,top=h,bottom=0;
            unsigned ink=0;
            for (int y=0;y<h;++y) for (int x=0;x<w;++x) if (fb[y*w+x]) {
                if (x<left) left=x;
                if (x>right) right=x;
                if (y<top) top=y;
                if (y>bottom) bottom=y;
                ++ink;
            }
            assert(ink>1000U && left>20 && right<w-20 && top>20 && bottom<h-20);
            assert(abs((left+right)/2-w/2)<w/20 && abs((top+bottom)/2-h/2)<h/12);
        }
        int main(void) {
            static const int modes[][2]={{1024,768},{1200,800},{1280,1024},{1680,1050},{1920,1080},{1360,768}};
            cmui_onee_standby_draw(NULL);
            for (unsigned mode=0;mode<6;++mode) {
                const int w=modes[mode][0],h=modes[mode][1];
                const size_t count=(size_t)w*(size_t)h;
                uint16_t *memory=malloc((count+64U)*sizeof(*memory)); assert(memory);
                for (size_t i=0;i<count+64U;++i) memory[i]=0xAD5AU;
                assert(fb16_set_size(w,h)); cmui_onee_standby_draw(memory+32);
                for (unsigned i=0;i<32;++i) assert(memory[i]==0xAD5AU && memory[count+32U+i]==0xAD5AU);
                check_frame(memory+32,w,h);
                assert(s_cmui_logo_ready && s_cmui_logo_w==440U && s_cmui_logo_h==78U);
                if (w==1024 || w==1920) {
                    char path[64]; snprintf(path,sizeof(path),"standby-%dx%d.ppm",w,h);
                    FILE *file=fopen(path,"wb"); assert(file);
                    fprintf(file,"P6\n%d %d\n255\n",w,h);
                    for (size_t i=0;i<count;++i) {
                        const uint32_t color=fb16_to_bgra32(memory[i+32U]);
                        const unsigned char rgb[]={(unsigned char)(color>>16),(unsigned char)(color>>8),(unsigned char)color};
                        assert(fwrite(rgb,1,3,file)==3U);
                    }
                    assert(!fclose(file));
                }
                free(memory);
            }
            free(s_cmui_logo_pixels);s_cmui_logo_pixels=NULL;s_cmui_logo_ready=0U;s_cmui_logo_failed=1U;
            assert(fb16_set_size(1024,768));
            uint16_t *fallback=malloc(1024U*768U*2U);assert(fallback);
            cmui_onee_standby_draw(fallback);check_frame(fallback,1024,768);free(fallback);
            puts("PASS real logo/text rendering in all six output modes, centering, bounds and decode-failure fallback");
            return 0;
        }
    ''', ("ps_sources/lib/fb16.c", "ps_sources/lib/lodepng.c",
          "ps_sources/frontend/config_menu_logo_png.c"))


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    service_test()
    compositor_test()
    render_test()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
