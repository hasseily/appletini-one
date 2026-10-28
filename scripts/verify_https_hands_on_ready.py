"""Verify the exact delivered artifacts on SD and leave STARFALL at its title."""
from pathlib import Path
import hashlib,json,sys
from board_http_acceptance import Board
from smartport_timing_board import get
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/https-hands-on-ready'
def main():
    sys.stdout.reconfigure(encoding='utf-8');m=json.loads((ROOT/'build/https-starfall-acceptance/inputs/manifest.json').read_text())
    labels=m['starfall_labels'];report={'pass':False,'fpga_rebuilt':False,'golden_written':False}
    with Board(OUT) as b:
        def check(ftp):
            files={'FIRMWARE.BIN':'/FIRMWARE.OK','STARFALL.PO':'/HTTPTEST1/STARFALL.PO','ATNETCA.PEM':'/ATNETCA.PEM'}
            for name,path in files.items():
                digest=hashlib.sha256(get(ftp,path)).hexdigest();assert digest==m['inputs'][name],(name,digest)
                report[name]=digest
            profile=get(ftp,'/appletini_cfg.txt').decode();assert 'smartport.disk.1.path="0:/HTTPTEST1/STARFALL.PO"' in profile
            assert 'ethernet.dns=8.8.8.8' in profile;report['dns']='8.8.8.8'
        b.ftp(check);report['boot']=b.reboot();b.wait(labels['_sf_stage'],(7,));b.wait(labels['_sf_mode'],(0,))
        report['status']=b.command('status',1);report['speed']=b.command('vtw status',1)
        assert 'speed=TURBO' in report['speed'] and 'crypto=1 trust=1' in report['boot']
        report['pass']=True;report['left_at_title']=True
        (OUT/'result.json').write_text(json.dumps(report,indent=2)+'\n')
        print('PASS exact firmware receipt, game disk and public roots; STARFALL at title in TURBO; DNS 8.8.8.8.',flush=True)
if __name__=='__main__':main()
