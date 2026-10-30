/* Exercise the actual foreground engine with a W5100S register model.
 * Only time, SD, MMIO and NIC access are replaced. TLS is the real library. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define COMMON_H
static uint32_t slot_mask;
#define REG_READ(addr) ((void)(addr),slot_mask)
#define REG_WRITE(addr,value) ((void)(addr),slot_mask=(value))
#include "../../ps_sources/frontend/http_service.c"

uint64_t host_time;
#ifndef HTTP_HOST_FAKE_SD
/* Network-state cases must never need first-use entropy or touch storage. */
int http_entropy_seed(uint8_t out[HTTP_SEED_BYTES]) { (void)out; return -1; }
void http_entropy_status(http_entropy_stats_t *out)
{ memset(out,0,sizeof(*out));out->result=HTTP_ENTROPY_UNAVAILABLE; }
const char *http_entropy_result_name(int result) { (void)result;return "fixture unavailable"; }
#endif
static uint8_t ir[2], socket_state;
static unsigned ir_reads, rx_reads, closes, connects, sends, clear_flags;
static unsigned tx_bytes;
static uint8_t dns_wire[520];
static uint16_t rx_available;
static int fail_ir;
static uint8_t host_dns[4];
void uart_puts(uint32_t uart,const char *text) { (void)uart;(void)text; }
uint8_t ftp_sd_service_active(void) { return 0; }
int usb_sdd_service_active(void) { return 0; }
void net_service_reset(void) {}
uint32_t net_service_json_max_us(void) { return 0; }
uint16_t uthernet2_w5100_socket_reg(uint8_t socket,uint16_t offset)
{ return (uint16_t)(0x400U+0x100U*socket+offset); }
int uthernet2_read_reg(uint16_t address,uint8_t *out)
{
    assert((address&255)==W5100_SN_IR);
    ++ir_reads;
    if(fail_ir)return -1;
    *out=ir[(address-0x400U)/0x100U];return 0;
}
int uthernet2_write_reg(uint16_t address,uint8_t value)
{
    if((address&255)==W5100_SN_IR) {
        ir[(address-0x400U)/0x100U]&=(uint8_t)~value;++clear_flags;
    }
    return 0;
}
int uthernet2_w5100_socket_command(uint8_t socket,uint8_t command)
{
    (void)socket;
    if(command==W5100_CR_CLOSE)++closes;
    if(command==4)++connects;
    if(command==W5100_CR_SEND)++sends;
    return 0;
}
int uthernet2_w5100_socket_status(uint8_t socket,uint8_t *out)
{ (void)socket;*out=socket_state;return 0; }
/* These network-state cases model NIC access; separate controller tests
 * exercise the actual W5100S allocation/readback and wrap helpers. */
int uthernet2_w5100_set_rx_layout(uint8_t update_only)
{ (void)update_only;return 0; }
int uthernet2_w5100_read16_stable(uint16_t address,uint16_t *out)
{ assert((address&255)==W5100_SN_RX_RSR);++rx_reads;*out=rx_available;return 0; }
int uthernet2_w5100_read16(uint16_t a,uint16_t *out) { (void)a;*out=0;return 0; }
int uthernet2_w5100_write16(uint16_t a,uint16_t value) { (void)a;(void)value;return 0; }
int uthernet2_read_block(uint16_t a,uint8_t *out,uint16_t n) { (void)a;memset(out,0,n);return 0; }
int uthernet2_write_block(uint16_t a,const uint8_t *p,uint16_t n) { (void)a;(void)p;(void)n;return 0; }
int uthernet2_w5100_socket_ring_read(uint8_t s,uint16_t a,uint8_t *p,uint16_t n)
{ assert(s==SOCKET_DNS&&a+n<=sizeof(dns_wire));memcpy(p,dns_wire+a,n);return 0; }
int uthernet2_w5100_socket_ring_write(uint8_t s,uint16_t a,const uint8_t *p,uint16_t n)
{ (void)a;assert(s==SOCKET_DNS&&p[0]==0x12&&p[1]==0x34);tx_bytes+=n;return 0; }
int uthernet2_read_network_config(uthernet2_network_config_t *out)
{ memset(out,0,sizeof(*out));memcpy(out->dns,host_dns,sizeof(host_dns));return 0; }
int uthernet2_test(uthernet2_test_result_t *out) { memset(out,0,sizeof(*out));return 0; }

static void setup(uint8_t state)
{
    memset(&g_http,0,sizeof(g_http));memset(ir,0,sizeof(ir));
    ir_reads=rx_reads=closes=connects=sends=clear_flags=tx_bytes=0;rx_available=0;fail_ir=0;
    host_time=1600000;slot_mask=0x94;socket_state=W5100_SR_CLOSED;
    g_http.status.state=state;g_http.owns_eth=1;g_http.saved_mask=0x96;
    g_http.deadline=30000000;g_http.dns_deadline=3000000;
    g_http.dns_id=0x1234;strcpy(g_http.url.host,"example.test");
    memcpy(g_http.dns,"\x0a\x11\x01\x11",4);
    mbedtls_ssl_init(&g_http.ssl);
}
static void failed(int result)
{
    http_status_t status;
    http_service_poll();http_service_status(&status);
    assert(status.state==HTTP_S_FAILED&&status.result==result);
    assert(!g_http.owns_eth&&slot_mask==0x96&&closes>=2&&clear_flags>=2);
    assert(!ir[0]&&!ir[1]);
    unsigned reads=ir_reads;http_service_poll();
    assert(ir_reads==reads); /* Final failure stops all further retries. */
}
static void dns_answer(int negative)
{
    memcpy(dns_wire,g_http.dns,4);dns_wire[4]=0;dns_wire[5]=53;
    int n=http_dns_query(g_http.url.host,g_http.dns_id,dns_wire+8,sizeof(dns_wire)-8);
    assert(n>0);dns_wire[10]=0x81;dns_wire[11]=(uint8_t)(negative?0x83:0x80);
    if(!negative) {
        static const uint8_t answer[]={0xC0,0x0C,0,1,0,1,0,0,0,60,0,4,10,17,1,17};
        dns_wire[15]=1;memcpy(dns_wire+8+n,answer,sizeof(answer));n+=sizeof(answer);
    }
    dns_wire[6]=(uint8_t)(n>>8);dns_wire[7]=(uint8_t)n;rx_available=(uint16_t)(n+8);
}
int main(void)
{
    /* Per-request setup must use boot-cached state. All SD stubs fail. */
    boot_prepared=0;assert(request_config()==HTTP_E_SEED);
    boot_prepared=1;prepare_error=HTTP_E_ARGUMENT;assert(request_config()==HTTP_E_ARGUMENT);
    prepare_error=0;g_http.crypto_ready=1;g_http.reseed_needed=0;
    memcpy(configured_dns,"\x08\x08\x08\x08",4);
    assert(!request_config()&&!memcmp(g_http.dns,configured_dns,4));
    g_http.reseed_needed=1;assert(request_config()==HTTP_E_SEED);
    g_http.reseed_needed=0;
    setup(HTTP_S_DNS_WAIT);ir[1]=W5100_IR_TIMEOUT;http_service_poll();
    assert(g_http.status.state==HTTP_S_DNS_SEND&&ir_reads==1&&!rx_reads);
    assert(!ir[1]&&clear_flags==1&&!closes&&g_http.deadline==30000000);
    http_service_poll();assert(g_http.status.state==HTTP_S_DNS_WAIT&&sends==1&&tx_bytes>0);
    assert(g_http.dns_deadline==host_time+DNS_TIMEOUT);
    /* Several ARP failures can precede forwarding. A late reply still succeeds. */
    for(unsigned i=0;i<14;++i) {
        host_time+=1600000;ir[1]=W5100_IR_TIMEOUT;http_service_poll();
        assert(g_http.status.state==HTTP_S_DNS_SEND&&!ir[1]);http_service_poll();
        assert(g_http.status.state==HTTP_S_DNS_WAIT&&g_http.deadline==30000000);
    }
    host_time=29000000;dns_answer(0);http_service_poll();
    assert(g_http.status.state==HTTP_S_OPEN&&!memcmp(g_http.peer,"\x0a\x11\x01\x11",4));
    assert(!connects&&sends==15); /* DNS retries did not send HTTP data. */
    setup(HTTP_S_DNS_WAIT);host_time=30000000;ir[1]=W5100_IR_TIMEOUT;
    failed(HTTP_E_NETWORK_NOT_READY);
    setup(HTTP_S_DNS_WAIT);http_service_poll();
    assert(g_http.status.state==HTTP_S_DNS_WAIT&&ir_reads==1&&rx_reads==1&&!closes);
    host_time=3100000;http_service_poll();assert(g_http.status.state==HTTP_S_DNS_SEND&&!closes);
    http_service_poll();assert(sends==1&&g_http.dns_deadline==6100000);
    host_time=5000000;dns_answer(0);http_service_poll();assert(g_http.status.state==HTTP_S_OPEN);
    setup(HTTP_S_DNS_WAIT);dns_answer(1);failed(HTTP_E_DNS);
    setup(HTTP_S_DNS_WAIT);host_time=30000000;failed(HTTP_E_NETWORK_NOT_READY);
    setup(HTTP_S_DNS_WAIT);fail_ir=1;failed(HTTP_E_IO);
    setup(HTTP_S_DNS_WAIT);http_service_cancel();assert(g_http.status.result==HTTP_E_CANCELLED);
    setup(HTTP_S_CONNECT);ir[0]=W5100_IR_TIMEOUT;http_service_poll();
    assert(g_http.status.state==HTTP_S_OPEN&&!ir[0]&&closes==1&&clear_flags==2);
    assert(g_http.connect_at==host_time+CONNECT_RETRY_US&&g_http.deadline==30000000);
    http_service_poll();assert(closes==1&&!connects&&!sends); /* Cooperative backoff. */
    host_time+=CONNECT_RETRY_US;http_service_poll();assert(g_http.status.state==HTTP_S_CONNECT);
    socket_state=W5100_SR_INIT;http_service_poll();assert(connects==1);
    socket_state=W5100_SR_ESTABLISHED;strcpy(g_http.method,"POST");
    strcpy(g_http.url.path,"/");g_http.url.port=80;http_service_poll();
    assert(g_http.status.state==HTTP_S_WRITE&&!sends); /* Exactly one pending POST. */
    setup(HTTP_S_CONNECT);http_service_poll();assert(g_http.status.state==HTTP_S_OPEN);
    host_time=30000000;failed(HTTP_E_NETWORK_NOT_READY); /* Closed without TIMEOUT also bounded. */
    setup(HTTP_S_CONNECT);host_time=30000000;failed(HTTP_E_NETWORK_NOT_READY);
    setup(HTTP_S_CONNECT);socket_state=W5100_SR_INIT;http_service_poll();
    assert(g_http.status.state==HTTP_S_CONNECT&&connects==1&&g_http.sending);
    setup(HTTP_S_CONNECT);socket_state=W5100_SR_ESTABLISHED;
    strcpy(g_http.method,"GET");strcpy(g_http.url.host,"example.test");
    strcpy(g_http.url.path,"/");g_http.url.port=80;http_service_poll();
    assert(g_http.status.state==HTTP_S_WRITE&&g_http.request_length);
    setup(HTTP_S_WRITE);g_http.sending=1;ir[0]=W5100_IR_TIMEOUT;
    g_http.request_length=1;failed(HTTP_E_TIMEOUT); /* Established SEND unchanged. */
    puts("PASS W5100S DNS/connect retries, lost DNS reply, 30s deadline, cancellation, IO, cleanup and no POST replay");
    return 0;
}
