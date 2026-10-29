#!/usr/bin/env python3
"""Compile production printer SD helpers against a live-handle FatFs model."""
import os
from pathlib import Path
import subprocess
from test_onee_input_service import find_native_c_compiler

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    start = source.index('static FRESULT ' + name + '(')
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        if source[end] == '{':
            depth += 1
        elif source[end] == '}':
            depth -= 1
        end += 1
    return source[start:end]


def main():
    compiler = find_native_c_compiler()
    if not compiler:
        raise RuntimeError('Native C compiler required')
    out = ROOT / 'build/printer-sd-ownership-tests'
    out.mkdir(parents=True, exist_ok=True)
    source = (ROOT / 'ps_sources/frontend/printer_service.c').read_text()
    helpers = '\n\n'.join(function(source, name) for name in
                          ('printer_mount_sd', 'printer_ensure_dir', 'printer_open_output'))
    (out / 'printer_sd_helpers.inc').write_text(helpers + '\n')
    assert 'fr = printer_open_output(&file, path);' in source
    env = {k: v for k, v in os.environ.items() if k.lower() != 'path'}
    env['PATH'] = str(compiler.parent) + os.pathsep + next(
        (v for k, v in os.environ.items() if k.lower() == 'path'), '')
    executable = out / 'printer_sd_ownership.exe'
    subprocess.run([str(compiler), '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(out), str(ROOT / 'scripts/fixtures/printer_sd_ownership_host.c'),
                    '-o', str(executable)], cwd=ROOT, env=env, check=True)
    result = subprocess.run([str(executable)], cwd=ROOT, env=env, capture_output=True,
                            text=True, timeout=10)
    (out / 'RESULT.log').write_text(result.stdout + result.stderr)
    result.check_returncode()
    print(result.stdout, end='')


if __name__ == '__main__':
    main()
