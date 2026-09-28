#!/usr/bin/env python3
"""Compile the real joystick settings helpers; exercise persistence and navigation.

Also renders the actual settings page and USB help in each availability state
to build/joystick_config_menu for visual review (no FPGA tools).
"""
from pathlib import Path
import os
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
FRONT = ROOT / "ps_sources/frontend"
OUT = ROOT / "build/joystick_config_menu"


def function(source: str, name: str) -> str:
    match = re.search(r"(?m)^(?:static\s+)?(?:const\s+)?\w+\s+\**" + name + r"\([^;]*?\)\s*\{", source)
    if not match:
        raise AssertionError(f"missing definition {name}")
    start = match.start()
    pos = match.end()
    depth = 1
    while depth:
        depth += (source[pos] == "{") - (source[pos] == "}")
        pos += 1
    return source[start:pos] + "\n"


def main() -> None:
    menu = (FRONT / "config_menu.c").read_text()
    tabs = (FRONT / "config_menu_device_tabs.c").read_text()
    main_c = (FRONT / "main.c").read_text()
    service = (FRONT / "onee_input_service.c").read_text()
    public_header = (FRONT / "config_menu.h").read_text()
    internal_header = (FRONT / "config_menu_internal.h").read_text()
    # Check real integration around the independently compiled helpers.
    assert "config_menu_parse_joystick(menu, key, value)" in menu
    assert "APPEND_CFG(\"%s\", joystick_line)" in menu
    assert "onee_input_service_default_joystick_config(&menu->joystick_config);" in function(menu, "config_menu_init")
    assert "onee_input_service_default_joystick_config(&menu->joystick_config);" in function(menu, "config_menu_reset_settings_only")
    assert "onee_input_service_set_joystick_config(&menu->joystick_config);" in function(menu, "config_menu_apply_boot_runtime_internal")
    assert "menu->joystick_page_active = 1U;" in function(menu, "config_menu_activate_item")
    assert "config_menu_joystick_handle_input(menu, input)" in function(menu, "config_menu_handle_input")
    assert "menu->joystick_page_active = 0U;" in function(menu, "config_menu_set_active")
    assert "menu->joystick_page_active" in function(main_c, "ui_config_menu_has_close_consumer")
    assert "usb_hid_service_set_joystick_preview" in function(main_c, "ui_sync_usb_menu_capture")
    assert "onee_input_service_set_joystick_config" in main_c[main_c.index("usb_hid_service_init();"):main_c.index("usb_hid_service_init();") + 500]
    assert "onee_input_service_get_joystick_snapshot" in function(tabs, "config_menu_draw_joystick")
    assert "config_menu_joystick_config_line(menu" in function(menu, "config_menu_save_settings_to_path")
    assert "config_menu_save_settings_to_path(menu" in function(menu, "config_menu_save_profile_settings")
    assert "config_menu_read_settings_from_path(menu, cfg_path, 1U" in function(menu, "config_menu_load_profile_settings")

    OUT.mkdir(parents=True, exist_ok=True)
    compiler = shutil.which("gcc") or shutil.which("clang")
    if not compiler:
        candidate = Path("E:/AMDDesignTools/2025.2/tps/mingw/10.0.0/win64.o/nt/bin/gcc.exe")
        if candidate.exists():
            compiler = str(candidate)
    if not compiler:
        raise RuntimeError("A native C compiler is required")
    env = dict(os.environ, PATH=str(Path(compiler).parent) + os.pathsep + os.environ.get("PATH", ""))
    functions = "\n".join(function(menu, name) for name in (
        "config_menu_ascii_lower", "config_menu_str_ieq", "config_menu_bool_text",
        "config_menu_on_off", "config_menu_parse_indexed_config_key",
        "config_menu_joystick_source_text", "config_menu_joystick_percent",
        "config_menu_parse_joystick", "config_menu_joystick_config_line",
        "config_menu_joystick_handle_input"))
    code = r"""
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "onee_input_service.h"
#include "config_menu_ui.h"
#include "config_menu_help.h"
"""
    # Keep the real tab IDs, row IDs, and mode enum without pulling in the
    # target-only hardware headers or the full config-menu state structure.
    code += public_header[public_header.index("#define CONFIG_MENU_STATUS_LEN"):
                          public_header.index("typedef struct {")]
    code += internal_header[internal_header.index("/* Coordinate and palette"):
                            internal_header.index("uint8_t config_menu_appendf(")]
    uart = (FRONT / "uart_control.h").read_text()
    code += uart[uart.index("typedef enum {"):uart.index("typedef struct {\n    uint32_t frame_count")]
    code += r"""
typedef struct {
    onee_input_joystick_config_t joystick_config;
    uint8_t joystick_page_active, joystick_paddle, joystick_focus;
    uint8_t onee_mode_state, usb_owned, clock_enabled, vtw_enabled;
    uint8_t sdd_stream_enabled, usb0_sd_remote_active;
    unsigned tab, item_focus;
} config_menu_t;
static unsigned save_count, apply_count;
static onee_input_joystick_config_t applied;
static uint8_t snapshot_connected = 1, snapshot_active = 1;
static uint8_t snapshot_slots = 0x81;
static void config_menu_save_settings(config_menu_t *m) { (void)m; ++save_count; }
void onee_input_service_set_joystick_config(const onee_input_joystick_config_t *c)
{ applied = *c; ++apply_count; }
void onee_input_service_get_joystick_snapshot(onee_input_joystick_snapshot_t *s)
{
    memset(s, 0, sizeof(*s));
    s->connected = snapshot_connected; s->active = snapshot_active;
    s->owner_slot = ONEE_INPUT_DEVICE_SLOT_COUNT;
    memset(s->paddle_slots, ONEE_INPUT_DEVICE_SLOT_COUNT, sizeof(s->paddle_slots));
    if (!s->connected) {
        memset(s->paddles, 128, sizeof(s->paddles));
        return;
    }
    s->axis_valid_mask = 0x3f; s->buttons = 5;
    s->connected_mask = snapshot_slots;
    for (unsigned slot=0; slot<ONEE_INPUT_DEVICE_SLOT_COUNT; ++slot) {
        if (!(snapshot_slots & (1U<<slot))) continue;
        s->devices[slot].connected=1; s->devices[slot].axis_valid_mask=0x3f;
        s->devices[slot].buttons = slot == 0 ? 5 : 2;
        for (unsigned i=0; i<6; ++i) s->devices[slot].axis[i]=(uint8_t)((slot*16+i*37)%256);
        if (s->owner_slot == ONEE_INPUT_DEVICE_SLOT_COUNT) s->owner_slot=(uint8_t)slot;
    }
    for (unsigned i=0; i<6; ++i) s->axis[i] = (uint8_t)(i*51);
    s->paddles[0]=0; s->paddles[1]=128; s->paddles[2]=255; s->paddles[3]=63;
}
static void hgr_draw_item(uint16_t *f, int x,int y,int w,uint8_t focused,const char *t,uint32_t color)
{ (void)color; cmui_row(f,x,y,w,focused,0,t); }
static void hgr_draw_item_dimmed(uint16_t *f, int x,int y,int w,uint8_t focused,const char *t)
{ cmui_row(f,x,y,w,focused,1,t); }
static uint8_t config_menu_onee_fixed_bindings_active(const config_menu_t *m)
{ return m->onee_mode_state == CONFIG_MENU_ONEE_MODE_RUNNING; }
static uint8_t config_menu_video_pal_accurate_help_visible(const config_menu_t *m)
{ (void)m; return 0; }
"""
    code += function(service, "onee_input_service_default_joystick_config")
    code += functions
    code += function(menu, "hgr_draw_check_item") + function(menu, "hgr_draw_value_item")
    code += r"""
static char shown_device[64], shown_raw_title[64], shown_raw_x[32], shown_buttons[32];
static void record_value(uint16_t *fb,int x,int y,int w,uint8_t focus,const char *label,const char *value)
{
    if (!strcmp(label,"Source device:")) snprintf(shown_device,sizeof(shown_device),"%s",value);
    hgr_draw_value_item(fb,x,y,w,focus,label,value);
}
static void record_caption(uint16_t *fb,int x,int y,int w,const char *text)
{
    if (!strncmp(text,"Raw axes:",9)) snprintf(shown_raw_title,sizeof(shown_raw_title),"%s",text);
    if (!strncmp(text,"Buttons:",8)) snprintf(shown_buttons,sizeof(shown_buttons),"%s",text);
    cmui_caption(fb,x,y,w,text);
}
static void record_text(uint16_t *fb,int x,int y,const char *text,uint16_t fg,uint16_t bg,int scale)
{
    if (!strncmp(text,"X:",2)) snprintf(shown_raw_x,sizeof(shown_raw_x),"%s",text);
    cmui_text(fb,x,y,text,fg,bg,scale);
}
#define hgr_draw_value_item record_value
#define cmui_caption record_caption
#define cmui_text record_text
"""
    code += function(tabs, "config_menu_draw_joystick")
    code += "#undef hgr_draw_value_item\n#undef cmui_caption\n#undef cmui_text\n"
    code += function(tabs, "config_menu_draw_usb")
    help_source = (FRONT / "config_menu_help.c").read_text()
    # Compile all real help blocks and resolvers, using the real constants above.
    code += help_source.replace('#include "config_menu_internal.h"', '')
    code += re.search(r"(?m)^#define CONFIG_MENU_HELP_MAX_LINES .*", menu).group(0) + "\n"
    code += r"""
static const char *shown_help[CONFIG_MENU_HELP_MAX_LINES];
static uint32_t shown_help_count;
static unsigned compact_preview_rows, compact_focused_rows, compact_layout_checks;
static unsigned compact_largest_page, compact_smallest_view = 1000;
static char compact_raw[128];
void test_compact_bounds(unsigned first, unsigned visible, unsigned count)
{
    assert(first == 0 && count <= visible);
    ++compact_layout_checks;
    if (count > compact_largest_page) compact_largest_page = count;
    if (visible < compact_smallest_view) compact_smallest_view = visible;
}
void test_compact_row(const char *text, unsigned focused, int width, int available_width)
{
    assert(width <= available_width);
    compact_focused_rows += focused != 0;
    if (!strncmp(text,"Mapped (0-255):",15)) {
        assert(strstr(text,"P1:") && strstr(text,"P2:") && strstr(text,"P3:") && strstr(text,"P4:"));
        compact_preview_rows |= 1;
    }
    if (!strncmp(text,"Raw axes:",9)) {
        assert(strstr(text,"X:") && strstr(text,"Y:") && strstr(text,"Z:") &&
               strstr(text,"RX:") && strstr(text,"RY:") && strstr(text,"RZ:"));
        snprintf(compact_raw,sizeof(compact_raw),"%s",text);
        compact_preview_rows |= 2;
    }
    if (!strncmp(text,"Input buttons:",14)) compact_preview_rows |= 4;
}
static void record_help_panel(uint16_t *fb, const cmui_rect_t *rect, const char *title,
                              const char *const *lines, uint32_t count)
{
    assert(count <= CONFIG_MENU_HELP_MAX_LINES);
    shown_help_count = count;
    for (uint32_t i=0; i<count; ++i) shown_help[i] = lines[i];
    cmui_help_panel(fb,rect,title,lines,count);
}
#define cmui_help_panel record_help_panel
"""
    code += function(menu, "config_menu_draw_help")
    code += "#undef cmui_help_panel\n"
    code += r"""
static void press(config_menu_t *m, ui_key_t key)
{ assert(config_menu_joystick_handle_input(m, (ui_input_t){key,1,0})); }
static void render(config_menu_t *menu, const char *path)
{
    const unsigned compact = FB16_WIDTH == 1024 && FB16_HEIGHT == 768;
    static const char *const tabs[]={"Prof","Boot","Video","SP","Disk","CPU","Sound","Mouse","Net","TW","Clock","RAM","USB","Print","About"};
    uint16_t *fb=calloc(FB16_WIDTH*FB16_HEIGHT,sizeof(*fb)); assert(fb);
    cmui_rect_t nav,body,footer; cmui_screen_rects(&nav,&body,&footer);
    compact_preview_rows=0; compact_focused_rows=0; compact_raw[0]='\0';
    if (compact) {
        body=(cmui_rect_t){0,0,1480,812};
        cmui_compact_begin();
    } else {
        cmui_clear(fb); cmui_header(fb,"Appletini","Joystick settings preview",menu->usb_owned);
    }
    config_menu_draw_usb(fb,menu,body.x,body.y,body.w);
    cmui_rect_t help_rect={body.x,body.y+body.h-210,body.w,210};
    config_menu_draw_help(fb,menu,&help_rect);
    config_menu_help_block_t expected = menu->joystick_page_active ?
        config_menu_help_resolve_joystick(menu->joystick_focus) :
        config_menu_help_resolve(CONFIG_TAB_USB,menu->item_focus);
    assert(expected.count && shown_help_count == expected.count);
    for (uint32_t i=0; i<expected.count; ++i) {
        assert(shown_help[i] == expected.lines[i]);
        if (!compact) assert(cmui_text_width(shown_help[i],CMUI_SMALL_SCALE) <= help_rect.w-40);
    }
    if (compact) {
        cmui_compact_finish(fb,"USB",tabs,15,CONFIG_TAB_USB,"Settings saved",0,menu->usb_owned,0);
        assert(compact_preview_rows==7 && compact_focused_rows==1);
    } else {
        cmui_rect_t navrow={nav.x,nav.y+12*42,nav.w,34}; cmui_nav_item(fb,&navrow,"USB",1,1);
        cmui_footer(fb,&footer,"Settings saved",0,0,0);
    }
    if (!path) { free(fb); return; }
    FILE *ppm=fopen(path,"wb"); assert(ppm);
    fprintf(ppm,"P6\n%d %d\n255\n",FB16_WIDTH,FB16_HEIGHT);
    for(unsigned i=0;i<(unsigned)(FB16_WIDTH*FB16_HEIGHT);++i) {
        unsigned char rgb[3]={(fb[i]>>11)*255/31,((fb[i]>>5)&63)*255/63,(fb[i]&31)*255/31};
        fwrite(rgb,1,3,ppm);
    }
    fclose(ppm); free(fb);
}
int main(void)
{
    config_menu_t menu = {0}, loaded = {0};
    onee_input_service_default_joystick_config(&menu.joystick_config);
    onee_input_service_default_joystick_config(&loaded.joystick_config);
    for (unsigned p=0; p<4; ++p) {
        assert(menu.joystick_config.paddle[p].source == ONEE_INPUT_JOYSTICK_SOURCE_AUTO);
        assert(menu.joystick_config.paddle[p].device == 0);
        assert(menu.joystick_config.paddle[p].sensitivity_percent == 100);
        assert(!menu.joystick_config.paddle[p].invert && !menu.joystick_config.paddle[p].deadzone_percent);
    }
    assert(!config_menu_joystick_handle_input(&menu, (ui_input_t){UI_KEY_ENTER,1,0}));
    menu.joystick_page_active = 1;
    press(&menu, UI_KEY_LEFT); assert(menu.joystick_paddle == 3 && !save_count);
    press(&menu, UI_KEY_RIGHT); assert(menu.joystick_paddle == 0 && !save_count);
    press(&menu, UI_KEY_DOWN); press(&menu, UI_KEY_LEFT);
    assert(menu.joystick_config.paddle[0].device == ONEE_INPUT_DEVICE_SLOT_COUNT);
    assert(applied.paddle[0].device == ONEE_INPUT_DEVICE_SLOT_COUNT && apply_count == save_count);
    press(&menu, UI_KEY_ENTER); assert(menu.joystick_config.paddle[0].device == 0);
    for(unsigned device=1;device<=ONEE_INPUT_DEVICE_SLOT_COUNT;++device) {
        press(&menu,UI_KEY_RIGHT); assert(menu.joystick_config.paddle[0].device == device);
    }
    press(&menu,UI_KEY_RIGHT); assert(menu.joystick_config.paddle[0].device == 0);
    press(&menu, UI_KEY_DOWN); press(&menu, UI_KEY_LEFT);
    assert(menu.joystick_config.paddle[0].source == ONEE_INPUT_JOYSTICK_SOURCE_OFF);
    press(&menu, UI_KEY_ENTER); assert(menu.joystick_config.paddle[0].source == ONEE_INPUT_JOYSTICK_SOURCE_AUTO);
    press(&menu, UI_KEY_DOWN); press(&menu, UI_KEY_ENTER);
    assert(menu.joystick_config.paddle[0].invert == 1 && apply_count == save_count);
    press(&menu, UI_KEY_DOWN);
    for(unsigned i=0;i<40;++i) press(&menu, UI_KEY_LEFT);
    assert(menu.joystick_config.paddle[0].sensitivity_percent == 25);
    for(unsigned i=0;i<40;++i) press(&menu, UI_KEY_RIGHT);
    assert(menu.joystick_config.paddle[0].sensitivity_percent == 200);
    press(&menu, UI_KEY_DOWN);
    for(unsigned i=0;i<15;++i) press(&menu, UI_KEY_RIGHT);
    assert(menu.joystick_config.paddle[0].deadzone_percent == 50);
    for(unsigned i=0;i<15;++i) press(&menu, UI_KEY_LEFT);
    assert(menu.joystick_config.paddle[0].deadzone_percent == 0);

    /* Older profiles omit .device and keep Auto for each paddle. */
    for(unsigned p=0;p<4;++p) {
        char key[80]; snprintf(key,sizeof(key),"usb.joystick.paddle.%u.source",p);
        assert(config_menu_parse_joystick(&loaded,key,"X"));
        assert(loaded.joystick_config.paddle[p].device == 0);
    }

    /* Exercise all five persisted settings per paddle, with different values. */
    for(unsigned p=0;p<4;++p) {
        char key[80], value[16];
        snprintf(key,sizeof(key),"usb.joystick.paddle.%u.device",p);
        snprintf(value,sizeof(value),"%u",p+1); config_menu_parse_joystick(&menu,key,value);
        snprintf(key,sizeof(key),"usb.joystick.paddle.%u.source",p);
        assert(config_menu_parse_joystick(&menu,key,p == 0 ? "rx" : p == 1 ? "OFF" : p == 2 ? "Y" : "Auto"));
        snprintf(key,sizeof(key),"usb.joystick.paddle.%u.invert",p);
        config_menu_parse_joystick(&menu,key,p%2 ? "ON" : "OFF");
        snprintf(key,sizeof(key),"usb.joystick.paddle.%u.sensitivity",p);
        snprintf(value,sizeof(value),"%u",25+p*50); config_menu_parse_joystick(&menu,key,value);
        snprintf(key,sizeof(key),"usb.joystick.paddle.%u.deadzone",p);
        snprintf(value,sizeof(value),"%u",p*10); config_menu_parse_joystick(&menu,key,value);
    }
    for(unsigned p=0;p<4;++p) {
        char text[256], *line;
        config_menu_joystick_config_line(&menu,p,text,sizeof(text));
        line = strtok(text,"\n");
        while(line) {
            char *eq = strchr(line,'='); assert(eq); *eq++='\0';
            assert(config_menu_parse_joystick(&loaded,line,eq));
            line = strtok(NULL,"\n");
        }
    }
    assert(memcmp(&loaded.joystick_config,&menu.joystick_config,sizeof(menu.joystick_config)) == 0);
    /* Every shared input slot, including Auto, round-trips on all four paddles. */
    for(unsigned device=0;device<=ONEE_INPUT_DEVICE_SLOT_COUNT;++device) {
        for(unsigned p=0;p<4;++p) {
            char text[256], *line;
            menu.joystick_config.paddle[p].device=(uint8_t)device;
            config_menu_joystick_config_line(&menu,p,text,sizeof(text));
            if (!device) assert(strstr(text,".device=Auto\n"));
            line=strtok(text,"\n");
            while(line) {
                char *eq=strchr(line,'='); assert(eq); *eq++='\0';
                assert(config_menu_parse_joystick(&loaded,line,eq));
                line=strtok(NULL,"\n");
            }
            assert(loaded.joystick_config.paddle[p].device == device);
        }
    }
    static const char *invalid_devices[]={"0","9","-1","256","1.0","1garbage","", "99999999999999999", "Auto", "AUTO"};
    for(unsigned i=0;i<sizeof(invalid_devices)/sizeof(invalid_devices[0]);++i) {
        loaded.joystick_config.paddle[0].device=3;
        config_menu_parse_joystick(&loaded,"usb.joystick.paddle.0.device",invalid_devices[i]);
        assert(loaded.joystick_config.paddle[0].device == 0);
    }
    assert(!config_menu_parse_joystick(&loaded,"usb.joystick.paddle.4.source","X"));
    assert(!config_menu_parse_joystick(&loaded,"usb.joystick.paddle.-1.source","X"));
    config_menu_parse_joystick(&loaded,"usb.joystick.paddle.0.sensitivity","-1");
    assert(loaded.joystick_config.paddle[0].sensitivity_percent==25);
    config_menu_parse_joystick(&loaded,"usb.joystick.paddle.0.sensitivity","99999999999999999");
    assert(loaded.joystick_config.paddle[0].sensitivity_percent==200);
    config_menu_parse_joystick(&loaded,"usb.joystick.paddle.0.sensitivity","garbage");
    assert(loaded.joystick_config.paddle[0].sensitivity_percent==100);
    config_menu_parse_joystick(&loaded,"usb.joystick.paddle.0.deadzone","999");
    assert(loaded.joystick_config.paddle[0].deadzone_percent==50);
    config_menu_parse_joystick(&loaded,"usb.joystick.paddle.0.deadzone","-2");
    assert(loaded.joystick_config.paddle[0].deadzone_percent==0);
    config_menu_parse_joystick(&loaded,"usb.joystick.paddle.0.source","missing");
    assert(loaded.joystick_config.paddle[0].source==ONEE_INPUT_JOYSTICK_SOURCE_AUTO);
    menu.joystick_focus=6; unsigned saves=save_count;
    press(&menu,UI_KEY_RIGHT); assert(save_count==saves);
    press(&menu,UI_KEY_ENTER); assert(save_count==saves+1);
    for(unsigned p=0;p<4;++p) {
        assert(applied.paddle[p].source==ONEE_INPUT_JOYSTICK_SOURCE_AUTO);
        assert(applied.paddle[p].device==0);
        assert(applied.paddle[p].sensitivity_percent==100);
        assert(!applied.paddle[p].invert && !applied.paddle[p].deadzone_percent);
    }
    press(&menu,UI_KEY_TAB); assert(menu.joystick_focus==7);
    press(&menu,UI_KEY_TAB); assert(menu.joystick_focus==0);
    press(&menu,UI_KEY_SHIFT_TAB); assert(menu.joystick_focus==7);
    press(&menu,UI_KEY_ENTER); assert(!menu.joystick_page_active && menu.item_focus==3);
    menu.joystick_page_active=1; press(&menu,UI_KEY_ESC); assert(!menu.joystick_page_active);
    menu.joystick_page_active=1; press(&menu,UI_KEY_BACK); assert(!menu.joystick_page_active);

    menu.tab=CONFIG_TAB_USB; menu.usb_owned=1; menu.joystick_page_active=1;
    menu.joystick_focus=4; menu.joystick_paddle=2;
    render(&menu,"joystick.ppm");
    assert(!strcmp(shown_device,"Auto (Input 1)"));
    assert(!strcmp(shown_raw_x,"X: 0"));
    menu.joystick_config.paddle[2].device=8; menu.joystick_focus=1;
    render(&menu,"joystick_input8.ppm");
    assert(!strcmp(shown_device,"Input 8 (connected)"));
    assert(!strcmp(shown_raw_title,"Raw axes: Input 8"));
    assert(!strcmp(shown_raw_x,"X: 112") && !strcmp(shown_buttons,"Buttons: - 2 -"));
    menu.joystick_config.paddle[2].device=4;
    render(&menu,"joystick_input_unavailable.ppm");
    assert(!strcmp(shown_device,"Input 4 (unavailable)"));
    assert(!strcmp(shown_raw_x,"X: --") && !strcmp(shown_buttons,"Buttons: - - -"));
    menu.joystick_config.paddle[2].device=0; snapshot_slots=0x80;
    render(&menu,"joystick_auto_input8.ppm");
    assert(!strcmp(shown_device,"Auto (Input 8)"));
    assert(!strcmp(shown_raw_x,"X: 112"));
    snapshot_slots=0x81;
    snapshot_active=0;
    render(&menu,"joystick_inactive.ppm");
    menu.vtw_enabled=1;
    render(&menu,"joystick_waiting.ppm");
    menu.vtw_enabled=0;
    menu.usb_owned=0;
    render(&menu,"joystick_boot_menu.ppm");
    menu.usb_owned=1; snapshot_connected=0;
    render(&menu,"joystick_disconnected.ppm");
    assert(!strcmp(shown_device,"Auto (unavailable)"));
    assert(!strcmp(shown_raw_title,"Raw axes: no input connected") && !strcmp(shown_raw_x,"X: --"));
    snapshot_connected=1; menu.onee_mode_state=CONFIG_MENU_ONEE_MODE_RUNNING;
    render(&menu,"joystick_standalone_onee.ppm");
    menu.onee_mode_state=CONFIG_MENU_ONEE_MODE_OFF; snapshot_active=1;
    for(unsigned focus=0;focus<CONFIG_JOYSTICK_ITEM_COUNT;++focus) {
        char path[64];
        menu.joystick_focus=(uint8_t)focus;
        snprintf(path,sizeof(path),"joystick_help_%u.ppm",focus);
        render(&menu,path);
    }
    menu.joystick_page_active=0; menu.item_focus=CONFIG_USB_ITEM_JOYSTICK;
    render(&menu,"usb_joystick_help.ppm");
    menu.item_focus=2;
    render(&menu,"usb_general_help.ppm");
    /* Exercise real compact row collection and wrapping for every focused
     * control, including inactive guidance and missing input previews. */
    assert(fb16_set_size(1024,768));
    menu.joystick_page_active=1; menu.joystick_paddle=2;
    for(unsigned state=0;state<7;++state) {
        snapshot_active=state==1 || state==4 || state==5 || state==6 ? 0 : 1;
        snapshot_connected=state==3 ? 0 : 1;
        menu.vtw_enabled=state==4 ? 1 : 0;
        menu.onee_mode_state=state==5 ? CONFIG_MENU_ONEE_MODE_RUNNING : CONFIG_MENU_ONEE_MODE_OFF;
        menu.joystick_config.paddle[2].device=state==2 || state==6 ? 4 : state==3 ? 0 : 8;
        menu.joystick_focus=0;
        for(unsigned focus=0;focus<CONFIG_JOYSTICK_ITEM_COUNT;++focus) {
            char path[80];
            assert(menu.joystick_focus==focus);
            snprintf(path,sizeof(path),"joystick_compact_state%u_focus%u.ppm",state,focus);
            render(&menu,focus==1 || focus==7 ? path : NULL);
            if (state==2 || state==3 || state==6) {
                assert(strstr(compact_raw,"RX: --") && strstr(compact_raw,"RZ: --"));
            } else {
                assert(strstr(compact_raw,"X: 112") && strstr(compact_raw,"RZ: 41"));
            }
            press(&menu,UI_KEY_DOWN);
        }
        assert(menu.joystick_focus==0);
    }
    assert(compact_layout_checks==7*CONFIG_JOYSTICK_ITEM_COUNT);
    printf("PASS: 1024x768 compact controls/preview bounds, %u focus/state checks, largest page %u rows, smallest viewport %u rows\n",
           compact_layout_checks,compact_largest_page,compact_smallest_view);
    puts("PASS: joystick config round-trip, validation, navigation, apply/save, defaults, and rendering");
    return 0;
}
"""
    harness = OUT / "test.c"
    harness.write_text(code)
    # Audit the real compact layout at the point where it has computed the
    # visible rows and wrapped help, without duplicating that calculation.
    ui_source = (FRONT / "config_menu_ui.c").read_text()
    audit_point = "    cmui_clear(fb);\n    (void)snprintf(heading, sizeof(heading),"
    assert ui_source.count(audit_point) == 1
    audit = """    extern void test_compact_bounds(unsigned, unsigned, unsigned);
    extern void test_compact_row(const char *, unsigned, int, int);
    test_compact_bounds(first, visible, s_compact_count);
    for (uint32_t row = 0; row < s_compact_count; ++row) {
        test_compact_row(s_compact_rows[row].text, s_compact_rows[row].focused,
                         cmui_text_width(s_compact_rows[row].text, scale),
                         FB16_WIDTH - 4 * margin);
    }
"""
    ui_harness = OUT / "config_menu_ui.c"
    ui_harness.write_text(ui_source.replace(audit_point, audit + audit_point))
    exe = OUT / "test.exe"
    command = [compiler,"-std=c11","-O0","-funsigned-char","-Wall","-Wextra","-Werror","-I",str(FRONT),
        str(harness),str(ui_harness),str(FRONT / "config_menu_logo_png.c"),
        str(ROOT / "ps_sources/lib/fb16.c"),str(ROOT / "ps_sources/lib/lodepng.c"),"-o",str(exe)]
    subprocess.run(command, check=True, env=env)
    subprocess.run([str(exe)], cwd=OUT, check=True, env=env)
    try:
        from PIL import Image
        for preview in OUT.glob("*.ppm"):
            with Image.open(preview) as image:
                image.save(preview.with_suffix(".png"))
    except ImportError:
        pass
    print("PASS: joystick menu global/profile integration and startup hooks")
    print(f"Previews: {OUT}")


if __name__ == "__main__":
    main()
