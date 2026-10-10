#ifndef APPLETINI_MEMORY_API_H
#define APPLETINI_MEMORY_API_H

#include <stdint.h>

#define MEMORY_API_SELECTOR          0x80U
#define MEMORY_API_MAJOR             1U
#define MEMORY_API_MINOR             2U
#define MEMORY_API_STATUS_SIZE       32U
#define MEMORY_API_HEADER_SIZE       8U
#define MEMORY_API_DESCRIPTOR_SIZE   16U
#define MEMORY_API_MAX_DESCRIPTORS   16U
#define MEMORY_API_MAX_AUX_BANK      126U
#define MEMORY_API_MIN_ADDR          0x0200U
#define MEMORY_API_LIMIT_ADDR        0xC000U
#define MEMORY_API_DMA_CHUNK         512U
#define MEMORY_API_COPY_CHUNK        504U
#define MEMORY_API_COPY              1U
#define MEMORY_API_FILL              2U
#define MEMORY_API_COPY_ROWS         3U
#define MEMORY_API_PRIVATE           1U
#define MEMORY_API_PUBLISH_SHR       2U
#define MEMORY_API_SHR_MIN_ADDR      0x2000U
#define MEMORY_API_SHR_LIMIT_ADDR    0x9D00U
#define MEMORY_API_MAIN              0U
#define MEMORY_API_AUX               1U
#define MEMORY_API_FEATURE_COPY      1U
#define MEMORY_API_FEATURE_FILL      2U
#define MEMORY_API_FEATURE_PRIVATE   4U
#define MEMORY_API_FEATURE_PUBLISH_SHR 8U
#define MEMORY_API_FEATURE_COPY_ROWS  16U

enum {
    MEMORY_API_OK = 0,
    MEMORY_API_UNAVAILABLE = 0x60,
    MEMORY_API_BAD_HEADER = 0x61,
    MEMORY_API_BAD_DESCRIPTOR = 0x62,
    MEMORY_API_RANGE = 0x63,
    MEMORY_API_OVERLAP = 0x64,
    MEMORY_API_PRIVATE_REQUIRED = 0x65,
    MEMORY_API_BUSY = 0x66,
    MEMORY_API_IO = 0x67,
    MEMORY_API_SESSION_LOST = 0x68,
    MEMORY_API_UNSAFE = 0x69
};

/* Physical addresses: MAIN bank 0, base AUX bank 1, logical RamWorks bank
 * n maps to physical bank n+1. The backend runs with one CPU hold over the
 * complete batch. Error callbacks return MEMORY_API_*; available and
 * private_required are boolean, features is a bit mask, micros is a timestamp.
 * end() must not release a surviving hold if DMA cannot drain. */
typedef struct {
    void *ctx;
    /* Advisory STATUS only. begin() authoritatively checks availability,
     * distinguishing UNAVAILABLE from any previous unsafe drain failure. */
    uint8_t (*available)(void *ctx);
    uint8_t (*begin)(void *ctx, uint8_t needs_ramworks);
    uint8_t (*read)(void *ctx, uint32_t phys, uint8_t *data, uint16_t length);
    uint8_t (*write)(void *ctx, uint32_t phys, const uint8_t *data, uint16_t length);
    uint8_t (*end)(void *ctx);
    uint8_t (*private_required)(void *ctx, uint32_t phys, uint16_t length);
    uint32_t (*micros)(void *ctx);
    /* Optional whole-descriptor engine. UNAVAILABLE with zero completed
     * bytes permits the read/write fallback for ordinary memory only.
     * Publication must never use that fallback. Every other result is final;
     * completed counts confirmed writes (and capture admissions) only. */
    uint8_t (*transfer)(void *ctx, uint32_t source, uint32_t destination,
                        uint16_t length, uint8_t operation, uint8_t fill,
                        uint8_t flags, uint16_t *completed);
    /* Optional feature bits beyond COPY/FILL/PRIVATE. Advisory STATUS only;
     * PUBLISH_SHR also requires transfer and validate_publish callbacks. */
    uint16_t (*features)(void *ctx);
    /* Called once under the hold after all publication ranges validate,
     * before any descriptor executes. Must verify supported, enabled SHR
     * capture. transfer must recheck live readiness before each START. */
    uint8_t (*validate_publish)(void *ctx);
    /* Optional strided copy. Width and gaps are bytes; rows_minus1 encodes
     * 1..256 rows. No scalar fallback, even when zero bytes completed.
     * Validate support once under hold before any batch descriptor executes;
     * transfer_rows rechecks capability and live session before each START.
     * UNAVAILABLE from validation is safe preflight rejection; UNAVAILABLE
     * from transfer_rows becomes IO, as execution may have written a prefix. */
    uint8_t (*validate_rows)(void *ctx);
    uint8_t (*transfer_rows)(void *ctx, uint32_t source, uint32_t destination,
                             uint16_t width, uint8_t rows_minus1,
                             uint8_t source_gap, uint8_t destination_gap,
                             uint8_t flags, uint16_t *completed);
} memory_api_backend_t;

/* payload starts at AMEM, after the SmartPort control-list length word.
 * All descriptors validate before writes. Status progress includes only
 * confirmed chunks/records. A failing write can have partially changed its
 * chunk; a hardware error must never trigger a whole-operation fallback. */
uint8_t memory_api_execute(const uint8_t *payload, uint16_t length,
                           const memory_api_backend_t *backend);
void memory_api_status(uint8_t out[MEMORY_API_STATUS_SIZE],
                        const memory_api_backend_t *backend);
void memory_api_reset(void);

#endif
