#ifndef HTTP_DOWNLOAD_H
#define HTTP_DOWNLOAD_H
#include <stddef.h>
#include <stdint.h>
#include "http_service.h"

#define HTTP_DOWNLOAD_DIRECTORY "0:/downloads"
#define HTTP_DOWNLOAD_PATH_MAX 192U
#define HTTP_DOWNLOAD_NAME_MAX 96U
enum { HTTP_DL_NONE, HTTP_DL_WAITING, HTTP_DL_WRITING, HTTP_DL_DONE,
       HTTP_DL_FAILED, HTTP_DL_CANCELLED };
enum { HTTP_DL_E_STORAGE=-120, HTTP_DL_E_NAMES=-121, HTTP_DL_E_WRITE=-122,
       HTTP_DL_E_SYNC=-123, HTTP_DL_E_CLOSE=-124, HTTP_DL_E_RENAME=-125,
       HTTP_DL_E_CLEANUP=-126, HTTP_DL_E_LENGTH=-127,
       HTTP_DL_E_OWNER=-128, HTTP_DL_E_ARGUMENT=-129, HTTP_DL_E_LIMIT=-130,
       HTTP_DL_E_HTTP=-131 };
typedef struct {
    uint8_t state, total_known;
    int16_t result;
    uint32_t written, total;
} http_download_status_t;

#if defined(APPLETINI_HTTP_SERVICE) || defined(NET_SERVICE_HOST)
void http_download_wait(void);
void http_download_reset(void);
void http_download_cancel(void);
/* Foreground SD handover barrier. Nonzero retains failed cleanup for retry. */
int http_download_quiesce(void);
void http_download_fail(int result);
int http_download_begin(const http_status_t *source, const char *preferred);
void http_download_poll(void);
/* Pending download decision, writing, or unfinished file cleanup. */
int http_download_active(void);
void http_download_status(http_download_status_t *out);
/* Empty until a successful sync/close/rename; remains valid until reset. */
const char *http_download_path(void);
#else
static inline int http_download_active(void){return 0;}
static inline int http_download_quiesce(void){return 0;}
#endif
#endif
