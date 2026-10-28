"""Stress CPU0 RSA with moving video, USB and actual Apple disk writes.

Only /RSASTRESS and the two copied scratch images belong to this test.
The journal preserves the exact prior profile and firmware; seed state is
never copied or rolled back. Restore runs on both success and failure.
"""
from pathlib import Path
import argparse,hashlib,json,re,sys,time
from board_http_acceptance import Board
from smartport_timing_board import get,put
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/rsa-stress'
sys.path.insert(0,str(ROOT/'software/net_transport_checkpoint'))
from prodos_disk import read_root_files,validate_image
sha=lambda b:hashlib.sha256(b).hexdigest()
def result_rows(text):
    return [{k:int(v,16 if v.lstrip('-').startswith('0x') else 10)
             for k,v in re.findall(r'(\w+)=(-?0x[0-9A-Fa-f]+|-?\d+)',line)}
            for line in re.findall(r'\[http-result\][^\r\n]*',text)]
def phase_rows(text):
    rows=[]
    for line in re.findall(r'\[http-phase\][^\r\n]*',text):
        row={k:int(v) for k,v in re.findall(r'(\w+)=(\d+)',line)}
        row['name']=re.search(r'name=(\w+)',line)[1];rows.append(row)
    return rows
def setting(profile,key,value):
    pattern=r'(?m)^'+re.escape(key)+r'=[^\r\n]*'
    assert len(re.findall(pattern,profile))==1,key
    return re.sub(pattern,key+'='+value,profile)
def main():
    args=argparse.ArgumentParser();args.add_argument('action',choices=['stage','restage','run','restore'])
    args.add_argument('--seconds',type=int,default=60);args=args.parse_args()
    sys.stdout.reconfigure(encoding='utf-8');OUT.mkdir(parents=True,exist_ok=True)
    journal_path=OUT/'journal.json';journal=json.loads(journal_path.read_text()) if journal_path.exists() else {}
    build=json.loads((OUT/'client-build.json').read_text());labels=build['labels']
    def save():journal_path.write_text(json.dumps(journal,indent=2)+'\n')
    with Board(OUT) as b:
        def restore():
            if journal.get('restored'):return
            assert journal.get('changed') and (OUT/'original-settings.bin').is_file()
            def files(ftp):
                put(ftp,'/appletini_cfg.txt',(OUT/'original-settings.bin').read_bytes())
                put(ftp,'/FIRMWARE.BIN',(OUT/'inputs/FIRMWARE.BIN').read_bytes())
            b.command('http cancel',.4)
            if journal.get('test_running'):
                b.poke(labels['_stress_command'],b'\3');b.wait(labels['_stress_phase'],(2,3))
                journal['test_running']=False;save()
            b.ftp(files);journal['restore_boot']=b.reboot()
            journal['restored']=True;save();print('Original firmware and profile restored.',flush=True)
        if args.action=='restore':restore();return
        if args.action in ('stage','restage'):
            restage=args.action=='restage'
            if restage:
                assert journal.get('restored') and journal.get('directory_created')
                journal['previous_error']=journal.pop('error',None);journal['cases']=[];journal['pass']=False
                journal['restored']=False;journal['test_running']=False
            else:assert not journal,'Existing stress journal: inspect before another stage'
            original=(OUT/'inputs/FIRMWARE.BIN').read_bytes();diagnostic=(OUT/'inputs/DIAGNOSTIC.BIN').read_bytes()
            journal.update(before=b.command('status',1)+b.command('vtw status',1),changed=False,
                original_firmware_sha256=sha(original),diagnostic_sha256=sha(diagnostic),cases=[],restored=False)
            def stage(ftp):
                assert sha(get(ftp,'/FIRMWARE.OK'))==sha(original),'Unexpected installed firmware'
                names={Path(p).name.upper() for p in ftp.nlst('/')};assert ('RSASTRESS' in names)==restage
                assert 'FIRMWARE.BIN' not in names,'Pending update exists'
                profile=get(ftp,'/appletini_cfg.txt')
                if restage:assert profile==(OUT/'original-settings.bin').read_bytes()
                else:(OUT/'original-settings.bin').write_bytes(profile)
                assert sha(get(ftp,'/ATNETCA.PEM'))==sha((ROOT/'software/https_setup/ATNETCA.PEM').read_bytes())
                if not restage:ftp.mkd('/RSASTRESS')
                journal['directory_created']=True;save()
                for name in ('RSAHOST.PO','RSADISK.PO'):put(ftp,'/RSASTRESS/'+name,(OUT/name).read_bytes())
                profile=profile.decode()
                for key,value in {'smartport.disk.1.path':'"0:/RSASTRESS/RSAHOST.PO"','smartport.disk.1.enabled':'ON',
                    'disk2.drive.1.path':'"0:/RSASTRESS/RSADISK.PO"','disk2.drive.2.path':'""','disk2.slot6.enabled':'ON',
                    'vtw.speed.mode':'3','vtw.disk2.acceleration.disabled':'OFF','vtw.slowdown.mask':'0x0'}.items():profile=setting(profile,key,value)
                journal['profile']=profile;journal['changed']=True;save()
                put(ftp,'/appletini_cfg.txt',profile.encode());put(ftp,'/FIRMWARE.BIN',diagnostic)
            try:
                b.ftp(stage);journal['diagnostic_boot']=b.reboot();b.wait(labels['_stress_frames'],tuple(range(1,256)))
                journal['staged']=True;save();print('READY: moving video and isolated SmartPort/Disk II volumes; waiting for USB input.',flush=True)
            except Exception:
                if journal.get('changed'):restore()
                raise
            return
        assert journal.get('staged') and not journal.get('restored')
        def word(name,n=2):return int.from_bytes(b.dump(labels[name],n),'little')
        def collect(speed):
            phase=word('_stress_phase',1);assert phase==2,phase
            row={'speed':speed,'rounds':word('_stress_round'),'smartport_verified':word('_stress_sp'),
                'disk2_verified':word('_stress_d2'),'frames':word('_stress_frames'),'video_errors':word('_stress_video_errors')}
            tags=b.dump(labels['_stress_tags'],256)
            frame=b.dump(0x400,1024);tag=row['frames']&255
            for y in range(24):
                at=(y&7)*128+(y>>3)*40
                for x in range(40):assert frame[at+x]==((x+y+tag)&15)|(((x^y^tag)&15)<<4),(y,x)
            row['video_ram_verified']=True
            def download(ftp):
                for target,volume in enumerate(('RSAHOST','RSADISK')):
                    image=get(ftp,'/RSASTRESS/'+volume+'.PO');(OUT/(speed+'-'+volume+'.PO')).write_bytes(image)
                    validate_image(image);actual=read_root_files(image);original=read_root_files((OUT/(volume+'.PO')).read_bytes())
                    assert actual.keys()==original.keys()
                    for name in original:
                        if name!='SCRATCH':assert actual[name]==original[name],(volume,name)
                    scratch=actual['SCRATCH'];assert len(scratch)==32768
                    for block in range(64):
                        epoch=int.from_bytes(tags[target*128+block*2:target*128+block*2+2],'little')
                        expected=bytes(512) if epoch==65535 else bytes(((i*17)^(block*13)^(epoch*29)^(epoch>>8)^(target*0x69)^0x53)&255 for i in range(512))
                        assert scratch[block*512:(block+1)*512]==expected,(volume,block,epoch)
                    row[volume]={'sha256':sha(image),'file_bytes':sum(map(len,actual.values())),
                        'structure_valid':True,'all_scratch_blocks_verified':True,'unchanged_files_verified':True}
            b.ftp(download);return row
        try:
            for speed in ('full','turbo'):
                # A reboot clears counters and closes all ProDOS handles. Only
                # the owned scratch images are replaced between the two runs.
                if speed=='turbo':
                    def reset(ftp):
                        for name in ('RSAHOST.PO','RSADISK.PO'):put(ftp,'/RSASTRESS/'+name,(OUT/name).read_bytes())
                    b.ftp(reset);b.reboot()
                b.command('vtw speed '+speed,.5);b.wait(labels['_stress_phase'],(0,))
                assert b.dump(0x2000,16)==(OUT/'RSATEST.SYSTEM').read_bytes()[:16],'Stress SYS is not loaded'
                first_frames=word('_stress_frames');second_frames=word('_stress_frames')
                assert first_frames!=second_frames,'Stress video loop is not running'
                before=b.command('vtw status',.8);journal['test_running']=True;save();b.poke(labels['_stress_command'],b'\1')
                log_start=len(b.logs);b.command('http stress '+str(args.seconds),.5)
                deadline=time.monotonic()+args.seconds+50;notice=time.monotonic()+10;probe_at=time.monotonic()+5
                while time.monotonic()<deadline:
                    data=b.uart.read(max(1,b.uart.in_waiting)).decode('utf-8',errors='replace')
                    if data:b.logs.append(data)
                    text=''.join(b.logs[log_start:])
                    if '[http-bench] done' in text:break
                    if time.monotonic()>probe_at:
                        phase=word('_stress_phase',1)
                        if phase==3:
                            failure={n:word(n,1 if n in ('_stress_target','_stress_stage','_stress_errno','_stress_oserror','_stress_got','_stress_expected') else 2) for n in ('_stress_target','_stress_stage','_stress_errno','_stress_oserror','_stress_mismatch','_stress_got','_stress_expected','_stress_round')}
                            raise RuntimeError('Apple disk verification failed: '+json.dumps(failure))
                        probe_at=time.monotonic()+5
                    if time.monotonic()>notice:
                        print(speed,'load: ',text.count('[http-result]'),'RSA results',flush=True);notice=time.monotonic()+10
                else:raise RuntimeError('Stress deadline')
                (OUT/(speed+'-uart.txt')).write_text(text,encoding='utf-8')
                requests=result_rows(text)
                hashes=re.findall(r'\[http-sha256\] ([a-f0-9]{64})',text)
                expected=json.loads((OUT/'expected.json').read_text())
                assert len(requests)==len(hashes)>=20,(len(requests),len(hashes))
                for i,(r,digest) in enumerate(zip(requests,hashes)):
                    assert r['rc']==0 and r['verify']==0 and r['status']==200 and r['handshake_us']>0,r
                    assert digest==expected['rsa2048' if i%2==0 else 'rsa4096']['sha256']
                phases=phase_rows(text)
                assert len(phases)==3
                for p in phases:
                    assert p['health'] and p['video_frames'] and p['capture_records']
                    assert not any(p[k] for k in ('gaps','oversized','resync','hid_errors','xbox_errors')),(speed,p)
                assert phases[1]['hid']>=10 and phases[1]['xbox']>=10,phases[1]
                assert '[http-stress-summary]' in text and 'start_errors=0' in text
                b.poke(labels['_stress_command'],b'\3');b.wait(labels['_stress_phase'],(2,))
                journal['test_running']=False;save()
                row=collect(speed);row.update(requests=requests,hashes=hashes,phases=phases,live_profile=before)
                assert row['smartport_verified']>=16 and row['disk2_verified']>=16 and row['video_errors']==0,row
                journal['cases'].append(row);save();print('PASS',speed,len(requests),'RSA handshakes,',row['smartport_verified'],'SP and',row['disk2_verified'],'Disk II verified writes.',flush=True)
            journal['pass']=True;save()
        except Exception as e:journal['error']=str(e);journal['pass']=False;save();raise
        finally:restore()
if __name__=='__main__':main()
