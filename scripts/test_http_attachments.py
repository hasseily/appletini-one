#!/usr/bin/env python3
"""Run production attachment parsing and same-request stream ownership tests."""
import os
from pathlib import Path
import subprocess
from test_onee_input_service import find_native_c_compiler

ROOT = Path(__file__).resolve().parents[1]


def main():
    compiler = find_native_c_compiler()
    if not compiler:
        raise RuntimeError('Native C compiler required')
    out = ROOT / 'build/http-attachment-tests'
    out.mkdir(parents=True, exist_ok=True)
    env = {k: v for k, v in os.environ.items() if k.lower() != 'path'}
    env['PATH'] = str(compiler.parent) + os.pathsep + next(
        (v for k, v in os.environ.items() if k.lower() == 'path'), '')
    core = [str(ROOT / 'ps_sources/frontend/http_core.c')]
    service = [str(ROOT / ('ps_sources/' + name)) for name in
               ('frontend/http_seed.c', 'frontend/http_roots.c', 'lib/crc32.c')]
    service += [str(p) for p in sorted((ROOT / 'third_party/mbedtls/library').glob('*.c'))]
    for name, sources, definitions in (
            ('parser', core, []),
            ('service', core + service, ['-DAPPLETINI_HTTP_SERVICE=1',
                                        '-DMBEDTLS_CONFIG_FILE=<http_service_mbedtls_config.h>'])):
        executable = out / ('attachment_' + name + '.exe')
        args = [str(compiler), '-std=c11', '-O1', '-Wall', '-Wextra', *definitions,
                '-I' + str(ROOT / 'scripts/fixtures/http_service_host'),
                '-I' + str(ROOT / 'ps_sources/frontend'),
                '-I' + str(ROOT / 'third_party/mbedtls/include'),
                str(ROOT / ('scripts/fixtures/http_attachment_' + name + '_host.c')),
                *sources, '-o', str(executable)]
        if name == 'service':
            args += ['-lws2_32']
        with (out / (name + '-BUILD.log')).open('w') as log:
            subprocess.run(args, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
        result = subprocess.run([str(executable)], cwd=ROOT, env=env, capture_output=True,
                                text=True, timeout=20)
        (out / (name + '-RESULT.log')).write_text(result.stdout + result.stderr)
        print(result.stdout + result.stderr, end='')
        result.check_returncode()


if __name__ == '__main__':
    main()
