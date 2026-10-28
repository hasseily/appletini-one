"""Drive the actual game-over panel; submit one artificial, removable test run."""
from pathlib import Path
import json,subprocess,sys
from board_http_acceptance import Board
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/starfall-online-acceptance'

def main():
    sys.stdout.reconfigure(encoding='utf-8');OUT.mkdir(parents=True,exist_ok=True)
    manifest=json.loads((ROOT/'build/https-starfall-acceptance/inputs/manifest.json').read_text());labels=manifest['starfall_labels']
    report={'pass':False,'state_mutations':True,'score_is_artificial':True,'inputs':manifest['inputs']}
    def save():(OUT/'result.json').write_text(json.dumps(report,indent=2)+'\n')
    with Board(OUT) as b:
        try:
            assert b.dump(labels['_sf_stage'],1)==b'\7'
            assert b.dump(labels['_sf_mode'],1)==b'\0','Expected fresh title'
            b.key('P');b.wait(labels['_sf_mode'],(1,));print('STARFALL running with Phasor.',flush=True)
            b.poke(labels['_sf_state']+21,b'\1');b.wait(labels['_sf_mode'],(2,))
            b.poke(labels['_sf_state']+4,(1234).to_bytes(4,'little'))
            b.poke(labels['_sf_state']+10,(10).to_bytes(2,'little'));b.poke(labels['_sf_state']+22,b'\0')
            b.key('N');b.wait(labels['_sf_online_phase'],(0,))
            for c in 'QACARD':b.key(c)
            b.key(13);phase=b.wait(labels['_sf_online_phase'],(8,9),45)
            report['first_phase']=phase;report['http']=b.command('http status',1);save();assert phase==8,report
            run_id=b.dump(labels['_sf_online_run_id'],32).decode('ascii');report['run_id']=run_id;save()
            name=b.dump(labels['_sf_online_name'],13).split(b'\0')[0].decode();assert name=='QACARD',name
            report['name']=name;print('Card submission passed, run ID',run_id,flush=True)
            b.key(27);b.key('N');b.wait(labels['_sf_online_phase'],(0,));b.key(13)
            phase=b.wait(labels['_sf_online_phase'],(8,9),45);report['retry_phase']=phase
            assert phase==8 and b.dump(labels['_sf_online_run_id'],32).decode()==run_id
            b.key(9);phase=b.wait(labels['_sf_online_phase'],(8,9),45);report['ladder_phase']=phase;assert phase==8
            ladder=json.loads(subprocess.check_output(['curl.exe','-fsS','https://appletini.org/leaderboards/starfall/api/ladder'],text=True))
            report['ladder']=ladder;assert any(r['name']==name and r['score']==1234 and r['platform']=='appletini' for r in ladder['scores'])
            report['pass']=True;save();print('PASS actual card POST, Q in name, same-ID retry, JSON rank and top ten.',flush=True)
        except Exception as e:report['error']=str(e);save();raise
        finally:
            report['left_at_title']=b.reboot();save()
if __name__=='__main__':main()
