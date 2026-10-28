"""Check A2LYNX's fallback on stock, then restore the exact hands-on image."""
from pathlib import Path
import hashlib,json,re,struct,sys,time
from board_http_acceptance import Board
from smartport_timing_board import get,put,STOCK_HASH
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/a2lynx-stock-fallback'
def main():
    sys.stdout.reconfigure(encoding='utf-8');OUT.mkdir(parents=True,exist_ok=True)
    candidate=ROOT/'build/https-starfall-acceptance/inputs/FIRMWARE.BIN';stock=ROOT/'firmwares/firmware-F1.3.3b3/FIRMWARE.BIN'
    assert hashlib.sha256(stock.read_bytes()).hexdigest()==STOCK_HASH
    expected=hashlib.sha256(candidate.read_bytes()).hexdigest();report={'pass':False,'stock_sha256':STOCK_HASH,'restore_sha256':expected}
    def save():(OUT/'result.json').write_text(json.dumps(report,indent=2)+'\n')
    original=None;changed=False
    with Board(OUT) as b:
        try:
            def install(ftp):
                nonlocal original,changed
                assert hashlib.sha256(get(ftp,'/FIRMWARE.OK')).hexdigest()==expected
                original=get(ftp,'/appletini_cfg.txt');save()
                profile=re.sub(r'(?m)^smartport.disk.1.path=[^\r\n]*','smartport.disk.1.path="0:/HTTPTEST1/A2LYNX.PO"',original.decode())
                changed=True;put(ftp,'/appletini_cfg.txt',profile.encode());put(ftp,'/FIRMWARE.BIN',stock.read_bytes())
            b.ftp(install);report['stock_boot']=b.reboot(require_http=False);print('Stock frontend booted; checking legacy program entry.',flush=True)
            payload=(ROOT/'build/a2lynx-acceptance/inputs/A2LEGACY.SYSTEM').read_bytes()
            if payload[:4]==bytes.fromhex('00051600'):
                entries=[struct.unpack_from('>III',payload,26+12*i) for i in range(struct.unpack_from('>H',payload,24)[0])]
                at,n=next((at,n) for kind,at,n in entries if kind==1);payload=payload[at:at+n]
            until=time.monotonic()+40;observed=b''
            while time.monotonic()<until:
                observed=b.dump(0x2000,16)
                if observed==payload[:16]:break
            report['expected_prefix']=payload[:16].hex();report['actual_prefix']=observed.hex();report['status']=b.command('vtw status',1)
            assert observed==payload[:16],'A2LEGACY.SYSTEM did not replace A2BROWSE.SYSTEM'
            report['pass']=True;save();print('PASS stock firmware: A2LYNX launched A2LEGACY.SYSTEM.',flush=True)
        except Exception as e:report['error']=str(e);save();raise
        finally:
            if changed:
                def restore(ftp):
                    report['stock_receipt_verified']=hashlib.sha256(get(ftp,'/FIRMWARE.OK')).hexdigest()==STOCK_HASH
                    put(ftp,'/appletini_cfg.txt',original);put(ftp,'/FIRMWARE.BIN',candidate.read_bytes())
                b.ftp(restore);report['restored']=b.reboot();save();print('Restored the exact hands-on firmware and STARFALL selection.',flush=True)
if __name__=='__main__':main()
