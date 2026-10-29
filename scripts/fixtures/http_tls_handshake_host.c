#define _WIN32_WINNT 0x0601
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

static unsigned char pool[512U*1024U] __attribute__((aligned(32))),pem[65568];
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
    assert(argc==7);
    WSADATA ws;assert(!WSAStartup(MAKEWORD(2,2),&ws));
    SOCKET socket_fd=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);assert(socket_fd!=INVALID_SOCKET);
    struct sockaddr_in remote={0};remote.sin_family=AF_INET;remote.sin_port=htons((unsigned short)atoi(argv[2]));
    assert(inet_pton(AF_INET,argv[1],&remote.sin_addr)==1);
    assert(!connect(socket_fd,(struct sockaddr *)&remote,sizeof(remote)));
    u_long nonblocking=1;assert(!ioctlsocket(socket_fd,FIONBIO,&nonblocking));
    mbedtls_ssl_context ssl;mbedtls_ssl_config config;mbedtls_x509_crt ca;mbedtls_ctr_drbg_context rng;
    mbedtls_memory_buffer_alloc_init(pool,sizeof(pool));mbedtls_ssl_init(&ssl);mbedtls_ssl_config_init(&config);
    mbedtls_x509_crt_init(&ca);mbedtls_ctr_drbg_init(&rng);mbedtls_ctr_drbg_set_entropy_len(&rng,48);
    assert(!mbedtls_ctr_drbg_seed(&rng,entropy,NULL,(const unsigned char *)"handshake fixture",17));
    if(!strcmp(argv[4],"embedded")) {
        assert(!http_builtin_roots_load(&ca));
    } else {
        FILE *file=fopen(argv[4],"rb");assert(file);size_t n=fread(pem,1,sizeof(pem)-1,file);assert(feof(file));fclose(file);pem[n]=0;
        assert(!mbedtls_x509_crt_parse(&ca,pem,n+1));
    }
    assert(!mbedtls_ssl_config_defaults(&config,MBEDTLS_SSL_IS_CLIENT,MBEDTLS_SSL_TRANSPORT_STREAM,MBEDTLS_SSL_PRESET_DEFAULT));
    int suites[2]={atoi(argv[6]),0};
    mbedtls_ssl_conf_ciphersuites(&config,suites);mbedtls_ssl_conf_authmode(&config,MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&config,&ca,NULL);mbedtls_ssl_conf_rng(&config,mbedtls_ctr_drbg_random,&rng);
    assert(!mbedtls_ssl_setup(&ssl,&config));assert(!mbedtls_ssl_set_hostname(&ssl,argv[3]));
    mbedtls_ssl_set_bio(&ssl,&socket_fd,send_bytes,receive_bytes,NULL);mbedtls_ecp_set_max_ops(32);
    int r=0;unsigned yields=0,steps=0;uint64_t max_step=0,deadline=usec()+15000000;
    while(!mbedtls_ssl_is_handshake_over(&ssl)&&usec()<deadline) {
        uint64_t at=usec();r=mbedtls_ssl_handshake_step(&ssl);uint64_t elapsed=usec()-at;
        if(elapsed>max_step)max_step=elapsed;++steps;
        if(r==MBEDTLS_ERR_SSL_CRYPTO_IN_PROGRESS){++yields;continue;}
        if(r==MBEDTLS_ERR_SSL_WANT_READ||r==MBEDTLS_ERR_SSL_WANT_WRITE){Sleep(1);continue;}
        if(r)break;
    }
    uint32_t flags=mbedtls_ssl_get_verify_result(&ssl),expected=(uint32_t)strtoul(argv[5],NULL,0);
    size_t peak,blocks;mbedtls_memory_buffer_alloc_max_get(&peak,&blocks);
    printf("suite=%s rc=%d verify=%08lx yields=%u steps=%u max_step_us=%llu heap=%zu/%zu\n",
        argv[6],r,(unsigned long)flags,yields,steps,(unsigned long long)max_step,peak,sizeof(pool));
    if(!expected)assert(!r&&!flags&&mbedtls_ssl_is_handshake_over(&ssl)&&yields>0);
    else assert(r<0&&(flags&expected));
    assert(peak<sizeof(pool));
    mbedtls_ssl_free(&ssl);mbedtls_ssl_config_free(&config);mbedtls_x509_crt_free(&ca);mbedtls_ctr_drbg_free(&rng);
    assert(!mbedtls_memory_buffer_alloc_verify());mbedtls_memory_buffer_alloc_free();closesocket(socket_fd);WSACleanup();
    return 0;
}
