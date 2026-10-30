#!/usr/bin/env python3
"""Native offline tests for stable release selection and streamed SD preparation.

All synthetic Bootgen images are built here from fixed bytes, then checked by
production multiboot.c. Retained release binaries are extra integration inputs,
not dependencies needed to generate or run the synthetic cases.
"""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import zlib
from test_onee_input_service import find_native_c_compiler

ROOT=Path(__file__).resolve().parents[1]
PREFIX='https://github.com/hasseily/appletini-one/releases/download/'


def image(version, *, golden=False, product='appletini', padding=0):
    """Small valid Zynq Bootgen table with FSBL/[PL]/loaded CPU0 identity."""
    if golden:
        text=(b'APPLETINI-MULTIBOOT-GOLDEN-V1\0APPLETINI-GOLDEN-RECOVERY-V1\0'
              b'[GOLDEN] boot_updater start @0x00100000\0'
              b'[GOLDEN] Boot image version: Boot '+version.encode()+b'\r\n\0')
        parts=[(0x10,0,0,b'FSBL'),(0x10,0x100000,0x100000,text)]
    else:
        marker=b'APPLETINI-MULTIBOOT-V1' if product=='appletini' else b'MULTITINI-MULTIBOOT-V1'
        text=(marker+b'\0APPLETINI-GOLDEN-RECOVERY-V1\0\r\nAppletini control UART\r\n\0'
              b'Firmware image: Firmware F'+version[1:].encode()+b'\r\n\0')
        parts=[(0x10,0,0,b'FSBL'),(0x20,0,0,b'FPGA'),(0x10,0x100000,0x100000,text)]
    if padding:
        attr,load,execute,data=parts[-1]
        parts[-1]=(attr,load,execute,data+bytes((i*7+(i>>8))&255 for i in range(padding)))
    payload=bytearray(0x400)
    def word(at,value): struct.pack_into('<I',payload,at,value&0xffffffff)
    word(0x20,0xaa995566);word(0x24,0x584c4e58)
    word(0x30,0x400);word(0x34,4);word(0x40,4)
    word(0x98,0x100);word(0x9c,0x180)
    word(0x100,1);word(0x104,len(parts));word(0x108,0x180//4);word(0x10c,0x140//4)
    for i,(attr,load,execute,data) in enumerate(parts):
        data+=b'\0'*(-len(data)%4)
        start=len(payload);payload.extend(data)
        p=0x180+i*64
        for offset,value in ((4,len(data)//4),(8,len(data)//4),(12,load),
                             (16,execute),(20,start//4),(24,attr)):
            word(p+offset,value)
        word(p+60,~sum(struct.unpack_from('<15I',payload,p)))
    word(0x180+len(parts)*64+60,0xffffffff)
    word(0x48,~sum(struct.unpack_from('<10I',payload,0x20)))
    fmt=1 if golden else 2
    flags=1 if golden else (0x501 if product=='appletini' else 0x601)
    footer=struct.pack('<8s5I',b'ATNIMG1\0',fmt,1 if golden else 2,flags,len(payload),zlib.crc32(payload))
    return bytes(payload)+footer+struct.pack('<I',zlib.crc32(footer))


def release(fw,boot,tag='F1.3.4'):
    def asset(name,data):
        return dict(name=name,size=len(data),digest='sha256:'+hashlib.sha256(data).hexdigest(),
                    browser_download_url=PREFIX+tag+'/'+name)
    return dict(tag_name=tag,draft=False,prerelease=False,published_at='2026-10-29T20:00:00Z',
                assets=[asset('FIRMWARE.BIN',fw),asset('BOOT.BIN',boot)])


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--core-only',action='store_true')
    parser.add_argument('--output',type=Path,default=ROOT/'build/online-update-tests',help='native test output directory')
    args=parser.parse_args()
    compiler=find_native_c_compiler()
    if not compiler: raise RuntimeError('Native C compiler required')
    out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    env={k:v for k,v in os.environ.items() if k.lower()!='path'}
    env['PATH']=str(compiler.parent)+os.pathsep+next(v for k,v in os.environ.items() if k.lower()=='path')
    common=[str(compiler),'-std=c11','-O1','-Wall','-Wextra','-Werror',
            '-I'+str(ROOT/'ps_sources/frontend'),'-I'+str(ROOT/'ps_sources/lib')]
    core=out/'online_update_core_host.exe'
    sources=['scripts/fixtures/online_update_core_host.c','ps_sources/frontend/online_update_core.c',
             'ps_sources/frontend/net_json.c','ps_sources/lib/multiboot.c','ps_sources/lib/crc32.c']
    with (out/'BUILD-core.log').open('w') as log:
        subprocess.run(common+[str(ROOT/s) for s in sources]+['-o',str(core)],env=env,cwd=ROOT,
                       stdout=log,stderr=subprocess.STDOUT,check=True)
    results=[]
    def run(*command):
        completed=subprocess.run([str(core),*map(str,command)],env=env,cwd=ROOT,
                                 capture_output=True,text=True,check=True,timeout=15)
        return completed.stdout.strip().split()
    def case(name,condition):
        if not condition: raise AssertionError(name)
        results.append(name)
    def parse_case(name,document,want,chunk=17,budget=13):
        path=out/(name+'.json')
        path.write_bytes(document if isinstance(document,bytes) else json.dumps(document,separators=(',',':')).encode())
        got=run('parse',path,chunk,budget)
        case(name,int(got[0])==want)
        return got
    fw=image('F1.3.4');boot=image('B2.0.6',golden=True)
    for name,data,kind,version in [('firmware',fw,0,'F1.3.4'),('boot',boot,1,'B2.0.6')]:
        path=out/(name+'.bin');path.write_bytes(data)
        got=run('image',path,kind);case('generated-'+name+'-real-validator',got==['0',version])
    base=release(fw,boot)
    got=parse_case('stable-both-assets',base,1)
    case('exact-both-size',got[2:]==['1',str(len(fw)),'1',str(len(boot))])
    for key in ('draft','prerelease'):
        doc=copy.deepcopy(base);doc[key]=True;parse_case(key,doc,-202)
    for tag in ('F1.3.5b1','F1.3.5-rc1','F01.3.5','vF1.3.5','B2.0.7','F1.3','F1.3.5+build'):
        doc=copy.deepcopy(base);doc['tag_name']=tag;parse_case('reject-tag-'+tag.replace('+','_'),doc,-202)
    for key in ('tag_name','draft','prerelease','published_at','assets'):
        doc=copy.deepcopy(base);del doc[key];parse_case('missing-'+key,doc,-202)
    for key,value in [('size',0),('size',0x600001),('size',2**32),('size',1.5),
                      ('digest',None),('digest','sha256:'+'g'*64),
                      ('browser_download_url','http://github.com/hasseily/appletini-one/releases/download/F1.3.4/FIRMWARE.BIN'),
                      ('browser_download_url',PREFIX+'F1.3.3/FIRMWARE.BIN'),
                      ('browser_download_url',PREFIX+'F1.3.4/FIRMWARE.BIN?extra=1')]:
        doc=copy.deepcopy(base);doc['assets'][0][key]=value
        parse_case('bad-asset-'+str(len(results)),doc,-203)
    doc=copy.deepcopy(base);doc['assets']*=2;parse_case('duplicate-asset',doc,-203)
    doc=copy.deepcopy(base);doc['assets']=[];parse_case('no-assets-safe-empty',doc,1)
    doc=copy.deepcopy(base);doc['assets']=doc['assets'][1:];got=parse_case('only-boot-asset',doc,1)
    case('missing-firmware-flag',got[2]=='0' and got[4]=='1')
    doc=copy.deepcopy(base);doc['assets'].append(dict(name='source.zip',digest=None,browser_download_url='x'*3000))
    parse_case('ignore-unrelated-asset-metadata',doc,1)
    for name in ('release-notes-'+'x'*100+'.zip','sources-\u00e9.zip'):
        doc=copy.deepcopy(base);doc['assets'].append(dict(name=name,digest=None,browser_download_url='x'*3000))
        parse_case('ignore-unrelated-name-'+str(len(results)),doc,1)
    parse_case('wrong-html',b'<!doctype html><h1>GitHub gateway error</h1>',-201)
    parse_case('truncated-json',json.dumps(base).encode()[:-4],-201)
    parse_case('duplicate-release-key',b'{"draft":false,"draft":false}',-202)
    parse_case('json-byte-limit',b' '*32769,-204,chunk=32769)
    doc=copy.deepcopy(base);doc['extra']=[0]*17000;parse_case('json-token-limit',doc,-204,chunk=40000)
    # Valid JSON with escaped names/URL separators must retain exact asset identity.
    raw=json.dumps(base).replace('FIRMWARE.BIN','FIRMWARE\\u002eBIN').replace('https://','https:\\/\\/').encode()
    parse_case('json-escaped-ascii',raw,1,chunk=1,budget=1)
    for installed,offered,prefix,want in [
        ('F1.3.9','F1.3.10','F',1),('F1.9.9','F1.10.0','F',1),
        ('F2.0.0','F1.99.99','F',-1),('F1.3.4','F1.3.4','F',0),
        ('B2.0.6','B2.0.7','B',1),('B3.0.0','B2.99.99','B',-1),
        ('Firmware F1.3.4','F1.3.5','F',1),('Boot B2.0.6','B2.0.7','B',1)]:
        got=run('compare',installed,offered,prefix)
        case('semver-'+installed+'-'+offered,got==['0',str(want)])
    for installed,offered,prefix in [('F1.3.4','B2.0.7','F'),('B2.0.6','F1.3.5','B'),
                                   ('F1.3.4b1','F1.3.5','F'),('F1.3.4','F1.3.5-rc1','F'),
                                   ('F1.3.4','F4294967296.0.0','F')]:
        case('semver-reject-'+installed+'-'+offered,run('compare',installed,offered,prefix)[0]=='-207')
    for version,golden in [('F01.3.4',False),('B02.0.6',True)]:
        path=out/('leading-zero-'+version+'.bin');path.write_bytes(image(version,golden=golden))
        case('loaded-version-reject-'+version,run('image',path,int(golden))[0]!='0')
    live=ROOT/'scripts/fixtures/github_latest_F1.3.4.json'
    got=run('parse',live,97,31)
    case('captured-github-latest-F134-exact-two-names',got[0]=='1' and got[1]=='F1.3.4' and got[2]=='1' and got[4]=='1')
    actual=[]
    for path,kind,version in [(ROOT/'firmwares/firmware-F1.3.4/FIRMWARE.BIN',0,'F1.3.4'),
                              (ROOT/'BOOT.BIN',1,'B2.0.6')]:
        if path.is_file():
            case('retained-'+path.name+'-loaded-version',run('image',path,kind)==['0',version])
            actual.append(dict(path=str(path.relative_to(ROOT)),sha256=hashlib.sha256(path.read_bytes()).hexdigest(),version=version))
    summary=dict(core_cases=len(results),cases=results,retained_images=actual,
                 source_hashes={s:hashlib.sha256((ROOT/s).read_bytes()).hexdigest() for s in sources})
    (out/'core-validation.json').write_text(json.dumps(summary,indent=2)+'\n')
    print(f'PASS {len(results)} native online-update core/image cases')
    if not args.core_only:
        service=out/'online_update_service_host.exe'
        service_sources=['scripts/fixtures/online_update_service_host.c',
                         'ps_sources/frontend/online_update_service.c',
                         'ps_sources/frontend/online_update_core.c','ps_sources/frontend/net_json.c',
                         'ps_sources/frontend/http_core.c','ps_sources/lib/multiboot.c','ps_sources/lib/crc32.c',
                         'third_party/mbedtls/library/sha256.c','third_party/mbedtls/library/platform_util.c',
                         'third_party/mbedtls/library/platform.c']
        command=common+['-DAPPLETINI_HTTP_SERVICE=1','-DHTTP_HOST_FAKE_SD=1',
                        '-DMBEDTLS_CONFIG_FILE=<http_service_mbedtls_config.h>',
                        '-I'+str(ROOT/'scripts/fixtures/online_update_host'),
                        '-I'+str(ROOT/'third_party/mbedtls/include')]
        with (out/'BUILD-service.log').open('w') as log:
            subprocess.run(command+[str(ROOT/s) for s in service_sources]+['-o',str(service)],
                           cwd=ROOT,env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
        large_fw=image('F1.3.4',padding=200003)
        service_results=[]
        def service_case(name,scenario,fw_bytes=large_fw,boot_bytes=boot,document=None):
            directory=out/('service-'+name);directory.mkdir(exist_ok=True)
            if document is None: document=release(fw_bytes,boot_bytes)
            (directory/'release.json').write_text(json.dumps(document))
            (directory/'firmware.bin').write_bytes(fw_bytes);(directory/'boot.bin').write_bytes(boot_bytes)
            done=subprocess.run([str(service),scenario,str(directory/'release.json'),
                                 str(directory/'firmware.bin'),str(directory/'boot.bin')],
                                cwd=ROOT,env=env,capture_output=True,text=True,timeout=20)
            (directory/'RESULT.log').write_text(done.stdout+done.stderr)
            if done.returncode: raise AssertionError(name+': '+done.stdout+done.stderr)
            service_results.append(dict(name=name,output=done.stdout.strip()))
            print(done.stdout,end='')
        for scenario in ('success','boot-success','current','downgrade','boot-current','firmware-current',
                         'http-owner-busy','start-owner-busy','root-conflict','part-conflict',
                         'power-leftover','old-boot-preserved','readonly','write-fail','short-write',
                         'sync-fail','close-fail','read-fail','rename-fail','rename-race','truncated',
                         'http-fail','http404','cancel','lost-allowed','storage-busy','cleanup-retry',
                         'cancel-before-start','cancel-ready','api-owner-change','cancel-restore','completion-restore',
                         'redirect-assets','redirect-objects','redirect-evil','redirect-http','redirect-port'):
            service_case(scenario,scenario)
        for name,assets in [('missing-fw',[release(large_fw,boot)['assets'][1]]),
                            ('missing-boot',[release(large_fw,boot)['assets'][0]]),('no-assets',[])]:
            doc=release(large_fw,boot);doc['assets']=assets
            service_case(name,name,document=doc)
        service_case('api-redirect-evil','redirect-api-evil')
        service_case('boot-extra-chunked','boot-extra-chunked',boot_bytes=boot+b'EXTRA13BYTES!!',document=release(large_fw,boot))
        service_case('image-extra-chunked','image-extra-chunked',fw_bytes=large_fw+b'EXTRA13BYTES!!',document=release(large_fw,boot))
        service_case('bad-boot-preserves-FW-offer','bad-boot-fw-available',boot_bytes=image('B2.0.7-rc1',golden=True))
        for name,changed in [('html',b'<html>wrong image, valid download SHA256</html>'),
                             ('product',image('F1.3.4',product='multitini')),
                             ('version',image('F1.3.5')),
                             ('suffix-version',image('F1.3.4-rc1'))]:
            service_case('reject-'+name,'bad-image',changed)
        changed=bytearray(large_fw);changed[-33]^=1
        service_case('reject-crc','bad-image',bytes(changed))
        service_case('reject-sha','bad-image',bytes(changed),document=release(large_fw,boot))
        for name,changed in [('boot-html',b'<html>bad golden image</html> padding'),
                             ('boot-unstable',image('B2.0.7-rc1',golden=True))]:
            service_case('reject-'+name,'check-invalid',boot_bytes=changed)
        malformed=copy.deepcopy(release(large_fw,boot));malformed['prerelease']=True
        service_case('reject-prerelease','check-invalid',document=malformed)
        (out/'service-validation.json').write_text(json.dumps(dict(cases=service_results,
            source_hashes={s:hashlib.sha256((ROOT/s).read_bytes()).hexdigest() for s in service_sources}),indent=2)+'\n')
        print(f'PASS {len(service_results)} native online-update service lifecycle cases')
        diagnostic=out/'online_update_diagnostics_host.exe'
        diag_sources=['scripts/fixtures/online_update_diagnostics_host.c',*service_sources[1:]]
        with (out/'BUILD-diagnostics.log').open('w') as log:
            subprocess.run(command+[str(ROOT/s) for s in diag_sources]+['-o',str(diagnostic)],
                           cwd=ROOT,env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
        diagnostics=[]
        def diagnostic_case(name,mode,*,rc=0,code=0,verify=0,stage,primary,fr=0,mb=0,cleanup=0,
                            http_rc=None,fw_bytes=large_fw,boot_bytes=boot,document=None,keyword='-'):
            directory=out/('diagnostic-'+name);directory.mkdir(exist_ok=True)
            if document is None:document=release(fw_bytes,boot_bytes)
            raw=document if isinstance(document,bytes) else json.dumps(document).encode()
            (directory/'release.json').write_bytes(raw)
            (directory/'firmware.bin').write_bytes(fw_bytes);(directory/'boot.bin').write_bytes(boot_bytes)
            values=[name,str(directory/'release.json'),str(directory/'firmware.bin'),str(directory/'boot.bin'),
                    mode,str(rc),str(code),str(verify),stage,str(primary),str(fr),str(mb),str(cleanup),
                    str(rc if http_rc is None else http_rc),keyword]
            done=subprocess.run([str(diagnostic),*values],cwd=ROOT,env=env,capture_output=True,text=True,timeout=20)
            (directory/'RESULT.log').write_text(done.stdout+done.stderr)
            if done.returncode:raise AssertionError(name+': '+done.stdout+done.stderr)
            diagnostics.append(dict(name=name,output=done.stdout.strip()));print(done.stdout,end='')
        for name,rc in [('busy',-100),('argument',-101),('io',-102),('storage',-110),('network',-111)]:
            diagnostic_case('api-start-'+name,'api-start',rc=rc,stage='Release request',primary=rc)
        diagnostic_case('old-api-header-negative','api-format',stage='Release request',primary=-1,http_rc=-1)
        diagnostic_case('foreign-owner','foreign-owner',stage='Release request',primary=-100,http_rc=-100)
        for name,rc in [('clock',-103),('seed',-104),('trust',-105),('dns',-106),('io',-102),
                        ('timeout',-107),('network',-111),('format',-1)]:
            diagnostic_case('api-response-'+name,'api-response',rc=rc,stage='Release response',primary=rc)
        for mask in (4,8,0x4008,0xffffffff):
            diagnostic_case('api-tls-'+hex(mask),'api-response',rc=-9984,verify=mask,
                            stage='Release response',primary=-9984)
        for code in (403,404,429,500,503):
            diagnostic_case('api-http-'+str(code),'api-response',code=code,stage='Release response',primary=-215)
        diagnostic_case('invalid-json','metadata',code=200,stage='Release metadata',primary=-201,
                        document=b'<html>not release metadata</html>')
        doc=release(large_fw,boot);doc['prerelease']=True
        diagnostic_case('release-policy','metadata',code=200,stage='Release metadata',primary=-202,document=doc)
        doc=release(large_fw,boot);doc['assets'][0]['digest']=None
        diagnostic_case('missing-digest','metadata',code=200,stage='Release metadata',primary=-203,document=doc)
        diagnostic_case('boot-start-dns','boot-start',rc=-106,stage='Boot request',primary=-106)
        diagnostic_case('old-boot-header-negative','boot-format',stage='Boot request',primary=-1,http_rc=-1)
        diagnostic_case('boot-tls','boot-response',rc=-9984,verify=8,stage='Boot download',primary=-9984)
        diagnostic_case('boot-http404','boot-response',code=404,stage='Boot download',primary=-215)
        diagnostic_case('boot-invalid-image','boot-validation',code=200,stage='Boot validation',primary=-206,mb=3,
                        boot_bytes=b'<html>invalid image with matching API SHA256</html>'+b' '*256)
        diagnostic_case('boot-truncated-header','boot-validation',code=200,stage='Boot validation',primary=-206,mb=2,
                        boot_bytes=b'<html>short image with matching API SHA256</html>')
        diagnostic_case('image-start-network','image-start',rc=-111,stage='Image request',primary=-111)
        diagnostic_case('old-image-header-negative','image-format',stage='Image request',primary=-1,http_rc=-1)
        diagnostic_case('image-timeout','image-response',rc=-107,stage='Image download',primary=-107)
        diagnostic_case('image-http503','image-response',code=503,stage='Image download',primary=-215)
        diagnostic_case('sd-readonly','sd-stage',stage='SD staging',primary=-208,fr=10)
        diagnostic_case('write-plus-close-fail','image-write-close',code=200,stage='Image download',primary=-210,fr=1,cleanup=1)
        diagnostic_case('sync-plus-unlink-fail','sd-sync-unlink',code=200,stage='Image validation',primary=-211,fr=1,cleanup=1)
        diagnostic_case('close-primary-plus-cleanup','sd-close',code=200,stage='SD close',primary=-212,fr=1,cleanup=1)
        diagnostic_case('image-product','image-validation',code=200,stage='Image validation',primary=-206,mb=4,
                        fw_bytes=image('F1.3.4',product='multitini'))
        (out/'diagnostic-validation.json').write_text(json.dumps(dict(cases=diagnostics,
            source_hashes={s:hashlib.sha256((ROOT/s).read_bytes()).hexdigest() for s in diag_sources}),indent=2)+'\n')
        print(f'PASS {len(diagnostics)} native online-update diagnostic cases, including actual header-builder regression')
        owner_exe=out/'online_update_owner_host.exe'
        owner_sources=['scripts/fixtures/online_update_owner_host.c','ps_sources/frontend/http_core.c',
                       'ps_sources/frontend/http_seed.c','ps_sources/frontend/http_roots.c','ps_sources/lib/crc32.c']
        owner_sources += [str(p.relative_to(ROOT)) for p in sorted((ROOT/'third_party/mbedtls/library').glob('*.c'))]
        owner_command=[str(compiler),'-std=c11','-O1','-Wall','-Wextra',
                       '-DAPPLETINI_HTTP_SERVICE=1','-DMBEDTLS_CONFIG_FILE=<http_service_mbedtls_config.h>',
                       '-I'+str(ROOT/'scripts/fixtures/http_service_host'),
                       '-I'+str(ROOT/'ps_sources/frontend'),'-I'+str(ROOT/'third_party/mbedtls/include')]
        with (out/'BUILD-owner.log').open('w') as log:
            subprocess.run(owner_command+[str(ROOT/s) for s in owner_sources]+['-lws2_32','-o',str(owner_exe)],
                           cwd=ROOT,env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
        done=subprocess.run([str(owner_exe)],cwd=ROOT,env=env,capture_output=True,text=True,timeout=20)
        (out/'owner-RESULT.log').write_text(done.stdout+done.stderr)
        done.check_returncode();print(done.stdout,end='')
        tested=set(sources+service_sources+owner_sources+diag_sources)
        tested.update(['ps_sources/frontend/http_service.c','ps_sources/frontend/http_service.h',
                       'ps_sources/frontend/online_update_core.h','ps_sources/frontend/online_update_service.h',
                       'ps_sources/lib/multiboot.h','ps_sources/lib/crc32.h',
                       'scripts/test_online_update.py','scripts/fixtures/online_update_host/ff.h',
                       'scripts/fixtures/http_service_host/ff.h','scripts/fixtures/http_attachment_service_host.c'])
        (out/'validation.json').write_text(json.dumps(dict(core_cases=len(results),
            service_cases=len(service_results),diagnostic_cases=len(diagnostics),actual_http_owner_pass=True,
            no_flash_adapter=True,board_test=False,
            executables={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in (core,service,owner_exe)},
            source_hashes={s:hashlib.sha256((ROOT/s).read_bytes()).hexdigest() for s in sorted(tested)}),indent=2)+'\n')


if __name__=='__main__':main()
