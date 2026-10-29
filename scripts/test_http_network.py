"""Exercise the production HTTP engine with modeled W5100S failure flags."""
import os
from pathlib import Path
import subprocess
from test_onee_input_service import find_native_c_compiler

ROOT = Path(__file__).resolve().parents[1]

def main():
    compiler = find_native_c_compiler()
    if not compiler:
        raise RuntimeError('Native C compiler required')
    out = ROOT / 'build/http-service-tests'
    out.mkdir(parents=True, exist_ok=True)
    env = {k:v for k,v in os.environ.items() if k.lower() != 'path'}
    env['PATH'] = str(compiler.parent) + os.pathsep + next(v for k,v in os.environ.items() if k.lower() == 'path')
    executable = out / 'http_network_host.exe'
    args = [str(compiler), '-std=c11', '-O1', '-Wall', '-Wextra',
            '-DAPPLETINI_HTTP_SERVICE=1', '-DMBEDTLS_CONFIG_FILE=<http_service_mbedtls_config.h>',
            '-I'+str(ROOT/'scripts/fixtures/http_service_host'),
            '-I'+str(ROOT/'ps_sources/frontend'), '-I'+str(ROOT/'third_party/mbedtls/include'),
            str(ROOT/'scripts/fixtures/http_network_host.c'),
            str(ROOT/'ps_sources/frontend/http_core.c'), str(ROOT/'ps_sources/frontend/http_seed.c'),
            str(ROOT/'ps_sources/frontend/http_roots.c'),
            str(ROOT/'ps_sources/lib/crc32.c')]
    args += [str(p) for p in sorted((ROOT/'third_party/mbedtls/library').glob('*.c'))]
    args += ['-lws2_32', '-o', str(executable)]
    subprocess.run(args, cwd=ROOT, env=env, check=True)
    subprocess.run([str(executable)], cwd=ROOT, env=env, check=True, timeout=15)

if __name__ == '__main__':
    main()
