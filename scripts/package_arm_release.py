"""Package an ARM release from explicit retained inputs and test evidence."""
from pathlib import Path
import argparse,hashlib,json,re,shutil,subprocess,sys,zipfile
ROOT=Path(__file__).resolve().parents[1]
sha=lambda p:hashlib.sha256(Path(p).read_bytes()).hexdigest()
def main():
    parser=argparse.ArgumentParser();parser.add_argument('--version',required=True)
    parser.add_argument('--baseline',type=Path,required=True)
    parser.add_argument('--build-inputs',type=Path,required=True,
                        help='retained directory containing FIRMWARE.BIN, report and ELF inputs')
    parser.add_argument('--build-report',type=Path,
                        help='source report; default ARM_BUILD.json or build-report.json in --build-inputs')
    parser.add_argument('--stress-report',type=Path,required=True,help='retained completed RSA stress result JSON')
    parser.add_argument('--platform-xsa',type=Path,required=True,help='explicit matching platform XSA used by the build')
    args=parser.parse_args()
    assert re.fullmatch(r'F\d+\.\d+\.\d+',args.version)
    out=ROOT/'firmwares'/('firmware-'+args.version);assert not out.exists(),'Release directory already exists'
    build=args.build_inputs.resolve()
    report_path=args.build_report or build/('ARM_BUILD.json' if (build/'ARM_BUILD.json').is_file() else 'build-report.json')
    report=json.loads(report_path.read_text())
    elf_inputs=build/'inputs' if (build/'inputs').is_dir() else build
    assert report['application'] and not report['diagnostics'] and not report['fpga_rebuilt']
    assert report['package']['bitstream_sha256']=='94c578698466ac057d5a58768e7b1c98fd680dca38a7840cd82f1d226f6dfd34'
    for name,value in report['sources'].items():assert sha(ROOT/name)==value,('Stale ARM build',name)
    assert report['package']['sha256']==sha(build/'FIRMWARE.BIN')
    stress=json.loads(args.stress_report.read_text())
    assert stress['pass'] and stress['restored'] and len(stress['cases'])==2
    out.mkdir(parents=True);inputs=out/'inputs';inputs.mkdir()
    for name in ('fsbl.elf','appletini_yarz_top.bit','appletini_yarz_top.xsa'):
        shutil.copy2(args.baseline/'inputs'/name,inputs/name)
    for name in ('frontend.elf','frontend_core1.elf','core1_blob.bin'):shutil.copy2(elf_inputs/name,inputs/name)
    shutil.copy2(build/'FIRMWARE.BIN',out/'FIRMWARE.BIN')
    shutil.copy2(report_path,out/'ARM_BUILD.json')
    shutil.copy2(args.baseline/'TIMING_MANIFEST.txt',out/'TIMING_MANIFEST.txt')
    subprocess.run([sys.executable,str(ROOT/'scripts/audit_firmware_release.py'),
        '--firmware',str(out/'FIRMWARE.BIN'),'--fsbl',str(inputs/'fsbl.elf'),'--frontend',str(inputs/'frontend.elf'),
        '--bitstream',str(inputs/'appletini_yarz_top.bit'),'--xsa',str(inputs/'appletini_yarz_top.xsa'),
        '--platform-xsa',str(args.platform_xsa),
        '--run-manifest',str(out/'TIMING_MANIFEST.txt'),'--core1-elf',str(inputs/'frontend_core1.elf'),
        '--core1-blob',str(inputs/'core1_blob.bin'),'--version',args.version,'--boot-version','B2.0.6',
        '--output',str(out/'FIRMWARE_AUDIT.json')],check=True,cwd=ROOT)
    public={
      'setup_https.py':ROOT/'scripts/setup_https.py','ATNETCA.PEM':ROOT/'software/https_setup/ATNETCA.PEM',
      'roots.json':ROOT/'software/https_setup/roots.json','README.md':ROOT/'software/https_setup/README.md',
      'LICENSE-roots.txt':ROOT/'third_party/mozilla-ca/LICENSE','LICENSE-mbedtls.txt':ROOT/'third_party/mbedtls/LICENSE',
      'MBEDTLS-CHANGES.md':ROOT/'third_party/mbedtls/APPLETINI_PATCHES.md'}
    setup=out/'HTTPS-SETUP';setup.mkdir()
    for name,p in public.items():shutil.copy2(p,setup/name)
    (setup/'INSTALL.txt').write_text('''HTTPS first use: run once per board from this folder.
python setup_https.py E:\\ --roots ATNETCA.PEM --utc-offset 0
Use the actual SD drive and the offset of its RTC (minutes east of UTC).
For an RTC set to UTC+3 use 180. Set the RTC date/time in the menu.
Use Ethernet menu/DHCP DNS, or give setup --dns with an explicit server.
Keep the two private seed files with this board; do not clone or roll back them.
Existing initialized cards keep their seeds. This package contains no private seed.
Copy FIRMWARE.BIN to the SD root and reboot through the normal updater.
''',newline='\n')
    for zip_name,include_firmware in [('HTTPS-SETUP.zip',False),('Appletini-'+args.version+'.zip',True)]:
        with zipfile.ZipFile(out/zip_name,'w',zipfile.ZIP_DEFLATED) as archive:
            for p in sorted(setup.iterdir()):archive.write(p,p.name)
            if include_firmware:archive.write(out/'FIRMWARE.BIN','FIRMWARE.BIN')
        with zipfile.ZipFile(out/zip_name) as archive:assert archive.testzip() is None
    (out/'SHA256SUMS.txt').write_text(''.join(sha(out/n)+'  '+n+'\n' for n in
        ('FIRMWARE.BIN','HTTPS-SETUP.zip','Appletini-'+args.version+'.zip')),newline='\n')
    (out/'SOURCE_COMMIT.txt').write_text('PENDING REVIEWED COMMIT\n')
    print('PASS release image, explicit FPGA/XSA, ELF loads, embedded CPU1, memory and setup package:',out)
if __name__=='__main__':main()
