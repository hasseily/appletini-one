/* Actual controller C through the card's command/status MMIO protocol. Only
 * the ARM dsb instruction is changed to a host compiler barrier by the runner. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define COMMON_H
static uint32_t bridge_read(uint32_t address);
static void bridge_write(uint32_t address,uint32_t value);
#define REG_READ(address) bridge_read(address)
#define REG_WRITE(address,value) bridge_write(address,value)
#include W5100_TEST_SOURCE

uint64_t host_time;
static uint8_t memory[65536],sequence;
static uint16_t selected;
static uint32_t bridge_status;
static uint8_t bridge_data;
static int fail_read=-1,fail_write=-1,corrupt_read=-1;
static unsigned writes,reads,groups,max_rx_sum;
static uint32_t bridge_read(uint32_t address)
{assert(address==CARD_CTRL_ETH_STATUS_REG);return bridge_status|CARD_CTRL_ETH_STATUS_READY;}
static void bridge_write(uint32_t address,uint32_t value)
{
    if(address==CARD_CTRL_ETH_ADDR_REG){selected=(uint16_t)value;return;}
    if(address==CARD_CTRL_ETH_DATA_REG){bridge_data=(uint8_t)value;return;}
    assert(address==CARD_CTRL_ETH_CMD_REG&&(value&CARD_CTRL_ETH_CMD_GO));
    unsigned error=0;uint8_t response=0;
    if(value&CARD_CTRL_ETH_CMD_WRITE) {
        ++writes;
        if(selected==fail_write)error=1;
        else {
            memory[selected]=bridge_data;
            if(selected==W5100_REG_RMSR&&bridge_data==W5100_SOCKET_MEM_4_2_1_1)
                for(unsigned i=0;i<4;++i)memory[0x400+256*i+W5100_SN_RXBUF_SIZE]=(uint8_t)(i?i==1?2:1:4);
            if(selected>=0x400&&selected<0x800&&(selected&255)==W5100_SN_RXBUF_SIZE) {
                unsigned sum=0;for(unsigned i=0;i<4;++i)sum+=memory[0x400+256*i+W5100_SN_RXBUF_SIZE];
                assert(sum<=8);if(sum>max_rx_sum)max_rx_sum=sum;
            }
        }
    } else {
        ++reads;if(selected==fail_read)error=1;
        response=memory[selected];if(selected==corrupt_read)response^=1;
    }
    sequence=(uint8_t)((sequence+1U)&CARD_CTRL_ETH_STATUS_SEQ_MASK);
    bridge_status=CARD_CTRL_ETH_STATUS_DONE|((uint32_t)sequence<<CARD_CTRL_ETH_STATUS_SEQ_SHIFT)|
        ((uint32_t)response<<CARD_CTRL_ETH_STATUS_RDATA_SHIFT)|(error?CARD_CTRL_ETH_STATUS_ERROR:0);
}
static void normal(void)
{
    fail_read=fail_write=corrupt_read=-1;
    for(unsigned i=0;i<4;++i)memory[0x400+256*i+W5100_SN_SR]=W5100_SR_CLOSED;
    assert(!uthernet2_w5100_set_rx_layout(0)&&!rx_update_only);
    for(unsigned i=0;i<4;++i)assert(memory[0x400+256*i+W5100_SN_RXBUF_SIZE]==(i?i==1?2:1:4));
}
static void check_rx(unsigned socket,unsigned base,unsigned capacity,uint16_t pointer,unsigned n)
{
    uint8_t bytes[8192];assert(n<=sizeof bytes);
    assert(!uthernet2_w5100_socket_ring_read((uint8_t)socket,pointer,bytes,(uint16_t)n));
    for(unsigned i=0;i<n;++i)assert(bytes[i]==memory[base+((pointer+i)&(capacity-1))]);
    ++groups;
}
int main(void)
{
    normal();
    for(unsigned i=0;i<8192;++i)memory[0x6000+i]=(uint8_t)(i*17+(i>>4));
    const unsigned base[4]={0x6000,0x7000,0x7800,0x7c00},size[4]={4096,2048,1024,1024};
    for(unsigned i=0;i<4;++i)check_rx(i,base[i],size[i],(uint16_t)(size[i]-7),31);
    for(unsigned socket=0;socket<4;++socket) {
        memory[0x400+256*socket+W5100_SN_SR]=W5100_SR_INIT;unsigned before=writes;
        assert(uthernet2_w5100_set_rx_layout(1)<0&&writes==before&&!rx_update_only);
        memory[0x400+256*socket+W5100_SN_SR]=W5100_SR_CLOSED;++groups;
    }
    assert(!uthernet2_w5100_set_rx_layout(1)&&rx_update_only);
    assert(memory[0x400+W5100_SN_RXBUF_SIZE]==8);
    for(unsigned i=1;i<4;++i)assert(!memory[0x400+256*i+W5100_SN_RXBUF_SIZE]);
    check_rx(0,0x6000,8192,65529,41);check_rx(0,0x6000,8192,1,8192);
    uint8_t bytes[64];
    for(unsigned socket=1;socket<4;++socket){unsigned before=reads;assert(uthernet2_w5100_socket_ring_read((uint8_t)socket,0,bytes,1)<0&&reads==before);++groups;}
    for(unsigned i=0;i<sizeof bytes;++i)bytes[i]=(uint8_t)i;
    assert(!uthernet2_w5100_socket_ring_write(1,2041,bytes,sizeof bytes));
    for(unsigned i=0;i<sizeof bytes;++i)assert(memory[0x5000+((2041+i)&2047)]==bytes[i]);++groups;
    /* Failed normal readback must not publish the normal software mapping. */
    corrupt_read=0x400+W5100_SN_RXBUF_SIZE;
    assert(uthernet2_w5100_set_rx_layout(0)<0&&rx_update_only);++groups;
    normal();check_rx(1,0x7000,2048,2041,41);
    corrupt_read=0x700+W5100_SN_RXBUF_SIZE;
    assert(uthernet2_w5100_set_rx_layout(1)<0&&!rx_update_only);++groups;normal();
    fail_write=0x600+W5100_SN_RXBUF_SIZE;
    assert(uthernet2_w5100_set_rx_layout(1)<0&&!rx_update_only);++groups;normal();
    fail_read=0x6000;assert(uthernet2_w5100_socket_ring_read(0,0,bytes,1)<0);++groups;
    fail_read=-1;assert(uthernet2_w5100_socket_ring_read(4,0,bytes,1)<0);++groups;
    assert(max_rx_sum==8&&!rx_update_only);
    printf("PASS %u actual W5100S layout/ring groups: 4+2+1+1 and8+0+0+0, 16bitwrap, disabledDNS, unchangedTX, allCLOSED, read/write/readbackfaults, noearlypublish; MMIOcommands reads=%u writes=%u\n",groups,reads,writes);
    return 0;
}
