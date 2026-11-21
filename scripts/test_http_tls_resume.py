"""Check TLS session reuse with the firmware's Mbed TLS configuration.

Without --live this only builds the native client. With --live it connects
twice to a public host (default a2cogs.com, the CoGS catalog host), with the
firmware's built-in roots, and requires that the second handshake resumes
the session of the first: same session ID and no key-exchange work.
"""
import argparse
import os
from pathlib import Path
import socket
import subprocess
from test_onee_input_service import find_native_c_compiler
ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--live', action='store_true', help='connect to the host')
    parser.add_argument('--host', default='a2cogs.com')
    args = parser.parse_args()
    compiler = find_native_c_compiler()
    if not compiler:
        raise RuntimeError('Native C compiler required')
    out = ROOT / 'build/http-service-tests'
    out.mkdir(exist_ok=True, parents=True)
    env = {k: v for k, v in os.environ.items() if k.lower() != 'path'}
    env['PATH'] = str(compiler.parent) + os.pathsep + next(v for k, v in os.environ.items() if k.lower() == 'path')
    exe = out / 'http_tls_resume.exe'
    cmd = [str(compiler), '-std=c11', '-O1', '-Wall', '-Wextra',
           '-DMBEDTLS_CONFIG_FILE=<http_service_mbedtls_config.h>',
           '-I' + str(ROOT / 'ps_sources/frontend'), '-I' + str(ROOT / 'third_party/mbedtls/include'),
           str(ROOT / 'scripts/fixtures/http_tls_resume_host.c'), str(ROOT / 'ps_sources/frontend/http_roots.c')]
    cmd += [str(p) for p in sorted((ROOT / 'third_party/mbedtls/library').glob('*.c'))] + ['-lws2_32', '-o', str(exe)]
    subprocess.run(cmd, env=env, cwd=ROOT, check=True)
    if not args.live:
        print('Built ' + str(exe) + '; run with --live to connect')
        return
    address = socket.gethostbyname(args.host)
    # The two suites that http_service.c offers: ECDSA 0xC02B, RSA 0xC02F.
    for suite in (0xC02B, 0xC02F):
        done = subprocess.run([str(exe), address, '443', args.host, str(suite)], env=env, cwd=ROOT,
                              capture_output=True, text=True, timeout=60)
        print('suite 0x%04X: %s' % (suite, (done.stdout + done.stderr).strip()))
        if done.returncode == 0:
            print('PASS the second handshake resumed the saved session of ' + args.host)
            return
    raise RuntimeError('No suite gave a resumed session on ' + args.host)


if __name__ == '__main__':
    main()
