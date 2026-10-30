/* Native body-pipeline benchmark. Production HTTP parser/ring/poll, updater,
 * SHA256 and Bootgen validation run; NIC, TLS connection and SD are modeled.
 * Clock advances 1ms per foreground round, not according to native wall time. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define COMMON_H
static uint32_t slot_mask=2;
static void test_reg_write(uint32_t address,uint32_t value);
#define REG_READ(address) ((void)(address),slot_mask)
#define REG_WRITE(address,value) test_reg_write(address,value)
#ifndef PERF_HTTP_SOURCE
#define PERF_HTTP_SOURCE "../../ps_sources/frontend/http_service.c"
#endif
#include PERF_HTTP_SOURCE
#include "online_update_service.h"

uint64_t host_time;
static unsigned char *metadata,*payload,*wire,*sd;
static size_t metadata_n,payload_n,wire_n,wire_at,sd_n;
static uint16_t rx_pointer;
static unsigned writes,write_min=UINT32_MAX,write_max,write_tail,rx_calls,rx_max,poll_rx_max;
static unsigned rounds,asset_rounds,starts,open_count,unlinks,renames,layout,layout_restores;
static unsigned write_hist_full,write_hist_partial;
static int part_exists,final_exists,sync_fail,close_fail,scenario;
static int layout_fail,enforce_layout_closed,enforce_guest_layout;
static unsigned rx_cost_us;
static uint8_t registers[4096];
static void test_reg_write(uint32_t address,uint32_t value)
{(void)address;if(enforce_guest_layout&&(value&2U))assert(!layout);slot_mask=value;}

void uart_puts(uint32_t uart,const char *text){(void)uart;(void)text;}
uint8_t ftp_sd_service_active(void){return 0;}
int usb_sdd_service_active(void){return 0;}
void net_service_reset(void){}
uint32_t net_service_json_max_us(void){return 0;}
int http_entropy_seed(uint8_t out[HTTP_SEED_BYTES]){(void)out;return -1;}
void http_entropy_status(http_entropy_stats_t *out){memset(out,0,sizeof *out);}
const char *http_entropy_result_name(int result){(void)result;return "fixture";}
uint16_t uthernet2_w5100_socket_reg(uint8_t socket,uint16_t offset){return (uint16_t)(0x400+socket*256+offset);}
int uthernet2_read_reg(uint16_t address,uint8_t *out){assert(address<sizeof registers);*out=registers[address];return 0;}
int uthernet2_write_reg(uint16_t address,uint8_t value)
{assert(address<sizeof registers);if(enforce_layout_closed&&address==0x500+W5100_SN_MR&&value==UDP_MODE)assert(!layout);registers[address]=value;return 0;}
int uthernet2_w5100_socket_command(uint8_t socket,uint8_t command)
{if(command==W5100_CR_CLOSE)registers[0x400+socket*256+W5100_SN_SR]=W5100_SR_CLOSED;
 if(command==W5100_CR_OPEN)registers[0x400+socket*256+W5100_SN_SR]=W5100_SR_INIT;return 0;}
int uthernet2_w5100_socket_status(uint8_t socket,uint8_t *out){*out=registers[0x400+socket*256+W5100_SN_SR];return 0;}
int uthernet2_w5100_read16_stable(uint16_t address,uint16_t *out)
{
    assert((address&255)==W5100_SN_RX_RSR);size_t n=wire_n-wire_at;
    size_t maximum=layout?8192:4096;if(n>maximum)n=maximum;*out=(uint16_t)n;return 0;
}
int uthernet2_w5100_read16(uint16_t address,uint16_t *out)
{assert((address&255)==W5100_SN_RX_RD);*out=rx_pointer;return 0;}
int uthernet2_w5100_write16(uint16_t address,uint16_t value)
{if((address&255)==W5100_SN_RX_RD){wire_at+=(uint16_t)(value-rx_pointer);rx_pointer=value;assert(wire_at<=wire_n);}return 0;}
int uthernet2_read_block(uint16_t address,uint8_t *out,uint16_t n){(void)address;memset(out,0,n);return 0;}
int uthernet2_write_block(uint16_t address,const uint8_t *data,uint16_t n){(void)address;(void)data;(void)n;return 0;}
int uthernet2_w5100_socket_ring_read(uint8_t socket,uint16_t pointer,uint8_t *out,uint16_t n)
{
    assert(!socket&&pointer==rx_pointer&&n<=wire_n-wire_at);memcpy(out,wire+wire_at,n);
    host_time+=rx_cost_us;++rx_calls;if(n>rx_max)rx_max=n;return 0;
}
int uthernet2_w5100_socket_ring_write(uint8_t socket,uint16_t pointer,const uint8_t *data,uint16_t n)
{(void)socket;(void)pointer;(void)data;(void)n;return 0;}
int uthernet2_read_network_config(uthernet2_network_config_t *out){memset(out,0,sizeof *out);return 0;}
int uthernet2_test(uthernet2_test_result_t *out){memset(out,0,sizeof *out);return 0;}
/* The layout helper itself is exercised separately against actual controller
 * C and an MMIO model. Here it changes the NIC's exposed RX window only. */
int uthernet2_w5100_set_rx_layout(uint8_t update_only)
{
    if(enforce_layout_closed)for(unsigned i=0;i<4;++i)assert(registers[0x400+256*i+W5100_SN_SR]==W5100_SR_CLOSED);
    if(!update_only&&layout_fail)return -1;
    layout=!!update_only;if(!layout)++layout_restores;return 0;
}

FRESULT f_stat(const char *path,FILINFO *info)
{
    memset(info,0,sizeof *info);
    if(strstr(path,".part"))return part_exists?FR_OK:FR_NO_FILE;
    if(!strcmp(path,"0:/FIRMWARE.BIN"))return final_exists?FR_OK:FR_NO_FILE;
    return FR_NO_FILE;
}
FRESULT f_mount(FATFS *fs,const char *path,int now){(void)fs;(void)path;(void)now;assert(!"must not remount");return FR_DISK_ERR;}
FRESULT f_open(FIL *file,const char *path,int mode)
{assert(!part_exists&&strstr(path,".part")&&(mode&FA_CREATE_NEW));part_exists=1;++open_count;memset(file,0,sizeof *file);return FR_OK;}
FRESULT f_write(FIL *file,const void *data,UINT n,UINT *got)
{
    assert(open_count&&part_exists);++writes;if(n<write_min)write_min=n;if(n>write_max)write_max=n;
    write_tail=n;if(n==128U*1024U)++write_hist_full;else ++write_hist_partial;
    size_t end=file->position+n;unsigned char *p=realloc(sd,end?end:1);assert(p);sd=p;
    memcpy(sd+file->position,data,n);file->position=end;file->size=sd_n=end;*got=n;return FR_OK;
}
FRESULT f_read(FIL *file,void *out,UINT n,UINT *got)
{assert(open_count&&file->position<=sd_n);if(n>sd_n-file->position)n=(UINT)(sd_n-file->position);
 memcpy(out,sd+file->position,n);file->position+=n;*got=n;return FR_OK;}
FRESULT f_lseek(FIL *file,size_t offset){if(offset>sd_n)return FR_DISK_ERR;file->position=offset;return FR_OK;}
FRESULT f_sync(FIL *file){(void)file;assert(open_count);return sync_fail?FR_DISK_ERR:FR_OK;}
FRESULT f_close(FIL *file){(void)file;assert(open_count);if(close_fail)return FR_DISK_ERR;--open_count;return FR_OK;}
FRESULT f_unlink(const char *path){assert(strstr(path,".part")&&!open_count);++unlinks;part_exists=0;return FR_OK;}
FRESULT f_rename(const char *old,const char *next){assert(strstr(old,".part")&&!strcmp(next,"0:/FIRMWARE.BIN")&&!open_count&&!final_exists);part_exists=0;final_exists=1;++renames;return FR_OK;}
FRESULT f_mkdir(const char *path){(void)path;assert(!"no directories");return FR_DENIED;}

static void load_file(const char *path,unsigned char **out,size_t *length)
{FILE *f=fopen(path,"rb");assert(f);assert(!fseek(f,0,SEEK_END));long n=ftell(f);assert(n>=0&&!fseek(f,0,SEEK_SET));
 *out=malloc((size_t)n);assert(*out);*length=(size_t)n;assert(fread(*out,1,*length,f)==*length);fclose(f);}
static void connection_ready(void)
{
    if(g_http.status.state!=HTTP_S_PREPARE)return;
    assert(g_http.owner==HTTP_OWNER_UPDATE);++starts;
    int api=!strcmp(g_http.status.url,ONLINE_UPDATE_API_URL);
    const unsigned char *data=api?metadata:payload;size_t n=api?metadata_n:payload_n;
    char headers[160];int h=snprintf(headers,sizeof headers,"HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n\r\n",n);assert(h>0);
    free(wire);wire=malloc((size_t)h+n);assert(wire);memcpy(wire,headers,(size_t)h);memcpy(wire+h,data,n);
    wire_n=(size_t)h+n;wire_at=rx_pointer=0;
    /* This benchmark begins at authenticated response RX. Connection/TLS and
     * layout transition/restoration have independent native tests. */
    g_http.url.tls=0;g_http.status.verify=0;g_http.status.state=HTTP_S_READ;
    http_parser_init(&g_http.parser,0,api?HTTP_RETAIN_MAX:UINT32_MAX);
    registers[0x400+W5100_SN_SR]=W5100_SR_ESTABLISHED;
#ifndef PERF_BASELINE
    if(!api)assert(!uthernet2_w5100_set_rx_layout(1));
#endif
}
int main(int argc,char **argv)
{
    assert(argc==4);scenario=atoi(argv[3]);load_file(argv[1],&metadata,&metadata_n);load_file(argv[2],&payload,&payload_n);
    memset(&g_http,0,sizeof g_http);mbedtls_ssl_init(&g_http.ssl);host_time=1000000;
    online_update_init("F1.3.3","B2.0.6");online_update_poll(1,0);
    for(unsigned i=0;i<20000&&online_update_active();++i){connection_ready();host_time+=1000;http_service_poll();online_update_poll(1,0);}
    online_update_snapshot_t status;online_update_status(&status);assert(status.available_mask==1&&status.state==ONLINE_UPDATE_AVAILABLE);
    assert(!online_update_start(ONLINE_UPDATE_FIRMWARE));
    int cancelled=0,retried=0;
    while(online_update_active()&&rounds<100000U) {
        connection_ready();host_time+=1000;++rounds;
        unsigned before=rx_calls;http_service_poll();if(rx_calls-before>poll_rx_max)poll_rx_max=rx_calls-before;
        if(http_service_getowner()==HTTP_OWNER_UPDATE)++asset_rounds;
        online_update_poll(1,0);online_update_status(&status);
        if(scenario==1&&!cancelled&&status.received>32768U){online_update_cancel();cancelled=1;}
        if(scenario==2&&status.state==ONLINE_UPDATE_VERIFYING)sync_fail=1;
        if(scenario==3&&status.state==ONLINE_UPDATE_VERIFYING)close_fail=1;
        if(scenario==3&&status.state==ONLINE_UPDATE_FAILED&&online_update_active()){assert(open_count);close_fail=0;retried=1;}
    }
    assert(!online_update_active()&&!open_count&&http_service_getowner()==HTTP_OWNER_NONE);
    if(!scenario){assert(status.state==ONLINE_UPDATE_READY&&final_exists&&renames==1&&sd_n==payload_n&&!memcmp(sd,payload,payload_n));}
    else {assert(!final_exists&&!part_exists&&unlinks==1);if(scenario==1)assert(status.state==ONLINE_UPDATE_CANCELLED);else assert(status.state==ONLINE_UPDATE_FAILED);}
#ifndef PERF_BASELINE
    if(!scenario){assert(write_hist_full==payload_n/(128U*1024U));assert(write_hist_partial==(payload_n%(128U*1024U)?1U:0U));}
    if(scenario==1)assert(!writes); /* Cancel buffered bytes before the first flush. */
#endif
    printf("{\"scenario\":%d,\"bytes\":%zu,\"rounds\":%u,\"asset_rounds\":%u,\"rx_calls\":%u,\"rx_max\":%u,\"rx_calls_per_poll_max\":%u,\"writes\":%u,\"write_min\":%u,\"write_max\":%u,\"write_tail\":%u,\"full128k_writes\":%u,\"partial_writes\":%u,\"cancelled\":%d,\"cleanup_retried\":%d,\"model_tick_us\":1000,\"board_test\":false}\n",
      scenario,payload_n,rounds,asset_rounds,rx_calls,rx_max,poll_rx_max,writes,writes?write_min:0,write_max,write_tail,write_hist_full,write_hist_partial,cancelled,retried);
    return 0;
}
