/* Read-only stock SmartPort benchmark. Owns VIA1 T1 while audio is idle. */
#include <conio.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

unsigned int sc_dest, sc_ticks;
unsigned char sc_raw, sc_rc, sc_overflow;
void sc_call(void);
#define VIA(a) (*(volatile unsigned char *)(a))
typedef struct {
    uint16_t dest;
    uint8_t raw, count;
    uint16_t min, max;
    uint32_t sum;
} sample_t;
typedef char sample_size[(sizeof(sample_t)==12) ? 1 : -1];
static sample_t samples[4];
static unsigned char header[16], saved_acr, saved_ier, choice, fail;
static unsigned int row, iteration, at;

static void run(void)
{
    int fd;
    unsigned char *buffer;
    memset(samples,0,sizeof(samples)); fail=0;
    saved_acr=VIA(0xC48B); saved_ier=VIA(0xC48E);
    VIA(0xC48E)=0x40; VIA(0xC48B)=saved_acr|0x40;
    for(row=0;row<4 && !fail;++row) {
        sc_raw=row&1; sc_dest=row<2 ? 0x4751 : 0x9000;
        samples[row].dest=sc_dest; samples[row].raw=sc_raw;
        samples[row].min=65535U;
        buffer=(unsigned char *)sc_dest;
        for(iteration=0;iteration<32;++iteration) {
            memset(buffer,0xCC,514);
            VIA(0xC484)=255; VIA(0xC485)=255;
            sc_call();
            if(sc_rc || sc_overflow || !sc_ticks) { fail=1; break; }
            for(at=0;at<512;++at) if(buffer[at]!=0xA5) { fail=2; break; }
            if(buffer[512]!=0xCC || buffer[513]!=0xCC) fail=3;
            if(fail) break;
            ++samples[row].count;
            if(sc_ticks<samples[row].min) samples[row].min=sc_ticks;
            if(sc_ticks>samples[row].max) samples[row].max=sc_ticks;
            samples[row].sum+=sc_ticks;
        }
        cprintf("%c %s $%04X %u..%u N=%u\r\n",choice,
                sc_raw ? "RAM" : "ROM",sc_dest,samples[row].min,
                samples[row].max,samples[row].count);
    }
    VIA(0xC48B)=saved_acr; VIA(0xC48E)=saved_ier|0x80;
    memset(header,0,sizeof(header)); memcpy(header,"SCB1",4);
    header[4]=choice; header[5]=fail; header[6]=sc_rc; header[7]=sc_overflow;
    fd=open(choice=='1' ? "/SPCOPY/NATIVE.BIN" : "/SPCOPY/TURBO.BIN",
            O_WRONLY|O_CREAT|O_TRUNC,0666);
    if(fd<0) { cprintf("SAVE OPEN FAILED\r\n"); return; }
    if(write(fd,header,sizeof(header))!=sizeof(header) ||
       write(fd,samples,sizeof(samples))!=sizeof(samples) || close(fd)!=0)
        cprintf("SAVE FAILED\r\n");
    else cprintf("SAVED %s RC=%02X WRAP=%u\r\n",fail ? "FAIL" : "PASS",sc_rc,sc_overflow);
}

int main(void)
{
    clrscr(); cprintf("STOCK SMARTPORT COPY TIMING\r\n");
    cprintf("512 BYTES; BLOCK 1599; 32 PER CASE\r\n");
    for(;;) {
        cprintf("READY: 1=NATIVE T=TURBO\r\n");
        choice=cgetc(); if(choice=='t') choice='T';
        if(choice=='1' || choice=='T') run();
    }
    return 0;
}
