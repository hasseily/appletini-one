#!/usr/bin/env python3
"""Run actual W5100S layout and ring helpers against command/status MMIO."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
from test_onee_input_service import find_native_c_compiler
ROOT=Path(__file__).resolve().parents[1]


def main():
    out=ROOT/'build/w5100-update-layout-tests';out.mkdir(parents=True,exist_ok=True)
    source=ROOT/'ps_sources/frontend/uthernet2_control.c';text=source.read_text()
    barrier='__asm__ volatile ("dsb sy" ::: "memory");'
    assert text.count(barrier)==1
    generated=out/'uthernet2_control_host.c';generated.write_text(text.replace(barrier,'__asm__ volatile ("" ::: "memory");'))
    compiler=find_native_c_compiler();assert compiler
    env={k:v for k,v in os.environ.items() if k.lower()!='path'}
    env['PATH']=str(compiler.parent)+os.pathsep+next(v for k,v in os.environ.items() if k.lower()=='path')
    exe=out/'w5100-layout.exe'
    command=[str(compiler),'-std=c11','-O1','-Wall','-Wextra',
             '-DW5100_TEST_SOURCE="'+generated.as_posix()+'"',
             '-I'+str(ROOT/'ps_sources/frontend'),'-I'+str(ROOT/'scripts/fixtures/online_update_perf_host'),
             '-I'+str(ROOT/'scripts/fixtures/http_service_host'),str(ROOT/'scripts/fixtures/w5100_update_layout_host.c'),'-o',str(exe)]
    with (out/'BUILD.log').open('w') as log:subprocess.run(command,cwd=ROOT,env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
    done=subprocess.run([str(exe)],cwd=ROOT,env=env,capture_output=True,text=True,timeout=20)
    (out/'RESULT.log').write_text(done.stdout+done.stderr);print(done.stdout+done.stderr,end='');done.check_returncode()
    (out/'validation.json').write_text(json.dumps(dict(status='PASS',hardware_test=False,
        production_source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
        host_substitution='ARM dsb sy replaced by empty compiler memory barrier; controller code otherwise unchanged',
        stdout=done.stdout),indent=2)+'\n')


if __name__=='__main__':main()
