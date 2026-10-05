#!/usr/bin/env python3
"""Check F1.2.3 axis migration into the restored F1.2.2 filter controls."""
import os, shutil, subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]

def main():
    out=ROOT/'build/video_filter_config';out.mkdir(parents=True,exist_ok=True)
    cc=shutil.which('gcc') or 'E:/AMDDesignTools/2025.2/tps/mingw/10.0.0/win64.o/nt/bin/gcc.exe'
    env=dict(os.environ,PATH=str(Path(cc).parent)+os.pathsep+os.environ.get('PATH',''))
    source=out/'test.c';source.write_text(r'''
#include <assert.h>
#include <stdio.h>
#include "video_filter_config.h"
int main(void) {
    unsigned cases=0;
    for(unsigned mono=0;mono<2;++mono)
    for(unsigned h=0;h<4;++h) for(unsigned v=0;v<4;++v)
    for(unsigned axes=0;axes<4;++axes) for(unsigned explicit_keys=0;explicit_keys<4;++explicit_keys)
    for(unsigned old_blur=0;old_blur<4;++old_blur) for(unsigned old_dot=0;old_dot<4;++old_dot) {
        uint8_t blur=old_blur,dot=old_dot;
        video_filter_migrate(&blur,&dot,explicit_keys,axes,h,v,mono);
        unsigned expected_blur=old_blur,expected_dot=old_dot;
        if(axes && !(explicit_keys&1)) {
            expected_blur=!mono && h ? (h==3?3:1):0;
            if(v && expected_blur<2)expected_blur=2;
        }
        if(axes && !(explicit_keys&2))expected_dot=mono?h:0;
        assert(blur==expected_blur && dot==expected_dot);
        ++cases;
    }
    uint8_t blur=255,dot=255;
    video_filter_migrate(&blur,&dot,3,3,255,255,0);
    assert(blur==3 && dot==3);
    printf("PASS: %u original/axis migration combinations, explicit Off and bounds\n",cases);
}
''')
    exe=out/'test.exe'
    subprocess.run([cc,'-std=c11','-O2','-Wall','-Wextra','-Werror','-I'+str(ROOT/'ps_sources/frontend'),str(source),'-o',str(exe)],env=env,check=True)
    subprocess.run([str(exe)],env=env,check=True)
    menu=(ROOT/'ps_sources/frontend/config_menu.c').read_text()
    assert menu.count('config_menu_resolve_video_blending(menu);')==2
    assert menu.count('menu->video_filter_explicit = 0U;')==2
    for key in ('blur','dot.bleed'):assert f'"video.{key}=%s\\n"' in menu
    for axis in ('horizontal','vertical'):
        for prefix in ('blending','smoothing'):
            assert f'strcmp(key, "video.{prefix}.{axis}") == 0' in menu
            assert f'"video.{prefix}.{axis}=%s\\n"' not in menu
    print('PASS: both loader paths migrate; saves use original filter keys')
if __name__=='__main__':main()
