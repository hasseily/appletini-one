#include "net_service.h"
#include "net_json.h"
#include "http_service.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static http_status_t backend;
static uint8_t body[32768];
static size_t bytes, consumed;
static unsigned starts, cancels;
int http_service_start(const http_request_t *r)
{
    ++starts;memset(&backend,0,sizeof(backend));backend.state=HTTP_S_READ;
    backend.retained=r->retain;strcpy(backend.url,r->url);strcpy(backend.content_type,"application/json");
    bytes=consumed=0;return 0;
}
void http_service_cancel(void){++cancels;backend.state=HTTP_S_FAILED;backend.result=HTTP_E_CANCELLED;}
void http_service_status(http_status_t *out){*out=backend;out->buffered=(uint32_t)(bytes-consumed);out->body_bytes=(uint32_t)bytes;}
const uint8_t *http_service_retained(size_t *n){*n=bytes;return body;}
size_t http_service_read(uint8_t *out,size_t n){if(n>bytes-consumed)n=bytes-consumed;memcpy(out,body+consumed,n);consumed+=n;return n;}
static net_json_t json;
static void document(const char *s,int valid)
{
    size_t n=strlen(s);memcpy(body,s,n);net_json_init(&json,body);
    for(size_t i=0;i<n;++i)net_json_feed(&json,i+1,i+1==n,1);
    if(!n)net_json_feed(&json,0,1,1);
    assert((!json.bad&&json.complete)==valid);
}
static void lookup(const char *p,int result,const char *s)
{
    net_json_lookup(&json,(const uint8_t *)p,strlen(p),1);
    for(unsigned i=0;json.result==NJ_PENDING&&i<100000;++i)net_json_poll(&json,1);
    assert(json.result==result);
    if(s){assert(json.value_length==strlen(s));assert(!memcmp(json.value,s,strlen(s)));}
}
static void json_tests(void)
{
    document("{\"scores\":[{\"name\":\"A\\u00e9\\ud83d\\ude80\",\"n\":-0.12e+3}],\"a/b\":{\"~x\":null},\"dup\":1,\"dup\":2}",1);
    lookup("/scores/0/name",NJ_FOUND,"A\xc3\xa9\xf0\x9f\x9a\x80");
    lookup("/scores/0/n",NJ_FOUND,"-0.12e+3");lookup("/a~1b/~0x",NJ_FOUND,"null");
    lookup("/dup",NJ_FOUND,"1");lookup("/scores/01",NJ_BAD_PATH,NULL);
    lookup("/scores/-",NJ_BAD_PATH,NULL);lookup("/scores/1",NJ_MISSING,NULL);
    lookup("/absent",NJ_MISSING,NULL);lookup("/dup/x",NJ_MISSING,NULL);
    lookup("bad",NJ_BAD_PATH,NULL);lookup("/~2",NJ_BAD_PATH,NULL);
    lookup("/scores/0",NJ_FOUND,"{\"name\":\"A\\u00e9\\ud83d\\ude80\",\"n\":-0.12e+3}");
    document("[true,false,null,0,1,1.5,1e2,-1E-2,{},[],\"\"]",1);
    lookup("/0",NJ_FOUND,"true");lookup("/1",NJ_FOUND,"false");lookup("/10",NJ_FOUND,"");
    const char *bad[]={"", " ","[1,]","{\"a\":1,}","{1:2}","01","-","1.","1e+","tru","truefalse",
        "[}","{]","[1 2]","{\"x\" 1}","{\"x\":}","\"\\ud800\"","\"\\udc00\"","\"\\ud800\\u1234\"",
        "\"\xc0\x80\"","\"\xed\xa0\x80\"","\"\xf4\x90\x80\x80\"","\"\xe2\x82\"","\"\\x\""};
    for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);++i)document(bad[i],0);
    document("[[[[[[[[[[[[[[[[0]]]]]]]]]]]]]]]]",1);
    document("[[[[[[[[[[[[[[[[[0]]]]]]]]]]]]]]]]]",0);
    document("\"hello\"",1);lookup("",NJ_FOUND,"hello");
    net_json_lookup(&json,(const uint8_t *)"",0,0);assert(json.result==NJ_NO_DOCUMENT);
    document("[",0);lookup("",NJ_BAD_DOCUMENT,NULL);
    /* Maximum scalar/string and maximum possible token count fit without a
     * hidden, smaller document limit. Validation always advances by its budget. */
    body[0]='"';memset(body+1,'a',32766);body[32767]='"';
    net_json_init(&json,body);
    while(json.at<32768){unsigned old=json.at;net_json_feed(&json,32768,1,7);assert(json.at-old<=7);}
    assert(json.complete&&!json.bad);lookup("",NJ_FOUND,NULL);assert(json.value_length==32766);
    body[0]='[';for(unsigned i=1;i<32766;++i)body[i]=(i&1)?'0':',';body[32766]=']';body[32767]=' ';
    net_json_init(&json,body);net_json_feed(&json,32768,1,32768);assert(json.complete&&!json.bad);
}
static uint8_t packet[1013],reply[514],list[9]={3,0,0,0,0,0,0,0,2};
static unsigned read_at(unsigned selector,unsigned offset,unsigned max)
{
    list[4]=(uint8_t)selector;list[5]=(uint8_t)offset;list[6]=(uint8_t)(offset>>8);
    list[7]=(uint8_t)max;list[8]=(uint8_t)(max>>8);memset(reply,0xA5,sizeof(reply));
    unsigned n=net_service_status((uint8_t)selector,list,reply,sizeof(reply)-2);
    assert(n<=max||selector==0x82);assert(reply[512]==0xA5&&reply[513]==0xA5);return n;
}
static unsigned start(int retain)
{
    memset(packet,0,12);packet[0]=1;packet[1]=NET_START;packet[2]=(uint8_t)retain;
    packet[3]=3;const char *url="https://example.test/a";packet[4]=(uint8_t)strlen(url);
    memcpy(packet+12,"GET",3);memcpy(packet+15,url,strlen(url));
    unsigned before=starts;
    assert(!net_service_control(packet,15+strlen(url)));assert(starts==before);
    assert(read_at(0x82,0,64)==64&&!memcmp(reply,"ATNH",4));assert(reply[8]==NET_QUEUED);
    unsigned generation=reply[7];assert(generation);
    assert(read_at(0x83,0,128)==8&&reply[1]==NET_MORE);
    net_service_poll();assert(starts==before+1);return generation;
}
static void transport_tests(void)
{
    net_service_reset();net_service_poll();net_service_set_via_hz(1017812);
    unsigned g=start(1);
    memcpy(body,"{\"value\":42}",12);bytes=12;backend.state=HTTP_S_DONE;backend.status=200;net_service_poll();
    assert(read_at(0x82,0,64)==64&&reply[7]==g&&reply[8]==NET_DONE&&reply[12]==200);
    for(unsigned max=9;max<=512;++max) {
        unsigned n=read_at(0x83,0,max);assert(n==(max<20?max:20));assert(reply[6]==n-8);
        assert(!memcmp(reply+8,body,n-8));assert(reply[1]==(n==20?NET_END:NET_MORE));
        uint8_t saved[512];memcpy(saved,reply,n);assert(read_at(0x83,0,max)==n&&!memcmp(reply,saved,n));
    }
    memset(packet,0,12);packet[0]=1;packet[1]=NET_JSON;packet[4]=6;memcpy(packet+12,"/value",6);
    assert(!net_service_control(packet,18));net_service_poll();
    assert(read_at(0x84,0,128)==10&&reply[1]==NET_END&&!memcmp(reply+8,"42",2));
    assert(read_at(0x85,0,128)>8&&!memcmp(reply+8,"https://",8));
    assert(read_at(0x86,0,128)==24&&!memcmp(reply+8,"application/json",16));
    assert(read_at(0x83,13,128)==8&&reply[1]==NET_LOST);
    assert(!read_at(0x83,0,8)&&!read_at(0x83,0,513));
    assert(start(0)!=g);memset(body,'x',1024);bytes=1024;net_service_poll();
    assert(read_at(0x83,0,128)==128);uint8_t saved[128];memcpy(saved,reply,128);
    assert(read_at(0x83,0,128)==128&&!memcmp(saved,reply,128));
    assert(read_at(0x83,100,128)==28); /* replay suffix, never a fresh consume */
    assert(read_at(0x83,120,128)==128);assert(read_at(0x83,0,128)==8&&reply[1]==NET_LOST);
    assert(read_at(0x83,1000,128)==8&&reply[1]==NET_LOST);
    /* Receipt can finish before the Apple drains the stream. The final
     * reply and a repeat of it must report END with current ring occupancy. */
    start(0);memset(body,'z',32768);bytes=32768;backend.state=HTTP_S_DONE;net_service_poll();
    unsigned offset=0;
    while(offset<bytes) {
        unsigned n=read_at(0x83,offset,128)-8;
        assert(n>0&&reply[1]==(offset+n==bytes?NET_END:NET_MORE));offset+=n;
    }
    assert(read_at(0x83,offset,128)==8&&reply[1]==NET_END);
    start(0);
    backend.state=HTTP_S_FAILED;backend.result=HTTP_E_NETWORK_NOT_READY;net_service_poll();
    assert(read_at(0x82,0,64)==64&&reply[8]==NET_FAILED);
    assert((int16_t)((unsigned)reply[10]|((unsigned)reply[11]<<8))==HTTP_E_NETWORK_NOT_READY);
    assert(read_at(0x83,240,128)==8&&reply[1]==NET_SOURCE_FAILED);
    memset(packet,0,12);packet[0]=1;packet[1]=NET_CANCEL;
    unsigned before=cancels;assert(!net_service_control(packet,12)&&cancels==before);net_service_poll();assert(cancels==before+1);
    net_service_reset();assert(read_at(0x82,0,64)==64&&reply[7]==0&&reply[8]==NET_IDLE);net_service_poll();
    /* Generation zero stays reserved, including rollover and START replacement. */
    for(unsigned i=0;i<300;++i)start(i&1);
    packet[0]=2;assert(net_service_control(packet,12)==0x21);
    assert(net_service_control(packet,1013)==0x21);
}
int main(void){json_tests();transport_tests();puts("PASS strict incremental JSON, pointer lookup, production protocol, lifecycle, replay and reply bounds");return 0;}
