#!/usr/bin/env python3
"""Check video row copies, alignment, clipping, and exact ARM read bounds."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import random
import shutil
import struct
import subprocess
import sys

import test_video_smoothing as common

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build/fb16_video_copy_test'
SIZE = 8192

SOURCE = r'''
#include <stdint.h>
#include <string.h>
#include "fb16.h"
uint32_t params[8];
uint8_t input[8192] __attribute__((aligned(32)));
uint8_t output[8192] __attribute__((aligned(32)));
void run_case(void) {
    if (params[0] == 0) {
        fb16_copy_bgra32_row((uint32_t *)(output + params[2]),
                            (const uint32_t *)(input + params[1]), (int)params[3]);
    } else {
        fb16_set_size(700, 3);
        fb16_copy_row_attenuated((uint16_t *)(output + params[2]),
            (int)params[4], (int)params[5], (const uint16_t *)(input + params[1]),
            (int)params[3], params[6]);
    }
}
#if defined(__arm__)
void *memcpy(void *dst,const void *src,size_t n) {
    unsigned char*d=dst;const unsigned char*s=src;while(n--)*d++=*s++;return dst;
}
void *memset(void *dst,int value,size_t n) {
    unsigned char*d=dst;while(n--)*d++=(unsigned char)value;return dst;
}
#endif
'''


class Runner:
    def __init__(self, neon):
        self.neon = neon
        directory = BUILD / ('arm' if neon else 'native')
        directory.mkdir(parents=True, exist_ok=True)
        wrapper = directory / 'copy_test.c'
        wrapper.write_text(SOURCE)
        # Freeze linked production sources so a later edit cannot change evidence.
        for sub in ('lib', 'frontend'):
            (directory / sub).mkdir(exist_ok=True)
        for sub, filename in (('lib','fb16.c'), ('lib','fb16.h'), ('frontend','scanlines.h')):
            shutil.copy2(ROOT / 'ps_sources' / sub / filename, directory / sub / filename)
        self.hashes = {f'{sub}/{name}':hashlib.sha256((directory/sub/name).read_bytes()).hexdigest()
                       for sub,name in (('lib','fb16.c'),('lib','fb16.h'),('frontend','scanlines.h'))}
        args = ['-std=c11', '-funsigned-char', '-O2', '-Wall', '-Wextra', '-Werror',
                '-I'+str(directory/'lib'), str(wrapper), str(directory/'lib/fb16.c')]
        if not neon:
            cc = os.environ.get('CC') or shutil.which('gcc') or 'E:/AMDDesignTools/2025.2/tps/mingw/10.0.0/win64.o/nt/bin/gcc.exe'
            env = os.environ.copy(); env['PATH'] = str(Path(cc).parent)+os.pathsep+env['PATH']
            dll = directory / ('copy.dll' if os.name=='nt' else 'copy.so')
            subprocess.run([cc,*args,'-shared','-fPIC','-o',str(dll)],env=env,check=True)
            self.lib=ctypes.CDLL(str(dll));self.params=(ctypes.c_uint32*8).in_dll(self.lib,'params')
            self.input=(ctypes.c_uint8*SIZE).in_dll(self.lib,'input');self.output=(ctypes.c_uint8*SIZE).in_dll(self.lib,'output')
        else:
            cc,tool=common.tools();elf=directory/'copy.elf';binary=directory/'copy.bin'
            subprocess.run([str(cc),*args,'-mcpu=cortex-a9','-mfpu=neon','-mfloat-abi=hard','-marm',
                '-ffreestanding','-fno-builtin','-ffunction-sections','-fdata-sections','-nostdlib',
                '-Wl,--gc-sections,--build-id=none,-Ttext=0x10000,-e,run_case','-lgcc','-o',str(elf)],check=True)
            subprocess.run([str(tool('objcopy')),'-O','binary',str(elf),str(binary)],check=True)
            assembly=subprocess.check_output([str(tool('objdump')),'-d',str(elf)],text=True)
            (directory/'copy.asm').write_text(assembly)
            assert 'vld1.8\t{d0-d3}' in assembly and 'vst1.8\t{d0-d3}' in assembly, 'missing explicit 32-byte transfer'
            self.symbols={v[2]:int(v[0],16) for line in subprocess.check_output([str(tool('nm')),'-n',str(elf)],text=True).splitlines() if len(v:=line.split())==3}
            for dep in ('build/video_mono_neon_test/deps','build/video_smoothing_test/deps'):sys.path.insert(0,str(ROOT/dep))
            from unicorn import Uc,UC_ARCH_ARM,UC_MODE_ARM,UC_HOOK_MEM_READ,UC_HOOK_MEM_WRITE
            from unicorn.arm_const import UC_CPU_ARM_CORTEX_A9,UC_ARM_REG_C1_C0_2,UC_ARM_REG_FPEXC
            self.cpu=Uc(UC_ARCH_ARM,UC_MODE_ARM);self.cpu.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_A9)
            self.cpu.mem_map(0x10000,0x2000000);self.cpu.mem_write(0x10000,binary.read_bytes())
            self.cpu.reg_write(UC_ARM_REG_C1_C0_2,0xf<<20);self.cpu.reg_write(UC_ARM_REG_FPEXC,1<<30)
            self.reads=[];self.writes=[]
            def trace(cpu,access,address,size,value,data):
                if self.symbols['input'] <= address < self.symbols['input']+SIZE:
                    self.reads.append((address-self.symbols['input'],size))
                if self.symbols['output'] <= address < self.symbols['output']+SIZE:
                    self.writes.append((address-self.symbols['output'],size))
            self.cpu.hook_add(UC_HOOK_MEM_READ|UC_HOOK_MEM_WRITE,trace)
            self.cpu.ctl_flush_tb()

    def run(self, p, source, allowed_read, allowed_write, expected):
        if not self.neon:
            self.params[:] = [n&0xffffffff for n in p]
            self.input[:] = source;self.output[:] = [0xA5]*SIZE
            self.lib.run_case(); actual=bytes(self.output)
        else:
            from unicorn.arm_const import UC_ARM_REG_SP,UC_ARM_REG_LR,UC_ARM_REG_PC
            c=self.cpu;s=self.symbols
            c.mem_write(s['params'],struct.pack('<8I',*[n&0xffffffff for n in p]))
            c.mem_write(s['input'],source);c.mem_write(s['output'],bytes([0xA5])*SIZE)
            self.reads.clear();self.writes.clear()
            c.reg_write(UC_ARM_REG_SP,0x1f00000);c.reg_write(UC_ARM_REG_LR,0x1ff0000)
            c.emu_start(s['run_case'],0x1ff0000,count=1000000)
            assert c.reg_read(UC_ARM_REG_PC)==0x1ff0000
            actual=bytes(c.mem_read(s['output'],SIZE))
            for accesses,allowed in ((self.reads,allowed_read),(self.writes,allowed_write)):
                assert all(allowed[0]<=a and a+n<=allowed[1] for a,n in accesses),('out of row access',p,allowed,accesses[:10])
                assert sum(n for _,n in accesses)==allowed[1]-allowed[0],('bytes accessed more than once or missed',p,allowed)
        assert actual==expected,('output/guards',p,next((i,a,b) for i,(a,b) in enumerate(zip(actual,expected)) if a!=b))


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--neon',action='store_true');args=parser.parse_args()
    runner=Runner(args.neon);rng=random.Random(9463);source=bytes(rng.randrange(256) for _ in range(SIZE))
    widths=(-1,0,1,2,3,7,8,9,15,16,17,31,32,33,280,560,616,640)
    count=0
    for width in widths:
        for src_offset in range(0,32,4):
            for dst_offset in range(0,32,4):
                a,b=64+src_offset,64+dst_offset;n=max(0,width)*4
                expected=bytearray([0xA5])*SIZE;expected[b:b+n]=source[a:a+n]
                runner.run([0,a,b,width,0,0,0,0],source,(a,a+n),(b,b+n),expected);count+=1
    bgra=count
    for width in widths:
        for src_offset in range(0,32,2):
            for dst_offset in range(0,32,2):
                for x,y in ((0,1),(-5,1),(693,1),(700,1),(0,-1),(0,3)):
                    a,b=64+src_offset,64+dst_offset
                    skip=max(0,-x);n=max(0,min(width-skip,700-max(0,x))) if 0<=y<3 else 0
                    read=a+skip*2;write=b+(y*700+max(0,x))*2
                    expected=bytearray([0xA5])*SIZE
                    if n:expected[write:write+2*n]=source[read:read+2*n]
                    runner.run([1,a,b,width,x,y,4,0],source,(read,read+n*2),(write,write+n*2),expected);count+=1
    report={'backend':'ARM NEON' if args.neon else 'native','bgra_copy_cases':bgra,'clipped_rgb565_cases':count-bgra,
            'total_cases':count,'hashes':runner.hashes,'checks':'exact bytes and guards; all mod32 source/destination alignments; ARM source/write bounds and single access per byte'}
    (BUILD/('arm_result.json' if args.neon else 'native_result.json')).write_text(json.dumps(report,indent=2))
    print('PASS',json.dumps(report))


if __name__=='__main__':main()
