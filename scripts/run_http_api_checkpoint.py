"""Run the real libtini HTTPS POST/JSON client, then restore the exact board state.

Inputs are copied artifacts under build/http-api-checkpoint/inputs. No sibling
checkout is a runtime dependency. Test secrets are generated in memory only.
"""
from datetime import datetime, timezone
import argparse
import ftplib
import hashlib
import json
import os
from pathlib import Path
import re
import serial
import time
from smartport_timing_board import serial_command, get, put, STOCK_HASH

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'build/http-api-checkpoint'
sha=lambda b:hashlib.sha256(b).hexdigest()

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--settle',type=float,default=0,
                        help='extra wait after the first working frontend status; default 0 seconds')
    parser.add_argument('--prime-socket0',action='store_true')
    parser.add_argument('--require-dns',action='store_true')
    parser.add_argument('--check-network-deadline',action='store_true')
    parser.add_argument('--reboot-provisioned',action='store_true',
                        help='reboot once after first-use setup, then test normal startup with persisted seed/trust')
    args=parser.parse_args()
    if args.settle<0:parser.error('--settle must be nonnegative')
    inputs=OUT/'inputs'
    manifest=json.loads((inputs/'manifest.json').read_text())
    labels=manifest['labels']
    for name,digest in manifest['inputs'].items():
        assert sha((inputs/name).read_bytes())==digest,name
    stock=(ROOT/'firmwares/firmware-F1.3.3b3/FIRMWARE.BIN').read_bytes()
    assert sha(stock)==STOCK_HASH
    report={'inputs':manifest['inputs'],'fpga_rebuilt':False,'golden_written':False,'pass':False}
    log=[];original=None;backup={};owned=False;updated=False;seed_created=False;last_boot_ready=0
    assert not (OUT/'original-settings.bin').exists(),'A prior checkpoint needs recovery first'
    with serial.Serial('COM13',921600,timeout=.01) as uart:
        def command(text,timeout=.5,secret=False,until=None):
            if until is None:
                value=serial_command(uart,text,timeout=timeout)
            else:
                wire=(':'+text+'\r\n').encode('ascii')
                for at in range(0,len(wire),8):
                    uart.write(wire[at:at+8]);uart.flush();time.sleep(.01)
                data=bytearray();deadline=time.monotonic()+timeout
                while time.monotonic()<deadline:
                    data.extend(uart.read(max(1,uart.in_waiting)))
                    if until.replace('\r','').encode('ascii') in data.replace(b'\r',b''):break
                value=data.decode('utf-8',errors='replace')
            if not secret:
                log.append(value)
                (OUT/'uart.txt').write_text(''.join(log),encoding='utf-8',newline='\n')
            return value
        def transfer(callback):
            command('ftp on',5,until='ftp: on (anonymous read/write, passive)\r\n')
            try:
                # Reboot can leave a switch port non-forwarding for about 30s.
                # Retry only connection/login; never replay a mutating callback.
                until=time.monotonic()+50
                while True:
                    ftp=ftplib.FTP()
                    try:
                        ftp.connect('10.17.1.18',timeout=8);ftp.login('anonymous','http-api-checkpoint')
                        break
                    except (OSError,EOFError,ftplib.error_temp):
                        ftp.close()
                        if time.monotonic()>=until:raise
                        time.sleep(1)
                with ftp:
                    return callback(ftp)
            finally:command('ftp off',5,until='ftp: off\r\n')
        def optional(ftp,path):
            try:return get(ftp,path)
            except ftplib.error_perm as e:
                if not str(e).startswith('550'):raise
                return None
        def boot():
            nonlocal last_boot_ready
            started=time.monotonic();value=command('reboot',.25)
            output=value;next_notice=started+10
            while time.monotonic()-started<110:
                # Wait for the frontend UART, without sending keys into Golden.
                if 'Appletini control UART' not in output:
                    value=uart.read(max(1,uart.in_waiting)).decode('utf-8',errors='replace')
                    if value:
                        output+=value;log.append(value)
                        (OUT/'uart.txt').write_text(''.join(log),encoding='utf-8',newline='\n')
                    else:time.sleep(.02)
                else:
                    value=command('status',.4)
                    if '---- card status ----' in value:
                        last_boot_ready=time.monotonic()
                        report.setdefault('boots',[]).append({'frontend_ready_seconds':last_boot_ready-started})
                        return value
                    time.sleep(.1)
                if time.monotonic()>=next_notice:
                    print('Waiting for firmware updater and frontend UART.',flush=True)
                    next_notice=time.monotonic()+10
            raise RuntimeError('Boot deadline')
        def dump(address,size):
            value=command(f'vtw dump {address:X} {size:X}',.8);data={}
            for match in re.finditer(r'(?m)^([0-9A-F]{5}):((?: [0-9A-F]{2})+)',value.replace('\r','')):
                for n,byte in enumerate(match[2].split()):data[int(match[1],16)+n]=int(byte,16)
            assert all(address+i in data for i in range(size)),value
            return bytes(data[address+i] for i in range(size))
        try:
            status=command('status',1);report['before']=status+command('vtw status',1)
            match=re.search(r'rtc: (\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)',status)
            assert match,'RTC is not set'
            difference=(datetime.strptime(match[1],'%Y-%m-%d %H:%M:%S').replace(tzinfo=timezone.utc).timestamp()-time.time())/60
            offset=round(difference/15)*15
            assert abs(difference-offset)<2 and abs(offset)<=840,'RTC needs correction'
            def stage(ftp):
                nonlocal original,owned,updated
                assert sha(get(ftp,'/FIRMWARE.OK'))==STOCK_HASH
                assert optional(ftp,'/FIRMWARE.BIN') is None,'Pending update exists'
                original=get(ftp,'/appletini_cfg.txt');(OUT/'original-settings.bin').write_bytes(original)
                for name in ('ATNET.CFG','ATNETCA.PEM'):
                    backup[name]=optional(ftp,'/'+name)
                (OUT/'original-network-files.json').write_text(json.dumps(
                    {name:None if data is None else data.hex() for name,data in backup.items()},indent=2)+'\n')
                names={Path(p).name.upper() for p in ftp.nlst('/')}
                report['seed_existing']={'ATNETA.SED','ATNETB.SED'}.issubset(names)
                assert report['seed_existing'] or not ({'ATNETA.SED','ATNETB.SED'}&names),'Partial seed state'
                ftp.mkd('/HTTPAPI');owned=True
                put(ftp,'/HTTPAPI/NETCHECK.PO',(inputs/'NETCHECK.PO').read_bytes())
                put(ftp,'/ATNETCA.PEM',(ROOT/'build/http-service-checkpoint/ATNETCA.PEM').read_bytes())
                cfg=b'N\x01'+bytes((10,17,1,17))+int(offset).to_bytes(2,'little',signed=True)
                put(ftp,'/ATNET.CFG',cfg)
                profile=original.decode()
                for name,value in {'smartport.disk.1.path':'"0:/HTTPAPI/NETCHECK.PO"','smartport.disk.1.enabled':'ON','vtw.speed.mode':'3'}.items():
                    profile,count=re.subn(r'(?m)^'+re.escape(name)+r'=[^\r\n]*',name+'='+value,profile);assert count==1
                put(ftp,'/appletini_cfg.txt',profile.encode())
                updated=True;put(ftp,'/FIRMWARE.BIN',(inputs/'FIRMWARE.BIN').read_bytes())
            transfer(stage);print('Checked SD firmware and test disk; loading HTTPS API firmware.',flush=True)
            report['loaded']=boot()
            if not report['seed_existing']:
                seed_created=True;report['test_seed_owned']=True
                result=command('http provision '+os.urandom(48).hex(),5,secret=True,
                               until='[http] provision rc=0\r\n')
                assert 'provision rc=0' in result,'Seed provisioning failed (secret transcript withheld)'
            report['provisioned_reboot']=args.reboot_provisioned
            if args.reboot_provisioned:
                print('First-use setup complete; rebooting to test normal persisted-seed startup.',flush=True)
                report['setup_boot']=report['loaded'];report['loaded']=boot()
            report['settle_seconds']=args.settle
            if args.settle:
                print(f'Waiting {args.settle:g}s to isolate the cold-boot network window.',flush=True)
                time.sleep(args.settle)
            print('First-use seed and test trust are ready; starting DNS/HTTPS.',flush=True)
            command('http trace on',.2)
            # Distinguish PC DNS routing from the Apple transport and TLS.
            report['dns_start_seconds_after_frontend_ready']=time.monotonic()-last_boot_ready
            command('http get https://checkpoint.appletini.test:8443/short',.3)
            for _ in range(35):
                time.sleep(1);value=command('http status',.5)
                if 'state=10 ' in value or 'state=11 ' in value:break
            report['controlled_dns']=value;print(value.strip(),flush=True)
            report['dns_registers']=command('http net',1);print(report['dns_registers'].strip(),flush=True)
            report['network']=report['dns_registers']
            if args.require_dns:assert 'state=10 ' in value,report['controlled_dns']
            if args.require_dns:
                command('http get https://checkpoint.appletini.test:8443/short',1)
                for _ in range(35):
                    time.sleep(1);value=command('http status',.5)
                    if 'state=10 ' in value or 'state=11 ' in value:break
                report['warm_controlled_dns']=value;print('Warm repeat: '+value.strip(),flush=True)
                assert 'state=10 ' in value,value
            if args.prime_socket0:
                command('ftp on',1);command('ftp off',1)
                command('http get https://checkpoint.appletini.test:8443/short',1)
                for _ in range(12):
                    time.sleep(1);value=command('http status',.5)
                    if 'state=10 ' in value or 'state=11 ' in value:break
                report['dns_after_socket0_listen']=value;print('After socket-0 LISTEN/CLOSE: '+value.strip(),flush=True)
            command(f'http config 8.8.8.8 {offset}',1)
            command('http get https://www.cloudflare.com/favicon.ico',1)
            for _ in range(30):
                time.sleep(1);value=command('http status',.5)
                if 'state=10 ' in value or 'state=11 ' in value:break
            report['public_dns_https']=value;print(value.strip(),flush=True)
            if args.require_dns:assert 'state=10 ' in value,report['public_dns_https']
            command(f'http config 10.17.1.17 {offset}',1)
            if args.check_network_deadline:
                command('http get https://noanswer.appletini.test:8443/short',1)
                for _ in range(35):
                    time.sleep(1);value=command('http status',.5)
                    if 'state=10 ' in value or 'state=11 ' in value:break
                report['network_deadline']=value;print('Lost DNS replies: '+value.strip(),flush=True)
                assert 'state=11 ' in value and 'result=-111 ' in value,value
            assert dump(labels['_netcheck_phase'],1)==b'\0','Client did not wait at its start gate'
            command(f'mwr 0x400001C4 0x{labels["_netcheck_go"]:X}')
            command('mwr 0x400001C8 1')
            for attempt in range(45):
                time.sleep(1)
                at=labels['_netcheck_phase'];raw=dump(at,8)
                phase=raw[0];error=raw[labels['_netcheck_error']-at]
                if attempt%5==0:print(f'Apple client phase={phase} error={error}',flush=True)
                if phase==4 or error:break
            raw=dump(labels['_netcheck_phase'],8)
            report['client']={name:int.from_bytes(raw[labels[name]-labels['_netcheck_phase']:labels[name]-labels['_netcheck_phase']+size],'little')
                for name,size in (('_netcheck_phase',1),('_netcheck_error',1),('_netcheck_http',2),('_netcheck_bytes',2))}
            report['http_status']=command('http status',1)
            report['network_after']=command('http net',1)
            assert report['client']=={'_netcheck_phase':4,'_netcheck_error':0,'_netcheck_http':200,'_netcheck_bytes':18},report['client']
            report['pass']=True;print('PASS real Apple HTTPS POST, retained body and JSON Pointer via libtini.',flush=True)
        except Exception as e:
            report['error']=str(e);raise
        finally:
            try:
                if seed_created:
                    assert 'erase-test-provisioning rc=0' in command('http erase-test-provisioning',3)
                if original is not None:
                    def restore(ftp):
                        put(ftp,'/appletini_cfg.txt',original)
                        for name,data in backup.items():
                            if data is None:
                                if optional(ftp,'/'+name) is not None:ftp.delete('/'+name)
                            else:put(ftp,'/'+name,data)
                        if owned:
                            ftp.delete('/HTTPAPI/NETCHECK.PO');ftp.rmd('/HTTPAPI')
                        if updated:put(ftp,'/FIRMWARE.BIN',stock)
                    transfer(restore)
                    print('Restoring stock firmware and original settings.',flush=True)
                    boot()
                    def verify(ftp):
                        assert sha(get(ftp,'/FIRMWARE.OK'))==STOCK_HASH
                        assert get(ftp,'/appletini_cfg.txt')==original
                    transfer(verify);boot();report['restored']=True
            finally:
                (OUT/'result.json').write_text(json.dumps(report,indent=2)+'\n',newline='\n')
    print('Stock firmware, original media/settings and FTP-off state restored.',flush=True)

if __name__=='__main__':main()
