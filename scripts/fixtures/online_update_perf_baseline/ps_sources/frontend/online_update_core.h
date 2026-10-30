#ifndef ONLINE_UPDATE_CORE_H
#define ONLINE_UPDATE_CORE_H
#include <stddef.h>
#include <stdint.h>
#include "net_json.h"

#define ONLINE_UPDATE_VERSION_BYTES 32U
#define ONLINE_UPDATE_URL_BYTES 256U
#define ONLINE_UPDATE_JSON_BYTES 32768U
#define ONLINE_UPDATE_API_URL "https://api.github.com/repos/hasseily/appletini-one/releases/latest"
#define ONLINE_UPDATE_DOWNLOAD_PREFIX "https://github.com/hasseily/appletini-one/releases/download/"
enum { ONLINE_UPDATE_FIRMWARE, ONLINE_UPDATE_BOOT, ONLINE_UPDATE_KIND_COUNT };
enum { ONLINE_UPDATE_IDLE, ONLINE_UPDATE_CHECKING, ONLINE_UPDATE_CURRENT,
       ONLINE_UPDATE_AVAILABLE, ONLINE_UPDATE_DOWNLOADING, ONLINE_UPDATE_VERIFYING,
       ONLINE_UPDATE_READY, ONLINE_UPDATE_FAILED, ONLINE_UPDATE_CANCELLED };
enum { ONLINE_UPDATE_E_ARGUMENT=-200, ONLINE_UPDATE_E_JSON=-201,
       ONLINE_UPDATE_E_RELEASE=-202, ONLINE_UPDATE_E_ASSET=-203,
       ONLINE_UPDATE_E_LENGTH=-204, ONLINE_UPDATE_E_DIGEST=-205,
       ONLINE_UPDATE_E_IMAGE=-206, ONLINE_UPDATE_E_VERSION=-207,
       ONLINE_UPDATE_E_STORAGE=-208, ONLINE_UPDATE_E_CONFLICT=-209,
       ONLINE_UPDATE_E_WRITE=-210, ONLINE_UPDATE_E_SYNC=-211,
       ONLINE_UPDATE_E_CLOSE=-212, ONLINE_UPDATE_E_RENAME=-213,
       ONLINE_UPDATE_E_CLEANUP=-214, ONLINE_UPDATE_E_HTTP=-215,
       ONLINE_UPDATE_E_CANCELLED=-216, ONLINE_UPDATE_E_UNSAFE=-217 };
typedef struct {
    uint8_t present;
    uint32_t size;
    uint8_t sha256[32];
    char url[ONLINE_UPDATE_URL_BYTES];
} online_update_asset_t;
typedef struct {
    char tag[ONLINE_UPDATE_VERSION_BYTES];
    online_update_asset_t assets[ONLINE_UPDATE_KIND_COUNT];
} online_update_release_t;
typedef struct {
    uint8_t state, kind, available_mask;
    int result;
    uint32_t received, total, firmware_bytes, boot_bytes;
    char installed_f[ONLINE_UPDATE_VERSION_BYTES], installed_b[ONLINE_UPDATE_VERSION_BYTES];
    char latest_f[ONLINE_UPDATE_VERSION_BYTES], latest_b[ONLINE_UPDATE_VERSION_BYTES];
    char detail[96];
    /* Diagnostic fields describe this attempt and survive cleanup/retries.
     * result is the original cause; cleanup_result is a secondary FRESULT.
     * HTTP fields are captured only from this service's owned transaction. */
    char stage[24];
    uint16_t http_status;
    int http_result;
    uint32_t tls_verify;
    int fatfs_result, validation_result, cleanup_result;
} online_update_snapshot_t;
typedef struct {
    net_json_t json;
    online_update_release_t release;
    int result;
} online_update_core_t;

/* Stable versions are F/B followed by three decimal components, no suffix.
 * Returns zero on valid input; comparison is offered relative to installed. */
int online_update_version_compare(const char *installed, const char *offered,
                                  char prefix, int *comparison);
const char *online_update_filename(uint8_t kind);
/* Caller owns an immutable source buffer and advances available as RX arrives.
 * Feed is bounded by budget; result 0 pending, 1 complete, negative failure. */
void online_update_core_init(online_update_core_t *core, const uint8_t *source);
int online_update_core_feed(online_update_core_t *core, size_t available,
                            int final, size_t budget);
#endif
