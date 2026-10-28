"""Boot and check a release image, preserving profile and per-board seed state."""
from pathlib import Path
import argparse,hashlib,json,re,sys
from board_http_acceptance import Board
from smartport_timing_board import get,put
from run_rsa_stress import result_rows
ROOT=Path(__file__).resolve().parents[1]
sha=lambda b:hashlib.sha256(b).hexdigest()
def main():
    p=argparse.ArgumentParser();p.add_argument('--image',type=Path,required=True);p.add_argument('--version',required=True);a=p.parse_args()
    sys.stdout.reconfigure(encoding='utf-8');out=ROOT/'build'/('release-board-'+a.version);out.mkdir(parents=True,exist_ok=True)
    image=a.image.read_bytes();previous=(ROOT/'build/rsa-stress/inputs/FIRMWARE.BIN').read_bytes()
    report={'pass':False,'version':a.version,'sha256':sha(image),'golden_written':False,'fpga_rebuilt':False,'requests':[]}
    changed=False;profile=None
    def save():(out/'result.json').write_text(json.dumps(report,indent=2)+'\n')
    with Board(out) as b:
        try:
            def stage(ftp):
                nonlocal changed,profile
                assert sha(get(ftp,'/FIRMWARE.OK'))==sha(previous)
                assert 'FIRMWARE.BIN' not in {Path(n).name.upper() for n in ftp.nlst('/')}
                profile=get(ftp,'/appletini_cfg.txt');(out/'original-profile.bin').write_bytes(profile)
                changed=True;save();put(ftp,'/FIRMWARE.BIN',image)
            b.ftp(stage);report['boot']=b.reboot()
            report['status']=b.command('status',1)
            assert 'local_fw=\'Firmware '+a.version+'\'' in report['status'],report['status']
            assert 'crypto=1 trust=1' in report['boot'];save()
            expected=json.loads((ROOT/'build/rsa-stress/expected.json').read_text())
            for name,url in [('ladder','https://appletini.org/leaderboards/starfall/api/ladder'),
                ('rsa2048','https://rsa2048.badssl.com/'),('rsa4096','https://rsa4096.badssl.com/')]:
                text=b.until('http get '+url,'[http] done');rows=result_rows(text);assert len(rows)==1
                r=rows[0];assert r['rc']==0 and r['verify']==0 and r['status']==200,r
                digest=re.search(r'\[http-sha256\] ([a-f0-9]{64})',text)[1]
                if name in expected:assert digest==expected[name]['sha256']
                report['requests'].append(dict(name=name,**r,sha256=digest));save();print('PASS release',name,flush=True)
            def verify(ftp):
                assert sha(get(ftp,'/FIRMWARE.OK'))==sha(image)
                assert get(ftp,'/appletini_cfg.txt')==profile
                report['receipt_verified']=True;report['profile_unchanged']=True
            b.ftp(verify);report['final_boot']=b.reboot()
            labels=json.loads((ROOT/'build/https-starfall-acceptance/inputs/manifest.json').read_text())['starfall_labels']
            b.wait(labels['_sf_stage'],(7,));b.wait(labels['_sf_mode'],(0,));report['final_status']=b.command('status',1)
            report['pass']=True;report['left_at_starfall_title']=True;save()
            print('PASS exact FIRMWARE.OK hash; profile preserved; release firmware at STARFALL title.',flush=True)
        except Exception as e:
            report['error']=str(e);save()
            if changed:
                b.command('http cancel',.5)
                b.ftp(lambda ftp:put(ftp,'/FIRMWARE.BIN',previous));report['rollback']=b.reboot();save()
            raise
if __name__=='__main__':main()
