#!/usr/bin/env python3
"""Compile real resolution helpers and menu rows; check layout and framebuffer bounds."""
from pathlib import Path
import os
import re
import shutil
import subprocess
from test_joystick_config_menu import function

ROOT = Path(__file__).resolve().parents[1]
FRONT = ROOT / "ps_sources/frontend"
OUT = ROOT / "build/resolution_config_menu"


def main():
    menu = (FRONT / "config_menu.c").read_text()
    tabs = (FRONT / "config_menu_main_tabs.c").read_text()
    internal = (FRONT / "config_menu_internal.h").read_text()
    help_source = (FRONT / "config_menu_help.c").read_text()
    assert menu.count("menu->output_mode = DISPLAY_MODE_DEFAULT;") == 2
    assert '"video.resolution=%s\\n"' in menu
    assert 'menu->output_mode = config_menu_output_mode_text(value);' in menu
    assert "config_menu_cycle_output_mode(menu, delta);" in menu
    assert "config_menu_cycle_output_mode(menu, 1);" in menu
    assert menu.count("menu->size_multiplier = 0U;") == 3
    assert '"video.size_multiplier=%s\\n"' in menu
    assert "menu->size_multiplier = config_menu_size_multiplier_text(value);" in menu
    assert "config_menu_cycle_size_multiplier(menu, delta);" in menu
    assert "config_menu_cycle_size_multiplier(menu, 1);" in menu
    assert "config_menu_reset_settings_only(menu);" in function(menu, "config_menu_read_settings_from_path")
    assert "cmui_compact_finish" in function(menu, "config_menu_draw")
    assert "k_compact_tab_labels, CONFIG_TAB_COUNT" in function(menu, "config_menu_draw")
    OUT.mkdir(parents=True, exist_ok=True)
    compiler = shutil.which("gcc") or shutil.which("clang")
    if not compiler:
        compiler = "E:/AMDDesignTools/2025.2/tps/mingw/10.0.0/win64.o/nt/bin/gcc.exe"
    env = dict(os.environ, PATH=str(Path(compiler).parent) + os.pathsep + os.environ.get("PATH", ""))
    code = r"""
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config_menu_ui.h"
#include "display_modes.h"
#include "scanlines.h"
#include "video_blur.h"
#include "video_glow.h"
#include "video_ghosting.h"
#include "video_mono.h"
"""
    code += re.search(r"typedef enum \{\s+CONFIG_TAB_PROFILES.*?} config_tab_t;", internal, re.S).group(0) + "\n"
    for label_array in ("k_tab_labels", "k_compact_tab_labels"):
        code += re.search(r"static const char \* const " + label_array + r"\[CONFIG_TAB_COUNT\] = \{.*?\n};", menu, re.S).group(0) + "\n"
    code += r"""
static unsigned rendered_focus, resolution_visible, multiplier_visible, help_rows, help_min_scale;
static unsigned record_tabs, expected_tab, visible_tabs, active_tabs;
static int tab_text_right;
static void record_string(uint16_t *fb, int x, int y, const char *text,
                           uint16_t fg, uint16_t bg, int scale)
{
    unsigned is_tab=0;
    assert(x >= 0 && y >= 0);
    assert(x + (int)strlen(text) * FB16_BUILTIN_FONT_ADVANCE_X * scale <= FB16_WIDTH);
    assert(y + FB16_BUILTIN_FONT_HEIGHT * scale <= FB16_HEIGHT);
    if (strstr(text, "Output resolution")) resolution_visible++;
    if (strstr(text, "Size multiplier")) multiplier_visible++;
    if (strcmp(text,"Output resolution")==0 || strcmp(text,"Size multiplier")==0) assert(scale==2);
    if (strcmp(text,"IIgs border")==0) assert(scale==2);
    if (record_tabs) {
        for (unsigned i=0; i<CONFIG_TAB_COUNT; ++i) {
            if (strcmp(text,k_compact_tab_labels[i])!=0) continue;
            assert(scale==2);
            assert(x>tab_text_right);
            tab_text_right=x+(int)strlen(text)*FB16_BUILTIN_FONT_ADVANCE_X*scale;
            assert((visible_tabs & (1U<<i))==0);
            visible_tabs |= 1U<<i;
            is_tab=1;
            if (i==expected_tab) {
                assert(fg==CMUI_COLOR_ACCENT && bg==CMUI_COLOR_ROW_ACTIVE);
                active_tabs++;
            } else {
                assert(fg==CMUI_COLOR_MUTED && bg==CMUI_COLOR_PANEL);
            }
        }
    }
    if (bg == CMUI_COLOR_ROW_ACTIVE && !is_tab) rendered_focus++;
    if (bg == CMUI_COLOR_PANEL && fg == CMUI_COLOR_MUTED) {
        ++help_rows;
        if ((unsigned)scale < help_min_scale) help_min_scale=(unsigned)scale;
    }
    fb16_string_scaled(fb,x,y,text,fg,bg,scale);
}
#define fb16_string_scaled record_string
#include "config_menu_ui.c"
#undef fb16_string_scaled
static void finish_compact(uint16_t *fb, unsigned selected_tab, const char *status, unsigned usb)
{
    expected_tab=selected_tab; visible_tabs=0; active_tabs=0; record_tabs=1; tab_text_right=0;
    cmui_compact_finish(fb,k_tab_labels[selected_tab],k_compact_tab_labels,
                        CONFIG_TAB_COUNT,selected_tab,status,0,(uint8_t)usb,0);
    record_tabs=0;
    assert(visible_tabs==(1U<<CONFIG_TAB_COUNT)-1U);
    assert(active_tabs==1);
}
#define HGR_WHITE CMUI_COLOR_TEXT
#define HGR_DIMMED CMUI_COLOR_DIM
typedef struct {
    uint8_t output_mode, size_multiplier, video_output_mono, video_mono_color, video_color_mode;
    uint8_t video_dot_bleed, scanlines_mode, video_blur_strength, video_glow_strength;
    uint8_t video_ghosting_strength, border_flood, border_enabled, video7_auto_mono_enabled;
    uint8_t dhgr_col140m_enabled, border_color, show_bezel, show_debugging, format_badge_enabled;
    uint8_t vtw_turbo_enabled, settings_loaded, session_only;
    uint32_t item_focus;
} config_menu_t;
"""
    code += "\n".join(line for line in internal.splitlines() if line.startswith("#define CONFIG_VIDEO_ITEM_")) + "\n"
    for name in ("config_menu_ascii_lower", "config_menu_str_ieq", "config_menu_output_mode",
                 "config_menu_output_mode_text", "config_menu_cycle_output_mode",
                 "config_menu_size_multiplier", "config_menu_size_multiplier_text",
                 "config_menu_size_multiplier_config", "config_menu_cycle_size_multiplier",
                 "hgr_draw_value_item", "hgr_draw_value_item_dimmed",
                 "hgr_draw_check_item", "hgr_draw_check_item_dimmed",
                 "hgr_draw_video_ghosting_item", "hgr_draw_video_blur_item", "hgr_draw_video_glow_item"):
        code += function(menu, name)
    code += r"""
static const char *config_menu_video_output_text(uint8_t v) { return v ? "Monochrome" : "Color"; }
static const char *config_menu_video_variant_label(const config_menu_t *m) { (void)m; return "Color mode"; }
static const char *config_menu_video_variant_text(const config_menu_t *m) { (void)m; return "Composite Monitor"; }
static const char *config_menu_border_color_text(uint8_t c) { (void)c; return "Black"; }
static const char *config_menu_border_outside_text(uint8_t f) { return f ? "Flood" : "Bezel"; }
static const char *config_menu_video_rom_text(const config_menu_t *m) { (void)m; return "Enhanced US"; }
static const char *config_menu_bezel_text(const config_menu_t *m) { (void)m; return "bezel.png"; }
"""
    code += function(tabs, "config_menu_draw_video")
    code += r"""
typedef unsigned UINT;
typedef int FRESULT;
typedef struct { int unused; } FIL;
#define FR_OK 0
#define FR_NO_FILE 1
#define FR_DISK_ERR 2
#define FA_READ 1
#define APPLETINI_CFG_MAX 4096
#define CONFIG_DEFAULT_VTW_TURBO_ENABLED 0
#define config_menu_slot2_reset(menu) ((void)(menu))
static const char *cfg_contents;
static int cfg_read_result;
static unsigned cfg_saves, cfg_saved_multiplier;
static FRESULT config_menu_open_cfg(FIL *file, unsigned flags)
{ (void)file; (void)flags; return FR_OK; }
static FRESULT f_read(FIL *file, char *buffer, unsigned size, unsigned *read)
{
    (void)file; assert(strlen(cfg_contents)<size);
    *read=(unsigned)strlen(cfg_contents); memcpy(buffer,cfg_contents,*read);
    return cfg_read_result;
}
static FRESULT f_close(FIL *file) { (void)file; return FR_OK; }
static void config_menu_save_settings(config_menu_t *m)
{ ++cfg_saves; cfg_saved_multiplier=config_menu_size_multiplier(m); m->session_only=0; }
static void config_menu_set_status(config_menu_t *m, unsigned warning, const char *text)
{ (void)m; (void)warning; (void)text; }
static void config_menu_set_sd_error(config_menu_t *m, const char *text, FRESULT result)
{ (void)m; (void)text; (void)result; }
static char *config_menu_parse_config_line(char *line, char **value)
{ char *equals=strchr(line,'='); if (!equals) return NULL; *equals=0; *value=equals+1; return line; }
static void config_menu_parse_key_value(config_menu_t *m, const char *key, const char *value)
{ if (strcmp(key,"video.size_multiplier")==0) m->size_multiplier=config_menu_size_multiplier_text(value); }
"""
    loader = function(menu, "config_menu_load_settings")
    for helper in re.findall(r"^    (config_menu_(?:coerce_\w+|migrate_\w+|usb_bindings_coerce))\(menu\);", loader, re.M):
        code += "#define " + helper + "(menu) ((void)(menu))\n"
    code += re.search(r"^#define APPLETINI_CFG_VERSION .*", menu, re.M).group(0) + "\n"
    code += loader
    help_lines = re.search(r"HELP\(video_size_multiplier,\s*(.*?)\);", help_source, re.S).group(1)
    code += "\nstatic const char *help[]={\n" + help_lines + "\n};\n"
    code += r"""
static void save_preview(uint16_t *fb, const char *name)
{
    char path[80]; snprintf(path,sizeof(path),"%s.ppm",name);
    FILE *out=fopen(path,"wb"); assert(out);
    fprintf(out,"P6\n%d %d\n255\n",FB16_WIDTH,FB16_HEIGHT);
    for (int i=0; i<FB16_WIDTH*FB16_HEIGHT; ++i) {
        uint32_t rgb=fb16_to_bgra32(fb[i]);
        fputc((rgb>>16)&255,out); fputc((rgb>>8)&255,out); fputc(rgb&255,out);
    }
    fclose(out);
}
int main(void)
{
    config_menu_t m={0};
    assert(config_menu_output_mode(NULL)==DISPLAY_MODE_DEFAULT);
    m.output_mode=255; assert(config_menu_output_mode(&m)==DISPLAY_MODE_DEFAULT);
    assert(config_menu_output_mode_text("nonsense")==DISPLAY_MODE_DEFAULT);
    assert(config_menu_output_mode_text("640x400")==DISPLAY_MODE_DEFAULT);
    assert(config_menu_output_mode_text("1920x1200")==DISPLAY_MODE_DEFAULT);
    assert(config_menu_output_mode_text("1024X768")==0);
    assert(config_menu_size_multiplier(NULL)==0);
    m.size_multiplier=255; assert(config_menu_size_multiplier(&m)==0);
    assert(config_menu_size_multiplier_text("max")==0);
    assert(config_menu_size_multiplier_text("MAX")==0);
    assert(config_menu_size_multiplier_text("nonsense")==0);
    assert(config_menu_size_multiplier_text("3")==0);
    assert(config_menu_size_multiplier_text("2x")==0);
    assert(config_menu_size_multiplier_text("1")==1);
    assert(config_menu_size_multiplier_text("2")==2);
    for (unsigned pref=0;pref<=2;++pref) {
        m.size_multiplier=(uint8_t)pref;
        assert(config_menu_size_multiplier_text(config_menu_size_multiplier_config(&m))==pref);
    }
    m.size_multiplier=1; cfg_contents="appletini.config.version=118\n";
    cfg_read_result=FR_DISK_ERR;
    config_menu_load_settings(&m);
    assert(m.size_multiplier==1 && m.session_only && !cfg_saves);
    cfg_read_result=FR_OK;
    config_menu_load_settings(&m);
    assert(m.size_multiplier==0 && cfg_saves==1 && cfg_saved_multiplier==0);
    cfg_contents="appletini.config.version=120\nvideo.size_multiplier=2\n";
    config_menu_load_settings(&m);
    assert(m.size_multiplier==2 && cfg_saves==1);
    cfg_contents="appletini.config.version=120\nvideo.size_multiplier=invalid\n";
    config_menu_load_settings(&m);
    assert(m.size_multiplier==0 && cfg_saves==1);
    const char *ui_order[] = {
        "1024x768", "1200x800", "1280x1024", "1360x768", "1680x1050", "1920x1080"
    };
    assert(sizeof(ui_order)/sizeof(ui_order[0])==DISPLAY_MODE_COUNT);
    m.output_mode=config_menu_output_mode_text(ui_order[0]);
    for (unsigned i=0; i<DISPLAY_MODE_COUNT; ++i) {
        assert(strcmp(display_mode_get(m.output_mode)->name,ui_order[i])==0);
        config_menu_cycle_output_mode(&m,1);
    }
    assert(strcmp(display_mode_get(m.output_mode)->name,ui_order[0])==0);
    for (unsigned i=DISPLAY_MODE_COUNT; i>0; --i) {
        config_menu_cycle_output_mode(&m,-1);
        assert(strcmp(display_mode_get(m.output_mode)->name,ui_order[i-1])==0);
    }
    for (unsigned mode=0; mode<DISPLAY_MODE_COUNT; ++mode) {
        const display_mode_t *d=display_mode_get(mode);
        assert(config_menu_output_mode_text(d->name)==mode);
        m.output_mode=(uint8_t)mode;
        m.size_multiplier=2;
        config_menu_cycle_output_mode(&m,1);
        assert(m.output_mode!=mode);
        config_menu_cycle_output_mode(&m,-1); assert(m.output_mode==mode);
        assert(config_menu_size_multiplier(&m)==2); /* Resolution changes preserve the request. */
        const unsigned maximum=(mode==0) ? 1U : 2U;
        assert(display_mode_max_multiplier((uint8_t)mode)==maximum);
        m.size_multiplier=0;
        for (unsigned pref=1;pref<=maximum;++pref) {
            config_menu_cycle_size_multiplier(&m,1);
            assert(m.size_multiplier==pref);
        }
        config_menu_cycle_size_multiplier(&m,1); assert(m.size_multiplier==0);
        config_menu_cycle_size_multiplier(&m,-1); assert(m.size_multiplier==maximum);
        for (unsigned pref=maximum;pref>0;--pref) {
            config_menu_cycle_size_multiplier(&m,-1);
            assert(m.size_multiplier==pref-1);
        }
        m.size_multiplier=2;
        config_menu_cycle_size_multiplier(&m,-1); assert(m.size_multiplier==1);
        m.size_multiplier=2;
        config_menu_cycle_size_multiplier(&m,1); assert(m.size_multiplier==0);
        assert(fb16_set_size(d->width,d->height));
        size_t pixels=(size_t)d->width*d->height;
        uint16_t *allocation=malloc((pixels+128)*sizeof(uint16_t)); assert(allocation);
        for (size_t i=0;i<pixels+128;++i) allocation[i]=0xA55A;
        uint16_t *fb=allocation+64;
        for (unsigned focus=0;focus<CONFIG_VIDEO_ITEM_COUNT;++focus) {
            if (focus==CONFIG_VIDEO_ITEM_DOT_BLEED) continue;
            m.item_focus=focus; rendered_focus=0; resolution_visible=0; multiplier_visible=0;
            cmui_compact_begin();
            config_menu_draw_video(fb,&m,0,0,1480);
            assert(s_compact_count==CONFIG_VIDEO_ITEM_COUNT-1);
            assert(strstr(s_compact_rows[0].text,"Output resolution")!=NULL);
            assert(strstr(s_compact_rows[1].text,"Size multiplier")!=NULL);
            assert(strstr(s_compact_rows[2].text,"Video output")!=NULL);
            if (focus<=CONFIG_VIDEO_ITEM_OUTPUT) assert(s_compact_rows[focus].focused);
            unsigned focused=0;
            for (unsigned i=0;i<s_compact_count;++i) focused+=s_compact_rows[i].focused!=0;
            assert(focused==1);
            cmui_help_panel(fb,NULL,"Help",help,4);
            finish_compact(fb,CONFIG_TAB_VIDEO,"Settings saved",0);
            assert(rendered_focus>0);
            if (focus==CONFIG_VIDEO_ITEM_RESOLUTION) assert(resolution_visible);
            if (focus==CONFIG_VIDEO_ITEM_SIZE_MULTIPLIER) assert(multiplier_visible);
            for (unsigned i=0;i<64;++i) {
                assert(allocation[i]==0xA55A);
                assert(allocation[pixels+64+i]==0xA55A);
            }
        }
        m.size_multiplier=2;
        cmui_compact_begin();
        config_menu_draw_video(fb,&m,0,0,1480);
        assert(strstr(s_compact_rows[CONFIG_VIDEO_ITEM_SIZE_MULTIPLIER].text,
                      mode==0 ? "2x (1x fit)" : "2x")!=NULL);
        m.size_multiplier=0;
        m.item_focus=CONFIG_VIDEO_ITEM_RESOLUTION;
        for (unsigned selected=0;selected<CONFIG_TAB_COUNT;++selected) {
            cmui_compact_begin();
            cmui_compact_entry("Focused setting","value",1,0);
            rendered_focus=0;
            finish_compact(fb,selected,"Tab navigation test",0);
            assert(rendered_focus==1);
        }
        cmui_compact_begin();
        config_menu_draw_video(fb,&m,0,0,1480);
        cmui_help_panel(fb,NULL,"Help",help,4);
        finish_compact(fb,CONFIG_TAB_VIDEO,"Settings saved",0);
        if (d->width>=1680 && d->height>=1000) {
            cmui_rect_t nav,body,footer;
            cmui_screen_rects(&nav,&body,&footer);
            cmui_clear(fb);
            cmui_header(fb,"Appletini","Output resolution preview",0);
            config_menu_draw_video(fb,&m,body.x,body.y,body.w);
            int help_h=body.h<810 ? 190 : 210;
            cmui_rect_t help_rect={body.x,body.y+body.h-help_h,body.w,help_h};
            assert(body.y+13*(CMUI_ROW_H+CMUI_ROW_GAP)+CMUI_ROW_H<=help_rect.y);
            help_rows=0; help_min_scale=2;
            cmui_help_panel(fb,&help_rect,"Help",help,4);
            assert(help_min_scale==2);
            assert(d->width==1680 ? help_rows>4 : help_rows==4);
            cmui_footer(fb,&footer,"Settings saved",0,0,0);
        }
        save_preview(fb,d->name);
        if (d->width==1680) {
            cmui_rect_t small_help={380,154,1252,150};
            help_rows=0; help_min_scale=2;
            cmui_help_panel(fb,&small_help,"Help",help,4);
            assert(help_min_scale==1 && help_rows==4);
        }
        cmui_compact_begin();
        for (unsigned i=0;i<70;++i) {
            char label[32]; snprintf(label,sizeof(label),"Scrollable item %u",i);
            cmui_compact_entry(label,"value",i==69,0);
        }
        rendered_focus=0;
        finish_compact(fb,CONFIG_TAB_ABOUT,"Scroll test",1);
        assert(rendered_focus>0);
        for (unsigned i=0;i<64;++i) {
            assert(allocation[i]==0xA55A);
            assert(allocation[pixels+64+i]==0xA55A);
        }
        free(allocation);
    }
    puts("PASS: resolution/size settings, round-trip values, constrained cycling, all compact tabs visible and selected, native menu text bounds, focused scrolling, framebuffer guards");
    return 0;
}
"""
    harness = OUT / "test.c"
    harness.write_text(code)
    exe = OUT / "test.exe"
    subprocess.run([compiler,"-std=c11","-O0","-funsigned-char","-Wall","-Wextra","-Werror",
                    "-I",str(FRONT),str(harness),str(FRONT / "config_menu_logo_png.c"),
                    str(ROOT / "ps_sources/lib/fb16.c"),str(ROOT / "ps_sources/lib/lodepng.c"),
                    "-o",str(exe)],check=True,env=env)
    subprocess.run([str(exe)],cwd=OUT,check=True,env=env)
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
