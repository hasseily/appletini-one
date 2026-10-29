#!/usr/bin/env python3
"""Exercise the service's exact TLS configuration independently of the board."""
import os
from pathlib import Path
import subprocess
from test_onee_input_service import find_native_c_compiler

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'build/http-service-tests'

def main():
    compiler=find_native_c_compiler()
    if not compiler: raise RuntimeError('Native C compiler required')
    OUT.mkdir(parents=True,exist_ok=True)
    ca=ROOT/'software/https_setup/ATNETCA.PEM'
    env={k:v for k,v in os.environ.items() if k.lower()!='path'}
    env['PATH']=str(compiler.parent)+os.pathsep+next((v for k,v in os.environ.items() if k.lower()=='path'),'')
    executable=OUT/'http_tls_host.exe'
    args=[str(compiler),'-std=c11','-O1','-DMBEDTLS_CONFIG_FILE=<http_service_mbedtls_config.h>',
          '-I'+str(ROOT/'ps_sources/frontend'),'-I'+str(ROOT/'third_party/mbedtls/include'),
          str(ROOT/'scripts/fixtures/http_tls_host.c'),str(ROOT/'ps_sources/frontend/http_roots.c')]
    args += [str(p) for p in sorted((ROOT/'third_party/mbedtls/library').glob('*.c'))]
    args += ['-lws2_32','-o',str(executable)]
    subprocess.run(args,cwd=ROOT,env=env,check=True)
    subprocess.run([str(executable),str(ca)],cwd=ROOT,env=env,check=True,timeout=15)
    subprocess.run([str(executable),'embedded'],cwd=ROOT,env=env,check=True,timeout=15)

if __name__=='__main__': main()
