#!/usr/bin/env python3
"""Run the real slot-2 parser, loaders, controls and compact menu renderer."""
from pathlib import Path
import os
import re
import subprocess

from test_joystick_config_menu import function
from test_onee_vtw_runtime import find_native_c_compiler


ROOT = Path(__file__).resolve().parents[1]
FRONT = ROOT / "ps_sources/frontend"
OUT = ROOT / "build/slot2_config_menu"


def main():
    menu = (FRONT / "config_menu.c").read_text()
    tabs = (FRONT / "config_menu_device_tabs.c").read_text()
    internal = (FRONT / "config_menu_internal.h").read_text()
    help_source = (FRONT / "config_menu_help.c").read_text()
    for name in ("config_menu_init", "config_menu_reset_settings_only",
                 "config_menu_load_settings", "config_menu_read_settings_from_path"):
        assert "config_menu_slot2_reset(menu);" in function(menu, name), name
    assert "config_menu_parse_slot2(menu, key, value)" in function(menu, "config_menu_parse_key_value")
    assert "config_menu_apply_slot2(menu);" in function(menu, "config_menu_apply_runtime_internal")
    assert "config_menu_slot2_adjust(menu, 1)" in function(menu, "config_menu_activate_item")
    assert "config_menu_slot2_adjust(menu, delta)" in menu
    serializer = function(menu, "config_menu_save_settings_to_path")
    assert 'APPEND_CFG("slot2.card=%s\\n"' in serializer
    assert 'APPEND_CFG("slot2.player%u.device=%s\\n"' in serializer
    assert 'config_menu_on_off(menu->slot2_card == SLOT2_CARD_MOUSE)' in serializer
    count = function(menu, "config_menu_tab_item_count")
    assert "config_menu_slot2_player_count(menu->slot2_card)" in count

    OUT.mkdir(parents=True, exist_ok=True)
    compiler = find_native_c_compiler()
    if compiler is None:
        candidate = Path("E:/AMDDesignTools/2025.2/tps/mingw/10.0.0/win64.o/nt/bin/gcc.exe")
        if candidate.exists():
            compiler = candidate
    if compiler is None:
        raise RuntimeError("slot-2 checks require a native C compiler")
    env = dict(os.environ, PATH=str(Path(compiler).parent) + os.pathsep + os.environ.get("PATH", ""))
    code = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "slot2_gamepad_service.h"
#include "config_menu_ui.h"
#include "display_modes.h"
'''
    code += re.search(r"typedef enum \{\s+CONFIG_TAB_PROFILES.*?} config_tab_t;", internal, re.S).group(0) + "\n"
    for label_array in ("k_tab_labels", "k_compact_tab_labels"):
        code += re.search(r"static const char \* const " + label_array + r"\[CONFIG_TAB_COUNT\] = \{.*?\n};", menu, re.S).group(0) + "\n"
    code += r'''
typedef struct {
    uint8_t slot2_card, slot2_card_explicit, slot2_player_devices[4];
    uint8_t mouse_slot2_enabled, mouse_sensitivity;
    uint8_t settings_loaded, session_only, vtw_turbo_enabled, size_multiplier;
    uint32_t tab, item_focus;
    struct {
        void *ctx;
        void (*set_slot2_card)(void *,uint8_t);
        void (*set_slot2_player_devices)(void *,const uint8_t [4]);
        void (*set_slot_enabled)(void *,uint8_t,uint8_t);
    } platform;
} config_menu_t;
static unsigned saves, card_applies, device_applies, legacy_applies, events;
static uint8_t applied_card, applied_devices[4], legacy_enabled;
static void config_menu_save_settings(config_menu_t *menu)
{ ++saves; menu->session_only=0; }
static void apply_devices(void *ctx,const uint8_t devices[4])
{ (void)ctx; ++device_applies; memcpy(applied_devices,devices,4); events=events*10+1; }
static void apply_card(void *ctx,uint8_t card)
{ (void)ctx; ++card_applies; applied_card=card; events=events*10+2; }
static void apply_legacy(void *ctx,uint8_t slot,uint8_t enable)
{ (void)ctx; assert(slot==2); ++legacy_applies; legacy_enabled=enable; }
#define MOUSE_CONTROL_SLOT 2U
'''
    for name in ("config_menu_ascii_lower", "config_menu_str_ieq", "config_menu_bool_text",
                 "config_menu_on_off", "config_menu_parse_indexed_config_key",
                 "config_menu_slot2_card_text", "config_menu_slot2_card_config",
                 "config_menu_slot2_device_text", "config_menu_slot2_device_config",
                 "config_menu_slot2_player_count", "config_menu_slot2_reset",
                 "config_menu_parse_slot2", "config_menu_apply_slot2", "config_menu_slot2_adjust",
                 "config_menu_ascii_lower_in_place", "config_menu_is_space", "config_menu_trim",
                 "config_menu_parse_config_line"):
        code += function(menu, name)
    code += r'''
typedef unsigned UINT;
typedef int FRESULT;
typedef struct { int unused; } FIL;
#define FR_OK 0
#define FR_NO_FILE 1
#define FR_DISK_ERR 2
#define FA_READ 1
#define APPLETINI_CFG_MAX 8192
#define CONFIG_DEFAULT_VTW_TURBO_ENABLED 0
static const char *cfg_contents;
static int cfg_read_result;
static FRESULT config_menu_open_cfg(FIL *file,unsigned flags)
{ (void)file; (void)flags; return FR_OK; }
static FRESULT config_menu_open_path(FIL *file,const char *path,unsigned flags)
{ (void)path; return config_menu_open_cfg(file,flags); }
static FRESULT f_read(FIL *file,char *buffer,unsigned size,unsigned *read)
{ (void)file; assert(strlen(cfg_contents)<size); *read=(unsigned)strlen(cfg_contents);
  memcpy(buffer,cfg_contents,*read); return cfg_read_result; }
static FRESULT f_close(FIL *file) { (void)file; return FR_OK; }
static void config_menu_set_status(config_menu_t *m,unsigned warning,const char *text)
{ (void)m; (void)warning; (void)text; }
static void config_menu_set_sd_error(config_menu_t *m,const char *text,FRESULT result)
{ (void)m; (void)text; (void)result; }
static void config_menu_reset_settings_only(config_menu_t *m)
{ config_menu_slot2_reset(m); m->mouse_sensitivity=100; }
static void config_menu_parse_key_value(config_menu_t *m,const char *key,const char *value)
{ (void)config_menu_parse_slot2(m,key,value); }
'''
    loaders = function(menu, "config_menu_load_settings") + function(menu, "config_menu_read_settings_from_path")
    for helper in sorted(set(re.findall(r"^    (config_menu_(?:coerce_\w+|migrate_\w+|usb_bindings_coerce))\(menu\);", loaders, re.M))):
        code += "#define " + helper + "(menu) ((void)(menu))\n"
    code += re.search(r"^#define APPLETINI_CFG_VERSION .*", menu, re.M).group(0) + "\n" + loaders
    code += r'''
static void record_string(uint16_t *fb,int x,int y,const char *text,uint16_t fg,uint16_t bg,int scale)
{
    assert(x>=0 && y>=0);
    assert(x+(int)strlen(text)*FB16_BUILTIN_FONT_ADVANCE_X*scale<=FB16_WIDTH);
    assert(y+FB16_BUILTIN_FONT_HEIGHT*scale<=FB16_HEIGHT);
    fb16_string_scaled(fb,x,y,text,fg,bg,scale);
}
#define fb16_string_scaled record_string
#include "config_menu_ui.c"
#undef fb16_string_scaled
'''
    code += "\n".join(re.findall(r"^#define MOUSE_SENSITIVITY_.*", menu, re.M)) + "\n"
    code += re.search(r"static const uint8_t k_mouse_sensitivity_steps.*?\n};", menu, re.S).group(0) + "\n"
    for name in ("config_menu_mouse_sensitivity_clamp", "config_menu_mouse_sensitivity_index",
                 "hgr_draw_mouse_sensitivity_item", "hgr_draw_value_item"):
        code += function(menu, name)
    code += function(tabs, "config_menu_draw_mouse")
    code += "static const char *const help[]={\n" + re.search(r"HELP\(mouse,\s*(.*?)\);", help_source, re.S).group(1) + "\n};\n"
    code += r'''
static void parse(config_menu_t *m,const char *key,const char *value)
{ assert(config_menu_parse_slot2(m,key,value)); }
static void save_preview(uint16_t *fb,const char *name)
{
    char path[100]; snprintf(path,sizeof(path),"%s.ppm",name);
    FILE *out=fopen(path,"wb"); assert(out);
    fprintf(out,"P6\n%d %d\n255\n",FB16_WIDTH,FB16_HEIGHT);
    for (int i=0;i<FB16_WIDTH*FB16_HEIGHT;++i) {
        uint32_t rgb=fb16_to_bgra32(fb[i]);
        fputc((rgb>>16)&255,out); fputc((rgb>>8)&255,out); fputc(rgb&255,out);
    }
    fclose(out);
}
int main(void)
{
    config_menu_t m={0};
    m.tab=CONFIG_TAB_MOUSE; m.mouse_sensitivity=65;
    m.platform.set_slot2_card=apply_card;
    m.platform.set_slot2_player_devices=apply_devices;
    m.platform.set_slot_enabled=apply_legacy;
    for (unsigned card=0;card<SLOT2_CARD_COUNT;++card) {
        for (unsigned order=0;order<2;++order) {
            config_menu_slot2_reset(&m);
            if (!order) parse(&m,"mouse.slot2.enabled","ON");
            parse(&m,"slot2.card",config_menu_slot2_card_config((uint8_t)card));
            if (order) parse(&m,"mouse.slot2.enabled","ON");
            assert(m.slot2_card==card);
            assert(m.mouse_slot2_enabled==(card==SLOT2_CARD_MOUSE));
        }
    }
    config_menu_slot2_reset(&m);
    parse(&m,"mouse.slot2.enabled","on"); assert(m.slot2_card==SLOT2_CARD_MOUSE);
    parse(&m,"slot2.card","invalid"); parse(&m,"mouse.slot2.enabled","ON");
    assert(m.slot2_card==SLOT2_CARD_OFF && !m.mouse_slot2_enabled);
    parse(&m,"slot2.card","snes_max"); assert(m.slot2_card==SLOT2_CARD_SNES_MAX);
    for (unsigned player=0;player<4;++player) {
        char key[32]; snprintf(key,sizeof(key),"slot2.player%u.device",player+1);
        for (unsigned device=0;device<=9;++device) {
            parse(&m,key,config_menu_slot2_device_config((uint8_t)device));
            assert(m.slot2_player_devices[player]==device);
        }
        const char *bad[]={"-1","9","255","1x","","USB 1"};
        for (unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i) {
            parse(&m,key,bad[i]); assert(m.slot2_player_devices[player]==0);
        }
    }
    assert(!config_menu_parse_slot2(&m,"slot2.player0.device","1"));
    assert(!config_menu_parse_slot2(&m,"slot2.player5.device","1"));
    assert(!config_menu_parse_slot2(&m,"slot2.player1.device.extra","1"));

    /* Actual global loader: failed read retains the live mode; a successful
     * retry with an old/missing key starts from Off/Auto, never stale state. */
    m.slot2_card=SLOT2_CARD_SNES_MAX; m.slot2_card_explicit=1; m.slot2_player_devices[3]=8;
    cfg_contents="appletini.config.version=119\nmouse.slot2.enabled=ON\n";
    cfg_read_result=FR_DISK_ERR; config_menu_load_settings(&m);
    assert(m.slot2_card==SLOT2_CARD_SNES_MAX && !m.settings_loaded);
    cfg_read_result=FR_OK; config_menu_load_settings(&m);
    assert(m.slot2_card==SLOT2_CARD_MOUSE && !m.slot2_card_explicit && !m.slot2_player_devices[3]);
    cfg_contents="appletini.config.version=120\n"; config_menu_load_settings(&m);
    assert(m.slot2_card==SLOT2_CARD_OFF && !m.mouse_slot2_enabled);
    cfg_contents="SLOT2.CARD=Four_Play # comment\nslot2.player1.device=8\nmouse.slot2.enabled=ON\n";
    assert(config_menu_read_settings_from_path(&m,"profile",1,NULL));
    assert(m.slot2_card==SLOT2_CARD_FOUR_PLAY && m.slot2_player_devices[0]==8);
    cfg_contents="mouse.slot2.enabled=ON\n";
    assert(config_menu_read_settings_from_path(&m,"profile",0,NULL));
    assert(m.slot2_card==SLOT2_CARD_MOUSE && !m.slot2_player_devices[0]);
    cfg_contents=""; assert(config_menu_read_settings_from_path(&m,"profile",1,NULL));
    assert(m.slot2_card==SLOT2_CARD_OFF && !m.slot2_card_explicit);

    m.mouse_sensitivity=65; m.item_focus=0; events=0;
    assert(config_menu_slot2_adjust(&m,1));
    assert(applied_card==SLOT2_CARD_MOUSE && m.mouse_slot2_enabled && events==12);
    assert(!config_menu_slot2_adjust(&(config_menu_t){.slot2_card=SLOT2_CARD_MOUSE,.item_focus=1},1));
    assert(config_menu_slot2_adjust(&m,1)); assert(applied_card==SLOT2_CARD_FOUR_PLAY);
    m.item_focus=4;
    for (unsigned i=1;i<=10;++i) {
        events=0; assert(config_menu_slot2_adjust(&m,1));
        assert(m.slot2_player_devices[3]==i%10 && applied_devices[3]==i%10 && events==12);
    }
    assert(config_menu_slot2_adjust(&m,-1)); assert(m.slot2_player_devices[3]==9);
    m.slot2_card=SLOT2_CARD_SNES_MAX; config_menu_apply_slot2(&m); assert(m.item_focus==2);
    m.slot2_card=SLOT2_CARD_MOUSE; config_menu_apply_slot2(&m); assert(m.item_focus==1);
    m.slot2_card=SLOT2_CARD_OFF; config_menu_apply_slot2(&m); assert(m.item_focus==0);
    assert(m.mouse_sensitivity==65 && legacy_applies==0 && device_applies==card_applies);
    m.platform.set_slot2_card=NULL;
    for (unsigned card=0;card<SLOT2_CARD_COUNT;++card) {
        m.slot2_card=(uint8_t)card; config_menu_apply_slot2(&m);
        assert(legacy_enabled==(card==SLOT2_CARD_MOUSE));
    }
    assert(legacy_applies==SLOT2_CARD_COUNT && saves>0);
    assert(strcmp(config_menu_on_off(m.slot2_card==SLOT2_CARD_MOUSE),"OFF")==0);

    /* Render every card and focused row at every supported output size. */
    unsigned render_cases=0;
    for (unsigned mode=0;mode<DISPLAY_MODE_COUNT;++mode) {
        const display_mode_t *d=display_mode_get((uint8_t)mode);
        assert(fb16_set_size(d->width,d->height));
        size_t pixels=(size_t)d->width*d->height;
        uint16_t *allocation=malloc((pixels+128)*sizeof(uint16_t)); assert(allocation);
        for (size_t i=0;i<pixels+128;++i) allocation[i]=0xA55A;
        uint16_t *fb=allocation+64;
        for (unsigned card=0;card<SLOT2_CARD_COUNT;++card) {
            m.slot2_card=(uint8_t)card;
            unsigned count=card==SLOT2_CARD_MOUSE ? 2 : 1+config_menu_slot2_player_count((uint8_t)card);
            for (unsigned focus=0;focus<count;++focus) {
                m.item_focus=focus; cmui_compact_begin();
                config_menu_draw_mouse(fb,&m,0,0,1480);
                assert(s_compact_count==count);
                unsigned focused=0;
                for (unsigned i=0;i<count;++i) focused+=s_compact_rows[i].focused!=0;
                assert(focused==1 && s_compact_rows[focus].focused);
                assert(strstr(s_compact_rows[0].text,config_menu_slot2_card_text((uint8_t)card)));
                cmui_help_panel(fb,NULL,"Help",help,sizeof(help)/sizeof(help[0]));
                cmui_compact_finish(fb,k_tab_labels[CONFIG_TAB_MOUSE],k_compact_tab_labels,
                    CONFIG_TAB_COUNT,CONFIG_TAB_MOUSE,"Settings saved",0,0,0);
                ++render_cases;
                if (!mode && !focus) {
                    char name[32]; snprintf(name,sizeof(name),"slot2_%u",card); save_preview(fb,name);
                }
                for (unsigned i=0;i<64;++i) {
                    assert(allocation[i]==0xA55A && allocation[pixels+64+i]==0xA55A);
                }
            }
        }
        free(allocation);
    }
    assert(render_cases==DISPLAY_MODE_COUNT*11);
    puts("PASS: slot2 legacy migration, key precedence, strict device values, global retry/profile reset, callbacks, controls, focus, all resolution text/framebuffer bounds");
    return 0;
}
'''
    harness = OUT / "test.c"
    harness.write_text(code)
    exe = OUT / "test.exe"
    subprocess.run([str(compiler), "-std=c11", "-O0", "-funsigned-char", "-Wall", "-Wextra", "-Werror",
                    "-I", str(FRONT), str(harness), str(FRONT / "config_menu_logo_png.c"),
                    str(ROOT / "ps_sources/lib/fb16.c"), str(ROOT / "ps_sources/lib/lodepng.c"),
                    "-o", str(exe)], check=True, env=env)
    subprocess.run([str(exe)], cwd=OUT, check=True, env=env)
    try:
        from PIL import Image
        for preview in OUT.glob("*.ppm"):
            with Image.open(preview) as image:
                image.save(preview.with_suffix(".png"))
    except ImportError:
        pass
    print(f"Previews: {OUT}")


if __name__ == "__main__":
    main()
