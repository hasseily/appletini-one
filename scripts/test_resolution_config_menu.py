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
    for loader_name in ("config_menu_load_settings", "config_menu_read_settings_from_path"):
        loader_body = function(menu, loader_name)
        assert "menu->video_legacy_crt_blending = 0U;" in loader_body
        assert "menu->video_legacy_crt_explicit = 0U;" in loader_body
        assert "menu->video_blending_vertical_explicit = 0U;" in loader_body
        assert "menu->video_blending_horizontal_explicit = 0U;" in loader_body
        assert "menu->video_pixel_mask = CONFIG_DEFAULT_VIDEO_PIXEL_MASK;" in loader_body
        assert "config_menu_parse_key_value(menu, key, value);" in loader_body
    assert '"video.blur=%s\\n"' in function(menu, "config_menu_save_settings_to_path")
    assert '"video.dot.bleed=%s\\n"' in function(menu, "config_menu_save_settings_to_path")
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
#include "video_output.h"
#include "scanlines.h"
#include "video_blur.h"
#include "video_filter_config.h"
#include "video_pixel_mask.h"
#include "video_glow.h"
#include "video_ghosting.h"
#include "video_mono.h"
"""
    code += re.search(r"typedef enum \{\s+CONFIG_TAB_PROFILES.*?} config_tab_t;", internal, re.S).group(0) + "\n"
    for label_array in ("k_tab_labels", "k_compact_tab_labels"):
        code += re.search(r"static const char \* const " + label_array + r"\[CONFIG_TAB_COUNT\] = \{.*?\n};", menu, re.S).group(0) + "\n"
    code += r"""
static unsigned rendered_focus, resolution_visible, multiplier_visible, help_rows, help_min_scale;
static const char *expected_color_label;
static unsigned color_label_visible;
static unsigned record_tabs, expected_tab, visible_tabs, active_tabs;
static int tab_text_right;
enum { FIELD_RESOLUTION, FIELD_SIZE, FIELD_OUTPUT, FIELD_VARIANT, FIELD_ROM,
       FIELD_SCANLINES, FIELD_MASK, FIELD_DOT, FIELD_BLUR, FIELD_GLOW,
       FIELD_GHOST, FIELD_BORDER, FIELD_OUTSIDE, FIELD_BEZEL, FIELD_COUNT };
static const char *field_labels[FIELD_COUNT] = {
    "Output resolution", "Size multiplier", "Video output", "Color mode", "Video ROM",
    "Scanlines", "Pixel mask", "Dot bleed", "Phosphor blur", "Phosphor glow",
    "Phosphor ghosting", "Border color", "Outside ring", "Bezel"
};
static unsigned record_fields, recorded_fields;
static int pending_field=-1, field_x[FIELD_COUNT], field_y[FIELD_COUNT];
static uint16_t field_fg[FIELD_COUNT], field_bg[FIELD_COUNT];
static void record_string(uint16_t *fb, int x, int y, const char *text,
                           uint16_t fg, uint16_t bg, int scale)
{
    unsigned is_tab=0;
    assert(x >= 0 && y >= 0);
    assert(x + (int)strlen(text) * FB16_BUILTIN_FONT_ADVANCE_X * scale <= FB16_WIDTH);
    assert(y + FB16_BUILTIN_FONT_HEIGHT * scale <= FB16_HEIGHT);
    if (record_fields) {
        if (pending_field >= 0) {
            field_x[pending_field]=x;
            field_y[pending_field]=y;
            field_fg[pending_field]=fg;
            field_bg[pending_field]=bg;
            recorded_fields |= 1U<<pending_field;
            assert(scale==2);
            pending_field=-1;
        } else {
            for (unsigned i=0;i<FIELD_COUNT;++i) {
                if (strcmp(text,field_labels[i])==0) {
                    assert(scale==2);
                    pending_field=(int)i;
                    break;
                }
            }
        }
    }
    if (strstr(text, "Output resolution")) resolution_visible++;
    if (strstr(text, "Size multiplier")) multiplier_visible++;
    if (expected_color_label && strcmp(text,expected_color_label)==0) {
        ++color_label_visible;
        assert(scale==2);
    }
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
    uint8_t video_legacy_horizontal, video_legacy_vertical, video_smoothing_explicit, video_filter_explicit;
    uint8_t video_legacy_crt_blending, video_legacy_crt_explicit, video_blending_vertical_explicit;
    uint8_t video_blending_horizontal_explicit, video_pixel_mask;
    uint8_t boot_timeout_mode, boot_device;
    char bezel_path[32];
    struct {
        uint8_t (*is_apple_video_50hz)(void *);
        void (*set_video_blur)(void *, uint8_t);
        void (*set_video_pixel_mask)(void *, uint8_t);
        void *ctx;
    } platform;
    uint32_t item_focus;
} config_menu_t;
"""
    code += "\n".join(line for line in internal.splitlines() if line.startswith("#define CONFIG_VIDEO_ITEM_")) + "\n"
    code += "#define CONFIG_DEFAULT_VIDEO_COLOR_MODE APPLE_VIDEO_COLOR_COMPOSITE_MONITOR\n"
    code += "#define CONFIG_DEFAULT_VIDEO_PIXEL_MASK APPLETINI_VIDEO_PIXEL_MASK_OFF\n"
    code += "#define CONFIG_DEFAULT_VIDEO_BLUR_STRENGTH APPLETINI_VIDEO_BLUR_OFF\n"
    code += "#define CONFIG_DEFAULT_VIDEO_DOT_BLEED APPLETINI_VIDEO_DOT_BLEED_LIGHT\n"
    for name in ("config_menu_ascii_lower", "config_menu_str_ieq", "config_menu_bool_text",
                 "config_menu_video_output_text_value",
                 "config_menu_video_color_mode_text", "config_menu_video_color_mode_config",
                 "config_menu_video_color_mode_value", "config_menu_pal_accurate_modes_allowed",
                 "config_menu_video_color_mode_allowed", "config_menu_next_color_mode",
                 "config_menu_video_mono_color_text", "config_menu_video_variant_label",
                 "config_menu_video_variant_text",
                 "config_menu_video_blur_text", "config_menu_video_blur_config",
                 "config_menu_video_dot_bleed_text", "config_menu_video_dot_bleed_config",
                 "config_menu_parse_video_color_setting", "config_menu_parse_video_vertical_setting",
                 "config_menu_parse_video_horizontal_setting",
                 "config_menu_video_pixel_mask_config", "config_menu_video_pixel_mask_text",
                 "config_menu_cycle_video_pixel_mask", "config_menu_apply_video_pixel_mask",
                 "config_menu_resolve_video_blending", "config_menu_coerce_video_blur",
                 "config_menu_apply_video_blur",
                 "config_menu_scanlines_config", "config_menu_on_off",
                 "config_menu_video_mono_color_config", "config_menu_video_ghosting_config",
                 "config_menu_video_glow_config",
                 "config_menu_output_mode",
                 "config_menu_output_mode_text", "config_menu_cycle_output_mode",
                 "config_menu_size_multiplier", "config_menu_size_multiplier_text",
                 "config_menu_size_multiplier_config", "config_menu_cycle_size_multiplier",
                 "hgr_draw_value_item", "hgr_draw_value_item_dimmed",
                 "hgr_draw_check_item", "hgr_draw_check_item_dimmed",
                 "hgr_draw_video_ghosting_item", "hgr_draw_video_blur_item", "hgr_draw_video_glow_item"):
        code += function(menu, name)
    code += r"""
static const char *config_menu_video_output_text(uint8_t v) { return v ? "Monochrome" : "Color"; }
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
static FRESULT config_menu_open_path(FIL *file, const char *path, unsigned flags)
{ (void)path; return config_menu_open_cfg(file,flags); }
static void config_menu_reset_settings_only(config_menu_t *m)
{ m->video_color_mode=CONFIG_DEFAULT_VIDEO_COLOR_MODE; m->size_multiplier=0; }
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
static const char *config_menu_quote_path(const char *path, char *scratch, unsigned size)
{ (void)scratch; (void)size; return path; }
"""
    # Compile the production parser branches for every migrated video key.
    parser = function(menu, "config_menu_parse_key_value")
    code += "static void config_menu_parse_key_value(config_menu_t *menu, const char *key, const char *value) {\n"
    for key in ("video.size_multiplier", "video.output", "video.color.mode", "video.blending.horizontal",
                "video.blending.vertical", "video.blur", "video.dot.bleed", "video.pixel_mask"):
        branch_start = parser.index('} else if (strcmp(key, "' + key + '")') + 7
        branch_end = parser.index("    } else if", branch_start)
        code += "    " + parser[branch_start:branch_end] + "    }\n"
    code += "}\n"
    loader = function(menu, "config_menu_load_settings")
    for helper in re.findall(r"^    (config_menu_(?:coerce_\w+|migrate_\w+|usb_bindings_coerce))\(menu\);", loader, re.M):
        if helper != "config_menu_coerce_video_blur":
            code += "#define " + helper + "(menu) ((void)(menu))\n"
    code += re.search(r"^#define APPLETINI_CFG_VERSION .*", menu, re.M).group(0) + "\n"
    code += loader
    code += function(menu, "config_menu_read_settings_from_path")
    save = function(menu, "config_menu_save_settings_to_path")
    save_start = save.index('    APPEND_CFG("appletini.config.version=')
    save_end = save.index('\n\n', save_start)
    code += r"""
#define CONFIG_BOOT_TIMEOUT_OPEN_MENU 0
#define CONFIG_BOOT_TIMEOUT_UNLIMITED 1
#define CONFIG_BOOT_TIMEOUT_5S 2
#define CONFIG_BOOT_DEVICE_DISK2 0
static void save_video_settings(const config_menu_t *menu, char *output, unsigned size)
{
    char path_val[40];
#define APPEND_CFG(...) snprintf(output,size,__VA_ARGS__)
"""
    code += save[save_start:save_end] + "\n#undef APPEND_CFG\n}\n"
    help_lines = re.search(r"HELP\(video_size_multiplier,\s*(.*?)\);", help_source, re.S).group(1)
    code += "\nstatic const char *help[]={\n" + help_lines + "\n};\n"
    variant_lines = re.search(r"HELP\(video_variant,\s*(.*?)\);", help_source, re.S).group(1)
    code += "\nstatic const char *variant_help[]={\n" + variant_lines + "\n};\n"
    crt_lines = re.search(r"HELP\(video_blur,\s*(.*?)\);", help_source, re.S).group(1)
    code += "\nstatic const char *blur_help[]={\n" + crt_lines + "\n};\n"
    mask_lines = re.search(r"HELP\(video_pixel_mask,\s*(.*?)\);", help_source, re.S).group(1)
    code += "\nstatic const char *mask_help[]={\n" + mask_lines + "\n};\n"
    code += r"""
static uint8_t video_is_pal(void *context) { return *(uint8_t *)context; }
static void set_blur(void *context, uint8_t strength) { *(uint8_t *)context=strength; }
static void set_pixel_mask(void *context, uint8_t mode) { *(uint8_t *)context=mode; }
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
static unsigned compact_index(unsigned item,unsigned mono)
{ return item-(!mono && item>CONFIG_VIDEO_ITEM_DOT_BLEED); }
int main(void)
{
    config_menu_t m={0};
    const char *color_tokens[] = {"IDEALIZED", "RGB", "COMPOSITE_MONITOR", "COLOR_TV",
                                 "PAL_ACCURATE_COMPOSITE", "PAL_ACCURATE_TV"};
    assert(sizeof(color_tokens)/sizeof(color_tokens[0])==APPLE_VIDEO_COLOR_COUNT);
    for (unsigned color=0;color<APPLE_VIDEO_COLOR_COUNT;++color) {
        assert(strcmp(config_menu_video_color_mode_config(color),color_tokens[color])==0);
        assert(config_menu_video_color_mode_value(color_tokens[color])==color);
        assert(apple_video_color_mode_clamp(color)==color);
    }
    assert(config_menu_video_color_mode_value("Idealized_Mix")==APPLE_VIDEO_COLOR_IDEALIZED);
    assert(config_menu_video_color_mode_value("invalid")==APPLE_VIDEO_COLOR_COMPOSITE_MONITOR);
    uint8_t pal=0;
    m.platform.is_apple_video_50hz=video_is_pal;
    m.platform.ctx=&pal;
    assert(config_menu_next_color_mode(&m,APPLE_VIDEO_COLOR_TV,1)==APPLE_VIDEO_COLOR_IDEALIZED);
    assert(config_menu_next_color_mode(&m,APPLE_VIDEO_COLOR_IDEALIZED,-1)==APPLE_VIDEO_COLOR_TV);
    pal=1;
    for (unsigned color=0;color<APPLE_VIDEO_COLOR_COUNT;++color) {
        assert(config_menu_next_color_mode(&m,color,1)==(color+1)%APPLE_VIDEO_COLOR_COUNT);
        assert(config_menu_next_color_mode(&m,color,-1)==(color+APPLE_VIDEO_COLOR_COUNT-1)%APPLE_VIDEO_COLOR_COUNT);
    }
    uint8_t applied_blur=0;
    m.platform.set_video_blur=set_blur;
    m.platform.ctx=&applied_blur;
    for (unsigned strength=0;strength<4;++strength) {
        m.video_blur_strength=strength;
        config_menu_apply_video_blur(&m);
        assert(applied_blur==strength);
    }
    m.video_blur_strength=255;
    config_menu_apply_video_blur(&m);
    assert(applied_blur==3 && m.video_blur_strength==3);
    config_menu_apply_video_blur(NULL);
    m.platform.ctx=&pal;
    m.video_color_mode=APPLE_VIDEO_COLOR_IDEALIZED;
    m.video_mono_color=APPLE_VIDEO_MONO_WHITE;
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
    cfg_contents="appletini.config.version=124\nvideo.size_multiplier=2\n";
    config_menu_load_settings(&m);
    assert(m.size_multiplier==2 && cfg_saves==1);
    cfg_contents="appletini.config.version=124\nvideo.size_multiplier=invalid\n";
    config_menu_load_settings(&m);
    assert(m.size_multiplier==0 && cfg_saves==1);
    /* Compile real loaders and writer: explicit restored keys beat old axes
       in either order, for global and both profile loading policies. */
    unsigned migration_cases=0;
    const char *strength_tokens[]={"OFF","LIGHT","MEDIUM","STRONG"};
    for (unsigned mono=0;mono<2;++mono)
    for (unsigned horizontal=0;horizontal<4;++horizontal)
    for (unsigned vertical=0;vertical<4;++vertical)
    for (int blur=-1;blur<4;++blur)
    for (int dot=-1;dot<4;++dot)
    for (unsigned restored_last=0;restored_last<2;++restored_last) {
        char restored[100]={0}, axes[160], contents[400];
        unsigned used=0;
        if (blur>=0) used+=(unsigned)snprintf(restored+used,sizeof(restored)-used,
            "video.blur=%s\n",strength_tokens[blur]);
        if (dot>=0) snprintf(restored+used,sizeof(restored)-used,
            "video.dot.bleed=%s\n",strength_tokens[dot]);
        snprintf(axes,sizeof(axes),"video.blending.horizontal=%s\nvideo.blending.vertical=%s\n",
                 strength_tokens[horizontal],strength_tokens[vertical]);
        snprintf(contents,sizeof(contents),"appletini.config.version=124\nvideo.output=%s\n%s%s",
                 mono ? "MONOCHROME" : "COLOR",restored_last?axes:restored,restored_last?restored:axes);
        unsigned expected_blur=!mono && horizontal ? (horizontal==3 ? 3:1):0;
        if (vertical && expected_blur<2) expected_blur=2;
        if (blur>=0) expected_blur=(unsigned)blur;
        const unsigned expected_dot=dot>=0 ? (unsigned)dot:mono?horizontal:0;
        for (unsigned loader=0;loader<3;++loader) {
            cfg_contents=contents;
            m.video_blur_strength=m.video_dot_bleed=255;
            if (!loader) config_menu_load_settings(&m);
            else {
                uint32_t version=0;
                assert(config_menu_read_settings_from_path(&m,"profile/config.txt",loader-1,&version));
                assert(version==124);
            }
            assert(m.video_blur_strength==expected_blur && m.video_dot_bleed==expected_dot);
            char saved[2048], expected_line[64];
            save_video_settings(&m,saved,sizeof(saved));
            snprintf(expected_line,sizeof(expected_line),"video.blur=%s\n",strength_tokens[expected_blur]);
            assert(strstr(saved,expected_line));
            snprintf(expected_line,sizeof(expected_line),"video.dot.bleed=%s\n",strength_tokens[expected_dot]);
            assert(strstr(saved,expected_line));
            assert(!strstr(saved,"video.blending.") && !strstr(saved,"video.smoothing."));
            assert(!strstr(saved,"video.crt_blending=") && !strstr(saved,"IDEALIZED_MIX"));
            cfg_contents=saved; m.video_blur_strength=m.video_dot_bleed=255;
            if (!loader) config_menu_load_settings(&m);
            else assert(config_menu_read_settings_from_path(&m,"profile/config.txt",loader-1,NULL));
            assert(m.video_blur_strength==expected_blur && m.video_dot_bleed==expected_dot);
            ++migration_cases;
        }
    }
    cfg_contents="appletini.config.version=124\n";
    config_menu_load_settings(&m);
    assert(!m.video_legacy_crt_blending && !m.video_legacy_crt_explicit);
    assert(m.video_blur_strength==APPLETINI_VIDEO_BLUR_OFF);
    assert(m.video_dot_bleed==APPLETINI_VIDEO_DOT_BLEED_LIGHT);
    printf("PASS: %u restored filter migration/load/save/reload cases and original defaults\n",migration_cases);

    const char *mask_tokens[]={"OFF","APERTURE","SHADOW","LCD"};
    unsigned mask_cases=0;
    for (unsigned mode=0;mode<4;++mode) {
        assert(config_menu_video_pixel_mask_text(mask_tokens[mode])==mode);
        assert(strcmp(config_menu_video_pixel_mask_config(mode),mask_tokens[mode])==0);
        m.video_pixel_mask=mode;
        config_menu_cycle_video_pixel_mask(&m,1); assert(m.video_pixel_mask==(mode+1)%4);
        config_menu_cycle_video_pixel_mask(&m,-1); assert(m.video_pixel_mask==mode);
    }
    assert(config_menu_video_pixel_mask_text(NULL)==0);
    assert(config_menu_video_pixel_mask_text("unknown")==0);
    assert(config_menu_video_pixel_mask_text("shadow")==2);
    uint8_t runtime_mask=255;
    void *saved_context=m.platform.ctx;
    m.platform.ctx=&runtime_mask; m.platform.set_video_pixel_mask=set_pixel_mask;
    for (unsigned mode=0;mode<5;++mode) {
        m.video_pixel_mask=mode<4 ? mode : 255;
        config_menu_apply_video_pixel_mask(&m);
        assert(runtime_mask==(mode<4 ? mode : 0));
    }
    m.platform.ctx=saved_context; m.platform.set_video_pixel_mask=NULL;
    for (int mask=-2;mask<4;++mask)
    for (unsigned horizontal=0;horizontal<4;++horizontal)
    for (unsigned vertical=0;vertical<4;++vertical)
    for (unsigned loader=0;loader<3;++loader) {
        char contents[300], mask_line[64]={0}, saved[2048], expected_line[64];
        if (mask>=0) snprintf(mask_line,sizeof(mask_line),"video.pixel_mask=%s\n",mask_tokens[mask]);
        else if (mask==-1) snprintf(mask_line,sizeof(mask_line),"video.pixel_mask=invalid\n");
        snprintf(contents,sizeof(contents),"appletini.config.version=124\nvideo.dot.bleed=%s\nvideo.blur=%s\n%s",
                 strength_tokens[horizontal],strength_tokens[vertical],mask_line);
        cfg_contents=contents; m.video_pixel_mask=3;
        if (!loader) config_menu_load_settings(&m);
        else assert(config_menu_read_settings_from_path(&m,"profile/config.txt",loader-1,NULL));
        const unsigned expected=mask>=0 ? (unsigned)mask : 0;
        assert(m.video_pixel_mask==expected);
        assert(m.video_dot_bleed==horizontal && m.video_blur_strength==vertical);
        save_video_settings(&m,saved,sizeof(saved));
        snprintf(expected_line,sizeof(expected_line),"video.pixel_mask=%s\n",mask_tokens[expected]);
        assert(strstr(saved,expected_line));
        cfg_contents=saved; m.video_pixel_mask=255;
        if (!loader) config_menu_load_settings(&m);
        else assert(config_menu_read_settings_from_path(&m,"profile/config.txt",loader-1,NULL));
        assert(m.video_pixel_mask==expected);
        assert(m.video_dot_bleed==horizontal && m.video_blur_strength==vertical);
        ++mask_cases;
    }
    printf("PASS: %u independent mask/global/profile cases, defaults, invalid keys, cycling and runtime apply\n",mask_cases);
    m.video_color_mode=APPLE_VIDEO_COLOR_IDEALIZED;
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
        for (unsigned mono=0;mono<2;++mono)
        for (unsigned focus=0;focus<CONFIG_VIDEO_ITEM_COUNT;++focus) {
            if (!mono && focus==CONFIG_VIDEO_ITEM_DOT_BLEED) continue;
            m.item_focus=focus; rendered_focus=0; resolution_visible=0; multiplier_visible=0;
            m.video_blur_strength=focus&3U;
            m.video_pixel_mask=focus&3U;
            m.video_output_mono=mono;
            cmui_compact_begin();
            config_menu_draw_video(fb,&m,0,0,1480);
            assert(s_compact_count==CONFIG_VIDEO_ITEM_COUNT-(!mono));
            assert(strstr(s_compact_rows[0].text,"Output resolution")!=NULL);
            assert(strstr(s_compact_rows[1].text,"Size multiplier")!=NULL);
            assert(strstr(s_compact_rows[2].text,"Video output")!=NULL);
            assert(s_compact_rows[compact_index(focus,mono)].focused);
            assert(strstr(s_compact_rows[compact_index(CONFIG_VIDEO_ITEM_VARIANT,mono)].text,
                          m.video_output_mono ? "White" : "Idealized"));
            assert(CONFIG_VIDEO_ITEM_BLUR==CONFIG_VIDEO_ITEM_DOT_BLEED+1);
            if (mono) assert(strstr(s_compact_rows[compact_index(CONFIG_VIDEO_ITEM_DOT_BLEED,mono)].text,"Dot bleed"));
            assert(strstr(s_compact_rows[compact_index(CONFIG_VIDEO_ITEM_BLUR,mono)].text,"Phosphor blur"));
            assert(CONFIG_VIDEO_ITEM_PIXEL_MASK==CONFIG_VIDEO_ITEM_SCANLINES+1);
            assert(CONFIG_VIDEO_ITEM_DOT_BLEED==CONFIG_VIDEO_ITEM_PIXEL_MASK+1);
            assert(strstr(s_compact_rows[compact_index(CONFIG_VIDEO_ITEM_SCANLINES,mono)].text,"Scanlines"));
            assert(strstr(s_compact_rows[compact_index(CONFIG_VIDEO_ITEM_PIXEL_MASK,mono)].text,"Pixel mask"));
            assert(strstr(s_compact_rows[compact_index(CONFIG_VIDEO_ITEM_PIXEL_MASK,mono)].text,
                          appletini_video_pixel_mask_name(m.video_pixel_mask)));
            assert(CONFIG_VIDEO_ITEM_GHOSTING==CONFIG_VIDEO_ITEM_GLOW+1);
            assert(strstr(s_compact_rows[compact_index(CONFIG_VIDEO_ITEM_GLOW,mono)].text,"Phosphor glow"));
            assert(strstr(s_compact_rows[compact_index(CONFIG_VIDEO_ITEM_GHOSTING,mono)].text,"Phosphor ghosting"));
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
        m.video_output_mono=0;
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
            for (unsigned mono=0;mono<2;++mono)
            for (unsigned strength=0;strength<4;++strength)
            for (unsigned color=0;color<APPLE_VIDEO_COLOR_COUNT;++color) {
                m.video_output_mono=mono;
                m.video_color_mode=color;
                m.scanlines_mode=strength;
                m.video_pixel_mask=strength;
                m.video_dot_bleed=strength;
                m.video_blur_strength=strength;
                m.video_glow_strength=strength;
                m.video_ghosting_strength=strength;
                m.border_flood=strength&1;
                field_labels[FIELD_VARIANT]=config_menu_video_variant_label(&m);
                record_fields=1; recorded_fields=0; pending_field=-1;
                config_menu_draw_video(fb,&m,body.x,body.y,body.w);
                record_fields=0;
                assert(pending_field==-1);
                assert(recorded_fields==((1U<<FIELD_COUNT)-1U)-(mono ? 0U : 1U<<FIELD_DOT));
                const int left=d->width==1680 ? 674 : 768;
                const int right=d->width==1680 ? 1306 : 1520;
                for (unsigned field=0;field<FIELD_COUNT;++field) {
                    if (!mono && field==FIELD_DOT) continue;
                    const unsigned rhs=field==FIELD_SIZE || field==FIELD_VARIANT ||
                        field==FIELD_MASK || field==FIELD_GHOST || field==FIELD_BORDER ||
                        (field==FIELD_BLUR && mono);
                    assert(field_x[field]==(rhs ? right : left));
                }
                assert(field_y[FIELD_SCANLINES]==field_y[FIELD_MASK]);
                assert(field_y[FIELD_SCANLINES]<field_y[FIELD_BLUR]);
                assert(field_y[FIELD_BLUR]<field_y[FIELD_GLOW]);
                assert(field_y[FIELD_GLOW]==field_y[FIELD_GHOST]);
                if (mono) assert(field_y[FIELD_DOT]==field_y[FIELD_BLUR]);
                if (m.border_flood) {
                    assert(field_fg[FIELD_BEZEL]==CMUI_COLOR_DIM);
                    assert(field_bg[FIELD_BEZEL]==CMUI_COLOR_ROW_DISABLED);
                }
            }
            printf("PASS: %s Video value columns x=%d/%d, all modes/strengths/color+mono and row order\n",
                   d->name,field_x[FIELD_SCANLINES],field_x[FIELD_MASK]);
            m.video_output_mono=0; m.border_flood=0;
            for (unsigned color=0;color<APPLE_VIDEO_COLOR_COUNT;++color) {
                m.video_color_mode=color;
                expected_color_label=config_menu_video_color_mode_text(color);
                color_label_visible=0;
                config_menu_draw_video(fb,&m,body.x,body.y,body.w);
                assert(color_label_visible==1);
            }
            expected_color_label=NULL;
            m.video_color_mode=APPLE_VIDEO_COLOR_IDEALIZED;
            config_menu_draw_video(fb,&m,body.x,body.y,body.w);
            int help_h=body.h<810 ? 190 : 210;
            cmui_rect_t help_rect={body.x,body.y+body.h-help_h,body.w,help_h};
            assert(body.y+11*(CMUI_ROW_H+CMUI_ROW_GAP)+CMUI_ROW_H<=help_rect.y);
            help_rows=0; help_min_scale=2;
            cmui_help_panel(fb,&help_rect,"Help",help,4);
            assert(help_min_scale==2);
            assert(d->width==1680 ? help_rows>4 : help_rows==4);
            cmui_footer(fb,&footer,"Settings saved",0,0,0);
        }
        save_preview(fb,d->name);
        for (unsigned choice=0;choice<3;++choice) {
            char preview_name[80];
            m.video_color_mode=choice==1 ? APPLE_VIDEO_COLOR_PAL_ACCURATE_COMPOSITE : APPLE_VIDEO_COLOR_IDEALIZED;
            m.item_focus=choice==1 ? CONFIG_VIDEO_ITEM_VARIANT :
                choice==2 ? CONFIG_VIDEO_ITEM_PIXEL_MASK : CONFIG_VIDEO_ITEM_BLUR;
            m.video_blur_strength=3;
            m.video_pixel_mask=APPLETINI_VIDEO_PIXEL_MASK_APERTURE;
            const char **preview_help=choice==1 ? variant_help : choice==2 ? mask_help : blur_help;
            const unsigned preview_lines=choice==1 ? 6 : choice==2 ? 5 : 4;
            if (d->width>=1680 && d->height>=1000) {
                cmui_rect_t nav,body,footer;
                cmui_screen_rects(&nav,&body,&footer);
                cmui_clear(fb);
                cmui_header(fb,"Appletini","Video settings preview",0);
                config_menu_draw_video(fb,&m,body.x,body.y,body.w);
                int help_h=body.h<810 ? 190 : 210;
                cmui_rect_t help_rect={body.x,body.y+body.h-help_h,body.w,help_h};
                help_rows=0; help_min_scale=2;
                cmui_help_panel(fb,&help_rect,"Help",preview_help,preview_lines);
                assert(help_min_scale==2 && help_rows==preview_lines);
                cmui_footer(fb,&footer,"Settings saved",0,0,0);
            } else {
                cmui_compact_begin();
                config_menu_draw_video(fb,&m,0,0,1480);
                cmui_help_panel(fb,NULL,"Help",preview_help,preview_lines);
                finish_compact(fb,CONFIG_TAB_VIDEO,"Settings saved",0);
            }
            snprintf(preview_name,sizeof(preview_name),"%s-%s",d->name,
                     choice==1 ? "pal" : choice==2 ? "pixel-mask" : "blur");
            save_preview(fb,preview_name);
        }
        m.video_color_mode=APPLE_VIDEO_COLOR_IDEALIZED;
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
    puts("PASS: six color modes, restored Blur/Dot controls and migration, resolution/size settings, constrained cycling, all compact tabs visible and selected, native menu text bounds, focused scrolling, framebuffer guards");
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
