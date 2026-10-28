#!/usr/bin/env python3
"""Check the linked reply reservation and reject code/BSS/buffer overlap."""
import json
from pathlib import Path
import subprocess

from build_net_transport_client import ROOT, SOURCE, OUT, buffer_layout, main as build


def main():
    build()
    layout=buffer_layout(OUT/'client.lbl')
    # The assembly model must exercise the same reserved destination.
    for source,name in ((SOURCE/'transport.s','transport'),
                        (ROOT/'scripts/fixtures/net_transport_client_globals.s','client_globals')):
        subprocess.run(['ca65',str(source),'-o',str(OUT/(name+'.o'))],check=True)
    subprocess.run(['ld65','-C',str(SOURCE/'model.cfg'),'-o',str(OUT/'client-model.bin'),
                    '-Ln',str(OUT/'client-model.lbl'),str(OUT/'transport.o'),
                    str(OUT/'client_globals.o')],check=True)
    model_labels={line.split()[2].lstrip('.'):int(line.split()[1],16)
                  for line in (OUT/'client-model.lbl').read_text().splitlines()}
    assert model_labels['_nt_buffer']==layout['reply_start']
    assert model_labels['__NETBUF_SIZE__']==514
    checks=[]
    # Link real client sources with oversized segments. The linker must fail
    # rather than put code, globals or guard bytes into the reserved region.
    for segment,size in (('CODE',0x8000),('BSS',0x8000),('NETBUF',1)):
        source=OUT/('overflow_'+segment.lower()+'.s')
        source.write_text('.segment "'+segment+'"\n.res '+str(size)+'\n')
        result=subprocess.run(['cl65','-t','apple2enh','-C',str(SOURCE/'apple2enh.cfg'),
                               '-O','-o',str(OUT/'overflow.as'),
                               str(SOURCE/'main.c'),str(SOURCE/'transport.s'),str(source)],
                              cwd=OUT,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
        (OUT/('overflow_'+segment.lower()+'.log')).write_text(result.stdout)
        assert result.returncode and 'overflow' in result.stdout.lower(), (segment,result.stdout)
        checks.append(segment)
    report={'pass':True,'buffer_layout':layout,'model_reply_start':model_labels['_nt_buffer'],
            'linker_overflow_rejected':checks}
    (OUT/'buffer-layout-test.json').write_text(json.dumps(report,indent=2)+'\n')
    print('PASS reserved non-video reply/guards; program, heap, stack and overflow checks')


if __name__=='__main__':
    main()
