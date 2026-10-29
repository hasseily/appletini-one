"""Check production trust loading with embedded DER and optional SD overrides."""
import os
from pathlib import Path
import subprocess
import warnings

from cryptography import x509
from cryptography.hazmat.primitives.serialization import Encoding
from test_onee_input_service import find_native_c_compiler

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'build/http-trust-tests'

def main():
    compiler=find_native_c_compiler()
    if not compiler:raise RuntimeError('Native C compiler required')
    OUT.mkdir(parents=True,exist_ok=True)
    source=(ROOT/'ps_sources/frontend/http_service.c').read_text()
    start=source.index('static int trust_load(void)')
    end=source.index('\nint http_service_boot_prepare(',start)
    (OUT/'trust_load.inc').write_text(source[start:end])
    with warnings.catch_warnings():
        warnings.simplefilter('ignore')
        first=x509.load_pem_x509_certificates((ROOT/'software/https_setup/ATNETCA.PEM').read_bytes())[0]
    single=OUT/'single-public-root.pem';single.write_bytes(first.public_bytes(Encoding.PEM))
    env={k:v for k,v in os.environ.items() if k.lower()!='path'}
    env['PATH']=str(compiler.parent)+os.pathsep+next(v for k,v in os.environ.items() if k.lower()=='path')
    executable=OUT/'http_trust_host.exe'
    args=[str(compiler),'-std=c11','-O1','-Wall','-Wextra',
          '-DMBEDTLS_CONFIG_FILE=<http_service_mbedtls_config.h>',
          '-I'+str(OUT),'-I'+str(ROOT/'ps_sources/frontend'),'-I'+str(ROOT/'third_party/mbedtls/include'),
          str(ROOT/'scripts/fixtures/http_trust_host.c'),str(ROOT/'ps_sources/frontend/http_roots.c')]
    args += [str(p) for p in sorted((ROOT/'third_party/mbedtls/library').glob('*.c'))]
    args += ['-lws2_32','-o',str(executable)]
    subprocess.run(args,cwd=ROOT,env=env,check=True)
    subprocess.run([str(executable),str(single)],cwd=ROOT,env=env,check=True,timeout=15)

if __name__=='__main__':main()
