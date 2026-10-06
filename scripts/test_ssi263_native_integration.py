#!/usr/bin/env python3
"""Exercise production SSI bus/reset/IRQ and compare native PCM to frozen host.

No FPGA tools or physical chip are required. Uses Verilator (Ubuntu WSL on
Windows). These tests prove integration and retained bus policy, not new
claims about the production silicon or prototype reset nodes.
"""
from __future__ import annotations
import json
from pathlib import Path
import shutil
from ssi263_host_data import write_tables
from test_ssi263_native_engine import RTL as ENGINE_RTL, HOST, ROOT, linux_path, linux_run, digest
BUILD=ROOT/'build/test_ssi263_native_integration'
FIX=ROOT/'scripts/fixtures/ssi263_native'
RTL=[*ENGINE_RTL, *[ROOT/'hdl/apple'/f for f in ('ssi263_response_timing.sv','ssi263_bus_wrapper.sv','ssi263_voice.sv')],FIX/'integration_top.sv']

def main():
    BUILD.mkdir(parents=True,exist_ok=True)
    shutil.copyfile(ROOT/'hdl/apple/ssi263_sc02_rom.mem',BUILD/'ssi263_sc02_rom.mem')
    tables=BUILD/'tables.txt';write_tables(tables)
    results=[]
    hashes={str(p.relative_to(ROOT)).replace('\\','/'):digest(p) for p in [*RTL,FIX/'integration_bus.cpp', FIX/'response_timing.cpp']}
    for chip in (2,1):
        directory=BUILD/f'type-{chip}';directory.mkdir(exist_ok=True)
        command=['verilator','--cc','--exe','--build','-j','4','--top-module','integration_top',
                 f'-GCHIP_TYPE={chip}','--Mdir',linux_path(directory),'-CFLAGS',
                 f'-O2 -std=c++17 -DTEST_CHIP_TYPE={chip} -I'+linux_path(HOST),
                 *map(linux_path,RTL),linux_path(FIX/'integration_bus.cpp'),
                 *[linux_path(HOST/f) for f in ('native_control.cpp','native_source.cpp','prototype_tract.cpp')]]
        linux_run(command,directory/'compile.log',BUILD)
        output=linux_run([linux_path(directory/'Vintegration_top'),linux_path(tables)],directory/'simulation.log',BUILD)
        result=json.loads(output.strip().splitlines()[-1]);results.append(result)
        print(json.dumps(result),flush=True)
    timing=BUILD/'timing';timing.mkdir(exist_ok=True)
    linux_run(['verilator','--cc','--exe','--build','-j','4','--top-module','ssi263_response_timing',
               '--Mdir',linux_path(timing),'-CFLAGS','-O2 -std=c++17',
               linux_path(ROOT/'hdl/apple/ssi263_response_timing.sv'),linux_path(FIX/'response_timing.cpp')],
              timing/'compile.log',BUILD)
    output=linux_run([linux_path(timing/'Vssi263_response_timing')],timing/'simulation.log',BUILD)
    timing_result=json.loads(output.strip().splitlines()[-1]);print(json.dumps(timing_result),flush=True)
    if any(digest(ROOT/p)!=h for p,h in hashes.items()):
        raise RuntimeError('Sources changed during validation; rerun once stable')
    (BUILD/'validation.json').write_text(json.dumps({'results':results,'timing':timing_result,'sha256':hashes,
        'coverage':['accepted SSI bus writes and aliases 4..7','SSI263P/AP reset and retained registers/pitch',
                    'frame/phone repeating responses and DR00 masking','live RATE reload and ACK/CTL response coincidences',
                    'pending IRQ mode routing','optional Votrax address translated to native SSI',
                    'valid zero samples throughout AP reset','exact frozen-host PCM on 130-clock schedule'],
        'physical_ssi_behavior_verified':False},indent=2)+'\n')
if __name__=='__main__':main()
