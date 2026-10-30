/* Extend the existing real HTTP service fixture, rather than replacing its
 * parser, stream ring, state machine or ownership implementation with mocks. */
#define main attachment_fixture_main
#include "http_attachment_service_host.c"
#undef main

static void update_begin(int retain)
{
    begin(retain);
    assert(!http_service_cancel_owned(HTTP_OWNER_NET));
    http_request_t request={"http://192.0.2.1/update","GET","",NULL,0,(uint8_t)retain};
    assert(!http_service_start_owned(&request,HTTP_OWNER_UPDATE));
    g_http.status.state=HTTP_S_READ;
    http_parser_init(&g_http.parser,0,retain?HTTP_RETAIN_MAX:UINT32_MAX);
}
int main(void)
{
    assert(!attachment_fixture_main());
    uint8_t bytes[16];size_t length;
    http_request_t other={"http://192.0.2.2/other","GET","",NULL,0,0};
    update_begin(0);header(200,"",5);append("image",5);finish_body();
    assert(http_service_getowner()==HTTP_OWNER_UPDATE);
    assert(http_service_start(&other)==HTTP_E_BUSY);
    assert(http_service_start_owned(&other,HTTP_OWNER_NET)==HTTP_E_BUSY);
    assert(http_service_start_owned(&other,HTTP_OWNER_UPDATE)==HTTP_E_BUSY);
    assert(http_service_cancel_owned(HTTP_OWNER_NET)==HTTP_E_BUSY);
    assert(http_service_cancel_owned(HTTP_OWNER_DOWNLOAD)==HTTP_E_BUSY);
    http_service_cancel();assert(http_service_getowner()==HTTP_OWNER_UPDATE);
    assert(!http_service_read(bytes,sizeof bytes));
    assert(!http_service_read_owned(HTTP_OWNER_NET,bytes,sizeof bytes));
    assert(http_service_handoff_download()==HTTP_E_ARGUMENT);
    assert(http_service_release_owned(HTTP_OWNER_UPDATE)==HTTP_E_BUSY);
    assert(http_service_read_owned(HTTP_OWNER_UPDATE,bytes,sizeof bytes)==5&&!memcmp(bytes,"image",5));
    assert(!http_service_release_owned(HTTP_OWNER_UPDATE));
    assert(http_service_getowner()==HTTP_OWNER_NONE);

    update_begin(1);header(200,"",2);append("{}",2);finish_body();
    assert(!http_service_retained(&length)&&!length);
    assert(!http_service_retained_owned(HTTP_OWNER_NET,&length)&&!length);
    const uint8_t *json=http_service_retained_owned(HTTP_OWNER_UPDATE,&length);
    assert(json&&length==2&&!memcmp(json,"{}",2));
    assert(!http_service_cancel_owned(HTTP_OWNER_UPDATE));
    assert(http_service_getowner()==HTTP_OWNER_NONE);

    update_begin(0);header(200,"",100);poll_until_headers();
    uint64_t deadline=g_http.download_idle;
    assert(deadline==host_time+HTTP_DOWNLOAD_IDLE_US);
    host_time=deadline;http_service_poll();
    assert(g_http.status.result==HTTP_E_TIMEOUT&&http_service_getowner()==HTTP_OWNER_UPDATE);
    assert(!http_service_cancel_owned(HTTP_OWNER_UPDATE));
    begin(0);unsigned old_owner=http_service_getowner();
    assert(http_service_start_owned(&other,HTTP_OWNER_UPDATE)==HTTP_E_BUSY&&http_service_getowner()==old_owner);
    assert(http_service_cancel_owned(HTTP_OWNER_UPDATE)==HTTP_E_BUSY);
    assert(!http_service_cancel_owned(HTTP_OWNER_NET));
    puts("PASS real UPDATE HTTP ownership: stream+retained isolation, active/DONE busy, timeout cleanup, no NET/download theft");
    return 0;
}
