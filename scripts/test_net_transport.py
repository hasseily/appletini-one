#!/usr/bin/env python3
"""Run the ARM test source and both real 65C02 clients with the stock ROM bytes."""
import ctypes
import hashlib
import json
import os
from pathlib import Path
import random
import shutil
import subprocess
from py65.devices.mpu65c02 import MPU
from test_onee_input_service import find_native_c_compiler

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'build/net-transport-checkpoint'
SOURCE=ROOT/'software/net_transport_checkpoint'

def run(args):
    subprocess.run([str(x) for x in args],cwd=OUT,check=True,
                   stdout=subprocess.PIPE,stderr=subprocess.STDOUT)

def main():
    OUT.mkdir(parents=True,exist_ok=True)
    cc=find_native_c_compiler()
    if not cc: raise RuntimeError('Native C compiler required')
    env_path=str(cc.parent)
    if os.name=='nt': dll_path=os.add_dll_directory(env_path)
    lib=OUT/('net_test.dll' if os.name=='nt' else 'net_test.so')
    run([cc,'-std=c11','-Wall','-Wextra','-Werror','-O2','-shared',
         '-DAPPLETINI_NET_TRANSPORT_TEST=1',ROOT/'ps_sources/frontend/net_transport_test.c','-o',lib])
    core=ctypes.CDLL(str(lib))
    core.net_transport_test_reset.argtypes=[ctypes.c_uint32]
    core.net_transport_test_status.argtypes=[ctypes.c_uint8,ctypes.c_void_p,ctypes.c_void_p,ctypes.c_size_t]
    core.net_transport_test_status.restype=ctypes.c_uint16
    disabled=OUT/('net_default.dll' if os.name=='nt' else 'net_default.so')
    run([cc,'-std=c11','-Wall','-Wextra','-Werror','-O2','-shared',
         ROOT/'ps_sources/frontend/net_transport_test.c','-o',disabled])
    default_core=ctypes.CDLL(str(disabled))
    default_core.net_transport_test_status.argtypes=core.net_transport_test_status.argtypes
    default_core.net_transport_test_status.restype=ctypes.c_uint16
    default_out=(ctypes.c_ubyte*512)(*([0xCC]*512))
    assert default_core.net_transport_test_status(0x82,bytes([3,0,0,0,0x82,0,0,0,2]),default_out,512)==0
    assert bytes(default_out)==bytes([0xCC])*512
    core.net_transport_test_reset(0)
    def response(selector,offset=0,capacity=512,unit=0,count=3):
        params=bytes([count,unit,0,0,selector])+offset.to_bytes(2,'little')+capacity.to_bytes(2,'little')
        guarded=(ctypes.c_ubyte*514)(*([0xCC]*514))
        length=core.net_transport_test_status(selector,params,guarded,512)
        assert bytes(guarded)[length:]==bytes([0xCC])*(514-length)
        return bytes(guarded[:length])
    fixtures=json.loads((ROOT/'scripts/fixtures/net_transport_test.json').read_text())
    for fixture in fixtures['vectors']:
        assert response(fixture['selector'],fixture['offset'],fixture['capacity']).hex()==fixture['reply_hex'],fixture['name']
    assert not response(0x83,0,8) and not response(0x83,0,513)
    assert not response(0x83,0,512,unit=1) and not response(0x83,0,512,count=2)
    rng=random.Random(0x41544E54)
    for _ in range(10000):
        at=rng.randrange(65536); cap=rng.randrange(9,513)
        actual=response(0x83,at,cap)
        n=min(cap-8,max(0,32768-at))
        expected=bytes((((pos*73+(pos//256)*19+90)^(pos//16))&255) for pos in range(at,at+n))
        assert len(actual)==n+8 and actual[8:]==expected
        assert response(0x83,at,cap)==actual
    for tool in ('ca65','ld65'):
        if not shutil.which(tool):raise RuntimeError(tool+' required')
    run(['ca65',SOURCE/'transport.s','-o',OUT/'transport.o'])
    run(['ca65',ROOT/'scripts/fixtures/net_transport_client_globals.s','-o',OUT/'client_globals.o'])
    run(['ld65','-C',SOURCE/'model.cfg','-o',OUT/'client-model.bin','-Ln',OUT/'client-model.lbl',
         OUT/'transport.o',OUT/'client_globals.o'])
    labels={line.split()[2].lstrip('.'):int(line.split()[1],16)
            for line in (OUT/'client-model.lbl').read_text().splitlines()}
    class Memory:
        def __init__(self):
            self.data=bytearray(65536)
            for start,name in ((0xC700,'c700'),(0xC800,'c800')):
                rom=bytes(int(x,16) for x in (ROOT/f'hdl/apple/smartport_a2retronet_style_{name}.mem').read_text().split())
                self.data[start:start+len(rom)]=rom
            image=(OUT/'client-model.bin').read_bytes();self.data[0x800:0x800+len(image)]=image
            self.fifo=bytearray();self.input=bytearray();self.ready=False
            self.cpu=None;self.writes=0;self.timeout=False;self.bad_length=False;self.internal=0x80
            self.t1_cleared=0;self.ready_at=0;self.vbl_divisor=100
        def __getitem__(self,at):
            if at==0xCFF0:return self.fifo[0] if self.fifo else 0
            if at==0xCFF1:return 0x80 if self.ready and self.cpu.processorCycles>=self.ready_at else 0
            if at==0xC019:return ((self.cpu.processorCycles//self.vbl_divisor)&1)<<7
            if at==0xC015:return self.internal
            if at==0xC48D:return 0x40 if self.cpu.processorCycles//65536>self.t1_cleared//65536 else 0
            if at in (0xC484,0xC485):
                ticks=(0xFFFF-self.cpu.processorCycles)&65535
                if at==0xC484:self.t1_cleared=self.cpu.processorCycles
                return (ticks>>(8 if at==0xC485 else 0))&255
            return self.data[at]
        def __setitem__(self,at,value):
            value&=255
            if at==0xCFF0:self.input.append(value);self.writes+=1
            elif at==0xCFF2:
                if self.fifo:self.fifo.pop(0)
            elif at==0xC006:self.internal=0
            elif at==0xC007:self.internal=0x80
            elif at==0xCFF1:
                assert value==2 and len(self.input)==10,(value,self.input.hex())
                frame=bytes(self.input);self.input.clear()
                payload=response(frame[5],int.from_bytes(frame[6:8],'little'),int.from_bytes(frame[8:10],'little'))
                self.fifo=bytearray(b'\0'+(len(payload)+(1 if self.bad_length else 0)).to_bytes(2,'little')+payload)
                self.ready=not self.timeout
            else:self.data[at]=value
    def execute(memory,entry):
        cpu=MPU(memory=memory,pc=labels[entry]);memory.cpu=cpu
        cpu.sp=0xFD;cpu.p=0x20
        memory.data[0x1FE:0x200]=(0x5FF).to_bytes(2,'little')
        for _ in range(200000):
            if cpu.pc==0x600:return cpu
            cpu.step()
        raise AssertionError(('client timeout',hex(cpu.pc)))
    def call(memory,mode,selector,at,cap):
        def set16(name,n): memory.data[labels[name]:labels[name]+2]=n.to_bytes(2,'little')
        memory.data[labels['_nt_mode']]=mode;memory.data[labels['_nt_selector']]=selector
        set16('_nt_offset',at);set16('_nt_capacity',cap)
        base=labels['_nt_buffer'];memory.data[base:base+514]=b'\xCC'*514
        memory.ready=False
        memory.t1_cleared=0
        cpu=execute(memory,'_nt_call')
        assert cpu.sp==0xFF and not (cpu.p&4) and memory.internal==0x80
        rc=memory.data[labels['_nt_rc']]
        n=int.from_bytes(memory.data[labels['_nt_length']:labels['_nt_length']+2],'little')
        assert memory.data[base+n:base+514]==b'\xCC'*(514-n)
        if not rc and selector==0x83:
            assert execute(memory,'_nt_check_data').a==1
            for bad_at in ([base+8] if n>8 else [])+[base+513]:
                memory.data[bad_at]^=1
                assert execute(memory,'_nt_check_data').a==0
                memory.data[bad_at]^=1
        return rc,bytes(memory.data[base:base+n]),cpu.processorCycles
    cycles={}
    for mode in (0,1):
        memory=Memory()
        for cap in range(9,513):
            at=(cap*73)&32767
            rc,payload,ticks=call(memory,mode,0x83,at,cap)
            assert rc==0 and payload==response(0x83,at,cap),(mode,cap,rc,payload[:12].hex())
            assert not memory.fifo and not memory.input
            cycles[str(mode)+':'+str(cap)]=ticks
            assert call(memory,mode,0x83,at,cap)[1]==payload
        assert call(memory,mode,0x82,0,64)[1][:4]==b'ATNT'
        # Model an Apple reset abandoning a partial FIFO read, then a new program.
        memory.fifo=bytearray(b'old reply');memory.ready=True
        core.net_transport_test_reset(7)
        memory=Memory()
        rc,payload,_=call(memory,mode,0x82,0,64)
        assert rc==0 and payload[24:28]==(7).to_bytes(4,'little') and payload[28:32]==bytes(4)
    memory=Memory();memory.data[labels['_nt_reset_partial']]=1
    try:call(memory,1,0x83,0,512)
    except AssertionError as exc:
        assert 'client timeout' in str(exc)
        assert memory.data[labels['_nt_reset_stage']]==1
        assert len(memory.fifo)==256 and memory.cpu.p&4
    else:raise AssertionError('Partial-read reset fixture did not pause')
    core.net_transport_test_reset(8)
    fresh=Memory();rc,payload,_=call(fresh,1,0x82,0,64)
    assert rc==0 and payload[24:28]==(8).to_bytes(4,'little') and payload[28:32]==bytes(4)
    memory=Memory();memory.timeout=True
    assert call(memory,1,0x83,0,512)[0]==0x67
    writes=memory.writes
    assert call(memory,1,0x83,0,512)[0]==0x67 and memory.writes==writes
    memory=Memory();memory.bad_length=True
    assert call(memory,1,0x83,0,512)[0]==0x61
    memory=Memory();memory.ready_at=70000;memory.vbl_divisor=1000
    assert call(memory,1,0x83,0,512)[0]==0
    assert memory.data[labels['_nt_timer_overflow']]&0x40
    previous=response(0x82)[7]
    for sequence in range(256):
        core.net_transport_test_reset(sequence)
        desc=response(0x82)
        assert desc[7] and desc[7]!=previous and desc[28:32]==bytes(4)
        previous=desc[7]
    report={'test_class':'native ARM core plus real 65C02 ROM/raw client model; no board timing',
            'native_random_reads':20000,'successful_client_reads':2020,'failure_calls':3,'all_reply_sizes':[9,512],
            'default_test_source_disabled':True,'generation_wrap_skips_zero':True,'timer_wrap_detected':True,
            'client_cycles_without_ARM_wait':cycles,'pass':True}
    (OUT/'host-test-report.json').write_text(json.dumps(report,indent=2)+'\n')
    print('PASS native pattern/limits/repeat and stock-ROM + raw 65C02 paths, 9..512 bytes, reset/timeout')
if __name__=='__main__':main()
