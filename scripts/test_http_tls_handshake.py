"""Use the controlled peer to verify both restartable ECDHE suites and failure checks."""
import os
from pathlib import Path
import subprocess
from test_onee_input_service import find_native_c_compiler
ROOT=Path(__file__).resolve().parents[1]
def main():
    compiler=find_native_c_compiler()
    if not compiler:raise RuntimeError('Native C compiler required')
    out=ROOT/'build/http-service-tests';out.mkdir(exist_ok=True,parents=True)
    roots=out/'public-and-controlled.pem'
    roots.write_bytes((ROOT/'software/https_setup/ATNETCA.PEM').read_bytes()+b'\n'+(ROOT/'build/https-cpu0-checkpoint/trusted-ca.pem').read_bytes())
    env={k:v for k,v in os.environ.items() if k.lower()!='path'}
    env['PATH']=str(compiler.parent)+os.pathsep+next(v for k,v in os.environ.items() if k.lower()=='path')
    exe=out/'http_tls_handshake.exe'
    cmd=[str(compiler),'-std=c11','-O1','-DMBEDTLS_CONFIG_FILE=<http_service_mbedtls_config.h>',
         '-I'+str(ROOT/'ps_sources/frontend'),'-I'+str(ROOT/'third_party/mbedtls/include'),str(ROOT/'scripts/fixtures/http_tls_handshake_host.c')]
    cmd += [str(p) for p in sorted((ROOT/'third_party/mbedtls/library').glob('*.c'))]+['-lws2_32','-o',str(exe)]
    subprocess.run(cmd,env=env,cwd=ROOT,check=True)
    # TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256=0xC02B; RSA=0xC02F.
    cases=[(8443,'checkpoint.appletini.test',0,0xC02B),(8446,'checkpoint.appletini.test',0,0xC02F),
           (8443,'wrong.appletini.test',4,0xC02B),(8446,'wrong.appletini.test',4,0xC02F),
           (8444,'checkpoint.appletini.test',8,0xC02B),(8445,'checkpoint.appletini.test',1,0xC02B)]
    for port,name,flags,suite in cases:
        subprocess.run([str(exe),'10.17.1.17',str(port),name,str(roots),str(flags),str(suite)],env=env,cwd=ROOT,check=True,timeout=20)
    print('PASS live native ECDSA/RSA crypto yields, full public roots plus test CA, bad hostname/issuer/expiry and pool integrity')
if __name__=='__main__':main()
