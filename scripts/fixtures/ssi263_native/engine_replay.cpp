// Whole-engine parity against the frozen listening renderer. Idle fabric
// clocks are skipped; every active pipeline clock executes and is counted.
#include "Vssi263_native_engine.h"
#include "baseline.h"
#include "native_control.h"
#include "native_source.h"
#include "prototype_tract.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
using namespace ssi_host;

static void require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}
static Tables read_tables(const char* path) {
    std::ifstream in(path);
    Tables t;
    for (auto& p : t.phones)
        for (int* v : {&p.f1,&p.va,&p.f2,&p.fc,&p.f2q,&p.f3,&p.fa,
                       &p.cld,&p.vd,&p.closure,&p.duration,&p.pause}) in >> *v;
    for (auto& n : t.native_rom) in >> n;
    for (auto* map : {&t.native_f1,&t.native_f2,&t.native_f2q,&t.native_f3,&t.native_va,&t.native_fa})
        for (auto& n : *map) in >> n;
    for (auto& n : t.sc01_map) in >> n;
    for (auto& row : t.coeff_f1) for (auto& n : row) in >> n;
    for (auto& bank : t.coeff_f2) for (auto& row : bank) for (auto& n : row) in >> n;
    for (auto& row : t.coeff_f3) for (auto& n : row) in >> n;
    for (auto& n : t.coeff_f4) in >> n;
    for (auto& n : t.coeff_fn) in >> n;
    for (auto& n : t.coeff_fx) in >> n;
    require(bool(in), "tables truncated");
    return t;
}
static int64_t floor_div(int64_t a, int64_t b) { return a/b - (a%b < 0); }
static int64_t ceil_div(int64_t a, int64_t b) { return -floor_div(-a,b); }
struct Event { int64_t tick; int socket, reg, value; };

// Execute every fabric clock here: Q3 every65, effective XCK every130, and
// sample requests every2771 clocks. Their phases sweep past each pipeline
// stage. Writes also occur during jobs and on clock/sample boundaries.
static int timed_contracts(const Tables& tables) {
    auto a=std::make_unique<Vssi263_native_engine>();
    auto b=std::make_unique<Vssi263_native_engine>();
    Vssi263_native_engine* dut[2]={a.get(),b.get()};
    Baseline pitch[2]={Baseline(tables),Baseline(tables)};
    NativeControl control[2]={NativeControl(tables),NativeControl(tables)};
    NativeSource source[2];
    PrototypeTract tract[2]={PrototypeTract(1),PrototypeTract(1)};
    bool pending[2]={false,false};
    int16_t expected[2]={0,0};
    int checks=0;
    auto cycle=[&] {
        for(auto d:dut) { d->clk=0;d->eval(); }
        for(auto d:dut) { d->clk=1;d->eval(); }
    };
    auto reset=[&] {
        for(auto d:dut) { d->warm_reset=0;d->mode_latch=0;d->rstn=0;d->xck_ce=0;d->write_strobe=0;d->audio_tick=0; }
        cycle();cycle();
        for(auto d:dut) d->rstn=1;
        cycle();
        for(auto d:dut) require(!d->fault&&!d->busy&&!d->audio_valid,"reset contract");
    };
    reset();
    const int setup_reg[]={0,1,2,4,3};
    const int setup_value[]={0xc0,255,255,255,0x7f};
    for(int k=0;k<5;++k) {
        for(int s=0;s<2;++s) {
            dut[s]->write_strobe=1; dut[s]->write_reg=setup_reg[k];
            dut[s]->write_data=setup_value[k];
            pitch[s].write(setup_reg[k],setup_value[k]);
            control[s].write(setup_reg[k],setup_value[k]);
        }
        cycle();
    }
    for(auto d:dut) d->write_strobe=0;
    bool div2=false;
    for(int n=0;n<130*8192+200;++n) {
        const bool raw=n<130*8192 && n%65==0;
        const bool effective=raw&&div2;
        if(raw) div2=!div2;
        const bool sample=n<130*8192 && (n%2771==37 || n==65);
        // Include simultaneous XCK+write, writes during source/tract work,
        // AMP zero and power-down/release without resetting the circuitry.
        for(int s=0;s<2;++s) {
            auto d=dut[s];
            d->write_strobe=0;d->xck_ce=raw;d->audio_tick=sample;
            if(n<130*8192 && (n%1009==s || n==65)) {
                const int sequence=n/1009;
                const int reg=sequence%5;
                const int value=reg==4?255:reg==3?
                    (sequence%17==0?0x80:sequence%13==0?0:0x7f):
                    (sequence*37+s*53)&255;
                d->write_strobe=1;d->write_reg=reg;d->write_data=value;
                pitch[s].write(reg,value);control[s].write(reg,value);
            }
            if(effective) {
                control[s].set_ampct_zero(source[s].ampct_zero());
                control[s].advance_xck(1);
                const int rate=pitch[s].register_value(2);
                const int live=((rate&8)<<8)|(pitch[s].register_value(1)<<3)|(rate&7);
                source[s].tick(control[s],pitch[s].current_function()==3?pitch[s].active_inflection():live);
                const auto& o=source[s].output();
                tract[s].process({o.phase,o.phase_edge,
                    {o.f1,o.f2,o.f2q,o.f3,o.f4,o.filter_amp,o.voice_amp,o.fric_amp},
                    o.voice_target_q16,o.fric_drive_q16,o.fric1,o.fric2,o.output_open});
            }
            if(sample) {
                require(!pending[s],"overlapping reference sample");
                expected[s]=tract[s].sample();pending[s]=true;
                (void)pitch[s].sample();
            }
        }
        cycle();
        for(int s=0;s<2;++s) {
            require(!dut[s]->fault,"valid130-clock schedule fault atfabric="+std::to_string(n));
            if(dut[s]->audio_valid) {
                require(pending[s],"unsolicited sample");
                require(static_cast<int16_t>(dut[s]->audio)==expected[s],
                        "timed PCM mismatch atfabric="+std::to_string(n)+" socket="+std::to_string(s));
                pending[s]=false;++checks;
            }
        }
    }
    require(!pending[0]&&!pending[1],"sample request lost at end");
    // Bad scheduling must be visible, and reset must clear the sticky fault.
    reset();
    for(auto d:dut) d->xck_ce=1;
    cycle();cycle(); // second physical pulse starts one effective tick
    for(auto d:dut) { d->xck_ce=0;d->audio_tick=1; }
    cycle();cycle(); // repeated sample request while the source is busy
    for(auto d:dut) require(d->fault,"sample overrun must latch fault");
    reset();
    for(auto d:dut) d->xck_ce=1;
    cycle();cycle();cycle();cycle();
    for(auto d:dut) require(d->fault,"XCK overrun must latch fault");
    reset();
    return checks;
}

int main(int argc, char** argv) {
    try {
        require(argc == 4, "engine_replay TABLES EVENTS OUTPUT.pcm");
        Verilated::commandArgs(argc, argv);
        const auto tables = read_tables(argv[1]);
        std::ifstream trace(argv[2]);
        std::string magic;
        int hz;
        int64_t start, end;
        size_t count;
        trace >> magic >> hz >> start >> end >> count;
        require(bool(trace) && magic == "SSIHOST1", "bad trace header");
        std::vector<Event> events(count);
        for (auto& e : events) trace >> e.tick >> e.socket >> e.reg >> e.value;
        require(bool(trace), "truncated events");
        std::ofstream pcm(argv[3], std::ios::binary);
        Baseline pitch[2] = {Baseline(tables,hz), Baseline(tables,hz)};
        NativeControl control[2] = {NativeControl(tables,hz), NativeControl(tables,hz)};
        NativeSource source[2];
        PrototypeTract tract[2] = {PrototypeTract(1), PrototypeTract(1)};
        auto a = std::make_unique<Vssi263_native_engine>();
        auto b = std::make_unique<Vssi263_native_engine>();
        Vssi263_native_engine* dut[2] = {a.get(), b.get()};
        uint64_t cycles = 0, ticks = 0, frames = 0, samples = 0, writes = 0;
        int max_latency = 0;
        auto cycle = [&] {
            for (auto d : dut) { d->clk=0; d->eval(); }
            for (auto d : dut) { d->clk=1; d->eval(); }
            ++cycles;
        };
        for (auto d : dut) {
            d->warm_reset=0;d->mode_latch=0;d->rstn=0; d->xck_ce=0; d->write_strobe=0; d->audio_tick=0;
            d->write_reg=0; d->write_data=0;
        }
        cycle(); cycle();
        for (auto d : dut) d->rstn=1;
        cycle();
        auto check_fault = [&] {
            for (int s=0;s<2;++s)
                require(!dut[s]->fault, "native schedule/arithmetic fault socket="+std::to_string(s)+
                        " tick="+std::to_string(ticks));
        };
        auto tick = [&] {
            for (int s=0;s<2;++s) {
                control[s].set_ampct_zero(source[s].ampct_zero());
                control[s].advance_xck(1);
                const int rate = pitch[s].register_value(2);
                const int live = ((rate&8)<<8)|(pitch[s].register_value(1)<<3)|(rate&7);
                source[s].tick(control[s],pitch[s].current_function()==3?pitch[s].active_inflection():live);
                const auto& o = source[s].output();
                tract[s].process({o.phase,o.phase_edge,
                    {o.f1,o.f2,o.f2q,o.f3,o.f4,o.filter_amp,o.voice_amp,o.fric_amp},
                    o.voice_target_q16,o.fric_drive_q16,o.fric1,o.fric2,o.output_open});
            }
            // Default DIV2: only the second Q3 enable advances the engine.
            for (auto d : dut) d->xck_ce=1;
            cycle();
            for (auto d : dut) d->xck_ce=0;
            cycle();
            for (auto d : dut) d->xck_ce=1;
            cycle();
            for (auto d : dut) d->xck_ce=0;
            bool done[2] = {false,false};
            int latency=0;
            do {
                cycle(); ++latency;
                for (int s=0;s<2;++s) done[s] = done[s] || dut[s]->tick_done;
                require(latency<128, "effective XCK deadline exceeded");
            } while (!done[0] || !done[1]);
            max_latency=std::max(max_latency,latency);
            for (int s=0;s<2;++s)
                require(dut[s]->duration_phase==control[s].duration_phase(), "duration parity");
            check_fault();
            ++ticks;
        };
        auto write = [&](const Event& e) {
            pitch[e.socket].write(e.reg,e.value);
            control[e.socket].write(e.reg,e.value);
            dut[e.socket]->write_strobe=1;
            dut[e.socket]->write_reg=e.reg;
            dut[e.socket]->write_data=e.value;
            cycle();
            dut[e.socket]->write_strobe=0;
            ++writes;
        };
        const int64_t first_tick=events.empty()?0:std::min<int64_t>(0,events[0].tick);
        const int64_t first_frame=floor_div(first_tick*48000,hz);
        const int64_t first_output=ceil_div(start*48000,hz), last_frame=ceil_div(end*48000,hz);
        int64_t now=ceil_div(first_frame*hz,48000);
        size_t next=0;
        auto advance = [&](int64_t target) { while(now<target) { tick(); ++now; } };
        for (int64_t frame=first_frame;frame<last_frame;++frame) {
            const int64_t at=ceil_div(frame*hz,48000);
            while(next<events.size() && events[next].tick<=at) {
                advance(events[next].tick); write(events[next++]);
            }
            advance(at);
            for (auto d : dut) d->audio_tick=1;
            cycle();
            for (auto d : dut) d->audio_tick=0;
            bool received[2]={bool(dut[0]->audio_valid),bool(dut[1]->audio_valid)};
            for(int wait=0;!received[0]||!received[1];++wait) {
                require(wait<128,"audio sample deadline"); cycle();
                for(int s=0;s<2;++s) received[s]=received[s]||dut[s]->audio_valid;
            }
            for(int s=0;s<2;++s) {
                (void)pitch[s].sample();
                const int16_t expected=tract[s].sample();
                const int16_t actual=static_cast<int16_t>(dut[s]->audio);
                require(actual==expected,"PCM mismatch frame="+std::to_string(frame)+" socket="+
                        std::to_string(s)+" actual="+std::to_string(actual)+" expected="+std::to_string(expected));
                ++samples;
                if(frame>=first_output) {
                    const uint16_t value=static_cast<uint16_t>(actual);
                    pcm.put(static_cast<char>(value&255)); pcm.put(static_cast<char>(value>>8));
                }
            }
            check_fault();
            if(frame>=first_output) ++frames;
        }
        while(next<events.size()) { advance(events[next].tick); write(events[next++]); }
        advance(end);
        pcm.close(); require(bool(pcm),"PCM output failed");
        const int timed_checks=timed_contracts(tables);
        std::cout<<"{\"frames\":"<<frames<<",\"sample_checks\":"<<samples
                 <<",\"xck_ticks_per_socket\":"<<ticks<<",\"fabric_cycles_simulated\":"<<cycles
                 <<",\"max_tick_latency\":"<<max_latency<<",\"writes\":"<<writes
                 <<",\"timed_sample_checks\":"<<timed_checks<<"}\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
