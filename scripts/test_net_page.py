"""Run the ARM page service (web page to a short ASCII page) on the host."""
import os
from pathlib import Path
import subprocess
from test_onee_input_service import find_native_c_compiler

ROOT = Path(__file__).resolve().parents[1]


def build():
    compiler = find_native_c_compiler()
    if compiler is None:
        raise RuntimeError('A native C compiler is required')
    out = ROOT / 'build/net-page-tests'
    out.mkdir(parents=True, exist_ok=True)
    env = {k: v for k, v in os.environ.items() if k.lower() != 'path'}
    env['PATH'] = str(compiler.parent) + os.pathsep + next(v for k, v in os.environ.items() if k.lower() == 'path')
    exe = out / 'net_page_host.exe'
    subprocess.run([str(compiler), '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(ROOT / 'ps_sources/frontend'),
                    str(ROOT / 'scripts/fixtures/net_page_host.c'),
                    str(ROOT / 'ps_sources/frontend/net_page.c'), '-o', str(exe)],
                   env=env, cwd=ROOT, check=True)
    return exe, env


def main():
    exe, env = build()
    subprocess.run([str(exe)], env=env, cwd=ROOT, check=True)


if __name__ == '__main__':
    main()
