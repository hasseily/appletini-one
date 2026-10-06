#include "Vtract_tb.h"
#include "prototype_tract.h"
#include <verilated.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

using ssi_host::FilterEvent;
using ssi_host::PrototypeTract;

namespace {
uint32_t random_state = 0x26302025;
uint32_t random_word() {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}
uint32_t packed_codes(const FilterEvent& e) {
    const auto& c=e.codes;
    return uint32_t(c.f1) | uint32_t(c.f2)<<4 | uint32_t(c.f2q)<<8 |
        uint32_t(c.f3)<<12 | uint32_t(c.f4)<<16 | uint32_t(c.filter_amp)<<20 |
        uint32_t(c.voice_amp)<<24 | uint32_t(c.fric_amp)<<28;
}
void codes_from_word(FilterEvent& e, uint32_t word) {
    auto& c=e.codes;
    int* fields[]={&c.f1,&c.f2,&c.f2q,&c.f3,&c.f4,&c.filter_amp,&c.voice_amp,&c.fric_amp};
    for (int k=0;k<8;++k) *fields[k]=(word>>(k*4))&15;
}
std::array<int,61> flat_state(const PrototypeTract::State& s) {
    std::array<int,61> v{};
    v[0]=s.voice; v[1]=s.fric1; v[2]=s.fric2_source; v[3]=s.fric2_shape;
    for (int k=0;k<4;++k) {
        v[4+k]=s.voice_plates[k]; v[8+k]=s.fric1_plates[k];
        v[12+k]=s.f2q_plates[k]; v[16+k]=s.filter_plates[k];
    }
    for (int k=0;k<5;++k) {
        v[20+k*7]=s.formants[k].output;
        v[21+k*7]=s.formants[k].history;
        v[22+k*7]=s.formants[k].fixed_plate;
        for (int j=0;j<4;++j) v[23+k*7+j]=s.formants[k].plates[j];
    }
    v[55]=s.c143_plate; v[56]=s.c151_plate; v[57]=s.c150_delta;
    v[58]=s.c151_delta; v[59]=s.output; v[60]=s.reconstruction;
    return v;
}
class Test {
public:
    Vtract_tb rtl;
    PrototypeTract reference{8};
    unsigned events{}, max_cycles{}, checks{};
    uint64_t total_saturations{};
    void require(bool condition, const std::string& message) {
        ++checks;
        if (!condition) throw std::runtime_error("event "+std::to_string(events)+": "+message);
    }
    void cycle() { rtl.clk=0; rtl.eval(); rtl.clk=1; rtl.eval(); }
    void reset() {
        total_saturations+=reference.metrics().state_saturations;
        rtl.event_valid=0; rtl.rstn=0; cycle(); rtl.rstn=1; cycle();
        reference=PrototypeTract(8);
        require(!rtl.busy && rtl.event_ready && !rtl.event_done && !rtl.fault && !rtl.state_saturated,"cold reset outputs");
        compare();
    }
    void drive(const FilterEvent& e) {
        rtl.phase=e.phase; rtl.phase_edge=e.phase_edge; rtl.codes=packed_codes(e);
        rtl.voice_drive=uint32_t(e.voice_drive)&0xffffff;
        rtl.fric_drive=uint32_t(e.fric_drive)&0x3ffff;
        rtl.fric1_route=e.fric1; rtl.fric2_route=e.fric2; rtl.output_open=e.output_open;
    }
    void compare() {
        const auto values=flat_state(reference.state());
        for (unsigned k=0;k<values.size();++k) {
            const auto actual=static_cast<int32_t>(rtl.debug_state[k]);
            require(actual==values[k],"charge["+std::to_string(k)+"] RTL="+
                std::to_string(actual)+" host="+std::to_string(values[k]));
        }
        require(static_cast<int16_t>(rtl.sample)==reference.sample(),"PCM sample differs");
        require(bool(rtl.state_saturated)==bool(reference.metrics().state_saturations),"24-bit saturation flag differs");
    }
    void event(const FilterEvent& e, unsigned period=130) {
        require(rtl.event_ready && !rtl.busy,"event would miss XCK deadline");
        ++events; drive(e); rtl.event_valid=1; reference.process(e); cycle();
        rtl.event_valid=0;
        unsigned elapsed=0;
        while (!rtl.event_done) {
            require(elapsed<120,"event exceeded 120-clock bound");
            cycle(); ++elapsed;
        }
        max_cycles=std::max(max_cycles,elapsed);
        require(!rtl.busy && rtl.event_ready && !rtl.fault,"completion contract");
        compare();
        // Exactly period clocks between event acceptances, including fast
        // phase edges at each XCK and events whose source/code fields change.
        require(elapsed+1<=period,"event exceeded requested input cadence");
        for (unsigned k=elapsed+1;k<period;++k) { cycle(); require(!rtl.event_done,"event_done is not a pulse"); }
    }
};
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc,argv);
    try {
        Test t; t.reset();
        FilterEvent e;
        // Identity events and output-open-only events do no charge work.
        for (int k=0;k<32;++k) { e.output_open=bool(k&1); t.event(e); }
        // All nibbles, zero stimulus: switching alone cannot create charge.
        for (int bank=0;bank<8;++bank) for (int code=0;code<16;++code) for (int phase=0;phase<2;++phase) {
            codes_from_word(e,uint32_t(code)<<(bank*4)); e.phase=bool(phase); e.phase_edge=true;
            t.event(e); t.require(t.reference.state().output==0,"zero-input code change generated output");
        }
        // At FF=255 the phase may change at every effective XCK. Every event
        // also changes source voltage and/or capacitor codes. No queue slack.
        for (int k=0;k<8192;++k) {
            codes_from_word(e,random_word()); e.phase=!e.phase; e.phase_edge=true;
            e.voice_drive=(k&4)?-2048:0; e.fric_drive=(k&1)?301:-301;
            e.fric1=bool(k&2); e.fric2=bool(k&4); e.output_open=!e.phase;
            t.event(e);
        }
        t.require(t.max_cycles==111,"expected longest pipeline schedule not reached");
        // Exercise the documented tightest independent-tract acceptance
        // period as well as the physical 130-clock XCK spacing above.
        for (int k=0;k<128;++k) {
            e.phase=true; e.phase_edge=true; codes_from_word(e,random_word());
            e.voice_drive=(k&1)?-2048:0; e.fric_drive=(k&1)?301:-301;
            t.event(e,112);
        }
        t.reset();
        // Held-phase source and mask edits retain each disconnected plate.
        // Includes 7->8->7 banks, all F2Q attach/remove combinations, both
        // signs near source limits and plenty of intentional state saturation.
        for (int k=0;k<10000;++k) {
            if (k%4==0) codes_from_word(e,random_word());
            if (k%31==0) codes_from_word(e,(k&1)?0x77777777u:0x88888888u);
            e.phase=bool((k/17)&1); e.phase_edge=k%17==0;
            e.voice_drive=static_cast<int>(random_word()&0xffffff)-8388608;
            e.fric_drive=static_cast<int>(random_word()&0x3ffff)-131072;
            e.fric1=bool(k&2); e.fric2=bool(k&8); e.output_open=bool(k%3);
            t.event(e);
            if (k%71==0) { e.phase_edge=false; e.output_open=!e.output_open; t.event(e); }
        }
        t.require(t.reference.metrics().state_saturations>100,"saturation paths not exercised");
        // Confirm that a busy input is reported as a contract failure rather
        // than corrupting the accepted event. The producer must never do this.
        e.phase_edge=true; t.drive(e); t.reference.process(e); t.rtl.event_valid=1; t.cycle();
        t.require(t.rtl.busy,"busy violation test did not start");
        t.cycle(); t.rtl.event_valid=0;
        t.require(t.rtl.fault,"busy event was not flagged");
        while (!t.rtl.event_done) t.cycle();
        t.compare(); t.cycle(); t.require(t.rtl.fault,"fault is not sticky");
        t.reset();
        std::cout<<"SSI263 NATIVE TRACT PASS events="<<t.events<<" checks="<<t.checks
            <<" max_cycles="<<t.max_cycles<<" saturations="<<t.total_saturations<<"\n";
        return 0;
    } catch (const std::exception& e) { std::cerr<<e.what()<<"\n"; return 1; }
}
