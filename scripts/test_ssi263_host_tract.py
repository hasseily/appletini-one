#!/usr/bin/env python3
"""Analytic charge tests for the optional SC-02 prototype tract, without Vivado.

The named capacitor fixtures and exact F1/F3 values come from the archived
dual-ssi263-sc02 testbench, not from listening targets or this C++ model.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest
import os

from test_ssi263_host_baseline import compiler

ROOT = Path(__file__).resolve().parents[1]
HOST = ROOT / "scripts" / "ssi263_host"

HARNESS = r'''
#include "prototype_tract.h"
#include <iostream>
#include <stdexcept>
using namespace ssi_host;
int checks = 0;
void check(bool ok, const char* what) {
    ++checks;
    if (!ok) throw std::runtime_error(what);
}
int rounded(int64_t n, int d) {
    return int(n < 0 ? -((-n + d/2) / d) : (n + d/2) / d);
}
void phase(PrototypeTract& t, FilterEvent& e, bool phi1) {
    e.phase=phi1; e.phase_edge=true; t.process(e); e.phase_edge=false;
}
int main() {
 try {
    // No input charge: clocks, arbitrary capacitor codes and route switches
    // must not manufacture a startup impulse from the deterministic zero seed.
    PrototypeTract silent;
    FilterEvent z;
    for (int n=0;n<4096;++n) {
        z.codes={n&15,(n>>1)&15,(n>>2)&15,(n>>3)&15,(n>>4)&15,
                 (n>>5)&15,(n>>6)&15,(n>>7)&15};
        z.fric1=n&4;z.fric2=n&8;
        phase(silent,z,n&1);
        check(silent.sample()==0,"zero charge produced sound");
    }
    check(silent.metrics().state_saturations==0,"zero charge saturated");

    // U116: 220/430/870/1800 pF selected independently, C205=3300 pF.
    const int caps[4]={220,430,870,1800};
    for(int mask=0;mask<16;++mask) {
        PrototypeTract t;
        FilterEvent e;
        e.voice_drive=-65141;e.codes.voice_amp=mask;
        phase(t,e,false);phase(t,e,true);
        int sum=0;for(int b=0;b<4;++b)if(mask&(1<<b))sum+=caps[b];
        check(t.state().voice==-rounded(int64_t(sum)*-65141,3300),"U116 capacitor sum");
        for(int b=0;b<4;++b)
            check(t.state().voice_plates[b]==((mask&(1<<b))?-65141:0),"U116 independent plates");
    }
    {
        PrototypeTract t;FilterEvent e;
        e.voice_drive=-65141;e.codes.voice_amp=1;
        phase(t,e,false);phase(t,e,true);
        check(t.state().voice==4343,"C201 transfer");
        e.codes.voice_amp=0;phase(t,e,false);
        check(t.state().voice==0&&t.state().voice_plates[0]==-65141,"C201 open plate retention");
        e.voice_drive=0;phase(t,e,true);e.codes.voice_amp=1;t.process(e);
        check(t.state().voice==-4343&&t.state().voice_plates[0]==0,"C201 reconnect equal opposite charge");
    }
    {
        PrototypeTract t;FilterEvent e;
        e.voice_drive=-65141;e.codes.voice_amp=7;
        phase(t,e,false);phase(t,e,true);
        const int first=-rounded(int64_t(1520)*-65141,3300);
        check(t.state().voice==first,"three U116 capacitors");
        e.codes.voice_amp=8;t.process(e);
        const int second=first-rounded(int64_t(1800)*-65141,3300);
        check(t.state().voice==second,"atomic U116 7 to 8 transfer");
        for(int p:t.state().voice_plates) check(p==-65141,"opened plate lost voltage");
        e.codes.voice_amp=7;t.process(e);
        check(t.state().voice==second,"disconnect invented U116 charge");
        const auto events=t.metrics().events;
        for(int n=0;n<100;++n)t.process(e);
        check(t.metrics().events==events&&t.state().voice==second,"unchanged held phase invented events");
    }
    {
        PrototypeTract t;FilterEvent e;
        e.fric_drive=301;e.codes.fric_amp=1;
        phase(t,e,true);phase(t,e,false);
        check(t.state().fric1==-21&&t.state().fric1_plates[0]==301,"U157 opposite phase transfer");
        e.codes.fric_amp=0;phase(t,e,true);
        check(t.state().fric1==0&&t.state().fric1_plates[0]==301,"U157 open retention");
        e.fric_drive=-301;phase(t,e,false);e.codes.fric_amp=1;t.process(e);
        check(t.state().fric1==42&&t.state().fric1_plates[0]==-301,"U157 bipolar reconnect");
    }
    // U152/U154 source steps: 4042 pF bank and C134=3900 pF. The
    // shaper responds with 19/13 in Phi0 and 31/13 in Phi1.
    for(int phi=0;phi<2;++phi) {
        PrototypeTract t(1,-301);FilterEvent e;
        e.fric_drive=-301;e.codes.fric_amp=15;t.process(e);
        if(phi)phase(t,e,true);
        e.fric_drive=301;t.process(e);
        check(t.state().fric2_source==-624,"U152 4042 pF source step");
        check(t.state().fric2_shape==(phi?1488:912),"U154 phase-dependent feedback");
        check(t.state().c150_delta==(phi?-624:0),"C150 source-edge phase gate");
        check(t.state().c151_delta==0,"open U159C leaked C151");
    }
    {
        PrototypeTract t(1,-301);FilterEvent e;
        e.fric_drive=-301;e.codes.fric_amp=15;e.fric2=true;t.process(e);
        phase(t,e,true);e.fric_drive=301;t.process(e);
        check(t.state().c150_delta==-624&&t.state().c151_delta==-624,"C150/C151 same source edge");
        const int held=t.state().output;t.process(e);
        check(t.state().output==held&&t.state().c151_delta==-624,"steady U152 re-transferred charge");
        phase(t,e,false);
        check(t.state().c150_delta==0&&t.state().c151_delta==0,"Phi0 leaked source into F5");
    }
    {
        // Archived exact mid-Phi1 fixture. A same-event rounded F1 change
        // must cross C127 into F3 even without another clock edge.
        PrototypeTract t;FilterEvent e;e.voice_drive=-65141;
        phase(t,e,false);phase(t,e,true);
        e.codes.voice_amp=15;t.process(e);
        check(t.state().voice==65536,"exact U116 reference level");
        check(t.state().formants[0].history==-30247&&t.state().formants[0].output==658,
              "mid-Phi1 C205/C128 F1 transfer");
        check(t.state().formants[2].history==-269&&t.state().formants[2].output==47,
              "C127 same-event F1 to F3 transfer");
    }
    {
        PrototypeTract t;FilterEvent e;
        e.voice_drive=-65141;e.codes={8,8,8,8,8,15,15,0};
        for(int n=0;n<32;++n){phase(t,e,false);phase(t,e,true);}
        for(const auto& f:t.state().formants)check(f.output!=0,"five-stage series continuity");
        check(t.state().output!=0,"U146 output continuity");
        e.output_open=false;t.process(e);const int held=t.state().reconstruction;
        for(int n=0;n<4;++n){phase(t,e,false);phase(t,e,true);}
        check(t.state().reconstruction==held,"U148 closed switch failed to hold");
        e.output_open=true;t.process(e);
        check(t.state().reconstruction==t.state().output,"U148 sampling pulse failed");
        e.codes.filter_amp=0;phase(t,e,false);
        e.codes.filter_amp=1;t.process(e);
        const int precharge=t.state().formants[4].output;
        check(t.state().filter_plates[0]==precharge,"filter amplitude Phi0 precharge");
        e.codes.filter_amp=0;t.process(e);phase(t,e,true);phase(t,e,false);
        check(t.state().filter_plates[0]==precharge,"open output-bank plate lost charge");
    }
    {
        // An F2 resonance capacitor can reconnect while Phi0 remains open.
        // Its stored plate joins the 7000 pF node in one charge-sharing step.
        PrototypeTract t;FilterEvent e;
        e.voice_drive=-65141;e.codes={8,8,1,8,8,15,15,0};
        for(int n=0;n<12;++n){phase(t,e,true);phase(t,e,false);}
        const int retained=t.state().f2q_plates[0];
        e.codes.f2q=0;t.process(e);
        phase(t,e,true);phase(t,e,false);
        check(t.state().f2q_plates[0]==retained,"open F2Q plate did not retain voltage");
        const int before=t.state().formants[1].history;
        e.codes.f2q=1;t.process(e);
        const int after=rounded(int64_t(7000)*before+int64_t(220)*retained,7220);
        check(t.state().formants[1].history==after&&t.state().f2q_plates[0]==after,
              "F2Q reconnect failed charge conservation");
        e.codes.f2q=0;t.process(e);
        check(t.state().formants[1].history==after,"F2Q disconnect invented charge");
    }
    {
        PrototypeTract t;FilterEvent e;e.fric_drive=301;e.codes.fric_amp=15;e.fric1=true;
        phase(t,e,true);phase(t,e,false);e.fric_drive=-301;t.process(e);
        check(t.state().voice==0&&t.state().fric1!=0&&t.state().formants[1].output!=0,
              "FRIC1 injection must enter F2");
        check(t.state().c143_plate==t.state().fric1,"FRIC1 capacitor failed to track active source");
    }
    {
        PrototypeTract t(1024);FilterEvent e;
        e.voice_drive=-8388608;e.codes={15,15,15,15,15,15,15,15};
        for(int n=0;n<2000;++n){phase(t,e,n&1);t.sample();}
        check(t.metrics().state_saturations>0,"rail-input saturation fixture did not exercise clamp");
        check(t.metrics().output_clips>0,"output clipping counter failed");
        for(const auto& f:t.state().formants)
            check(f.output>=-8388608&&f.output<=8388607,"charge state wrapped");
    }
    bool rejected=false;
    try {PrototypeTract t;FilterEvent e;e.codes.f1=16;t.process(e);}catch(const std::invalid_argument&){rejected=true;}
    check(rejected,"invalid capacitor mask accepted");
    std::cout<<"Prototype tract: "<<checks<<" analytic charge checks passed.\n";
 } catch(const std::exception& e) {std::cerr<<e.what()<<"\n";return 1;}
}
'''


class PrototypeTractTests(unittest.TestCase):
    def test_analytic_charge_fixtures(self):
        with tempfile.TemporaryDirectory(prefix="ssi263_tract_") as name:
            directory = Path(name)
            source = directory / "tract_test.cpp"
            source.write_text(HARNESS)
            executable = directory / ("tract_test.exe" if os.name == "nt" else "tract_test")
            command = [compiler(), "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror"]
            if os.name == "nt":
                command += ["-static", "-static-libgcc", "-static-libstdc++"]
            command += ["-I", str(HOST), str(source), str(HOST / "prototype_tract.cpp"), "-o", str(executable)]
            subprocess.run(command, check=True)
            result = subprocess.run([str(executable)], check=True, text=True, capture_output=True)
            print(result.stdout.strip())


if __name__ == "__main__":
    unittest.main()
