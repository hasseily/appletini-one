/* Exercises the real hardware backend with deterministic fake MMIO/DMA.
 * This verifies address and ownership contracts, not FPGA timing. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define COMMON_H
static uint32_t mock_read(uint32_t address);
static void mock_write(uint32_t address, uint32_t value);
#define REG_READ(a) mock_read(a)
#define REG_WRITE(a,v) mock_write(a,v)
#include "../../ps_sources/frontend/memory_api_hw.c"
static uint8_t memory[0x800000];
static uint32_t ptr, read_count, write_count, read_data, flush_count, reset_seq;
static uint32_t dma_calls, maximum_dma, abort_on_dma, release_calls;
static uint8_t held, live, pending, ramworks;
static psdma_owner_t owner;
static uint8_t dma_busy, copy_available, copy_fault;
static uint32_t copy_source, copy_destination, copy_length, copy_status;
static uint32_t copy_completed, copy_starts, copy_aborts, copy_partial;
enum { COPY_OK, COPY_ERROR, COPY_STALL, COPY_ABORT_STUCK, COPY_SESSION_LOST,
       COPY_SHORT };
static uint64_t ticks;
static uint64_t tick_step = (COUNTS_PER_SECOND) / 10000U;
void XTime_GetTime(XTime *out) { ticks += tick_step; *out = ticks; }
void Xil_DCacheFlushRange(UINTPTR address, unsigned length) {(void)address; assert(length==512);}
void Xil_DCacheInvalidateRange(UINTPTR address, unsigned length) {(void)address; assert(length==512);}
psdma_owner_t psdma_current_owner(void) {return owner;}
psdma_result_t psdma_acquire(psdma_owner_t requested) {
    assert(requested==PSDMA_OWNER_MEMORY);
    if(owner!=PSDMA_OWNER_NONE)return PSDMA_ERR_OWNED;
    if(dma_busy)return PSDMA_ERR_BUSY;
    owner=requested;return PSDMA_OK;
}
void psdma_release(psdma_owner_t requested) {assert(owner==requested);owner=PSDMA_OWNER_NONE;}
psdma_result_t psdma_transfer_checked(psdma_owner_t owner,uint32_t physical,uint32_t ddr,uint32_t length,psdma_direction_t direction,uint32_t timeout,uint32_t abort_timeout,uint8_t(*guard)(void*),void *ctx) {
    assert(owner==PSDMA_OWNER_MEMORY && ddr==(uint32_t)(uintptr_t)bounce);
    assert(length && length<=512 && !(physical&7) && !(length&7));
    assert(timeout && abort_timeout && held && guard(ctx));
    assert(physical>=0x20000 && physical+length<=sizeof memory);
    dma_calls++; if(length>maximum_dma)maximum_dma=length;
    if(abort_on_dma==dma_calls) return PSDMA_ERR_ABORT;
    if(direction==PSDMA_MC_TO_DDR) memcpy(bounce,memory+physical,length);
    else memcpy(memory+physical,bounce,length);
    return PSDMA_OK;
}
static uint32_t mock_read(uint32_t a) {
    switch(a) {
    case CARD_CTRL_VTW_STATUS_REG:return live?MEM_LIVE_MASK:0;
    case CARD_CTRL_APPLE_RESET_STATUS_REG:return CARD_CTRL_APPLE_RESET_RES_BIT | reset_seq;
    case MEM_SP_STATUS:return pending?(MEM_SP_EXEC_PENDING | MEM_SP_EXEC_VTW):0;
    case MEM_RAMWORKS_ENABLE:return ramworks;
    case CARD_CTRL_VTW_RW_FLUSH_REG:return flush_count | (held?CARD_CTRL_VTW_RW_FLUSH_HELD_BIT:0);
    case CARD_CTRL_VTW_SHADOW_ADDR_REG:return ptr;
    case CARD_CTRL_VTW_SHADOW_READ4_STATUS_REG:return CARD_CTRL_VTW_SHADOW_READ4_READY_BIT | read_count;
    case CARD_CTRL_VTW_SHADOW_READ4_DATA_REG:return read_data;
    case CARD_CTRL_VTW_SHADOW_DATA4_STATUS_REG:return CARD_CTRL_VTW_SHADOW_DATA4_READY_BIT | write_count;
    case CARD_CTRL_VTW_COPY_SIGNATURE_REG:return copy_available?CARD_CTRL_VTW_COPY_SIGNATURE:0;
    case CARD_CTRL_VTW_COPY_STATUS_REG:return copy_status;
    case CARD_CTRL_VTW_COPY_COMPLETED_REG:return copy_completed;
    default:assert(0);return 0;
    }
}
static void mock_write(uint32_t a,uint32_t v) {
    unsigned i;
    switch(a) {
    case CARD_CTRL_VTW_RW_FLUSH_REG:
        if(v&CARD_CTRL_VTW_RW_FLUSH_REQ_BIT) {assert(!held);held=1;flush_count++;}
        if(v&CARD_CTRL_VTW_RW_FLUSH_RELEASE_BIT) {held=0;release_calls++;}
        break;
    case CARD_CTRL_VTW_SHADOW_ADDR_REG:ptr=v;break;
    case CARD_CTRL_VTW_SHADOW_READ4_REG:
        assert(held && ptr+4<=0x20000); read_data=0;
        for(i=0;i<4;i++)read_data|=(uint32_t)memory[ptr+i]<<(i*8);
        ptr+=4;read_count++;break;
    case CARD_CTRL_VTW_SHADOW_DATA4_REG:
        assert(held && ptr+4<=0x20000);
        for(i=0;i<4;i++)memory[ptr+i]=(uint8_t)(v>>(i*8));
        ptr+=4;write_count++;break;
    case CARD_CTRL_VTW_SHADOW_DATA_REG:assert(held);memory[ptr++]=(uint8_t)v;break;
    case CARD_CTRL_VTW_COPY_SOURCE_REG:copy_source=v;break;
    case CARD_CTRL_VTW_COPY_DESTINATION_REG:copy_destination=v;break;
    case CARD_CTRL_VTW_COPY_LENGTH_REG:copy_length=v;break;
    case CARD_CTRL_VTW_COPY_COMMAND_REG:
        if(v&CARD_CTRL_VTW_COPY_ABORT_BIT) {
            copy_aborts++;
            if(copy_fault!=COPY_ABORT_STUCK)copy_status=CARD_CTRL_VTW_COPY_ABORTED_BIT;
            break;
        }
        assert((v&CARD_CTRL_VTW_COPY_START_BIT) && held && live && pending);
        assert(owner==PSDMA_OWNER_MEMORY && !dma_busy && copy_available);
        assert(!(copy_status&CARD_CTRL_VTW_COPY_BUSY_BIT));
        assert(copy_length && copy_destination+copy_length<=sizeof memory);
        copy_starts++;copy_completed=copy_fault==COPY_OK?copy_length:copy_partial;
        assert(copy_completed<=copy_length);
        if(v&CARD_CTRL_VTW_COPY_FILL_BIT)
            memset(memory+copy_destination,(uint8_t)(v>>CARD_CTRL_VTW_COPY_FILL_SHIFT),copy_completed);
        else {
            assert(copy_source+copy_length<=sizeof memory);
            memcpy(memory+copy_destination,memory+copy_source,copy_completed);
        }
        if(copy_fault==COPY_OK || copy_fault==COPY_SHORT)copy_status=CARD_CTRL_VTW_COPY_DONE_BIT;
        else if(copy_fault==COPY_ERROR)copy_status=CARD_CTRL_VTW_COPY_ERROR_BIT;
        else copy_status=CARD_CTRL_VTW_COPY_BUSY_BIT;
        if(copy_fault==COPY_SESSION_LOST)live=0;
        break;
    default:assert(0);
    }
}
static void reset_mock(void) {
    memset(&state,0,sizeof state); held=0; live=1; pending=1; ramworks=1;
    ptr=0; read_count=0;write_count=0;read_data=0;flush_count=0;reset_seq=0;
    dma_calls=maximum_dma=abort_on_dma=release_calls=0;
    owner=PSDMA_OWNER_NONE;dma_busy=copy_available=copy_fault=0;
    copy_source=copy_destination=copy_length=copy_status=0;
    copy_completed=copy_starts=copy_aborts=copy_partial=0;
    memory_api_hw_prepare(1);
}
static void put16_mock(uint8_t *p,uint16_t v) {p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static void descriptor(uint8_t p[24],uint8_t srcspace,uint8_t srcbank,uint16_t srcaddr,uint8_t dstspace,uint8_t dstbank,uint16_t dstaddr,uint16_t length) {
    memset(p,0,24);memcpy(p,"AMEM",4);p[4]=1;p[5]=1;
    p[8]=1;p[9]=1;p[10]=srcspace;p[11]=srcbank;put16_mock(p+12,srcaddr);
    p[14]=dstspace;p[15]=dstbank;put16_mock(p+16,dstaddr);put16_mock(p+18,length);
}
static void test_microsecond_clock(void)
{
    /* The actual timer runs at 333333343 Hz. Check known answers, including
     * both sides of the BSP macro's old two-second discontinuity and the
     * intended 32-bit microsecond wrap after about 71.6 minutes. */
    static const struct {
        uint64_t ticks;
        uint32_t microseconds;
    } cases[] = {
        {0ULL, 0U},
        {333333342ULL, 999999U},
        {333333343ULL, 1000000U},
        {333333344ULL, 1000000U},
        {666666685ULL, 1999999U},
        {666666686ULL, 2000000U},
        {666666687ULL, 2000000U},
        {1333333372ULL, 4000000U},
        {1333333374ULL, 4000000U},
        {1431655806851ULL, UINT32_MAX},
        {1431655806852ULL, 0U},
        {1431655806853ULL, 0U},
    };
    unsigned i;
    uint32_t before, after;

    assert((COUNTS_PER_SECOND) == 333333343U);
    tick_step = 0U;
    for (i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        uint32_t actual;
        ticks = cases[i].ticks;
        actual = hw_micros(NULL);
        if (actual != cases[i].microseconds) {
            fprintf(stderr, "microsecond clock case %u: got %lu, expected %lu\n",
                    i, (unsigned long)actual,
                    (unsigned long)cases[i].microseconds);
        }
        assert(actual == cases[i].microseconds);
    }
    ticks = 666666685ULL;
    before = hw_micros(NULL);
    ticks = 666666687ULL;
    after = hw_micros(NULL);
    assert((uint32_t)(after - before) == 1U);
    /* The old expression also jumped forward by 750001 us over these
     * two microseconds around its four-second boundary. */
    ticks = 1333333041ULL;
    before = hw_micros(NULL);
    ticks = 1333333707ULL;
    after = hw_micros(NULL);
    assert((uint32_t)(after - before) == 2U);
    ticks = 1431655806851ULL;
    before = hw_micros(NULL);
    ticks = 1431655806852ULL;
    after = hw_micros(NULL);
    assert((uint32_t)(after - before) == 1U);
    ticks = 0U;
    tick_step = (COUNTS_PER_SECOND) / 10000U;
    puts("PASS hardware microsecond clock: BSP macro, second boundaries, 32-bit wrap");
}
static uint32_t progress(const uint8_t result[32]) {
    return (uint32_t)result[28] | ((uint32_t)result[29]<<8) |
           ((uint32_t)result[30]<<16) | ((uint32_t)result[31]<<24);
}
static void test_direct_failures(void) {
    uint8_t payload[24],result[32];
    unsigned fault;
    for(fault=COPY_ERROR;fault<=COPY_SHORT;fault++) {
        reset_mock();copy_available=1;copy_fault=(uint8_t)fault;copy_partial=13;
        descriptor(payload,0,0,0x2000,1,5,0x8001,700);
        memset(memory+0x2000,0x31,700);memset(memory+0x68000,0xA5,702);
        assert(memory_api_execute(payload,24,&memory_api_hardware)==
            (fault==COPY_ABORT_STUCK?MEMORY_API_UNSAFE:
             fault==COPY_SESSION_LOST?MEMORY_API_SESSION_LOST:MEMORY_API_IO));
        assert(copy_starts==1 && !dma_calls && !read_count && !write_count);
        assert(memory[0x68001]==0x31 && memory[0x6800d]==0x31 && memory[0x6800e]==0xA5);
        memory_api_status(result,&memory_api_hardware);
        assert(result[23]==0);
        if(fault==COPY_ABORT_STUCK) {
            assert(held && state.poisoned && release_calls==0 && owner==PSDMA_OWNER_MEMORY);
            assert(progress(result)==0);
            memory_api_hw_prepare(1);
            assert(memory_api_execute(payload,24,&memory_api_hardware)==MEMORY_API_UNSAFE);
            assert(copy_starts==1 && release_calls==0);
        } else {
            assert(!held && release_calls==1 && owner==PSDMA_OWNER_NONE);
            assert(progress(result)==13);
        }
        assert(copy_aborts==((fault==COPY_ERROR || fault==COPY_SHORT)?0U:1U));
    }
    /* A rejected start with zero progress still must not enter fallback. */
    reset_mock();copy_available=1;copy_fault=COPY_ERROR;
    descriptor(payload,0,0,0x2000,1,5,0x8001,700);
    assert(memory_api_execute(payload,24,&memory_api_hardware)==MEMORY_API_IO);
    assert(copy_starts==1 && !dma_calls && !read_count && !write_count);
    reset_mock();copy_available=1;dma_busy=1;
    assert(memory_api_execute(payload,24,&memory_api_hardware)==MEMORY_API_BUSY);
    assert(!copy_starts && !held && owner==PSDMA_OWNER_NONE);
    reset_mock();copy_available=1;owner=PSDMA_OWNER_SMARTPORT;
    assert(memory_api_execute(payload,24,&memory_api_hardware)==MEMORY_API_BUSY);
    assert(!copy_starts && !held && owner==PSDMA_OWNER_SMARTPORT);
    reset_mock();copy_available=1;copy_status=CARD_CTRL_VTW_COPY_BUSY_BIT;
    assert(memory_api_execute(payload,24,&memory_api_hardware)==MEMORY_API_BUSY);
    assert(!copy_starts && copy_aborts==1 && !held && owner==PSDMA_OWNER_NONE);
    puts("PASS direct copy failures: partial progress, timeout, drain, session loss, ownership, no fallback");
}
static void test_direct_fill_and_large_copy(void) {
    uint8_t payload[24],result[32];
    unsigned space,offset,i;
    for(space=0;space<3;space++)for(offset=0;offset<8;offset++) {
        uint32_t destination=(space==0?0:space==1?0x10000:0x7f0000)+0x8000+offset;
        reset_mock();copy_available=1;
        descriptor(payload,0,0,0,space!=0,space==2?126:0,(uint16_t)destination,1057);
        payload[8]=MEMORY_API_FILL;payload[20]=0x73;
        memset(memory+destination-1,0xA5,1059);
        assert(memory_api_execute(payload,24,&memory_api_hardware)==0);
        for(i=0;i<1057;i++)assert(memory[destination+i]==0x73);
        assert(memory[destination-1]==0xA5 && memory[destination+1057]==0xA5);
        assert(copy_starts==1 && !dma_calls && !read_count && !write_count);
    }
    reset_mock();copy_available=1;
    descriptor(payload,1,126,0x200,0,0,0x200,0xbe00);
    for(i=0;i<0xbe00;i++)memory[0x7f0200+i]=(uint8_t)(i*47U);
    memset(memory+0x1ff,0xA5,0xbe02);
    assert(memory_api_execute(payload,24,&memory_api_hardware)==0);
    assert(!memcmp(memory+0x200,memory+0x7f0200,0xbe00));
    assert(memory[0x1ff]==0xA5 && memory[0xc000]==0xA5 && copy_starts==1);
    memory_api_status(result,&memory_api_hardware);
    assert(result[23]==1 && progress(result)==0xbe00);
    puts("PASS direct fill alignments and full $0200-$BFFF descriptor");
}
int main(void) {
    uint8_t payload[24], result[32], expected[1600];
    unsigned srcspace,dstspace,so,doff,i,direct;
    test_microsecond_clock();
    for(direct=0;direct<2;direct++)
    for(srcspace=0;srcspace<3;srcspace++)for(dstspace=0;dstspace<3;dstspace++)
    for(so=0;so<8;so++)for(doff=0;doff<8;doff++) {
        uint32_t source=(srcspace==0?0:srcspace==1?0x10000:0x7F0000)+0x1000+so;
        uint32_t dest=(dstspace==0?0:dstspace==1?0x10000:0x60000)+0x8000+doff;
        reset_mock();
        copy_available=(uint8_t)direct;
        for(i=0;i<sizeof expected;i++)expected[i]=(uint8_t)(i*29+so);
        memcpy(memory+source,expected,sizeof expected);memset(memory+dest-8,0xA5,sizeof expected+16);
        descriptor(payload,srcspace!=0,srcspace==2?126:0,(uint16_t)source,dstspace!=0,dstspace==2?5:0,(uint16_t)dest,1600);
        assert(memory_api_execute(payload,24,&memory_api_hardware)==0);
        assert(!held && release_calls==1 && !memcmp(memory+dest,expected,sizeof expected));
        for(i=1;i<=8;i++)assert(memory[dest-i]==0xA5 && memory[dest+1600+i-1]==0xA5);
        assert(maximum_dma<=512);
        if(direct)assert(copy_starts==1 && !dma_calls && !read_count && !write_count);
        else assert(!copy_starts);
    }
    reset_mock();descriptor(payload,0,0,0x2000,1,5,0x8001,700);ramworks=0;
    assert(memory_api_execute(payload,24,&memory_api_hardware)==MEMORY_API_UNAVAILABLE);
    assert(!held && dma_calls==0);
    reset_mock();descriptor(payload,0,0,0x2000,1,5,0x8001,700);abort_on_dma=1;
    assert(memory_api_execute(payload,24,&memory_api_hardware)==MEMORY_API_UNSAFE);
    assert(held && release_calls==0 && state.poisoned);
    memory_api_status(result,&memory_api_hardware);assert(result[15]==0);
    memory_api_hw_prepare(1);
    assert(memory_api_execute(payload,24,&memory_api_hardware)==MEMORY_API_UNSAFE);
    assert(held && release_calls==0);
    reset_mock();descriptor(payload,0,0,0x2000,1,0,0x8000,700);payload[9]=0;
    assert(memory_api_execute(payload,24,&memory_api_hardware)==MEMORY_API_PRIVATE_REQUIRED);
    assert(!held && !dma_calls);
    puts("PASS backend fake-MMIO: 1152 direct/legacy alignment/direction cases, unaligned PSRAM edges, physical bank127, missing RAMWorks, sticky unsafe drain, PRIVATE");
    test_direct_failures();
    test_direct_fill_and_large_copy();
}
