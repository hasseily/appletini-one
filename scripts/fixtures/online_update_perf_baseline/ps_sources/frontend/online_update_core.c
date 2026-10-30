#include "online_update_core.h"
#include "../lib/multiboot.h"
#include <stdio.h>
#include <string.h>

static int version(const char *text, char prefix, uint32_t parts[3])
{
    if (!text) return 0;
    if (!strncmp(text,"Firmware ",9)) text+=9;
    else if (!strncmp(text,"Boot ",5)) text+=5;
    if (*text++!=prefix) return 0;
    for (unsigned i=0;i<3;++i) {
        uint32_t n=0;
        const char *start=text;
        while (*text>='0'&&*text<='9') {
            unsigned digit=(unsigned)(*text++-'0');
            if (n>(UINT32_MAX-digit)/10U) return 0;
            n=n*10U+digit;
        }
        if (text==start || (text-start>1 && *start=='0')) return 0;
        parts[i]=n;
        if (i<2) { if (*text++!='.') return 0; }
        else if (*text) return 0;
    }
    return 1;
}
int online_update_version_compare(const char *installed,const char *offered,
                                  char prefix,int *comparison)
{
    uint32_t a[3],b[3];
    if (!comparison || (prefix!='F'&&prefix!='B') ||
        !version(installed,prefix,a) || !version(offered,prefix,b))
        return ONLINE_UPDATE_E_VERSION;
    *comparison=0;
    for (unsigned i=0;i<3;++i) if (a[i]!=b[i]) {
        *comparison=b[i]>a[i]?1:-1; break;
    }
    return 0;
}
const char *online_update_filename(uint8_t kind)
{
    return kind==ONLINE_UPDATE_FIRMWARE?"FIRMWARE.BIN":
           kind==ONLINE_UPDATE_BOOT?"BOOT.BIN":"";
}
static int hex(unsigned c)
{
    return c>='0'&&c<='9'?(int)(c-'0'):c>='a'&&c<='f'?(int)(c-'a'+10):
           c>='A'&&c<='F'?(int)(c-'A'+10):-1;
}
/* Names, versions and URLs have an ASCII contract. Unknown JSON fields remain
 * valid UTF-8 but are skipped without copying them into a fixed buffer. */
static int string(const net_json_t *j,unsigned token,char *out,size_t capacity)
{
    const net_json_token_t *t=&j->tokens[token];
    size_t n=0;
    if ((t->type&0x7FU)!=NJ_STRING || t->end<t->start+2U) return 0;
    for (uint32_t at=t->start+1U;at<t->end-1U;++at) {
        unsigned c=j->source[at];
        if (c=='\\') {
            c=j->source[++at];
            if (c=='u') {
                c=0;
                for (unsigned k=0;k<4;++k) c=(c<<4)|(unsigned)hex(j->source[++at]);
            } else if (c!='"'&&c!='\\'&&c!='/') return 0;
        }
        if (c<32U || c>126U || n+1U>=capacity) return 0;
        out[n++]=(char)c;
    }
    out[n]=0; return 1;
}
static int literal(const net_json_t *j,unsigned token,const char *text)
{
    const net_json_token_t *t=&j->tokens[token];
    size_t n=strlen(text);
    return (size_t)(t->end-t->start)==n && !memcmp(j->source+t->start,text,n);
}
static int number(const net_json_t *j,unsigned token,uint32_t *out)
{
    const net_json_token_t *t=&j->tokens[token];
    uint32_t n=0;
    if (t->type!=NJ_NUMBER || t->start==t->end) return 0;
    for (unsigned at=t->start;at<t->end;++at) {
        unsigned d=(unsigned)(j->source[at]-'0');
        if (d>9U || n>(UINT32_MAX-d)/10U) return 0;
        n=n*10U+d;
    }
    *out=n; return 1;
}
static int asset(online_update_core_t *c,unsigned object)
{
    net_json_t *j=&c->json;
    char name[64]="",url[ONLINE_UPDATE_URL_BYTES]="",digest[80]="";
    uint32_t size=0;
    unsigned seen=0, url_token=0, digest_token=0, size_token=0;
    if (j->tokens[object].type!=NJ_OBJECT) return ONLINE_UPDATE_E_ASSET;
    for (unsigned at=object+1U;at<j->tokens[object].next;) {
        unsigned value=at+1U,bit=0;
        char key[32];
        if (string(j,at,key,sizeof key)) {
            if (!strcmp(key,"name")) bit=1;
            else if (!strcmp(key,"browser_download_url")) bit=2;
            else if (!strcmp(key,"digest")) bit=4;
            else if (!strcmp(key,"size")) bit=8;
        }
        if (bit) {
            if (seen&bit) return ONLINE_UPDATE_E_ASSET;
            seen|=bit;
            /* A long or non-ASCII unrelated asset name cannot equal either
             * fixed update filename, and must not disable valid BIN assets. */
            if (bit==1 && !string(j,value,name,sizeof name)) name[0]=0;
            if (bit==2) url_token=value;
            if (bit==4) digest_token=value;
            if (bit==8) size_token=value;
        }
        at=j->tokens[value].next;
    }
    unsigned kind;
    if (!strcmp(name,"FIRMWARE.BIN")) kind=ONLINE_UPDATE_FIRMWARE;
    else if (!strcmp(name,"BOOT.BIN")) kind=ONLINE_UPDATE_BOOT;
    else return 0;
    if (seen!=15U || !string(j,url_token,url,sizeof url) ||
        !string(j,digest_token,digest,sizeof digest) || !number(j,size_token,&size))
        return ONLINE_UPDATE_E_ASSET;
    online_update_asset_t *a=&c->release.assets[kind];
    char expected[ONLINE_UPDATE_URL_BYTES];
    int n=snprintf(expected,sizeof expected,"%s%s/%s",ONLINE_UPDATE_DOWNLOAD_PREFIX,
                    c->release.tag,online_update_filename((uint8_t)kind));
    uint32_t maximum=kind==ONLINE_UPDATE_BOOT?MB_GOLDEN_SIZE:MB_APPLETINI_CAPACITY;
    if (seen!=15U || a->present || size<MB_MANIFEST_SIZE || size>maximum ||
        n<0 || (size_t)n>=sizeof expected || strcmp(url,expected) ||
        strlen(digest)!=71U || memcmp(digest,"sha256:",7)) return ONLINE_UPDATE_E_ASSET;
    for (unsigned i=0;i<32;++i) {
        int high=hex((unsigned char)digest[7+2*i]);
        int low=hex((unsigned char)digest[8+2*i]);
        if (high<0 || low<0) return ONLINE_UPDATE_E_ASSET;
        a->sha256[i]=(uint8_t)((high<<4)|low);
    }
    a->size=size; strcpy(a->url,url); a->present=1;
    return 0;
}
static int release(online_update_core_t *c)
{
    net_json_t *j=&c->json;
    unsigned seen=0,assets=0;
    uint32_t parts[3];
    if (!j->count || j->tokens[0].type!=NJ_OBJECT) return ONLINE_UPDATE_E_RELEASE;
    for (unsigned at=1;at<j->tokens[0].next;) {
        unsigned value=at+1U,bit=0;
        char key[32];
        if (string(j,at,key,sizeof key)) {
            if (!strcmp(key,"tag_name")) bit=1;
            else if (!strcmp(key,"draft")) bit=2;
            else if (!strcmp(key,"prerelease")) bit=4;
            else if (!strcmp(key,"published_at")) bit=8;
            else if (!strcmp(key,"assets")) bit=16;
        }
        if (bit) {
            if (seen&bit) return ONLINE_UPDATE_E_RELEASE;
            seen|=bit;
            if (bit==1 && (!string(j,value,c->release.tag,sizeof c->release.tag) ||
                c->release.tag[0]!='F' || strchr(c->release.tag,' ') ||
                !version(c->release.tag,'F',parts)))
                return ONLINE_UPDATE_E_RELEASE;
            if ((bit==2 || bit==4) && (j->tokens[value].type!=NJ_BOOL ||
                !literal(j,value,"false"))) return ONLINE_UPDATE_E_RELEASE;
            if (bit==8 && (j->tokens[value].type!=NJ_STRING ||
                (unsigned)(j->tokens[value].end-j->tokens[value].start)<3U)) return ONLINE_UPDATE_E_RELEASE;
            if (bit==16) {
                if (j->tokens[value].type!=NJ_ARRAY) return ONLINE_UPDATE_E_RELEASE;
                assets=value;
            }
        }
        at=j->tokens[value].next;
    }
    if (seen!=31U) return ONLINE_UPDATE_E_RELEASE;
    for (unsigned at=assets+1U;at<j->tokens[assets].next;at=j->tokens[at].next) {
        int result=asset(c,at);
        if (result) return result;
    }
    return 1;
}
void online_update_core_init(online_update_core_t *c,const uint8_t *source)
{
    if (!c) return;
    memset(&c->release,0,sizeof c->release);
    c->result=source?0:ONLINE_UPDATE_E_ARGUMENT;
    net_json_init(&c->json,source);
}
int online_update_core_feed(online_update_core_t *c,size_t available,int final,size_t budget)
{
    if (!c) return ONLINE_UPDATE_E_ARGUMENT;
    if (c->result) return c->result;
    if (available>ONLINE_UPDATE_JSON_BYTES) return c->result=ONLINE_UPDATE_E_LENGTH;
    net_json_feed(&c->json,available,final,budget);
    if (c->json.bad) return c->result=ONLINE_UPDATE_E_JSON;
    if (c->json.complete) c->result=release(c);
    return c->result;
}
