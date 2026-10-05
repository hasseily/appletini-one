#!/usr/bin/env python3
"""Verify restored F1.2.2 Blur/Dot behavior, retained glow, and ARM/scalar parity."""
import argparse, ctypes, os, random, shutil, struct, subprocess, sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
BUILD=ROOT/'build/video_filter_rollback_test'
BASELINE=False
MAX_W,MAX_H=640,400

def source():
    text=(subprocess.check_output(['git','show','3101934:ps_sources/frontend/compositor.c'],text=True)
          if BASELINE else (ROOT/'ps_sources/frontend/compositor.c').read_text())
    effects=text[text.index('#define EFFECT_HISTORY_STRIDE'):text.index('/* ---------- Format badge')]
    return r'''#include <stdint.h>
#include <string.h>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif
#include "compositor_layout.h"
#include "video_mono.h"
#include "video_blur.h"
#include "video_glow.h"
#include "video_ghosting.h"
#include "scanlines.h"
static int smartport_service_has_pending(void){return 0;}
static void smartport_service_poll(void){}
static uint8_t s_video_dot_bleed;
''' + effects + r'''
uint32_t params[12];
uint32_t input[640*400];
uint16_t output[(640*2+2)*(400*4+2)];
void run_case(void) {
    int w=params[0],h=params[1],sx=params[2],sy=params[3];
    fb16_set_size(w*sx+2,h*sy+2);
    memset(output,0xa5,(size_t)(w*sx+2)*(h*sy+2)*2);
    if(params[9])effect_clear_history();
    s_video_dot_bleed=params[7];
    s_mono_x=s_mono_y=0;
    s_mono_width=params[7]?w:0;s_mono_height=h;
    s_mono_channel_shift=video_mono_channel_shift(params[10]?params[10]:1);
    video_mono_build_tint(s_mono_tint,params[10]?params[10]:1);
    if(params[5]||params[6]||params[7]||params[8])
        blit_apple_effects_scaled(output,1,1,input,w,h,w,sx,sy,params[4],params[5],params[6],params[8]);
    else blit_apple_scaled_serviced(output,1,1,input,w,h,w,sx,sy,params[4]);
}
uint32_t history_at(unsigned x,unsigned y){return s_effect_history[y*EFFECT_HISTORY_STRIDE+x];}
uint32_t decay_numer(uint32_t pixel,unsigned strength){return effect_decay_numer(pixel,strength);}
void direct_pack(void){fb16_pack_row_bgra32src(output,input,params[0]);}
void direct_blit(void){
    int w=params[0],h=params[1],sx=params[2],sy=params[3];
    fb16_set_size(w*sx+2,h*sy+2);
    memset(output,0xa5,(size_t)(w*sx+2)*(h*sy+2)*2);
    fb16_blit_scaled_scanlines(output,1,1,input,w,h,w,sx,sy,params[4]);
}
'''

def tools():
    compiler=os.environ.get('ARM_CC') or shutil.which('arm-none-eabi-gcc')
    if not compiler:compiler='E:/AMDDesignTools/2025.2/Vitis/gnu/aarch32/nt/gcc-arm-none-eabi/bin/arm-none-eabi-gcc.exe'
    c=Path(compiler);prefix=c.name.removesuffix(c.suffix).removesuffix('gcc')
    return c,lambda n:c.with_name(prefix+n+c.suffix)

def build(neon):
    BUILD.mkdir(parents=True,exist_ok=True)
    wrapper=BUILD/('neon.c' if neon else 'scalar.c')
    body=source()
    if neon:body+=r'''
void *memcpy(void *dst,const void *src,size_t n){unsigned char*d=dst;const unsigned char*s=src;while(n--)*d++=*s++;return dst;}
void *memset(void *dst,int value,size_t n){unsigned char*d=dst;while(n--)*d++=(unsigned char)value;return dst;}
'''
    wrapper.write_text(body)
    common=['-std=c11','-funsigned-char','-O2','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable',
        '-I'+str(ROOT/'ps_sources/frontend'),'-I'+str(ROOT/'ps_sources/lib'),str(wrapper),str(ROOT/'ps_sources/lib/fb16.c')]
    if neon:
        cc,tool=tools();elf=BUILD/'neon.elf'
        subprocess.run([str(cc),*common,'-mcpu=cortex-a9','-mfpu=neon','-mfloat-abi=hard','-marm','-ffreestanding','-fno-builtin',
            '-ffunction-sections','-fdata-sections','-fno-unwind-tables','-fno-asynchronous-unwind-tables','-nostdlib',
            '-Wl,--gc-sections,--build-id=none,-Ttext=0x10000,-e,run_case','-Wl,-u,decay_numer,-u,direct_blit,-u,direct_pack','-lgcc','-o',str(elf)],check=True)
        binary=BUILD/'neon.bin';subprocess.run([str(tool('objcopy')),'-O','binary',str(elf),str(binary)],check=True)
        symbols={f[2]:int(f[0],16) for line in subprocess.check_output([str(tool('nm')),'-n',str(elf)],text=True).splitlines() if len(f:=line.split())==3}
        assembly=subprocess.check_output([str(tool('objdump')),'-d',str(elf)],text=True)
        (BUILD/'neon.asm').write_text(assembly)
        for instruction in ('vld1','vqadd.u8','vhadd.u8'):assert instruction in assembly,instruction
        return binary.read_bytes(),symbols
    cc=os.environ.get('CC') or shutil.which('gcc')
    if not cc:
        cc=next((p for p in ('C:/msys64/ucrt64/bin/gcc.exe','E:/AMDDesignTools/2025.2/tps/mingw/10.0.0/win64.o/nt/bin/gcc.exe') if Path(p).exists()),None)
    if not cc:raise RuntimeError('Set CC to a native GCC compiler')
    dll=BUILD/('scalar.dll' if os.name=='nt' else 'scalar.so')
    env=os.environ.copy();env['PATH']=str(Path(cc).resolve().parent)+os.pathsep+env['PATH']
    subprocess.run([cc,*common,'-shared','-fPIC','-o',str(dll)],env=env,check=True)
    return ctypes.CDLL(str(dll))

class Runner:
    def __init__(self,neon):
        self.neon=neon
        if not neon:
            self.lib=build(False);self.params=(ctypes.c_uint32*12).in_dll(self.lib,'params');self.input=(ctypes.c_uint32*(640*400)).in_dll(self.lib,'input');self.output=(ctypes.c_uint16*((1282)*(1602))).in_dll(self.lib,'output')
            return
        for p in (ROOT/'build/video_mono_neon_test/deps',ROOT/'build/video_smoothing_test/deps'):sys.path.insert(0,str(p))
        from unicorn import Uc,UC_ARCH_ARM,UC_MODE_ARM
        from unicorn.arm_const import UC_CPU_ARM_CORTEX_A9,UC_ARM_REG_C1_C0_2,UC_ARM_REG_FPEXC
        binary,self.symbols=build(True);self.cpu=Uc(UC_ARCH_ARM,UC_MODE_ARM);self.cpu.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_A9)
        self.cpu.mem_map(0x10000,0x2000000);self.cpu.mem_write(0x10000,binary)
        self.cpu.reg_write(UC_ARM_REG_C1_C0_2,0xf<<20);self.cpu.reg_write(UC_ARM_REG_FPEXC,1<<30)
    def run(self,pixels,w,h,sx,sy,scan,ghost,horizontal,vertical,glow,reset=1,entry='run_case',tv=0):
        params=[w,h,sx,sy,scan,ghost,horizontal,vertical,glow,reset,tv,0]
        size=(w*sx+2)*(h*sy+2)
        if not self.neon:
            self.params[:]=params;self.input[:len(pixels)]=pixels;getattr(self.lib,entry)();return list(self.output[:size])
        from unicorn.arm_const import UC_ARM_REG_SP,UC_ARM_REG_LR,UC_ARM_REG_PC
        c=self.cpu;s=self.symbols;c.mem_write(s['params'],struct.pack('<12I',*params));c.mem_write(s['input'],struct.pack('<%dI'%len(pixels),*pixels))
        c.reg_write(UC_ARM_REG_SP,0x1f00000);c.reg_write(UC_ARM_REG_LR,0x1ff0000);c.emu_start(s[entry],0x1ff0000,count=1000000000)
        assert c.reg_read(UC_ARM_REG_PC)==0x1ff0000,'instruction budget exceeded'
        return list(struct.unpack('<%dH'%size,c.mem_read(s['output'],size*2)))

def rgb(p): return [(p>>(8*i))&255 for i in range(3)]
def pack(p): return ((p[2]&248)<<8)|((p[1]&252)<<3)|(p[0]>>3)
def reference(pixels,w,h,blur,glow,sx,sy,scan):
    raw=list(map(rgb,pixels))
    def at(x,y): return raw[max(0,min(h-1,y))*w+max(0,min(w-1,x))]
    horizontal=[]
    for y in range(h):
        for x in range(w):
            if not blur or w<4: q=at(x,y)
            elif blur==3:
                q=[(at(x-2,y)[c]>>3)+(at(x-1,y)[c]>>2)+(at(x,y)[c]>>2)+(at(x+1,y)[c]>>2)+(at(x+2,y)[c]>>3) for c in range(3)]
            else:q=[(at(x-1,y)[c]>>2)+(at(x,y)[c]>>1)+(at(x+1,y)[c]>>2) for c in range(3)]
            horizontal.append(q)
    def ht(x,y): return horizontal[max(0,min(h-1,y))*w+x]
    def bound(x,y,dx,dy):
        lo,hi=0,w-1; first,last=0,h-1
        if w==616 and h==224:
            lo,hi=(0,27) if x<28 else (28,587) if x<588 else (588,615)
            first,last=(0,15) if y<16 else (16,207) if y<208 else (208,223)
        return max(first,min(last,y+dy))*w+max(lo,min(hi,x+dx))
    halo_h=[[sum(raw[bound(x,y,dx,0)][c]*k for dx,k in ((-1,1),(0,2),(1,1)))//4 for c in range(3)] for y in range(h) for x in range(w)] if glow else None
    out=[0xa5a5]*((w*sx+2)*(h*sy+2));stride=w*sx+2
    for y in range(h):
        row=[]
        for x in range(w):
            q=ht(x,y) if blur<2 else [(ht(x,y-1)[c]>>2)+(ht(x,y)[c]>>1)+(ht(x,y+1)[c]>>2) for c in range(3)]
            if glow:
                halo=[sum(halo_h[bound(x,y,0,dy)][c]*k for dy,k in ((-1,1),(0,2),(1,1)))//4 for c in range(3)]
                q=[min(255,q[c]+(halo[c]>>(4-glow))) for c in range(3)]
            row.extend([pack(q)]*sx)
        for phase in range(sy):
            keep=4 if not scan or sy==1 else (0 if phase>=4-scan else 4) if sy==4 else 4-scan if phase==1 else 4
            shaded=[((p>>11)*keep//4<<11)|(((p>>5)&63)*keep//4<<5)|((p&31)*keep//4) for p in row]
            offset=(1+y*sy+phase)*stride+1;out[offset:offset+w*sx]=shaded
    return out

def main():
    global BUILD,BASELINE
    ap=argparse.ArgumentParser();ap.add_argument('--neon',action='store_true');args=ap.parse_args()
    base=BUILD;BUILD=base/'baseline';BASELINE=True;baseline=Runner(False)
    BUILD=base/('arm' if args.neon else 'native');BASELINE=False;current=Runner(args.neon)
    rng=random.Random(124); count=0
    # Compare executable 1.2.2 source, not a reimplementation, including mono tints/tails and native1x.
    for w,h in ((1,1),(2,3),(3,2),(4,3),(7,3),(8,3),(9,3),(17,5),(560,3),(616,3),(640,3)):
        pixels=[0xff000000|rng.randrange(1<<24) for _ in range(w*h)]
        for blur in range(4):
            for dot in range(4):
                for sx,sy in ((1,1),(1,2),(2,2),(2,4)):
                    color=(blur+dot)%3+1
                    a=current.run(pixels,w,h,sx,sy,0,0,blur,dot,0,tv=color)
                    b=baseline.run(pixels,w,h,sx,sy,0,0,blur,dot,0,tv=color)
                    assert a==b,('1.2.2 parity',w,h,blur,dot,sx,sy)
                    count+=1
    print(f'PASS {count} exact F1.2.2 Blur/Dot comparisons')
    oracle_count=0
    for w,h in ((1,1),(3,3),(9,5),(17,3),(640,3)):
        pixels=[rng.randrange(1<<32) for _ in range(w*h)]
        for blur in range(4):
            for glow in range(4):
                for sx,sy in ((1,1),(1,2),(2,2),(2,4)):
                    scan=(blur+glow)%4
                    got=current.run(pixels,w,h,sx,sy,scan,0,blur,0,glow)
                    expected=reference(pixels,w,h,blur,glow,sx,sy,scan)
                    assert got==expected,('oracle',w,h,blur,glow,sx,sy,next((i,a,b) for i,(a,b) in enumerate(zip(got,expected)) if a!=b))
                    oracle_count+=1
    pixels=[0xff000000|rng.randrange(1<<24) for _ in range(616*224)]
    got=current.run(pixels,616,224,1,2,2,0,3,0,3)
    assert got==reference(pixels,616,224,3,3,1,2,2),'captured border/glow isolation'
    print(f'PASS {oracle_count+1} blur/glow/scanline/tail oracle cases')
    for level in (1,2,3):
        for value in range(256):
            seed=[0xff000000|value*(1<<c) for c in (0,8,16)]*3
            current.run(seed,9,1,1,1,0,level,0,0,0)
            n=0 if value<4 else (14,18,24)[level-1] if value>=128 else (40,48,56)[level-1] if value>=32 else (58,60,61)[level-1]
            faded=[0xff000000|((value*n//64)*(1<<c)) for c in (0,8,16)]*3
            got=current.run([0xff000000]*9,9,1,1,1,0,level,0,0,0,reset=0)
            assert got==reference(faded,9,1,0,0,1,1,0),('ghost knee',level,value)
    print('PASS 768 ghosting threshold/decay cases; '+('Cortex-A9 NEON' if args.neon else 'native scalar'))

if __name__=='__main__':main()
