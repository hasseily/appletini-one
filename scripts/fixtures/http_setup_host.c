/* Production boot setup and real MbedTLS; only FATFS, ADC result, NIC and
 * clock are modeled. No private seed fixture is written to the host disk. */
#define HTTP_HOST_FAKE_SD
#define main network_fixture_main
#include "http_network_host.c"
#undef main
#include "../../ps_sources/frontend/http_roots.h"

enum { CFG, SEED_A, SEED_B, PEM, FILES };
static struct { uint8_t data[256]; size_t length; int exists; } files[FILES];
static unsigned entropy_calls, creates, writes, syncs, reads, pem_opens, handles;
static unsigned tests;
static int entropy_failed, readonly_sd, stat_failed, mount_failed;
static int create_failed, sync_failed, read_failed, close_failed, short_write, corrupt_read;
static int create_race;
static int seed_stat_failed;

static int path_slot(const char *path)
{
    if (!strcmp(path,"0:/ATNET.CFG")) return CFG;
    if (!strcmp(path,"0:/ATNETA.SED")) return SEED_A;
    if (!strcmp(path,"0:/ATNETB.SED")) return SEED_B;
    if (!strcmp(path,"0:/ATNETCA.PEM")) return PEM;
    assert(!"unexpected setup file"); return -1;
}
FRESULT f_stat(const char *path,FILINFO *info)
{
    int slot=path_slot(path);
    if(stat_failed) return FR_DISK_ERR;
    if(seed_stat_failed==slot) return FR_DISK_ERR;
    if(!files[slot].exists) return FR_NO_FILE;
    info->fsize=files[slot].length; return FR_OK;
}
FRESULT f_mount(FATFS *fs,const char *path,int now)
{ (void)fs;(void)path;(void)now;return mount_failed?FR_DISK_ERR:FR_OK; }
FRESULT f_open(FIL *file,const char *path,int mode)
{
    int slot=path_slot(path);
    if(slot==PEM) ++pem_opens;
    if(mode&FA_WRITE) {
        if(readonly_sd) return FR_WRITE_PROTECTED;
        if(mode&FA_CREATE_NEW) {
            if(create_race && slot==SEED_A) {
                files[slot].exists=1;files[slot].length=HTTP_SEED_RECORD;
                memset(files[slot].data,0x5A,HTTP_SEED_RECORD);
            }
            if(files[slot].exists) return FR_EXIST;
            if(create_failed==slot) return FR_DENIED;
            ++creates; files[slot].exists=1;files[slot].length=0;
        } else if(mode&FA_CREATE_ALWAYS) {
            files[slot].exists=1;files[slot].length=0;
        }
    }
    if(!files[slot].exists) return FR_NO_FILE;
    file->slot=slot;file->mode=mode;file->size=files[slot].length;file->position=0;
    ++handles;return FR_OK;
}
FRESULT f_close(FIL *file)
{
    assert(handles);--handles;
    return close_failed==file->slot ? FR_DISK_ERR : FR_OK;
}
FRESULT f_read(FIL *file,void *out,UINT length,UINT *got)
{
    int slot=file->slot;++reads;*got=0;
    if(read_failed==slot) return FR_DISK_ERR;
    if(length>files[slot].length-file->position) length=(UINT)(files[slot].length-file->position);
    memcpy(out,files[slot].data+file->position,length);file->position+=length;*got=length;
    if(corrupt_read==slot && length) ((uint8_t*)out)[20%length]^=1;
    return FR_OK;
}
FRESULT f_write(FIL *file,const void *input,UINT length,UINT *got)
{
    int slot=file->slot;assert(file->mode&FA_WRITE);++writes;
    assert(length<=sizeof(files[slot].data)-file->position);
    if(short_write==slot && length) --length;
    memcpy(files[slot].data+file->position,input,length);file->position+=length;
    files[slot].length=file->size=file->position;*got=length;return FR_OK;
}
FRESULT f_sync(FIL *file)
{ ++syncs;return sync_failed==file->slot?FR_DISK_ERR:FR_OK; }
FRESULT f_unlink(const char *path)
{ (void)path;assert(!"boot must not delete seed files");return FR_DENIED; }
int http_entropy_seed(uint8_t out[HTTP_SEED_BYTES])
{
    ++entropy_calls;
    for(unsigned i=0;i<HTTP_SEED_BYTES;++i) out[i]=entropy_failed?0:(uint8_t)(i+0x31);
    return entropy_failed?-1:0;
}
void http_entropy_status(http_entropy_stats_t *out)
{
    memset(out,0,sizeof(*out));out->result=entropy_failed?HTTP_ENTROPY_UNAVAILABLE:HTTP_ENTROPY_OK;
    out->samples=entropy_calls?HTTP_ENTROPY_SAMPLES:0;
}
const char *http_entropy_result_name(int result) { (void)result;return "modeled ADC"; }
static void service_reset(void)
{
    crypto_reset();mbedtls_memory_buffer_alloc_free();memset(&g_http,0,sizeof(g_http));
    boot_prepared=0;prepare_error=0;rtc_valid=0;storage_busy=0;
    host_time=1000000;http_service_clock(2026,10,29,12,0,0,1);
    memset(host_dns,1,sizeof(host_dns));
    ir_reads=rx_reads=closes=connects=sends=clear_flags=tx_bytes=0;
}
static void case_reset(void)
{
    service_reset();memset(files,0,sizeof(files));
    entropy_calls=creates=writes=syncs=reads=pem_opens=handles=0;
    entropy_failed=readonly_sd=stat_failed=mount_failed=create_race=0;
    seed_stat_failed=-1;
    create_failed=sync_failed=read_failed=close_failed=short_write=corrupt_read=-1;
}
static void existing_pair(void)
{
    uint8_t seed[HTTP_SEED_BYTES];memset(seed,0x67,sizeof(seed));
    for(int slot=SEED_A;slot<=SEED_B;++slot) {
        http_seed_encode(files[slot].data,5,seed);
        files[slot].exists=1;files[slot].length=HTTP_SEED_RECORD;
    }
}
static void boot_failure(int expected)
{
    assert(http_service_boot_prepare()==expected);
    assert(!g_http.crypto_ready && !handles);
    assert(request_config()==expected);
    http_request_t request={"https://example.com/","GET",NULL,NULL,0,0};
    assert(!http_service_start(&request));http_service_poll();
    http_status_t status;http_service_status(&status);
    assert(status.state==HTTP_S_FAILED && status.result==expected);
    assert(!connects&&!sends&&!ir_reads&&!g_http.owns_eth);++tests;
}
int main(void)
{
    case_reset();assert(!http_service_boot_prepare());
    assert(g_http.crypto_ready && g_http.trust_ready && !request_config());
    assert(entropy_calls==1 && creates==2 && writes==3 && syncs==3 && !handles && pem_opens==1);
    assert(g_http.status.seed_generation==2);
    assert(rtc_offset==0 && !memcmp(g_http.dns,"\1\1\1\1",4));
    unsigned anchors=0;for(mbedtls_x509_crt *c=&g_http.ca;c;c=c->next)if(c->raw.p)++anchors;
    assert(anchors==HTTP_BUILTIN_ROOT_COUNT);
    unsigned before=writes;assert(!http_service_boot_prepare() && writes==before && entropy_calls==1);++tests;

    case_reset();existing_pair();uint8_t saved[HTTP_SEED_RECORD];memcpy(saved,files[SEED_A].data,sizeof(saved));
    assert(!http_service_boot_prepare());
    assert(!entropy_calls && !creates && writes==1 && g_http.status.seed_generation==6);
    assert(!memcmp(saved,files[SEED_A].data,sizeof(saved)));++tests;

    case_reset();existing_pair();files[CFG].exists=1;files[CFG].length=8;
    memcpy(files[CFG].data,"N\1\11\11\11\11\170\0",8);
    assert(!http_service_boot_prepare());assert(rtc_offset==120&&!memcmp(g_http.dns,"\11\11\11\11",4));++tests;

    case_reset();existing_pair();readonly_sd=1;
    memcpy(saved,files[SEED_B].data,sizeof(saved));
    boot_failure(HTTP_E_SEED);assert(!entropy_calls&&!creates&&!writes);
    assert(!memcmp(saved,files[SEED_B].data,sizeof(saved)));

    case_reset();existing_pair();files[SEED_B].exists=0;
    boot_failure(HTTP_E_SEED);assert(!entropy_calls&&!creates&&!writes);
    case_reset();existing_pair();files[SEED_A].exists=0;
    boot_failure(HTTP_E_SEED);assert(!entropy_calls&&!creates&&!writes);
    case_reset();existing_pair();files[SEED_A].data[20]^=1;
    boot_failure(HTTP_E_SEED);assert(!entropy_calls&&!creates&&!writes);
    case_reset();readonly_sd=1;
    boot_failure(HTTP_E_SEED);assert(entropy_calls==1&&!creates&&!writes);
    case_reset();entropy_failed=1;
    boot_failure(HTTP_E_SEED);assert(entropy_calls==1&&!creates&&!writes);
    case_reset();create_failed=SEED_B;
    boot_failure(HTTP_E_SEED);assert(entropy_calls==1&&creates==1&&files[SEED_A].exists&&!files[SEED_B].exists);
    create_failed=-1;service_reset();boot_failure(HTTP_E_SEED);assert(entropy_calls==1);
    case_reset();sync_failed=SEED_A;
    boot_failure(HTTP_E_SEED);assert(creates==1&&syncs==1);
    case_reset();short_write=SEED_A;
    boot_failure(HTTP_E_SEED);assert(creates==1);
    case_reset();corrupt_read=SEED_A;
    boot_failure(HTTP_E_SEED);assert(creates==1);
    case_reset();read_failed=SEED_A;
    boot_failure(HTTP_E_SEED);assert(creates==1);
    case_reset();close_failed=SEED_A;
    boot_failure(HTTP_E_SEED);assert(creates==1&&!handles);
    case_reset();create_race=1;
    boot_failure(HTTP_E_SEED);assert(!creates&&!writes);
    for(unsigned i=0;i<HTTP_SEED_RECORD;++i) assert(files[SEED_A].data[i]==0x5A);
    case_reset();files[CFG].exists=1;files[CFG].length=8;
    boot_failure(HTTP_E_ARGUMENT);assert(!entropy_calls&&!creates&&!writes);
    case_reset();stat_failed=1;
    boot_failure(HTTP_E_IO);assert(!entropy_calls&&!creates&&!writes);
    case_reset();seed_stat_failed=SEED_A;
    boot_failure(HTTP_E_SEED);assert(!entropy_calls&&!creates&&!writes);
    case_reset();seed_stat_failed=SEED_B;
    boot_failure(HTTP_E_SEED);assert(!entropy_calls&&!creates&&!writes);
    service_reset();printf("PASS %u production HTTPS setup cases: first use, existing-state preservation, partial/corrupt state, create race, storage faults, embedded roots and no PEM dependency\n",tests);
    return 0;
}
