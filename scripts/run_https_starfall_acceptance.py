"""Run copied production artifacts on the board and leave the test game ready.

The journal saves original public settings. Seeds stay on this board and are
never read back to the PC. Golden and FPGA inputs remain unchanged.
"""
from datetime import datetime,timezone
from pathlib import Path
import argparse,ftplib,hashlib,io,json,os,re,serial,sys,time,urllib.request
from smartport_timing_board import serial_command,get,put,STOCK_HASH
from setup_https import seed_record
from http_service_host import json32

ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/https-starfall-acceptance'
sha=lambda b:hashlib.sha256(b).hexdigest()
def main():
    parser=argparse.ArgumentParser();parser.add_argument('--resume',action='store_true')
    parser.add_argument('--replace-inputs',action='store_true');args=parser.parse_args()
    assert not args.replace_inputs or args.resume
    sys.stdout.reconfigure(encoding='utf-8')
    inputs=OUT/'inputs';manifest=json.loads((inputs/'manifest.json').read_text())
    for name,value in manifest['inputs'].items():assert sha((inputs/name).read_bytes())==value,name
    assert args.resume or not (OUT/'journal.json').exists(),'Inspect the previous acceptance journal before another run'
    report={'pass':False,'inputs':manifest['inputs'],'fpga_rebuilt':False,'golden_written':False,'requests':[],'sdk':[]}
    journal={'seed_created':False,'firmware_loaded':False,'public_backup':{},'test_directory':'/HTTPTEST1'}
    if args.resume:
        report=json.loads((OUT/'result.json').read_text());journal=json.loads((OUT/'journal.json').read_text())
        assert (args.replace_inputs or report['inputs']==manifest['inputs']) and journal['firmware_loaded']
        report.pop('error',None)
    logs=[(OUT/'uart.txt').read_text(encoding='utf-8')] if args.resume else []
    def save():
        (OUT/'result.json').write_text(json.dumps(report,indent=2)+'\n')
        (OUT/'journal.json').write_text(json.dumps(journal,indent=2)+'\n')
    with serial.Serial('COM13',921600,timeout=.01) as uart:
        def command(text,timeout=.5,until=None):
            if until is None:value=serial_command(uart,text,timeout=timeout)
            else:
                wire=(':'+text+'\r\n').encode();data=bytearray()
                for at in range(0,len(wire),8):uart.write(wire[at:at+8]);uart.flush();time.sleep(.01)
                deadline=time.monotonic()+timeout;needle=until.replace('\r','').encode()
                while time.monotonic()<deadline:
                    data.extend(uart.read(max(1,uart.in_waiting)))
                    if needle in data.replace(b'\r',b''):break
                value=data.decode('utf-8',errors='replace')
            logs.append(value);(OUT/'uart.txt').write_text(''.join(logs),encoding='utf-8')
            return value
        def transfer(fn):
            value=command('ftp on',6,'ftp: on (anonymous read/write, passive)\r\n');assert 'ftp: on' in value,value
            deadline=time.monotonic()+50
            while True:
                ftp=ftplib.FTP()
                try:ftp.connect('10.17.1.18',timeout=7);ftp.login('anonymous','https-starfall-acceptance');break
                except (OSError,EOFError,ftplib.error_temp):
                    ftp.close()
                    if time.monotonic()>deadline:raise
                    time.sleep(1)
            try:
                with ftp:return fn(ftp)
            finally:command('ftp off',6,'ftp: off\r\n')
        def boot():
            output=command('reboot',.25);started=time.monotonic();notice=started+10
            while time.monotonic()-started<110:
                if 'Appletini control UART' in output:
                    value=command('status',.4)
                    if '---- card status ----' in value:
                        # USB enumeration can still occupy CPU0 just after the
                        # first prompt. Wait for an actual HTTP status reply.
                        for attempt in range(20):
                            ready=command('http status',1)
                            if '[http-status]' in ready:return value+ready
                        raise RuntimeError('HTTP status did not answer after frontend boot')
                else:
                    value=uart.read(max(1,uart.in_waiting)).decode('utf-8',errors='replace')
                    if value:
                        output+=value;logs.append(value);(OUT/'uart.txt').write_text(''.join(logs),encoding='utf-8')
                    else:time.sleep(.02)
                if time.monotonic()>notice:print('Waiting for firmware/frontend.',flush=True);notice=time.monotonic()+10
            raise RuntimeError('Frontend boot deadline')
        def optional(ftp,path):
            try:return get(ftp,path)
            except ftplib.error_perm as e:
                if not str(e).startswith('550'):raise
                return None
        def setting(profile,key,value):
            pattern=r'(?m)^'+re.escape(key)+r'=[^\r\n]*'
            if re.search(pattern,profile):return re.sub(pattern,key+'='+value,profile)
            return profile.rstrip()+'\n'+key+'='+value+'\n'
        def dump(address,n):
            value=command(f'vtw dump {address:X} {n:X}',.3);data={}
            for m in re.finditer(r'(?m)^([0-9A-F]{5}):((?: [0-9A-F]{2})+)',value.replace('\r','')):
                for i,b in enumerate(m[2].split()):data[int(m[1],16)+i]=int(b,16)
            assert all(address+i in data for i in range(n)),value
            return bytes(data[address+i] for i in range(n))
        def poke(address,data):
            command(f'mwr 0x400001C4 0x{address:X}',.03)
            for b in data:command(f'mwr 0x400001C8 {b}',.03)
        def wait_byte(address,wanted,seconds=20):
            until=time.monotonic()+seconds
            while time.monotonic()<until:
                if dump(address,1)[0]==wanted:return
                time.sleep(.15)
            raise RuntimeError(f'RAM {address:04X} did not reach {wanted}')
        def request(url):
            print('HTTPS',url,flush=True)
            text=command('http get '+url,35,'[http] done\r\n')
            rows=[{k:int(v,16 if v.startswith('0x') else 10) for k,v in re.findall(r'(\w+)=(-?0x[0-9A-Fa-f]+|-?\d+)',l)}
                  for l in text.splitlines() if l.startswith('[http-result]')]
            assert rows,text[-1200:];row=rows[-1];row['url']=url;report['requests'].append(row);save()
            assert row['rc']==0 and row['verify']==0 and row['status']==200,row
            print('PASS',row['body'],'bytes; max step',row['step_max_us'],'us',flush=True)
        try:
            before=command('status',1);report['before']=before+command('vtw status',.8)
            match=re.search(r'rtc: (\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)',before);assert match
            difference=(datetime.strptime(match[1],'%Y-%m-%d %H:%M:%S').replace(tzinfo=timezone.utc).timestamp()-time.time())/60
            offset=round(difference/15)*15;assert abs(difference-offset)<2 and abs(offset)<=840
            def stage(ftp):
                assert sha(get(ftp,'/FIRMWARE.OK'))==STOCK_HASH,'Unexpected starting firmware'
                assert optional(ftp,'/FIRMWARE.BIN') is None,'Pending update exists'
                profile=get(ftp,'/appletini_cfg.txt');(OUT/'original-settings.bin').write_bytes(profile)
                for name in ('ATNET.CFG','ATNETCA.PEM'):
                    data=optional(ftp,'/'+name);journal['public_backup'][name]=None if data is None else data.hex()
                names={Path(p).name.upper() for p in ftp.nlst('/')}
                seeds={'ATNETA.SED','ATNETB.SED'}&names;assert not seeds or len(seeds)==2,'Partial seed pair'
                assert 'HTTPTEST1' not in names,'Test directory already exists'
                journal['seed_existing']=bool(seeds);save()
                if not seeds:
                    journal['seed_created']=True;save();record=seed_record(os.urandom(48))
                    # Stock first-use maintenance writes only. Do not RETR seeds.
                    for name in ('ATNETA.SED','ATNETB.SED'):ftp.storbinary('STOR /'+name,io.BytesIO(record))
                    del record
                put(ftp,'/ATNETCA.PEM',(inputs/'ATNETCA.PEM').read_bytes())
                put(ftp,'/ATNET.CFG',b'N\x01'+bytes(4)+int(offset).to_bytes(2,'little',signed=True))
                ftp.mkd('/HTTPTEST1');journal['directory_created']=True
                for name in ('STARFALL.PO','NETCHECK.PO'):put(ftp,'/HTTPTEST1/'+name,(inputs/name).read_bytes())
                text=profile.decode();text=setting(text,'ethernet.dns','8.8.8.8')
                text=setting(text,'smartport.disk.1.path','"0:/HTTPTEST1/STARFALL.PO"')
                text=setting(text,'smartport.disk.1.enabled','ON');text=setting(text,'vtw.speed.mode','3')
                journal['test_profile']=text;save();put(ftp,'/appletini_cfg.txt',text.encode())
                put(ftp,'/FIRMWARE.BIN',(inputs/'FIRMWARE.BIN').read_bytes());journal['firmware_loaded']=True;save()
            if args.replace_inputs:
                previous=report['inputs'];stamp=time.strftime('%Y%m%dT%H%M%S')
                (OUT/('result-before-'+stamp+'.json')).write_text(json.dumps(report,indent=2)+'\n')
                def replace(ftp):
                    assert sha(get(ftp,'/FIRMWARE.OK'))==previous['FIRMWARE.BIN'],'Unexpected installed image'
                    assert optional(ftp,'/FIRMWARE.BIN') is None,'Pending update exists'
                    for name in ('STARFALL.PO','NETCHECK.PO'):put(ftp,'/HTTPTEST1/'+name,(inputs/name).read_bytes())
                    put(ftp,'/FIRMWARE.BIN',(inputs/'FIRMWARE.BIN').read_bytes())
                transfer(replace);report['loaded']=boot()
                report['inputs']=manifest['inputs'];report['requests']=[];report['sdk']=[];save()
            if not args.resume:
                transfer(stage);print('First-use state, public roots, firmware and game staged.',flush=True)
                report['loaded']=boot()
            for attempt in range(20):
                report['prepared']=command('http status',1)
                if '[http-status]' in report['prepared']:break
            assert 'crypto=1 trust=1' in report['prepared'],report['prepared']
            for url in ('https://appletini.org/leaderboards/starfall/api/ladder','https://rsa2048.badssl.com/','https://rsa4096.badssl.com/'):
                if not any(r['url']==url and r['rc']==0 for r in report['requests']):request(url)
            report['starfall_title']=command('status',.8)
            # Select the copied diagnostic disk; keep the same production image.
            def select(ftp,path):
                profile=setting(journal['test_profile'],'smartport.disk.1.path','"'+path+'"')
                put(ftp,'/appletini_cfg.txt',profile.encode())
            transfer(lambda ftp:select(ftp,'0:/HTTPTEST1/NETCHECK.PO'));boot()
            labels=manifest['netcheck_labels']
            cases=[('echo','POST','http://10.17.1.17:8080/anything',1,18,0),
                   ('max-string','POST','http://10.17.1.17:8080/json32',1,json32('')[1],0),
                   ('max-nodes','POST','http://10.17.1.17:8080/json32-nodes',1,json32('nodes')[1],0),
                   ('max-utf8','POST','http://10.17.1.17:8080/json32-utf8',1,json32('utf8')[1],0),
                   ('stream32k','GET','http://10.17.1.17:8080/json32',0,32768,0),
                   ('invalid-json','POST','http://10.17.1.17:8080/invalid-json',1,0,10),
                   ('bad-hostname','GET','https://wrong.host.badssl.com/',0,0,9)]
            for speed in ('full','turbo'):
                command('vtw speed '+speed,.5);report.setdefault('speeds',{})[speed]=command('vtw status',.5)
                for name,method,url,retain,size,error in cases:
                    previous=[r for r in report['sdk'] if r['speed']==speed and r['case']==name]
                    if previous:
                        r=previous[-1]
                        if (not error and r['phase']==4 and r['error']==0 and r['bytes']==size) or (error and r['error']==error and (name!='bad-hostname' or 'result=-9984 verify=0x00000004' in r['status'])):
                            continue
                    print('SDK',speed,name,flush=True)
                    poke(labels['_netcheck_url'],url.encode()+b'\0')
                    poke(labels['_netcheck_method'],method.encode()+b'\0')
                    poke(labels['_netcheck_retain'],bytes((retain,)))
                    poke(labels['_netcheck_go'],b'\1')
                    deadline=time.monotonic()+45
                    while time.monotonic()<deadline:
                        phase=dump(labels['_netcheck_phase'],1)[0];err=dump(labels['_netcheck_error'],1)[0]
                        if phase==4 or err:break
                        time.sleep(.1)
                    row={'speed':speed,'case':name,'phase':phase,'error':err,
                         'http':int.from_bytes(dump(labels['_netcheck_http'],2),'little'),
                         'bytes':int.from_bytes(dump(labels['_netcheck_bytes'],2),'little'),
                         'result':int.from_bytes(dump(labels['_netcheck_result'],2),'little',signed=True),
                         'status':command('http status',.4)}
                    report['sdk'].append(row);save()
                    assert err==error,row
                    if not error:assert phase==4 and row['http']==200 and row['bytes']==size,row
                    else:
                        assert phase in (2,3),row
                        if name=='bad-hostname':assert 'result=-9984 verify=0x00000004' in row['status'],row
            transfer(lambda ftp:select(ftp,'0:/HTTPTEST1/STARFALL.PO'));report['final_boot']=boot()
            report['pass']=True;report['left_ready']=True;save()
            print('PASS normal firmware, public roots, RSA, SDK 33MHz/TURBO; STARFALL ready for next checks.',flush=True)
        except Exception as e:
            report['error']=str(e);save();print('Acceptance stopped; original-state journal retained.',flush=True);raise
if __name__=='__main__':main()
