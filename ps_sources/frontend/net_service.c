#if defined(APPLETINI_HTTP_SERVICE) || defined(NET_SERVICE_HOST)
#include "net_service.h"
#include "net_json.h"
#include "http_service.h"
#include "http_download.h"
#include <string.h>
#ifndef NET_SERVICE_HOST
#include "xiltimer.h"
#endif
static uint32_t json_max_us;
static uint64_t json_clock(void)
{
#ifdef NET_SERVICE_HOST
    return 0;
#else
    XTime ticks;XTime_GetTime(&ticks);
    return (uint64_t)ticks*1000000ULL/(uint64_t)COUNTS_PER_SECOND;
#endif
}
static void json_measure(uint64_t before)
{uint64_t elapsed=json_clock()-before;if(elapsed>json_max_us)json_max_us=(uint32_t)elapsed;}
uint32_t net_service_json_max_us(void){return json_max_us;}

static struct {
    uint8_t generation, state, retain, pending, flags, download_handoff;
    int16_t result;
    uint8_t packet[NET_REQUEST_MAX], replay[504];
    uint16_t packet_length, replay_length;
    uint32_t replay_offset, stream_offset, via_hz;
    char method[16], url[HTTP_URL_MAX], headers[NET_REQUEST_MAX+1U];
    char download_hint[HTTP_FILENAME_MAX];
    http_status_t status;
    net_json_t json;
} net;
static uint16_t get16(const uint8_t *p) {return (uint16_t)(p[0]|((unsigned)p[1]<<8));}
static void put16(uint8_t *p,uint16_t n) {p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8);}
static void put32(uint8_t *p,uint32_t n) {put16(p,(uint16_t)n);put16(p+2,(uint16_t)(n>>16));}
static void drop(void)
{
    net.replay_length=0;net.replay_offset=net.stream_offset=0;
    net.json.result=NJ_NONE;net.json.value_length=0;net.json.value_type=0;
    memset(&net.status,0,sizeof(net.status));
}
void net_service_set_via_hz(uint32_t hz) {net.via_hz=hz;}
void net_service_reset(void)
{
    /* Called at foreground scope: finish private-file cleanup before an SD
     * handover. Ordinary NET socket cleanup remains deferred to its poll. */
    http_download_reset();
    net.pending=NET_CANCEL;net.state=NET_IDLE;net.generation=0;net.result=0;
    net.flags=net.download_handoff=0;drop();
}
uint8_t net_service_control(const uint8_t *p,size_t length)
{
    if(!p||length<NET_START_HEADER||length>NET_REQUEST_MAX||p[0]!=NET_VERSION)return 0x21;
    unsigned op=p[1],method=p[3];
    size_t url=get16(p+4),headers=get16(p+6),body=get16(p+8),hint=get16(p+10);
    if(op==NET_START) {
        if((p[2]&~7U)||((p[2]&NET_FLAG_RETAIN)&&(p[2]&6U))||
           !method||method>15||!url||url>=HTTP_URL_MAX||hint>=HTTP_FILENAME_MAX||
           (hint&&!(p[2]&NET_FLAG_FORCE_DOWNLOAD))||
           NET_START_HEADER+method+url+headers+body+hint!=length)return 0x21;
        if((p[2]&6U)&&(method!=3||body||memcmp(p+12,"GET",3)))return 0x21;
        /* Text fields are length delimited on the wire, never embedded NUL. */
        if(memchr(p+NET_START_HEADER,0,method+url+headers)||
           memchr(p+NET_START_HEADER+method+url+headers+body,0,hint))return 0x21;
        if(http_download_active())return 0x2F;
        memcpy(net.packet,p,length);net.packet_length=(uint16_t)length;
        drop();net.flags=p[2];net.retain=p[2]&NET_FLAG_RETAIN;
        net.download_handoff=0;
        if(p[2]&6U)http_download_wait();else http_download_reset();
        net.state=NET_QUEUED;net.result=0;
        if(!++net.generation)++net.generation;
        net.pending=NET_START;return 0;
    }
    if(p[2]||method||headers||body||hint)return 0x21;
    if(op==NET_CANCEL&&length==NET_START_HEADER&&!url) {
        http_download_cancel();
        net.pending=NET_CANCEL;net.state=NET_CANCELLED;net.result=HTTP_E_CANCELLED;drop();return 0;
    }
    if(op==NET_JSON&&url<=NET_JSON_PATH&&NET_START_HEADER+url==length) {
        /* A JSON command must not replace a START still waiting for its poll. */
        if(net.pending)return 0x2F;
        net_json_lookup(&net.json,p+NET_START_HEADER,url,net.state==NET_DONE&&net.retain);
        return 0;
    }
    return 0x21;
}
void net_service_poll(void)
{
    if(net.pending) {
        unsigned op=net.pending;net.pending=0;
        if(http_service_getowner()==HTTP_OWNER_NET)
            (void)http_service_cancel_owned(HTTP_OWNER_NET);
        if(op==NET_START) {
            const uint8_t *p=net.packet;unsigned m=p[3],u=get16(p+4),h=get16(p+6),b=get16(p+8),f=get16(p+10);
            memcpy(net.method,p+12,m);net.method[m]=0;
            memcpy(net.url,p+12+m,u);net.url[u]=0;
            memcpy(net.headers,p+12+m+u,h);net.headers[h]=0;
            memcpy(net.download_hint,p+12+m+u+h+b,f);net.download_hint[f]=0;
            http_request_t request={net.url,net.method,net.headers,p+12+m+u+h,b,net.retain};
            int result=http_service_start_owned(&request,HTTP_OWNER_NET);
            if(result){net.state=NET_FAILED;net.result=(int16_t)result;
                if(net.flags&6U)http_download_fail(result);
                return;}
            size_t length;
            net_json_init(&net.json,http_service_retained_owned(HTTP_OWNER_NET,&length));
            net.state=NET_ACTIVE;
        }
    }
    if(net.state==NET_DONE&&!net.download_handoff&&http_service_getowner()==HTTP_OWNER_NET)
        http_service_status(&net.status);
    if(net.state==NET_ACTIVE) {
        http_download_status_t saved;
        http_download_status(&saved);
        if(net.download_handoff) {
            int had_owner=http_service_getowner()==HTTP_OWNER_DOWNLOAD;
            if(had_owner)http_service_status(&net.status);
            http_download_poll();http_download_status(&saved);
            /* No other foreground client can start between these calls.
             * Capture final counters even if commit just released its owner. */
            if(had_owner)http_service_status(&net.status);
            if(saved.state==HTTP_DL_DONE)net.state=NET_DONE;
            else if(saved.state==HTTP_DL_FAILED||saved.state==HTTP_DL_CANCELLED) {
                net.result=saved.result;net.state=saved.state==HTTP_DL_CANCELLED?NET_CANCELLED:NET_FAILED;
            }
            return;
        }
        if(http_service_getowner()!=HTTP_OWNER_NET) {
            net.state=NET_FAILED;net.result=HTTP_E_CANCELLED;
            if(net.flags&6U)http_download_fail(net.result);
            return;
        }
        http_service_status(&net.status);
        if(net.status.state==HTTP_S_FAILED) {
            net.result=(int16_t)net.status.result;
            net.state=net.result==HTTP_E_CANCELLED?NET_CANCELLED:NET_FAILED;
            net.replay_length=0;net.json.result=NJ_NONE;
            if(net.flags&6U)http_download_fail(net.result);
        } else {
            if(saved.state==HTTP_DL_WAITING&&net.status.headers_ready&&
               net.status.status>=200&&net.status.status<300) {
                if((net.flags&NET_FLAG_FORCE_DOWNLOAD)||net.status.attachment) {
                    net.download_handoff=1;net.replay_length=0;net.json.result=NJ_NONE;
                    int result=http_download_begin(&net.status,net.download_hint);
                    if(result) {http_download_fail(result);net.state=NET_FAILED;net.result=(int16_t)result;}
                    return;
                }
                http_download_reset(); /* Ordinary page: expose its unread ring. */
            }
            else if(saved.state==HTTP_DL_WAITING && net.status.headers_ready &&
                    (net.status.status>=400 || net.status.state==HTTP_S_DONE)) {
                if(net.flags&NET_FLAG_FORCE_DOWNLOAD) {
                    http_download_fail(HTTP_DL_E_HTTP);net.state=NET_FAILED;
                    net.result=HTTP_DL_E_HTTP;return;
                }
                http_download_reset(); /* Final non-2xx belongs to the page client. */
            }
            size_t length;
            (void)http_service_retained_owned(HTTP_OWNER_NET,&length);
            int done=net.status.state==HTTP_S_DONE;
            if(net.retain) {
                uint64_t before=json_clock();net_json_feed(&net.json,length,done,256);json_measure(before);
            }
            if(done&&(!net.retain||net.json.bad||net.json.complete))net.state=NET_DONE;
        }
    }
    uint64_t before=json_clock();net_json_poll(&net.json,256);json_measure(before);
}
static uint16_t read_source(uint8_t selector,uint16_t offset,uint16_t max,uint8_t *out)
{
    const uint8_t *source=NULL;
    size_t length=0,n=0;
    uint8_t state=net.state==NET_FAILED||net.state==NET_CANCELLED?NET_SOURCE_FAILED:NET_MORE;
    if(net.state==NET_IDLE)state=NET_END;
    else if(state!=NET_SOURCE_FAILED&&net.state!=NET_QUEUED) {
        if(selector==0x87) {source=(const uint8_t*)http_download_path();length=strlen((const char*)source);}
        else if(selector==0x83) {
            http_download_status_t saved;http_download_status(&saved);
            if(net.download_handoff||saved.state==HTTP_DL_WAITING) {
                /* Never expose attachment bytes to the page consumer. */
                out[0]=net.generation;out[1]=net.download_handoff?NET_SOURCE_FAILED:NET_MORE;
                put32(out+2,offset);put16(out+6,0);return NET_REPLY_HEADER;
            }
            if(net.retain)source=http_service_retained_owned(HTTP_OWNER_NET,&length);
            else {
                uint32_t end=net.replay_offset+net.replay_length;
                if(offset<net.replay_offset||offset>end)state=NET_LOST;
                else if(offset<end) {
                    source=net.replay+(offset-net.replay_offset);length=end-offset;
                } else {
                    net.replay_offset=offset;
                    net.replay_length=(uint16_t)http_service_read_owned(HTTP_OWNER_NET,net.replay,max-NET_REPLY_HEADER);
                    net.stream_offset=offset+net.replay_length;
                    source=net.replay;length=net.replay_length;
                }
                /* Body reads drain the ring after receipt finishes. Do not
                 * use the occupancy cached when ACTIVE first became DONE. */
                http_service_status(&net.status);
                if(state!=NET_LOST&&net.state==NET_DONE&&offset+length==net.stream_offset&&!net.status.buffered)
                    state=NET_END;
                /* Stream pointers above already refer to the requested offset. */
                n=length;
            }
        } else if(selector==0x84) {
            if(net.json.result==NJ_FOUND){source=net.json.value;length=net.json.value_length;state=NET_END;}
            else if(net.json.result!=NJ_PENDING)state=NET_END;
        } else if(selector==0x85) {source=(const uint8_t *)net.status.url;length=strlen(net.status.url);}
        else if(selector==0x86) {source=(const uint8_t *)net.status.content_type;length=strlen(net.status.content_type);}
        if(selector!=0x83||net.retain) {
            if(offset>length)state=NET_LOST;
            else {if(source)source+=offset;n=length-offset;}
            if(net.state==NET_DONE&&state!=NET_LOST)state=NET_END;
        }
    }
    if(n>max-NET_REPLY_HEADER)n=max-NET_REPLY_HEADER;
    if(state==NET_SOURCE_FAILED||state==NET_LOST)n=0;
    /* END means this reply reaches the end, not merely that receipt finished. */
    if(state==NET_END&&offset<=length&&n<length-((selector==0x83&&!net.retain)?0:offset))state=NET_MORE;
    out[0]=net.generation;out[1]=state;put32(out+2,offset);put16(out+6,(uint16_t)n);
    if(n)memcpy(out+NET_REPLY_HEADER,source,n);
    return (uint16_t)(NET_REPLY_HEADER+n);
}
uint16_t net_service_status(uint8_t selector,const uint8_t list[9],uint8_t *out,size_t capacity)
{
    if(!out||!list||list[0]!=3||list[1]||selector<NET_SELECTOR||selector>NET_LAST_SELECTOR)return 0;
    if(selector==NET_SELECTOR) {
        if(capacity<NET_DESCRIPTOR_SIZE)return 0;
        memset(out,0,NET_DESCRIPTOR_SIZE);memcpy(out,"ATNH",4);
        out[4]=NET_VERSION;out[5]=NET_DESCRIPTOR_SIZE;out[6]=15|NET_CAP_DOWNLOAD;out[7]=net.generation;
        out[8]=net.state;out[9]=net.retain;put16(out+10,(uint16_t)net.result);
        put16(out+12,net.status.status);out[14]=net.json.result;out[15]=net.json.value_type;
        put32(out+16,net.status.body_bytes);put32(out+20,net.status.buffered);
        put32(out+24,net.json.value_length);put16(out+28,(uint16_t)strlen(net.status.url));
        put16(out+30,(uint16_t)strlen(net.status.content_type));
        put32(out+32,net.status.max_step_us);put32(out+36,net.status.max_slice_us);
        put32(out+40,net.status.verify);put32(out+44,net.via_hz);
        http_download_status_t saved;http_download_status(&saved);
        out[48]=saved.state;out[49]=saved.total_known?1:0;put16(out+50,(uint16_t)saved.result);
        put32(out+52,saved.written);put32(out+56,saved.total);
        put16(out+60,(uint16_t)strlen(http_download_path()));
        return NET_DESCRIPTOR_SIZE;
    }
    uint16_t max=get16(list+7);
    if(max<9||max>512||capacity<max)return 0;
    return read_source(selector,get16(list+5),max,out);
}
#endif
