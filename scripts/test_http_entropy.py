#!/usr/bin/env python3
"""Exercise the real XADC conditioner with deterministic raw-word adapters."""
import hashlib
import os
from pathlib import Path
import re
import subprocess
from test_onee_input_service import find_native_c_compiler

ROOT = Path(__file__).resolve().parents[1]
DOMAIN = b'Appletini experimental XADC raw12 v1'


def expected_seed():
    state = 0x319A27D5
    raw = bytearray()
    for _ in range(4096):
        state ^= (state << 13) & 0xffffffff
        state ^= state >> 17
        state ^= (state << 5) & 0xffffffff
        raw.extend((2048 + (state & 3)).to_bytes(2, 'little'))
    digest = hashlib.sha256(DOMAIN + raw).digest()
    return (hashlib.sha256(DOMAIN + b'\0' + digest).digest() +
            hashlib.sha256(DOMAIN + b'\1' + digest).digest()[:16]).hex()


def main():
    compiler = find_native_c_compiler()
    if compiler is None:
        raise RuntimeError('Native C compiler required')
    out = ROOT/'build/http-entropy-tests'
    out.mkdir(parents=True, exist_ok=True)
    fixture = ROOT/'scripts/fixtures/http_entropy_host'
    env = {k: v for k, v in os.environ.items() if k.lower() != 'path'}
    env['PATH'] = str(compiler.parent) + os.pathsep + next(
        (v for k, v in os.environ.items() if k.lower() == 'path'), '')
    executable = out/'http_entropy_host.exe'
    args = [str(compiler), '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
            '-DHTTP_ENTROPY_TEST=1',
            '-DMBEDTLS_CONFIG_FILE=<http_entropy_mbedtls_config.h>',
            '-I'+str(fixture), '-I'+str(ROOT/'ps_sources/frontend'),
            '-I'+str(ROOT/'third_party/mbedtls/include'),
            str(ROOT/'scripts/fixtures/http_entropy_host.c'),
            str(ROOT/'ps_sources/frontend/http_entropy.c'),
            str(ROOT/'third_party/mbedtls/library/sha256.c'),
            str(ROOT/'third_party/mbedtls/library/platform_util.c'),
            str(ROOT/'third_party/mbedtls/library/platform.c'),
            '-o', str(executable)]
    subprocess.run(args, cwd=ROOT, env=env, check=True)
    results = {}
    for case in ('varied', 'varied-padding', 'different', 'two-codes', 'stuck',
                 'padding', 'cycle', 'cycle16', 'biased', 'upper-only', 'timeout',
                 'wall-timeout', 'frozen-time', 'unavailable', 'restore'):
        result = subprocess.run([str(executable), case], cwd=ROOT, env=env,
                                check=True, capture_output=True, text=True, timeout=10)
        results[case] = re.search(r'seed=([0-9a-f]{96})', result.stdout)[1]
        print(result.stdout.split(' seed=')[0])
    assert results['varied'] == results['varied-padding'] == expected_seed()
    assert results['varied'] != results['different']
    print('PASS XADC conditioner: 15 cases, real SHA-256, padding ignored, once-per-boot')
    mmio_executable = out/'http_entropy_mmio_host.exe'
    mmio_args = [a for a in args if a != '-DHTTP_ENTROPY_TEST=1']
    mmio_args[mmio_args.index(str(ROOT/'scripts/fixtures/http_entropy_host.c'))] = str(
        ROOT/'scripts/fixtures/http_entropy_mmio_host.c')
    mmio_args[mmio_args.index(str(executable))] = str(mmio_executable)
    subprocess.run(mmio_args, cwd=ROOT, env=env, check=True)
    for case in ('varied', 'por-reset', 'clock-off', 'clock-fast', 'pll-unlocked',
                 'busy', 'fifo-timeout', 'save-timeout', 'config-mismatch', 'restore-mismatch',
                 'persistent-timeout', 'stale', 'por-stale', 'late-response'):
        result = subprocess.run([str(mmio_executable), case], cwd=ROOT, env=env,
                                check=True, capture_output=True, text=True, timeout=10)
        print(result.stdout.strip())
    print('PASS XADC hardware adapter: 14 MMIO cases, POR/reset, FIFO pipeline, restoration')


if __name__ == '__main__':
    main()
