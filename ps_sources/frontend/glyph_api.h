#ifndef APPLETINI_GLYPH_API_H
#define APPLETINI_GLYPH_API_H

#include <stdint.h>

#define GLYPH_API_SELECTOR          0x81U
#define GLYPH_API_MAJOR             1U
#define GLYPH_API_MINOR             0U
#define GLYPH_API_STATUS_SIZE       32U
#define GLYPH_API_HEADER_SIZE       16U
#define GLYPH_API_ROM_SIZE          4096U
#define GLYPH_API_MAX_CHUNK         512U

#define GLYPH_API_BEGIN             1U
#define GLYPH_API_WRITE             2U
#define GLYPH_API_COMMIT            3U
#define GLYPH_API_ABORT             4U
#define GLYPH_API_RESTORE           5U

#define GLYPH_API_AVAILABLE         0x0001U
#define GLYPH_API_CUSTOM_ACTIVE     0x0002U
#define GLYPH_API_PENDING           0x0004U
#define GLYPH_API_UPLOADING         0x0008U

enum {
    GLYPH_API_OK = 0,
    GLYPH_API_UNAVAILABLE = 0x60,
    GLYPH_API_BAD_HEADER = 0x61,
    GLYPH_API_BAD_COMMAND = 0x62,
    GLYPH_API_RANGE = 0x63,
    GLYPH_API_TRANSACTION = 0x64,
    GLYPH_API_INCOMPLETE = 0x65,
    GLYPH_API_BUSY = 0x66,
    GLYPH_API_CHECKSUM = 0x67
};

/* The backend must copy all 4096 bytes before apply() returns OK. NULL means
 * restore the menu-selected ROM. A failed apply must not change either the
 * active image or the pending image. OK means queued; status reports PENDING
 * until the renderer has applied it between frames. CUSTOM_ACTIVE describes
 * the applied image, and generation changes when the renderer applies one.
 * Calls run on the frontend service CPU; these functions are not thread-safe. */
typedef struct {
    void *ctx;
    uint8_t (*apply)(void *ctx, const uint8_t *rom);
    void (*status)(void *ctx, uint16_t *flags, uint32_t *generation);
} glyph_api_backend_t;

/* Payload starts at AGLY, after the SmartPort control-list length word.
 * All wire words use little endian. The core owns one staged upload. */
uint8_t glyph_api_execute(const uint8_t *payload, uint16_t length,
                          const glyph_api_backend_t *backend);
void glyph_api_status(uint8_t out[GLYPH_API_STATUS_SIZE],
                       const glyph_api_backend_t *backend);
/* Reset only discards the upload and its status. It never calls apply(). */
void glyph_api_reset(void);
uint32_t glyph_api_crc32(const uint8_t *data, uint16_t length);

#endif
