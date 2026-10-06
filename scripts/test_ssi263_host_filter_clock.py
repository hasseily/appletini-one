#!/usr/bin/env python3
"""Check the native filter clock and pulse width together, without Vivado.

The FF=128 listening demo clocks the tract far below the datasheet's typical
20 kHz. This test observes delivered phase edges and actual source pulses;
it guards against an extra divide or an FF-independent pulse in the adapter.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess

from test_ssi263_host_control import compiler

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "scripts/ssi263_host"
HARNESS = r'''
#include "native_control.h"
#include "native_source.h"
#include "prototype_tract.h"
#include <iostream>
#include <stdexcept>
using namespace ssi_host;
void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
int main() {
    try {
        constexpr int ticks = 1015625;
        // Independent expected full-cycle periods, in effective XCK ticks.
        for (const auto setting : {std::pair<int,int>{128,256}, {231,50}}) {
            const int ff=setting.first, period=setting.second;
            Tables tables{};
            NativeControl control(tables);
            control.write(4,ff);
            NativeSource source;
            PrototypeTract tract;
            NativeSourceInputs inputs;
            inputs.codes={9,9,9,9,15,15,0,0};
            inputs.pw3=0; inputs.powered_down=false;
            inputs.inflection=2942; inputs.fric1=0; inputs.fric2=0;
            int last_phi0=-1, pulse_start=-1, cycles=0, pulses=0;
            bool voice_on=false;
            for(int tick=1; tick<=ticks; ++tick) {
                control.advance_xck(1);
                inputs.selector=control.selector();
                inputs.phase=control.filter_phase();
                inputs.phase_edge=control.filter_phase_edge();
                source.tick(inputs);
                const auto& s=source.output();
                if(s.phase_edge && !s.phase) {
                    if(last_phi0>=0) check(tick-last_phi0==period,
                        "delivered Phi0 period differs from datasheet clock");
                    last_phi0=tick; ++cycles;
                }
                const bool next_on=s.voice_target_q16!=0;
                if(next_on && !voice_on) pulse_start=tick;
                if(!next_on && voice_on) {
                    check(tick-pulse_start==4*period,
                          "U60 excitation must last four filter cycles");
                    ++pulses;
                }
                voice_on=next_on;
                tract.process({s.phase,s.phase_edge,
                    {s.f1,s.f2,s.f2q,s.f3,s.f4,s.filter_amp,s.voice_amp,s.fric_amp},
                    s.voice_target_q16,s.fric_drive_q16,s.fric1,s.fric2,s.output_open});
            }
            const auto& sm=source.metrics(); const auto& tm=tract.metrics();
            check(pulses>100,"source did not produce a full second of pitch pulses");
            check(sm.phi0_edges==static_cast<unsigned>(cycles),"source Phi0 accounting");
            check(tm.phase_edges==sm.phi0_edges+sm.phi1_edges,
                  "tract must receive every source phase edge exactly once");
            check(tm.state_saturations==0,"fixed-code clock experiment saturated");
            std::cout << "FF=" << ff << " full_period_xck=" << period
                      << " frequency_hz=" << double(ticks)/period
                      << " pulse_width_xck=" << 4*period
                      << " completed_pulses=" << pulses
                      << " delivered_phase_edges=" << tm.phase_edges << '\n';
        }
        return 0;
    } catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
'''


def main() -> None:
    out = ROOT / "build/ssi263_host/filter_clock_test"
    out.mkdir(parents=True, exist_ok=True)
    harness = out / "filter_clock.cpp"
    executable = out / ("filter_clock.exe" if os.name == "nt" else "filter_clock")
    harness.write_text(HARNESS)
    cxx = compiler()
    environment = os.environ.copy()
    environment["PATH"] = str(Path(cxx).parent) + os.pathsep + environment.get("PATH", "")
    command = [cxx, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
               "-I", str(SOURCE), str(harness), str(SOURCE / "native_control.cpp"),
               str(SOURCE / "native_source.cpp"), str(SOURCE / "prototype_tract.cpp"),
               "-o", str(executable)]
    subprocess.run(command, env=environment, check=True)
    result = subprocess.run([str(executable)], env=environment, capture_output=True,
                            text=True, check=True)
    (out / "result.json").write_text(json.dumps({"result": result.stdout}, indent=2) + "\n")
    print(result.stdout, end="")


if __name__ == "__main__":
    main()
