#include "Vmixer_tb.h"
#include <verilated.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
int signed18(uint32_t n) { return (n&0x20000)?int(n)-262144:int(n); }
int scale(int sample, int gain) {
    const int64_t product=int64_t(sample)*gain;
    const int result=int((std::abs(product)+8192)/16384);
    return product<0 ? -result : result;
}
int coefficient(int db) { return int(std::lround(16384*std::pow(10.0,std::clamp(db,-5,5)/20.0))); }
int pan_coefficient(int gain, int value, bool right) {
    const int left_gains[]={16,16,16,16,16,16,16,16,16,14,11,9,7,5,2,0};
    const int right_gains[]={0,2,4,6,8,10,12,14,16,16,16,16,16,16,16,16};
    const int weight=(right?right_gains:left_gains)[value];
    int result=0;
    // Each binary-weight term rounds separately, matching the existing AY
    // pan law. The independent dB law above computes coefficients from math.
    for (int bit=0;bit<5;++bit) if (weight&(1<<bit)) result+=gain>>(4-bit);
    return result;
}
class Test {
public:
    Vmixer_tb d;
    uint64_t checks{}, cycles{}, publications{};
    std::array<int,4> gains{20626,0,0,20626};
    int held0{}, held1{}, ay_l{}, ay_r{};
    void require(bool condition, const std::string& message) {
        ++checks;
        if (!condition) throw std::runtime_error("cycle "+std::to_string(cycles)+": "+message);
    }
    void cycle() {
        d.clk=0; d.eval(); d.clk=1; d.eval(); ++cycles;
        if (d.mix_valid) ++publications;
    }
    void idle(int count=40) {
        d.audio_tick=0; d.sample0_valid=d.sample1_valid=0;
        for (int k=0;k<count;++k) cycle();
    }
    void reset() {
        d.rstn=0; d.audio_tick=0; d.sample0_valid=d.sample1_valid=0;
        d.volume_db=2; d.pan=0xf0; d.ay_l=d.ay_r=0; cycle();
        d.rstn=1; idle();
        gains={20626,0,0,20626}; held0=held1=ay_l=ay_r=0;
        require(d.speech_l==0 && d.speech_r==0 && !d.mix_valid,"reset output");
    }
    void compare() {
        const int l=scale(held0,gains[0])+scale(held1,gains[2]);
        const int r=scale(held0,gains[1])+scale(held1,gains[3]);
        require(signed18(d.speech_l)==l,"left speech expected "+std::to_string(l)+" got "+std::to_string(signed18(d.speech_l)));
        require(signed18(d.speech_r)==r,"right speech expected "+std::to_string(r)+" got "+std::to_string(signed18(d.speech_r)));
        require(int16_t(d.mixed_l)==std::clamp(ay_l*16+l,-32768,32767),"left combined clamp");
        require(int16_t(d.mixed_r)==std::clamp(ay_r*16+r,-32768,32767),"right combined clamp");
    }
    void samples(int a, int b, bool valid0=true, bool valid1=true) {
        d.sample0=uint16_t(a); d.sample1=uint16_t(b);
        d.sample0_valid=valid0; d.sample1_valid=valid1;
        if (valid0) held0=a;
        if (valid1) held1=b;
        cycle(); idle(); compare();
    }
    void ay(int l, int r) { ay_l=l; ay_r=r; d.ay_l=l; d.ay_r=r; d.eval(); compare(); }
    void controls(int db, int pan, int ticks=512) {
        d.volume_db=uint8_t(db)&31; d.pan=pan;
        idle(4); // Control lookup/pan pipeline only; no gain ramp yet.
        const int g=coefficient(db);
        const std::array<int,4> target={pan_coefficient(g,pan&15,false),pan_coefficient(g,pan&15,true),
            pan_coefficient(g,pan>>4,false),pan_coefficient(g,pan>>4,true)};
        for (int k=0;k<ticks;++k) {
            for (int lane=0;lane<4;++lane) gains[lane]+=std::clamp(target[lane]-gains[lane],-64,64);
            d.audio_tick=1; cycle();
            for (int lane=0;lane<4;++lane) require(int(d.gains[lane])==gains[lane],"coefficient ramp");
            idle(); compare();
        }
        if (ticks>=512) require(gains==target,"gain did not settle");
    }
};
}

int main(int argc,char** argv) {
    Verilated::commandArgs(argc,argv);
    try {
        Test t; t.reset();
        t.samples(10000,-20000);
        // All eleven levels, signed endpoints and malformed 5-bit controls.
        for (int db=-5;db<=5;++db) {
            t.controls(db,0xf0);
            for (int sample : {-32768,-16385,-1,0,1,16385,32767}) t.samples(sample,-sample>32767?32767:-sample);
        }
        t.controls(-16,0xf0); t.controls(15,0xf0);
        // Independent pan endpoints/centres and each intermediate position.
        t.samples(10000,-7000);
        for (int p=0;p<16;++p) t.controls(0,(15-p)*16+p);
        t.controls(0,0x0f); t.compare(); // Swap sockets.
        t.require(signed18(t.d.speech_l)==-7000 && signed18(t.d.speech_r)==10000,"swapped endpoints");
        t.controls(5,0x88); t.samples(32767,32767); t.ay(3060,3060);
        t.require(signed18(t.d.speech_l)>65535 && int16_t(t.d.mixed_l)==32767,"positive wide sum must clip only at output");
        t.samples(-32768,-32768); t.ay(0,0);
        t.require(int16_t(t.d.mixed_l)==-32768,"negative clipping");
        t.controls(0,0xf0); t.samples(0,0); t.ay(3060,2500);
        t.require(int16_t(t.d.mixed_l)==32767 && int16_t(t.d.mixed_r)==32767,"AY-only high level must retain positive saturation");
        t.ay(1024,2047);
        t.require(int16_t(t.d.mixed_l)==16384 && int16_t(t.d.mixed_r)==32752,"AY-only normal level must retain x16 scaling");
        t.samples(-20000,-20000); t.ay(2500,2500);
        t.require(int16_t(t.d.mixed_l)==20000,"AY must not clip before negative SSI cancellation");
        // Data changes without valid must not change held samples.
        t.samples(12345,22222,false,false);
        t.samples(12345,22222,true,false);
        t.samples(31000,-12345,false,true);
        // Gain remains steady for arbitrarily many fabric clocks without an
        // audio tick; moving/reversing controls cannot make a fabric-rate ramp.
        const auto before=t.gains;
        t.d.volume_db=5; t.d.pan=0x0f; t.idle(500);
        for (int lane=0;lane<4;++lane) t.require(int(t.d.gains[lane])==before[lane],"gain moved without audio tick");
        t.controls(5,0x0f,73); t.controls(-5,0xf0,37); t.controls(0,0xf0);
        // Every arrival position within a mixer job, including publish and
        // idle snapshot. The most recent valid sample must never disappear.
        for (int offset=0;offset<38;++offset) {
            t.d.sample0=uint16_t(1000+offset); t.d.sample0_valid=1; t.d.sample1_valid=0;
            t.held0=1000+offset; t.cycle(); t.d.sample0_valid=0;
            t.idle(offset);
            t.d.sample1=uint16_t(-3000-offset); t.d.sample1_valid=1; t.held1=-3000-offset;
            t.cycle(); t.idle(60); t.compare();
        }
        // A full 48 kHz interval easily covers variable native-engine latency.
        for (int frame=0;frame<200;++frame) {
            t.d.audio_tick=1; t.cycle(); t.d.audio_tick=0;
            t.idle(frame%122);
            t.samples(frame*71-6000,5000-frame*37,true,false);
            t.idle((frame*7)%122);
            t.samples(9999,5000-frame*37,false,true);
            t.idle(2300); t.compare();
        }
        t.reset();
        std::cout<<"SSI263 MIXER PASS checks="<<t.checks<<" cycles="<<t.cycles<<" publications="<<t.publications<<"\n";
        return 0;
    } catch (const std::exception& e) { std::cerr<<e.what()<<"\n"; return 1; }
}
