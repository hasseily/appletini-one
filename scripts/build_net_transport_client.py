#!/usr/bin/env python3
"""Build a standalone ProDOS diagnostic and two owned scratch volumes."""
import hashlib
import json
from pathlib import Path
import subprocess
import struct
import sys

ROOT=Path(__file__).resolve().parents[1]
SOURCE=ROOT/'software/net_transport_checkpoint'
OUT=ROOT/'build/net-transport-checkpoint'
sys.path.insert(0,str(SOURCE))
from prodos_disk import build_image, validate_image

def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()

def buffer_layout(label_path):
    labels={line.split()[2].lstrip('.'):int(line.split()[1],16)
            for line in label_path.read_text().splitlines()}
    start=labels['_nt_buffer']
    size=labels['__NETBUF_SIZE__']
    himem=labels['__HIMEM__']
    heap_end=himem-labels['__STACKSIZE__']
    bss_end=labels['__BSS_RUN__']+labels['__BSS_SIZE__']
    if start!=0x9000 or size!=514 or labels['__NETBUF_RUN__']!=start:
        raise RuntimeError('Reply and guard bytes must occupy reserved MAIN $9000..$9201')
    if (labels['__MAIN_START__']+labels['__MAIN_SIZE__']>start or
        himem>start or bss_end>heap_end):
        raise RuntimeError('Program, heap or stack overlaps the reserved reply buffer')
    return {'reply_start':start,'reply_end_exclusive':start+size,
            'bss_end':bss_end,'heap_end_exclusive':heap_end,
            'software_stack_start':heap_end,'software_stack_end_exclusive':himem}

def main():
    OUT.mkdir(parents=True,exist_ok=True)
    program=OUT/'NETTEST.SYSTEM'
    container=OUT/'NETTEST.as'
    subprocess.run(['cl65','-t','apple2enh','-C',str(SOURCE/'apple2enh.cfg'),
                    '-O','--start-addr','0x2000',
                    '-Wl','-D,__FILETYPE__=0xFF','-o',str(container),
                    '-m',str(OUT/'client.map'),'-Ln',str(OUT/'client.lbl'),
                    str(SOURCE/'main.c'),str(SOURCE/'transport.s')],cwd=OUT,check=True)
    layout=buffer_layout(OUT/'client.lbl')
    raw=container.read_bytes()
    if raw[:8]!=bytes.fromhex('0005160000020000'):raise RuntimeError('Expected AppleSingle v2')
    entries={}
    for at in range(26,26+12*int.from_bytes(raw[24:26],'big'),12):
        kind,offset,length=struct.unpack_from('>III',raw,at)
        if offset+length>len(raw):raise RuntimeError('Truncated AppleSingle entry')
        entries[kind]=raw[offset:offset+length]
    if entries.get(11)!=bytes.fromhex('00c300ff00002000'):
        raise RuntimeError('SYS must be file type FF, load address $2000')
    if entries[1][:3]!=bytes.fromhex('a2ff9a'):raise RuntimeError('Unexpected SYS startup')
    program.write_bytes(entries[1])
    template=ROOT/'software/ProDOS_2_4_3.po'
    for volume,blocks in (('NETSP',1600),('NETD2',280)):
        image=build_image(template.read_bytes(),program.read_bytes(),'NETTEST.SYSTEM',
                          total=blocks,volume=volume,
                          files=[('SCRATCH',bytes(512),0x06,0)])
        validate_image(image)
        (OUT/(volume+'.po')).write_bytes(image)
    report={'program_sha256':digest(program),'prodos_input_sha256':digest(template),
            'buffer_layout':layout,
            'volumes':{name:digest(OUT/name) for name in ('NETSP.po','NETD2.po')},
            'sources':{p.relative_to(ROOT).as_posix():digest(p) for p in SOURCE.iterdir() if p.is_file()}}
    (OUT/'client-build-report.json').write_text(json.dumps(report,indent=2)+'\n')
    print('PASS Apple client and validated NETSP/NETD2 scratch volumes')
if __name__=='__main__':main()
