#!/usr/bin/env python3
"""Profile the production SmartPort copy loop over the ONE//e RTL bus."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

from test_onee_disk2_boot import SOURCES, vivado_tool

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/smartport-copy-timing'


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    sources = [name for name in SOURCES if name not in
               ('hdl/apple/disk2_card.sv', 'hdl/sim/tb_onee_disk2_boot.sv')]
    sources += ['hdl/apple/linear_text_overlay_card.sv', 'hdl/apple/smartport_card.sv',
                'hdl/sim/tb_smartport_copy_timing.sv']
    roms = ['hdl/apple/smartport_a2retronet_style_c700.mem',
            'hdl/apple/smartport_a2retronet_style_c800.mem']
    for name in roms:
        shutil.copy2(ROOT / name, OUT / Path(name).name)
    rom = bytes(int(v, 16) for v in (ROOT / roms[1]).read_text().split())
    assert rom[0x1C5:0x1D1] == bytes.fromhex('adf0cf8df2cf9144c8d0f560')
    commands = [
        ('xvlog', ['--sv'] + [str(ROOT / name) for name in sources]),
        ('xelab', ['tb_smartport_copy_timing', '-s', 'copy_timing_snap',
                   '--timescale', '1ns/1ps', '-L', 'unisims_ver']),
        ('xsim', ['copy_timing_snap', '--runall']),
    ]
    for tool, args in commands:
        run = subprocess.run([vivado_tool(tool)] + args, cwd=OUT, text=True,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (OUT / (tool + '.log')).write_text(run.stdout, encoding='utf-8')
        if run.returncode:
            print(run.stdout[-6000:])
            raise RuntimeError(tool + ' failed')
        if tool == 'xsim':
            for line in run.stdout.splitlines():
                if 'COPY RESULT' in line or 'TIMING PASS' in line:
                    print(line, flush=True)
            assert 'SMARTPORT COPY TIMING PASS' in run.stdout
    provenance = {'scope': 'RTL simulation only, no board or firmware changes',
                  'sources': {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest()
                              for name in sources + roms}}
    (OUT / 'provenance.json').write_text(json.dumps(provenance, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
