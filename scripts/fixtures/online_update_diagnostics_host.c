/* Extend the lifecycle's adapters; production core/service/SHA/manifest remain
 * separate linked translation units. The network mock deliberately destroys
 * its status on cancellation, so preserved diagnostics cannot be accidental. */
#define main lifecycle_fixture_main
#include "online_update_service_host.c"
#undef main

static void diagnostic_expect(const online_update_snapshot_t *s,const char *stage,
                              int primary,unsigned code,unsigned verify,int http_rc,
                              int fatfs,int validation,int clean)
{
    if(strcmp(s->stage,stage)||s->result!=primary||s->http_status!=code||
       s->tls_verify!=verify||s->http_result!=http_rc||s->fatfs_result!=fatfs||
       s->validation_result!=validation||s->cleanup_result!=clean) {
        fprintf(stderr,"got stage=%s primary=%d HTTP=%u verify=%08x httpRC=%d FR=%d MB=%d cleanup=%d detail=%s\n",
                s->stage,s->result,s->http_status,(unsigned)s->tls_verify,s->http_result,
                s->fatfs_result,s->validation_result,s->cleanup_result,s->detail);
        assert(!"diagnostic mismatch");
    }
    assert(s->detail[0]&&strcmp(s->detail,"Online update failed"));
}
int main(int argc,char **argv)
{
    assert(argc==16);
    scenario="diagnostic";
    load(&documents[0],argv[2]);load(&documents[1],argv[3]);load(&documents[2],argv[4]);
    const char *mode=argv[5],*stage=argv[9];
    int source_rc=atoi(argv[6]),primary=atoi(argv[10]),fr=atoi(argv[11]),mb=atoi(argv[12]);
    int clean=atoi(argv[13]),expected_http_rc=atoi(argv[14]);
    unsigned code=(unsigned)strtoul(argv[7],NULL,0),verify=(unsigned)strtoul(argv[8],NULL,0);
    int use_image=!strncmp(mode,"image",5)||!strncmp(mode,"sd-",3);
    int foreign=!strcmp(mode,"foreign-owner");
    int dual_cleanup=!strcmp(mode,"image-write-close")||!strcmp(mode,"sd-sync-unlink")||!strcmp(mode,"sd-close");
    poison_cancel_status=1;
    if(!strncmp(mode,"api",3))inject_source=0;
    else if(!strncmp(mode,"boot",4))inject_source=2;
    if(strstr(mode,"start"))inject_start_rc=source_rc;
    else if(strstr(mode,"format"))inject_old_request=1;
    else if(inject_source>=0) {inject_transport_rc=source_rc;inject_http_status=code;inject_verify=verify;}
    /* Leftover previous status must not leak through a failed start. */
    status_http.status=418;status_http.result=-9999;status_http.verify=0x81234567U;
    if(foreign) {occupied=1;owner=HTTP_OWNER_NET;}
    online_update_init("F1.3.3","B2.0.5");step(1,0);
    if(foreign) {
        assert(!online_update_active()&&owner==HTTP_OWNER_NET&&!cancels);
        online_update_snapshot_t s=snapshot();diagnostic_expect(&s,stage,primary,code,verify,expected_http_rc,fr,mb,clean);
        assert(s.http_status!=418&&s.tls_verify!=0x81234567U);
        puts("PASS diagnostic foreign-owner data isolation");return 0;
    }
    terminal();
    if(use_image) {
        assert(snapshot().state==ONLINE_UPDATE_AVAILABLE);
        inject_source=1;inject_http_status=code;inject_verify=verify;
        if(strstr(mode,"start"))inject_start_rc=source_rc;
        else if(strstr(mode,"format"))inject_old_request=1;
        else if(!strcmp(mode,"image-response"))inject_transport_rc=source_rc;
        if(!strcmp(mode,"sd-stage"))readonly_sd=1;
        if(!strcmp(mode,"image-write-close"))write_error=close_error=1;
        if(!strcmp(mode,"sd-sync-unlink"))sync_error=unlink_error=1;
        if(!strcmp(mode,"sd-close"))close_error=1;
        assert(!online_update_start(ONLINE_UPDATE_FIRMWARE));
        if(dual_cleanup) {
            for(unsigned i=0;i<20000&&snapshot().state!=ONLINE_UPDATE_FAILED;++i)step(1,0);
            assert(online_update_active()&&owner==HTTP_OWNER_NONE);
            online_update_snapshot_t before=snapshot();
            diagnostic_expect(&before,stage,primary,code,verify,expected_http_rc,fr,mb,clean);
            write_error=sync_error=close_error=unlink_error=0;terminal();
            online_update_snapshot_t after=snapshot();
            diagnostic_expect(&after,stage,primary,code,verify,expected_http_rc,fr,mb,clean);
            assert(!strcmp(before.stage,after.stage)&&before.result==after.result);
        } else terminal();
    }
    online_update_snapshot_t s=snapshot();
    assert(s.state==ONLINE_UPDATE_FAILED&&!handles&&!occupied&&!renames);
    diagnostic_expect(&s,stage,primary,code,verify,expected_http_rc,fr,mb,clean);
    if(!strncmp(mode,"boot",4))assert(s.available_mask==1&&!strcmp(s.latest_f,"F1.3.4"));
    if(!strncmp(mode,"api",3)||!strcmp(mode,"metadata"))assert(!s.available_mask);
    assert(!*online_update_ready_path()&&find("0:/FIRMWARE.BIN")<0&&find("0:/FIRMWARE.BIN.part")<0);
    if(strcmp(argv[15],"-"))assert(strstr(s.detail,argv[15]));
    printf("PASS diagnostic %s: %s rc=%d HTTP=%u TLS=%08x FR=%d MB=%d cleanup=%d | %s\n",
           argv[1],s.stage,s.result,s.http_status,(unsigned)s.tls_verify,s.fatfs_result,
           s.validation_result,s.cleanup_result,s.detail);
    return 0;
}
