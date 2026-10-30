#!/usr/bin/env python3
"""Exercise actual HTTP batching, backpressure, budgets and Ethernet cleanup."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
from test_onee_input_service import find_native_c_compiler
ROOT=Path(__file__).resolve().parents[1]


def main():
    out=ROOT/'build/http-update-throughput-tests';out.mkdir(parents=True,exist_ok=True)
    compiler=find_native_c_compiler();assert compiler
    env={k:v for k,v in os.environ.items() if k.lower()!='path'}
    env['PATH']=str(compiler.parent)+os.pathsep+next(v for k,v in os.environ.items() if k.lower()=='path')
    sources=[ROOT/'scripts/fixtures/http_update_throughput_host.c',ROOT/'ps_sources/frontend/online_update_service.c',
             ROOT/'ps_sources/frontend/online_update_core.c',ROOT/'ps_sources/frontend/http_core.c',
             ROOT/'ps_sources/frontend/net_json.c',ROOT/'ps_sources/frontend/http_seed.c',
             ROOT/'ps_sources/frontend/http_roots.c',ROOT/'ps_sources/lib/multiboot.c',ROOT/'ps_sources/lib/crc32.c']
    sources+=sorted((ROOT/'third_party/mbedtls/library').glob('*.c'))
    exe=out/'http-update-throughput.exe'
    command=[str(compiler),'-std=c11','-O1','-Wall','-Wextra','-DAPPLETINI_HTTP_SERVICE=1',
             '-DHTTP_HOST_FAKE_SD=1','-DMBEDTLS_CONFIG_FILE=<http_service_mbedtls_config.h>',
             '-I'+str(ROOT/'ps_sources/frontend'),'-I'+str(ROOT/'scripts/fixtures/online_update_host'),
             '-I'+str(ROOT/'scripts/fixtures/http_service_host'),'-I'+str(ROOT/'third_party/mbedtls/include')]
    with (out/'BUILD.log').open('w') as log:
        subprocess.run(command+[str(p) for p in sources]+['-lws2_32','-o',str(exe)],cwd=ROOT,env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
    done=subprocess.run([str(exe)],cwd=ROOT,env=env,capture_output=True,text=True,timeout=20)
    (out/'RESULT.log').write_text(done.stdout+done.stderr);print(done.stdout+done.stderr,end='');done.check_returncode()
    tracked=sources+[ROOT/'ps_sources/frontend/http_service.c',ROOT/'ps_sources/frontend/http_service.h',ROOT/'scripts/fixtures/online_update_perf_host.c']
    (out/'validation.json').write_text(json.dumps(dict(status='PASS',board_test=False,
        production_http_engine=True,TLS_connection_modeled=True,stdout=done.stdout,
        source_hashes={p.relative_to(ROOT).as_posix():hashlib.sha256(p.read_bytes()).hexdigest() for p in tracked}),indent=2)+'\n')


if __name__=='__main__':main()
