#!/usr/bin/env python3
"""Compile SSI mixer settings, register writes, and actual menu rendering."""
from pathlib import Path
import os
import shutil
import subprocess

from test_joystick_config_menu import function

ROOT = Path(__file__).resolve().parents[1]
FRONT = ROOT / "ps_sources/frontend"
OUT = ROOT / "build/phasor_ssi_ui"


def main() -> None:
    public = (FRONT / "config_menu.h").read_text()
    internal = (FRONT / "config_menu_internal.h").read_text()
    menu = (FRONT / "config_menu.c").read_text()
    callbacks = (FRONT / "main.c").read_text()
    phasor = (FRONT / "config_menu_phasor.c").read_text()
    # Older configuration files start with defaults before their keys load.
    assert "config_menu_phasor_set_defaults(menu);" in function(menu, "config_menu_init")
    assert "config_menu_phasor_set_defaults(menu);" in function(menu, "config_menu_reset_settings_only")
    assert "config_menu_phasor_append_settings(menu" in function(menu, "config_menu_save_settings_to_path")
    assert "config_menu_phasor_parse_setting(menu, key, value)" in menu
    assert "config_menu_phasor_item_count()" in menu

    code = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "card_control_regs.h"
#include "config_menu_ui.h"
#include "config_menu_help.h"
'''
    code += internal[internal.index("/* Coordinate and palette"):
                     internal.index("uint8_t config_menu_appendf(")]
    code += "typedef struct {\n    struct {\n        void *ctx;\n"
    code += "        void (*set_slot_enabled)(void *, uint8_t, uint8_t);\n"
    code += public[public.index("    void (*set_phasor_pan)"):
                   public.index("    void (*set_mouse_sensitivity)")]
    code += "    } platform;\n    unsigned item_focus;\n"
    code += public[public.index("    uint8_t mockingboard_slot4_enabled;"):
                   public.index("    uint8_t ethernet_slot1_enabled;")]
    code += "} config_menu_t;\n"
    code += r'''
static unsigned saved, writes;
static uint32_t pan_lo, pan_hi, audio;
static void reg_write(uint32_t reg, uint32_t value)
{
    if (reg == CARD_CTRL_PHASOR_PAN_LO_REG) pan_lo = value;
    else if (reg == CARD_CTRL_PHASOR_PAN_HI_REG) pan_hi = value;
    else { assert(reg == CARD_CTRL_PHASOR_AUDIO_REG); audio = value; }
    ++writes;
}
#define REG_WRITE reg_write
static void config_menu_save_settings(config_menu_t *menu) { (void)menu; ++saved; }
static void config_menu_refresh_vtw_slowdown(config_menu_t *menu) { (void)menu; }
'''
    for name in ("control_set_phasor_pan", "phasor_audio_pack5", "control_set_phasor_audio"):
        code += function(callbacks, name)
    for name in ("config_menu_pan_clamp", "config_menu_appendf",
                 "hgr_draw_check_item", "hgr_draw_value_item"):
        code += function(menu, name)
    code += phasor.replace('#include "config_menu_internal.h"', "")
    help_source = (FRONT / "config_menu_help.c").read_text()
    # Only this page's actual help declarations are needed by the harness.
    code += help_source[help_source.index("#define HELP_COUNT"):
                        help_source.index("/* ======================================================================== */")]
    code += help_source[help_source.index("HELP(phasor,"):
                        help_source.index("/* ======================================================================== */", help_source.index("HELP(phasor,"))]
    code += r'''
static unsigned expected_focus, compact_checks, focused_rows;
void test_compact_bounds(unsigned first, unsigned visible, unsigned count)
{
    assert(count == config_menu_phasor_item_count());
    assert(expected_focus >= first && expected_focus < first + visible);
    ++compact_checks;
}
void test_compact_row(const char *text, unsigned focused, int width, int available_width)
{
    (void)text;
    assert(width <= available_width);
    focused_rows += focused != 0;
}
static void check_audio_fields(const config_menu_t *menu)
{
    assert((audio & 31U) == ((unsigned)menu->phasor_bass & 31U));
    assert(((audio >> 5) & 31U) == ((unsigned)menu->phasor_mid & 31U));
    assert(((audio >> 10) & 31U) == ((unsigned)menu->phasor_treble & 31U));
    assert(((audio >> 15) & 31U) == PHASOR_WARMTH_DEFAULT);
    assert(((audio >> 20) & 31U) == ((unsigned)menu->phasor_volume & 31U));
    assert(((audio >> 25) & 1U) == menu->phasor_psg_ay_mode);
    assert(((audio >> 26) & 1U) == menu->phasor_mockingboard_only);
    assert((audio >> 27) == ((unsigned)menu->phasor_ssi_volume_db & 31U));
    for (unsigned chip=0; chip<2; ++chip) {
        assert(((pan_hi >> (24 + 4*chip)) & 15U) == menu->phasor_ssi_pan[chip]);
    }
    for (unsigned channel=0; channel<12; ++channel) {
        uint32_t word=channel<6 ? pan_lo : pan_hi;
        assert(((word >> ((channel%6)*4)) & 15U) == menu->mockingboard_pan[channel]);
    }
}
static void round_trip(config_menu_t *menu)
{
    char buffer[2048], *line;
    int len=0;
    config_menu_t loaded={0};
    config_menu_phasor_set_defaults(&loaded);
    assert(config_menu_phasor_append_settings(menu,buffer,sizeof(buffer),&len));
    line=strtok(buffer,"\n");
    while (line) {
        char *eq=strchr(line,'='); assert(eq); *eq++='\0';
        assert(config_menu_phasor_parse_setting(&loaded,line,eq));
        line=strtok(NULL,"\n");
    }
    assert(loaded.phasor_ssi_volume_db == menu->phasor_ssi_volume_db);
    assert(!memcmp(loaded.phasor_ssi_pan,menu->phasor_ssi_pan,2));
    assert(!memcmp(loaded.mockingboard_pan,menu->mockingboard_pan,12));
    assert(loaded.phasor_mockingboard_only == menu->phasor_mockingboard_only);
}
static void render(config_menu_t *menu, const char *path)
{
    const unsigned compact=FB16_HEIGHT < 1080;
    static const char *const tabs[]={"Profiles","Boot","Video","SmartPort","Disk","CPU","Phasor","Slot2","Net","TW","Clock","RAM","USB","Print","About"};
    uint16_t *fb=calloc(FB16_WIDTH*FB16_HEIGHT,sizeof(*fb)); assert(fb);
    cmui_rect_t nav,body,footer; cmui_screen_rects(&nav,&body,&footer);
    if (compact) { body=(cmui_rect_t){0,0,1480,812}; cmui_compact_begin(); }
    else { cmui_clear(fb); cmui_header(fb,"Appletini","F1.2.5-d2 SSI mixer",1); }
    config_menu_phasor_draw(fb,menu,body.x,body.y,body.w);
    const char *const *help=help_phasor;
    unsigned help_count=sizeof(help_phasor)/sizeof(*help_phasor);
    for (unsigned i=0; i<sizeof(phasor_overrides)/sizeof(*phasor_overrides); ++i) {
        if (phasor_overrides[i].item == menu->item_focus) {
            help=phasor_overrides[i].lines; help_count=phasor_overrides[i].count;
        }
    }
    cmui_rect_t help_rect={body.x,body.y+body.h-210,body.w,210};
    cmui_help_panel(fb,&help_rect,"Help",help,help_count);
    if (compact) {
        expected_focus=menu->item_focus; focused_rows=0;
        cmui_compact_finish(fb,"Phasor",tabs,15,CONFIG_TAB_MOCKINGBOARD,"Settings saved",0,1,0);
        assert(focused_rows == 1);
    } else {
        for (unsigned i=0; i<help_count; ++i) assert(cmui_text_width(help[i],2) <= body.w-40);
        cmui_rect_t navrow={nav.x,nav.y+6*42,nav.w,34}; cmui_nav_item(fb,&navrow,"Phasor",1,1);
        cmui_footer(fb,&footer,"Settings saved",0,1,0);
    }
    if (path) {
        FILE *file=fopen(path,"wb"); assert(file);
        fprintf(file,"P6\n%d %d\n255\n",FB16_WIDTH,FB16_HEIGHT);
        for(unsigned i=0; i<(unsigned)(FB16_WIDTH*FB16_HEIGHT); ++i) {
            unsigned char rgb[3]={(fb[i]>>11)*255/31,((fb[i]>>5)&63)*255/63,(fb[i]&31)*255/31};
            fwrite(rgb,1,3,file);
        }
        fclose(file);
    }
    free(fb);
}
int main(void)
{
    config_menu_t menu={0};
    menu.platform.set_phasor_pan=control_set_phasor_pan;
    menu.platform.set_phasor_audio=control_set_phasor_audio;
    config_menu_phasor_set_defaults(&menu);
    assert(menu.phasor_ssi_volume_db == 2 && menu.phasor_ssi_pan[0] == 0 && menu.phasor_ssi_pan[1] == 15);
    config_menu_phasor_apply(&menu);
    assert(audio == 0x12040000U && pan_hi == 0xF05B5B5BU && pan_lo == 0x005B5B5BU);
    /* Old configurations have no SSI settings: the new defaults remain. */
    assert(config_menu_phasor_parse_setting(&menu,"phasor.volume","-2"));
    assert(menu.phasor_ssi_volume_db == 2 && menu.phasor_ssi_pan[1] == 15);
    menu.phasor_bass=-3; menu.phasor_mid=4; menu.phasor_treble=-7;
    for (int db=-5; db<=5; ++db) for (unsigned p0=0; p0<16; ++p0)
    for (unsigned p1=0; p1<16; ++p1) for (unsigned mb=0; mb<2; ++mb) {
        menu.phasor_ssi_volume_db=(int8_t)db;
        menu.phasor_ssi_pan[0]=(uint8_t)p0; menu.phasor_ssi_pan[1]=(uint8_t)p1;
        menu.phasor_mockingboard_only=(uint8_t)mb; menu.phasor_psg_ay_mode=(uint8_t)(p1%2);
        config_menu_phasor_apply(&menu); check_audio_fields(&menu); round_trip(&menu);
    }
    menu.item_focus=PHASOR_SSI_VOLUME_FOCUS;
    config_menu_phasor_adjust(&menu,100); assert(menu.phasor_ssi_volume_db == 5);
    config_menu_phasor_adjust(&menu,-100); assert(menu.phasor_ssi_volume_db == -5);
    config_menu_phasor_activate(&menu); assert(menu.phasor_ssi_volume_db == 0);
    for (unsigned chip=0; chip<2; ++chip) {
        menu.item_focus=PHASOR_SSI_PAN_FOCUS_BASE+chip;
        config_menu_phasor_adjust(&menu,-100); assert(menu.phasor_ssi_pan[chip] == 0);
        config_menu_phasor_adjust(&menu,100); assert(menu.phasor_ssi_pan[chip] == 15);
        config_menu_phasor_activate(&menu); assert(menu.phasor_ssi_pan[chip] == 8);
        check_audio_fields(&menu);
    }
    assert(saved && writes);
    assert(config_menu_phasor_parse_setting(&menu,"phasor.ssi.volume_db","-99"));
    assert(menu.phasor_ssi_volume_db == -5);
    assert(config_menu_phasor_parse_setting(&menu,"phasor.ssi.volume_db","99"));
    assert(menu.phasor_ssi_volume_db == 5);
    assert(!config_menu_phasor_parse_setting(&menu,"phasor.ssi.pan.2","3"));
    config_menu_phasor_set_defaults(&menu);
    menu.mockingboard_slot4_enabled=1;
    menu.item_focus=PHASOR_SSI_VOLUME_FOCUS;
    assert(fb16_set_size(1920,1080)); render(&menu,"phasor_1920x1080.ppm");
    const unsigned sizes[][2]={{640,400},{640,480},{1024,768}};
    for (unsigned size=0; size<3; ++size) {
        assert(fb16_set_size(sizes[size][0],sizes[size][1]));
        for (unsigned focus=0; focus<config_menu_phasor_item_count(); ++focus) {
            char path[80]; menu.item_focus=focus;
            snprintf(path,sizeof(path),"phasor_%ux%u_focus%u.ppm",sizes[size][0],sizes[size][1],focus);
            render(&menu,focus>=PHASOR_SSI_VOLUME_FOCUS ? path : NULL);
        }
    }
    assert(compact_checks == 3*config_menu_phasor_item_count());
    puts("PASS: 5,632 SSI gain/pan/mode combinations, config round trips, bounds, register preservation, defaults, 66 compact focus checks");
    return 0;
}
'''
    OUT.mkdir(parents=True, exist_ok=True)
    harness = OUT / "test.c"
    harness.write_text(code)
    ui = (FRONT / "config_menu_ui.c").read_text()
    point = "    cmui_clear(fb);\n    (void)snprintf(heading, sizeof(heading),"
    assert ui.count(point) == 1
    audit = '''    extern void test_compact_bounds(unsigned, unsigned, unsigned);
    extern void test_compact_row(const char *, unsigned, int, int);
    test_compact_bounds(first, visible, s_compact_count);
    for (uint32_t row=0; row<s_compact_count; ++row) {
        test_compact_row(s_compact_rows[row].text,s_compact_rows[row].focused,
                         cmui_text_width(s_compact_rows[row].text,scale),FB16_WIDTH-4*margin);
    }
'''
    ui_harness = OUT / "config_menu_ui.c"
    ui_harness.write_text(ui.replace(point, audit + point))
    compiler = shutil.which("gcc") or shutil.which("clang")
    if not compiler:
        compiler = "E:/AMDDesignTools/2025.2/tps/mingw/10.0.0/win64.o/nt/bin/gcc.exe"
    env = dict(os.environ, PATH=str(Path(compiler).parent) + os.pathsep + os.environ.get("PATH", ""))
    exe = OUT / "test.exe"
    subprocess.run([compiler, "-std=c11", "-O0", "-funsigned-char", "-Wall", "-Wextra", "-Werror",
                    "-I", str(FRONT), str(harness), str(ui_harness),
                    str(FRONT / "config_menu_logo_png.c"), str(ROOT / "ps_sources/lib/fb16.c"),
                    str(ROOT / "ps_sources/lib/lodepng.c"), "-o", str(exe)], check=True, env=env)
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
