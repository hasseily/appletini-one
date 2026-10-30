/* Production updater with deterministic HTTP/FatFs adapters. No flash adapter
 * exists in this executable: every state, including READY, is SD-only. */
#include "online_update_service.h"
#include "http_service.h"
#include "ff.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char *path; unsigned char *data; size_t length; } input_t;
static input_t documents[3];
static struct { char path[96]; unsigned char *data; size_t size; int used,open; } fs[12];
static unsigned handles,creates,renames,unlinks,writes,reads,starts,releases,cancels,polls;
static int readonly_sd,write_error,short_write,sync_error,close_error,read_error,rename_error,unlink_error,rename_race;
static unsigned byte_limit,fail_after;
static const char *scenario;
static http_status_t status_http;
static uint8_t owner;
static int occupied,which;
static size_t delivered,consumed;
static unsigned char retained[ONLINE_UPDATE_JSON_BYTES];
static unsigned char ring[4096];
static size_t ring_bytes;
/* Diagnostic fault controls are unused by the ordinary lifecycle cases. */
static int inject_source=-1,inject_start_rc,inject_transport_rc,poison_cancel_status,inject_old_request;
static unsigned inject_http_status,inject_verify;
static unsigned built_request_mask,old_header_rejections;
static int cancel_block;
static int is(const char *name) { return !strcmp(scenario,name); }
static int same(const char *a,const char *b)
{
    while (*a&&*b) {
        unsigned x=(unsigned char)*a++,y=(unsigned char)*b++;
        if (x>='a'&&x<='z') x-=32;
        if (y>='a'&&y<='z') y-=32;
        if (x!=y) return 0;
    }
    return *a==*b;
}
static int find(const char *path)
{ for (unsigned i=0;i<12;++i) if (fs[i].used&&same(fs[i].path,path)) return (int)i; return -1; }
static int make(const char *path,const void *bytes,size_t n)
{
    assert(find(path)<0);
    for (unsigned i=0;i<12;++i) if (!fs[i].used) {
        memset(&fs[i],0,sizeof fs[i]);fs[i].used=1;strcpy(fs[i].path,path);
        fs[i].data=malloc(n?n:1);assert(fs[i].data);if(n)memcpy(fs[i].data,bytes,n);fs[i].size=n;return (int)i;
    }
    assert(!"fixture filesystem full");return -1;
}
FRESULT f_stat(const char *path,FILINFO *out)
{
    memset(out,0,sizeof *out);
    if (!strcmp(path,"0:")||!strcmp(path,"0:/")) { out->fattrib=AM_DIR;return FR_OK; }
    int slot=find(path);if(slot<0)return FR_NO_FILE;out->fsize=fs[slot].size;return FR_OK;
}
FRESULT f_mount(FATFS *volume,const char *path,int now)
{ (void)volume;(void)path;(void)now;assert(!"must not remount an already ready volume");return FR_DISK_ERR; }
FRESULT f_open(FIL *file,const char *path,int mode)
{
    int slot=find(path);
    if (mode&FA_WRITE) {
        assert((mode&FA_CREATE_NEW)&&!(mode&FA_CREATE_ALWAYS));
        assert(strstr(path,".part")!=NULL);
        if(readonly_sd)return FR_WRITE_PROTECTED;
        if(slot>=0)return FR_EXIST;
        slot=make(path,NULL,0);++creates;
    } else if(slot<0)return FR_NO_FILE;
    assert(!fs[slot].open);++fs[slot].open;++handles;
    file->slot=slot;file->position=0;file->size=fs[slot].size;file->mode=mode;return FR_OK;
}
FRESULT f_close(FIL *file)
{
    assert(handles&&fs[file->slot].open);
    if(close_error)return FR_DISK_ERR;
    --fs[file->slot].open;--handles;return FR_OK;
}
FRESULT f_write(FIL *file,const void *data,UINT n,UINT *got)
{
    ++writes;*got=0;assert(file->mode&FA_WRITE);assert(fs[file->slot].open);
    if(write_error)return FR_DISK_ERR;
    if(short_write&&n)--n;
    size_t end=file->position+n;
    unsigned char *p=realloc(fs[file->slot].data,end?end:1);assert(p);
    fs[file->slot].data=p;memcpy(p+file->position,data,n);
    if(end>fs[file->slot].size)fs[file->slot].size=end;
    file->size=fs[file->slot].size;file->position=end;*got=n;return FR_OK;
}
FRESULT f_read(FIL *file,void *data,UINT n,UINT *got)
{
    ++reads;*got=0;assert(fs[file->slot].open);
    if(read_error)return FR_DISK_ERR;
    size_t available=fs[file->slot].size-file->position;if(n>available)n=(UINT)available;
    memcpy(data,fs[file->slot].data+file->position,n);file->position+=n;*got=n;return FR_OK;
}
FRESULT f_lseek(FIL *file,size_t offset)
{ assert(fs[file->slot].open);if(offset>fs[file->slot].size)return FR_DISK_ERR;file->position=offset;return FR_OK; }
FRESULT f_sync(FIL *file)
{ assert(fs[file->slot].open);return sync_error?FR_DISK_ERR:FR_OK; }
FRESULT f_unlink(const char *path)
{
    ++unlinks;
    assert(strstr(path,".part")!=NULL); /* Old root firmware/Boot is never removed. */
    if(unlink_error)return FR_DISK_ERR;
    int slot=find(path);if(slot<0)return FR_NO_FILE;assert(!fs[slot].open);
    free(fs[slot].data);memset(&fs[slot],0,sizeof fs[slot]);return FR_OK;
}
FRESULT f_rename(const char *old,const char *next)
{
    int slot=find(old);assert(slot>=0&&!fs[slot].open);
    assert(strstr(old,".part")&&!strstr(next,".part"));
    if(rename_error)return FR_DISK_ERR;
    if(rename_race&&find(next)<0) { make(next,"RACE",4);rename_race=0;return FR_EXIST; }
    if(find(next)>=0)return FR_EXIST;
    strcpy(fs[slot].path,next);++renames;return FR_OK;
}
FRESULT f_mkdir(const char *path) { (void)path;assert(!"updater writes only SD root");return FR_DENIED; }

uint8_t http_service_getowner(void) { return owner; }
static unsigned occurrences(const char *text,const char *needle)
{ unsigned count=0;while((text=strstr(text,needle))!=NULL){++count;text+=strlen(needle);}return count; }
static int build_actual_request(const http_request_t *request)
{
    http_url_t url;char wire[4096],old_headers[2048];
    assert(!http_url_parse(request->url,&url));
    int n=http_request_build(&url,request->method,request->headers,request->body_length,0,wire,sizeof wire);
    assert(n>0&&(size_t)n<sizeof wire);
    assert(occurrences(wire,"\r\nAccept-Encoding: identity\r\n")==1);
    assert(occurrences(wire,"\r\nUser-Agent:")==1&&strstr(wire,"User-Agent: Appletini-Updater\r\n"));
    if(which==0)assert(strstr(wire,"Accept: application/vnd.github+json\r\n")&&strstr(wire,"X-GitHub-Api-Version: 2022-11-28\r\n"));
    else assert(strstr(wire,"Accept: application/octet-stream\r\n"));
    built_request_mask|=1U<<which;
    /* Exact old producer bug: append the formerly supplied forbidden header
     * to the current producer's actual header block, without mirroring it. */
    n=snprintf(old_headers,sizeof old_headers,"%sAccept-Encoding: identity\r\n",request->headers);
    assert(n>0&&(size_t)n<sizeof old_headers);
    int old=http_request_build(&url,request->method,old_headers,request->body_length,0,wire,sizeof wire);
    assert(old==HTTP_FORMAT);++old_header_rejections;
    return inject_old_request&&which==inject_source?old:0;
}
int http_service_start_owned(const http_request_t *request,uint8_t who)
{
    if(occupied)return HTTP_E_BUSY;
    assert(who==HTTP_OWNER_UPDATE);assert(!strcmp(request->method,"GET"));assert(!request->body_length);
    ++starts;
    if(!strcmp(request->url,ONLINE_UPDATE_API_URL))which=0;
    else if(strstr(request->url,"/FIRMWARE.BIN"))which=1;
    else if(strstr(request->url,"/BOOT.BIN"))which=2;
    else assert(!"unexpected updater URL");
    int framed=build_actual_request(request);if(framed)return framed;
    if(which==inject_source&&inject_start_rc)return inject_start_rc;
    occupied=1;owner=who;ring_bytes=delivered=consumed=0;
    memset(&status_http,0,sizeof status_http);status_http.state=HTTP_S_READ;
    status_http.retained=request->retain;status_http.status=200;status_http.headers_ready=1;
    strcpy(status_http.url,request->url);
    if(which==inject_source&&inject_http_status)status_http.status=(uint16_t)inject_http_status;
    assert((which==0)==!!request->retain);
    status_http.content_length_known=1;
    status_http.content_length=(uint32_t)documents[which].length;
    if((is("boot-extra-chunked")&&which==2)||(is("image-extra-chunked")&&which==1))status_http.content_length_known=0;
    if(is("http404")&&which==1)status_http.status=404;
    if(is("redirect-assets")&&which==1)strcpy(status_http.url,"https://release-assets.githubusercontent.com/github-production-release-asset/checked.bin");
    if(is("redirect-objects")&&which==1)strcpy(status_http.url,"https://objects.githubusercontent.com/github-production-release-asset/checked.bin");
    if(is("redirect-evil")&&which==1)strcpy(status_http.url,"https://github.com.evil.test/checked.bin");
    if(is("redirect-http")&&which==1)strcpy(status_http.url,"http://github.com/checked.bin");
    if(is("redirect-port")&&which==1)strcpy(status_http.url,"https://github.com:444/checked.bin");
    if(is("redirect-api-evil")&&which==0)strcpy(status_http.url,"https://api.github.com.evil.test/latest");
    return 0;
}
int http_service_cancel_owned(uint8_t who)
{
    if(!occupied||owner!=who)return HTTP_E_BUSY;
    if(cancel_block){status_http.ethernet_restore_pending=1;return HTTP_E_IO;}
    ++cancels;occupied=0;owner=HTTP_OWNER_NONE;ring_bytes=0;
    status_http.state=HTTP_S_FAILED;status_http.result=HTTP_E_CANCELLED;
    if(poison_cancel_status) {status_http.status=599;status_http.verify=0xdeadbeefU;}
    return 0;
}
int http_service_release_owned(uint8_t who)
{
    if(!occupied||owner!=who||status_http.state!=HTTP_S_DONE||ring_bytes)return HTTP_E_BUSY;
    ++releases;occupied=0;owner=HTTP_OWNER_NONE;return 0;
}
void http_service_status(http_status_t *out)
{ *out=status_http;out->buffered=(uint32_t)ring_bytes;out->body_bytes=(uint32_t)delivered; }
size_t http_service_read_owned(uint8_t who,uint8_t *out,size_t capacity)
{
    if(!occupied||owner!=who)return 0;
    assert(which!=0);size_t n=ring_bytes;if(n>capacity)n=capacity;
    memcpy(out,ring,n);memmove(ring,ring+n,ring_bytes-n);ring_bytes-=n;consumed+=n;return n;
}
const uint8_t *http_service_retained_owned(uint8_t who,size_t *length)
{
    *length=0;if(!occupied||owner!=who||!status_http.retained)return NULL;
    assert(which==0);*length=delivered;return retained;
}
static void pump(void)
{
    if(!occupied||status_http.state==HTTP_S_FAILED||status_http.state==HTTP_S_DONE)return;
    if(which==inject_source&&inject_transport_rc) {
        status_http.state=HTTP_S_FAILED;status_http.result=inject_transport_rc;
        status_http.status=0;status_http.verify=inject_verify;return;
    }
    if(is("http-fail")&&which==1&&delivered>=fail_after) {
        status_http.state=HTTP_S_FAILED;status_http.result=HTTP_E_TIMEOUT;return;
    }
    size_t n=documents[which].length-delivered;
    if(which==1&&byte_limit&&delivered>=byte_limit)n=0;
    if(n>337)n=337; /* Many nonaligned reads, unrelated to the disk write block. */
    if(((is("boot-extra-chunked")&&which==2)||(is("image-extra-chunked")&&which==1))&&
       delivered<documents[which].length-13&&n>documents[which].length-13-delivered)
        n=documents[which].length-13-delivered;
    if(which==1&&byte_limit&&n>byte_limit-delivered)n=byte_limit-delivered;
    if(which==0) {
        assert(delivered+n<=sizeof retained);
        memcpy(retained+delivered,documents[0].data+delivered,n);
    } else {
        if(n>sizeof ring-ring_bytes)n=sizeof ring-ring_bytes;
        memcpy(ring+ring_bytes,documents[which].data+delivered,n);ring_bytes+=n;
    }
    delivered+=n;
    if(delivered==documents[which].length || (which==1&&byte_limit&&delivered>=byte_limit))status_http.state=HTTP_S_DONE;
}
static online_update_snapshot_t snapshot(void)
{ online_update_snapshot_t out;online_update_status(&out);return out; }
static void step(uint8_t allowed,uint8_t busy)
{ ++polls;pump();online_update_poll(allowed,busy); }
static void terminal(void)
{
    for(unsigned i=0;i<20000&&online_update_active();++i)step(1,0);
    assert(!online_update_active());assert(!handles);assert(owner==HTTP_OWNER_NONE);
}
static void load(input_t *in,const char *path)
{
    FILE *file=fopen(path,"rb");assert(file);assert(!fseek(file,0,SEEK_END));long n=ftell(file);assert(n>=0);
    assert(!fseek(file,0,SEEK_SET));in->data=malloc((size_t)n+1);assert(in->data);
    in->length=(size_t)n;assert(fread(in->data,1,in->length,file)==in->length);fclose(file);in->path=path;
}
static void expect_old(const char *path,const char *text)
{ int slot=find(path);assert(slot>=0&&fs[slot].size==strlen(text)&&!memcmp(fs[slot].data,text,strlen(text))); }
int main(int argc,char **argv)
{
    assert(argc==5);scenario=argv[1];load(&documents[0],argv[2]);load(&documents[1],argv[3]);load(&documents[2],argv[4]);
    const char *installed_f=is("current")||is("firmware-current")?"F1.3.4":is("downgrade")?"F9.0.0":"F1.3.3";
    const char *installed_b=is("current")||is("boot-current")?"B2.0.6":is("downgrade")?"B9.0.0":"B2.0.5";
    online_update_init(installed_f,installed_b);
    if(is("http-owner-busy")) {
        occupied=1;owner=HTTP_OWNER_NET;status_http.state=HTTP_S_READ;
        online_update_poll(1,0);
        assert(owner==HTTP_OWNER_NET&&!cancels&&!starts);printf("PASS owner-busy %d\n",snapshot().result);return 0;
    }
    step(0,1);assert(!starts&&!online_update_active());
    if(is("cancel-before-start")) {
        online_update_cancel();step(1,0);assert(!starts&&!online_update_active());
        puts("PASS cancel-before-auto-check");return 0;
    }
    step(1,0);
    if(is("api-owner-change")) {
        for(unsigned i=0;i<100&&!snapshot().total;++i)step(1,0);
        assert(snapshot().total&&owner==HTTP_OWNER_UPDATE);
        unsigned before=cancels;owner=HTTP_OWNER_NET;
        step(1,0);assert(!online_update_active()&&snapshot().state==ONLINE_UPDATE_FAILED);
        assert(owner==HTTP_OWNER_NET&&cancels==before&&!creates);puts("PASS retained API owner changed safely");return 0;
    }
    terminal();
    online_update_snapshot_t state=snapshot();
    if(is("check-invalid")||is("redirect-api-evil")||is("boot-extra-chunked")) {
        if(is("boot-extra-chunked"))assert(state.result==ONLINE_UPDATE_E_LENGTH&&state.available_mask==1);
        assert(state.state==ONLINE_UPDATE_FAILED&&!creates&&!renames);printf("PASS check-invalid %d\n",state.result);return 0;
    }
    if(is("current")||is("downgrade")||is("no-assets")) {
        assert(state.state==ONLINE_UPDATE_CURRENT&&!state.available_mask&&!creates);
        assert(online_update_start(ONLINE_UPDATE_FIRMWARE)<0);assert(online_update_start(ONLINE_UPDATE_BOOT)<0);
        printf("PASS %s %s %s\n",scenario,state.latest_f,state.latest_b);return 0;
    }
    assert(state.state==(is("bad-boot-fw-available")?ONLINE_UPDATE_FAILED:ONLINE_UPDATE_AVAILABLE));
    assert(!strcmp(state.latest_f,is("missing-fw")?"":"F1.3.4"));
    assert(!strcmp(state.latest_b,is("missing-boot")||is("bad-boot-fw-available")?"":"B2.0.6"));
    unsigned expected_mask=is("boot-current")||is("missing-boot")||is("bad-boot-fw-available")?1U:
                           is("firmware-current")||is("missing-fw")?2U:3U;
    assert(state.available_mask==expected_mask);
    if(is("boot-current")) { assert(online_update_start(ONLINE_UPDATE_BOOT)<0);printf("PASS boot-independent-current\n");return 0; }
    if(is("start-owner-busy")) {
        unsigned old_cancels=cancels;
        occupied=1;owner=HTTP_OWNER_NET;status_http.state=HTTP_S_READ;
        assert(!online_update_start(ONLINE_UPDATE_FIRMWARE));
        for(unsigned i=0;i<20&&online_update_active();++i)step(1,0);
        assert(!online_update_active()&&!handles&&!renames&&owner==HTTP_OWNER_NET&&cancels==old_cancels);
        assert(find("0:/FIRMWARE.BIN.part")<0);puts("PASS start-owner-busy keeps other owner");return 0;
    }
    if(is("root-conflict"))make("0:/FIRMWARE.BIN","OLD FIRMWARE",12);
    if(is("part-conflict")||is("power-leftover"))make("0:/FIRMWARE.BIN.part","FOREIGN PART",12);
    if(is("old-boot-preserved"))make("0:/BOOT.BIN","OLD BOOT",8);
    if(is("readonly"))readonly_sd=1;
    if(is("write-fail"))write_error=1;
    if(is("short-write"))short_write=1;
    if(is("sync-fail"))sync_error=1;
    if(is("close-fail"))close_error=1;
    if(is("read-fail"))read_error=1;
    if(is("rename-fail"))rename_error=1;
    if(is("rename-race"))rename_race=1;
    if(is("truncated"))byte_limit=(unsigned)documents[1].length-17;
    if(is("http-fail"))fail_after=337;
    int use_boot=is("boot-success")||is("firmware-current")||is("missing-fw");
    if(is("missing-fw"))assert(online_update_start(ONLINE_UPDATE_FIRMWARE)<0);
    if(is("missing-boot"))assert(online_update_start(ONLINE_UPDATE_BOOT)<0);
    int start_result=online_update_start(use_boot?ONLINE_UPDATE_BOOT:ONLINE_UPDATE_FIRMWARE);
    if(is("root-conflict")||is("part-conflict")||is("power-leftover")) {
        if(!start_result)terminal();
        assert(!creates&&!renames&&!unlinks);expect_old(is("root-conflict")?"0:/FIRMWARE.BIN":"0:/FIRMWARE.BIN.part",is("root-conflict")?"OLD FIRMWARE":"FOREIGN PART");
        printf("PASS %s %d\n",scenario,snapshot().result);return 0;
    }
    assert(!start_result);
    if(is("cancel-restore")||is("completion-restore")) {
        cancel_block=1;
        if(is("cancel-restore")) {
            for(unsigned i=0;i<20&&(!occupied||!snapshot().received);++i)step(1,0);
            assert(occupied&&owner==HTTP_OWNER_UPDATE);online_update_cancel();
        }
        for(unsigned i=0;i<20000&&snapshot().state!=ONLINE_UPDATE_FAILED;++i)step(1,0);
        assert(online_update_active()&&owner==HTTP_OWNER_UPDATE&&!handles&&!renames);
        assert(snapshot().result==(is("cancel-restore")?ONLINE_UPDATE_E_CANCELLED:HTTP_E_IO));
        assert(online_update_start(ONLINE_UPDATE_FIRMWARE)==HTTP_E_BUSY);
        cancel_block=0;terminal();assert(!renames&&find("0:/FIRMWARE.BIN.part")<0);
        printf("PASS %s heldHTTPowner across deferredrestore; noverify/publish\n",scenario);return 0;
    }
    if(is("cancel")||is("lost-allowed")||is("storage-busy")||is("cleanup-retry")) {
        for(unsigned i=0;i<10&&!snapshot().received;++i)step(1,0);
        assert(snapshot().received||!online_update_active());
        if(is("cleanup-retry"))unlink_error=1;
        if(is("cancel")||is("cleanup-retry"))online_update_cancel();
        else if(is("lost-allowed"))step(0,0);
        else step(1,1);
        for(unsigned i=0;i<10&&online_update_active();++i)step(1,0);
        if(is("cleanup-retry")) { assert(online_update_active());unlink_error=0; }
        terminal();assert(!renames&&find("0:/FIRMWARE.BIN")<0&&find("0:/FIRMWARE.BIN.part")<0);
        assert(snapshot().state==ONLINE_UPDATE_CANCELLED||snapshot().state==ONLINE_UPDATE_FAILED);
        printf("PASS %s cleanup, owner released\n",scenario);return 0;
    }
    if(is("close-fail")) {
        for(unsigned i=0;i<20000&&online_update_active()&&!handles;++i)step(1,0);
        for(unsigned i=0;i<20000&&online_update_active()&&snapshot().state!=ONLINE_UPDATE_FAILED;++i)step(1,0);
        assert(handles&&online_update_active());close_error=0;
    }
    terminal();state=snapshot();
    if(is("success")||use_boot||is("old-boot-preserved")||is("missing-boot")||
       is("bad-boot-fw-available")||is("redirect-assets")||is("redirect-objects")||is("cancel-ready")) {
        unsigned kind=use_boot?2:1;
        const char *path=kind==2?"0:/BOOT.BIN":"0:/FIRMWARE.BIN";
        int slot=find(path);assert(state.state==ONLINE_UPDATE_READY&&slot>=0&&renames==1);
        assert(fs[slot].size==documents[kind].length&&!memcmp(fs[slot].data,documents[kind].data,fs[slot].size));
        assert(!strcmp(online_update_ready_path(),path));assert(!handles&&!occupied);
        if(is("old-boot-preserved"))expect_old("0:/BOOT.BIN","OLD BOOT");
        if(is("cancel-ready")) {
            online_update_cancel();assert(!*online_update_ready_path()&&snapshot().state==ONLINE_UPDATE_CANCELLED);
            assert(find(path)==slot&&!unlinks);
        }
        /* A repeated init/check cannot remove the already prepared root file. */
        online_update_init(installed_f,installed_b);
        assert(find(path)==slot&&fs[slot].size==documents[kind].length&&
               !memcmp(fs[slot].data,documents[kind].data,fs[slot].size));
    } else {
        assert(state.state==ONLINE_UPDATE_FAILED&&state.result<0&&!renames);
        assert(!*online_update_ready_path());assert(find("0:/FIRMWARE.BIN.part")<0);
        if(is("rename-race"))expect_old("0:/FIRMWARE.BIN","RACE");
        else assert(find("0:/FIRMWARE.BIN")<0);
    }
    printf("PASS %s state=%u result=%d starts=%u writes=%u reads=%u polls=%u\n",scenario,state.state,state.result,starts,writes,reads,polls);
    return 0;
}
