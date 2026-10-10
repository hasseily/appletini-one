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
#ifndef MEMORY_API_HW_SOURCE
#define MEMORY_API_HW_SOURCE "../../ps_sources/frontend/memory_api_hw.c"
#endif
#ifndef EXPECT_COPY_ROWS
#define EXPECT_COPY_ROWS 1
#endif
#ifndef EXPECT_BATCH_LEASE
#define EXPECT_BATCH_LEASE 1
#endif
#include MEMORY_API_HW_SOURCE
static uint8_t memory[0x800000];
static uint32_t ptr, read_count, write_count, read_data, flush_count, reset_seq;
static uint32_t dma_calls, maximum_dma, abort_on_dma, release_calls;
static uint8_t held, live, pending, ramworks;
static psdma_owner_t owner;
static uint8_t dma_busy, copy_available, copy_fault;
static uint32_t copy_source, copy_destination, copy_length, copy_status, copy_rows;
static uint32_t copy_completed, copy_starts, copy_aborts, copy_partial;
static uint32_t copy_capabilities, copy_cap_reads, copy_cap_drop_at, copy_cap_drop_mask;
static uint32_t mmio_reads, mmio_writes, acquire_calls, owner_release_calls;
static uint32_t fault_at_start, lose_after_complete;
static uint8_t loss_kind;
enum { LOSS_NONE, LOSS_RESET, LOSS_PENDING, LOSS_LIVE, LOSS_HOLD, LOSS_MODE };
static uint32_t captured_count, captured_addresses[32768];
static uint8_t captured_data[32768];
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
    acquire_calls++;
    if(owner!=PSDMA_OWNER_NONE)return PSDMA_ERR_OWNED;
    if(dma_busy)return PSDMA_ERR_BUSY;
    owner=requested;return PSDMA_OK;
}
void psdma_release(psdma_owner_t requested) {assert(owner==requested);owner=PSDMA_OWNER_NONE;owner_release_calls++;}
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
    mmio_reads++;
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
    case CARD_CTRL_VTW_COPY_CAPABILITIES_REG:
        if (++copy_cap_reads == copy_cap_drop_at) copy_capabilities &= ~copy_cap_drop_mask;
        return copy_capabilities;
    case CARD_CTRL_VTW_COPY_STATUS_REG:return copy_status;
    case CARD_CTRL_VTW_COPY_COMPLETED_REG:
        /* Change the session after a descriptor has completed, before the
         * next START. Both old and leased paths must stop at the same prefix. */
        if(copy_starts==lose_after_complete) {
            if(loss_kind==LOSS_RESET)reset_seq++;
            if(loss_kind==LOSS_PENDING)pending=0;
            if(loss_kind==LOSS_LIVE)live=0;
            if(loss_kind==LOSS_HOLD)held=0;
            if(loss_kind==LOSS_MODE)copy_capabilities&=~CARD_CTRL_VTW_COPY_CAP_SHR_ACTIVE;
        }
        return copy_completed;
    default:assert(0);return 0;
    }
}
static void mock_write(uint32_t a,uint32_t v) {
    unsigned i;
    mmio_writes++;
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
    case CARD_CTRL_VTW_COPY_ROWS_REG:copy_rows=v;break;
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
        copy_starts++;
        {
            unsigned rows = (v&CARD_CTRL_VTW_COPY_ROWS_BIT) ? (copy_rows&255)+1 : 1;
            unsigned sg = (v&CARD_CTRL_VTW_COPY_ROWS_BIT) ? (copy_rows>>8)&255 : 0;
            unsigned dg = (v&CARD_CTRL_VTW_COPY_ROWS_BIT) ? (copy_rows>>16)&255 : 0;
            unsigned total = copy_length*rows;
            assert(total<=65535);
            if(v&CARD_CTRL_VTW_COPY_ROWS_BIT)assert(copy_capabilities&CARD_CTRL_VTW_COPY_CAP_ROWS);
            copy_completed=(fault_at_start && copy_starts!=fault_at_start) || copy_fault==COPY_OK ? total:copy_partial;
            assert(copy_completed<=total);
            for(i=0;i<copy_completed;i++) {
                unsigned dst=copy_destination+i+(i/copy_length)*dg;
                unsigned src=copy_source+i+(i/copy_length)*sg;
                assert(dst<sizeof memory && src<sizeof memory);
                memory[dst]=(v&CARD_CTRL_VTW_COPY_FILL_BIT)?(uint8_t)(v>>CARD_CTRL_VTW_COPY_FILL_SHIFT):memory[src];
                if(v&CARD_CTRL_VTW_COPY_PUBLISH_SHR_BIT) {
                    assert((copy_capabilities&7)==7 && dst>=0x12000 && dst<0x19d00);
                    assert(captured_count<32768);
                    captured_addresses[captured_count]=dst;
                    captured_data[captured_count++]=memory[dst];
                }
            }
        }
        if((fault_at_start && copy_starts!=fault_at_start) || copy_fault==COPY_OK || copy_fault==COPY_SHORT)copy_status=CARD_CTRL_VTW_COPY_DONE_BIT;
        else if(copy_fault==COPY_ERROR)copy_status=CARD_CTRL_VTW_COPY_ERROR_BIT;
        else copy_status=CARD_CTRL_VTW_COPY_BUSY_BIT;
        if(copy_fault==COPY_SESSION_LOST && (!fault_at_start || copy_starts==fault_at_start))live=0;
        break;
    default:assert(0);
    }
}
static void reset_mock(void) {
    memset(&state,0,sizeof state); held=0; live=1; pending=1; ramworks=1;
    ptr=0; read_count=0;write_count=0;read_data=0;flush_count=0;reset_seq=0;
    dma_calls=maximum_dma=abort_on_dma=release_calls=0;
    owner=PSDMA_OWNER_NONE;dma_busy=copy_available=copy_fault=0;
    copy_source=copy_destination=copy_length=copy_status=copy_rows=0;
    copy_completed=copy_starts=copy_aborts=copy_partial=0;
    copy_capabilities=copy_cap_reads=copy_cap_drop_at=captured_count=0;
    copy_cap_drop_mask=CARD_CTRL_VTW_COPY_CAP_SHR_ACTIVE;
    mmio_reads=mmio_writes=acquire_calls=owner_release_calls=0;
    fault_at_start=lose_after_complete=0;loss_kind=LOSS_NONE;
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
static void test_publication_capabilities_and_validation(void) {
    uint8_t payload[40], item[24], result[32];
    unsigned cap;
    /* Old VCP1 and absent engines must not advertise publication. New
     * hardware advertises support before SHR is selected, not readiness. */
    for(cap=0;cap<8;cap++) {
        reset_mock();copy_available=1;copy_capabilities=cap;
        memory_api_status(result,&memory_api_hardware);
        assert((result[8]&MEMORY_API_FEATURE_PUBLISH_SHR)==((cap&1)?8:0));
        descriptor(payload,0,0,0x0800,1,0,0x2000,128);payload[9]=2;
        assert(memory_api_execute(payload,24,&memory_api_hardware)==
               (cap==7?MEMORY_API_OK:MEMORY_API_UNAVAILABLE));
        assert(copy_starts==(cap==7?1U:0U));
        assert(captured_count==(cap==7?128U:0U));
        assert(!dma_calls && !read_count && !write_count && !held);
    }
    reset_mock();copy_capabilities=7;
    memory_api_status(result,&memory_api_hardware);assert(result[8]==7);
    descriptor(payload,0,0,0x0800,1,0,0x2000,128);payload[9]=2;
    assert(memory_api_execute(payload,24,&memory_api_hardware)==MEMORY_API_UNAVAILABLE);
    assert(!copy_starts && !captured_count && !dma_calls && !write_count);
    /* A later publication failure cannot follow an earlier private write. */
    reset_mock();copy_available=1;copy_capabilities=1;
    descriptor(payload,0,0,0x0800,1,0,0x1000,128);payload[5]=2;
    descriptor(item,0,0,0x0800,1,0,0x2000,128);item[9]=2;
    memcpy(payload+24,item+8,16);
    assert(memory_api_execute(payload,40,&memory_api_hardware)==MEMORY_API_UNAVAILABLE);
    assert(!copy_starts && !captured_count && release_calls==1);
    /* Readiness is checked again immediately before a descriptor starts. */
    reset_mock();copy_available=1;copy_capabilities=7;copy_cap_drop_at=2;
    descriptor(payload,0,0,0x0800,1,0,0x2000,128);payload[9]=2;
    assert(memory_api_execute(payload,24,&memory_api_hardware)==MEMORY_API_IO);
    assert(!copy_starts && !captured_count && !read_count && !write_count && !dma_calls);
    puts("PASS publication capabilities: absent/old engine, mode/egress gates, whole-batch hold validation, no scalar fallback");
}
static void test_publication_data_and_failures(void) {
    uint8_t payload[8+8*16], item[24], result[32];
    unsigned row,i,srcspace,offset;
    for(srcspace=0;srcspace<3;srcspace++)for(offset=0;offset<8;offset++) {
        uint32_t source=(srcspace==0?0:srcspace==1?0x10000:0x7f0000)+0x0800+offset;
        reset_mock();copy_available=1;copy_capabilities=7;
        for(i=0;i<1024;i++)memory[source+i]=(uint8_t)(i*29+offset);
        memset(memory+0x12000,0xA5,1280);
        descriptor(payload,srcspace!=0,srcspace==2?126:0,(uint16_t)source,1,0,0x2000,128);
        payload[5]=8;payload[9]=2;
        for(row=1;row<8;row++) {
            descriptor(item,srcspace!=0,srcspace==2?126:0,(uint16_t)(source+row*128),1,0,
                       (uint16_t)(0x2000+row*160),128);
            item[9]=2;memcpy(payload+8+16*row,item+8,16);
        }
        assert(memory_api_execute(payload,sizeof payload,&memory_api_hardware)==0);
        assert(copy_starts==8 && captured_count==1024 && !held);
        for(row=0;row<8;row++) {
            for(i=0;i<128;i++) {
                assert(captured_addresses[row*128+i]==0x12000+row*160+i);
                assert(captured_data[row*128+i]==memory[source+row*128+i]);
            }
            for(i=128;i<160;i++)assert(memory[0x12000+row*160+i]==0xA5);
        }
        memory_api_status(result,&memory_api_hardware);
        assert(result[23]==8 && progress(result)==1024);
        assert(!dma_calls && !read_count && !write_count);
    }
    reset_mock();copy_available=1;copy_capabilities=7;
    descriptor(item,0,0,0,1,0,0x9cff,1);item[8]=MEMORY_API_FILL;item[9]=2;item[20]=0x77;
    assert(memory_api_execute(item,24,&memory_api_hardware)==0);
    assert(captured_count==1 && captured_addresses[0]==0x19cff && captured_data[0]==0x77);
    /* PRIVATE remains private even on a publication-capable engine. */
    item[9]=1;item[20]=0x33;
    assert(memory_api_execute(item,24,&memory_api_hardware)==0);
    assert(memory[0x19cff]==0x33 && captured_count==1);
    for(i=COPY_ERROR;i<=COPY_SHORT;i++) {
        reset_mock();copy_available=1;copy_capabilities=7;copy_fault=(uint8_t)i;copy_partial=13;
        descriptor(item,0,0,0x0800,1,0,0x2000,128);item[9]=2;
        assert(memory_api_execute(item,24,&memory_api_hardware)==
               (i==COPY_SESSION_LOST?MEMORY_API_SESSION_LOST:i==COPY_ABORT_STUCK?MEMORY_API_UNSAFE:MEMORY_API_IO));
        assert(copy_starts==1 && captured_count==13 && !dma_calls && !read_count && !write_count);
        if(i==COPY_ABORT_STUCK)assert(held && state.poisoned && owner==PSDMA_OWNER_MEMORY && !release_calls);
        else {
            memory_api_status(result,&memory_api_hardware);
            assert(progress(result)==13 && !held && owner==PSDMA_OWNER_NONE);
        }
    }
    puts("PASS publication data: ordered 8-row strips from MAIN/AUX/PSRAM, unaligned sources, fill edge, unchanged PRIVATE, exact partial/drain errors");
}

static void batch(uint8_t *payload, unsigned count, uint8_t publish)
{
    uint8_t item[24];
    unsigned i;
    for(i=0;i<count;i++) {
        descriptor(item,0,0,(uint16_t)(0x0800+i*128),1,publish?0:16,
                   (uint16_t)(0x2000+i*160),128);
        item[9]=publish?MEMORY_API_PUBLISH_SHR:MEMORY_API_PRIVATE;
        if(i==0)memcpy(payload,item,8);
        memcpy(payload+8+i*16,item+8,16);
    }
    payload[5]=(uint8_t)count;
}

static void test_batch_lease_boundaries(void)
{
    uint8_t payload[264], result[32];
    unsigned publish,at,kind,fault,i,repeat;
    for(publish=0;publish<2;publish++) {
        unsigned count=publish?8:16;
        reset_mock();copy_available=1;copy_capabilities=7;
        batch(payload,count,(uint8_t)publish);
        for(i=0;i<count*128;i++)memory[0x0800+i]=(uint8_t)(i*17+(i>>7));
        /* A lease is per hold, never retained across a successful request. */
        for(repeat=0;repeat<2;repeat++) {
            uint32_t before_reads=mmio_reads, before_writes=mmio_writes;
            uint32_t before_claim=acquire_calls, before_release=owner_release_calls;
            assert(memory_api_execute(payload,(uint16_t)(8+16*count),&memory_api_hardware)==0);
            assert(!held && owner==PSDMA_OWNER_NONE);
            assert(acquire_calls-before_claim==(EXPECT_BATCH_LEASE?1U:count));
            assert(owner_release_calls-before_release==(EXPECT_BATCH_LEASE?1U:count));
            assert(mmio_reads-before_reads+acquire_calls-before_claim ==
                   (publish ? (EXPECT_BATCH_LEASE?118U:257U) :
                              (EXPECT_BATCH_LEASE?186U:306U)));
            assert(mmio_writes-before_writes==(publish?35U:67U));
            for(i=0;i<count;i++)assert(!memcmp(memory+0x0800+i*128,
                memory+(publish?0x10000:0x110000)+0x2000+i*160,128));
            /* psdma_acquire has one real status read, represented separately
             * by this fixture's mock. These counts feed the source cost model. */
            printf("BATCH_MMIO lease=%u publish=%u descriptors=%u reads=%lu writes=%lu claims=%lu\n",
                   (unsigned)EXPECT_BATCH_LEASE,publish,count,
                   (unsigned long)(mmio_reads-before_reads+acquire_calls-before_claim),
                   (unsigned long)(mmio_writes-before_writes),
                   (unsigned long)(acquire_calls-before_claim));
        }
        for(at=1;at<count;at++)for(kind=LOSS_RESET;kind<=LOSS_MODE;kind++) {
            if(kind==LOSS_MODE && !publish)continue;
            reset_mock();copy_available=1;copy_capabilities=7;
            batch(payload,count,(uint8_t)publish);
            lose_after_complete=at;loss_kind=(uint8_t)kind;
            assert(memory_api_execute(payload,(uint16_t)(8+16*count),&memory_api_hardware)==
                   (kind==LOSS_MODE?MEMORY_API_IO:MEMORY_API_SESSION_LOST));
            assert(copy_starts==at && !held && owner==PSDMA_OWNER_NONE);
            assert(!dma_calls && !read_count && !write_count && !copy_aborts);
            memory_api_status(result,&memory_api_hardware);
            assert(result[23]==at && progress(result)==at*128);
            assert(captured_count==(publish?at*128:0));
        }
        for(at=1;at<=count;at++)for(fault=COPY_ERROR;fault<=COPY_SHORT;fault++) {
            reset_mock();copy_available=1;copy_capabilities=7;
            batch(payload,count,(uint8_t)publish);
            fault_at_start=at;copy_fault=(uint8_t)fault;copy_partial=13;
            assert(memory_api_execute(payload,(uint16_t)(8+16*count),&memory_api_hardware)==
                   (fault==COPY_ABORT_STUCK?MEMORY_API_UNSAFE:
                    fault==COPY_SESSION_LOST?MEMORY_API_SESSION_LOST:MEMORY_API_IO));
            assert(copy_starts==at && !dma_calls && !read_count && !write_count);
            memory_api_status(result,&memory_api_hardware);
            assert(result[23]==at-1);
            assert(progress(result)==(at-1)*128+(fault==COPY_ABORT_STUCK?0:13));
            if(fault==COPY_ABORT_STUCK)
                assert(held && owner==PSDMA_OWNER_MEMORY && !release_calls && state.poisoned);
            else assert(!held && owner==PSDMA_OWNER_NONE && release_calls==1);
        }
    }
    puts("PASS batch lease: repeated requests, every descriptor session boundary and partial/error/timeout/unsafe position");
}

#if EXPECT_COPY_ROWS
static void rows_descriptor(uint8_t p[24],unsigned flags)
{
    descriptor(p,0,0,0x801,1,0,0x2011,128);
    p[8]=MEMORY_API_COPY_ROWS;p[9]=(uint8_t)flags;p[21]=7;p[22]=3;p[23]=32;
}
static void test_copy_rows(void)
{
    unsigned caps,mode,i,row;uint8_t payload[40],status[32];
    for(mode=0;mode<2;mode++)for(caps=0;caps<16;caps++) {
        reset_mock();copy_available=1;copy_capabilities=caps;rows_descriptor(payload,mode?2:1);
        assert(memory_api_execute(payload,24,&memory_api_hardware)==
               ((caps&(mode?15:8))==(mode?15:8)?0:MEMORY_API_UNAVAILABLE));
        assert(copy_starts==((caps&(mode?15:8))==(mode?15:8)?1U:0U));
        assert(!held && owner==PSDMA_OWNER_NONE && !dma_calls && !read_count && !write_count);
        memory_api_status(status,&memory_api_hardware);
        assert((status[8]&MEMORY_API_FEATURE_COPY_ROWS)==((caps&8)?MEMORY_API_FEATURE_COPY_ROWS:0));
    }
    reset_mock();copy_available=1;copy_capabilities=15;rows_descriptor(payload,2);
    for(i=0;i<1600;i++)memory[0x801+i]=(uint8_t)(i*37+1);
    memset(memory+0x12000,0xA5,1500);
    assert(memory_api_execute(payload,24,&memory_api_hardware)==0);
    assert(copy_starts==1 && copy_completed==1024 && captured_count==1024);
    assert(mmio_reads+acquire_calls==49 && mmio_writes==8 && acquire_calls==1);
    printf("ROWS_MMIO publish=1 rows=8 descriptors=1 reads=%u writes=%u claims=%u\n",mmio_reads+acquire_calls,mmio_writes,acquire_calls);
    for(row=0;row<8;row++)for(i=0;i<160;i++) {
        unsigned dst=0x12011+row*160+i;
        assert(memory[dst]==(i<128?memory[0x801+row*131+i]:0xA5));
        if(i<128)assert(captured_addresses[row*128+i]==dst && captured_data[row*128+i]==memory[dst]);
    }
    /* Older hardware/capability loss fails before first START, never scalar fallback. */
    for(mode=0;mode<3;mode++) {
        reset_mock();copy_available=mode!=0;copy_capabilities=mode==1?7:15;rows_descriptor(payload,2);
        if(mode==2){copy_cap_drop_at=3;copy_cap_drop_mask=CARD_CTRL_VTW_COPY_CAP_ROWS;}
        assert(memory_api_execute(payload,24,&memory_api_hardware)==(mode==2?MEMORY_API_IO:MEMORY_API_UNAVAILABLE));
        assert(!copy_starts && !dma_calls && !read_count && !write_count && !held);
    }
    /* An invalid later row span rejects the whole list before any writes. */
    reset_mock();copy_available=1;copy_capabilities=15;rows_descriptor(payload,2);
    memcpy(payload+24,payload+8,16);payload[5]=2;put16_mock(payload+32,0x9C00);
    assert(memory_api_execute(payload,40,&memory_api_hardware)==MEMORY_API_RANGE && !copy_starts);
    reset_mock();copy_available=1;copy_capabilities=15;rows_descriptor(payload,2);
    memcpy(payload+24,payload+8,16);payload[5]=2;
    put16_mock(payload+34,0);payload[37]=payload[38]=payload[39]=1;
    assert(memory_api_execute(payload,40,&memory_api_hardware)==MEMORY_API_RANGE && !copy_starts && !held && !flush_count);
    for(mode=COPY_ERROR;mode<=COPY_SHORT;mode++) {
        reset_mock();copy_available=1;copy_capabilities=15;rows_descriptor(payload,2);
        copy_fault=(uint8_t)mode;copy_partial=137;
        assert(memory_api_execute(payload,24,&memory_api_hardware)==
               (mode==COPY_ABORT_STUCK?MEMORY_API_UNSAFE:mode==COPY_SESSION_LOST?MEMORY_API_SESSION_LOST:MEMORY_API_IO));
        assert(copy_starts==1 && captured_count==137 && !dma_calls && !read_count && !write_count);
        memory_api_status(status,&memory_api_hardware);
        if(mode!=COPY_ABORT_STUCK)assert(status[23]==0 && (status[28]|(status[29]<<8))==137);
        assert(held==(mode==COPY_ABORT_STUCK));
        assert(owner==(mode==COPY_ABORT_STUCK?PSDMA_OWNER_MEMORY:PSDMA_OWNER_NONE));
    }
    puts("PASS COPY_ROWS hardware: all16capabilities, strided ordered pixels+borders, old engine, cap loss, whole batch range, partial/session/timeout/unsafe no fallback");
}
#endif

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
    test_publication_capabilities_and_validation();
    test_publication_data_and_failures();
    test_batch_lease_boundaries();
#if EXPECT_COPY_ROWS
    test_copy_rows();
#endif
}
