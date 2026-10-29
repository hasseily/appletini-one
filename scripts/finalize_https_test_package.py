"""Attach checked board evidence and notices to the hands-on test package."""
from pathlib import Path
import argparse,hashlib,json,shutil,zipfile
ROOT=Path(__file__).resolve().parents[1]
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--results',type=Path,required=True,
                        help='explicit retained hands-on results Markdown to include')
    args=parser.parse_args()
    results=args.results.resolve(strict=True)
    if not results.is_file():parser.error('--results must name a file')
    out=ROOT/'deliverables/https-starfall-test2-20261028'
    previous=json.loads((out/'SHA256SUMS.json').read_text())
    for name,digest in previous.items():assert hashlib.sha256((out/name).read_bytes()).hexdigest()==digest,name
    reports={'firmware-sdk.json':ROOT/'build/https-starfall-acceptance/result.json',
        'starfall-card.json':ROOT/'build/starfall-online-acceptance/result.json',
        'a2lynx.json':ROOT/'build/a2lynx-acceptance/result.json',
        'a2lynx-stock.json':ROOT/'build/a2lynx-stock-fallback/result.json',
        'board-ready.json':ROOT/'build/https-hands-on-ready/result.json'}
    for name,path in reports.items():assert json.loads(path.read_text())['pass'],name
    ready=json.loads(reports['board-ready.json'].read_text())
    assert ready['FIRMWARE.BIN']==hashlib.sha256((out/'FIRMWARE.BIN').read_bytes()).hexdigest()
    assert ready['STARFALL.PO']==hashlib.sha256((out/'STARFALL.po').read_bytes()).hexdigest()
    cleanup=json.loads((ROOT/'build/starfall-online-acceptance/public-cleanup.json').read_text())
    card=json.loads(reports['starfall-card.json'].read_text())
    assert cleanup[0]['results']==[{'run_id':card['run_id']}]
    evidence=ROOT/'validation/https-application-20261028';evidence.mkdir(parents=True,exist_ok=True)
    for name,path in reports.items():shutil.copy2(path,evidence/name)
    for name,path in {
      'inputs.json':ROOT/'build/https-starfall-acceptance/inputs/manifest.json',
      'public-card-cleanup.json':ROOT/'build/starfall-online-acceptance/public-cleanup.json',
      'firmware-build.json':ROOT/'build/http-service-checkpoint/build-report.json',
      'browser-web-local.png':ROOT/'build/starfall-online-acceptance/browser-web-local.png'}.items():shutil.copy2(path,evidence/name)
    for name,path in {
      'MBEDTLS-CHANGES.txt':ROOT/'third_party/mbedtls/APPLETINI_PATCHES.md',
      'LICENSE-IP65.txt':ROOT/'build/a2lynx-acceptance/inputs/LICENSE-IP65.txt',
      'LICENSE-W5100.txt':ROOT/'build/a2lynx-acceptance/inputs/LICENSE-W5100.txt',
      'RESULTS.md':results}.items():shutil.copy2(path,out/name)
    package_evidence=out/'evidence';package_evidence.mkdir(exist_ok=True)
    for path in evidence.iterdir():
        if path.is_file():shutil.copy2(path,package_evidence/path.name)
    sums={p.relative_to(out).as_posix():hashlib.sha256(p.read_bytes()).hexdigest()
          for p in sorted(out.rglob('*')) if p.is_file() and p.name!='SHA256SUMS.json'}
    (out/'SHA256SUMS.json').write_text(json.dumps(sums,indent=2)+'\n')
    with zipfile.ZipFile(out.with_suffix('.zip'),'w',zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(out.rglob('*')):
            if path.is_file():archive.write(path,out.name+'/'+path.relative_to(out).as_posix())
    with zipfile.ZipFile(out.with_suffix('.zip')) as archive:assert archive.testzip() is None
    print('PASS complete package hashes, board reports, licenses and ZIP integrity')
    print('Firmware:',ready['FIRMWARE.BIN']);print('STARFALL:',ready['STARFALL.PO'])
if __name__=='__main__':main()
