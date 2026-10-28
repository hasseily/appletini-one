"""Assemble copied, self-contained hands-on test artifacts with exact hashes."""
from pathlib import Path
import argparse,hashlib,json,shutil,zipfile
ROOT=Path(__file__).resolve().parents[1]
def main():
    parser=argparse.ArgumentParser();parser.add_argument('--starfall',type=Path,required=True)
    parser.add_argument('--sdk-root',type=Path,required=True);parser.add_argument('--a2lynx',type=Path)
    parser.add_argument('--name',default='https-starfall-test2-20261028');args=parser.parse_args()
    assert Path(args.name).name==args.name
    out=ROOT/'deliverables'/args.name;out.mkdir(parents=True,exist_ok=False)
    sdk=args.sdk_root.resolve()
    files={'FIRMWARE.BIN':ROOT/'build/http-service-checkpoint/FIRMWARE.BIN',
           'STARFALL.po':args.starfall,'ATNETCA.PEM':ROOT/'software/https_setup/ATNETCA.PEM',
           'setup_https.py':ROOT/'scripts/setup_https.py','firmware-build.json':ROOT/'build/http-service-checkpoint/build-report.json',
           'roots.json':ROOT/'software/https_setup/roots.json'}
    files.update({'LICENSE-mbedtls.txt':ROOT/'third_party/mbedtls/LICENSE',
                  'LICENSE-roots.txt':ROOT/'third_party/mozilla-ca/LICENSE','LICENSE-libtini.txt':sdk/'LICENSE',
                  'STARFALL-assets.txt':sdk/'assets/starfall/README.md',
                  'STARFALL-art-license.txt':sdk/'assets/starfall/source/License.txt',
                  'STARFALL-music.txt':sdk/'assets/starfall/music/README.md'})
    if args.a2lynx:files['A2LYNX.po']=args.a2lynx
    for name,source in files.items():shutil.copyfile(source,out/name)
    report=json.loads((out/'firmware-build.json').read_text())
    assert report['application'] and not report['diagnostics'] and not report['fpga_rebuilt']
    assert report['package']['bitstream_sha256']=='94c578698466ac057d5a58768e7b1c98fd680dca38a7840cd82f1d226f6dfd34'
    (out/'README.txt').write_text('''Appletini HTTPS + STARFALL test 2 - 28 October 2026

These are development test files. Firmware uses the tested F1.3.3b3 FPGA
payload and a new ARM build. Golden is unchanged. No test CA is included.
The version display keeps the F1.3.3b3 base; SHA256SUMS.json identifies this build.

1. Once per board, prepare its SD card with Python 3 from this folder:
   python setup_https.py E:\\ --roots ATNETCA.PEM --utc-offset 180
   Replace E:\\ with the card root. 180 means the RTC is set to UTC+3.
   Use the offset that matches your RTC. Set a correct RTC date/time in the menu.
   The script keeps existing private seed files. Never clone or roll them back.
2. Set DNS in the Ethernet menu (8.8.8.8 works on the test LAN), or use DHCP.
   setup_https.py defaults to using that menu/DHCP DNS. --dns can override it.
3. Copy ATNETCA.PEM/ATNET.CFG if setup did not already write them to the SD root.
   Copy FIRMWARE.BIN to the SD root and restart through the normal updater.
4. Put STARFALL.po on the SD card and select it as a SmartPort disk. Start it
   at 33 MHz or TURBO with slot-4 audio and the usual RamWorks configuration.
5. On game over press N for online rankings. Type a public game name and
   press Enter to submit, Tab to read the top ten, or Esc to return.
   Q quits outside name entry. During entry it types Q.
   Reopening this panel retries the same run ID and name until a new game.
   The service is https://appletini.org/leaderboards/starfall.
   Card submissions identify platform=appletini; web submissions use web.

Game changes: a bomb pickup at the maximum fires immediately and preserves
the full reserve. A new boss is hidden until its first simulation step sets
its position and parts, fixing the stale Excalibur arrival frame.

Firmware changes: seed sync and public-root parsing run before Apple release.
RSA-authenticated ECDHE uses yielding ECC work. DNS/TCP setup retries share
the 30-second request deadline; HTTP/POST data is never replayed.
The board passes verified public HTTPS and 32 KiB JSON/streaming at both
33 MHz and TURBO. This build fixes a missing END after a stream drains.
The game panel submits platform=appletini and keeps one ID across retries.
The browser player submits platform=web through its own HTTPS client.

This remains a test build: RSA verification still has 24-28 ms work steps.
The 750 us scheduling budget applies between calls, not inside such a step.
The selected 26-root bundle does not cover every public issuer. Missing
roots, wrong hostnames, expired certificates and a wrong RTC fail closed.
After many DRBG calls, the service can request a reboot for seed renewal.
Use A2LYNX.po, if included, as a separate SmartPort boot disk for direct
HTTP/S text browsing. It displays at most 10 KiB per page and rejects
non-text content. JavaScript web applications do not run on the Apple II.

Please test sound, input, boss arrivals, automatic bombs and online rankings.
''',newline='\n')
    sums={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in out.iterdir() if p.is_file() and p.name!='SHA256SUMS.json'}
    (out/'SHA256SUMS.json').write_text(json.dumps(sums,indent=2)+'\n',newline='\n')
    zip_path=out.with_suffix('.zip')
    with zipfile.ZipFile(zip_path,'w',zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(out.iterdir()):
            if path.is_file():archive.write(path,out.name+'/'+path.name)
    print(out);print(zip_path)
if __name__=='__main__':main()
