#!/usr/bin/env python3
"""Run one owned ONE//e transport measurement; never treat a model as a board run."""
import argparse
from datetime import datetime,timezone
import json
from pathlib import Path
import re
import serial
import sys
import time
from https_probe_host import serial_command

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'build/net-transport-checkpoint'

def screen(text):
    memory={}
    for match in re.finditer(r'(?m)^([0-9A-F]{5}):((?: [0-9A-F]{2})+)',text.replace('\r','')):
        for i,byte in enumerate(match[2].split()):memory[int(match[1],16)+i]=int(byte,16)
    if len(memory)!=1024:raise RuntimeError('Incomplete screen readback')
    lines=[]
    for row in range(24):
        base=0x400+(row&7)*0x80+(row>>3)*0x28
        chars=[memory[base+i]&127 for i in range(40)]
        lines.append(''.join(chr(c+64 if 0<c<32 else 32 if c==0 else c) for c in chars))
    return '\n'.join(lines)

def main():
    sys.stdout.reconfigure(encoding='utf-8',errors='backslashreplace')
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--mode',choices=['rom','libtini','timing-rom'],required=True)
    p.add_argument('--speed',choices=['1mhz','turbo'],required=True)
    p.add_argument('--max-seconds',type=int,default=1200)
    p.add_argument('--resume',action='store_true',help='collect an already running or completed case without sending keys')
    args=p.parse_args();OUT.mkdir(parents=True,exist_ok=True)
    expected=1008 if args.mode=='timing-rom' else 10000
    stamp=datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    prefix=OUT/(stamp+'-'+args.mode+'-'+args.speed)
    report={'mode':args.mode,'speed':args.speed,'pass':False,'board_run':True,
            'poll_interval_seconds':15,'started_utc':stamp}
    transcript=[];error=None
    symbols=json.loads((OUT/'live-symbols.json').read_text())
    with serial.Serial('COM13',921600,timeout=.05) as uart:
        def command(text,timeout=.4):
            result=serial_command(uart,text,timeout=timeout);transcript.append(result);return result
        def read(addr):
            text=command(f'mrd 0x{addr:08X}')
            match=re.search(rf'0x{addr:08X}:\s*0x([0-9A-F]{{8}})',text,re.I)
            if not match:raise RuntimeError('Missing memory readback')
            return int(match[1],16)
        try:
            code=symbols['net_transport_test_status']
            if read(code['address'])!=code['first_word']:raise RuntimeError('Test ELF does not match the running code')
            if (read(0x4000017C)&0xFF000200)!=0xE1000200:raise RuntimeError('ONE//e bridge not active')
            initial=screen(command('vtw dump 400 400',2))
            if not args.resume and ('SMARTPORT NET TRANSPORT CHECKPOINT' not in initial or 'SPEED LABEL:' in initial):
                raise RuntimeError('Diagnostic must be freshly launched and waiting at R/L/X/P')
            if not args.resume and read(symbols['read_count']['address'])!=0:raise RuntimeError('Expected a clean test source')
            if not args.resume:command('vtw speed '+args.speed)
            report['mode_status']=command('vtw status',1)
            expected_speed='speed=TURBO' if args.speed=='turbo' else 'speed=1 MHz'
            if expected_speed not in report['mode_status'] or 'c074=0' not in report['mode_status']:
                raise RuntimeError('Actual CPU mode does not match the case')
            key=('R' if args.mode=='rom' else 'S' if args.mode=='timing-rom' else 'L')+('1' if args.speed=='1mhz' else 'T')
            if not args.resume:
                for char in key:command(f'mwr 0x40000170 0x{ord(char):02X}',.15)
            report['resumed']=args.resume
            started=time.monotonic();last=-1;stale=0
            while time.monotonic()-started<args.max_seconds:
                time.sleep(15)
                count=read(symbols['read_count']['address'])
                print(f'{args.mode} {args.speed}: {count}/{expected} reads',flush=True)
                stale=stale+1 if count==last else 0;last=count
                if count>=expected or stale>=2:
                    time.sleep(3)
                    display=screen(command('vtw dump 400 400',2))
                    report['screen']=display;report['source_reads']=count
                    if f'PASS AT READ {expected} RC=00' in display:
                        report['pass']=True;break
                    if 'FAIL AT READ' in display or (count<expected and stale>=2):raise RuntimeError(display)
            if not report['pass']:raise RuntimeError('Board run deadline expired')
            report['elapsed_host_seconds']=time.monotonic()-started
        except Exception as exc:error=str(exc)
    if error:report['error']=error
    prefix.with_suffix('.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    prefix.with_suffix('.txt').write_text(''.join(transcript),encoding='utf-8')
    print('PASS board run' if report['pass'] else 'FAIL board run',flush=True)
    if error:raise SystemExit(error)
if __name__=='__main__':main()
