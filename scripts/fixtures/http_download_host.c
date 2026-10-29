#include "net_service.h"
#include "http_download.h"
#include "ff.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {char path[192];uint8_t *data;size_t length;int exists,dir,open;} fs[40];
static unsigned creates,renames,unlinks,handles,tests,stat_calls;
static int readonly_sd,write_fail,short_write,sync_fail,close_fail,rename_fail,unlink_fail,mkdir_fail,rename_race;
static http_status_t backend;
static uint8_t ring[4096],owner;
static size_t ring_length;
static unsigned starts,cancels,handoffs;
static int occupied;
static uint32_t received,consumed;
static int same_path(const char *a,const char *b)
{while(*a&&*b){unsigned x=(unsigned char)*a++,y=(unsigned char)*b++;
 if(x>='A'&&x<='Z')x+=32;
 if(y>='A'&&y<='Z')y+=32;
 if(x!=y)return 0;}return *a==*b;}
static int find(const char *path)
{for(unsigned i=0;i<40;++i)if(fs[i].exists&&same_path(fs[i].path,path))return (int)i;return -1;}
static int make(const char *path,int dir)
{
    assert(find(path)<0);
    for(unsigned i=0;i<40;++i)if(!fs[i].exists){memset(&fs[i],0,sizeof(fs[i]));
        strcpy(fs[i].path,path);fs[i].exists=1;fs[i].dir=dir;return (int)i;}
    assert(!"fixture directory full");return -1;
}
FRESULT f_stat(const char *path,FILINFO *info)
{++stat_calls;int slot=find(path);if(slot<0)return FR_NO_FILE;info->fsize=fs[slot].length;info->fattrib=fs[slot].dir?AM_DIR:0;return FR_OK;}
FRESULT f_mount(FATFS *f,const char *path,int now){(void)f;(void)path;(void)now;return FR_OK;}
FRESULT f_mkdir(const char *path)
{if(readonly_sd||mkdir_fail)return FR_DENIED;if(find(path)>=0)return FR_EXIST;make(path,1);return FR_OK;}
FRESULT f_open(FIL *file,const char *path,int mode)
{
    assert((mode&(FA_WRITE|FA_CREATE_NEW))==(FA_WRITE|FA_CREATE_NEW));
    if(readonly_sd)return FR_WRITE_PROTECTED;
    if(find(path)>=0)return FR_EXIST;
    int slot=make(path,0);++creates;++handles;++fs[slot].open;
    file->slot=slot;file->position=file->size=0;file->mode=mode;return FR_OK;
}
FRESULT f_close(FIL *file)
{assert(handles&&fs[file->slot].open);if(close_fail)return FR_DISK_ERR;--handles;--fs[file->slot].open;return FR_OK;}
FRESULT f_write(FIL *file,const void *data,UINT length,UINT *wrote)
{
    assert(fs[file->slot].open);*wrote=0;if(write_fail)return FR_DISK_ERR;
    if(short_write&&length)--length;
    size_t end=file->position+length;uint8_t *next=realloc(fs[file->slot].data,end?end:1);assert(next);
    fs[file->slot].data=next;memcpy(next+file->position,data,length);
    fs[file->slot].length=file->size=file->position=end;*wrote=length;return FR_OK;
}
FRESULT f_sync(FIL *file){assert(fs[file->slot].open);return sync_fail?FR_DISK_ERR:FR_OK;}
FRESULT f_unlink(const char *path)
{int slot=find(path);++unlinks;if(unlink_fail)return FR_DISK_ERR;if(slot<0)return FR_NO_FILE;
 assert(!fs[slot].open);free(fs[slot].data);memset(&fs[slot],0,sizeof(fs[slot]));return FR_OK;}
FRESULT f_rename(const char *old,const char *next)
{
    int slot=find(old);assert(slot>=0&&!fs[slot].open);
    if(rename_fail)return FR_DISK_ERR;
    if(rename_race){make(next,0);rename_race=0;return FR_EXIST;}
    if(find(next)>=0)return FR_EXIST;
    strcpy(fs[slot].path,next);++renames;return FR_OK;
}
FRESULT f_read(FIL *file,void *data,UINT length,UINT *got)
{(void)file;(void)data;(void)length;*got=0;assert(!"manager must stream HTTP, not reread file");return FR_DISK_ERR;}

uint8_t http_service_getowner(void){return owner;}
int http_service_start_owned(const http_request_t *request,uint8_t who)
{
    if(occupied)return HTTP_E_BUSY;
    occupied=1;owner=who;++starts;ring_length=received=consumed=0;
    memset(&backend,0,sizeof(backend));backend.state=HTTP_S_PREPARE;backend.retained=request->retain;
    strcpy(backend.url,request->url);strcpy(backend.content_type,"application/octet-stream");return 0;
}
int http_service_start(const http_request_t *request){return http_service_start_owned(request,HTTP_OWNER_NONE);}
int http_service_cancel_owned(uint8_t who)
{if(!occupied||owner!=who)return HTTP_E_BUSY;++cancels;occupied=0;owner=HTTP_OWNER_NONE;
 backend.state=HTTP_S_FAILED;backend.result=HTTP_E_CANCELLED;ring_length=0;return 0;}
void http_service_cancel(void){(void)http_service_cancel_owned(HTTP_OWNER_NONE);}
int http_service_release_owned(uint8_t who)
{if(!occupied||owner!=who||backend.state!=HTTP_S_DONE||ring_length)return HTTP_E_BUSY;
 occupied=0;owner=HTTP_OWNER_NONE;return 0;}
size_t http_service_read_owned(uint8_t who,uint8_t *out,size_t max)
{
    if(!occupied||who!=owner)return 0;
    size_t n=ring_length;if(n>max)n=max;memcpy(out,ring,n);memmove(ring,ring+n,ring_length-n);
    ring_length-=n;consumed+=(uint32_t)n;return n;
}
size_t http_service_read(uint8_t *out,size_t max){return http_service_read_owned(HTTP_OWNER_NONE,out,max);}
const uint8_t *http_service_retained(size_t *length){*length=0;return ring;}
const uint8_t *http_service_retained_owned(uint8_t who,size_t *length)
{*length=0;return owner==who?ring:NULL;}
void http_service_status(http_status_t *out){*out=backend;out->buffered=(uint32_t)ring_length;out->body_bytes=received;}
int http_service_handoff_download(void)
{
    if(!occupied||owner!=HTTP_OWNER_NET||consumed||backend.retained||!backend.headers_ready||
       backend.status<200||backend.status>=300||backend.state==HTTP_S_FAILED)return HTTP_E_ARGUMENT;
    owner=HTTP_OWNER_DOWNLOAD;++handoffs;return 0;
}
static uint8_t packet[1012],reply[512],list[9]={3};
static unsigned descriptor(void)
{assert(net_service_status(0x82,list,reply,sizeof(reply))==64);return reply[48];}
static unsigned read_source(unsigned selector,unsigned offset)
{list[5]=(uint8_t)offset;list[6]=(uint8_t)(offset>>8);list[7]=0;list[8]=2;
 return net_service_status((uint8_t)selector,list,reply,sizeof(reply));}
static size_t request(unsigned flags,const char *hint)
{
    const char *url="https://example.test/path/source.bin?private=ignored";
    size_t u=strlen(url),h=hint?strlen(hint):0;
    memset(packet,0,sizeof(packet));packet[0]=1;packet[1]=NET_START;packet[2]=(uint8_t)flags;packet[3]=3;
    packet[4]=(uint8_t)u;packet[10]=(uint8_t)h;
    memcpy(packet+12,"GET",3);memcpy(packet+15,url,u);if(h)memcpy(packet+15+u,hint,h);
    return 15+u+h;
}
static void begin(unsigned flags,const char *hint,int attachment,int known,uint32_t total)
{
    size_t length=request(flags,hint);assert(!net_service_control(packet,length));net_service_poll();
    assert(starts==1);backend.state=HTTP_S_READ;backend.status=200;backend.headers_ready=1;
    backend.attachment=(uint8_t)attachment;backend.content_length_known=(uint8_t)known;backend.content_length=total;
    strcpy(backend.suggested_filename,"server.bin");net_service_poll();
}
static void feed(const uint8_t *data,size_t n)
{assert(n<=sizeof(ring)-ring_length);memcpy(ring+ring_length,data,n);ring_length+=n;received+=(uint32_t)n;}
static int completed(void)
{
    for(unsigned i=0;i<100&&descriptor()==HTTP_DL_WRITING;++i)net_service_poll();
    assert(descriptor()==HTTP_DL_DONE);assert(reply[8]==NET_DONE);
    unsigned length=reply[60]|((unsigned)reply[61]<<8);assert(length&&length<192);
    assert(read_source(0x87,0)==length+8&&reply[1]==NET_END);
    char path[192];memcpy(path,reply+8,length);path[length]=0;
    assert(!strncmp(path,"0:/downloads/",13));return find(path);
}
static void reset(void)
{
    net_service_reset();net_service_poll();
    for(unsigned i=0;i<40;++i){assert(!fs[i].open);free(fs[i].data);memset(&fs[i],0,sizeof(fs[i]));}
    memset(&backend,0,sizeof(backend));owner=0;occupied=0;ring_length=received=consumed=0;
    creates=renames=unlinks=handles=starts=cancels=handoffs=stat_calls=0;
    readonly_sd=write_fail=short_write=sync_fail=close_fail=rename_fail=unlink_fail=mkdir_fail=rename_race=0;
}
static void failure(int error)
{net_service_poll();assert(descriptor()==HTTP_DL_FAILED);assert((int16_t)(reply[50]|((unsigned)reply[51]<<8))==error);
 assert(reply[8]==NET_FAILED&&!handles&&(owner==HTTP_OWNER_NONE||(!handoffs&&owner==HTTP_OWNER_NET)));assert(!renames);++tests;}
int main(void)
{
    reset();begin(NET_FLAG_AUTO_DOWNLOAD,NULL,1,1,200003);assert(descriptor()==HTTP_DL_WRITING);
    assert(read_source(0x83,0)==8&&reply[1]==NET_SOURCE_FAILED&&!consumed);
    assert(net_service_control(packet,request(NET_FLAG_FORCE_DOWNLOAD,NULL))==0x2F);
    uint8_t block[3001];uint32_t at=0;
    while(at<200003){unsigned n=200003-at;if(n>sizeof(block))n=sizeof(block);
        for(unsigned i=0;i<n;++i)block[i]=(uint8_t)((at+i)*7U+((at+i)>>8));
        feed(block,n);net_service_poll();at+=n;}
    backend.state=HTTP_S_DONE;net_service_poll();int slot=completed();assert(slot>=0&&fs[slot].length==200003);
    for(unsigned i=0;i<200003;++i)assert(fs[slot].data[i]==(uint8_t)(i*7U+(i>>8)));
    assert(starts==1&&handoffs==1&&creates==1&&renames==1&&!handles);
    http_request_t external={"https://external.test/private","GET",NULL,NULL,0,0};assert(!http_service_start(&external));
    feed((const uint8_t*)"PRIVATE",7);unsigned old_cancels=cancels;net_service_poll();
    assert(read_source(0x83,0)==8&&reply[1]==NET_SOURCE_FAILED&&!consumed);
    net_service_reset();net_service_poll();assert(fs[slot].exists&&occupied&&cancels==old_cancels);++tests;

    reset();size_t length=request(NET_FLAG_AUTO_DOWNLOAD,NULL);assert(!net_service_control(packet,length));net_service_poll();
    feed((const uint8_t*)"ABC",3);assert(read_source(0x83,0)==8&&reply[1]==NET_MORE&&!consumed);
    backend.state=HTTP_S_READ;backend.status=302;backend.headers_ready=1;backend.attachment=1;net_service_poll();assert(!handoffs);
    backend.status=200;backend.attachment=0;net_service_poll();assert(!handoffs&&descriptor()==HTTP_DL_NONE);
    assert(read_source(0x83,0)==11&&!memcmp(reply+8,"ABC",3));assert(!creates);++tests;

    reset();begin(0,NULL,1,1,1);feed((const uint8_t*)"X",1);assert(read_source(0x83,0)==9);assert(!creates&&!handoffs);++tests;
    reset();length=request(NET_FLAG_FORCE_DOWNLOAD,NULL);assert(!net_service_control(packet,length));net_service_poll();
    backend.state=HTTP_S_DONE;backend.status=404;backend.headers_ready=1;backend.attachment=1;
    failure(HTTP_DL_E_HTTP);assert(!creates&&!handoffs);
    reset();length=request(NET_FLAG_AUTO_DOWNLOAD,NULL);assert(!net_service_control(packet,length));net_service_poll();
    backend.state=HTTP_S_DONE;backend.status=404;backend.headers_ready=1;backend.attachment=1;feed((const uint8_t*)"ERR",3);net_service_poll();
    assert(descriptor()==HTTP_DL_NONE&&reply[8]==NET_DONE&&!creates&&!http_download_active());
    assert(read_source(0x83,0)==11&&reply[1]==NET_END&&!memcmp(reply+8,"ERR",3));++tests;
    reset();length=request(NET_FLAG_AUTO_DOWNLOAD,NULL);assert(!net_service_control(packet,length));net_service_poll();
    backend.state=HTTP_S_DONE;backend.status=302;backend.headers_ready=1;backend.attachment=1;feed((const uint8_t*)"ERR",3);net_service_poll();
    assert(descriptor()==HTTP_DL_NONE&&reply[8]==NET_DONE&&!creates&&!http_download_active());
    assert(read_source(0x83,0)==11&&reply[1]==NET_END);++tests;
    reset();begin(NET_FLAG_FORCE_DOWNLOAD,"../../wanted.zip",0,0,0);feed((const uint8_t*)"ZIP",3);net_service_poll();backend.state=HTTP_S_DONE;net_service_poll();slot=completed();assert(slot>=0&&!strcmp(fs[slot].path,"0:/downloads/wanted.zip"));++tests;
    reset();make("0:/downloads",0);begin(NET_FLAG_FORCE_DOWNLOAD,NULL,0,1,0);failure(HTTP_DL_E_STORAGE);
    reset();readonly_sd=1;begin(NET_FLAG_FORCE_DOWNLOAD,NULL,0,1,0);failure(HTTP_DL_E_STORAGE);
    reset();mkdir_fail=1;begin(NET_FLAG_FORCE_DOWNLOAD,NULL,0,1,0);failure(HTTP_DL_E_STORAGE);
    reset();begin(NET_FLAG_FORCE_DOWNLOAD,NULL,0,1,3);write_fail=1;feed((const uint8_t*)"ABC",3);failure(HTTP_DL_E_WRITE);assert(unlinks==1);
    reset();begin(NET_FLAG_FORCE_DOWNLOAD,NULL,0,1,3);short_write=1;feed((const uint8_t*)"ABC",3);failure(HTTP_DL_E_WRITE);assert(unlinks==1);
    reset();begin(NET_FLAG_FORCE_DOWNLOAD,NULL,0,1,0);sync_fail=1;backend.state=HTTP_S_DONE;failure(HTTP_DL_E_SYNC);
    reset();begin(NET_FLAG_FORCE_DOWNLOAD,NULL,0,1,0);close_fail=1;backend.state=HTTP_S_DONE;net_service_poll();
    assert(descriptor()==HTTP_DL_FAILED&&handles==1&&http_download_active());
    assert(http_download_quiesce()==HTTP_DL_E_CLEANUP&&handles==1&&!unlinks);
    close_fail=0;assert(!http_download_quiesce()&&!handles&&unlinks==1);++tests;
    reset();begin(NET_FLAG_FORCE_DOWNLOAD,NULL,0,1,0);rename_fail=1;backend.state=HTTP_S_DONE;failure(HTTP_DL_E_RENAME);
    reset();begin(NET_FLAG_FORCE_DOWNLOAD,NULL,0,1,3);backend.state=HTTP_S_DONE;failure(HTTP_DL_E_LENGTH);
    reset();begin(NET_FLAG_FORCE_DOWNLOAD,NULL,0,1,1);feed((const uint8_t*)"AB",2);failure(HTTP_DL_E_LENGTH);
    reset();begin(NET_FLAG_FORCE_DOWNLOAD,NULL,0,0,0);backend.state=HTTP_S_FAILED;backend.result=HTTP_E_TIMEOUT;failure(HTTP_E_TIMEOUT);
    reset();begin(NET_FLAG_FORCE_DOWNLOAD,NULL,0,1,0);rename_race=1;backend.state=HTTP_S_DONE;net_service_poll();slot=completed();assert(slot>=0&&!strcmp(fs[slot].path,"0:/downloads/server-1.bin")&&find("0:/downloads/server.bin")>=0);++tests;
    reset();make("0:/downloads/server.bin",0);make("0:/downloads/server-1.bin.part",0);begin(NET_FLAG_FORCE_DOWNLOAD,NULL,0,1,0);backend.state=HTTP_S_DONE;net_service_poll();slot=completed();assert(!strcmp(fs[slot].path,"0:/downloads/server-2.bin"));++tests;
    reset();make("0:/downloads/SERVER.BIN",0);begin(NET_FLAG_FORCE_DOWNLOAD,NULL,0,1,0);backend.state=HTTP_S_DONE;net_service_poll();slot=completed();assert(!strcmp(fs[slot].path,"0:/downloads/server-1.bin"));++tests;
    reset();for(unsigned i=0;i<20;++i){char path[80];if(i)snprintf(path,sizeof(path),"0:/downloads/server-%u.bin",i);else strcpy(path,"0:/downloads/server.bin");make(path,0);}
    begin(NET_FLAG_FORCE_DOWNLOAD,NULL,0,1,0);backend.state=HTTP_S_DONE;
    for(unsigned i=0;i<30&&descriptor()==HTTP_DL_WRITING;++i){unsigned before_stat=stat_calls;net_service_poll();assert(stat_calls-before_stat<=1);}
    slot=completed();assert(!strcmp(fs[slot].path,"0:/downloads/server-20.bin")&&creates==1);++tests;
    reset();begin(NET_FLAG_FORCE_DOWNLOAD,NULL,0,0,0);memset(packet,0,12);packet[0]=1;packet[1]=NET_CANCEL;
    assert(!net_service_control(packet,12));assert(descriptor()==HTTP_DL_CANCELLED&&!handles&&unlinks==1);net_service_poll();++tests;
    reset();begin(NET_FLAG_FORCE_DOWNLOAD,NULL,0,0,0);memset(packet,0,12);packet[0]=1;packet[1]=NET_CANCEL;
    assert(!net_service_control(packet,12));assert(!handles&&unlinks==1);
    length=request(NET_FLAG_FORCE_DOWNLOAD,"replacement.bin");assert(!net_service_control(packet,length));net_service_poll();
    assert(starts==2);backend.state=HTTP_S_DONE;backend.status=200;backend.headers_ready=1;
    net_service_poll();net_service_poll();slot=completed();assert(!strcmp(fs[slot].path,"0:/downloads/replacement.bin"));++tests;
    reset();begin(NET_FLAG_FORCE_DOWNLOAD,NULL,0,0,0);net_service_reset();assert(!handles&&unlinks==1);net_service_poll();assert(descriptor()==HTTP_DL_NONE);++tests;
    reset();begin(NET_FLAG_FORCE_DOWNLOAD,NULL,0,0,0);unlink_fail=1;memset(packet,0,12);packet[0]=1;packet[1]=NET_CANCEL;assert(!net_service_control(packet,12));assert(descriptor()==HTTP_DL_FAILED);unlink_fail=0;++tests;
    reset();http_request_t other={"https://other.test/","GET",NULL,NULL,0,0};assert(!http_service_start(&other));unsigned before=cancels;
    length=request(0,NULL);assert(!net_service_control(packet,length));net_service_poll();assert(descriptor()==HTTP_DL_NONE&&reply[8]==NET_FAILED&&cancels==before&&occupied);++tests;
    reset();for(unsigned flags=0;flags<16;++flags){net_service_reset();net_service_poll();length=request(flags,NULL);int valid=!(flags&~7U)&&!((flags&1U)&&(flags&6U));assert(net_service_control(packet,length)==(valid?0:0x21));if(valid)net_service_poll();}
    reset();length=request(NET_FLAG_AUTO_DOWNLOAD,"hint.bin");assert(net_service_control(packet,length)==0x21);
    length=request(NET_FLAG_FORCE_DOWNLOAD,NULL);memcpy(packet+12,"PUT",3);assert(net_service_control(packet,length)==0x21);
    length=request(NET_FLAG_AUTO_DOWNLOAD,NULL);memcpy(packet+12,"PUT",3);assert(net_service_control(packet,length)==0x21);
    length=request(NET_FLAG_FORCE_DOWNLOAD,"BAD");packet[length-1]=0;assert(net_service_control(packet,length)==0x21);++tests;
    reset();printf("PASS %u download lifecycle groups: same-response200003B, ownership, no MIME auto, redirects, safe names, races, file faults, cancel/reset, ABI validation\n",tests);return 0;
}
