#!/usr/bin/env python3
"""Exercise actual local/online About update controls and all output layouts."""
from pathlib import Path
import os
import hashlib
import json
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
FRONT = ROOT / "ps_sources/frontend"
OUT = ROOT / "build/about-signature-tests"


def function(source: str, name: str) -> str:
    match = re.search(r"(?m)^(?:static\s+)?(?:const\s+)?\w+\s+\**" + name + r"\([^;]*?\)\s*\{", source)
    assert match, name
    pos, depth = match.end(), 1
    while depth:
        depth += (source[pos] == "{") - (source[pos] == "}")
        pos += 1
    return source[match.start():pos] + "\n"


def main() -> None:
    source = (FRONT / "config_menu.c").read_text(encoding="utf-8")
    assert "APPLETINI_BOOT_IMAGE_VERSION_FULL" not in function(source, "config_menu_draw_about")
    assert "config_menu_refresh_golden_boot(menu)" in function(source, "config_menu_on_tab_entered")
    assert "config_menu_refresh_golden_boot(menu)" in function(source, "config_menu_set_active")
    assert "config_menu_start_golden_boot_update(menu)" in function(source, "config_menu_activate_item")
    handler = function(source, "config_menu_handle_input")
    assert handler.index("config_menu_golden_boot_handle_input") < handler.index("input.key == UI_KEY_MENU")
    draws = "\n".join(function(source, name) for name in (
        "config_menu_draw_tabs", "config_menu_draw_about_section", "config_menu_about_columns",
        "config_menu_about_wrap", "config_menu_about_credit_lines", "config_menu_draw_about_credits",
        "config_menu_draw_update_message", "config_menu_draw_online_update_result",
        "config_menu_draw_online_update_row", "config_menu_draw_about",
        "config_menu_draw_golden_boot_modal", "config_menu_draw_online_update_modal"))
    assert "get_golden_boot" not in draws and "f_stat" not in draws
    assert "config_menu_draw_about_bottom_text" not in function(source, "config_menu_draw_about")
    assert "config_menu_about_reset_scroll" in function(source, "config_menu_set_active")
    main_source = (FRONT / "main.c").read_text()
    close_consumer = function(main_source, "ui_config_menu_has_close_consumer")
    assert "about_credits_active" in close_consumer
    assert handler.index("config_menu_about_handle_input") < handler.rindex("switch (input.key)")
    credits_hint = re.search(r'input_hint = ("Credits:[^"]*");', function(source, "config_menu_draw")).group(1)
    assert '"Hardware:' in source and '"Firmware:   Henri' in source
    assert 'Project origin - John \\"Elltwo\\" Flanagan' in source
    assert not re.search(r'And in no particular order:[^\n]*Elltwo', source)
    OUT.mkdir(parents=True, exist_ok=True)
    # Header-only I2C type: the real menu structure includes RTC/sensor types,
    # while this fixture never calls hardware I2C operations.
    (OUT / "xiicps.h").write_text("#ifndef TEST_XIICPS_H\n#define TEST_XIICPS_H\ntypedef struct { unsigned unused; } XIicPs;\n#endif\n")
    from test_onee_input_service import find_native_c_compiler
    compiler = find_native_c_compiler()
    assert compiler, "A native compiler is required"
    env = {k:v for k,v in os.environ.items() if k.lower() != "path"}
    env["PATH"] = str(compiler.parent) + os.pathsep + next((v for k,v in os.environ.items() if k.lower() == "path"), "")
    code = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config_menu.h"
#include "config_menu_ui.h"
#include "config_menu_help.h"
#include "../image_versions.h"
static unsigned version_reads, file_checks, allowed_checks, update_requests, reject_request;
static uint8_t file_present, update_allowed=1;
static online_update_snapshot_t online;
static unsigned online_reads, online_checks, online_downloads, online_cancels, online_installs;
static unsigned online_reject;
static uint8_t online_kind;
static void online_status(void *ctx, online_update_snapshot_t *status)
{ (void)ctx; ++online_reads; *status=online; }
static int online_check(void *ctx) { (void)ctx; ++online_checks; return online_reject; }
static int online_download(void *ctx,uint8_t kind)
{ (void)ctx; ++online_downloads; online_kind=kind; return online_reject; }
static int online_install(void *ctx,uint8_t kind)
{ (void)ctx; ++online_installs; online_kind=kind; return online_reject; }
static void online_cancel(void *ctx) { (void)ctx; ++online_cancels; }
static const char *installed_version = "B1.2.0";
static void read_version(void *ctx, char *dst, size_t cap)
{ (void)ctx; ++version_reads; snprintf(dst, cap, "%s", installed_version); }
static uint8_t read_present(void *ctx)
{ (void)ctx; ++file_checks; return file_present; }
static uint8_t read_allowed(void *ctx)
{ (void)ctx; ++allowed_checks; return update_allowed; }
static int request_update(void *ctx)
{ (void)ctx; ++update_requests; return (int)reject_request; }
static void config_menu_set_status(config_menu_t *menu, uint8_t warn, const char *text)
{ menu->status_warning=warn; snprintf(menu->status,sizeof(menu->status),"%s",text); }
static uint8_t config_menu_profiles_handle_input(config_menu_t *menu, ui_input_t in)
{ (void)menu; (void)in; return 0; }
static uint8_t config_menu_printing_handle_input(config_menu_t *menu, ui_input_t in)
{ (void)menu; (void)in; return 0; }
static uint8_t config_menu_joystick_handle_input(config_menu_t *menu, ui_input_t in)
{ (void)menu; (void)in; return 0; }
static uint8_t config_menu_text_reader_handle_input(ui_input_t in) { (void)in; return 0; }
static void config_menu_try_read_rtc(config_menu_t *menu,uint8_t status)
{ (void)menu; (void)status; }
void config_menu_stop_ethernet_ftp_sd_remote(config_menu_t *m) { (void)m; }
static void config_menu_browser_move(config_menu_t *m,int n) { (void)m; (void)n; }
static void config_menu_browser_parent(config_menu_t *m) { (void)m; }
static void config_menu_browser_select(config_menu_t *m) { (void)m; }
static uint8_t config_menu_browser_selected_file(config_menu_t *m,char *a,size_t b,char *c,size_t d)
{ (void)m; (void)a; (void)b; (void)c; (void)d; return 0; }
static void config_menu_printing_start_delete(config_menu_t *m,const char *a,const char *b)
{ (void)m; (void)a; (void)b; }
static void config_menu_browser_close(config_menu_t *m) { (void)m; }
static uint8_t config_menu_boot_usb_binding_move_vertical(config_menu_t *m,int n) { (void)m; (void)n; return 0; }
static uint8_t config_menu_boot_usb_binding_move_horizontal(config_menu_t *m,int n) { (void)m; (void)n; return 0; }
static void config_menu_next_tab(config_menu_t *m) { m->tab=(m->tab+1)%CONFIG_TAB_COUNT; }
static void config_menu_prev_tab(config_menu_t *m) { m->tab=(m->tab+CONFIG_TAB_COUNT-1)%CONFIG_TAB_COUNT; }
static void config_menu_prev_item(config_menu_t *m) { m->item_focus=0; }
static void config_menu_next_item(config_menu_t *m) { m->item_focus=0; }
static uint8_t config_menu_adjust_focused_value(config_menu_t *m,int d) { (void)m; (void)d; return 0; }
void config_menu_stop_usb0_sd_remote(config_menu_t *m) { (void)m; }
uint8_t config_menu_is_active(const config_menu_t *m) { return m && m->active; }
void config_menu_set_active(config_menu_t *m,uint8_t a) { m->active=a; }
void config_menu_toggle(config_menu_t *m) { m->active=!m->active; }
'''
    internal = (FRONT / "config_menu_internal.h").read_text()
    enums = internal[internal.index("/* Coordinate and palette"):
                     internal.index("uint8_t config_menu_appendf(")]
    enums += re.search(r"typedef enum \{[^}]*CONFIG_BROWSER_TARGET_PRINTOUT[^}]*\} \w+;", source, re.S).group(0)
    code = code.replace("static unsigned version_reads", enums + "\nstatic unsigned version_reads")
    code += "static const char credits_hint[] = " + credits_hint + ";\n"
    for array in ("k_tab_labels", "k_about_contributors", "k_about_third_party"):
        declaration = re.search(r"static const char \* const " + array + r"\[[^\]]*\] = \{.*?\n\};", source, re.S).group(0)
        if array == "k_about_third_party":
            future = ["Future credit %03d: " % i + "A future contributor and library entry with a deliberately long description that must wrap intact and stay reachable by normal navigation. " + "FUTURE-END-%03d" % i for i in range(80)]
            declaration = declaration.replace("\n};", ",\n#ifdef TEST_FUTURE_CREDITS\n" + ",\n".join(json.dumps(x) for x in future) + "\n#endif\n};")
        code += declaration + "\n"
    code += "\n".join(re.findall(r"static uint32_t g_about_credits_\w+;", source)) + "\n"
    code += "\n".join(function(source, name) for name in (
        "config_menu_about_credits_offset", "config_menu_about_reset_scroll",
        "config_menu_about_scroll", "config_menu_about_handle_input"))
    code += "\n".join(function(source, name) for name in (
        "config_menu_online_update_busy", "config_menu_poll_online_update",
        "config_menu_online_update_finished", "config_menu_start_online_update",
        "config_menu_online_update_check", "config_menu_online_update_handle_input",
        "config_menu_refresh_golden_boot", "config_menu_golden_boot_update_finished",
        "config_menu_start_golden_boot_update", "config_menu_golden_boot_handle_input",
        "config_menu_on_tab_entered"))
    about_action = function(source, "config_menu_activate_item")
    about_action = about_action[about_action.index("    case CONFIG_TAB_ABOUT:"):]
    about_action = about_action[:about_action.index("    default:")]
    code += r'''
static void config_menu_activate_item(config_menu_t *m)
{ config_menu_t *menu=m; switch(menu->tab) {
''' + about_action + ' default: break; } }\n'
    code += handler
    code += r'''
static unsigned drawn_button, drawn_version, focused_buttons, disabled_buttons;
static unsigned native_badges, compact_badges;
static unsigned progress_rate_visible;
static unsigned hint_visible;
static uint8_t audit_credits,capture_previews;
static uint8_t contributor_covered[sizeof(k_about_contributors)/sizeof(k_about_contributors[0])][1024];
static uint8_t party_covered[sizeof(k_about_third_party)/sizeof(k_about_third_party[0])][1024];
static uint8_t help_covered[8][1024],frame_help[8][1024];
static int third_heading_y,third_heading_bottom,last_credit_y,credit_limit;
static uint64_t fixed_top_hash;
static unsigned audit_frames;
static unsigned compact_first,compact_visible,compact_count;
static unsigned frame_compact,signature_hits,signature_pairs;
static int compact_left,compact_right,signature_bottom,origin_y;
static int last_action_bottom,last_contributor_bottom;
static int action_gap,contributor_gap,signature_gap;
static int logical_action,logical_contributors,logical_contributor,logical_third,logical_signature,logical_origin;
static uint8_t mark_text(const char *source,const char *text,uint8_t covered[1024],uint8_t *once)
{
    char line[1024];size_t n=strlen(text);
    while(n && (*text==' '||*text=='\t')){++text;--n;}
    while(n && (text[n-1]==' '||text[n-1]=='\t'))--n;
    if(!n || n>=sizeof line)return 0;
    memcpy(line,text,n);line[n]=0;
    const char *at=strstr(source,line);if(!at)return 0;
    size_t offset=(size_t)(at-source);assert(strlen(source)<1024);
    for(size_t i=0;i<n;++i)if(line[i]!=' '&&line[i]!='\t'){
        covered[offset+i]=1;
        if(once)assert(++once[offset+i]==1);
    }
    return 1;
}
static uint8_t audit_text(const char *text)
{
    uint8_t found=0;
    for(unsigned i=0;i<sizeof(contributor_covered)/sizeof(contributor_covered[0]);++i)
        found|=mark_text(k_about_contributors[i],text,contributor_covered[i],NULL);
    for(unsigned i=0;i<sizeof(party_covered)/sizeof(party_covered[0]);++i)
        if(k_about_third_party[i][0])if(mark_text(k_about_third_party[i],text,party_covered[i],NULL))found|=2;
    config_menu_help_block_t help=config_menu_help_resolve(CONFIG_TAB_ABOUT,0);
    assert(help.count<=8);
    for(unsigned i=0;i<help.count;++i)if(mark_text(help.lines[i],text,help_covered[i],frame_help[i]))found|=4;
    return found;
}
static void assert_covered(const char *text,const uint8_t *covered)
{
    for(unsigned i=0;text[i];++i)if(text[i]!=' '&&text[i]!='\t'&&!covered[i]){
        fprintf(stderr,"Unreachable credit byte %u in: %s\n",i,text);abort();
    }
}
static unsigned diagnostic_case, diagnostic_visible, diagnostic_code_visible;
static const char *diagnostic_marker, *diagnostic_code;
static int body_x, body_y, body_w, body_h;
static void record_row(uint16_t *fb,int x,int y,int w,uint8_t focus,uint8_t dim,const char *text)
{
    if (!cmui_compact_active()) {
        assert(x>=body_x && x+w<=body_x+body_w && y>=body_y && y+CMUI_ROW_H<=body_y+body_h);
        assert(cmui_text_width(text,CMUI_BODY_SCALE)<=w-34);
        if(audit_credits)last_action_bottom=y+CMUI_ROW_H;
    }
    if (!strncmp(text,"Update Golden Boot...",21)) { ++drawn_button; focused_buttons+=focus!=0; disabled_buttons+=dim!=0; }
    cmui_row(fb,x,y,w,focus,dim,text);
}
static void record_text(uint16_t *fb,int x,int y,int w,const char *text,uint32_t fg,uint32_t bg,int scale)
{
    uint8_t credit_scope=audit_credits&&!cmui_compact_active()?audit_text(text):0;
    if(credit_scope) {
        if(credit_scope&6U)assert(third_heading_bottom && y>=third_heading_bottom);
        assert(x>=body_x && cmui_text_width(text,scale)<=w && x+w<=body_x+body_w);
        assert(y>=body_y && y+FB16_BUILTIN_FONT_HEIGHT*scale<=credit_limit);
        assert(y>=last_credit_y);last_credit_y=y+FB16_BUILTIN_FONT_HEIGHT*scale;
        if(credit_scope&1U)last_contributor_bottom=last_credit_y;
        if(strstr(text,"Rikkles && KKR75"))assert(fg==CMUI_COLOR_MUTED);
    }
    if(!cmui_compact_active() && strstr(text,"Elapsed 12.3 s") && strstr(text,"44.0 KiB/s")) {
        assert(cmui_text_width(text,scale)<=w);++progress_rate_visible;
    }
    if(diagnostic_marker && !cmui_compact_active()) {
        if(strstr(text,diagnostic_marker)) ++diagnostic_visible;
        if(diagnostic_code && strstr(text,diagnostic_code)) ++diagnostic_code_visible;
        if(strstr(text,diagnostic_marker) || (diagnostic_code && strstr(text,diagnostic_code))) {
            assert(x>=body_x && x+w<=body_x+body_w && y>=body_y && y+8*scale<=body_y+body_h);
            assert(cmui_text_width(text,scale)<=w);
        }
    }
    if(!strcmp(text,"Update available")) {
        ++native_badges; assert(x>=0 && x+w<=FB16_WIDTH && y>=0 && y+8<=FB16_HEIGHT);
        assert(cmui_text_width(text,scale)<=w);
    }
    if (!strncmp(text,"Golden Boot:",12)) {
        ++drawn_version;
        assert(strstr(text,installed_version));
        assert(!strstr(text,"Boot Boot"));
        if(!cmui_compact_active()) assert(cmui_text_width(text,scale)<=w);
    }
    cmui_text_clipped(fb,x,y,w,text,fg,bg,scale);
}
static void record_plain(uint16_t *fb,int x,int y,const char *text,uint32_t fg,uint32_t bg,int scale)
{
    if(audit_credits && !cmui_compact_active()) {
        if(!strcmp(text,"Contributors")){action_gap=y-last_action_bottom;assert(last_action_bottom&&action_gap>=24);}
        if(!strncmp(text,"Third-party",11)){
            third_heading_y=y;third_heading_bottom=y+FB16_BUILTIN_FONT_HEIGHT*scale;
            contributor_gap=y-last_contributor_bottom;assert(last_contributor_bottom&&contributor_gap>=24);
        }
    }
    cmui_text(fb,x,y,text,fg,bg,scale);
}
#define cmui_row record_row
#define cmui_text_clipped record_text
#define cmui_text record_plain
'''
    help_source = (FRONT / "config_menu_help.c").read_text()
    code += help_source.replace('#include "config_menu_internal.h"', '')
    code += draws
    code += "#undef cmui_row\n#undef cmui_text_clipped\n#undef cmui_text\n"
    code += r'''
uint8_t config_menu_usb0_sd_remote_active(const config_menu_t *menu){(void)menu;return 0;}
uint8_t config_menu_ethernet_ftp_sd_remote_active(const config_menu_t *menu){(void)menu;return 0;}
'''
    code += close_consumer + function(main_source, "ui_input_requests_menu_close")
    code += r'''
static unsigned compact_focus, compact_button, compact_bounds, compact_disabled;
void test_compact_rows(unsigned first,unsigned visible,unsigned count,int left,int right)
{
    assert(first<count && visible>0);compact_first=first;compact_visible=visible;compact_count=count;++compact_bounds;
    compact_left=left;compact_right=right;
    logical_action=logical_contributors=logical_contributor=logical_third=logical_signature=logical_origin=-1;
}
void test_compact_tab(int x,int y,int w,int h,const char *label)
{
    assert(x>=0 && x+w<=FB16_WIDTH && y>=0 && y+h<FB16_HEIGHT);
    if(strstr(label,"Update available")) ++compact_badges;
}
void test_compact_row(const char *text,unsigned focus,unsigned dim,unsigned index,unsigned first,unsigned visible)
{
    if(audit_credits) {
        if(!strncmp(text,"Update Golden Boot...",21)||!strncmp(text,"Update boot...",14))logical_action=(int)index;
        if(!strcmp(text,"Contributors"))logical_contributors=(int)index;
        for(unsigned i=0;i<sizeof(k_about_contributors)/sizeof(k_about_contributors[0]);++i)
            if(!strcmp(text,k_about_contributors[i]))logical_contributor=(int)index;
        if(!strncmp(text,"Third-party",11))logical_third=(int)index;
        if(strstr(text,"Rikkles && KKR75"))logical_signature=(int)index;
        if(!strncmp(text,"Project origin",14))logical_origin=(int)index;
    }
    if(strstr(text,"Elapsed 12.3 s") && strstr(text,"44.0 KiB/s")) {
        assert(index>=first && index<first+visible);++progress_rate_visible;
    }
    if(diagnostic_marker && strstr(text,diagnostic_marker)) {
        assert(index>=first && index<first+visible); ++diagnostic_visible;
        assert(cmui_text_width(text,FB16_WIDTH>=1024?2:1)<=FB16_WIDTH-32);
    }
    if(diagnostic_code && strstr(text,diagnostic_code)) {
        assert(index>=first && index<first+visible); ++diagnostic_code_visible;
    }
    if(!strncmp(text,"Update Golden Boot...",21)) {
        ++compact_button; compact_disabled+=dim!=0; assert(cmui_text_width(text,FB16_WIDTH>=1024?2:1)<=FB16_WIDTH-32);
    }
    if(focus) { ++compact_focus; assert(index>=first && index<first+visible); }
}
void test_compact_gaps(void)
{
    if(!audit_credits)return;
    assert(logical_action>=0 && logical_contributors>logical_action+1);
    assert(logical_contributor>=0 && logical_third>logical_contributor+1);
    assert(logical_signature>=0 && logical_origin>logical_signature+1);
    action_gap=logical_contributors-logical_action-1;
    contributor_gap=logical_third-logical_contributor-1;
}
void test_compact_draw(const char *text,int x,int y,int w,int scale,int top,int bottom,unsigned right,unsigned fg,unsigned bg)
{
    if(audit_credits) {
        if(right){assert(strstr(text,"Rikkles && KKR75"));assert(fg==CMUI_COLOR_MUTED);}
        else assert(x==compact_left);
        if(!text[strspn(text," \t")])assert(bg==CMUI_COLOR_BG);
    }
    if(audit_credits && audit_text(text)) {
        assert(y>=top && y+FB16_BUILTIN_FONT_HEIGHT*scale<=bottom);
        assert(x>=0&&x+w<=FB16_WIDTH&&cmui_text_width(text,scale)<=w);
    }
}
void test_ui_text(int x,int y,const char *text,int scale)
{
    if(audit_credits) {
        if(strstr(text,"Rikkles && KKR75")) {
            assert(x+cmui_text_width(text,scale)==(frame_compact?compact_right:body_x+body_w-18));
            signature_bottom=y+FB16_BUILTIN_FONT_HEIGHT*scale;++signature_hits;
        }
        if(!strncmp(text,"Project origin",14)) {
            origin_y=y;
            if(signature_bottom){signature_gap=origin_y-signature_bottom;assert(signature_gap>=24);++signature_pairs;}
        }
    }
    if(!strcmp(text,credits_hint)) {
        ++hint_visible;assert(x>=0&&y>=0&&x+cmui_text_width(text,scale)<=FB16_WIDTH);
        assert(y+FB16_BUILTIN_FONT_HEIGHT*scale<=FB16_HEIGHT);
    }
}
static void press(config_menu_t *menu, ui_key_t key)
{ assert(config_menu_handle_input(menu,(ui_input_t){key,1,0})); }
static uint64_t render(config_menu_t *menu,unsigned width,unsigned height,unsigned mode)
{
    static const char *const tabs[]={"Prof","Boot","Video","SP","Disk","CPU","Sound","Slot2","Net","TW","Clock","RAM","USB","Print","About"};
    assert(fb16_set_size((int)width,(int)height));
    size_t pixels=width*height;
    uint16_t *guard=calloc(pixels+32,sizeof(*guard)); assert(guard);
    for(unsigned i=0;i<16;++i) guard[i]=guard[pixels+16+i]=0xa55a;
    uint16_t *fb=guard+16;
    cmui_rect_t nav,body,footer; cmui_screen_rects(&nav,&body,&footer);
    const unsigned compact=width<1680 || height<1000;
    frame_compact=compact;
    unsigned old_reads=version_reads,old_checks=file_checks,old_allowed=allowed_checks,old_online=online_reads;
    drawn_button=drawn_version=focused_buttons=disabled_buttons=compact_focus=compact_button=compact_disabled=0;
    native_badges=compact_badges=0;
    diagnostic_visible=diagnostic_code_visible=0;
    progress_rate_visible=0;
    hint_visible=0;
    memset(frame_help,0,sizeof(frame_help));last_credit_y=0;third_heading_y=third_heading_bottom=0;
    signature_bottom=origin_y=last_action_bottom=last_contributor_bottom=0;
    if(audit_credits)++audit_frames;
    cmui_set_update_badge(menu->online_update.available_mask!=0);
    if(compact) { body=(cmui_rect_t){0,0,1480,812}; cmui_compact_begin(); }
    else { cmui_clear(fb); cmui_header(fb,"Appletini",APPLETINI_FIRMWARE_IMAGE_VERSION_FULL,0); }
    body_x=body.x; body_y=body.y; body_w=body.w; body_h=body.h;
    credit_limit=body.y+body.h;
    if(!compact && !mode) config_menu_draw_tabs(fb,menu,nav.x,nav.y,nav.w);
    if(mode>=3) config_menu_draw_online_update_modal(fb,menu,&body);
    else if(mode) config_menu_draw_golden_boot_modal(fb,menu,&body);
    else config_menu_draw_about(fb,menu,body.x,body.y,body.w,body.h);
    if(audit_credits && !compact) {
        fixed_top_hash=1469598103934665603ULL;assert(third_heading_y>body_y);
        for(int y0=body_y;y0<third_heading_y;++y0)for(int x0=body_x;x0<body_x+body_w;++x0)
            fixed_top_hash=(fixed_top_hash^fb[(size_t)y0*width+x0])*1099511628211ULL;
    }
    const char *input_hint=mode==0&&menu->about_credits_active?credits_hint:NULL;
    if(compact) cmui_compact_finish(fb,"About",tabs,CONFIG_TAB_COUNT,CONFIG_TAB_ABOUT,menu->status,menu->status_warning,0,0,input_hint);
    else cmui_footer(fb,&footer,menu->status,menu->status_warning,0,0,input_hint);
    assert(hint_visible==(input_hint!=NULL));
    assert(version_reads==old_reads && file_checks==old_checks && allowed_checks==old_allowed && online_reads==old_online);
    if(mode==3 || (mode==0 && menu->online_update.state==ONLINE_UPDATE_CHECKING && menu->online_update.elapsed_ms==12345)) assert(progress_rate_visible==1);
    if(mode==0) {
        if(diagnostic_marker) assert(diagnostic_visible);
        if(diagnostic_code) assert(diagnostic_code_visible);
        assert((compact?compact_badges:native_badges)==(menu->online_update.available_mask!=0));
        if(compact) {
            assert(compact_button==(menu->golden_boot_file_present!=0));
            assert(compact_focus==(menu->about_credits_active?0:(menu->item_focus<3 || (menu->golden_boot_file_present!=0 && menu->item_focus==3))));
            assert(compact_disabled==(menu->golden_boot_file_present!=0 && menu->golden_boot_update_allowed==0));
        } else {
            assert(drawn_version==1 && drawn_button==(menu->golden_boot_file_present!=0));
            assert(disabled_buttons==(menu->golden_boot_file_present!=0 && menu->golden_boot_update_allowed==0));
        }
    } else if(compact) assert(compact_focus==(mode==1 || (mode>=3 && !menu->online_update_install_pending)));
    for(unsigned i=0;i<16;++i) assert(guard[i]==0xa55a && guard[pixels+16+i]==0xa55a);
    uint64_t body_hash=1469598103934665603ULL;
    unsigned body_bottom=compact ? height-8-2*(FB16_BUILTIN_FONT_HEIGHT*(width>=1024?2:1)+4) : (unsigned)footer.y;
    for(size_t i=0;i<(size_t)width*body_bottom;++i) body_hash=(body_hash^fb[i])*1099511628211ULL;
    char path[112]; snprintf(path,sizeof(path),"about_%ux%u_mode%u_focus%u_present%u_allowed%u_diag%u.ppm",width,height,mode,
        mode==1?menu->golden_boot_confirm_update:menu->item_focus,menu->golden_boot_file_present,menu->golden_boot_update_allowed,diagnostic_case);
    FILE *ppm=capture_previews?fopen(path,"wb"):NULL;
    if(capture_previews) { assert(ppm); fprintf(ppm,"P6\n%u %u\n255\n",width,height);
    for(size_t i=0;i<pixels;++i) {
        unsigned char rgb[]={(unsigned char)((fb[i]>>11)*255/31),(unsigned char)(((fb[i]>>5)&63)*255/63),(unsigned char)((fb[i]&31)*255/31)};
        fwrite(rgb,1,3,ppm);
    }
    fclose(ppm); } free(guard);
    return body_hash;
}
static uint32_t offset(unsigned compact)
{ return compact?cmui_compact_scroll_offset():config_menu_about_credits_offset(); }
static void scroll_case(config_menu_t *menu,unsigned width,unsigned height,unsigned failed)
{
    const unsigned compact=width<1680||height<1000;
    unsigned old_updates=update_requests,old_checks=online_checks,old_downloads=online_downloads,old_installs=online_installs;
    menu->active=1;menu->tab=CONFIG_TAB_ABOUT;menu->golden_boot_file_present=1;file_present=1;
    menu->online_update_ui_error[0]=0;menu->online_update_confirm=0;
    menu->online_update_download_pending=menu->online_update_install_pending=0;
    memset(&menu->online_update,0,sizeof menu->online_update);
    menu->online_update.state=failed?ONLINE_UPDATE_FAILED:ONLINE_UPDATE_CURRENT;
    if(failed) {
        menu->online_update.result=-210;menu->online_update.http_status=200;
        menu->online_update.http_result=-102;menu->online_update.tls_verify=8;
        menu->online_update.fatfs_result=7;menu->online_update.validation_result=4;menu->online_update.cleanup_result=1;
        snprintf(menu->online_update.stage,sizeof menu->online_update.stage,"Image validation");
        snprintf(menu->online_update.detail,sizeof menu->online_update.detail,"SD write failed. Check free space and write access, then retry cleanup.");
    }
    config_menu_on_tab_entered(menu);menu->item_focus=3;
    memset(contributor_covered,0,sizeof contributor_covered);memset(party_covered,0,sizeof party_covered);memset(help_covered,0,sizeof help_covered);
    signature_hits=signature_pairs=0;action_gap=contributor_gap=signature_gap=0;
    audit_credits=1;render(menu,width,height,0);
    press(menu,UI_KEY_DOWN);assert(menu->about_credits_active);render(menu,width,height,0);
    for(unsigned i=0;i<100&&offset(compact);++i){press(menu,UI_KEY_PAGE_UP);render(menu,width,height,0);}
    assert(!offset(compact));
    press(menu,UI_KEY_UP);assert(!menu->about_credits_active&&menu->item_focus==3);
    press(menu,UI_KEY_DOWN);assert(menu->about_credits_active);render(menu,width,height,0);
    for(unsigned i=0;i<100&&offset(compact);++i){press(menu,UI_KEY_PAGE_UP);render(menu,width,height,0);}
    assert(!offset(compact));
    uint64_t stable=fixed_top_hash;
#ifndef TEST_FUTURE_CREDITS
    capture_previews=1;diagnostic_case=failed?42:40;render(menu,width,height,0);capture_previews=0;
#endif
    if(compact) {
        /* Put the real signature and first credit in the same painted viewport,
         * independent of where overlapping page boundaries happen to fall. */
        unsigned signature_row=(unsigned)logical_signature;
        for(unsigned i=0;i<signature_row;++i)press(menu,UI_KEY_DOWN);
#ifndef TEST_FUTURE_CREDITS
        capture_previews=width==640;diagnostic_case=44;
#endif
        render(menu,width,height,0);capture_previews=0;
        assert(signature_hits&&signature_pairs);
        for(unsigned i=0;i<100&&offset(compact);++i){press(menu,UI_KEY_PAGE_UP);render(menu,width,height,0);}
        assert(!offset(compact));
    }
    unsigned pages=0;
    for(;pages<1000;++pages) {
        uint32_t before=offset(compact);press(menu,UI_KEY_PAGE_DOWN);render(menu,width,height,0);
        assert(offset(compact)>=before);
        if(!compact)assert(fixed_top_hash==stable);
        if(offset(compact)==before)break;
    }
    assert(pages<1000);
    const unsigned logical_rows=compact?compact_count:g_about_credits_rows;
    const unsigned visible_rows=compact?compact_visible:g_about_credits_visible;
    const uint32_t maximum=compact?(compact_count>compact_visible?compact_count-compact_visible:0):
        (g_about_credits_rows>g_about_credits_visible?g_about_credits_rows-g_about_credits_visible:0);
    assert(offset(compact)==maximum);
    if(!compact)assert(g_about_credits_visible>0);
    for(unsigned i=0;i<sizeof(contributor_covered)/sizeof(contributor_covered[0]);++i)assert_covered(k_about_contributors[i],contributor_covered[i]);
    for(unsigned i=0;i<sizeof(party_covered)/sizeof(party_covered[0]);++i)assert_covered(k_about_third_party[i],party_covered[i]);
    config_menu_help_block_t help=config_menu_help_resolve(CONFIG_TAB_ABOUT,0);
    for(unsigned i=0;i<help.count;++i)assert_covered(help.lines[i],help_covered[i]);
    assert(signature_hits&&signature_pairs&&signature_gap>=24);
#ifdef TEST_FUTURE_CREDITS
    assert((compact?compact_count:g_about_credits_rows)>160);
    assert(maximum>0);
#else
    capture_previews=1;diagnostic_case=failed?43:41;render(menu,width,height,0);capture_previews=0;
#endif
    uint32_t bottom=offset(compact);press(menu,UI_KEY_DOWN);render(menu,width,height,0);assert(offset(compact)==bottom);
    if(bottom){press(menu,UI_KEY_UP);render(menu,width,height,0);assert(offset(compact)+1==bottom);
        press(menu,UI_KEY_DOWN);render(menu,width,height,0);assert(offset(compact)==bottom);}
    /* Reflow/viewport changes clamp the stored position without changing
     * the meaning of the focused credit pane. */
    unsigned alternate_w=compact?(width==1280&&height==1024?640:1280):(width==1920?1680:1920);
    unsigned alternate_h=compact?(alternate_w==640?400:1024):(alternate_w==1680?1050:1080);
    render(menu,alternate_w,alternate_h,0);assert(menu->about_credits_active);
    unsigned alternate_compact=alternate_w<1680||alternate_h<1000;
    uint32_t alternate_max=alternate_compact?(compact_count>compact_visible?compact_count-compact_visible:0):
        (g_about_credits_rows>g_about_credits_visible?g_about_credits_rows-g_about_credits_visible:0);
    assert(offset(alternate_compact)<=alternate_max);
    render(menu,width,height,0);assert(menu->about_credits_active);
    /* BOOT.BIN arrival/removal must never reinterpret credit focus as an action. */
    for(unsigned present=0;present<2;++present) {
        menu->golden_boot_file_present=(uint8_t)present;file_present=(uint8_t)present;
        render(menu,width,height,0);assert(menu->about_credits_active);
        assert(ui_config_menu_has_close_consumer(menu));
        ui_key_t exit_key=present?UI_KEY_BACK:UI_KEY_ENTER;
        if(present)assert(ui_input_requests_menu_close((ui_input_t){exit_key,1,0}));
        press(menu,exit_key);assert(!menu->about_credits_active&&menu->item_focus==0&&menu->active);
        assert(!ui_config_menu_has_close_consumer(menu));
        menu->item_focus=present?3:2;press(menu,UI_KEY_DOWN);assert(menu->about_credits_active);render(menu,width,height,0);
    }
    assert(ui_input_requests_menu_close((ui_input_t){UI_KEY_ESC,1,0}));
    press(menu,UI_KEY_ESC);assert(!menu->about_credits_active&&menu->item_focus==0&&menu->active);
    menu->item_focus=3;press(menu,UI_KEY_DOWN);assert(menu->about_credits_active);
    press(menu,UI_KEY_TAB);assert(!menu->about_credits_active&&menu->tab!=CONFIG_TAB_ABOUT);
    menu->tab=CONFIG_TAB_ABOUT;config_menu_on_tab_entered(menu);assert(!offset(compact)&&!menu->about_credits_active);
    menu->item_focus=3;press(menu,UI_KEY_DOWN);assert(menu->about_credits_active);
    press(menu,UI_KEY_MENU);assert(!menu->active);
    menu->active=1;menu->tab=CONFIG_TAB_ABOUT;config_menu_on_tab_entered(menu);
    assert(!offset(compact)&&!menu->about_credits_active);
    assert(update_requests==old_updates&&online_checks==old_checks&&online_downloads==old_downloads&&online_installs==old_installs);
    audit_credits=0;
    printf("SCROLL_REACHABLE %ux%u failed%u: pages=%u max_offset=%u logical_rows=%u visible=%u; all credit+intro bytes, fixed controls and input safety passed\n",
        width,height,failed,pages,maximum,logical_rows,visible_rows);
    printf("SECTION_GEOMETRY %ux%u failed%u: compact=%u versions_gap=%d contributors_gap=%d signature_gap_px=%d right_aligned_hits=%u\n",
        width,height,failed,compact,action_gap,contributor_gap,signature_gap,signature_hits);
}
int main(void)
{
    config_menu_t menu={0}; menu.active=1; menu.tab=CONFIG_TAB_ABOUT; menu.item_focus=3;
    menu.usb_binding_capture=CONFIG_MENU_USB_BIND_CAPTURE_NONE;
    menu.platform.get_golden_boot_version=read_version;
    menu.platform.get_golden_boot_file_present=read_present;
    menu.platform.request_golden_boot_update=request_update;
    /* An unknown platform denies updates even when BOOT.BIN exists. */
    file_present=1; config_menu_refresh_golden_boot(&menu);
    assert(menu.golden_boot_file_present && !menu.golden_boot_update_allowed);
    press(&menu,UI_KEY_ENTER);
    assert(!menu.golden_boot_update_confirm && !update_requests && menu.active);
    assert(!strcmp(menu.status,"Update Golden Boot from the initial boot menu"));
    /* A booted physical Apple II keeps the button visible and dimmed. Both
     * button activation and the real Enter/controller input path refuse it. */
    menu.platform.get_golden_boot_update_allowed=read_allowed;
    update_allowed=0; config_menu_on_tab_entered(&menu);
    const unsigned sizes[][2]={{1920,1080},{1680,1050},{1280,1024},{1360,768},{1200,800},{1280,720},{1024,768},{640,400}};
    for(unsigned s=0;s<8;++s) render(&menu,sizes[s][0],sizes[s][1],0);
    config_menu_activate_item(&menu);
    press(&menu,UI_KEY_ENTER);
    assert(!menu.golden_boot_update_confirm && !menu.golden_boot_update_pending && !update_requests);
    assert(!strcmp(menu.status,"Update Golden Boot from the initial boot menu"));
    /* Initial-boot-menu/ONE//e eligibility refreshes on About entry. */
    update_allowed=1; assert(!menu.golden_boot_update_allowed);
    config_menu_on_tab_entered(&menu); assert(menu.golden_boot_update_allowed);
    render(&menu,1920,1080,0);
    press(&menu,UI_KEY_ENTER); assert(menu.golden_boot_update_confirm && !menu.golden_boot_confirm_update);
    /* A host boot handoff while confirmation is open invalidates acceptance. */
    update_allowed=0; press(&menu,UI_KEY_RIGHT); press(&menu,UI_KEY_ENTER);
    assert(!menu.golden_boot_update_confirm && !menu.golden_boot_update_pending && !update_requests);
    assert(menu.active && menu.tab==CONFIG_TAB_ABOUT && menu.status_warning);
    assert(!strcmp(menu.status,"Update Golden Boot from the initial boot menu"));
    update_allowed=1; file_present=0;
    config_menu_refresh_golden_boot(&menu);
    assert(!strcmp(menu.golden_boot_version,"B1.2.0") && !menu.golden_boot_file_present);
    press(&menu,UI_KEY_ENTER); assert(!menu.golden_boot_update_confirm && !update_requests);
    file_present=1; press(&menu,UI_KEY_ENTER);
    assert(menu.golden_boot_update_confirm && !menu.golden_boot_confirm_update);
    for(unsigned i=0;i<12;++i) press(&menu,UI_KEY_ENTER);
    assert(!update_requests && !menu.golden_boot_update_pending);
    if(menu.golden_boot_update_confirm) press(&menu,UI_KEY_BACK);
    press(&menu,UI_KEY_ENTER); press(&menu,UI_KEY_RIGHT); press(&menu,UI_KEY_BACK);
    assert(!menu.golden_boot_update_confirm && !update_requests && menu.active);
    press(&menu,UI_KEY_ENTER); press(&menu,UI_KEY_DOWN); file_present=0; press(&menu,UI_KEY_ENTER);
    assert(!update_requests && !menu.golden_boot_update_pending && menu.status_warning);
    file_present=1; reject_request=1; press(&menu,UI_KEY_ENTER); press(&menu,UI_KEY_TAB); press(&menu,UI_KEY_ENTER);
    assert(update_requests==1 && !menu.golden_boot_update_pending && menu.status_warning);
    reject_request=0; press(&menu,UI_KEY_ENTER); press(&menu,UI_KEY_RIGHT); press(&menu,UI_KEY_LEFT);
    assert(!menu.golden_boot_confirm_update); press(&menu,UI_KEY_RIGHT); press(&menu,UI_KEY_ENTER);
    assert(update_requests==2 && menu.golden_boot_update_pending);
    press(&menu,UI_KEY_ENTER); press(&menu,UI_KEY_BACK); press(&menu,UI_KEY_MENU);
    assert(update_requests==2 && menu.golden_boot_update_pending && menu.active);
    config_menu_golden_boot_update_finished(&menu,"BOOT IMAGE CRC MISMATCH");
    assert(!menu.golden_boot_update_pending && menu.status_warning && strstr(menu.status,"CRC MISMATCH"));
    installed_version=""; config_menu_refresh_golden_boot(&menu); assert(!strcmp(menu.golden_boot_version,"Unknown"));
    installed_version="B1.2.0"; config_menu_refresh_golden_boot(&menu);
    for(unsigned s=0;s<8;++s) {
        menu.item_focus=3; render(&menu,sizes[s][0],sizes[s][1],0);
        if(sizes[s][0]<1680 || sizes[s][1]<1000) {
            press(&menu,UI_KEY_DOWN); assert(menu.about_credits_active);
            render(&menu,sizes[s][0],sizes[s][1],0);
            press(&menu,UI_KEY_ENTER); assert(menu.item_focus==0 && !menu.golden_boot_update_confirm);
        }
        menu.golden_boot_update_confirm=1; menu.golden_boot_confirm_update=0; render(&menu,sizes[s][0],sizes[s][1],1);
        menu.golden_boot_confirm_update=1; render(&menu,sizes[s][0],sizes[s][1],1);
        menu.golden_boot_update_confirm=0; menu.golden_boot_update_pending=1; render(&menu,sizes[s][0],sizes[s][1],2);
        menu.golden_boot_update_pending=0; menu.golden_boot_file_present=0; render(&menu,sizes[s][0],sizes[s][1],0);
        menu.golden_boot_file_present=1;
    }
    menu.platform.get_online_update_status=online_status;
    menu.platform.request_online_update_check=online_check;
    menu.platform.request_online_update_download=online_download;
    menu.platform.cancel_online_update=online_cancel;
    menu.platform.request_online_update_install=online_install;
    online.state=ONLINE_UPDATE_AVAILABLE; online.available_mask=3;
    snprintf(online.latest_f,sizeof(online.latest_f),"F1.3.6");
    snprintf(online.latest_b,sizeof(online.latest_b),"B2.0.7");
    assert(config_menu_poll_online_update(&menu));
    assert(!config_menu_poll_online_update(&menu));
    menu.item_focus=0; press(&menu,UI_KEY_ENTER);
    assert(online_checks==1 && !online_downloads && !online_installs);
    online.state=ONLINE_UPDATE_CHECKING; assert(config_menu_poll_online_update(&menu));
    online.received=12345;online.total=1048576;online.elapsed_ms=12345;online.bytes_per_second=45056;
    assert(!config_menu_poll_online_update(&menu));
    for(unsigned s=0;s<8;++s) render(&menu,sizes[s][0],sizes[s][1],0);
    press(&menu,UI_KEY_ENTER); assert(online_cancels==1 && online_checks==1);
    online.state=ONLINE_UPDATE_AVAILABLE; assert(config_menu_poll_online_update(&menu));
    menu.item_focus=1; press(&menu,UI_KEY_ENTER);
    assert(menu.online_update_confirm && !menu.online_update_confirm_update && !online_downloads);
    press(&menu,UI_KEY_ENTER); assert(!menu.online_update_confirm && !online_downloads);
    press(&menu,UI_KEY_ENTER); press(&menu,UI_KEY_RIGHT); press(&menu,UI_KEY_BACK);
    assert(!menu.online_update_confirm && !online_downloads);
    press(&menu,UI_KEY_ENTER); press(&menu,UI_KEY_RIGHT); press(&menu,UI_KEY_ENTER);
    assert(online_downloads==1 && online_kind==ONLINE_UPDATE_FIRMWARE && menu.online_update_download_pending);
    online.state=ONLINE_UPDATE_DOWNLOADING; online.kind=ONLINE_UPDATE_FIRMWARE;
    online.received=12345; online.total=4761708; online.elapsed_ms=12345;online.bytes_per_second=45056;
    assert(config_menu_poll_online_update(&menu));
    for(unsigned s=0;s<8;++s) render(&menu,sizes[s][0],sizes[s][1],3);
    press(&menu,UI_KEY_MENU); assert(online_cancels==2 && menu.active && !online_installs);
    online.state=ONLINE_UPDATE_CANCELLED; assert(config_menu_poll_online_update(&menu));
    assert(!menu.online_update_download_pending);
    online.state=ONLINE_UPDATE_AVAILABLE; assert(config_menu_poll_online_update(&menu));
    menu.item_focus=2; press(&menu,UI_KEY_ENTER); press(&menu,UI_KEY_RIGHT);
    update_allowed=0; press(&menu,UI_KEY_ENTER);
    assert(online_downloads==1 && !menu.online_update_confirm && menu.status_warning);
    update_allowed=1; press(&menu,UI_KEY_ENTER); press(&menu,UI_KEY_RIGHT);
    online_reject=1; press(&menu,UI_KEY_ENTER);
    assert(online_downloads==2 && !menu.online_update_download_pending && menu.status_warning);
    online_reject=0; press(&menu,UI_KEY_ENTER);
    for(unsigned s=0;s<8;++s) render(&menu,sizes[s][0],sizes[s][1],4);
    press(&menu,UI_KEY_RIGHT); press(&menu,UI_KEY_ENTER);
    assert(online_downloads==3 && online_kind==ONLINE_UPDATE_BOOT);
    online.state=ONLINE_UPDATE_READY; online.kind=ONLINE_UPDATE_BOOT;
    assert(config_menu_poll_online_update(&menu) && !menu.online_update_download_pending);
    press(&menu,UI_KEY_ENTER); assert(menu.online_update_confirm_install && !menu.online_update_confirm_update);
    press(&menu,UI_KEY_RIGHT); press(&menu,UI_KEY_ENTER);
    assert(online_installs==1 && online_kind==ONLINE_UPDATE_BOOT && menu.online_update_install_pending);
    press(&menu,UI_KEY_MENU); assert(online_cancels==2 && menu.online_update_install_pending);
    for(unsigned s=0;s<8;++s) render(&menu,sizes[s][0],sizes[s][1],5);
    config_menu_online_update_finished(&menu,"UPDATE IMAGE CHECKSUM FAILED");
    assert(!menu.online_update_install_pending && menu.status_warning && strstr(menu.status,"CHECKSUM"));
    menu.online_update_confirm=0; menu.item_focus=1;
    for(unsigned s=0;s<8;++s) render(&menu,sizes[s][0],sizes[s][1],0);
    puts("PASS: queued online check/download/install, Cancel default, cancellation, progress, eligibility recheck and staged retry");
    static const struct {
        const char *stage,*detail,*marker,*code;
        int result,http_status,http_result,fatfs,validation,cleanup;
        uint32_t verify;
    } failures[] = {
        {"Release request","DNS lookup failed. Check the network DNS settings, then retry.","DNS lookup failed","transport -6",-215,0,-6,0,0,0,UINT32_MAX},
        {"Release response","TLS certificate expired. Check the device date and time before retrying.","certificate expired","TLS verify 00000001",-9984,0,-9984,0,0,0,1},
        {"Release response","TLS untrusted issuer. Update SD trust roots or use the firmware trust store.","untrusted issuer","TLS verify 00000008",-9984,0,-9984,0,0,0,8},
        {"Release request","Request format rejected (-1). No TLS verification was performed.","Request format rejected","TLS verify not run",-1,0,-1,0,0,0,UINT32_MAX},
        {"Release response","GitHub HTTP 403. Access was denied; try later or check the network policy.","GitHub HTTP 403","HTTP 403",-215,403,0,0,0,0,0},
        {"Release metadata","Required firmware asset is missing or invalid. Retry after the release is corrected.","firmware asset","Report code: -203",-203,200,0,0,0,0,0},
        {"SD staging","SD write failed. Check free space and write access, then retry cleanup.","SD write failed","cleanup 1",-210,200,0,7,0,1,0},
        {"Image validation","Image validation failed. The file is not installed; retry the download.","Image validation failed","image check -10",-206,200,0,0,-10,0,0},
    };
    menu.item_focus=0; menu.online_update_ui_error[0]=0;
    for(unsigned f=0;f<sizeof(failures)/sizeof(failures[0]);++f) {
        memset(&online,0,sizeof(online)); online.state=ONLINE_UPDATE_FAILED;
        online.result=failures[f].result;online.http_status=failures[f].http_status;
        online.http_result=failures[f].http_result;online.tls_verify=failures[f].verify;
        online.fatfs_result=failures[f].fatfs;online.validation_result=failures[f].validation;online.cleanup_result=failures[f].cleanup;
        snprintf(online.stage,sizeof(online.stage),"%s",failures[f].stage);
        snprintf(online.detail,sizeof(online.detail),"%s",failures[f].detail);
        assert(config_menu_poll_online_update(&menu));
        diagnostic_case=f+1;diagnostic_marker=failures[f].marker;diagnostic_code=failures[f].code;
        for(unsigned s=0;s<8;++s) {
            config_menu_set_status(&menu,1,"Transient error footer");
            uint64_t before=render(&menu,sizes[s][0],sizes[s][1],0);
            config_menu_set_status(&menu,0,"UNRELATED MENU ACTION REPLACED THE FOOTER");
            assert(before==render(&menu,sizes[s][0],sizes[s][1],0));
        }
    }
    diagnostic_case=9;diagnostic_marker="INSTALLER FLASH WRITE FAILED";diagnostic_code=NULL;
    config_menu_online_update_finished(&menu,diagnostic_marker);
    config_menu_set_status(&menu,0,"UNRELATED ACTION");
    for(unsigned s=0;s<8;++s) render(&menu,sizes[s][0],sizes[s][1],0);
    menu.item_focus=0;press(&menu,UI_KEY_ENTER);
    assert(!menu.online_update_ui_error[0]);
    diagnostic_marker=diagnostic_code=NULL;
    memset(&online,0,sizeof(online));online.state=ONLINE_UPDATE_CURRENT;
    assert(config_menu_poll_online_update(&menu));
    assert(!strcmp(menu.status,"NO NEWER STABLE UPDATE"));
    config_menu_set_status(&menu,0,"UNRELATED AFTER COMPLETED CHECK");
    assert(!config_menu_poll_online_update(&menu));
    assert(!strcmp(menu.status,"UNRELATED AFTER COMPLETED CHECK"));
    online.state=ONLINE_UPDATE_AVAILABLE;online.available_mask=1;
    assert(config_menu_poll_online_update(&menu));assert(!strcmp(menu.status,"UPDATE AVAILABLE"));
    config_menu_set_status(&menu,0,"UNRELATED AFTER AVAILABLE");++online.total;
    assert(!config_menu_poll_online_update(&menu));
    assert(!strcmp(menu.status,"UNRELATED AFTER AVAILABLE"));
    puts("PASS: completed CURRENT/AVAILABLE checks replace the checking footer once and preserve subsequent unrelated status");
    menu.online_update_ui_error[0]=0;menu.online_update_download_pending=menu.online_update_install_pending=0;
    menu.golden_boot_file_present=1;menu.item_focus=0;
    puts("PASS: persistent DNS/TLS/request-format/403/asset/SD-cleanup/image/install diagnostics stay visible and unclipped at all 8 outputs after footer overwrite; unrun TLS is not a certificate failure");
    assert(compact_bounds);
    puts("PASS: actual installed version, missing/stale BOOT.BIN, explicit Cancel/Update, repeated input, queued update and error recovery");
    puts("PASS: physical Apple II boot-phase gate, missing-callback denial, disabled activation and eligibility loss during confirmation");
    puts("PASS: full/compact About and modal drawing, button visibility, focus and framebuffer bounds; no flash/SD work during draw");
    for(unsigned s=0;s<8;++s)scroll_case(&menu,sizes[s][0],sizes[s][1],0);
    scroll_case(&menu,1680,1050,1);
    printf("PASS: %u page-rendered credit audits; original and future array use actual wrapping/scrolling, no callback during navigation\n",audit_frames);
    return 0;
}
'''
    harness = OUT / "test.c"
    harness.write_text(code)
    ui_source = (FRONT / "config_menu_ui.c").read_text()
    text_start = re.search(r'void cmui_text\([^;]*?\)\s*\{', ui_source)
    assert text_start
    ui_source = ui_source[:text_start.end()] + '\n    extern void test_ui_text(int,int,const char *,int);\n    test_ui_text(x,y,text,scale);' + ui_source[text_start.end():]
    audit_point = "    cmui_clear(fb);\n    (void)snprintf(heading, sizeof(heading),"
    assert ui_source.count(audit_point) == 1
    audit = '''    extern void test_compact_rows(unsigned,unsigned,unsigned,int,int);
    extern void test_compact_row(const char *,unsigned,unsigned,unsigned,unsigned,unsigned);
    extern void test_compact_gaps(void);
    test_compact_rows(first,visible,s_compact_count,margin*2,FB16_WIDTH-margin*2);
    for(unsigned i=0;i<s_compact_count;++i) test_compact_row(s_compact_rows[i].text,s_compact_rows[i].focused,s_compact_rows[i].dimmed,i,first,visible);
    test_compact_gaps();
'''
    ui_harness = OUT / "config_menu_ui.c"
    tab_point = '        fb16_fill_rect(fb, x, y, tab_w, height, bg);'
    assert ui_source.count(tab_point) == 1
    ui_source = ui_source.replace(tab_point,
        '        extern void test_compact_tab(int,int,int,int,const char *);\n'
        '        test_compact_tab(x,y,tab_w,height,label);\n' + tab_point)
    row_point = '        cmui_text_clipped(fb, text_x, y + 3,'
    assert ui_source.count(row_point) == 1
    ui_source = ui_source.replace(row_point,
        '        extern void test_compact_draw(const char *,int,int,int,int,int,int,unsigned,unsigned,unsigned);\n'
        '        test_compact_draw(row->text,text_x,y+3,text_right-text_x,scale,content_y,help_y-4,row->right_aligned,fg,bg);\n' + row_point)
    ui_harness.write_text(ui_source.replace(audit_point, audit + audit_point), encoding="utf-8")
    exe = OUT / "test.exe"
    command = [str(compiler),"-std=c11","-O0","-funsigned-char","-Wall","-Wextra","-Werror","-I",str(OUT),"-I",str(FRONT),
        str(harness),str(ui_harness),str(FRONT / "config_menu_logo_png.c"),
        str(ROOT / "ps_sources/lib/fb16.c"),str(ROOT / "ps_sources/lib/lodepng.c"),"-o",str(exe)]
    subprocess.run(command,check=True,env=env)
    # Compile the real main predicate with PUBLIC headers alone. This prevents
    # the extracted menu's private enums from hiding integration mistakes.
    route = OUT / "route-public.c"
    route.write_text('#include <assert.h>\n#include "config_menu.h"\n'
        'uint8_t config_menu_is_active(const config_menu_t *m){return m&&m->active;}\n'
        'uint8_t config_menu_usb0_sd_remote_active(const config_menu_t *m){(void)m;return 0;}\n'
        'uint8_t config_menu_ethernet_ftp_sd_remote_active(const config_menu_t *m){(void)m;return 0;}\n' + close_consumer +
        'int main(void){config_menu_t m={0};m.active=1;m.usb_binding_capture=CONFIG_MENU_USB_BIND_CAPTURE_NONE;'
        'm.about_credits_active=1;assert(ui_config_menu_has_close_consumer(&m));m.about_credits_active=0;'
        'assert(!ui_config_menu_has_close_consumer(&m));return 0;}\n')
    route_exe = OUT / "route-public.exe"
    subprocess.run([str(compiler),"-std=c11","-Wall","-Wextra","-Werror","-DAPPLETINI_HTTP_SERVICE=1",
        "-I",str(OUT),"-I",str(FRONT),str(route),"-o",str(route_exe)],check=True,env=env)
    subprocess.run([str(route_exe)],check=True,env=env)
    completed = subprocess.run([str(exe)],cwd=OUT,env=env,
                               capture_output=True,text=True,encoding="utf-8")
    (OUT / "RESULT.log").write_text(completed.stdout+completed.stderr)
    print(completed.stdout+completed.stderr,end="");completed.check_returncode()
    future_exe = OUT / "future-test.exe"
    subprocess.run(command[:-2]+["-DTEST_FUTURE_CREDITS=1","-o",str(future_exe)],check=True,env=env)
    future = subprocess.run([str(future_exe)],cwd=OUT,env=env,capture_output=True,text=True,encoding="utf-8")
    (OUT / "FUTURE-RESULT.log").write_text(future.stdout+future.stderr)
    print(future.stdout+future.stderr,end="");future.check_returncode()
    from PIL import Image
    for preview in OUT.glob("*.ppm"):
        with Image.open(preview) as image:
            image.save(preview.with_suffix(".png"))
    files = ("config_menu.c", "config_menu.h", "config_menu_ui.c", "config_menu_ui.h", "config_menu_help.c", "online_update_core.h", "main.c")
    (OUT / "validation.json").write_text(json.dumps({
        "hardware_test": False, "native_production_functions": True,
        "network_flash_and_storage_callbacks_mocked": True,
        "progress_metrics": "elapsed12.3seconds and44.0KiB/s visibly rendered across all8outputs",
        "credits_scope": "wrapped scroll reachability and viewport bounds across all8outputs",
        "signature_alignment": "actual emitted glyph right edge equals body text edge at wide and compact outputs; muted signature and blank section rows checked",
        "section_spacing": "wide section gaps >=24 pixels, compact section spacer rows, and signature-to-origin gap >=24 pixels across all8outputs",
        "future_credits": "80 test-only extra long entries; actual production wrapping and renderer; >160 logical rows at every output",
        "credit_navigation": "Up/Down and overlapping pages clamp, active focus survives BOOT presence and viewport changes, Back/Esc/Enter return to actions, MENU closes, tab entry resets",
        "main_routing": "actual close-consumer extraction protects About credit focus; normalized keyboard/gamepad UI keys; physical inputs are not emulated",
        "public_header_routing_compile": "real main predicate compiled separately without private menu enums",
        "credits_footer_hint": "actual production literal, visibly rendered while credits are active and absent from modals",
        "coverage": "every non-whitespace byte of each source credit and both intro lines was visibly rendered; no duplicate intro in a frame; fixed wide controls hash-stable during scroll",
        "failed_layout": "realistic all-fields FAILED diagnostics at1680x1050; measured visible rows recorded in stdout; all credits reachable without footer overlap",
        "persistent_diagnostic_cases": ["DNS", "expired TLS", "untrusted TLS", "request format (TLS not run)", "HTTP403", "invalid asset", "SD cleanup", "image validation", "install failure"],
        "resolutions": [[1920,1080],[1680,1050],[1280,1024],[1360,768],[1200,800],[1280,720],[1024,768],[640,400]],
        "source_sha256": {name:hashlib.sha256((FRONT/name).read_bytes()).hexdigest() for name in files},
        "stdout":completed.stdout,"exit_code":completed.returncode,"future_stdout":future.stdout,"future_exit_code":future.returncode,
        "test_script_sha256":hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
    },indent=2)+"\n",encoding="utf-8")


if __name__ == "__main__":
    main()
