"""UART, SD and stock-image helpers for the SmartPort timing comparison."""
import io
import json
from pathlib import Path
import re
import time

ROOT=Path(__file__).resolve().parents[1]
STOCK_HASH='cbbdd97ff97d43f7609f3da73ff0368164621350b18ff6ec7398c480d66672c2'


def serial_command(uart,command,timeout=1):
    wire=(':'+command+'\r\n').encode('ascii')
    # CPU0 polls between compositor passes; pace input to fit its RX FIFO.
    for at in range(0,len(wire),8):
        uart.write(wire[at:at+8]);uart.flush();time.sleep(.01)
    result=bytearray();deadline=time.monotonic()+timeout
    while time.monotonic()<deadline:
        result.extend(uart.read(max(1,uart.in_waiting)))
    return result.decode('utf-8',errors='replace')


def screen(text):
    memory={}
    for match in re.finditer(r'(?m)^([0-9A-F]{5}):((?: [0-9A-F]{2})+)',text.replace('\r','')):
        for i,value in enumerate(match[2].split()):memory[int(match[1],16)+i]=int(value,16)
    if len(memory)!=1024:raise RuntimeError('Incomplete screen readback')
    rows=[]
    for row in range(24):
        base=0x400+(row&7)*0x80+(row>>3)*0x28
        chars=[memory[base+i]&127 for i in range(40)]
        rows.append(''.join(chr(c+64 if 0<c<32 else 32 if c==0 else c) for c in chars))
    return '\n'.join(rows)


def get(ftp,path):
    data=io.BytesIO();ftp.retrbinary('RETR '+path,data.write)
    return data.getvalue()


def put(ftp,path,data):
    ftp.storbinary('STOR '+path,io.BytesIO(data))
    if get(ftp,path)!=data:raise RuntimeError('SD upload readback mismatch: '+path)


def stock_fingerprints():
    """Entry words extracted from the archived stock ELF, with its hash."""
    fixture=json.loads((ROOT/'scripts/fixtures/smartport_stock_f1_3_3b3.json').read_text())
    assert fixture['firmware_sha256']==STOCK_HASH
    return fixture['fingerprints']
