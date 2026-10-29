/* Exercise real request/redirect code with the network fixture's fake NIC. */
#define main network_fixture_main
#include "http_network_host.c"
#undef main

static const char mobile[] =
    "User-Agent: Mozilla/5.0 (Mobile; APPLE2LYNX/1)\r\n"
    "Sec-CH-UA-Mobile: ?1\r\nSave-Data: on\r\n";
static const char authenticated_mobile[] =
    "User-Agent: Mozilla/5.0 (Mobile; APPLE2LYNX/1)\r\n"
    "Sec-CH-UA-Mobile: ?1\r\nSave-Data: on\r\n"
    "Authorization: Bearer private\r\nCookie: private\r\n";

static void check_wire(int credentials)
{
    int length = http_request_build(&g_http.url, g_http.method, g_http.headers, 0,
                                   g_http.strip_credentials, g_http.request, sizeof(g_http.request));
    assert(length > 0);
    const char *agent = strstr(g_http.request, "User-Agent: ");
    assert(agent && !strstr(agent + 1, "User-Agent: "));
    assert(strstr(g_http.request, mobile) && !strstr(g_http.request, "Appletini-HTTP/1"));
    assert(strstr(g_http.request, "Accept-Encoding: identity\r\n"));
    assert(!!strstr(g_http.request, "private") == credentials);
}

int main(void)
{
    static const unsigned codes[] = {301, 302, 303, 307, 308};
    for (unsigned i = 0; i < sizeof(codes)/sizeof(codes[0]); ++i) {
        setup(HTTP_S_IDLE);
        mbedtls_memory_buffer_alloc_init(tls_heap, sizeof(tls_heap));
        mbedtls_ctr_drbg_init(&g_http.drbg);
        mbedtls_ctr_drbg_set_entropy_len(&g_http.drbg, HTTP_SEED_BYTES);
        memset(g_http.entropy, 0x63, sizeof(g_http.entropy));
        assert(!mbedtls_ctr_drbg_seed(&g_http.drbg, entropy, NULL,
                                     (const uint8_t *)"mobile fixture", 14));
        g_http.crypto_ready = g_http.trust_ready = 1;
        http_service_clock(2026, 10, 29, 12, 0, 0, 1);
        http_request_t request = {"https://start.test/old/page?q=1", "GET",
                                  authenticated_mobile, NULL, 0, 0};
        assert(!http_service_start(&request));
        assert(!strcmp(g_http.status.url, request.url));
        check_wire(1);

        /* The server chooses a mobile URL; the browser does not invent one. */
        g_http.parser.status = (uint16_t)codes[i];
        strcpy(g_http.parser.location, "../mobile/page");
        assert(!redirect());
        assert(!strcmp(g_http.status.url, "https://start.test:443/mobile/page"));
        assert(g_http.url.tls && !g_http.strip_credentials);
        assert(!strcmp(g_http.headers, authenticated_mobile));
        check_wire(1);

        g_http.parser.status = (uint16_t)codes[i];
        strcpy(g_http.parser.location, "https://final.test/phone");
        assert(!redirect());
        assert(!strcmp(g_http.status.url, "https://final.test/phone"));
        assert(g_http.url.tls && g_http.strip_credentials);
        assert(!strcmp(g_http.headers, authenticated_mobile));
        check_wire(0);

        /* The preference never authorizes an HTTPS-to-HTTP downgrade. */
        g_http.parser.status = (uint16_t)codes[i];
        strcpy(g_http.parser.location, "http://final.test/phone");
        assert(redirect() == HTTP_E_REDIRECT);
        assert(!mbedtls_memory_buffer_alloc_verify());
        crypto_reset();
        mbedtls_memory_buffer_alloc_free();
    }
    puts("PASS mobile GET headers: one UA, all five redirect codes, same/cross-origin, credential stripping and HTTPS downgrade rejection");
    return 0;
}
