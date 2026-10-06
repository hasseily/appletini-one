// All duration/rate/function combinations and live reload ordering.
#include "Vssi263_response_timing.h"
#include <iostream>
#include <stdexcept>
#include <string>
static uint64_t checks=0,cycles=0;
static void require(bool ok,const char* message){++checks;if(!ok)throw std::runtime_error(std::string(message)+" cycle="+std::to_string(cycles));}
struct Bench {
    Vssi263_response_timing d;
    void edge(){d.clk=0;d.eval();d.clk=1;d.eval();++cycles;}
    void reset(){d.rstn=0;d.warm_reset=0;d.start=0;d.start_compat=0;d.xck_ce=0;edge();d.rstn=1;edge();}
    void start(int dur,int rate,int function){d.duration_phoneme=dur<<6;d.rate_inflection=rate<<4;d.current_function=function;d.start=1;edge();d.start=0;}
};
int main(int argc,char**argv){
 try{
    Verilated::commandArgs(argc,argv);Bench b;auto&d=b.d;
    for(int dur=0;dur<4;++dur)for(int rate=0;rate<16;++rate)for(int mode=0;mode<4;++mode){
        b.reset();b.start(dur,rate,mode);d.xck_ce=1;
        const int frame=(16-rate)*8192, phone=frame*(4-dur);
        for(int n=1;n<=2*phone+2;++n){
            b.edge();
            require(bool(d.phoneme_done)==(n%phone==0),"D/R full phoneme timing");
            const int period=mode==1?frame:phone;
            require(bool(d.response_done)==(mode!=0&&n>1&&(n-1)%period==0),"mode-dependent repeated response timing");
        }
    }
    // Change RATE midway through each of the first sixteen slots. The old
    // slot finishes intact; every later slot uses the new live rate.
    for(int old_rate=0;old_rate<16;++old_rate)for(int new_rate=0;new_rate<16;++new_rate){
        b.reset();b.start(3,old_rate,1);d.xck_ce=1;
        const int first=(16-old_rate)*512, next=(16-new_rate)*512;
        const int full=first+15*next;
        for(int n=1;n<=full+2;++n){
            if(n==first/2)d.rate_inflection=new_rate<<4;
            b.edge();require(bool(d.response_done)==(n==full+1),"live RATE next-slot reload");
        }
    }
    // Warm reset cancels a response already staged in the one-clock pipeline.
    b.reset();b.start(3,15,3);d.xck_ce=1;
    for(int n=1;n<=8192;++n)b.edge();
    require(d.phoneme_done&&!d.response_done,"pre-reset staged response");
    d.warm_reset=1;b.edge();require(!d.response_done&&!d.phoneme_done,"warm reset cancels response");
    d.warm_reset=0;for(int n=0;n<16384;++n){b.edge();require(!d.response_done&&!d.phoneme_done,"reset timer stays inactive");}
    std::cout<<"{\"timing_checks\":"<<checks<<",\"timing_cycles\":"<<cycles<<"}\n";
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
