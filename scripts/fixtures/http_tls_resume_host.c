/* TLS session reuse with the firmware's Mbed TLS configuration: one full
 * handshake, then a second connection that offers the saved session. The
 * call order is the one in http_service.c. */
#define _WIN32_WINNT 0x0601
#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "mbedtls/ssl.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/memory_buffer_alloc.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/ecp.h"
#include "http_roots.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static unsigned char pool[512U*1024U] __attribute__((aligned(32)));
mbedtls_ms_time_t mbedtls_ms_time(void) { return (mbedtls_ms_time_t)GetTickCount64(); }
struct tm *mbedtls_platform_gmtime_r(const mbedtls_time_t *t,struct tm *out)
{ struct tm *p=gmtime(t);if(!p)return NULL;*out=*p;return out; }
static int entropy(void *ctx,unsigned char *out,size_t n)
{ (void)ctx;for(size_t i=0;i<n;++i)out[i]=(unsigned char)(0x51U+i);return 0; } /* Fixture only. */
static int send_bytes(void *ctx,const unsigned char *p,size_t n)
{
    int r=send(*(SOCKET *)ctx,(const char *)p,(int)n,0);
    if(r>=0)return r;
    return WSAGetLastError()==WSAEWOULDBLOCK?MBEDTLS_ERR_SSL_WANT_WRITE:MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}
static int receive_bytes(void *ctx,unsigned char *p,size_t n)
{
    int r=recv(*(SOCKET *)ctx,(char *)p,(int)n,0);
    if(r>=0)return r;
    return WSAGetLastError()==WSAEWOULDBLOCK?MBEDTLS_ERR_SSL_WANT_READ:MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}
static uint64_t usec(void)
{ LARGE_INTEGER t,f;QueryPerformanceCounter(&t);QueryPerformanceFrequency(&f);return (uint64_t)t.QuadPart*1000000ULL/(uint64_t)f.QuadPart; }

int main(int argc,char **argv)
{
    assert(argc==5);
    WSADATA ws;assert(!WSAStartup(MAKEWORD(2,2),&ws));
    struct sockaddr_in remote={0};remote.sin_family=AF_INET;remote.sin_port=htons((unsigned short)atoi(argv[2]));
    assert(inet_pton(AF_INET,argv[1],&remote.sin_addr)==1);
    mbedtls_ssl_context ssl;mbedtls_ssl_config config;mbedtls_x509_crt ca;mbedtls_ctr_drbg_context rng;
    mbedtls_ssl_session saved;unsigned char first_id[32];size_t first_id_len=0;
    mbedtls_memory_buffer_alloc_init(pool,sizeof(pool));mbedtls_ssl_init(&ssl);mbedtls_ssl_config_init(&config);
    mbedtls_x509_crt_init(&ca);mbedtls_ctr_drbg_init(&rng);mbedtls_ssl_session_init(&saved);
    mbedtls_ctr_drbg_set_entropy_len(&rng,48);
    assert(!mbedtls_ctr_drbg_seed(&rng,entropy,NULL,(const unsigned char *)"resume fixture",14));
    assert(!http_builtin_roots_load(&ca));
    mbedtls_ecp_set_max_ops(32);
    int suites[2]={atoi(argv[4]),0};
    unsigned yields[2]={0,0},steps[2]={0,0};uint64_t took[2]={0,0};
    for(unsigned round=0;round<2;++round) {
        SOCKET socket_fd=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);assert(socket_fd!=INVALID_SOCKET);
        assert(!connect(socket_fd,(struct sockaddr *)&remote,sizeof(remote)));
        u_long nonblocking=1;assert(!ioctlsocket(socket_fd,FIONBIO,&nonblocking));
        /* http_service.c builds a new configuration for each connection. */
        mbedtls_ssl_config_free(&config);mbedtls_ssl_config_init(&config);
        assert(!mbedtls_ssl_config_defaults(&config,MBEDTLS_SSL_IS_CLIENT,MBEDTLS_SSL_TRANSPORT_STREAM,MBEDTLS_SSL_PRESET_DEFAULT));
        mbedtls_ssl_conf_authmode(&config,MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&config,&ca,NULL);mbedtls_ssl_conf_rng(&config,mbedtls_ctr_drbg_random,&rng);
        mbedtls_ssl_conf_ciphersuites(&config,suites);
        assert(!mbedtls_ssl_setup(&ssl,&config));assert(!mbedtls_ssl_set_hostname(&ssl,argv[3]));
        if(round)assert(!mbedtls_ssl_set_session(&ssl,&saved));
        mbedtls_ssl_set_bio(&ssl,&socket_fd,send_bytes,receive_bytes,NULL);
        int r=0;uint64_t began=usec(),deadline=began+15000000;
        while(!mbedtls_ssl_is_handshake_over(&ssl)&&usec()<deadline) {
            r=mbedtls_ssl_handshake_step(&ssl);++steps[round];
            if(r==MBEDTLS_ERR_SSL_CRYPTO_IN_PROGRESS){++yields[round];continue;}
            if(r==MBEDTLS_ERR_SSL_WANT_READ||r==MBEDTLS_ERR_SSL_WANT_WRITE){Sleep(1);continue;}
            if(r)break;
        }
        took[round]=usec()-began;
        assert(!r&&mbedtls_ssl_is_handshake_over(&ssl));
        assert(mbedtls_ssl_get_verify_result(&ssl)==0U);
        /* Save the session as http_service.c does: drop the old one first. */
        mbedtls_ssl_session_free(&saved);mbedtls_ssl_session_init(&saved);
        assert(!mbedtls_ssl_get_session(&ssl,&saved));
        assert(saved.id_len>0&&saved.id_len<=sizeof(first_id));
        if(!round){first_id_len=saved.id_len;memcpy(first_id,saved.id,saved.id_len);}
        (void)mbedtls_ssl_close_notify(&ssl);
        mbedtls_ssl_free(&ssl);mbedtls_ssl_init(&ssl);closesocket(socket_fd);
    }
    const int resumed=saved.id_len==first_id_len&&!memcmp(saved.id,first_id,first_id_len)&&!yields[1];
    size_t peak,blocks;mbedtls_memory_buffer_alloc_max_get(&peak,&blocks);
    printf("full: steps=%u yields=%u us=%llu; second: steps=%u yields=%u us=%llu resumed=%d heap=%zu\n",
        steps[0],yields[0],(unsigned long long)took[0],steps[1],yields[1],(unsigned long long)took[1],resumed,peak);
    assert(yields[0]>0);
    mbedtls_ssl_session_free(&saved);mbedtls_ssl_config_free(&config);mbedtls_x509_crt_free(&ca);mbedtls_ctr_drbg_free(&rng);
    assert(!mbedtls_memory_buffer_alloc_verify());mbedtls_memory_buffer_alloc_free();WSACleanup();
    return resumed?0:2;
}
