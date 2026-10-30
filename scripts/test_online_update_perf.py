#!/usr/bin/env python3
"""Compare production HTTP-to-SD poll/chunk behavior against retained F1.3.5.

The native model supplies an already connected response and advances its clock
by 1ms per foreground round. Counts describe software batching and limits;
they are not measured W5100S, TLS, SD, ARM or physical-board throughput.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
from test_onee_input_service import find_native_c_compiler
from test_online_update import image,release

ROOT=Path(__file__).resolve().parents[1]
BASE=ROOT/'scripts/fixtures/online_update_perf_baseline'


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,default=ROOT/'build/online-update-perf-tests')
    parser.add_argument('--baseline-only',action='store_true')
    args=parser.parse_args();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    baseline=json.loads((BASE/'baseline.json').read_text())
    for name,digest in baseline['sources'].items():
        if hashlib.sha256((BASE/name).read_bytes()).hexdigest()!=digest:raise RuntimeError('Changed baseline '+name)
    payload=image('F1.3.4',padding=4700003)
    metadata=release(payload,b'')
    metadata['assets']=metadata['assets'][:1]
    firmware=out/'pipeline-firmware.bin';firmware.write_bytes(payload)
    document=out/'pipeline-metadata.json';document.write_text(json.dumps(metadata))
    compiler=find_native_c_compiler()
    if not compiler:raise RuntimeError('Native compiler required')
    env={k:v for k,v in os.environ.items() if k.lower()!='path'}
    env['PATH']=str(compiler.parent)+os.pathsep+next(v for k,v in os.environ.items() if k.lower()=='path')
    results={}
    for variant in ('baseline',) if args.baseline_only else ('baseline','current'):
        frontend=BASE/'ps_sources/frontend' if variant=='baseline' else ROOT/'ps_sources/frontend'
        sources=[ROOT/'scripts/fixtures/online_update_perf_host.c',frontend/'online_update_service.c',
                 frontend/'online_update_core.c',frontend/'http_core.c',ROOT/'ps_sources/frontend/net_json.c',
                 ROOT/'ps_sources/frontend/http_seed.c',ROOT/'ps_sources/frontend/http_roots.c',
                 ROOT/'ps_sources/lib/multiboot.c',ROOT/'ps_sources/lib/crc32.c']
        sources+=sorted((ROOT/'third_party/mbedtls/library').glob('*.c'))
        tracked=sources+[frontend/'http_service.c',frontend/'http_service.h',frontend/'online_update_core.h']
        captured={p.relative_to(ROOT).as_posix():hashlib.sha256(p.read_bytes()).hexdigest() for p in tracked}
        command=[str(compiler),'-std=c11','-O1','-Wall','-Wextra','-DAPPLETINI_HTTP_SERVICE=1',
                 '-DHTTP_HOST_FAKE_SD=1','-DMBEDTLS_CONFIG_FILE=<http_service_mbedtls_config.h>',
                 '-DPERF_HTTP_SOURCE="'+(frontend/'http_service.c').as_posix()+'"',
                 '-I'+str(frontend),'-I'+str(ROOT/'ps_sources/frontend'),
                 '-I'+str(ROOT/'scripts/fixtures/online_update_host'),
                 '-I'+str(ROOT/'scripts/fixtures/http_service_host'),
                 '-I'+str(ROOT/'third_party/mbedtls/include')]
        if variant=='baseline':command+=['-DPERF_BASELINE=1']
        exe=out/('pipeline-'+variant+'.exe')
        with (out/(variant+'-BUILD.log')).open('w') as log:
            subprocess.run(command+[str(p) for p in sources]+['-lws2_32','-o',str(exe)],env=env,cwd=ROOT,
                           stdout=log,stderr=subprocess.STDOUT,check=True)
        runs=[]
        for scenario in range(4):
            done=subprocess.run([str(exe),str(document),str(firmware),str(scenario)],env=env,cwd=ROOT,
                                capture_output=True,text=True,timeout=30)
            (out/f'{variant}-{scenario}-RESULT.log').write_text(done.stdout+done.stderr)
            if done.returncode:raise AssertionError(variant+' '+str(scenario)+': '+done.stdout+done.stderr)
            runs.append(json.loads(done.stdout));print(variant,done.stdout.strip(),flush=True)
        assert all(hashlib.sha256((ROOT/n).read_bytes()).hexdigest()==v for n,v in captured.items()),'Sources changed during benchmark'
        results[variant]=dict(runs=runs,source_hashes=captured,
                              http_source_sha256=hashlib.sha256((frontend/'http_service.c').read_bytes()).hexdigest())
    if 'current' in results:
        before=results['baseline']['runs'][0];after=results['current']['runs'][0]
        assert after['writes']==(len(payload)+131071)//131072
        assert after['rx_max']<=4096 and before['rx_max']<=512
        assert after['rounds']<before['rounds'] and after['writes']<before['writes']
        assert results['current']['runs'][1]['writes']==0
    report=dict(status='PASS',payload_bytes=len(payload),payload_sha256=hashlib.sha256(payload).hexdigest(),
                model_clock='1ms per foreground round; TLS/connection/physical SD timing not modeled',
                board_test=False,hardware_throughput_claim=False,results=results)
    (out/'performance.json').write_text(json.dumps(report,indent=2)+'\n')


if __name__=='__main__':main()
