#!/usr/bin/env python3
"""Read the diagnostic screen or send paced commands to its owned UART."""
import argparse
from datetime import datetime,timezone
import json
from pathlib import Path
import re
import serial
import sys
from https_probe_host import serial_command

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'build/net-transport-checkpoint'

def main():
    sys.stdout.reconfigure(encoding='utf-8',errors='backslashreplace')
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--command',action='append',default=[])
    p.add_argument('--keys',default='')
    p.add_argument('--screen',action='store_true')
    args=p.parse_args();OUT.mkdir(parents=True,exist_ok=True)
    log=[]
    with serial.Serial('COM13',921600,timeout=.05) as uart:
        uart.reset_input_buffer()
        for command in args.command:
            text=serial_command(uart,command,timeout=1);print(text);log.append(text)
        if args.keys:
            text=serial_command(uart,'mrd 0x4000017C',timeout=.5);log.append(text)
            match=re.search(r'0x4000017C:\s*0x([0-9A-F]{8})',text,re.I)
            if not match or (int(match[1],16)&0xFF000200)!=0xE1000200:
                raise RuntimeError('ONE//e input bridge is not active')
            for key in args.keys:
                text=serial_command(uart,f'mwr 0x40000170 0x{ord(key):02X}',timeout=.15)
                log.append(text)
        if args.screen:
            text=serial_command(uart,'vtw dump 400 400',timeout=2);log.append(text)
            memory={}
            for match in re.finditer(r'(?m)^([0-9A-F]{5}):((?: [0-9A-F]{2})+)',text.replace('\r','')):
                for i,byte in enumerate(match[2].split()):memory[int(match[1],16)+i]=int(byte,16)
            if len(memory)!=1024:raise RuntimeError('Incomplete screen dump')
            screen=[]
            for row in range(24):
                base=0x400+(row&7)*0x80+(row>>3)*0x28
                line=''
                for i in range(40):
                    ch=memory[base+i]&127
                    if ch<32:ch=ch+64 if ch else 32
                    line+=chr(ch)
                screen.append(line)
            print('\n'.join(screen))
        stamp=datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')
        (OUT/('uart-'+stamp+'.txt')).write_text(''.join(log),encoding='utf-8')
if __name__=='__main__':main()
