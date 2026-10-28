#!/usr/bin/env python3
"""Build the step-1 SmartPort test on the exact F1.3.3b3 FPGA/platform."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
from build_https_checkpoint import append_config, restore_default_config
from frontend_memory import audit_frontend_elf
from image_manifest import verify_image, ROLE_FIRMWARE

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'build/net-transport-checkpoint'
BASE=ROOT/'firmwares/firmware-F1.3.3b3'

def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    global OUT
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--restore-default-build',action='store_true')
    parser.add_argument('--timer-calibration',action='store_true')
    parser.add_argument('--output-dir',type=Path,default=OUT)
    args=parser.parse_args()
    OUT=args.output_dir.resolve()
    OUT.mkdir(parents=True,exist_ok=True)
    baseline_xsa=BASE/'inputs/appletini_yarz_top.xsa'
    platform_xsa=ROOT/'vitis_workspace/appletini_platform/hw/appletini_yarz_top.xsa'
    if digest(platform_xsa)!=digest(baseline_xsa):
        raise RuntimeError('Generated ARM platform differs from the selected stock XSA')
    bit=BASE/'inputs/appletini_yarz_top.bit'
    if digest(bit)!='94c578698466ac057d5a58768e7b1c98fd680dca38a7840cd82f1d226f6dfd34':
        raise RuntimeError('Selected stock FPGA hash changed')
    restore_default_config()
    config=ROOT/'vitis_workspace/frontend/src/UserConfig.cmake'
    config.write_text(''.join(line for line in config.read_text().splitlines(keepends=True)
                             if 'APPLETINI_NET_TRANSPORT_TEST=' not in line and
                                'APPLETINI_TIMER_CALIBRATION=' not in line))
    append_config('frontend','USER_COMPILE_SOURCES',['"../../../ps_sources/frontend/net_transport_test.c"'])
    if not args.restore_default_build:
        append_config('frontend','USER_COMPILE_DEFINITIONS',['APPLETINI_NET_TRANSPORT_TEST=1'])
        if args.timer_calibration:
            append_config('frontend','USER_COMPILE_DEFINITIONS',['APPLETINI_TIMER_CALIBRATION=1'])
    files=sorted((ROOT/'ps_sources').rglob('*.c'))+sorted((ROOT/'ps_sources').rglob('*.h'))
    sources={p.relative_to(ROOT).as_posix():digest(p) for p in files}
    commands=json.loads((ROOT/'vitis_workspace/frontend/build/compile_commands.json').read_text())
    match=re.match(r'^(?:"([^"]+)"|(\S+))',commands[0]['command'])
    compiler=Path(match.group(1) or match.group(2)); tools=compiler.parent
    env={k:v for k,v in os.environ.items() if k.lower()!='path'}
    env['PATH']=str(tools)+os.pathsep+next(v for k,v in os.environ.items() if k.lower()=='path')
    cache=(ROOT/'vitis_workspace/frontend/build/CMakeCache.txt').read_text()
    cmake=re.search(r'^CMAKE_COMMAND:INTERNAL=(.+)$',cache,re.M).group(1)
    def run(command):subprocess.run([str(x) for x in command],cwd=ROOT,env=env,check=True)
    run([cmake,'--build',ROOT/'vitis_workspace/frontend_core1/build','--parallel','8'])
    core=ROOT/'vitis_workspace/frontend_core1/build/frontend_core1.elf'
    blob=ROOT/'vitis_workspace/frontend/src/core1_blob.bin'
    run([tools/'arm-none-eabi-objcopy.exe','-O','binary',core,blob])
    run([cmake,'--build',ROOT/'vitis_workspace/frontend/build','--parallel','8'])
    if any(digest(ROOT/name)!=sha for name,sha in sources.items()):
        raise RuntimeError('ARM source changed while building')
    elf=ROOT/'vitis_workspace/frontend/build/frontend.elf'
    memory=audit_frontend_elf(elf)
    symbols=subprocess.check_output([str(tools/'arm-none-eabi-nm.exe'),str(elf)],env=env,text=True)
    if re.search(r'\bT (https_probe_command|http_service_start|http_bench_start)$',symbols,re.M):
        raise RuntimeError('Unrelated HTTP diagnostics remain enabled')
    flag='APPLETINI_NET_TRANSPORT_TEST=1' in config.read_text()
    if flag==args.restore_default_build:raise RuntimeError('Incorrect test flag')
    report={'test_enabled':flag,'timer_calibration_enabled':flag and args.timer_calibration,
            'fpga_rebuilt':False,'golden_written':False,
            'platform_xsa_sha256':digest(platform_xsa),'bitstream_sha256':digest(bit),
            'sources':sources,'frontend_memory':memory}
    if args.restore_default_build:
        report['frontend_sha256']=digest(elf)
        (OUT/'default-build-restored.json').write_text(json.dumps(report,indent=2)+'\n')
        print('PASS default ARM build restored; selected test artifacts preserved',flush=True)
        return
    selected=OUT/'inputs';selected.mkdir(exist_ok=True)
    for source in (elf,core,blob,bit,BASE/'inputs/fsbl.elf'):
        shutil.copy2(source,selected/source.name)
    env['XILINX_VITIS']=str(tools.parents[4])
    package=OUT/'FIRMWARE.BIN'
    run([ROOT/'scripts/make_firmware_bin.bat',package,selected/'fsbl.elf',
         selected/'appletini_yarz_top.bit',selected/'frontend.elf'])
    verify_image(package,expected_role=ROLE_FIRMWARE,expected_recovery=True,expected_product='appletini')
    report['inputs']={p.name:digest(p) for p in selected.iterdir() if p.is_file()}
    report['package_sha256']=digest(package)
    (OUT/'build-report.json').write_text(json.dumps(report,indent=2)+'\n')
    print('PASS ARM-only transport package:',report['package_sha256'],flush=True)
if __name__=='__main__':main()
