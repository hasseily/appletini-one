"""Calibrate the VIA and measure real STARFALL audio with bounded reads."""
import ftplib
import hashlib
import json
from pathlib import Path
import re
import struct
import sys
import time
import serial
from smartport_timing_board import serial_command,get,put,STOCK_HASH

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'build/starfall-irq-checkpoint'
sha=lambda data:hashlib.sha256(data).hexdigest()


def main():
    sys.stdout.reconfigure(encoding='utf-8',errors='replace')
    build=json.loads((OUT/'client-build.json').read_text());labels=build['probe_labels']
    stock=(ROOT/'firmwares/firmware-F1.3.3b3/FIRMWARE.BIN').read_bytes()
    image=(OUT/'FIRMWARE.BIN').read_bytes()
    assert sha(stock)==STOCK_HASH
    assert not (OUT/'original-settings.bin').exists()
    report={'firmware_sha256':sha(image),'stock_sha256':STOCK_HASH,'fpga_rebuilt':False,
            'golden_written':False,'cases':[]}
    log=[];original=None;owned=False;updated=False
    with serial.Serial('COM13',921600,timeout=.003) as uart:
        def command(text,timeout=.3,fast=False):
            if not fast:
                value=serial_command(uart,text,timeout=timeout)
            else:
                uart.reset_input_buffer();wire=(':'+text+'\r\n').encode()
                for at in range(0,len(wire),8):
                    uart.write(wire[at:at+8]);uart.flush();time.sleep(.01)
                data=bytearray();end=time.monotonic()+2
                while time.monotonic()<end:
                    data.extend(uart.read(max(1,uart.in_waiting)))
                    if b'\nok' in data:break
                else:raise RuntimeError('No write acknowledgement: '+data.decode(errors='replace'))
                value=data.decode(errors='replace')
            log.append(value)
            (OUT/'uart.txt').write_text(''.join(log),encoding='utf-8')
            return value
        def read(address):
            value=command(f'mrd 0x{address:08X}')
            found=re.search(rf'0x{address:08X}:\s*0x([0-9A-F]{{8}})',value,re.I)
            assert found,value
            return int(found[1],16)
        def dump(address,size):
            value=command(f'vtw dump {address:X} {size:X}',.4);data={}
            for match in re.finditer(r'(?m)^([0-9A-F]{5}):((?: [0-9A-F]{2})+)',value.replace('\r','')):
                for n,item in enumerate(match[2].split()):data[int(match[1],16)+n]=int(item,16)
            assert all(address+i in data for i in range(size)),value
            return bytes(data[address+i] for i in range(size))
        def write(address,data):
            command(f'mwr 0x400001C4 0x{address:X}',fast=True)
            for at,value in enumerate(data):
                command(f'mwr 0x400001C8 0x{value:X}',fast=True)
                if len(data)>256 and at%256==255:print(f'Probe upload: {at+1}/{len(data)} bytes',flush=True)
        def key(value):command(f'mwr 0x40000170 0x{ord(value):02X}',fast=True)
        def transfer(operation):
            command('ftp on',1)
            try:
                with ftplib.FTP() as ftp:
                    ftp.connect('10.17.1.18',timeout=60);ftp.login('anonymous','appletini-checkpoint')
                    return operation(ftp)
            finally:command('ftp off',1)
        def boot():
            command('reboot',15)
            for attempt in range(30):
                time.sleep(3)
                value=command('status',1)
                if '---- card status ----' in value:
                    time.sleep(10);return
                if attempt%5==0:print('Waiting for update/boot readiness.',flush=True)
            raise RuntimeError('Board boot deadline')
        def probe_command(value):
            previous=dump(labels['ack'],1)[0]
            write(labels['command'],bytes([value]))
            for _ in range(100):
                time.sleep(.1)
                if dump(labels['ack'],1)[0]!=(previous&255):return
            raise RuntimeError('STARFALL probe command deadline')
        def stamp():
            probe_command(3)
            raw=dump(labels['cal_before'],40)
            before,after=struct.unpack_from('<II',raw)
            assert raw[8:12]==b'ATTC',raw.hex()
            ticks,hz=struct.unpack_from('<QI',raw,12)
            assert 100000000<hz<400000000
            return {'before_units':before,'after_units':after,'arm_ticks':ticks,'arm_hz':hz}
        try:
            report['before']=command('status',1)+command('vtw status',1)
            def stage(ftp):
                nonlocal original,owned,updated
                assert sha(get(ftp,'/FIRMWARE.OK'))==STOCK_HASH
                original=get(ftp,'/appletini_cfg.txt');(OUT/'original-settings.bin').write_bytes(original)
                ftp.mkd('/IRQCHECK');owned=True
                put(ftp,'/IRQCHECK/STARFALL.PO',(OUT/'STARFALL.PO').read_bytes())
                profile=original.decode()
                for name,value in {'smartport.disk.1.path':'"0:/IRQCHECK/STARFALL.PO"',
                    'smartport.disk.1.enabled':'ON','vtw.speed.mode':'3'}.items():
                    profile,count=re.subn(r'(?m)^'+re.escape(name)+r'=[^\r\n]*',name+'='+value,profile)
                    assert count==1
                put(ftp,'/appletini_cfg.txt',profile.encode())
                updated=True;put(ftp,'/FIRMWARE.BIN',image)
            transfer(stage);print('Verified diagnostic firmware and owned STARFALL disk; rebooting.',flush=True)
            boot()
            for _ in range(40):
                mode=dump(0xB370,1)[0]
                if mode==0 and dump(build['game']['GAME_UPDATE'],3).hex()==build['update_prefix']:break
                time.sleep(1)
            else:raise RuntimeError('STARFALL title not ready')
            key('P')
            for _ in range(30):
                if dump(0xB370,1)[0]==1 and dump(0xB47C,1)[0]==1:break
                time.sleep(1)
            else:raise RuntimeError('STARFALL Phasor audio did not start')
            saved_control=read(0x400001C0)
            assert not saved_control&0x100,'Close the Appletini menu before the test'
            command(f'mwr 0x400001C0 0x{saved_control|0x100:X}',fast=True)
            assert read(0x400001C0)&0x100,'CPU pause did not apply'
            assert dump(build['game']['GAME_IRQ'],4).hex()==build['irq_prefix']
            code=(OUT/'probe.bin').read_bytes()
            write(0xA280,code);write(0xA900,bytes(256))
            assert dump(0xA280,len(code))==code
            for target,entry,size in ((build['game']['GAME_IRQ'],labels['probe_irq'],4),
                                      (build['game']['GAME_UPDATE'],labels['probe_foreground'],3)):
                patch=b'\x4C'+entry.to_bytes(2,'little')+b'\xEA'*(size-3)
                write(target,patch);assert dump(target,size)==patch
            command(f'mwr 0x400001C0 0x{saved_control:X}',fast=True)
            assert not read(0x400001C0)&0x100,'CPU resume did not apply'
            print('Probe loaded and checked; real STARFALL audio is running.',flush=True)
            for speed in ('1mhz-calibration','turbo','26mhz'):
                setting={'1mhz-calibration':'1mhz','turbo':'turbo','26mhz':'div 5'}[speed]
                command('vtw speed '+setting,1)
                if speed!='1mhz-calibration':key('R');time.sleep(1)
                entry={'speed':speed,'profile':command('vtw status',1),'calibration':[],'phases':[]}
                report['cases'].append(entry)
                # Sample continuously for 32768 * 256 ticks. A slow game
                # can mask IRQs for >65536 ticks, so IRQ-only extension is
                # not a valid calibration source at native CPU speed.
                write(labels['cal_wait_limit'],(32768).to_bytes(2,'little'))
                probe_command(5)
                raw=dump(labels['cal_before'],52)
                first_before,last_after=struct.unpack_from('<II',raw)
                first_after,last_before=struct.unpack_from('<II',raw,42)
                for before_units,after_units,packet in ((first_before,first_after,raw[8:24]),
                                                       (last_before,last_after,raw[24:40])):
                    assert packet[:4]==b'ATTC',packet.hex()
                    ticks,hz=struct.unpack_from('<QI',packet,4)
                    entry['calibration'].append({'before_units':before_units,'after_units':after_units,
                                                 'arm_ticks':ticks,'arm_hz':hz})
                first,last=entry['calibration'][0],entry['calibration'][-1]
                elapsed=(last['arm_ticks']-first['arm_ticks'])/first['arm_hz']
                low=(last['before_units']*256-(first['after_units']*256+255))/elapsed
                high=((last['after_units']*256+255)-first['before_units']*256)/elapsed
                entry['calibration_bounds_hz']=[low,high]
                frequency=(low+high)/2
                assert 1000000<low<high<1030000,'Invalid VIA calibration span'
                if speed=='1mhz-calibration':
                    entry['scope']='timer calibration only; no 1 MHz gameplay acceptance'
                    print(f'{speed}: VIA {low:.2f}..{high:.2f} Hz; no game workload.',flush=True)
                    continue
                latch=int(frequency/100)-2
                write(labels['latch'],latch.to_bytes(2,'little'));probe_command(0x20)
                entry['audio_period_ticks']=latch+2
                print(f'{speed}: calibrated VIA {low:.2f}..{high:.2f} Hz; audio divisor {latch+2}.',flush=True)
                for phase,name,size in ((0x10,'baseline',0),(0x11,'raw128',128),(0x12,'raw511',511),
                                        (0x13,'raw512',512),(0x14,'rom512',512),(0x15,'positive25ms',0)):
                    key('R');time.sleep(.5);before=stamp();probe_command(phase)
                    time.sleep(6)
                    # Freeze before reading payload: a live ROM copy can leave
                    # its last byte at the guard initializer during UART dump.
                    probe_command(4)
                    if size:
                        payload=dump(0xAA00,size)
                        assert payload[1]==1 and payload[2:6]==bytes(4)
                        assert int.from_bytes(payload[6:8],'little')==size-8
                        assert payload[8:]==bytes(((p*73+(p>>8)*19+90)^(p>>4))&255 for p in range(size-8)),name
                    raw=dump(0xA900,128)
                    assert raw[9]==0 and raw[60]==0,(name,raw.hex())
                    after=stamp()
                    u16=lambda at:int.from_bytes(raw[at:at+2],'little')
                    u32=lambda at:int.from_bytes(raw[at:at+4],'little')
                    duration=(u32(30)-u32(26))*256/frequency
                    ticks=(u32(38)-u32(34))&0xFFFFFFFF
                    expected=(u32(30)-u32(26))*256/(latch+2)
                    item={'name':name,'seconds':duration,'audio_ticks':ticks,'expected_audio_ticks':expected,
                          'tick_deficit':expected-ticks,'irq_count':u16(16),'calls':u16(50),
                          'max_irq_gap_ms':u16(14)*256/frequency*1000,
                          'max_foreground_gap_ms':u16(18)*256/frequency*1000,
                          'max_mask_ticks_approx':u16(52)*256,'max_mask_ms_approx':u16(52)*256/frequency*1000,
                          'frames':(u32(46)-u32(42))&0xFFFFFFFF,'stamp_before':before,'stamp_after':after,
                          'game_state':dump(0xB370,48).hex(),'last_payload_verified':bool(size)}
                    (OUT/(speed+'-'+name+'.bin')).write_bytes(raw)
                    entry['phases'].append(item)
                    print(f'{speed} {name}: {ticks} ticks, deficit {expected-ticks:.2f}, '
                          f'IRQ gap {item["max_irq_gap_ms"]:.2f} ms, mask {item["max_mask_ms_approx"]:.2f} ms, {item["calls"]} calls.',flush=True)
            report['completed']=True
        finally:
            try:
                if original is not None:
                    def restore(ftp):
                        put(ftp,'/appletini_cfg.txt',original)
                        if owned:ftp.delete('/IRQCHECK/STARFALL.PO');ftp.rmd('/IRQCHECK')
                        if updated:put(ftp,'/FIRMWARE.BIN',stock)
                    transfer(restore);boot()
                    def verify(ftp):
                        assert get(ftp,'/appletini_cfg.txt')==original
                        assert sha(get(ftp,'/FIRMWARE.OK'))==STOCK_HASH
                    transfer(verify);boot()
                    report['restored']={'settings_sha256':sha(original),'media':command('sd status',1),
                                        'runtime':command('status',1),'ftp':command('ftp status',1)}
                    assert 'SP1 open=1' in report['restored']['media'] and 'ftp: off' in report['restored']['ftp']
                    print('Exact stock firmware/settings restored; original media open; FTP off.',flush=True)
            finally:(OUT/'board-results.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')


if __name__=='__main__':main()
