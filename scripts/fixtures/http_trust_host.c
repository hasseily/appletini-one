/* Test the exact production trust_load() with real Mbed TLS parsing and an
 * SD model. The Python test extracts that function without changing it. */
#include "http_roots.h"
#include "mbedtls/memory_buffer_alloc.h"
#include "mbedtls/platform.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static unsigned char pool[512U*1024U] __attribute__((aligned(32)));
static unsigned char trust_pem[65568U] __attribute__((aligned(32)));
static struct { mbedtls_x509_crt ca; unsigned char trust_ready; } g_http;
enum { FR_OK=0,FR_DISK_ERR=1,FR_NO_FILE=4,FA_READ=1,HTTP_E_TRUST=-105 };
typedef unsigned UINT;
typedef int FRESULT;
typedef struct { size_t size; } FIL;
#define f_size(file) ((file)->size)
static unsigned char override[4096],single[4096];
static size_t override_size,single_size;
static int open_result,read_result,close_result,short_read;
static unsigned opens,reads,closes;

mbedtls_ms_time_t mbedtls_ms_time(void)
{ return (mbedtls_ms_time_t)((unsigned long long)clock()*1000U/CLOCKS_PER_SEC); }
struct tm *mbedtls_platform_gmtime_r(const mbedtls_time_t *value,struct tm *out)
{ struct tm *p=gmtime(value);if(!p)return NULL;*out=*p;return out; }
static FRESULT f_open(FIL *file,const char *path,int mode)
{
    assert(!strcmp(path,"0:/ATNETCA.PEM")&&mode==FA_READ);
    ++opens;file->size=override_size;return open_result;
}
static FRESULT f_read(FIL *file,void *out,UINT length,UINT *got)
{
    (void)file;++reads;
    assert(length==override_size&&length<=sizeof(override));
    *got=short_read?length-1U:length;
    memcpy(out,override,*got);return read_result;
}
static FRESULT f_close(FIL *file) { (void)file;++closes;return close_result; }

#include "trust_load.inc"

static unsigned roots(void)
{
    unsigned n=0;
    for(mbedtls_x509_crt *c=&g_http.ca;c&&c->version;c=c->next)++n;
    return n;
}
static void fresh(void)
{
    mbedtls_x509_crt_free(&g_http.ca);mbedtls_x509_crt_init(&g_http.ca);
    g_http.trust_ready=0;opens=reads=closes=0;
    open_result=read_result=close_result=FR_OK;short_read=0;
    override_size=single_size;memcpy(override,single,single_size);
}
static void rejected(void)
{
    assert(trust_load()==HTTP_E_TRUST&&!g_http.trust_ready&&!roots());
    assert(!mbedtls_memory_buffer_alloc_verify());
}
int main(int argc,char **argv)
{
    assert(argc==2);
    FILE *file=fopen(argv[1],"rb");assert(file);
    single_size=fread(single,1,sizeof(single),file);assert(feof(file)&&single_size);fclose(file);
    mbedtls_memory_buffer_alloc_init(pool,sizeof(pool));mbedtls_x509_crt_init(&g_http.ca);
    assert(http_builtin_roots_load(NULL)==MBEDTLS_ERR_X509_BAD_INPUT_DATA);
    /* Only a genuinely absent override selects built-in roots. */
    fresh();open_result=FR_NO_FILE;
    assert(!trust_load()&&g_http.trust_ready&&roots()==HTTP_BUILTIN_ROOT_COUNT);
    assert(opens==1&&!reads&&!closes);
    for(mbedtls_x509_crt *c=&g_http.ca;c&&c->version;c=c->next)
        assert(!c->MBEDTLS_PRIVATE(own_buffer));
    /* Cached roots do not reopen the SD file. */
    open_result=FR_DISK_ERR;assert(!trust_load()&&opens==1);
    /* A valid older/user SD bundle replaces, rather than appends defaults. */
    fresh();assert(!trust_load()&&g_http.trust_ready&&roots()==1);
    assert(reads==1&&closes==1&&g_http.ca.MBEDTLS_PRIVATE(own_buffer));
    for(size_t i=0;i<sizeof(trust_pem);++i)assert(!trust_pem[i]);
    fresh();open_result=FR_DISK_ERR;rejected();assert(!reads&&!closes);
    fresh();read_result=FR_DISK_ERR;rejected();assert(reads==1&&closes==1);
    fresh();short_read=1;rejected();
    fresh();close_result=FR_DISK_ERR;rejected();
    fresh();override_size=0;rejected();assert(!reads&&closes==1);
    fresh();override_size=sizeof(trust_pem);rejected();assert(!reads&&closes==1);
    fresh();memcpy(override,"not a certificate",17);override_size=17;rejected();
    /* A partially parseable PEM must not retain its successful first root. */
    fresh();static const char bad[]="\n-----BEGIN CERTIFICATE-----\nINVALID\n-----END CERTIFICATE-----\n";
    memcpy(override+override_size,bad,sizeof(bad)-1U);override_size+=sizeof(bad)-1U;rejected();
    fresh();open_result=FR_NO_FILE;
    mbedtls_memory_buffer_alloc_free();mbedtls_memory_buffer_alloc_init(pool,1024);
    rejected();
    mbedtls_x509_crt_free(&g_http.ca);
    assert(!mbedtls_memory_buffer_alloc_verify());mbedtls_memory_buffer_alloc_free();
    printf("PASS embedded trust: %u borrowed DER roots, legacy SD override precedence, cache, invalid/open/read/close/size failures and allocator exhaustion\n", (unsigned)HTTP_BUILTIN_ROOT_COUNT);
    return 0;
}
