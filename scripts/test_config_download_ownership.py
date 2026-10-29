#!/usr/bin/env python3
"""Run production config SD guards with pending DHCP/ONEe and live-FIL mocks."""
import os
from pathlib import Path
import subprocess
from test_onee_input_service import find_native_c_compiler

ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth, end = 1, brace + 1
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
    out = ROOT / 'build/config-download-ownership-tests'
    out.mkdir(parents=True, exist_ok=True)
    source = (ROOT / 'ps_sources/frontend/config_menu.c').read_text()
    signatures = (
        'FRESULT config_menu_mount_sd(',
        'static FRESULT config_menu_open_path(',
        'void config_menu_refresh_smartport_media_after_menu_sd(',
        'void config_menu_poll_ethernet(',
        'void config_menu_retry_settings_if_needed(',
        'void config_menu_poll_onee_mode(',
        'void config_menu_set_active(',
    )
    (out / 'config_sd_helpers.inc').write_text(
        '\n\n'.join(function(source, sig) for sig in signatures) + '\n')
    env = {k: v for k, v in os.environ.items() if k.lower() != 'path'}
    env['PATH'] = str(compiler.parent) + os.pathsep + next(
        (v for k, v in os.environ.items() if k.lower() == 'path'), '')
    executable = out / 'config_download_ownership.exe'
    subprocess.run([str(compiler), '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(out), str(ROOT / 'scripts/fixtures/config_download_ownership_host.c'),
                    '-o', str(executable)], cwd=ROOT, env=env, check=True)
    result = subprocess.run([str(executable)], cwd=ROOT, env=env, capture_output=True,
                            text=True, timeout=10)
    (out / 'RESULT.log').write_text(result.stdout + result.stderr)
    result.check_returncode()
    print(result.stdout, end='')


if __name__ == '__main__':
    main()
