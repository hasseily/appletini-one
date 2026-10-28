"""Exercise the actual production protocol and JSON parser with a fake HTTP peer."""
import os
from pathlib import Path
import subprocess
from test_onee_input_service import find_native_c_compiler

ROOT = Path(__file__).resolve().parents[1]

def main():
    compiler = find_native_c_compiler()
    if compiler is None:
        raise RuntimeError('A native C compiler is required')
    out = ROOT / 'build/net-service-tests'
    out.mkdir(parents=True, exist_ok=True)
    env = {k:v for k,v in os.environ.items() if k.lower() != 'path'}
    env['PATH'] = str(compiler.parent) + os.pathsep + next(v for k,v in os.environ.items() if k.lower() == 'path')
    exe = out / 'net_service_host.exe'
    subprocess.run([str(compiler), '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-DAPPLETINI_HTTP_SERVICE=1', '-DNET_SERVICE_HOST=1',
                    '-I' + str(ROOT / 'ps_sources/frontend'),
                    *[str(ROOT / name) for name in ('scripts/fixtures/net_service_host.c',
                        'ps_sources/frontend/net_service.c', 'ps_sources/frontend/net_json.c')],
                    '-o', str(exe)], env=env, cwd=ROOT, check=True)
    subprocess.run([str(exe)], env=env, cwd=ROOT, check=True)

if __name__ == '__main__': main()
