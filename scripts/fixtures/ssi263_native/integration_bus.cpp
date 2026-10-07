// Actual bus writes, native PCM parity, AP/P reset and interrupt contracts.
#include "Vintegration_top.h"
#include "baseline.h"
#include "native_control.h"
#include "native_source.h"
#include "prototype_tract.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
using namespace ssi_host;
static uint64_t checks=0, clocks=0, pcm_checks=0, valid_samples=0;
static int max_tick_latency=0,max_sample_latency=0;
static void require(bool ok,const std::string& why) {
    ++checks; if(!ok) throw std::runtime_error(why+" clock="+std::to_string(clocks));
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

struct Bench {
    Vintegration_top d;
    int16_t last=0;
    bool sample_clock=false;
    void cycle(int count=1) {
        while(count--) {
            if(sample_clock) d.audio_tick=(clocks%2771==17);
            d.clk=0;d.eval();d.clk=1;d.eval();++clocks;
            if(d.rstn && d.card_enabled) {
                require(!d.fault,"native deadline/arithmetic fault");
                require(d.audio_valid || int16_t(d.audio)==last,"audio changed without valid");
            }
            if(d.audio_valid) ++valid_samples;
            last=int16_t(d.audio);
        }
    }
    void reset() {
        sample_clock=false;d.rstn=0;d.card_enabled=1;d.apple_res=1;d.card_mode=5;
        d.via_pcr=0;d.write_strobe=0;d.compat_write=0;d.audio_tick=0;d.xck_ce=0;
        cycle(3);d.rstn=1;cycle(3);
        require(d.registers==0x00800000c0ULL,"cold programming latches");
        require(d.native_registers==d.registers,"cold FF native override");
        require(!d.d7&&!d.irq&&!d.ints,"cold interrupt reset");
    }
    void write(int reg,int data) {
        d.write_strobe=1;d.write_reg=reg;d.write_data=data;cycle();
        d.write_strobe=0;cycle(2);
    }
    void raw(int count=1) {
        while(count--) {d.xck_ce=1;cycle();d.xck_ce=0;cycle(64);}
    }
    void effective(int count) {raw(count*2);}
    void setup(int dur=0xf1,int rate=0xf0,int ctl=0x0f) {
        write(0,dur);write(1,0x58);write(2,rate);write(4,231);write(3,ctl);
        require(d.native_registers==d.registers,"accepted SSI register stream");
    }
    void request_sample() {
        d.audio_tick=1;cycle();d.audio_tick=0;
        for(int i=0; !d.audio_valid && i<132; ++i) cycle();
        require(d.audio_valid,"sample request completion");cycle();
    }
    // Stop at the raw response event, before the delayed bus-visible pulse.
    void to_response() {
        for(int n=0;n<20000;++n) {
            d.xck_ce=1;cycle();d.xck_ce=0;
            if(d.response_raw) return;
            cycle(64);
        }
        require(false,"response timeout");
    }
    void drain() {d.xck_ce=0;d.write_strobe=0;d.audio_tick=0;cycle(132);}
};

static void bus_contracts() {
    Bench b; auto& d=b.d; b.reset();
    // Frame mode requests after 4096 effective ticks, not D=1's 3x duration.
    b.setup(0x71);b.effective(4095);require(!d.d7,"early frame D7");
    b.effective(1);require(d.d7&&d.irq&&d.mode==1&&d.duration_phase==5,"frame response timing");
    for(int alias=4;alias<8;++alias) {
        b.write(alias,alias*29);require(d.d7&&d.irq,"FF must not acknowledge");
        require((d.native_registers>>32)==uint64_t(alias*29),"native FF aliases");
    }
    b.write(1,0x66);require(!d.d7&&!d.irq,"INF ACK");
    b.effective(4096);require(d.d7&&d.irq,"repeating frame request");
    d.card_mode=0;b.cycle();require(d.ifr_set==2&&!d.irq,"pending mode switch to CA1");
    b.cycle();require(!d.ifr_set,"CA1 is pulse");
    d.card_mode=5;b.cycle();require(d.irq,"pending mode switch to direct IRQ");
    b.write(3,0x80);b.write(0,0x31);b.write(3,0x0f);
    require(d.mode==1&&!d.ints,"DR00 retains response mode but disables external IRQ");
    b.effective(4096);require(d.d7&&!d.irq,"DR00 D7 still records response");
    b.reset();b.setup(0xf1);b.effective(4095);require(!d.d7,"early phoneme D7");
    b.effective(1);require(d.d7&&d.irq,"mode3 phoneme response");
    b.write(2,0xf0);require(!d.d7&&!d.irq,"RATE ACK without restart");
    b.effective(4096);require(d.d7&&d.irq,"repeating phoneme response");
    // RATE applies to the next reload only, including a coincident accepted write.
    b.reset();b.setup(0x71);b.effective(255);require(d.response_left==0,"last response subtick");
    b.raw();require(d.div2,"align effective edge");
    d.write_strobe=1;d.write_reg=2;d.write_data=0x50;d.xck_ce=1;b.cycle();
    require(d.response_left==2815&&d.response_phase==1,"same-edge live RATE reload");
    b.drain();
    // Host ACKs and power-down must beat the exact response-to-wrapper edge.
    for(int reg : {0,1,2,3}) {
        b.reset();b.setup();b.to_response();b.cycle();require(d.done,"response staging");
        d.write_strobe=1;d.write_reg=reg;d.write_data=reg==3?0x80:reg==0?0xf2:0xf0;
        b.cycle();require(!d.d7&&!d.irq,"ACK/CTL-high coincidence priority");
        b.drain();require(!d.d7&&!d.irq,"stale response crossed new start");
    }
    // A real nonzero sample must clear at the next sample boundary during AP reset.
    b.reset();b.setup();bool sounded=false;
    for(int n=0;n<5000;++n) {b.effective(1);if(n%16==0){b.request_sample();sounded |= int16_t(d.audio)!=0;}}
    require(sounded,"native SSI produces nonzero output");
    // Find a nonzero sample immediately before reset, rather than relying on history.
    for(int n=0;int16_t(d.audio)==0 && n<512;++n){b.effective(1);b.request_sample();}
    require(int16_t(d.audio)!=0,"reset starts with held nonzero sample");
#if TEST_CHIP_TYPE == 2
    // A one-clock AP reset overlaps an in-flight sample. At the reset edge
    // the native engine publishes a pre-reset nonzero value, while the bus
    // wrapper must publish immediate valid zero and discard that stale value
    // on release. This needs both current and queued reset qualification.
    d.audio_tick=1;d.write_strobe=1;d.write_reg=2;d.write_data=0xf1;b.cycle();
    d.apple_res=0;d.write_data=0x12;b.cycle();
    require(d.audio_valid&&d.audio==0,"immediate AP valid silence at input stage");
    require(d.native_valid&&int16_t(d.native_audio)!=0,"short-reset test hits stale native output");
    d.apple_res=1;d.audio_tick=0;d.write_strobe=0;b.cycle();
    require(((d.native_registers>>16)&255)==0xf1&&d.native_registers==d.registers,
            "accepted write before AP reset survives; reset-edge write is ignored");
    require(!d.audio_valid&&d.audio==0,"queued AP reset blocks stale sample on release");
    for(int n=0;n<8;++n){b.cycle();require(!d.audio_valid&&d.audio==0,"no delayed stale sample");}
    b.request_sample();require(d.audio==0,"short AP reset remains silent");
    // Resume the retained voice for the longer reset retention checks below.
    b.write(3,0x0f);b.effective(5000);b.request_sample();
    for(int n=0;int16_t(d.audio)==0&&n<512;++n){b.effective(1);b.request_sample();}
    require(int16_t(d.audio)!=0,"voice resumes after short AP reset");
#endif
    const uint64_t saved=d.registers;const int pitch=d.pitch_active;
    d.apple_res=0;b.cycle(4);
#if TEST_CHIP_TYPE == 2
    require((d.registers&0xff00ffffffULL)==(saved&0xff00ffffffULL),"AP retained programming latches");
    require(((d.registers>>24)&255)==0x80&&!d.d7&&!d.irq&&!d.ints,"AP powers down and releases requests");
    require(d.native_registers==d.registers,"AP native retained register image");
    require(d.pitch_active==pitch,"AP retained active pitch");
    for(int n=0;n<32;++n){b.request_sample();require(d.audio==0,"AP sample-held silence");b.raw(2);}
    d.write_strobe=1;d.write_reg=4;d.write_data=0x55;b.cycle();d.write_strobe=0;
    require(d.native_registers==d.registers && (d.registers>>32)==(saved>>32),"AP reset ignores writes");
    d.apple_res=1;b.cycle();b.write(3,0x0f);
    require(d.native_registers==d.registers,"AP resume retained configuration");
    b.effective(4096);require(d.d7&&d.irq,"AP resumed duration request");
#else
    require(d.registers==saved&&d.native_registers==saved,"P reset retains running voice");
    d.apple_res=1;b.cycle();b.write(0,0x71);require(d.mode==3,"P live DUR does not latch mode");
    d.apple_res=0;b.cycle(2);require(d.mode==1&&d.pitch_mode==1&&d.ints,"P reset relatches both timing and pitch mode");
    d.apple_res=1;b.cycle();
#endif
    // Card disable is a cold reset and ignores bus traffic.
    d.card_enabled=0;d.write_strobe=1;d.write_reg=2;d.write_data=0xff;b.cycle(3);
    require(d.registers==0x00800000c0ULL&&d.native_registers==d.registers,"card-disable cold reset");
    require(!d.d7&&!d.irq&&!d.audio_valid&&d.audio==0,"card-disable outputs");
    d.write_strobe=0;d.card_enabled=1;b.cycle(2);
    // Optional Votrax address decoding translates to SSI, including a CB1 ACK.
    d.compat_data=0;d.compat_write=1;b.cycle();d.compat_write=0;
    require(d.ifr_clr==16,"compatibility CB1 ACK");b.cycle(3);
    require((d.native_registers&255)==0xc2 && ((d.native_registers>>24)&255)==0x0f,
            "Votrax address uses translated native SSI phone/control");
    require(d.native_registers==d.registers,"compatibility native register image");
    b.write(2,0xf0);d.via_pcr=0xb0;bool cb1=false;
    for(int n=0;n<18000&&!cb1;++n) {
        d.xck_ce=1;b.cycle();d.xck_ce=0;
        for(int k=0;k<64;++k){b.cycle();cb1|=bool(d.ifr_set&16);}
    }
    require(cb1,"translated SSI compatibility completion");
}

static void pcm_parity(const Tables& tables) {
    Bench b;b.reset();auto& d=b.d;
    Baseline pitch(tables);NativeControl control(tables);NativeSource source;PrototypeTract tract(1);
    control.write(4,0); // Explicit production cold FF latch override.
    const int regs[]={0,1,2,4,3}, values[]={0xf1,0x58,0xf0,231,0x0f};
    for(int k=0;k<5;++k){b.write(regs[k],values[k]);pitch.write(regs[k],values[k]);control.write(regs[k],values[k]);}
    bool div2=false,pending=false;int16_t expected=0;int nonzero=0;
    uint64_t previous_registers=d.registers;
    int last_effective=-1,last_sample=-1;
    for(int n=0;n<130*16384+200;++n) {
        const bool raw=n<130*16384&&n%65==0, effective=raw&&div2;
        if(raw)div2=!div2;
        const bool sample=n<130*16384&&(n%2771==37||n==65);
        d.write_strobe=0;d.xck_ce=raw;d.audio_tick=sample;
        if(n==65 || (n>=200&&n<208) || (n>=130*8192 && n<130*16384 && n%1723==0)) {
            const int seq=n/1723,reg=n==65?2:n<208?4+n%4:seq%8;
            const int value=n==65?0xf1:n<208?231+n%4:reg>=4?(seq%2?231:255):reg==3?(seq%23==0?0x80:seq%17==0?0:0x0f):seq*37&255;
            d.write_strobe=1;d.write_reg=reg;d.write_data=value;
            pitch.write(reg,value);control.write(reg,value);
        }
        if(effective) {
            last_effective=n;
            control.set_ampct_zero(source.ampct_zero());control.advance_xck(1);
            const int r=pitch.register_value(2),live=((r&8)<<8)|(pitch.register_value(1)<<3)|(r&7);
            source.tick(control,pitch.current_function()==3?pitch.active_inflection():live);
            const auto& o=source.output();
            tract.process({o.phase,o.phase_edge,{o.f1,o.f2,o.f2q,o.f3,o.f4,o.filter_amp,o.voice_amp,o.fric_amp},
                o.voice_target_q16,o.fric_drive_q16,o.fric1,o.fric2,o.output_open});
        }
        if(sample){last_sample=n;require(!pending,"overlapping sample request");expected=tract.sample();pending=true;(void)pitch.sample();}
        b.cycle();
        if(d.tick_done){max_tick_latency=std::max(max_tick_latency,n-last_effective);require(n-last_effective<130,"staged tick meets physical deadline");}
        if(d.audio_valid){max_sample_latency=std::max(max_sample_latency,n-last_sample);require(pending,"unsolicited valid sample");require(int16_t(d.audio)==expected,"wrapper PCM differs from frozen host");pending=false;++pcm_checks;nonzero+=int16_t(d.audio)!=0;}
        require(previous_registers==d.native_registers,"one-clock staged SSI register image");
        previous_registers=d.registers;
    }
    require(!pending&&nonzero>10,"PCM checks cover sound and complete requests");
}
int main(int argc,char**argv) {
    try {Verilated::commandArgs(argc,argv);require(argc==2,"integration TABLES");
        bus_contracts();pcm_parity(read_tables(argv[1]));
        std::cout<<"{\"chip_type\":"<<TEST_CHIP_TYPE<<",\"checks\":"<<checks<<",\"fabric_cycles\":"<<clocks
            <<",\"pcm_checks\":"<<pcm_checks<<",\"valid_samples\":"<<valid_samples
            <<",\"max_tick_latency_from_bus\":"<<max_tick_latency<<",\"max_sample_latency_from_bus\":"<<max_sample_latency<<"}\n";
    }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
