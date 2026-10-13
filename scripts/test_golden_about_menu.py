#!/usr/bin/env python3
"""Exercise the real About controls/modal and render full and compact previews."""
from pathlib import Path
import os
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
FRONT = ROOT / "ps_sources/frontend"
OUT = ROOT / "build/golden_about_menu"


def function(source: str, name: str) -> str:
    match = re.search(r"(?m)^(?:static\s+)?(?:const\s+)?\w+\s+\**" + name + r"\([^;]*?\)\s*\{", source)
    assert match, name
    pos, depth = match.end(), 1
    while depth:
        depth += (source[pos] == "{") - (source[pos] == "}")
        pos += 1
    return source[match.start():pos] + "\n"


def main() -> None:
    source = (FRONT / "config_menu.c").read_text()
    assert "APPLETINI_BOOT_IMAGE_VERSION_FULL" not in function(source, "config_menu_draw_about")
    assert "config_menu_refresh_golden_boot(menu)" in function(source, "config_menu_on_tab_entered")
    assert "config_menu_refresh_golden_boot(menu)" in function(source, "config_menu_set_active")
    assert "config_menu_start_golden_boot_update(menu)" in function(source, "config_menu_activate_item")
    handler = function(source, "config_menu_handle_input")
    assert handler.index("config_menu_golden_boot_handle_input") < handler.index("input.key == UI_KEY_MENU")
    draws = "\n".join(function(source, name) for name in (
        "config_menu_draw_about_section", "config_menu_draw_about_bottom_text",
        "config_menu_draw_about", "config_menu_draw_golden_boot_modal"))
    assert "get_golden_boot" not in draws and "f_stat" not in draws
    assert '"Hardware:' in source and '"Firmware:   Henri' in source
    assert '"Project origin -' in source and "Project origin and harness" not in source
    OUT.mkdir(parents=True, exist_ok=True)
    # Header-only I2C type: the real menu structure includes RTC/sensor types,
    # while this fixture never calls hardware I2C operations.
    (OUT / "xiicps.h").write_text("#ifndef TEST_XIICPS_H\n#define TEST_XIICPS_H\ntypedef struct { unsigned unused; } XIicPs;\n#endif\n")
    compiler = shutil.which("gcc") or shutil.which("clang")
    if not compiler:
        for candidate in (Path("C:/msys64/ucrt64/bin/gcc.exe"), Path("E:/AMDDesignTools/2025.2/tps/mingw/10.0.0/win64.o/nt/bin/gcc.exe")):
            if candidate.exists():
                compiler = str(candidate)
                break
    assert compiler, "A native compiler is required"
    env = dict(os.environ, PATH=str(Path(compiler).parent) + os.pathsep + os.environ.get("PATH", ""))
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
    code += "\n".join(function(source, name) for name in (
        "config_menu_refresh_golden_boot", "config_menu_golden_boot_update_finished",
        "config_menu_start_golden_boot_update", "config_menu_golden_boot_handle_input",
        "config_menu_on_tab_entered"))
    code += r'''
static void config_menu_activate_item(config_menu_t *m)
{ if(m->tab==CONFIG_TAB_ABOUT && !m->item_focus) config_menu_start_golden_boot_update(m); }
'''
    code += handler
    for array in ("k_about_contributors", "k_about_third_party"):
        code += re.search(r"static const char \* const " + array + r"\[\] = \{.*?\n\};", source, re.S).group(0) + "\n"
    code += r'''
static unsigned drawn_button, drawn_version, focused_buttons, disabled_buttons;
static int body_x, body_y, body_w, body_h;
static void record_row(uint16_t *fb,int x,int y,int w,uint8_t focus,uint8_t dim,const char *text)
{
    if (!cmui_compact_active()) {
        assert(x>=body_x && x+w<=body_x+body_w && y>=body_y && y+CMUI_ROW_H<=body_y+body_h);
        assert(cmui_text_width(text,CMUI_BODY_SCALE)<=w-34);
    }
    if (!strcmp(text,"Update Golden Boot...")) { ++drawn_button; focused_buttons+=focus!=0; disabled_buttons+=dim!=0; }
    cmui_row(fb,x,y,w,focus,dim,text);
}
static void record_text(uint16_t *fb,int x,int y,int w,const char *text,uint32_t fg,uint32_t bg,int scale)
{
    if (!strncmp(text,"Golden Boot:",12)) {
        ++drawn_version;
        assert(strstr(text,installed_version));
        assert(!strstr(text,"Boot Boot"));
        if(!cmui_compact_active()) assert(cmui_text_width(text,scale)<=w);
    }
    cmui_text_clipped(fb,x,y,w,text,fg,bg,scale);
}
#define cmui_row record_row
#define cmui_text_clipped record_text
'''
    help_source = (FRONT / "config_menu_help.c").read_text()
    code += help_source.replace('#include "config_menu_internal.h"', '')
    code += draws
    code += "#undef cmui_row\n#undef cmui_text_clipped\n"
    code += r'''
static unsigned compact_focus, compact_button, compact_bounds, compact_disabled;
void test_compact_rows(unsigned first,unsigned visible,unsigned count)
{ assert(first<count && visible>0); ++compact_bounds; }
void test_compact_row(const char *text,unsigned focus,unsigned dim,unsigned index,unsigned first,unsigned visible)
{
    if(strstr(text,"Golden Boot:") && strstr(text,"[Update Golden Boot...]")) {
        ++compact_button; compact_disabled+=dim!=0; assert(cmui_text_width(text,FB16_WIDTH>=1024?2:1)<=FB16_WIDTH-32);
    }
    if(focus) { ++compact_focus; assert(index>=first && index<first+visible); }
}
static void press(config_menu_t *menu, ui_key_t key)
{ assert(config_menu_handle_input(menu,(ui_input_t){key,1,0})); }
static void render(config_menu_t *menu,unsigned width,unsigned height,unsigned mode)
{
    static const char *const tabs[]={"Prof","Boot","Video","SP","Disk","CPU","Sound","Slot2","Net","TW","Clock","RAM","USB","Print","About"};
    assert(fb16_set_size((int)width,(int)height));
    size_t pixels=width*height;
    uint16_t *guard=calloc(pixels+32,sizeof(*guard)); assert(guard);
    for(unsigned i=0;i<16;++i) guard[i]=guard[pixels+16+i]=0xa55a;
    uint16_t *fb=guard+16;
    cmui_rect_t nav,body,footer; cmui_screen_rects(&nav,&body,&footer);
    const unsigned compact=width<1680 || height<1000;
    unsigned old_reads=version_reads,old_checks=file_checks,old_allowed=allowed_checks;
    drawn_button=drawn_version=focused_buttons=disabled_buttons=compact_focus=compact_button=compact_disabled=0;
    if(compact) { body=(cmui_rect_t){0,0,1480,812}; cmui_compact_begin(); }
    else { cmui_clear(fb); cmui_header(fb,"Appletini",APPLETINI_FIRMWARE_IMAGE_VERSION_FULL,0); }
    body_x=body.x; body_y=body.y; body_w=body.w; body_h=body.h;
    if(mode) config_menu_draw_golden_boot_modal(fb,menu,&body);
    else config_menu_draw_about(fb,menu,body.x,body.y,body.w,body.h);
    if(compact) cmui_compact_finish(fb,"About",tabs,CONFIG_TAB_COUNT,CONFIG_TAB_ABOUT,menu->status,menu->status_warning,0,0);
    else cmui_footer(fb,&footer,menu->status,menu->status_warning,0,0);
    assert(version_reads==old_reads && file_checks==old_checks && allowed_checks==old_allowed);
    if(mode==0) {
        if(compact) {
            assert(compact_button==(menu->golden_boot_file_present!=0));
            assert(compact_focus==(menu->golden_boot_file_present!=0 && menu->item_focus==0));
            assert(compact_disabled==(menu->golden_boot_file_present!=0 && menu->golden_boot_update_allowed==0));
        } else {
            assert(drawn_version==1 && drawn_button==(menu->golden_boot_file_present!=0));
            assert(disabled_buttons==(menu->golden_boot_file_present!=0 && menu->golden_boot_update_allowed==0));
        }
    } else if(compact) assert(compact_focus==(mode==1));
    for(unsigned i=0;i<16;++i) assert(guard[i]==0xa55a && guard[pixels+16+i]==0xa55a);
    char path[96]; snprintf(path,sizeof(path),"about_%ux%u_mode%u_focus%u_present%u_allowed%u.ppm",width,height,mode,
        mode==1?menu->golden_boot_confirm_update:menu->item_focus,menu->golden_boot_file_present,menu->golden_boot_update_allowed);
    FILE *ppm=fopen(path,"wb"); assert(ppm); fprintf(ppm,"P6\n%u %u\n255\n",width,height);
    for(size_t i=0;i<pixels;++i) {
        unsigned char rgb[]={(unsigned char)((fb[i]>>11)*255/31),(unsigned char)(((fb[i]>>5)&63)*255/63),(unsigned char)((fb[i]&31)*255/31)};
        fwrite(rgb,1,3,ppm);
    }
    fclose(ppm); free(guard);
}
int main(void)
{
    config_menu_t menu={0}; menu.active=1; menu.tab=CONFIG_TAB_ABOUT;
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
    const unsigned sizes[][2]={{1920,1080},{1280,720},{1024,768},{640,400}};
    for(unsigned s=0;s<4;++s) render(&menu,sizes[s][0],sizes[s][1],0);
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
    for(unsigned s=0;s<4;++s) {
        menu.item_focus=0; render(&menu,sizes[s][0],sizes[s][1],0);
        if(s) {
            press(&menu,UI_KEY_DOWN); assert(menu.item_focus==1);
            render(&menu,sizes[s][0],sizes[s][1],0);
            press(&menu,UI_KEY_ENTER); assert(menu.item_focus==0 && !menu.golden_boot_update_confirm);
        }
        menu.golden_boot_update_confirm=1; menu.golden_boot_confirm_update=0; render(&menu,sizes[s][0],sizes[s][1],1);
        menu.golden_boot_confirm_update=1; render(&menu,sizes[s][0],sizes[s][1],1);
        menu.golden_boot_update_confirm=0; menu.golden_boot_update_pending=1; render(&menu,sizes[s][0],sizes[s][1],2);
        menu.golden_boot_update_pending=0; menu.golden_boot_file_present=0; render(&menu,sizes[s][0],sizes[s][1],0);
        menu.golden_boot_file_present=1;
    }
    assert(compact_bounds);
    puts("PASS: actual installed version, missing/stale BOOT.BIN, explicit Cancel/Update, repeated input, queued update and error recovery");
    puts("PASS: physical Apple II boot-phase gate, missing-callback denial, disabled activation and eligibility loss during confirmation");
    puts("PASS: full/compact About and modal drawing, button visibility, focus and framebuffer bounds; no flash/SD work during draw");
    return 0;
}
'''
    harness = OUT / "test.c"
    harness.write_text(code)
    ui_source = (FRONT / "config_menu_ui.c").read_text()
    audit_point = "    cmui_clear(fb);\n    (void)snprintf(heading, sizeof(heading),"
    assert ui_source.count(audit_point) == 1
    audit = '''    extern void test_compact_rows(unsigned,unsigned,unsigned);
    extern void test_compact_row(const char *,unsigned,unsigned,unsigned,unsigned,unsigned);
    test_compact_rows(first,visible,s_compact_count);
    for(unsigned i=0;i<s_compact_count;++i) test_compact_row(s_compact_rows[i].text,s_compact_rows[i].focused,s_compact_rows[i].dimmed,i,first,visible);
'''
    ui_harness = OUT / "config_menu_ui.c"
    ui_harness.write_text(ui_source.replace(audit_point, audit + audit_point))
    exe = OUT / "test.exe"
    subprocess.run([compiler,"-std=c11","-O0","-funsigned-char","-Wall","-Wextra","-Werror","-I",str(OUT),"-I",str(FRONT),
        str(harness),str(ui_harness),str(FRONT / "config_menu_logo_png.c"),
        str(ROOT / "ps_sources/lib/fb16.c"),str(ROOT / "ps_sources/lib/lodepng.c"),"-o",str(exe)],check=True,env=env)
    subprocess.run([str(exe)],cwd=OUT,check=True,env=env)
    from PIL import Image
    for preview in OUT.glob("*.ppm"):
        with Image.open(preview) as image:
            image.save(preview.with_suffix(".png"))


if __name__ == "__main__":
    main()
