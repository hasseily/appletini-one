#!/usr/bin/env python3
"""Read back owned scratch volumes and validate completed board results."""
import argparse
import csv
from datetime import datetime,timezone
import ftplib
import hashlib
import io
import json
from pathlib import Path
import serial
import struct
import sys
from https_probe_host import serial_command

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'build/net-transport-checkpoint'
sys.path.insert(0,str(ROOT/'software/net_transport_checkpoint'))
from prodos_disk import read_root_files,validate_image

def main():
    sys.stdout.reconfigure(encoding='utf-8',errors='backslashreplace')
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--board',default='10.17.1.18')
    p.add_argument('--expected-scratch-round',type=int,
                   help='verify both persisted scratch files against the final completed write round')
    args=p.parse_args();OUT.mkdir(parents=True,exist_ok=True)
    stamp=datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    report={'collected_utc':stamp,'runs':{},'source':'physical board SD readback'}
    log=[]
    with serial.Serial('COM13',921600,timeout=.05) as uart:
        log.append(serial_command(uart,'sd status',timeout=1))
        log.append(serial_command(uart,'disk2 status',timeout=1))
        log.append(serial_command(uart,'ftp on',timeout=1))
        try:
            with ftplib.FTP() as ftp:
                ftp.connect(args.board,timeout=15);ftp.login('anonymous','appletini-checkpoint')
                for name in ('NETSP','NETD2'):
                    data=io.BytesIO();ftp.retrbinary('RETR /NETTEST/'+name+'.PO',data.write)
                    payload=data.getvalue();(OUT/(stamp+'-'+name+'.po')).write_bytes(payload)
                    try:
                        validate_image(payload);files=read_root_files(payload)
                        report[name]={'sha256':hashlib.sha256(payload).hexdigest(),'valid_volume':True}
                    except ValueError as exc:
                        report[name]={'sha256':hashlib.sha256(payload).hexdigest(),'valid_volume':False,'error':str(exc)}
                        continue
                    scratch=files['SCRATCH']
                    assert len(scratch)==512
                    round_byte=scratch[0]
                    assert scratch==bytes((at*17+round_byte)&255 for at in range(512))
                    if args.expected_scratch_round is not None:
                        assert round_byte==args.expected_scratch_round&255
                    report[name]['scratch_sha256']=hashlib.sha256(scratch).hexdigest()
                    report[name]['scratch_round_mod256']=round_byte
                    report[name]['scratch_pattern_valid']=True
                    report[name]['expected_scratch_round']=args.expected_scratch_round
                    if name!='NETSP':continue
                    for filename in ('R1.LOG','RT.LOG','L1.LOG','LT.LOG','RS1.LOG',
                                     'R1.BIN','RT.BIN','L1.BIN','LT.BIN','RS1.BIN'):
                        if filename not in files:continue
                        raw=files[filename];(OUT/(stamp+'-'+filename)).write_bytes(raw)
                        if filename.endswith('.BIN'):
                            assert len(raw)==32+504*18 and raw[:5]==b'NTM1\x01'
                            assert raw[5] in (0,1) and raw[6] in (ord('1'),ord('T'))
                            assert raw[7]==3 and raw[18]!=0 and raw[24]==1
                            assert raw[19:24]==bytes(5) and raw[25:32]==bytes(7)
                            assert struct.unpack_from('<4H',raw,10)==(504,18,9,512)
                            header={'test':'transport-0.1','mode':'ROM' if raw[5]==0 else 'libtini-style',
                                    'speed':chr(raw[6]),'reads':str(int.from_bytes(raw[8:10],'little')),
                                    'pass':str(raw[7]&1),'timer_wrap_checked':str((raw[7]>>1)&1)}
                            rows=[]
                            for wanted,at in enumerate(range(32,len(raw),18),9):
                                count,cmin,cmax,imin,imax,ctotal,itotal=struct.unpack_from('<5H2I',raw,at)
                                rows.append(dict(size=wanted,count=count,call_min=cmin,call_max=cmax,
                                                 call_total=ctotal,irq_min=imin,irq_max=imax,irq_total=itotal))
                            converted=io.StringIO();writer=csv.DictWriter(converted,fieldnames=list(rows[0]))
                            writer.writeheader();writer.writerows(rows)
                            (OUT/(stamp+'-'+filename+'.csv')).write_text(converted.getvalue(),encoding='utf-8')
                        else:
                            text=raw.decode('ascii');lines=text.splitlines()
                            header=dict(item.split('=',1) for item in lines[0].split(','))
                            rows=list(csv.DictReader(lines[2:]))
                        expected_reads=1008 if filename.startswith('RS1.') else 10000
                        if header.get('pass')!='1' or header.get('reads')!=str(expected_reads):
                            report['runs'][filename]={'pass':False,'header':header};continue
                        assert len(rows)==504
                        total=0;maximum_call=maximum_inner=0
                        for wanted,row in enumerate(rows,9):
                            values={key:int(value) for key,value in row.items()}
                            assert values['size']==wanted
                            assert values['count']==(2 if expected_reads==1008 else 20 if wanted<=472 else 18)
                            assert 0<values['irq_min']<=values['irq_max']<65536
                            assert 0<values['call_min']<=values['call_max']<65536
                            assert values['irq_max']<=values['call_max']
                            assert values['irq_total']<=values['call_total']
                            total+=values['count'];maximum_call=max(maximum_call,values['call_max'])
                            maximum_inner=max(maximum_inner,values['irq_max'])
                        assert total==expected_reads
                        report['runs'][filename]={'pass':True,'header':header,'size_rows':len(rows),
                            'reads':total,'timing_wrap_validated':header.get('timer_wrap_checked')=='1',
                            'max_call_native_ticks':maximum_call,
                            'max_irq_inner_native_ticks':maximum_inner,
                            'irq_timing_scope':'inner interval is a lower bound; complete call interval is an upper bound',
                            'result_sha256':hashlib.sha256(raw).hexdigest()}
                    if 'RESET.OK' in files:
                        proof=files['RESET.OK'];assert len(proof)==32 and proof[:4]==b'ATNT'
                        (OUT/(stamp+'-RESET.OK')).write_bytes(proof)
                        report['reset_recovery']={'generation':proof[7],
                            'reset_sequence':int.from_bytes(proof[24:28],'little'),
                            'initial_read_count':int.from_bytes(proof[28:32],'little')}
        finally:log.append(serial_command(uart,'ftp off',timeout=1))
    (OUT/('collected-'+stamp+'.json')).write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    (OUT/('collected-'+stamp+'.txt')).write_text(''.join(log),encoding='utf-8')
    print(json.dumps(report,indent=2))
if __name__=='__main__':main()
