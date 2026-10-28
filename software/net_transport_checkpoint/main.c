/* Standalone Apple diagnostic. No network or JSON client is enabled here. */
#include <conio.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

unsigned char nt_selector, nt_mode, nt_rc, nt_poisoned;
unsigned char nt_reset_partial, nt_reset_stage;
unsigned char nt_timer_overflow;
unsigned int nt_offset, nt_capacity, nt_length, nt_call_ticks, nt_irq_ticks;
/* The linker reserves this range above the program, heap and stack. */
#pragma bss-name(push, "NETBUF")
unsigned char nt_buffer[514];
#pragma bss-name(pop)
void nt_call(void);
void nt_lock(void);
void nt_unlock(void);
unsigned char nt_check_data(void);

typedef struct {
    uint16_t count, call_min, call_max, irq_min, irq_max;
    uint32_t call_total, irq_total;
} sample_t;
typedef char sample_size_must_be_18[(sizeof(sample_t)==18) ? 1 : -1];
static sample_t samples[504];
static unsigned char disk_buffer[512];
static unsigned char generation, saved_acr, saved_ier;
static unsigned char speed_label, fail, choice;
static unsigned int iteration, size, offset, count, target_reads;
static unsigned char result_header[32];
#define VIA(a) (*(volatile unsigned char *)(a))

static unsigned int u16(const unsigned char *p)
{ return p[0] | ((unsigned int)p[1] << 8); }
static void put16(unsigned char *p,unsigned int value)
{ p[0]=(unsigned char)value;p[1]=(unsigned char)(value>>8); }

static unsigned char save_result(void)
{
    const char *path;
    int fd;
    unsigned int at,n;
    path=choice=='S' ? "/NETSP/RS1.BIN" :
         nt_mode ? (speed_label=='1' ? "/NETSP/L1.BIN" : "/NETSP/LT.BIN") :
                   (speed_label=='1' ? "/NETSP/R1.BIN" : "/NETSP/RT.BIN");
    memset(result_header,0,32);memcpy(result_header,"NTM1",4);
    result_header[4]=1;result_header[5]=nt_mode;result_header[6]=speed_label;
    result_header[7]=(fail ? 0:1)|2; /* pass, checked timer wrap */
    put16(result_header+8,iteration);put16(result_header+10,504);
    put16(result_header+12,18);put16(result_header+14,9);put16(result_header+16,512);
    result_header[18]=generation;result_header[24]=1; /* Native VIA ticks. */
    fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,0666);
    if(fd<0) return 0;
    if(write(fd,result_header,32)!=32) { close(fd);return 0; }
    for(at=0;at<sizeof(samples);at+=n) {
        n=sizeof(samples)-at;if(n>512) n=512;
        if(write(fd,(const unsigned char *)samples+at,n)!=n) { close(fd);return 0; }
    }
    return close(fd)==0;
}

static unsigned char call(unsigned char selector, unsigned int at, unsigned int cap)
{
    nt_selector=selector; nt_offset=at; nt_capacity=cap;
    memset(nt_buffer,0xCC,sizeof(nt_buffer));
    VIA(0xC484)=0xFF; VIA(0xC485)=0xFF;
    nt_call();
    if(nt_rc || nt_length>cap || nt_buffer[512]!=0xCC || nt_buffer[513]!=0xCC ||
       !nt_call_ticks || !nt_irq_ticks || nt_timer_overflow)
        return 0;
    return 1;
}

static unsigned char identify(void)
{
    const volatile unsigned char *rom=(const volatile unsigned char *)0xC700;
    uint16_t crc=0xFFFF, at;
    unsigned char bit;
    /* Same read-only interface guard as libtini AMEM. */
    nt_lock();
    for(at=0;at<256;++at) {
        crc^=(uint16_t)rom[at]<<8;
        for(bit=0;bit<8;++bit) crc=crc&0x8000 ? (crc<<1)^0x1021 : crc<<1;
    }
    nt_unlock();
    return crc==0x95D9;
}

static unsigned char descriptor(void)
{
    if(!call(0x82,0,64) || nt_length!=32 || memcmp(nt_buffer,"ATNT",4) ||
        nt_buffer[4]!=0 || nt_buffer[5]!=1 || nt_buffer[6]!=32 ||
        u16(nt_buffer+8)!=0x8000 || u16(nt_buffer+18)!=512) return 0;
    generation=nt_buffer[7];
    return generation!=0;
}

static unsigned char reset_recovery(void)
{
    int fd;
    unsigned char current[32];
    memcpy(current,nt_buffer,32);
    fd=open("/NETSP/RESET.TAG",O_RDONLY);
    if(fd<0) return choice!='P';
    if(read(fd,disk_buffer,32)!=32) { close(fd); return 0; }
    close(fd);
    if(memcmp(disk_buffer,"ATNT",4) || current[7]==disk_buffer[7] ||
       !memcmp(current+24,disk_buffer+24,4) || u16(current+28) || u16(current+30))
        return 0;
    fd=open("/NETSP/RESET.OK",O_WRONLY|O_CREAT|O_TRUNC,0666);
    if(fd<0) return 0;
    if(write(fd,current,32)!=32) { close(fd); return 0; }
    if(close(fd)!=0 || unlink("/NETSP/RESET.TAG")!=0) return 0;
    cprintf("RESET RECOVERY PASS, GEN=%u\r\n",current[7]);
    return 1;
}

static void reset_loop(void)
{
    int fd;
    fd=open("/NETSP/RESET.TAG",O_WRONLY|O_CREAT|O_TRUNC,0666);
    if(fd<0 || write(fd,nt_buffer,32)!=32) { fail=1; if(fd>=0) close(fd); return; }
    close(fd);
    cprintf("WARM CONTROL-RESET AT THE PAUSED READ\r\n");
    cprintf("THEN RUN NETTEST.SYSTEM AND CHOOSE P\r\n");
    nt_reset_partial=1;
    for(;;) if(!call(0x83,0,512)) { fail=1; return; }
}

static unsigned char disk_check(const char *name, unsigned int round)
{
    int fd;
    unsigned int at;
    for(at=0;at<512;++at) disk_buffer[at]=(unsigned char)(at*17U+round);
    fd=open(name,O_RDWR);
    if(fd<0) { cprintf("OPEN %s E=%u OS=%02X\r\n",name,errno,_oserror); return 0; }
    if(write(fd,disk_buffer,512)!=512) {
        cprintf("WRITE %s E=%u OS=%02X\r\n",name,errno,_oserror);close(fd);return 0;
    }
    close(fd);
    fd=open(name,O_RDONLY);
    if(fd<0) { cprintf("REOPEN %s E=%u OS=%02X\r\n",name,errno,_oserror); return 0; }
    if(read(fd,disk_buffer,512)!=512) {
        cprintf("READ %s E=%u OS=%02X\r\n",name,errno,_oserror);close(fd);return 0;
    }
    close(fd);
    for(at=0;at<512;++at)
        if(disk_buffer[at]!=(unsigned char)(at*17U+round)) {
            cprintf("VERIFY %s AT %u GOT=%02X EXP=%02X\r\n",name,at,disk_buffer[at],
                    (unsigned char)(at*17U+round));return 0;
        }
    return 1;
}

static void add_sample(void)
{
    sample_t *s=&samples[size-9];
    if(!s->count || nt_call_ticks<s->call_min) s->call_min=nt_call_ticks;
    if(!s->count || nt_irq_ticks<s->irq_min) s->irq_min=nt_irq_ticks;
    if(nt_call_ticks>s->call_max) s->call_max=nt_call_ticks;
    if(nt_irq_ticks>s->irq_max) s->irq_max=nt_irq_ticks;
    s->call_total+=nt_call_ticks; s->irq_total+=nt_irq_ticks; ++s->count;
}

int main(void)
{
    clrscr();
    cprintf("SMARTPORT NET TRANSPORT CHECKPOINT\r\n");
    cprintf("R: SLOT ROM   L: LIBTINI-STYLE FIFO\r\n");
    cprintf("X: RESET LOOP P: RECOVERY PROBE\r\n");
    cprintf("S: ROM TIMING SWEEP\r\n");
    choice=cgetc(); if(choice>='a' && choice<='z') choice-=32;
    nt_mode=choice!='R' && choice!='S';
    target_reads=choice=='S' ? 1008 : 10000;
    cprintf("SPEED LABEL: 1=1 MHZ, T=TURBO\r\n"); speed_label=cgetc();
    if(speed_label=='t') speed_label='T';
    /* This diagnostic owns VIA1 T1; no audio plays during these tests. */
    saved_acr=VIA(0xC48B); saved_ier=VIA(0xC48E);
    VIA(0xC48E)=0x40; VIA(0xC48B)=saved_acr|0x40;
    VIA(0xC484)=0xFF; VIA(0xC485)=0xFF;
    if(!identify() || !descriptor()) {
        cprintf("SERVICE/ROM NOT IDENTIFIED RC=%02X\r\n",nt_rc); fail=1;
        goto finish;
    }
    if(!reset_recovery()) { cprintf("RESET RECOVERY FAILED\r\n"); fail=1; goto finish; }
    if(choice=='P') goto finish;
    if(choice=='X') { reset_loop(); goto finish; }
    cprintf("GEN=%u; %u READS; 9..512 BYTES\r\n",generation,target_reads);
    cprintf("NETSP AND NETD2 SCRATCH FILES REQUIRED\r\n");
    if(!disk_check("/NETSP/SCRATCH",0) || !disk_check("/NETD2/SCRATCH",0)) {
        cprintf("SCRATCH DISK CHECK FAILED\r\n"); fail=1; goto finish;
    }
    for(iteration=0;iteration<target_reads;++iteration) {
        /* Each adjacent pair repeats the same offset and reply size. */
        size=(iteration/2)%504+9;
        offset=((unsigned long)(iteration/2)*73UL)&0x7FFF;
        if((iteration/2)%257==0) offset=32768U;
        if((iteration/2)%263==0) offset=65535U;
        if(!call(0x83,offset,size)) { fail=1; break; }
        count=offset>=32768U ? 0 : size-8;
        if(offset<32768U && count>32768U-offset) count=32768U-offset;
        if(nt_length!=count+8 || nt_buffer[0]!=generation ||
           nt_buffer[1]!=(offset>=32768U || (unsigned long)offset+count>=32768UL ? 2:1) ||
           u16(nt_buffer+2)!=offset || u16(nt_buffer+4)!=0 || u16(nt_buffer+6)!=count) {
            fail=1; break;
        }
        if(!nt_check_data()) fail=1;
        if(fail) break;
        add_sample();
        if(iteration%128==127) {
            if(!disk_check("/NETSP/SCRATCH",iteration) ||
               !disk_check("/NETD2/SCRATCH",iteration)) { fail=1; break; }
        }
        if(iteration%500==499) cprintf("%u READS OK\r\n",iteration+1);
    }
    if(!save_result()) fail=1;
finish:
    VIA(0xC48B)=saved_acr;
    if(saved_ier&0x40) VIA(0xC48E)=0xC0;
    cprintf("%s AT READ %u RC=%02X\r\n",fail ? "FAIL" : "PASS",iteration,nt_rc);
    cprintf("PRESS A KEY TO RETURN\r\n"); cgetc();
    return fail;
}
