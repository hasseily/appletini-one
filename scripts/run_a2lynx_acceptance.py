"""Exercise the actual A2LYNX UI on the production HTTP application image."""
from pathlib import Path
import argparse,json,sys,time
from board_http_acceptance import Board
from smartport_timing_board import put
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/a2lynx-acceptance'
def main():
    args=argparse.ArgumentParser();args.add_argument('--resume',action='store_true');args=args.parse_args()
    sys.stdout.reconfigure(encoding='utf-8');report=json.loads((OUT/'result.json').read_text()) if args.resume else {'pass':False,'cases':[]}
    report.pop('error',None)
    def save():(OUT/'result.json').write_text(json.dumps(report,indent=2)+'\n')
    with Board(OUT) as b:
        def screen():
            main=b.dump(0x400,1024);aux=b.dump(0x10400,1024);rows=[]
            for row in range(24):
                at=(row&7)*128+(row>>3)*40;chars=[]
                for x in range(40):
                    for value in (aux[at+x],main[at+x]):
                        value&=127;chars.append(chr(value+64 if 0<value<32 else 32 if value==0 else value))
                rows.append(''.join(chars))
            return '\n'.join(rows)
        def wait_page(expected,seconds=40):
            deadline=time.monotonic()+seconds;last=''
            while time.monotonic()<deadline:
                last=screen()
                if expected.upper() in last.upper() and 'LOADING HTTPS/HTTP:' not in last.upper():return last
            raise RuntimeError('Page deadline for '+expected+'\n'+last)
        def goto(url):
            b.key('G')
            for c in url:b.key(c)
            b.key(13)
        try:
            b.ftp(lambda ftp:put(ftp,'/HTTPTEST1/A2LYNX.PO',(OUT/'A2LYNX.PO').read_bytes()))
            b.select('0:/HTTPTEST1/A2LYNX.PO')
            report['home']=wait_page('APPLETINI');save();print('A2LYNX home loaded over verified HTTPS.',flush=True)
            cases=[('html','https://appletini.org/leaderboards/starfall','STARFALL LEADERBOARD'),
                ('redirect','https://www.appletini.org/leaderboards/starfall','STARFALL LEADERBOARD'),
                ('plain','https://appletini.org/robots.txt','CONTENT SIGNALS'),
                ('hostname','https://wrong.host.badssl.com/','CERTIFICATE VERIFICATION FAILED'),
                ('expired','https://expired.badssl.com/','CERTIFICATE VERIFICATION FAILED'),
                ('image','https://appletini.org/favicon.ico','NO SUPPORTED TEXT CONTENT-TYPE')]
            for name,url,expected in cases:
                if any(r['case']==name for r in report['cases']):continue
                print('A2LYNX',name,flush=True);goto(url);text=wait_page(expected)
                report['cases'].append({'case':name,'url':url,'screen':text,'http':b.command('http status',.5)});save()
            goto('http://10.17.1.17:8080/delay');time.sleep(1);b.key(27)
            report['cancel']=wait_page('REQUEST CANCELLED');save()
            goto('https://appletini.org/leaderboards/starfall');report['recovery']=wait_page('STARFALL LEADERBOARD')
            report['pass']=True;save();print('PASS A2LYNX HTML/text, HTTPS redirect, certificate errors, content type, cancel/retry.',flush=True)
        except Exception as e:report['error']=str(e);save();raise
        finally:report['left_ready']=b.select('0:/HTTPTEST1/STARFALL.PO');save()
if __name__=='__main__':main()
