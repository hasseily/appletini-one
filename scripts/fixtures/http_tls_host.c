#include "mbedtls/ssl.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/memory_buffer_alloc.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/platform_time.h"
#include "mbedtls/platform.h"
#include "http_roots.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static unsigned char pool[512U*1024U], pem[65537];
mbedtls_ms_time_t mbedtls_ms_time(void) { return (mbedtls_ms_time_t)((uint64_t)clock()*1000U/CLOCKS_PER_SEC); }
struct tm *mbedtls_platform_gmtime_r(const mbedtls_time_t *value,struct tm *out)
{ struct tm *p=gmtime(value);if(!p)return NULL;*out=*p;return out; }
static int entropy(void *ctx,unsigned char *out,size_t len)
{ (void)ctx;memset(out,0x5A,len);return 0; } /* Fixture only; no network or keys leave this test. */
int main(int argc,char **argv)
{
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config config;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_x509_crt ca;
    static const int suites[]={MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,0};
    assert(argc==2);
    mbedtls_memory_buffer_alloc_init(pool,sizeof(pool));
    /* Odd sizes must not leave later 64-bit contexts at four-byte offsets. */
    void *odd[12];
    for (unsigned i=0;i<12;++i) {
        odd[i]=mbedtls_calloc(1,4U*i+1U);
        assert(odd[i] && (uintptr_t)odd[i] % 8U == 0);
    }
    for (unsigned i=0;i<12;++i) mbedtls_free(odd[i]);
    mbedtls_ssl_init(&ssl);mbedtls_ssl_config_init(&config);mbedtls_ctr_drbg_init(&drbg);mbedtls_x509_crt_init(&ca);
    mbedtls_ctr_drbg_set_entropy_len(&drbg,48);
    assert(!mbedtls_ctr_drbg_seed(&drbg,entropy,NULL,(const unsigned char *)"native fixture",14));
    if(!strcmp(argv[1],"embedded")) {
        assert(!http_builtin_roots_load(&ca));
    } else {
        FILE *file=fopen(argv[1],"rb");assert(file);
        size_t n=fread(pem,1,sizeof(pem)-1,file);assert(!ferror(file));fclose(file);pem[n]=0;
        assert(!mbedtls_x509_crt_parse(&ca,pem,n+1));
    }
    for(unsigned i=0;i<5;++i) {
        mbedtls_ssl_config_free(&config);mbedtls_ssl_config_init(&config);
        assert(!mbedtls_ssl_config_defaults(&config,MBEDTLS_SSL_IS_CLIENT,MBEDTLS_SSL_TRANSPORT_STREAM,MBEDTLS_SSL_PRESET_DEFAULT));
        mbedtls_ssl_conf_authmode(&config,MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&config,&ca,NULL);mbedtls_ssl_conf_rng(&config,mbedtls_ctr_drbg_random,&drbg);
        mbedtls_ssl_conf_ciphersuites(&config,suites);
        assert(!mbedtls_ssl_setup(&ssl,&config));
        assert(!mbedtls_ssl_set_hostname(&ssl,"checkpoint.appletini.test"));
        assert(!mbedtls_memory_buffer_alloc_verify());
        mbedtls_ssl_free(&ssl);mbedtls_ssl_init(&ssl);
    }
    size_t peak,blocks;mbedtls_memory_buffer_alloc_max_get(&peak,&blocks);
    assert(peak<sizeof(pool));
    printf("TLS pool peak with supplied roots and repeated SSL setup: %zu/%zu bytes (%zu blocks)\n",peak,sizeof(pool),blocks);
    mbedtls_ssl_config_free(&config);mbedtls_x509_crt_free(&ca);mbedtls_ctr_drbg_free(&drbg);
    assert(!mbedtls_memory_buffer_alloc_verify());mbedtls_memory_buffer_alloc_free();
    puts("PASS native Mbed TLS: public/test CA parsing, repeated config/setup/free, allocator integrity");
    return 0;
}
