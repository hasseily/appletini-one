/* Hardware-only load client. All files belong to the stress test volumes. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

volatile uint8_t stress_command,stress_phase,stress_target,stress_stage;
volatile uint16_t stress_round,stress_sp,stress_d2,stress_mismatch;
volatile uint16_t stress_frames,stress_video_errors;
volatile uint8_t stress_errno,stress_oserror,stress_got,stress_expected;
uint16_t stress_tags[2][64];
static uint8_t data[512],video_tag;
static uint16_t at,block;
static int fd;
static const char *const paths[]={"/RSAHOST/SCRATCH","/RSADISK/SCRATCH"};

static uint8_t pattern(uint16_t index)
{
    return (uint8_t)((index*17U)^(block*13U)^(stress_round*29U)^
                    (stress_round>>8)^(stress_target*0x69U)^0x53U);
}
static uint8_t check(void)
{
    for(at=0;at<512;++at)data[at]=pattern(at);
    stress_stage=1;fd=open(paths[stress_target],O_RDWR);if(fd<0)return 0;
    stress_stage=2;if(lseek(fd,(long)block*512L,SEEK_SET)!=(long)block*512L)goto failed;
    stress_stage=3;if(write(fd,data,512)!=512)goto failed;
    stress_stage=4;if(close(fd))return 0;
    stress_stage=5;fd=open(paths[stress_target],O_RDONLY);if(fd<0)return 0;
    stress_stage=6;if(lseek(fd,(long)block*512L,SEEK_SET)!=(long)block*512L)goto failed;
    memset(data,0xA5,sizeof(data));
    stress_stage=7;if(read(fd,data,512)!=512)goto failed;
    stress_stage=8;if(close(fd))return 0;
    stress_stage=9;
    for(at=0;at<512;++at)if(data[at]!=pattern(at)) {
        stress_mismatch=at;stress_got=data[at];stress_expected=pattern(at);return 0;
    }
    stress_tags[stress_target][block]=stress_round;stress_stage=0;return 1;
failed:
    stress_errno=errno;stress_oserror=_oserror;close(fd);return 0;
}
static void frame(void)
{
    uint8_t row,col,value;
    volatile uint8_t *line;
    video_tag=(uint8_t)++stress_frames;
    for(row=0;row<24;++row) {
        line=(volatile uint8_t *)(0x400U+(row&7U)*128U+(row>>3)*40U);
        for(col=0;col<40;++col) {
            value=(uint8_t)(((col+row+video_tag)&15U)|(((col^row^video_tag)&15U)<<4));
            line[col]=value;
        }
        for(col=0;col<40;++col) {
            value=(uint8_t)(((col+row+video_tag)&15U)|(((col^row^video_tag)&15U)<<4));
            if(line[col]!=value)++stress_video_errors;
        }
    }
}
int main(void)
{
    uint8_t vblank=0;
    memset(stress_tags,0xFF,sizeof(stress_tags));
    *(volatile uint8_t *)0xC00C=0;*(volatile uint8_t *)0xC00E=0;
    (void)*(volatile uint8_t *)0xC054;(void)*(volatile uint8_t *)0xC056;
    (void)*(volatile uint8_t *)0xC052;(void)*(volatile uint8_t *)0xC050;
    for(;;) {
        if(stress_command==1) {stress_command=0;stress_phase=1;}
        if(stress_command==2) {stress_command=0;stress_phase=0;}
        if(stress_command==3) {stress_command=0;stress_phase=2;}
        if(stress_phase>=2)continue;
        if(((*(volatile uint8_t *)0xC019)&128U)!=vblank) {
            vblank^=128U;if(vblank)frame();
        }
        if(stress_phase!=1)continue;
        block=(stress_round*13U)&63U;
        for(stress_target=0;stress_target<2;++stress_target) {
            if(!check()) {
                if(!stress_errno){stress_errno=errno;stress_oserror=_oserror;}
                stress_phase=3;break;
            }
            if(stress_target)++stress_d2;else ++stress_sp;
            frame();
        }
        if(stress_phase==1)++stress_round;
    }
}
