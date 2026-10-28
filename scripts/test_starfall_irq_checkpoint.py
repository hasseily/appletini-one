"""Exercise the real 65C02 probe and stock ROM; check IRQ ACK ownership."""
import json
from pathlib import Path
import sys
from py65.devices.mpu65c02 import MPU
from build_starfall_irq_checkpoint import ROOT,OUT,SOURCE,main as build


def main():
    build()
    report=json.loads((OUT/'client-build.json').read_text());labels=report['probe_labels']
    class Memory:
        def __init__(self):
            self.data=bytearray(65536);self.cpu=None;self.packet=bytearray();self.fifo=bytearray()
            self.internal=0x80;self.pending=True;self.audio_acks=0;self.a_low_reads=0
            program=(SOURCE/'inputs/STARFALL.SYSTEM').read_bytes();self.data[0x2000:0x2000+len(program)]=program
            for start,name in ((0xC700,'c700'),(0xC800,'c800')):
                rom=bytes(int(x,16) for x in (ROOT/f'hdl/apple/smartport_a2retronet_style_{name}.mem').read_text().split())
                self.data[start:start+len(rom)]=rom
            code=(OUT/'probe.bin').read_bytes();self.data[0xA280:0xA280+len(code)]=code
            self.data[0xA900:0xAA00]=bytes(256)
        def __getitem__(self,at):
            if at==0xCFF0:return self.fifo[0] if self.fifo else 0
            if at==0xCFF1:return 0x80
            if at==0xC015:return self.internal
            if at==0xC419:return ((-self.cpu.processorCycles)&65535)>>8
            if at==0xC418:self.a_low_reads+=1
            if at==0xC48D:return 0x40 if self.pending else 0
            if at==0xC484:self.audio_acks+=1;self.pending=False;return 0
            return self.data[at]
        def __setitem__(self,at,value):
            value&=255
            if at==0xCFF0:self.packet.append(value)
            elif at==0xCFF2:
                if self.fifo:self.fifo.pop(0)
            elif at==0xC006:self.internal=0
            elif at==0xC007:self.internal=0x80
            elif at==0xCFF1:
                assert value==2 and len(self.packet)==10,self.packet.hex()
                cap=int.from_bytes(self.packet[8:10],'little')
                payload=bytes([4,1,0,0,0,0])+int(cap-8).to_bytes(2,'little')+bytes(
                    ((p*73+(p>>8)*19+90)^(p>>4))&255 for p in range(cap-8))
                if self.packet[5]==0x82:
                    payload=b'ATTC'+self.cpu.processorCycles.to_bytes(8,'little')+(1000000).to_bytes(4,'little')+bytes(16)
                self.fifo=bytearray(b'\0'+len(payload).to_bytes(2,'little')+payload);self.packet.clear()
            else:self.data[at]=value
    def execute(memory,entry,cycles=0,stop=0x600):
        cpu=MPU(memory=memory,pc=entry);memory.cpu=cpu;cpu.processorCycles=cycles
        cpu.sp=0xFD;cpu.p=0x20;memory.data[0x1FE:0x200]=(0x5FF).to_bytes(2,'little')
        for _ in range(150000):
            if cpu.pc==stop:return cpu
            cpu.step()
        raise AssertionError(hex(cpu.pc))
    for phase,size in ((0x11,128),(0x12,511),(0x13,512),(0x14,512),(0x15,512)):
        memory=Memory();memory.data[labels['phase']]=phase
        memory.data[labels['cap']:labels['cap']+2]=size.to_bytes(2,'little')
        memory.data[labels['selector']]=0x83
        execute(memory,labels['transport'])
        assert not memory.audio_acks and not memory.a_low_reads
        assert memory.internal==0x80 and not memory.fifo and not memory.packet
        assert memory.data[labels['rc']]==0 and memory.data[0xA909]==0
        if phase!=0x15:
            assert int.from_bytes(memory.data[labels['length']:labels['length']+2],'little')==size
            expected=bytes(((p*73+(p>>8)*19+90)^(p>>4))&255 for p in range(size-8))
            assert memory.data[0xAA08:0xAA00+size]==expected
    memory=Memory();memory.data[0xA902]=1;memory.data[labels['phase']]=0x10
    memory.data[report['game']['GAME_CLOCK_RUNNING']]=1
    address=report['game']['GAME_AUDIO_COUNTER']
    memory.data[address:address+4]=bytes(4)
    for index in range(1,11):
        memory.pending=True
        execute(memory,labels['probe_irq'],index*10000)
    assert memory.audio_acks==10 and not memory.a_low_reads
    assert int.from_bytes(memory.data[address:address+4],'little')==10
    assert int.from_bytes(memory.data[0xA910:0xA912],'little')==10
    memory=Memory();memory.data[0xA902]=1;memory.data[labels['phase']]=0x13
    memory.data[labels['command']]=4
    original=bytes((i*13)&255 for i in range(514))
    memory.data[0xAA00:0xAC02]=original
    # Stop after the replayed C prologue; only the instrumentation is under test.
    memory.data[report['game']['GAME_AUDIO_PROLOGUE']]=0x60
    execute(memory,labels['probe_foreground'],10000,report['game']['GAME_UPDATE']+3)
    assert memory.data[labels['phase']]==0 and memory.data[labels['ack']]==1
    assert memory.data[0xAA00:0xAC02]==original
    assert not memory.audio_acks and not memory.a_low_reads
    memory=Memory();memory.data[0xA902]=1
    memory.data[labels['command']]=5
    memory.data[labels['cal_wait_limit']:labels['cal_wait_limit']+2]=(256).to_bytes(2,'little')
    memory.data[report['game']['GAME_AUDIO_PROLOGUE']]=0x60
    execute(memory,labels['probe_foreground'],10000,report['game']['GAME_UPDATE']+3)
    assert memory.data[labels['ack']]==1
    packets=memory.data[labels['stamp']:labels['stamp']+32]
    assert packets[:4]==b'ATTC' and packets[16:20]==b'ATTC'
    first=int.from_bytes(packets[4:12],'little');last=int.from_bytes(packets[20:28],'little')
    assert last-first>=65536
    (OUT/'probe-host-test.json').write_text(json.dumps({'pass':True,'raw_sizes':[128,511,512],
        'stock_rom_512':True,'positive_control':True,'irq_ack_owned_by_original_driver':True,
        'via_a_low_reads':0,'actual_driver_ticks':10,'frozen_payload_unchanged':True,
        'tight_calibration_32bit_extension':True},indent=2)+'\n')
    print('PASS real probe: copies, guards, C8 restore and original audio IRQ acknowledgement')


if __name__=='__main__':main()
