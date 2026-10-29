#!/usr/bin/env python3
"""Run production NET/download/storage lifecycle against deterministic adapters."""
import os
from pathlib import Path
import subprocess
from test_onee_input_service import find_native_c_compiler
ROOT=Path(__file__).resolve().parents[1]

def main():
    compiler=find_native_c_compiler()
    if not compiler:raise RuntimeError('Native C compiler required')
    out=ROOT/'build/http-download-tests';out.mkdir(parents=True,exist_ok=True)
    env={k:v for k,v in os.environ.items() if k.lower()!='path'}
    env['PATH']=str(compiler.parent)+os.pathsep+next(v for k,v in os.environ.items() if k.lower()=='path')
    exe=out/'http_download_host.exe'
    command=[str(compiler),'-std=c11','-O1','-Wall','-Wextra','-Werror',
             '-DAPPLETINI_HTTP_SERVICE=1','-DNET_SERVICE_HOST=1','-DHTTP_HOST_FAKE_SD=1',
             '-I'+str(ROOT/'scripts/fixtures/http_service_host'),'-I'+str(ROOT/'ps_sources/frontend')]
    command += [str(ROOT/name) for name in ('scripts/fixtures/http_download_host.c',
        'ps_sources/frontend/net_service.c','ps_sources/frontend/net_json.c',
        'ps_sources/frontend/http_download.c','ps_sources/frontend/download_filename.c')]
    command += ['-o',str(exe)]
    with (out/'BUILD.log').open('w') as log:subprocess.run(command,env=env,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True)
    result=subprocess.run([str(exe)],env=env,cwd=ROOT,capture_output=True,text=True,timeout=20)
    (out/'RESULT.log').write_text(result.stdout+result.stderr);result.check_returncode();print(result.stdout,end='')
if __name__=='__main__':main()
