/* Actual HTTP engine/ring/poll/redirect/restoration with deterministic NIC. */
#define main pipeline_fixture_main
#include "online_update_perf_host.c"
#undef main

static uint8_t pattern(size_t at){return (uint8_t)(at*17+(at>>3));}
static void begin_owner(uint8_t who)
{
    assert(!g_http.owns_eth);memset(&g_http,0,sizeof g_http);mbedtls_ssl_init(&g_http.ssl);
    memset(registers,0,sizeof registers);slot_mask=2;layout=layout_fail=0;
    enforce_layout_closed=enforce_guest_layout=1;rx_cost_us=0;host_time=1000000;
    http_request_t request={"http://192.0.2.1/blob","GET","",NULL,0,0};
    assert(!http_service_start_owned(&request,who));assert(!claim()&&!(slot_mask&2U));
    g_http.status.state=HTTP_S_OPEN;memset(g_http.peer,1,sizeof g_http.peer);
    assert(!step());assert(g_http.status.state==HTTP_S_CONNECT);
    assert(registers[0x400+W5100_SN_MR]==(who==HTTP_OWNER_UPDATE?0x21:0x01));
    assert(layout==(unsigned)(who==HTTP_OWNER_UPDATE));
    g_http.status.state=HTTP_S_READ;registers[0x400+W5100_SN_SR]=W5100_SR_ESTABLISHED;
    http_parser_init(&g_http.parser,0,UINT32_MAX);
}
static void response(size_t n)
{
    char header[128];int h=snprintf(header,sizeof header,"HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n\r\n",n);
    free(wire);wire=malloc((size_t)h+n);assert(wire);memcpy(wire,header,(size_t)h);
    for(size_t i=0;i<n;++i)wire[h+i]=pattern(i);wire_n=(size_t)h+n;wire_at=rx_pointer=0;
}
static void ring_case(uint8_t who)
{
    begin_owner(who);response(400003);
    size_t capacity=who==HTTP_OWNER_UPDATE?HTTP_UPDATE_STREAM_BYTES:HTTP_DEFAULT_STREAM_BYTES;
    g_http.ring_at=capacity-19;
    for(unsigned i=0;i<100;++i){size_t before=wire_at;http_service_poll();if(wire_at==before&&g_http.ring_count)break;}
    size_t chunk=who==HTTP_OWNER_UPDATE?HTTP_UPDATE_IO_BYTES:HTTP_DEFAULT_IO_BYTES;
    assert(g_http.ring_count>capacity-chunk&&g_http.ring_count<=capacity&&http_service_active());size_t stalled=wire_at;
    for(unsigned i=0;i<4;++i)http_service_poll();assert(wire_at==stalled);
    http_service_poll_budget(0);assert(wire_at==stalled);
    uint8_t out[65537];size_t at=0;
    for(unsigned i=0;i<20000&&(http_service_active()||g_http.ring_count);++i) {
        size_t maximum=i%3==0?17:i%3==1?4093:sizeof out;
        assert(!http_service_read_owned(who==HTTP_OWNER_UPDATE?HTTP_OWNER_NET:HTTP_OWNER_UPDATE,out,maximum));
        size_t n=http_service_read_owned(who,out,maximum);
        for(size_t j=0;j<n;++j)assert(out[j]==pattern(at+j));at+=n;
        http_service_poll();assert(g_http.ring_count<=capacity);
    }
    assert(at==400003&&g_http.status.state==HTTP_S_DONE&&!g_http.owns_eth&&!layout&&(slot_mask&2U));
    assert(!http_service_release_owned(who));
}
int main(void)
{
    assert(http_service_filter_slot_mask(0xa7U)==0xa7U);
    ring_case(HTTP_OWNER_NET);ring_case(HTTP_OWNER_UPDATE);
    begin_owner(HTTP_OWNER_UPDATE);response(300000);rx_cost_us=50;
    unsigned before=rx_calls;http_service_poll_budget(120);assert(rx_calls-before>=1&&rx_calls-before<=3);
    assert(!http_service_cancel_owned(HTTP_OWNER_UPDATE)&&!layout&&(slot_mask&2U));
    begin_owner(HTTP_OWNER_NET);response(100000);before=rx_calls;http_service_poll_budget(120);assert(rx_calls==before);
    http_service_poll();assert(rx_calls-before<=8&&rx_max<=4096);assert(!http_service_cancel_owned(HTTP_OWNER_NET));

    begin_owner(HTTP_OWNER_UPDATE);response(10);layout_fail=1;
    slot_mask=http_service_filter_slot_mask(0xa7U);assert(slot_mask==0xa5U&&(g_http.saved_mask&2U));
    slot_mask=http_service_filter_slot_mask(0xa5U);assert(slot_mask==0xa5U&&!(g_http.saved_mask&2U));
    assert(http_service_cancel_owned(HTTP_OWNER_UPDATE)==HTTP_E_IO);
    assert(g_http.status.ethernet_restore_pending&&g_http.owns_eth&&g_http.owner==HTTP_OWNER_UPDATE&&!(slot_mask&2U));
    http_request_t other={"http://192.0.2.2/","GET","",NULL,0,0};assert(http_service_start(&other)==HTTP_E_BUSY);
    http_service_poll();assert(g_http.owner==HTTP_OWNER_UPDATE&&!(slot_mask&2U));
    slot_mask=http_service_filter_slot_mask(0xc7U);assert(slot_mask==0xc5U&&(g_http.saved_mask&2U));
    layout_fail=0;http_service_poll();assert(!g_http.owns_eth&&!g_http.status.ethernet_restore_pending&&g_http.owner==HTTP_OWNER_NONE&&!layout&&slot_mask==0xc7U);
    begin_owner(HTTP_OWNER_UPDATE);slot_mask=http_service_filter_slot_mask(0x81U);
    assert(!http_service_cancel_owned(HTTP_OWNER_UPDATE)&&slot_mask==0x81U&&!layout);

    for(unsigned failed=0;failed<2;++failed) {
        begin_owner(HTTP_OWNER_UPDATE);
        mbedtls_memory_buffer_alloc_init(tls_heap,sizeof tls_heap);mbedtls_ctr_drbg_init(&g_http.drbg);
        mbedtls_ctr_drbg_set_entropy_len(&g_http.drbg,HTTP_SEED_BYTES);memset(g_http.entropy,0x63,sizeof g_http.entropy);
        assert(!mbedtls_ctr_drbg_seed(&g_http.drbg,entropy,NULL,(const uint8_t *)"redirect",8));
        g_http.dns[0]=8;layout_fail=(int)failed;
        const char *redirect_text="HTTP/1.1 302 Found\r\nLocation: http://other.test/final\r\nContent-Length: 0\r\n\r\n";
        free(wire);wire_n=strlen(redirect_text);wire=malloc(wire_n);assert(wire);memcpy(wire,redirect_text,wire_n);wire_at=rx_pointer=0;
        http_service_poll();
        if(failed) {
            assert(g_http.status.ethernet_restore_pending&&g_http.owns_eth&&!(slot_mask&2U));
            layout_fail=0;http_service_poll();assert(!g_http.owns_eth&&!layout&&(slot_mask&2U));
        } else assert(g_http.status.state==HTTP_S_DNS_OPEN&&!layout&&g_http.owns_eth&&!(slot_mask&2U));
        assert(!http_service_cancel_owned(HTTP_OWNER_UPDATE));crypto_reset();mbedtls_memory_buffer_alloc_free();
    }
    puts("PASS actual HTTP throughput groups: NET4KiB/UPDATE128KiB backpressure+2spanwrap, binary400003B, ownerisolation, budget0/clamped/noNETwork, TCPmode1/21, cancelrestorefailurehold/retry, redirectDNSnormalmap+failurehold");
    return 0;
}
