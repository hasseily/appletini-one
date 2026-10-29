#ifdef APPLETINI_HTTP_SERVICE
#include "http_download.h"
#include "download_filename.h"
#include "ff.h"
#include <stdio.h>
#include <string.h>

#define COLLISION_LIMIT 10000U
#define COPY_BYTES 4096U
static struct {
    http_download_status_t status;
    FIL file;
    uint8_t open, owns_part, phase;
    unsigned suffix;
    char name[HTTP_DOWNLOAD_NAME_MAX], final[HTTP_DOWNLOAD_PATH_MAX];
    char part[HTTP_DOWNLOAD_PATH_MAX];
    uint8_t buffer[COPY_BYTES];
    http_status_t source;
} download;
static FATFS download_fs;

static int cleanup(void)
{
    int failed=0;
    if(download.open) {
        FRESULT result=f_close(&download.file);
        if(result==FR_OK || result==FR_INVALID_OBJECT) download.open=0;
        else failed=1;
    }
    if(download.owns_part && !download.open) {
        FRESULT result=f_unlink(download.part);
        if(result!=FR_OK && result!=FR_NO_FILE) failed=1;
        else download.owns_part=0;
    }
    if(http_service_getowner()==HTTP_OWNER_DOWNLOAD)
        (void)http_service_cancel_owned(HTTP_OWNER_DOWNLOAD);
    return failed;
}
void http_download_fail(int result)
{
    if(download.status.state==HTTP_DL_DONE) return;
    if(cleanup()) result=HTTP_DL_E_CLEANUP;
    download.status.result=(int16_t)(result?result:HTTP_DL_E_STORAGE);
    download.status.state=HTTP_DL_FAILED;
}
void http_download_cancel(void)
{
    if(download.status.state==HTTP_DL_DONE || download.status.state==HTTP_DL_NONE) return;
    int result=cleanup();
    download.status.result=(int16_t)(result?HTTP_DL_E_CLEANUP:HTTP_E_CANCELLED);
    download.status.state=result?HTTP_DL_FAILED:HTTP_DL_CANCELLED;
}
int http_download_quiesce(void)
{
    http_download_cancel();
    return download.open || download.owns_part ? HTTP_DL_E_CLEANUP : 0;
}
void http_download_reset(void)
{
    if(http_download_quiesce()) return;
    memset(&download,0,sizeof(download));
}
void http_download_wait(void)
{ http_download_reset();if(!download.open&&!download.owns_part)download.status.state=HTTP_DL_WAITING; }
int http_download_active(void)
{ return download.status.state==HTTP_DL_WAITING || download.status.state==HTTP_DL_WRITING ||
         download.open || download.owns_part; }
void http_download_status(http_download_status_t *out)
{ *out=download.status; }
const char *http_download_path(void)
{ return download.status.state==HTTP_DL_DONE?download.final:""; }

static int directory(void)
{
    FILINFO info;
    FRESULT result=f_stat(HTTP_DOWNLOAD_DIRECTORY,&info);
    if(result==FR_NOT_ENABLED) {
        if(f_mount(&download_fs,"0:/",1)!=FR_OK) return HTTP_DL_E_STORAGE;
        result=f_stat(HTTP_DOWNLOAD_DIRECTORY,&info);
    }
    if(result==FR_NO_FILE) {
        result=f_mkdir(HTTP_DOWNLOAD_DIRECTORY);
        if(result==FR_OK) return 0;
        if(result!=FR_EXIST) return HTTP_DL_E_STORAGE;
        result=f_stat(HTTP_DOWNLOAD_DIRECTORY,&info);
    }
    return result==FR_OK && (info.fattrib&AM_DIR)?0:HTTP_DL_E_STORAGE;
}
static int final_name(unsigned suffix)
{
    const char *extension=strrchr(download.name,'.');
    size_t stem=extension?(size_t)(extension-download.name):strlen(download.name);
    int n;
    if(!suffix) n=snprintf(download.final,sizeof(download.final),"%s/%s",
                           HTTP_DOWNLOAD_DIRECTORY,download.name);
    else n=snprintf(download.final,sizeof(download.final),"%s/%.*s-%u%s",
                    HTTP_DOWNLOAD_DIRECTORY,(int)stem,download.name,suffix,extension?extension:"");
    return n<0 || (size_t)n>=sizeof(download.final)?HTTP_DL_E_ARGUMENT:0;
}
static int open_part(void)
{
    FILINFO info;
    /* A crowded directory must not turn one foreground poll into thousands
     * of SD operations while the Apple waits for its descriptor. */
    if(download.suffix>=COLLISION_LIMIT)return HTTP_DL_E_NAMES;
    if(final_name(download.suffix)) return HTTP_DL_E_ARGUMENT;
    FRESULT found=f_stat(download.final,&info);
    if(found==FR_OK) {++download.suffix;return 0;}
    if(found!=FR_NO_FILE) return HTTP_DL_E_STORAGE;
    int n=snprintf(download.part,sizeof(download.part),"%s.part",download.final);
    if(n<0 || (size_t)n>=sizeof(download.part)) return HTTP_DL_E_ARGUMENT;
    FRESULT result=f_open(&download.file,download.part,FA_WRITE|FA_CREATE_NEW);
    if(result==FR_EXIST) {++download.suffix;return 0;}
    if(result!=FR_OK) return HTTP_DL_E_STORAGE;
    download.open=download.owns_part=download.phase=1;return 1;
}
int http_download_begin(const http_status_t *source,const char *preferred)
{
    if(!source || !source->headers_ready || source->status<200 || source->status>=300 ||
       source->retained || source->state==HTTP_S_FAILED || source->state==HTTP_S_IDLE)
        return HTTP_DL_E_ARGUMENT;
    if(download.open || download.owns_part) return HTTP_E_BUSY;
    if(http_service_handoff_download()) { http_download_fail(HTTP_DL_E_OWNER);return HTTP_DL_E_OWNER; }
    download.status.state=HTTP_DL_WRITING;
    download.status.total_known=source->content_length_known;
    download.status.total=source->content_length;
    const char *hint=preferred&&*preferred?preferred:source->suggested_filename;
    int result=download_filename(hint,source->url,download.name,sizeof(download.name));
    if(result) result=HTTP_DL_E_ARGUMENT;
    if(!result) result=directory();
    if(!result) {int opened=open_part();if(opened<0)result=opened;}
    if(result) http_download_fail(result);
    return result;
}
static int commit(void)
{
    if(download.phase!=2) {
        if(f_sync(&download.file)!=FR_OK) return HTTP_DL_E_SYNC;
        FRESULT closed=f_close(&download.file);
        if(closed==FR_OK || closed==FR_INVALID_OBJECT)download.open=0;
        if(closed!=FR_OK) return HTTP_DL_E_CLOSE;
        download.phase=2;
    }
    if(download.suffix>=COLLISION_LIMIT)return HTTP_DL_E_NAMES;
    if(final_name(download.suffix)) return HTTP_DL_E_ARGUMENT;
    FRESULT result=f_rename(download.part,download.final);
    if(result==FR_EXIST) {++download.suffix;return 0;}
    if(result!=FR_OK) return HTTP_DL_E_RENAME;
    download.owns_part=0;
    if(http_service_release_owned(HTTP_OWNER_DOWNLOAD))
        (void)http_service_cancel_owned(HTTP_OWNER_DOWNLOAD);
    download.status.state=HTTP_DL_DONE;download.status.result=0;return 1;
}
void http_download_poll(void)
{
    if(download.status.state!=HTTP_DL_WRITING) return;
    if(http_service_getowner()!=HTTP_OWNER_DOWNLOAD) { http_download_fail(HTTP_DL_E_OWNER);return; }
    http_service_status(&download.source);
    if(download.source.state==HTTP_S_FAILED) { http_download_fail(download.source.result);return; }
    if(download.phase==2) {int result=commit();if(result<0)http_download_fail(result);return;}
    if(!download.open) {
        int opened=open_part();
        if(opened<0)http_download_fail(opened);
        if(opened<=0)return;
    }
    size_t n=http_service_read_owned(HTTP_OWNER_DOWNLOAD,download.buffer,sizeof(download.buffer));
    if(n) {
        if(n>UINT32_MAX-download.status.written) { http_download_fail(HTTP_DL_E_LIMIT);return; }
        if(download.status.total_known && n>download.status.total-download.status.written) {
            http_download_fail(HTTP_DL_E_LENGTH);return;
        }
        UINT wrote=0;
        if(f_write(&download.file,download.buffer,(UINT)n,&wrote)!=FR_OK || wrote!=n) {
            http_download_fail(HTTP_DL_E_WRITE);return;
        }
        download.status.written+=(uint32_t)n;
    }
    http_service_status(&download.source);
    if(download.source.state==HTTP_S_DONE && !download.source.buffered) {
        if(download.source.status<200 || download.source.status>=300 ||
           (download.status.total_known && download.status.written!=download.status.total)) {
            http_download_fail(HTTP_DL_E_LENGTH);return;
        }
        int result=commit();if(result<0) http_download_fail(result);
    }
}
#endif
