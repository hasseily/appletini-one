#!/usr/bin/env python3
"""Check RGB565 mask rounding and execute the compositor's bank/exclusion code."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

from test_joystick_config_menu import function

ROOT = Path(__file__).resolve().parents[1]


def main():
    source = (ROOT / "ps_sources/frontend/compositor.c").read_text()
    extracted = "\n".join(function(source, name) for name in (
        "compositor_set_video_pixel_mask", "compositor_video_pixel_mask",
        "compositor_frame_pixel_mask", "compositor_frame_output_size", "compositor_pixel_mask_exclude",
        "pixel_mask_bounds", "pixel_mask_stage"))
    # The extractor returns the full function, including its signature.
    harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "video_pixel_mask.h"
#include "compositor_layout.h"
#include "display_modes.h"
static video_pixel_mask_frame_t s_pixel_mask[3], s_pixel_mask_uploaded[3];
static uint8_t s_pixel_mask_upload_valid[3], s_pixel_mask_supported;
static uint8_t s_pixel_mask_collecting, s_video_pixel_mask, s_force_full_refresh, s_writer_idx;
static uint8_t s_paused;
static struct { int output_width, output_height; } s_picture_rect[3];
static uint32_t registers[256], writes;
#define FB_MASK_BANK_REG(slot,word) (64U + (slot)*32U + (word))
#define REG_WRITE(address,value) (registers[(address)]=(value), ++writes)
''' + extracted + r'''
static uint16_t oracle(unsigned p, unsigned mode, unsigned x, unsigned y)
{
    unsigned channels[3] = {p >> 11, (p >> 5) & 63, p & 31};
    unsigned bits[3] = {5,6,5}, out[3];
    for (unsigned c=0; c<3; ++c) {
        unsigned v = channels[c] * (1U << (8-bits[c])) +
                     channels[c] / (1U << (2*bits[c]-8));
        int dim = mode == 3 ? (x % 3 == 2 || y % 3 == 2) :
                  mode == 1 || mode == 2 ? c != (x+(mode==2 ? y%2 : 0))%3 : 0;
        if (dim) v -= v/4;
        out[c] = v / (1U << (8-bits[c]));
    }
    return (uint16_t)((out[0]<<11)|(out[1]<<5)|out[2]);
}
static void check_layout_viewports(void)
{
    unsigned cases = 0;
    for (unsigned mode = 0; mode < DISPLAY_MODE_COUNT; ++mode) {
        const display_mode_t *output = display_mode_get(mode);
        for (unsigned scale = 0; scale <= 2; ++scale) {
            comp_layout_set_size_multiplier((uint8_t)scale);
            assert(comp_layout_set_output_size(output->width, output->height));
            for (unsigned shr = 0; shr < 2; ++shr) {
                const comp_viewport_t *view = shr ? &comp_shr_viewport : &comp_legacy_viewport;
                for (unsigned border = 0; border < 2; ++border) {
                    const int x = border ? view->border_x : view->x;
                    const int y = border ? view->border_y : view->y;
                    const int width = border ? view->border_width : view->width;
                    const int height = border ? view->border_height : view->height;
                    const int x0 = x > 0 ? x : 0;
                    const int y0 = y > 0 ? y : 0;
                    const int x1 = x + width < output->width ? x + width : output->width;
                    const int y1 = y + height < output->height ? y + height : output->height;
                    video_pixel_mask_frame_t frame = {0};
                    assert(video_pixel_mask_clip_rect(&frame.viewport, x, y, width, height,
                                                      output->width, output->height));
                    assert(frame.viewport.x0 == x0 && frame.viewport.x1 == x1);
                    assert(frame.viewport.y0 == y0 && frame.viewport.y1 == y1);
                    for (unsigned mask = 1; mask <= 3; ++mask) {
                        frame.mode = (uint8_t)mask;
                        /* The first visible pixel starts phase zero even when
                         * the original Apple border extends outside the output. */
                        for (int dy = -1; dy < 6; ++dy) {
                            for (int dx = -1; dx < 6; ++dx) {
                                const uint16_t expected = dx < 0 || dy < 0 ? 65535 :
                                    oracle(65535, mask, (unsigned)dx, (unsigned)dy);
                                assert(video_pixel_mask_frame_rgb565(&frame, 65535,
                                                                     x0 + dx, y0 + dy) == expected);
                            }
                        }
                        assert(video_pixel_mask_frame_rgb565(&frame, 65535, x1, y0) == 65535);
                        assert(video_pixel_mask_frame_rgb565(&frame, 65535, x0, y1) == 65535);
                        assert(video_pixel_mask_frame_rgb565(&frame, 65535, x1 - 1, y1 - 1) ==
                               oracle(65535, mask, x1 - x0 - 1, y1 - y0 - 1));
                    }
                    if (border && scale != 1) {
                        if (!shr && output->width == 1360) assert(y == -64 && y0 == 0);
                        if (!shr && output->width == 1200) assert(x == -16 && y == -48 && x0 == 0 && y0 == 0);
                        if (shr && output->width == 1280) assert(x == -56 && x0 == 0);
                    }
                    ++cases;
                }
            }
        }
    }
    assert(cases == 72);
    comp_layout_set_size_multiplier(0);
    assert(comp_layout_set_output_size(1920, 1080));
    puts("PASS 72 real layout viewports: six resolutions, Max/1x/2x, legacy/SHR, borders on/off, clipped bounds and mask phase");
}
int main(void)
{
    check_layout_viewports();
    for (unsigned mode=0; mode<=4; ++mode)
        for (unsigned y=0; y<6; ++y)
            for (unsigned x=0; x<3; ++x)
                for (unsigned p=0; p<=65535; ++p)
                    assert(video_pixel_mask_rgb565(p,mode,x,y)==oracle(p,mode,x,y));
    video_pixel_mask_frame_t f = {.mode=2,.viewport={4,12,3,10},.exclusion_count=1,
                                  .excluded={{6,9,5,7}}};
    for (int y=-1;y<13;++y) for(int x=-1;x<15;++x) {
        int enabled=x>=4&&x<12&&y>=3&&y<10&&!(x>=6&&x<9&&y>=5&&y<7);
        assert(video_pixel_mask_frame_rgb565(&f,65535,x,y)==
               (enabled ? oracle(65535,2,x-4,y-3):65535));
    }
    assert(video_pixel_mask_frame_rgb565(NULL,123,4,3)==123);
    int output_width, output_height;
    assert(!compositor_frame_output_size(0,&output_width,&output_height));
    s_picture_rect[0].output_width=1360; s_picture_rect[0].output_height=768;
    assert(compositor_frame_output_size(0,&output_width,&output_height));
    assert(output_width==1360 && output_height==768);
    s_paused=1;
    assert(!compositor_frame_output_size(0,&output_width,&output_height));
    s_paused=0;
    assert(!compositor_frame_output_size(3,&output_width,&output_height));
    assert(!compositor_frame_output_size(0,NULL,&output_height));
    video_pixel_mask_rect_t clipped;
    assert(video_pixel_mask_clip_rect(&clipped,64,-64,1232,896,1360,768));
    assert(clipped.x0==64 && clipped.x1==1296 && clipped.y0==0 && clipped.y1==768);
    assert(video_pixel_mask_clip_rect(&clipped,-16,-48,1232,896,1200,800));
    assert(clipped.x0==0 && clipped.x1==1200 && clipped.y0==0 && clipped.y1==800);
    assert(!video_pixel_mask_clip_rect(&clipped,-20,0,10,10,1200,800));
    assert(clipped.x0==0 && clipped.x1==0);
    compositor_set_video_pixel_mask(3); assert(compositor_video_pixel_mask()==3 && s_force_full_refresh);
    compositor_set_video_pixel_mask(255); assert(compositor_video_pixel_mask()==0);
    s_writer_idx=1; s_pixel_mask[1]=f;
    compositor_pixel_mask_exclude(0,0,2,2); assert(s_pixel_mask[1].exclusion_count==1);
    s_pixel_mask_collecting=1;
    compositor_pixel_mask_exclude(-2,-3,4,5);
    assert(s_pixel_mask[1].exclusion_count==2);
    assert(s_pixel_mask[1].excluded[1].x0==0 && s_pixel_mask[1].excluded[1].x1==2);
    compositor_pixel_mask_exclude(1919,1079,9,9);
    assert(s_pixel_mask[1].excluded[2].x1==1920 && s_pixel_mask[1].excluded[2].y1==1080);
    compositor_pixel_mask_exclude(1920,0,9,9); assert(s_pixel_mask[1].exclusion_count==3);
    for(int i=0;i<5;++i) compositor_pixel_mask_exclude(0,0,1,1);
    assert(s_pixel_mask[1].mode==2 && s_pixel_mask[1].exclusion_count==8);
    compositor_pixel_mask_exclude(0,0,1,1); assert(s_pixel_mask[1].mode==0);
    assert(!compositor_frame_pixel_mask(3,&f) && !compositor_frame_pixel_mask(0,NULL));
    s_pixel_mask[1]=f; assert(compositor_frame_pixel_mask(1,&f));
    pixel_mask_stage(1); assert(writes==0); /* Old PL capability bypass. */
    s_pixel_mask_supported=1; pixel_mask_stage(1); assert(writes==20);
    assert(registers[96]==0x3e400000 && registers[97]==0x12);
    assert(registers[98]==0x000c0004 && registers[99]==0x000a0003);
    assert(registers[100]==0x00090006 && registers[101]==0x00070005);
    for(unsigned i=0;i<256;++i) if(i<96 || i>=116) assert(registers[i]==0);
    pixel_mask_stage(1); assert(writes==20);
    s_pixel_mask[1].mode=3; pixel_mask_stage(1); assert(writes==40 && registers[97]==0x13);
    puts("PASS 5,898,240 mask values, viewport phase/exclusions, clipping, overflow, frozen slots, bank caching and legacy PL bypass");
}
'''
    tick = function(source, "compositor_tick")
    assert tick.index("pixel_mask_stage(s_writer_idx)") < tick.index('"dsb sy"') < tick.index("REG_WRITE(FB_BASE_ADDR_REG")
    assert "video_pixel_mask_clip_rect(&s_pixel_mask[s_writer_idx].viewport" in tick
    compiler = os.environ.get("CC") or shutil.which("gcc") or shutil.which("clang") or "E:/AMDDesignTools/2025.2/tps/mingw/10.0.0/win64.o/nt/bin/gcc.exe"
    env = dict(os.environ, PATH=str(Path(compiler).parent)+os.pathsep+os.environ.get("PATH", ""))
    with tempfile.TemporaryDirectory(prefix="mask-check-") as directory:
        path = Path(directory)
        (path / "mask.c").write_text(harness)
        subprocess.run([compiler,"-std=c11","-O2","-Wall","-Wextra","-Werror",
                        "-I"+str(ROOT / "ps_sources/frontend"),str(path / "mask.c"),
                        str(ROOT / "ps_sources/frontend/compositor_layout.c"),
                        "-o",str(path / "mask.exe")],check=True,env=env)
        subprocess.run([str(path / "mask.exe")],check=True,env=env)


if __name__ == "__main__":
    main()
