#!/usr/bin/env python3
"""Build an audited CPU0 candidate with explicit retained FPGA/CPU1 inputs.

Requires a normal configured Vitis workspace. Does not rebuild FPGA, FSBL,
CPU1 or Golden, change the published/top-level images, flash, tag or publish.
Use --build-only while native/UI evidence is still being collected, then
--finalize-only with optional --report files to refresh the source/proof ZIPs.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import zipfile
from frontend_memory import audit_frontend_elf
from http_build_config import configure_http_service

ROOT=Path(__file__).resolve().parents[1]
INPUTS={'fsbl.elf':'fsbl','appletini_yarz_top.bit':'bitstream',
        'appletini_yarz_top.xsa':'xsa','frontend_core1.elf':'core1_elf','core1_blob.bin':'core1_blob'}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path,value):
    path.write_text(json.dumps(value,indent=2)+'\n',encoding='utf-8',newline='\n')


def sources():
    # Include headers even when they are not separate compiler command entries.
    return {p.relative_to(ROOT).as_posix():sha(p) for tree in ('ps_sources','third_party')
            for p in sorted((ROOT/tree).rglob('*')) if p.is_file() and p.suffix.lower() in ('.c','.h','.s')}


def run(command,log,env,cwd=ROOT):
    with log.open('w',encoding='utf-8') as stream:
        result=subprocess.run(command,cwd=cwd,env=env,stdout=stream,stderr=subprocess.STDOUT)
    if result.returncode: raise RuntimeError(f'Command failed ({result.returncode}); see {log}')


def finish(out,args,report):
    if sources()!=report['sources']: raise RuntimeError('Production sources differ from the compiled snapshot')
    audit=json.loads((out/'FIRMWARE_AUDIT.json').read_text())
    if audit['status']!='PASS' or audit['version']!=args.version: raise RuntimeError('Passing image audit required')
    notes=ROOT/'docs/releases'/f'{args.version}.md'
    if not notes.is_file(): raise RuntimeError(f'Missing release notes: {notes}')
    if args.readme_note is not None: report['readme_note']=args.readme_note
    shutil.copy2(notes,out/'RELEASE_NOTES.md')
    validation=out/'validation';validation.mkdir(exist_ok=True)
    for supplied in args.report:
        path=Path(supplied).resolve()
        if not path.is_file(): raise RuntimeError(f'Missing explicit report: {path}')
        target=validation/path.parent.name/path.name;target.parent.mkdir(exist_ok=True)
        if target.exists() and target.read_bytes()!=path.read_bytes():
            raise RuntimeError(f'Report basename collision: {path.name}')
        if path!=target: shutil.copy2(path,target)
    members=set(report['sources'])
    for tree in ('scripts','docs'):
        members.update(p.relative_to(ROOT).as_posix() for p in (ROOT/tree).rglob('*')
                       if p.is_file() and p.suffix.lower() in ('.py','.c','.h','.md','.json','.pem','.ps1','.bat','.tcl'))
    members.update(p.relative_to(ROOT).as_posix() for p in ROOT.glob('README*.md'))
    members.update(p.name for p in ROOT.glob('LICENSE*') if p.is_file())
    base=args.baseline.resolve()
    members.update((base/name).relative_to(ROOT).as_posix() for name in
                   ('FIRMWARE.BIN','ARM_BUILD.json','FIRMWARE_AUDIT.json','TIMING_MANIFEST.txt'))
    members.update((base/'inputs'/name).relative_to(ROOT).as_posix() for name in INPUTS)
    members.update(p.relative_to(ROOT).as_posix() for tree in ('ps_sources','third_party')
                   for p in (ROOT/tree).rglob('*') if p.is_file() and
                   (p.name.startswith('LICENSE') or p.suffix.lower() in ('.inc','.def','.ld')))
    for tree in ('software/https_setup','third_party/mbedtls','third_party/mozilla-ca'):
        members.update(p.relative_to(ROOT).as_posix() for p in (ROOT/tree).rglob('*')
                       if p.is_file() and (p.name.startswith('LICENSE') or p.suffix.lower() in ('.md','.pem','.json')))
    inventory={name:{'bytes':(ROOT/name).stat().st_size,'sha256':sha(ROOT/name)} for name in sorted(members)}
    with zipfile.ZipFile(out/'ARM-SOURCES.zip','w',zipfile.ZIP_DEFLATED) as archive:
        for name in inventory: archive.write(ROOT/name,name)
    with zipfile.ZipFile(out/'ARM-SOURCES.zip') as archive:
        if archive.testzip() or any(hashlib.sha256(archive.read(name)).hexdigest()!=item['sha256'] for name,item in inventory.items()):
            raise RuntimeError('Source archive verification failed')
    write_json(out/'SOURCE_TEST_HASHES.json',inventory)
    report.update(source_archive_sha256=sha(out/'ARM-SOURCES.zip'),frontend_memory=audit_frontend_elf(out/'inputs/frontend.elf'))
    write_json(out/'ARM_BUILD.json',report)
    (out/'README.md').write_text(
        f'# {args.version} CPU0 candidate\n\n'
        'Read RELEASE_NOTES.md for the online updater and cumulative input/profile fixes.\n'
        'This is an unflashed test candidate. Native tests and an image audit passed; no board run of this image was performed.\n'
        f'Golden {report["boot_version"]}, FSBL, FPGA and CPU1 are retained byte-for-byte from {report["baseline"]}.\n'
        'The FPGA timing fixes in the current HDL are not included. Published images and the top-level FIRMWARE.BIN remain unchanged.\n\n'
        'Copy FIRMWARE.BIN to the SD root and use the normal explicit firmware installation action.\n'
        'An existing root update file is never overwritten by the online downloader; move or remove it before preparing another.\n'
        'The optional BOOT.BIN is the previously released unchanged Golden image, not a new build.\n\n'
        'FIRMWARE_AUDIT.json checks packaged CPU0/FSBL/FPGA/CPU1 bytes, stack/heap and retained timing provenance.\n'
        'ARM_BUILD.json records the source freeze and commands. ARM-SOURCES.zip contains ordinary source, tests and docs;\n'
        'inputs/ holds copied toolchain build inputs. validation/ contains only explicitly supplied evidence.\n'+
        ('\n'+report['readme_note'].strip()+'\n' if report.get('readme_note') else ''),encoding='utf-8',newline='\n')
    with zipfile.ZipFile(out/f'Appletini-{args.version}-candidate.zip','w',zipfile.ZIP_DEFLATED) as archive:
        for name in ('FIRMWARE.BIN','BOOT.BIN','README.md','RELEASE_NOTES.md','FIRMWARE_AUDIT.json','ARM_BUILD.json','PREVIOUS_CANDIDATE_PRESERVED.json'):
            if (out/name).is_file(): archive.write(out/name,name)
    files={p.relative_to(out).as_posix():{'bytes':p.stat().st_size,'sha256':sha(p)}
           for p in sorted(out.rglob('*')) if p.is_file() and p.name not in ('BUILD_PROVENANCE.json','SHA256SUMS.txt','PACKAGE_CHECK.json')}
    write_json(out/'BUILD_PROVENANCE.json',dict(report,files=files))
    (out/'SHA256SUMS.txt').write_text(''.join(item['sha256']+'  '+name+'\n' for name,item in files.items()),encoding='utf-8',newline='\n')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--version',required=True)
    parser.add_argument('--baseline',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--report',action='append',default=[],help='optional evidence file copied into candidate validation/')
    parser.add_argument('--readme-note',help='optional candidate-specific setup/limitation paragraph saved in build metadata')
    parser.add_argument('--resume',action='store_true',help='retry an unfinished build; refuses to replace any packaged firmware')
    mode=parser.add_mutually_exclusive_group();mode.add_argument('--build-only',action='store_true');mode.add_argument('--finalize-only',action='store_true');mode.add_argument('--audit-only',action='store_true')
    args=parser.parse_args();base=args.baseline.resolve();out=args.output.resolve()
    if not re.fullmatch(r'F\d+\.\d+\.\d+',args.version): raise RuntimeError('Stable F version required')
    if not base.is_relative_to(ROOT/'firmwares'): raise RuntimeError('Retained baseline must be a project-owned firmwares/ release')
    if not out.is_relative_to(ROOT) or out==ROOT or out.is_relative_to(ROOT/'firmwares'):
        raise RuntimeError('Candidate output must be a new project directory outside published firmwares/')
    if args.finalize_only:
        finish(out,args,json.loads((out/'ARM_BUILD.json').read_text()));return
    if args.audit_only:
        report=json.loads((out/'ARM_BUILD.json').read_text())
        if sources()!=report['sources'] or sha(out/'FIRMWARE.BIN')!=report['package']['sha256']: raise RuntimeError('Candidate source/image differs from build record')
        run(report['audit_command'],out/'AUDIT.log',dict(os.environ));print((out/'AUDIT.log').read_text());return
    if (out/'FIRMWARE.BIN').exists() or ((out/'inputs').exists() and not args.resume): raise RuntimeError(f'Preserve existing candidate: {out}')
    audit=json.loads((base/'FIRMWARE_AUDIT.json').read_text());baseline=json.loads((base/'ARM_BUILD.json').read_text())
    if audit['status']!='PASS' or sha(base/'FIRMWARE.BIN')!=audit['inputs']['firmware']['sha256'] or baseline['package']['sha256']!=sha(base/'FIRMWARE.BIN'):
        raise RuntimeError('Baseline firmware/audit identity mismatch')
    for name,key in INPUTS.items():
        if sha(base/'inputs'/name)!=audit['inputs'][key]['sha256']: raise RuntimeError(f'Baseline input mismatch: {name}')
    header=(ROOT/'ps_sources/image_versions.h').read_text()
    boot=audit['boot_version']
    if f'"{args.version}"' not in header or f'"{boot}"' not in header: raise RuntimeError('Version header differs from requested firmware/retained Boot')
    preserved={str(p.relative_to(ROOT)):sha(p) for p in [ROOT/'FIRMWARE.BIN',base/'FIRMWARE.BIN',
               ROOT/'hdl/apple/apple_top.sv',ROOT/'hdl/apple/onee_mode_safety_guard.sv'] if p.is_file()}
    before=sources();out.mkdir(parents=True,exist_ok=True);(out/'inputs').mkdir(exist_ok=True)
    if any((out/'inputs'/n).exists() and sha(out/'inputs'/n)!=sha(base/'inputs'/n) for n in INPUTS): raise RuntimeError('Prepared input differs from baseline')
    for name in INPUTS: shutil.copy2(base/'inputs'/name,out/'inputs'/name)
    for name in ('ARM_BUILD.json','FIRMWARE_AUDIT.json','TIMING_MANIFEST.txt'): shutil.copy2(base/name,out/('BASELINE_'+name))
    workspace=ROOT/'vitis_workspace/frontend';build=workspace/'build'
    platforms=[ROOT/'vitis_workspace/appletini_platform'/n for n in
               ('export/appletini_platform/hw/appletini_yarz_top.xsa','hw/appletini_yarz_top.xsa')]
    platforms=[p for p in platforms if p.is_file()]
    if not platforms or any(sha(p)!=sha(out/'inputs/appletini_yarz_top.xsa') for p in platforms): raise RuntimeError('Workspace XSA differs from retained FPGA')
    blob=workspace/'src/core1_blob.bin'
    if not blob.exists(): shutil.copy2(out/'inputs/core1_blob.bin',blob)
    if sha(blob)!=sha(out/'inputs/core1_blob.bin'): raise RuntimeError('Workspace CPU1 blob differs from retained CPU1')
    configure_http_service(ROOT)
    config=(workspace/'src/UserConfig.cmake').read_text()
    if any(x in config for x in ('APPLETINI_HTTP_BENCH=1','APPLETINI_HTTPS_PROBE=1','APPLETINI_HTTP_DIAGNOSTICS=1')): raise RuntimeError('Nonproduction HTTP configuration')
    cache=(build/'CMakeCache.txt').read_text();cmake=re.search(r'^CMAKE_COMMAND:INTERNAL=(.+)$',cache,re.M).group(1)
    commands=json.loads((build/'compile_commands.json').read_text());match=re.match(r'^(?:"([^"]+)"|(\S+))',commands[0]['command'])
    compiler=Path(match.group(1) or match.group(2));env={k:v for k,v in os.environ.items() if k.lower()!='path'}
    env['PATH']=str(compiler.parent)+os.pathsep+next(v for k,v in os.environ.items() if k.lower()=='path')
    command=[cmake,'--build',str(build),'--parallel','8'];direct=None
    print('Building CPU0 only: '+' '.join(command),flush=True)
    try: run(command,out/'CPU0_BUILD.log',env)
    except RuntimeError:
        log=(out/'CPU0_BUILD.log').read_text()
        if 'FAILED: frontend.elf' not in log or 'The system cannot execute the specified program.' not in log: raise
        block=re.search(r'build frontend\.elf:.*?(?=\n\n)',(build/'build.ninja').read_text(),re.S).group(0)
        flags=re.search(r'\n  FLAGS = ([^\n]+)',block).group(1);extra=re.search(r'\n  LINK_FLAGS = ([^\n]+)',block)
        fixed=re.search(r'rule C_EXECUTABLE_LINKER[^\n]*\n.*?\$FLAGS(.*?)\$LINK_FLAGS',(build/'CMakeFiles/rules.ninja').read_text(),re.S).group(1)
        direct=[str(compiler),*shlex.split(flags),*shlex.split(fixed),*shlex.split(extra.group(1) if extra else ''),'@CMakeFiles/frontend.elf.rsp','-o','frontend.elf']
        run(direct,out/'DIRECT_LINK.log',env,build)
    if sources()!=before: raise RuntimeError('Production sources changed during CPU0 build')
    shutil.copy2(build/'frontend.elf',out/'inputs/frontend.elf')
    (out/'build-config').mkdir()
    for name,path in {'UserConfig.cmake':workspace/'src/UserConfig.cmake','lscript.ld':workspace/'src/lscript.ld',
                      'build.ninja':build/'build.ninja','rules.ninja':build/'CMakeFiles/rules.ninja','frontend.elf.rsp':build/'CMakeFiles/frontend.elf.rsp'}.items(): shutil.copy2(path,out/'build-config'/name)
    scratch=out/'scratch';scratch.mkdir();env.update(XILINX_VITIS=r'C:\Xilinx\2025.2\Vitis',TEMP=str(scratch),TMP=str(scratch))
    package=[str(ROOT/'scripts/make_firmware_bin.bat'),str(out/'FIRMWARE.BIN'),str(out/'inputs/fsbl.elf'),str(out/'inputs/appletini_yarz_top.bit'),str(out/'inputs/frontend.elf')]
    run(package,out/'PACKAGE.log',env)
    audit_command=[sys.executable,str(ROOT/'scripts/audit_firmware_release.py'),'--firmware',str(out/'FIRMWARE.BIN'),
                   '--fsbl',str(out/'inputs/fsbl.elf'),'--frontend',str(out/'inputs/frontend.elf'),'--bitstream',str(out/'inputs/appletini_yarz_top.bit'),
                   '--xsa',str(out/'inputs/appletini_yarz_top.xsa'),'--platform-xsa',str(platforms[0]),'--run-manifest',str(out/'BASELINE_TIMING_MANIFEST.txt'),
                   '--core1-elf',str(out/'inputs/frontend_core1.elf'),'--core1-blob',str(out/'inputs/core1_blob.bin'),
                   '--version',args.version,'--boot-version',boot,'--output',str(out/'FIRMWARE_AUDIT.json')]
    if sources()!=before or any(sha(ROOT/name)!=digest for name,digest in preserved.items()): raise RuntimeError('Source/preserved image changed during packaging')
    boot_asset=ROOT/'BOOT.BIN'
    expected_boot='6382353ddd83d3f4fa3f4d447acac741b7d76a06501cb35042b69d190914222c'
    if boot_asset.is_file() and sha(boot_asset)==expected_boot: shutil.copy2(boot_asset,out/'BOOT.BIN')
    compiled={Path(c['file']).resolve().relative_to(ROOT).as_posix():sha(Path(c['file']).resolve())
              for c in json.loads((build/'compile_commands.json').read_text()) if Path(c['file']).resolve().is_relative_to(ROOT)}
    report=dict(kind='CPU0 firmware candidate',version=args.version,boot_version=boot,baseline=str(base.relative_to(ROOT)),
                sources=before,compiled_sources=compiled,source_changes=[n for n,v in before.items() if baseline['sources'].get(n)!=v],
                compiled_source_changes=[n for n,v in compiled.items() if baseline['sources'].get(n)!=v],
                build_command=command,direct_link_command=direct,package_command=package,audit_command=audit_command,
                fpga_rebuilt=False,cpu1_rebuilt=False,fsbl_rebuilt=False,golden_rebuilt=False,flashed=False,published=False,
                package={'sha256':sha(out/'FIRMWARE.BIN')},preserved_inputs=preserved,
                frontend_memory=audit_frontend_elf(out/'inputs/frontend.elf'))
    write_json(out/'ARM_BUILD.json',report)
    run(audit_command,out/'AUDIT.log',env)
    if not args.build_only: finish(out,args,report)
    print(json.dumps({'firmware':str(out/'FIRMWARE.BIN'),'sha256':sha(out/'FIRMWARE.BIN')},indent=2))


if __name__=='__main__': main()
