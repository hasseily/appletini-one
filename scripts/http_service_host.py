#!/usr/bin/env python3
"""Provision once, then test the standalone HTTP backend without per-run seeds/time."""
import argparse
from datetime import datetime, timedelta, timezone
import ftplib
import hashlib
import http.server
import io
import json
import os
from pathlib import Path
import re
import socket
import socketserver
import ssl
import sys
import threading
import time
import urllib.request

import serial
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, rsa
from cryptography.x509.oid import NameOID
import https_probe_host as probe

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/http-service-checkpoint"
JSON_BODY = b'{"message":"hello","rows":[{"name":"STARFALL","score":12345}]}'

def json32(kind):
    prefix=b'{"json":{"message":';suffix=b'}}'
    room=32768-len(prefix)-len(suffix)
    if kind=='nodes':
        count=(room-1)//2;value=b'['+b','.join([b'0']*count)+b']'
        expected=len(value)
    elif kind=='utf8':
        count=(room-2)//12;extra=room-2-count*12
        value=b'"'+b'\\ud83d\\ude80'*count+b'x'*extra+b'"';expected=4*count+extra
    else:value=b'"'+b'x'*(room-2)+b'"';expected=room-2
    result=prefix+value+b' '*(room-len(value))+suffix
    assert len(result)==32768
    return result,expected


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def reply(self, data, *, chunked=False, status=200, extra=()):
        self.send_response(status)
        self.send_header("Connection", "close")
        self.send_header("Transfer-Encoding" if chunked else "Content-Length",
                         "chunked" if chunked else str(len(data)))
        for key, value in extra:
            self.send_header(key, value)
        self.end_headers()
        if chunked:
            for at in range(0, len(data), 7):
                part = data[at:at+7]
                self.wfile.write(f"{len(part):x};test=1\r\n".encode()+part+b"\r\n")
            self.wfile.write(b"0\r\nX-Test: complete\r\n\r\n")
        else:
            for at in range(0, len(data), 256):
                self.wfile.write(data[at:at+256])
                self.wfile.flush()
                if self.path == "/probe":
                    time.sleep(.02)

    def do_GET(self):
        try:
            if self.path.startswith('/json32'):
                kind=self.path.removeprefix('/json32').strip('-')
                self.reply(json32(kind)[0],extra=(("Content-Type","application/json"),))
            elif self.path == "/invalid-json":
                self.reply(b'{"json":{"message":"\\ud800"}}',extra=(("Content-Type","application/json"),))
            elif self.path == "/redirect":
                self.reply(b"", status=302, extra=(("Location", "https://"+probe.NAME+":8443/short"),))
            elif self.path == "/relative":
                self.reply(b"", status=302, extra=(("Location", "/short"),))
            elif self.path == "/truncate":
                self.send_response(200)
                self.send_header("Content-Length", "999")
                self.send_header("Connection", "close")
                self.end_headers(); self.wfile.write(b"bad")
            elif self.path == "/delay":
                time.sleep(15); self.reply(JSON_BODY)
            elif self.path == "/timeout":
                time.sleep(35); self.reply(JSON_BODY)
            else:
                self.reply(probe.BODY if self.path == "/probe" else JSON_BODY,
                           chunked=self.path == "/chunked")
        except (ConnectionError, ssl.SSLError):
            pass

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        if self.path.startswith('/json32') or self.path=='/invalid-json':
            self.do_GET();return
        if self.path == "/anything":
            self.reply(json.dumps({"json": json.loads(body)}).encode(),
                       extra=(("Content-Type", "application/json"),))
            return
        self.reply(body)

    def log_message(self, fmt, *args):
        print("[peer]", self.client_address[0], fmt % args, flush=True)


class DNS(socketserver.ThreadingUDPServer):
    allow_reuse_address = True


class DNSHandler(socketserver.BaseRequestHandler):
    def handle(self):
        data, sock = self.request
        if len(data) < 12 or data[4:6] != b"\0\1":
            return
        at, labels = 12, []
        try:
            while data[at]:
                n = data[at]; at += 1
                if n > 63: return
                labels.append(data[at:at+n].decode("ascii")); at += n
            at += 5
            name = ".".join(labels).lower()
            print("[dns]", self.client_address, name, flush=True)
            if name == 'noanswer.appletini.test':
                return  # Controlled lost-UDP fixture; exercise the request deadline.
            if name.endswith(".appletini.test"):
                address = self.server.bind_ip
            else:
                address = socket.gethostbyname(name)
            reply = data[:2] + b"\x81\x80\0\1\0\1\0\0\0\0" + data[12:at]
            reply += b"\xc0\x0c\0\1\0\1\0\0\0\x3c\0\4" + socket.inet_aton(address)
            sock.sendto(reply, self.client_address)
        except (IndexError, UnicodeError, OSError):
            pass


def prepare():
    OUT.mkdir(parents=True, exist_ok=True)
    probe.prepare()
    ca = x509.load_pem_x509_certificate((probe.OUT/"trusted-ca.pem").read_bytes())
    key = serialization.load_pem_private_key((probe.OUT/"trusted-ca-key.pem").read_bytes(),None)
    leaf_key = ec.generate_private_key(ec.SECP256R1())
    now = datetime.now(timezone.utc)
    leaf = (x509.CertificateBuilder().subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME,probe.NAME)]))
            .issuer_name(ca.subject).public_key(leaf_key.public_key()).serial_number(x509.random_serial_number())
            .not_valid_before(now-timedelta(days=60)).not_valid_after(now-timedelta(days=30))
            .add_extension(x509.BasicConstraints(ca=False,path_length=None),True)
            .add_extension(x509.SubjectAlternativeName([x509.DNSName(probe.NAME)]),False)
            .add_extension(x509.ExtendedKeyUsage([x509.oid.ExtendedKeyUsageOID.SERVER_AUTH]),False)
            .sign(key,hashes.SHA256()))
    (OUT/"expired.pem").write_bytes(leaf.public_bytes(serialization.Encoding.PEM))
    (OUT/"expired-key.pem").write_bytes(leaf_key.private_bytes(serialization.Encoding.PEM,serialization.PrivateFormat.PKCS8,serialization.NoEncryption()))
    rsa_key=rsa.generate_private_key(public_exponent=65537,key_size=2048)
    rsa_leaf=(x509.CertificateBuilder().subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME,probe.NAME)]))
        .issuer_name(ca.subject).public_key(rsa_key.public_key()).serial_number(x509.random_serial_number())
        .not_valid_before(now-timedelta(days=1)).not_valid_after(now+timedelta(days=30))
        .add_extension(x509.BasicConstraints(ca=False,path_length=None),True)
        .add_extension(x509.SubjectAlternativeName([x509.DNSName(probe.NAME)]),False)
        .add_extension(x509.ExtendedKeyUsage([x509.oid.ExtendedKeyUsageOID.SERVER_AUTH]),False)
        .sign(key,hashes.SHA256()))
    (OUT/'rsa.pem').write_bytes(rsa_leaf.public_bytes(serialization.Encoding.PEM))
    (OUT/'rsa-key.pem').write_bytes(rsa_key.private_bytes(serialization.Encoding.PEM,serialization.PrivateFormat.PKCS8,serialization.NoEncryption()))
    roots={(ROOT/"scripts/fixtures/http-public-root.pem").read_bytes()}
    roots.update(path.read_bytes() for path in OUT.glob('*-root.pem'))
    root=b"\n".join(sorted(roots))
    (OUT/"ATNETCA.PEM").write_bytes(root+b"\n"+(probe.OUT/"trusted-ca.pem").read_bytes())
    print("Prepared controlled certificates and public/test trust bundle; private keys remain in ignored build paths")


def serve(bind):
    servers=[]
    for label,port in (("trusted",8443),("untrusted",8444),("expired",8445),("rsa",8446)):
        ctx=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.minimum_version=ctx.maximum_version=ssl.TLSVersion.TLSv1_2
        ctx.set_ciphers("ECDHE-RSA-AES128-GCM-SHA256" if label=='rsa' else "ECDHE-ECDSA-AES128-GCM-SHA256")
        ctx.set_ecdh_curve("prime256v1")
        cert=OUT/f"{label}.pem" if label in ('expired','rsa') else probe.OUT/f"{label}-server.pem"
        key=OUT/f"{label}-key.pem" if label in ('expired','rsa') else probe.OUT/f"{label}-server-key.pem"
        ctx.load_cert_chain(cert,key)
        server=probe.TLSServer((bind,port),Handler); server.tls_context=ctx
        servers.append(server)
    servers.append(http.server.ThreadingHTTPServer((bind,8080),Handler))
    dns=DNS((bind,53),DNSHandler); dns.bind_ip=bind; servers.append(dns)
    for server in servers:
        threading.Thread(target=server.serve_forever,daemon=True).start()
    print("Serving HTTP, three TLS certificate cases, and controlled DNS on",bind,flush=True)
    try:
        while True: time.sleep(1)
    except KeyboardInterrupt:
        for server in servers: server.shutdown()


def provision(args):
    with serial.Serial(args.port,921600,timeout=.05) as uart:
        uart.reset_input_buffer()
        status=probe.serial_command(uart,"status",timeout=2)
        rtc=re.search(r"rtc: (\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)",status)
        if not rtc: raise RuntimeError("No valid board RTC in UART status")
        local=datetime.strptime(rtc[1],"%Y-%m-%d %H:%M:%S").replace(tzinfo=timezone.utc).timestamp()
        difference=(local-time.time())/60
        offset=round(difference/15)*15
        if abs(difference-offset)>2 or abs(offset)>840: raise RuntimeError("RTC needs correction before automatic UTC-offset setup")
        print("RTC offset (minutes):",offset)
        print(probe.serial_command(uart,"ftp on",timeout=1))
        try:
            with ftplib.FTP(args.board,timeout=30) as ftp:
                ftp.login("anonymous","http-checkpoint")
                names={p.upper().split('/')[-1] for p in ftp.nlst('/')}
                seeded={"ATNETA.SED","ATNETB.SED"}.issubset(names)
                if not seeded and {"ATNETA.SED","ATNETB.SED"}&names:
                    raise RuntimeError("Incomplete seed state: stop and recover deliberately")
                trust=(OUT/"ATNETCA.PEM").read_bytes()
                ftp.storbinary("STOR /ATNETCA.PEM",io.BytesIO(trust))
                received=io.BytesIO();ftp.retrbinary("RETR /ATNETCA.PEM",received.write)
                if received.getvalue()!=trust: raise RuntimeError("CA bundle readback mismatch")
        finally:
            print(probe.serial_command(uart,"ftp off",timeout=1))
        if not seeded:
            # Generated in memory, sent once through the owned UART, and never logged.
            text=probe.serial_command(uart,"http provision "+os.urandom(48).hex(),timeout=3)
            print(text)
            if "provision rc=0" not in text: raise RuntimeError("Seed provisioning failed")
        text=probe.serial_command(uart,f"http config {args.dns} {offset}",timeout=2)
        print(text)
        if "config rc=0" not in text: raise RuntimeError("Network configuration failed")
        text=probe.serial_command(uart,"http status",timeout=2); print(text)
        epoch=int(re.search(r"utc=(\d+)",text)[1])
        if abs(epoch-time.time())>120: raise RuntimeError("Board UTC does not match provisioning time")
    (OUT/"provision-report.json").write_text(json.dumps({"seed_existing":seeded,"per_request_seed":False,
        "rtc_offset_minutes":offset,"dns":args.dns,"trust_sha256":hashlib.sha256(trust).hexdigest()},indent=2)+"\n")


def run(args):
    negative={"hostname":4,"untrusted":8,"expired":1}
    urls={"valid":f"https://{probe.NAME}:8443/probe","hostname":f"https://wrong.appletini.test:8443/probe",
          "untrusted":f"https://{probe.NAME}:8444/probe","expired":f"https://{probe.NAME}:8445/probe",
          "http":f"http://{probe.NAME}:8080/short","chunked":f"https://{probe.NAME}:8443/chunked",
          "redirect":f"http://{probe.NAME}:8080/redirect","relative":f"https://{probe.NAME}:8443/relative",
          "post":f"https://{probe.NAME}:8443/post","truncate":f"https://{probe.NAME}:8443/truncate",
          "retain":f"https://{probe.NAME}:8443/short","retain-limit":f"https://{probe.NAME}:8443/probe",
          "rsa":f"https://{probe.NAME}:8446/short",
          "timeout":"http://10.17.1.17:8080/timeout"}
    url=args.url or urls[args.case]
    expected=probe.BODY if args.case=="valid" and not args.url else JSON_BODY
    if args.url:
        request=urllib.request.Request(url,headers={'User-Agent':'Appletini-HTTP/1','Accept-Encoding':'identity'})
        with urllib.request.urlopen(request,timeout=30) as response: expected=response.read()
    command=f"http post {url} {JSON_BODY.decode()}" if args.case=="post" else f"http get {url}"
    if args.case.startswith('retain'): command=f"http retain {url}"
    if args.bench: command=f"http bench {url}"
    with serial.Serial(args.port,921600,timeout=.05) as uart:
        uart.reset_input_buffer()
        text=probe.serial_command(uart,command,"[http-bench] done" if args.bench else "[http] done",50)
    print(text)
    result={}
    for line in text.splitlines():
        if line.startswith("[http-result]"):
            result={k:int(v,16 if v.startswith('0x') else 10) for k,v in re.findall(r"(\w+)=(-?0x[0-9A-Fa-f]+|-?\d+)",line)}
    digest=re.search(r"\[http-sha256\] ([a-f0-9]{64})",text)
    if args.case in negative:
        passed=result.get('rc',0)!=0 and result.get('verify',0)!=0xFFFFFFFF and result.get('verify',0)&negative[args.case]!=0 and result.get('body')==0
    elif args.case=="truncate": passed=result.get('rc',0)!=0
    elif args.case=="retain-limit": passed=result.get('rc')==-2
    elif args.case=="timeout": passed=result.get('rc')==-107
    else: passed=result.get('rc')==0 and result.get('verify')==0 and result.get('status')==200 and result.get('body')==len(expected) and digest and digest[1]==hashlib.sha256(expected).hexdigest()
    stamp=datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    stem=OUT/f'{stamp}-{args.case}'
    stem.with_suffix('.txt').write_text(text,encoding='utf-8')
    phases=[]
    for line in text.splitlines():
        if line.startswith('[http-phase]'):
            phase={k:int(v) for k,v in re.findall(r'(\w+)=(\d+)',line)}
            phase['name']=re.search(r'name=(\w+)',line)[1]; phases.append(phase)
    if args.bench:
        passed=passed and len(phases)==3 and all(p['health'] and p['video_frames'] and p['cache_misses'] and
            p['sd_reads'] and not any(p[k] for k in ('gaps','oversized','resync','hid_errors','xbox_errors','sd_errors','sd_crc_errors')) for p in phases)
        if args.require_workload: passed=passed and all(p['hid'] and p['xbox'] for p in phases)
    report={'url':url,'case':args.case,'result':result,'phases':phases,'require_workload':args.require_workload,
            'pass':bool(passed),'sha256':digest[1] if digest else None,
            'expected_sha256':hashlib.sha256(expected).hexdigest(),'per_request_seed':False,'per_request_time':False}
    stem.with_suffix('.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print('Report:',stem.with_suffix('.json'))
    if not passed: raise SystemExit('Standalone HTTP case failed')


def main():
    sys.stdout.reconfigure(encoding='utf-8',errors='backslashreplace')
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('mode',choices=('prepare','serve','provision','run'))
    p.add_argument('--board',default='10.17.1.18');p.add_argument('--bind',default='10.17.1.17')
    p.add_argument('--dns',default='10.17.1.17');p.add_argument('--port',default='COM13')
    p.add_argument('--case',default='valid',choices=('valid','hostname','untrusted','expired','http','chunked','redirect','relative','post','truncate','retain','retain-limit','timeout','rsa'))
    p.add_argument('--bench',action='store_true'); p.add_argument('--require-workload',action='store_true')
    p.add_argument('--url');a=p.parse_args();OUT.mkdir(parents=True,exist_ok=True)
    if a.mode=='prepare':prepare()
    elif a.mode=='serve':serve(a.bind)
    elif a.mode=='provision':provision(a)
    else:run(a)


if __name__=='__main__': main()
