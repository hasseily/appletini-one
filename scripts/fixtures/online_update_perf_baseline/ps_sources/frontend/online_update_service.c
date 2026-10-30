#if defined(APPLETINI_HTTP_SERVICE) || defined(ONLINE_UPDATE_HOST)
#include "online_update_service.h"
#include "http_service.h"
#include "../lib/multiboot.h"
#include "ff.h"
#include "mbedtls/sha256.h"
#include "mbedtls/x509.h"
#include <stdio.h>
#include <string.h>

#define UPDATE_COPY_BYTES 4096U
enum { P_NONE, P_API_START, P_API_READ, P_API_PARSE, P_BOOT_START,
       P_BOOT_READ, P_BOOT_VERIFY, P_STAGE_OPEN, P_ASSET_START,
       P_ASSET_READ, P_STAGE_VERIFY, P_STAGE_CLOSE, P_STAGE_RENAME };
static struct {
    online_update_snapshot_t status;
    online_update_core_t core;
    online_update_release_t release;
    FIL file;
    mbedtls_sha256_context hash;
    uint8_t phase, open, owns_part, http_owned, hash_active, cancel, cleanup, auto_pending;
    uint8_t origin_rejected;
    char final[24], part[32];
    uint8_t copy[UPDATE_COPY_BYTES];
    uint8_t boot[MB_GOLDEN_SIZE];
} update;
static FATFS update_fs;
static const char api_headers[] =
    "User-Agent: Appletini-Updater\r\nAccept: application/vnd.github+json\r\n"
    "X-GitHub-Api-Version: 2022-11-28\r\n";
static const char asset_headers[] =
    "User-Agent: Appletini-Updater\r\nAccept: application/octet-stream\r\n";

static void detail(const char *text)
{ (void)snprintf(update.status.detail,sizeof update.status.detail,"%s",text); }
static void stage(void)
{
    static const char *const labels[]={"Idle","Release request","Release response",
        "Release metadata","Boot request","Boot download","Boot validation",
        "SD staging","Image request","Image download","Image validation",
        "SD close","SD rename"};
    (void)snprintf(update.status.stage,sizeof update.status.stage,"%s",
        update.phase<sizeof labels/sizeof labels[0]?labels[update.phase]:"Unknown stage");
}
static void transport_reset(void)
{
    update.status.http_status=0; update.status.http_result=0;
    update.status.tls_verify=0; update.origin_rejected=0;
}
static void diagnostics_reset(void)
{
    update.status.result=0;
    update.status.fatfs_result=update.status.validation_result=update.status.cleanup_result=0;
    transport_reset(); update.status.stage[0]=0;
}
static void transport_record(const http_status_t *source)
{
    if (!update.http_owned || http_service_getowner()!=HTTP_OWNER_UPDATE) return;
    update.status.http_status=source->status;
    update.status.http_result=source->result;
    update.status.tls_verify=source->verify;
}
static int storage_error(FRESULT result,int error)
{
    update.status.fatfs_result=(int)result;
    return error;
}
static void failure_detail(const char *fallback)
{
    const int result=update.status.result;
    const char *reason=NULL;
    switch (result) {
    case HTTP_E_BUSY: reason="Network busy or ownership lost; retry";break;
    case HTTP_E_ARGUMENT: reason="HTTP request setup invalid";break;
    case HTTP_E_IO: reason="Ethernet I/O failed; check network";break;
    case HTTP_E_TIME: reason="HTTPS clock invalid; set RTC and timezone";break;
    case HTTP_E_SEED: reason="TLS seed/entropy unavailable; restart";break;
    case HTTP_E_TRUST: reason="HTTPS CA roots unavailable; restart/check setup";break;
    case HTTP_E_DNS: reason="DNS failed; check DNS server and gateway";break;
    case HTTP_E_TIMEOUT: reason="Request timed out; check network and retry";break;
    case HTTP_E_CANCELLED: reason="HTTP request cancelled";break;
    case HTTP_E_REDIRECT: reason="HTTPS redirect rejected or limit reached";break;
    case HTTP_E_STORAGE_BUSY: reason="Finish SD sharing before updating";break;
    case HTTP_E_NETWORK_NOT_READY: reason="Ethernet not ready; check cable/DHCP";break;
    case HTTP_FORMAT: reason="HTTP request/framing invalid";break;
    case HTTP_LIMIT: reason="HTTP response exceeded transport limit";break;
    case HTTP_UNSUPPORTED: reason="HTTP framing/encoding unsupported";break;
    case ONLINE_UPDATE_E_JSON: reason="Release JSON is empty or malformed";break;
    case ONLINE_UPDATE_E_RELEASE: reason="Release is not valid published stable F metadata";break;
    case ONLINE_UPDATE_E_ASSET: reason="Release asset size/SHA256/URL metadata invalid";break;
    case ONLINE_UPDATE_E_LENGTH:
        reason=!strncmp(update.status.stage,"Release",7)?
            "Release JSON exceeds 32 KiB limit":"Download size differs from release";break;
    case ONLINE_UPDATE_E_DIGEST: reason=update.status.validation_result?
        "SHA256 processing failed":"SHA256 mismatch; retry/check release asset";break;
    case ONLINE_UPDATE_E_IMAGE:
        switch (update.status.validation_result) {
        case MB_E_READ: reason="Downloaded image cannot be read completely";break;
        case MB_E_IMAGE: reason="Downloaded Bootgen image/layout invalid";break;
        case MB_E_MANIFEST: reason="Downloaded image manifest/role/product invalid";break;
        case MB_E_CRC: reason="Downloaded image CRC mismatch; retry";break;
        case MB_E_MARKER: reason="Downloaded image identity/version marker invalid";break;
        default: reason="Downloaded image validation failed";break;
        }
        break;
    case ONLINE_UPDATE_E_VERSION: reason="Image version differs from published release";break;
    case ONLINE_UPDATE_E_STORAGE: reason="SD access failed; check card and free space";break;
    case ONLINE_UPDATE_E_CONFLICT: reason="Existing BIN/.part file; move or remove it first";break;
    case ONLINE_UPDATE_E_WRITE: reason=update.status.fatfs_result?
        "SD write failed; check card/free space":"Short SD write; check free space";break;
    case ONLINE_UPDATE_E_SYNC: reason="SD sync failed; check card/free space";break;
    case ONLINE_UPDATE_E_CLOSE: reason="SD close failed; check card";break;
    case ONLINE_UPDATE_E_RENAME: reason="SD rename failed; check card/root file";break;
    case ONLINE_UPDATE_E_UNSAFE: reason="Update cancelled by storage or menu activity";break;
    case ONLINE_UPDATE_E_CANCELLED: reason="Update cancelled";break;
    case ONLINE_UPDATE_E_ARGUMENT: reason="Update request argument invalid";break;
    default: break;
    }
    if (result==ONLINE_UPDATE_E_HTTP) {
        const unsigned status=update.status.http_status;
        if (update.origin_rejected) reason="HTTPS GitHub response origin rejected";
        else if (status==403U) reason="GitHub HTTP 403: access denied or rate limit; retry later";
        else if (status==429U) reason="GitHub HTTP 429: rate limited; retry later";
        else if (status==404U) reason="GitHub HTTP 404: release/asset not found";
        else if (status>=500U) reason="GitHub server error; retry later";
        else if (status>=300U && status<400U) reason="GitHub redirect did not resolve";
        else reason="GitHub HTTP response rejected";
        (void)snprintf(update.status.detail,sizeof update.status.detail,"%s (HTTP %u, %d)",reason,status,result);
    } else if (reason) {
        if (update.status.fatfs_result)
            (void)snprintf(update.status.detail,sizeof update.status.detail,"%s (%d, FR=%d)",reason,result,update.status.fatfs_result);
        else if (update.status.validation_result)
            (void)snprintf(update.status.detail,sizeof update.status.detail,"%s (%d, native=%d)",reason,result,update.status.validation_result);
        else (void)snprintf(update.status.detail,sizeof update.status.detail,"%s (%d)",reason,result);
    } else {
        const uint32_t flags=update.status.tls_verify;
        const char *tls="TLS handshake failed";
        if (flags && flags!=UINT32_MAX) {
            if (flags&MBEDTLS_X509_BADCERT_REVOKED) tls="TLS certificate revoked";
            else if (flags&MBEDTLS_X509_BADCERT_EXPIRED) tls="TLS certificate expired; check RTC";
            else if (flags&MBEDTLS_X509_BADCERT_FUTURE) tls="TLS certificate future; check RTC";
            else if (flags&MBEDTLS_X509_BADCERT_CN_MISMATCH) tls="TLS certificate hostname mismatch";
            else if (flags&MBEDTLS_X509_BADCERT_NOT_TRUSTED) tls="TLS certificate issuer untrusted";
            else if (flags&(MBEDTLS_X509_BADCERT_BAD_MD|MBEDTLS_X509_BADCERT_BAD_PK|MBEDTLS_X509_BADCERT_BAD_KEY))
                tls="TLS certificate algorithm rejected";
        }
        (void)fallback;
        (void)snprintf(update.status.detail,sizeof update.status.detail,
            "%s -0x%04lX (%d), verify=0x%08lX",tls,
            (unsigned long)(-(int64_t)result),result,(unsigned long)flags);
    }
}
static void drop_http(void)
{
    if (update.http_owned && http_service_getowner()==HTTP_OWNER_UPDATE) {
        http_status_t source;
        http_service_status(&source); transport_record(&source);
        (void)http_service_cancel_owned(HTTP_OWNER_UPDATE);
    }
    update.http_owned=0;
}
static void hash_end(void)
{
    if (update.hash_active) mbedtls_sha256_free(&update.hash);
    update.hash_active=0;
}
static int cleanup(void)
{
    int failed=0;
    drop_http(); hash_end();
    if (update.open) {
        FRESULT result=f_close(&update.file);
        if (result==FR_OK || result==FR_INVALID_OBJECT) update.open=0;
        else {failed=1;if(!update.status.cleanup_result)update.status.cleanup_result=(int)result;}
    }
    if (update.owns_part && !update.open) {
        FRESULT result=f_unlink(update.part);
        if (result==FR_OK || result==FR_NO_FILE) update.owns_part=0;
        else {failed=1;if(!update.status.cleanup_result)update.status.cleanup_result=(int)result;}
    }
    update.cleanup=(uint8_t)(failed || update.open || update.owns_part);
    return update.cleanup?ONLINE_UPDATE_E_CLEANUP:0;
}
static void fail(int result,const char *text)
{
    update.status.state=ONLINE_UPDATE_FAILED;
    update.status.result=result;
    update.phase=P_NONE;
    (void)cleanup();
    failure_detail(text);
}
int online_update_active(void)
{
    return update.phase!=P_NONE || update.open || update.owns_part || update.cleanup;
}
void online_update_status(online_update_snapshot_t *out)
{ if(out)*out=update.status; }
const char *online_update_ready_path(void)
{ return update.status.state==ONLINE_UPDATE_READY?update.final:""; }
static void installed(char *out,const char *text)
{
    if (!text || strlen(text)>=ONLINE_UPDATE_VERSION_BYTES) out[0]=0;
    else strcpy(out,text);
}
void online_update_init(const char *firmware,const char *boot)
{
    /* Initialization cannot discard an open transaction or its cleanup record. */
    if (online_update_active()) return;
    memset(&update,0,sizeof update);
    installed(update.status.installed_f,firmware);
    installed(update.status.installed_b,boot);
    update.auto_pending=1;
    detail("Update check waits for the configuration menu");
}
int online_update_check(void)
{
    if (online_update_active()) return HTTP_E_BUSY;
    update.auto_pending=0;
    memset(&update.release,0,sizeof update.release);
    update.status.available_mask=0;
    update.status.latest_f[0]=update.status.latest_b[0]=0;
    update.status.firmware_bytes=update.status.boot_bytes=0;
    update.status.received=update.status.total=0;
    diagnostics_reset();
    update.status.kind=ONLINE_UPDATE_FIRMWARE;
    update.status.state=ONLINE_UPDATE_CHECKING;
    update.cancel=0; update.phase=P_API_START;
    stage();
    detail("Checking published release");
    return 0;
}
int online_update_start(uint8_t kind)
{
    if (online_update_active()) return HTTP_E_BUSY;
    if (kind>=ONLINE_UPDATE_KIND_COUNT || !(update.status.available_mask&(1U<<kind)) ||
        !update.release.assets[kind].present) return ONLINE_UPDATE_E_ARGUMENT;
    update.status.kind=kind; update.status.state=ONLINE_UPDATE_DOWNLOADING;
    diagnostics_reset(); update.status.received=0;
    update.status.total=update.release.assets[kind].size;
    (void)snprintf(update.final,sizeof update.final,"0:/%s",online_update_filename(kind));
    (void)snprintf(update.part,sizeof update.part,"%s.part",update.final);
    update.cancel=0; update.phase=P_STAGE_OPEN;
    stage();
    detail("Preparing update file");
    return 0;
}
void online_update_cancel(void)
{
    update.auto_pending=0;
    if (online_update_active()) update.cancel=1;
    else if (update.status.state==ONLINE_UPDATE_READY) {
        /* A complete validated file belongs to the user once renamed. */
        update.status.state=ONLINE_UPDATE_CANCELLED;
        update.status.result=ONLINE_UPDATE_E_CANCELLED;
        detail("Update cancelled; downloaded file kept");
    }
}
static int start_http(const char *url,int retain)
{
    http_request_t request={url,"GET",retain?api_headers:asset_headers,NULL,0,(uint8_t)retain};
    transport_reset();
    int result=http_service_start_owned(&request,HTTP_OWNER_UPDATE);
    if (!result) update.http_owned=1;
    else update.status.http_result=result;
    return result;
}
static int source_status(http_status_t *source,int api)
{
    http_url_t url;
    if (!update.http_owned || http_service_getowner()!=HTTP_OWNER_UPDATE) return HTTP_E_BUSY;
    http_service_status(source);
    transport_record(source);
    if (source->state==HTTP_S_FAILED) return source->result?source->result:ONLINE_UPDATE_E_HTTP;
    if (http_url_parse(source->url,&url) || !url.tls || url.port!=443U) {
        update.origin_rejected=1;
        return ONLINE_UPDATE_E_HTTP;
    }
    if (api) {
        if (strcmp(url.host,"api.github.com")) {update.origin_rejected=1;return ONLINE_UPDATE_E_HTTP;}
    } else if (strcmp(url.host,"github.com") &&
               strcmp(url.host,"release-assets.githubusercontent.com") &&
               strcmp(url.host,"objects.githubusercontent.com")) {
        update.origin_rejected=1;return ONLINE_UPDATE_E_HTTP;
    }
    /* Intermediate redirect status is not a final response. Never consume it. */
    if (source->headers_ready && (source->status<200U || source->status>=300U)) {
        if (source->status>=300U && source->status<400U && source->state!=HTTP_S_DONE) return 0;
        return ONLINE_UPDATE_E_HTTP;
    }
    if (source->state==HTTP_S_DONE && (!source->headers_ready ||
        source->status<200U || source->status>=300U)) return ONLINE_UPDATE_E_HTTP;
    return source->headers_ready && source->status>=200U && source->status<300U?1:0;
}
static int hash_start(void)
{
    hash_end(); mbedtls_sha256_init(&update.hash); update.hash_active=1;
    int result=mbedtls_sha256_starts(&update.hash,0);
    if(result)update.status.validation_result=result;
    return result?ONLINE_UPDATE_E_DIGEST:0;
}
static int hash_finish(const online_update_asset_t *asset)
{
    uint8_t digest[32];
    int result=mbedtls_sha256_finish(&update.hash,digest);
    if(result)update.status.validation_result=result;
    hash_end();
    return result || memcmp(digest,asset->sha256,sizeof digest)?ONLINE_UPDATE_E_DIGEST:0;
}
static int boot_read(void *context,uint32_t offset,void *out,uint32_t length)
{
    (void)context;
    if (offset>update.status.received || length>update.status.received-offset) return -1;
    memcpy(out,update.boot+offset,length);return 0;
}
static int file_read(void *context,uint32_t offset,void *out,uint32_t length)
{
    FIL *file=(FIL *)context;
    UINT got=0;
    if (offset>update.status.received || length>update.status.received-offset) return -1;
    FRESULT result=f_lseek(file,offset);
    if(result!=FR_OK) {update.status.fatfs_result=(int)result;return -1;}
    result=f_read(file,out,(UINT)length,&got);
    if(result!=FR_OK || got!=length) {update.status.fatfs_result=(int)result;return -1;}
    return 0;
}
static void available(void)
{
    int comparison;
    update.status.available_mask=0;
    if (update.release.assets[ONLINE_UPDATE_FIRMWARE].present &&
        !online_update_version_compare(update.status.installed_f,update.status.latest_f,'F',&comparison) && comparison>0)
        update.status.available_mask|=1U<<ONLINE_UPDATE_FIRMWARE;
    if (update.release.assets[ONLINE_UPDATE_BOOT].present &&
        !online_update_version_compare(update.status.installed_b,update.status.latest_b,'B',&comparison) && comparison>0)
        update.status.available_mask|=1U<<ONLINE_UPDATE_BOOT;
    update.status.state=update.status.available_mask?ONLINE_UPDATE_AVAILABLE:ONLINE_UPDATE_CURRENT;
    update.status.received=update.status.total=0; update.status.result=0;
    update.phase=P_NONE;
    (void)snprintf(update.status.stage,sizeof update.status.stage,"Check complete");
    detail(update.status.available_mask?"Published update available":"No newer compatible update");
}
static int stage_open(void)
{
    FILINFO info;
    FRESULT result=f_stat(update.final,&info);
    if (result==FR_NOT_ENABLED) {
        result=f_mount(&update_fs,"0:/",1);
        if (result!=FR_OK) return storage_error(result,ONLINE_UPDATE_E_STORAGE);
        result=f_stat(update.final,&info);
    }
    if (result==FR_OK) return ONLINE_UPDATE_E_CONFLICT;
    if (result!=FR_NO_FILE) return storage_error(result,ONLINE_UPDATE_E_STORAGE);
    result=f_stat(update.part,&info);
    if (result==FR_OK) return ONLINE_UPDATE_E_CONFLICT;
    if (result!=FR_NO_FILE) return storage_error(result,ONLINE_UPDATE_E_STORAGE);
    result=f_open(&update.file,update.part,FA_READ|FA_WRITE|FA_CREATE_NEW);
    if (result!=FR_OK) return storage_error(result,result==FR_EXIST?ONLINE_UPDATE_E_CONFLICT:ONLINE_UPDATE_E_STORAGE);
    update.open=update.owns_part=1;
    return 0;
}
static void receive_asset(int discover)
{
    const online_update_asset_t *asset=&update.release.assets[
        discover?ONLINE_UPDATE_BOOT:update.status.kind];
    http_status_t source;
    int ready=source_status(&source,0);
    if (ready<0) {fail(ready,"Update HTTP request failed");return;}
    if (!ready) return;
    if (source.content_length_known && source.content_length!=asset->size) {
        fail(ONLINE_UPDATE_E_LENGTH,"Update size differs from release");return;
    }
    size_t n=http_service_read_owned(HTTP_OWNER_UPDATE,update.copy,sizeof update.copy);
    if (n) {
        if (update.status.received>asset->size || n>asset->size-update.status.received) {
            fail(ONLINE_UPDATE_E_LENGTH,"Update exceeds release size");return;
        }
        if (discover) memcpy(update.boot+update.status.received,update.copy,n);
        else {
            UINT wrote=0;
            FRESULT written=f_write(&update.file,update.copy,(UINT)n,&wrote);
            if (written!=FR_OK || wrote!=n) {
                update.status.fatfs_result=(int)written;
                fail(ONLINE_UPDATE_E_WRITE,"Update file write failed");return;
            }
        }
        int hashed=mbedtls_sha256_update(&update.hash,update.copy,n);
        if (hashed) {
            update.status.validation_result=hashed;
            fail(ONLINE_UPDATE_E_DIGEST,"Update SHA256 failed");return;
        }
        update.status.received+=(uint32_t)n;
    }
    http_service_status(&source);
    transport_record(&source);
    if (source.state==HTTP_S_FAILED) {fail(source.result,"Update HTTP request failed");return;}
    if (source.state==HTTP_S_DONE && !source.buffered) {
        if (source.status<200U || source.status>=300U || update.status.received!=asset->size) {
            fail(ONLINE_UPDATE_E_LENGTH,"Incomplete update download");return;
        }
        int result=hash_finish(asset);
        drop_http();
        if (result) {fail(result,"Update SHA256 does not match release");return;}
        update.phase=discover?P_BOOT_VERIFY:P_STAGE_VERIFY;
        if (!discover) update.status.state=ONLINE_UPDATE_VERIFYING;
        detail(discover?"Checking boot image identity":"Verifying downloaded image");
    }
}
void online_update_poll(uint8_t allowed,uint8_t busy)
{
    if (update.auto_pending && allowed && !busy) (void)online_update_check();
    if (!online_update_active()) return;
    /* Retrying cleanup must preserve the first error, even when the caller
     * remains busy or has already withdrawn menu permission. */
    if (update.cleanup && update.phase==P_NONE) { (void)cleanup();return; }
    if (update.phase!=P_NONE) stage();
    if (!allowed || busy) {
        /* Handover callers retain the mounted volume until active() clears.
         * Only close/unlink our own partial file after permission is lost. */
        fail(ONLINE_UPDATE_E_UNSAFE,"Update cancelled by storage or menu activity");
        return;
    }
    if (update.cancel) {
        update.cancel=0; update.phase=P_NONE;
        update.status.state=ONLINE_UPDATE_CANCELLED;
        update.status.result=ONLINE_UPDATE_E_CANCELLED;
        if (cleanup()) update.status.state=ONLINE_UPDATE_FAILED;
        failure_detail("Update cancelled");
        return;
    }
    if (update.cleanup) { (void)cleanup();return; }
    int result=0;
    switch (update.phase) {
    case P_API_START:
        result=start_http(ONLINE_UPDATE_API_URL,1);
        if (!result) update.phase=P_API_READ;
        break;
    case P_API_READ: {
        http_status_t source;
        int ready=source_status(&source,1);
        if (ready<0) {result=ready;break;}
        if (!ready) break;
        if (source.content_length_known && source.content_length>ONLINE_UPDATE_JSON_BYTES) {
            result=ONLINE_UPDATE_E_LENGTH;break;
        }
        if (source.state==HTTP_S_DONE) {
            size_t length=0;
            const uint8_t *body=http_service_retained_owned(HTTP_OWNER_UPDATE,&length);
            if (!body || !length || length>ONLINE_UPDATE_JSON_BYTES) {result=ONLINE_UPDATE_E_JSON;break;}
            update.status.received=update.status.total=(uint32_t)length;
            online_update_core_init(&update.core,body);update.phase=P_API_PARSE;
        }
        break;
    }
    case P_API_PARSE:
        if (http_service_getowner()!=HTTP_OWNER_UPDATE) {result=HTTP_E_BUSY;break;}
        result=online_update_core_feed(&update.core,update.status.total,1,256U);
        if (result==1) {
            update.release=update.core.release;
            if (update.release.assets[ONLINE_UPDATE_FIRMWARE].present) {
                strcpy(update.status.latest_f,update.release.tag);
                update.status.firmware_bytes=update.release.assets[ONLINE_UPDATE_FIRMWARE].size;
                int comparison;
                if (!online_update_version_compare(update.status.installed_f,
                    update.status.latest_f,'F',&comparison) && comparison>0)
                    update.status.available_mask|=1U<<ONLINE_UPDATE_FIRMWARE;
            }
            update.status.boot_bytes=update.release.assets[ONLINE_UPDATE_BOOT].size;
            drop_http();
            if (update.release.assets[ONLINE_UPDATE_BOOT].present) {
                update.phase=P_BOOT_START;update.status.kind=ONLINE_UPDATE_BOOT;
                update.status.received=0;update.status.total=update.status.boot_bytes;
                detail("Checking actual boot image version");
            } else available();
            result=0;
        }
        break;
    case P_BOOT_START:
        transport_reset();
        result=hash_start();
        if (!result) result=start_http(update.release.assets[ONLINE_UPDATE_BOOT].url,0);
        if (!result) update.phase=P_BOOT_READ;
        break;
    case P_BOOT_READ: receive_asset(1);return;
    case P_BOOT_VERIFY: {
        mb_image_t image;
        mb_reader_t reader={NULL,boot_read,update.status.received};
        update.status.validation_result=mb_update_image_validate(&reader,1,&image,update.status.latest_b,
                                     sizeof update.status.latest_b);
        if (update.status.validation_result!=MB_OK) {
            update.status.latest_b[0]=0;result=ONLINE_UPDATE_E_IMAGE;break;
        }
        available();break;
    }
    case P_STAGE_OPEN:
        result=stage_open();
        if (!result) update.phase=P_ASSET_START;
        break;
    case P_ASSET_START:
        transport_reset();
        result=hash_start();
        if (!result) result=start_http(update.release.assets[update.status.kind].url,0);
        if (!result) update.phase=P_ASSET_READ;
        if (!result) detail("Downloading update; Escape cancels");
        break;
    case P_ASSET_READ: receive_asset(0);return;
    case P_STAGE_VERIFY: {
        mb_reader_t reader={&update.file,file_read,update.status.received};
        mb_image_t image;
        char linked[ONLINE_UPDATE_VERSION_BYTES];
        const char *expected=update.status.kind==ONLINE_UPDATE_BOOT?
            update.status.latest_b:update.release.tag;
        FRESULT synced=f_sync(&update.file);
        if (synced!=FR_OK) {result=storage_error(synced,ONLINE_UPDATE_E_SYNC);break;}
        update.status.validation_result=mb_update_image_validate(&reader,update.status.kind==ONLINE_UPDATE_BOOT,
                                     &image,linked,sizeof linked);
        if (update.status.validation_result!=MB_OK) {
            result=ONLINE_UPDATE_E_IMAGE;break;
        }
        int comparison;
        if (online_update_version_compare(expected,linked,
            update.status.kind==ONLINE_UPDATE_BOOT?'B':'F',&comparison) || comparison) {
            result=ONLINE_UPDATE_E_VERSION;break;
        }
        update.phase=P_STAGE_CLOSE;break;
    }
    case P_STAGE_CLOSE: {
        FRESULT closed=f_close(&update.file);
        if (closed==FR_OK || closed==FR_INVALID_OBJECT) update.open=0;
        if (closed!=FR_OK) {result=storage_error(closed,ONLINE_UPDATE_E_CLOSE);break;}
        update.phase=P_STAGE_RENAME;break;
    }
    case P_STAGE_RENAME: {
        FILINFO info;
        FRESULT exists=f_stat(update.final,&info);
        if (exists==FR_OK) {result=ONLINE_UPDATE_E_CONFLICT;break;}
        if (exists!=FR_NO_FILE) {result=storage_error(exists,ONLINE_UPDATE_E_STORAGE);break;}
        FRESULT renamed=f_rename(update.part,update.final);
        if (renamed!=FR_OK) {result=storage_error(renamed,renamed==FR_EXIST?ONLINE_UPDATE_E_CONFLICT:ONLINE_UPDATE_E_RENAME);break;}
        update.owns_part=0; update.phase=P_NONE;
        update.status.state=ONLINE_UPDATE_READY;update.status.result=0;
        (void)snprintf(update.status.stage,sizeof update.status.stage,"Ready to install");
        detail("Validated update ready to install");break;
    }
    default: break;
    }
    if (result<0) fail(result,result==ONLINE_UPDATE_E_CONFLICT?
        "Existing update file must be moved or removed":"Online update failed");
}
#endif
