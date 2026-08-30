#include "golden_self_update.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ff.h"
#include "xstatus.h"

#include "crc32.h"
#include "qspi_nor.h"
#include "uart.h"

#define PRIMARY_FLASH_OFFSET                GOLDEN_SELF_UPDATE_PRIMARY_OFFSET
#define TRIAL_FLASH_OFFSET                  GOLDEN_SELF_UPDATE_TRIAL_OFFSET
#define GOLDEN_SLOT_SIZE                    GOLDEN_SELF_UPDATE_SLOT_SIZE
#define FALLBACK_FLASH_OFFSET               0x00200000U
#define FALLBACK_FLASH_SIZE                 0x00DF0000U
#define QSPI_ERASE_SIZE                     0x00010000U
#define QSPI_ADDRESS_BYTES                  3U

#define BOOT_HEADER_MIN_SIZE                0x000000A0U
#define BOOT_WIDTH_WORD_OFFSET              0x00000020U
#define BOOT_IDENT_WORD_OFFSET              0x00000024U
#define BOOT_SOURCE_OFFSET                  0x00000030U
#define BOOT_IMAGE_LENGTH_OFFSET            0x00000034U
#define BOOT_DEST_OFFSET                    0x00000038U
#define BOOT_EXEC_OFFSET                    0x0000003CU
#define BOOT_TOTAL_LENGTH_OFFSET            0x00000040U
#define BOOT_HEADER_CHECKSUM_OFFSET         0x00000048U
#define BOOT_IHT_OFFSET                     0x00000098U
#define BOOT_PHT_OFFSET                     0x0000009CU
#define BOOT_WIDTH_WORD                     0xAA995566U
#define BOOT_IDENT_WORD                     0x584C4E58U

#define IHT_SIZE                            64U
#define IHT_MAX_PARTITIONS                  14U
#define IHT_VERSION_WORD                    0U
#define IHT_PARTITION_COUNT_WORD            1U
#define IHT_PHT_ADDRESS_WORD                2U
#define IHT_FIRST_IMAGE_ADDRESS_WORD        3U
#define IHT_AUTH_CERT_ADDRESS_WORD          4U

#define PHT_SIZE                            64U
#define PHT_TOTAL_WORDS_WORD                2U
#define PHT_LOAD_ADDRESS_WORD               3U
#define PHT_EXEC_ADDRESS_WORD               4U
#define PHT_DATA_ADDRESS_WORD               5U
#define PHT_ATTRIBUTES_WORD                 6U
#define PHT_CHECKSUM_ADDRESS_WORD           8U
#define PHT_HEADER_CHECKSUM_WORD            15U

#define MULTIBOOT_ALIGNMENT                 0x00008000U
#define IO_CHUNK_SIZE                       4096U

enum {
    GSU_E_OK = 0,
    GSU_E_ARGUMENT = 1,
    GSU_E_SOURCE_OPEN = 2,
    GSU_E_SOURCE_READ = 3,
    GSU_E_SOURCE_SIZE = 4,
    GSU_E_MANIFEST = 5,
    GSU_E_IMAGE = 6,
    GSU_E_TARGET_CRC = 7,
    GSU_E_TARGET_MARKER = 8,
    GSU_E_QSPI_INIT = 9,
    GSU_E_FALLBACK_IMAGE = 10,
    GSU_E_FALLBACK_MANIFEST = 11,
    GSU_E_FALLBACK_CRC = 12,
    GSU_E_FALLBACK_MARKER = 13,
    GSU_E_INVALIDATE = 14,
    GSU_E_ERASE = 15,
    GSU_E_PROGRAM = 16,
    GSU_E_READBACK = 17,
    GSU_E_VERIFY = 18,
    GSU_E_FINAL_IDENT = 19,
    GSU_E_RENAME = 20,
    GSU_E_PRIMARY_IMAGE = 21,
    GSU_E_TRIAL_IMAGE = 22
};

typedef int (*image_read_fn)(void *ctx, uint32_t offset, void *dst, uint32_t len);

typedef struct {
    image_read_fn read;
    void *ctx;
    uint32_t size;
} image_reader_t;

typedef struct {
    FIL *file;
} file_reader_ctx_t;

typedef struct {
    const uint8_t *data;
} memory_reader_ctx_t;

typedef struct {
    qspi_nor_t *nor;
    uint32_t base;
} flash_reader_ctx_t;

typedef struct {
    golden_self_update_manifest_t manifest;
    uint32_t total_size;
} target_image_t;

static uint8_t g_source_buf[IO_CHUNK_SIZE] __attribute__((aligned(64)));
static uint8_t g_flash_buf[IO_CHUNK_SIZE] __attribute__((aligned(64)));

const char golden_self_update_recovery_marker[] __attribute__((used)) =
    GOLDEN_SELF_UPDATE_RECOVERY_MARKER;

typedef char golden_manifest_size_must_be_32[
    (sizeof(golden_self_update_manifest_t) == GOLDEN_SELF_UPDATE_MANIFEST_SIZE) ? 1 : -1
];

static uint32_t g_log_uart;
static uint32_t g_log_mirror_uart;

static void log_puts(const char *text)
{
    uart_puts(g_log_uart, text);
    if (g_log_mirror_uart != g_log_uart) {
        uart_puts(g_log_mirror_uart, text);
    }
}

static void log_printf(const char *fmt, ...)
{
    char line[220];
    va_list ap;

    va_start(ap, fmt);
    (void)vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    log_puts(line);
}

static void result_init(golden_self_update_result_t *out)
{
    memset(out, 0, sizeof(*out));
}

static void result_error(golden_self_update_result_t *out,
                         int code,
                         const char *message)
{
    out->error_code = code;
    out->updated = 0;
    out->verified = 0;
    out->reset_required = 0;
    (void)snprintf(out->error_msg, sizeof(out->error_msg), "%s", message);
    log_printf("[GSU] ERROR %d: %s\r\n", code, message);
    if (out->flash_changed != 0) {
        log_puts("[GSU] Flash operation was incomplete; retry from a valid golden copy.\r\n");
    }
}

static uint32_t load_le32(const uint8_t *p)
{
    return ((uint32_t)p[0]) |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int range_valid(uint32_t offset, uint32_t len, uint32_t size)
{
    return (offset <= size && len <= (size - offset)) ? 1 : 0;
}

static int reader_read(const image_reader_t *reader,
                       uint32_t offset,
                       void *dst,
                       uint32_t len)
{
    if (reader == NULL || reader->read == NULL || dst == NULL ||
        !range_valid(offset, len, reader->size)) {
        return XST_FAILURE;
    }
    return reader->read(reader->ctx, offset, dst, len);
}

static int file_read(void *ctx, uint32_t offset, void *dst, uint32_t len)
{
    file_reader_ctx_t *file_ctx = (file_reader_ctx_t *)ctx;
    UINT bytes_read = 0U;

    if (f_lseek(file_ctx->file, (FSIZE_t)offset) != FR_OK) {
        return XST_FAILURE;
    }
    if (f_read(file_ctx->file, dst, (UINT)len, &bytes_read) != FR_OK ||
        bytes_read != (UINT)len) {
        return XST_FAILURE;
    }
    return XST_SUCCESS;
}

static int memory_read(void *ctx, uint32_t offset, void *dst, uint32_t len)
{
    const memory_reader_ctx_t *memory_ctx = (const memory_reader_ctx_t *)ctx;
    memcpy(dst, memory_ctx->data + offset, len);
    return XST_SUCCESS;
}

static int flash_read(void *ctx, uint32_t offset, void *dst, uint32_t len)
{
    flash_reader_ctx_t *flash_ctx = (flash_reader_ctx_t *)ctx;
    return qspi_nor_read(flash_ctx->nor, flash_ctx->base + offset, dst, len);
}

static int words_checksum_valid(const uint8_t *bytes,
                                uint32_t word_count,
                                uint32_t checksum_word)
{
    uint32_t sum = 0U;
    uint32_t i;

    for (i = 0U; i < word_count; ++i) {
        sum += load_le32(bytes + (i * 4U));
    }
    return ((sum ^ 0xFFFFFFFFU) == load_le32(bytes + (checksum_word * 4U))) ? 1 : 0;
}

static int boot_header_valid_at(const image_reader_t *reader, uint32_t offset)
{
    uint8_t header[BOOT_HEADER_CHECKSUM_OFFSET + 4U];

    if (!range_valid(offset, (uint32_t)sizeof(header), reader->size) ||
        reader_read(reader, offset, header, (uint32_t)sizeof(header)) != XST_SUCCESS) {
        return 0;
    }
    if (load_le32(header + BOOT_WIDTH_WORD_OFFSET) != BOOT_WIDTH_WORD ||
        load_le32(header + BOOT_IDENT_WORD_OFFSET) != BOOT_IDENT_WORD) {
        return 0;
    }
    return words_checksum_valid(header + BOOT_WIDTH_WORD_OFFSET, 10U, 10U);
}

static int partition_sentinel_valid(const uint8_t header[PHT_SIZE])
{
    uint32_t i;

    for (i = 0U; i < PHT_HEADER_CHECKSUM_WORD; ++i) {
        if (load_le32(header + (i * 4U)) != 0U) {
            return 0;
        }
    }
    return (load_le32(header + (PHT_HEADER_CHECKSUM_WORD * 4U)) == 0xFFFFFFFFU) ? 1 : 0;
}

static int bootgen_image_validate(const image_reader_t *reader,
                                  uint32_t *payload_size_out)
{
    uint8_t boot_header[BOOT_HEADER_MIN_SIZE];
    uint8_t iht[IHT_SIZE];
    uint8_t pht[PHT_SIZE];
    uint32_t iht_offset;
    uint32_t pht_offset;
    uint32_t iht_pht_offset;
    uint32_t image_header_offset;
    uint32_t auth_cert_offset;
    uint32_t partition_count;
    uint32_t payload_end = 0U;
    uint32_t first_partition_start = 0U;
    uint32_t first_partition_total = 0U;
    uint32_t first_partition_load = 0U;
    uint32_t first_partition_exec = 0U;
    uint32_t i;

    if (reader->size < BOOT_HEADER_MIN_SIZE ||
        reader_read(reader, 0U, boot_header, sizeof(boot_header)) != XST_SUCCESS ||
        !boot_header_valid_at(reader, 0U)) {
        return XST_FAILURE;
    }

    iht_offset = load_le32(boot_header + BOOT_IHT_OFFSET);
    pht_offset = load_le32(boot_header + BOOT_PHT_OFFSET);
    if ((iht_offset & 3U) != 0U || (pht_offset & 3U) != 0U ||
        !range_valid(iht_offset, IHT_SIZE, reader->size) ||
        !range_valid(pht_offset, PHT_SIZE, reader->size) ||
        reader_read(reader, iht_offset, iht, sizeof(iht)) != XST_SUCCESS) {
        return XST_FAILURE;
    }

    partition_count = load_le32(iht + (IHT_PARTITION_COUNT_WORD * 4U));
    iht_pht_offset = load_le32(iht + (IHT_PHT_ADDRESS_WORD * 4U));
    image_header_offset = load_le32(iht + (IHT_FIRST_IMAGE_ADDRESS_WORD * 4U));
    auth_cert_offset = load_le32(iht + (IHT_AUTH_CERT_ADDRESS_WORD * 4U));
    if (load_le32(iht + (IHT_VERSION_WORD * 4U)) == 0U ||
        load_le32(iht + (IHT_VERSION_WORD * 4U)) == 0xFFFFFFFFU ||
        partition_count == 0U || partition_count > IHT_MAX_PARTITIONS ||
        iht_pht_offset > (UINT32_MAX / 4U) ||
        (iht_pht_offset * 4U) != pht_offset ||
        image_header_offset > (UINT32_MAX / 4U) ||
        !range_valid(image_header_offset * 4U, PHT_SIZE, reader->size) ||
        (auth_cert_offset != 0U &&
         (auth_cert_offset > (UINT32_MAX / 4U) ||
          !range_valid(auth_cert_offset * 4U, 4U, reader->size))) ||
        partition_count > ((reader->size - pht_offset) / PHT_SIZE)) {
        return XST_FAILURE;
    }

    for (i = 0U; i < partition_count; ++i) {
        uint32_t total_words;
        uint32_t data_words;
        uint32_t data_offset;
        uint32_t partition_end;
        uint32_t checksum_offset;
        uint32_t attributes;

        if (reader_read(reader, pht_offset + (i * PHT_SIZE), pht, sizeof(pht)) != XST_SUCCESS ||
            !words_checksum_valid(pht, PHT_HEADER_CHECKSUM_WORD,
                                  PHT_HEADER_CHECKSUM_WORD)) {
            return XST_FAILURE;
        }
        total_words = load_le32(pht + (PHT_TOTAL_WORDS_WORD * 4U));
        data_words = load_le32(pht + 4U);
        data_offset = load_le32(pht + (PHT_DATA_ADDRESS_WORD * 4U));
        checksum_offset = load_le32(pht + (PHT_CHECKSUM_ADDRESS_WORD * 4U));
        attributes = load_le32(pht + (PHT_ATTRIBUTES_WORD * 4U));
        if (total_words == 0U || data_words == 0U || data_words > total_words ||
            total_words > (UINT32_MAX / 4U) ||
            data_offset > (UINT32_MAX / 4U) ||
            !range_valid(data_offset * 4U, total_words * 4U, reader->size) ||
            ((attributes & 0xF0U) != 0x10U && (attributes & 0xF0U) != 0x20U) ||
            (checksum_offset != 0U &&
             (checksum_offset > (UINT32_MAX / 4U) ||
              !range_valid(checksum_offset * 4U, 4U, reader->size)))) {
            return XST_FAILURE;
        }
        partition_end = (data_offset * 4U) + (total_words * 4U);
        if (partition_end > payload_end) {
            payload_end = partition_end;
        }
        if (i == 0U) {
            first_partition_start = data_offset * 4U;
            first_partition_total = total_words * 4U;
            first_partition_load = load_le32(pht + (PHT_LOAD_ADDRESS_WORD * 4U));
            first_partition_exec = load_le32(pht + (PHT_EXEC_ADDRESS_WORD * 4U));
        }
    }

    if (!range_valid(pht_offset + (partition_count * PHT_SIZE), PHT_SIZE, reader->size) ||
        reader_read(reader, pht_offset + (partition_count * PHT_SIZE),
                    pht, sizeof(pht)) != XST_SUCCESS ||
        !partition_sentinel_valid(pht)) {
        return XST_FAILURE;
    }

    if (first_partition_start != load_le32(boot_header + BOOT_SOURCE_OFFSET) ||
        first_partition_total != load_le32(boot_header + BOOT_TOTAL_LENGTH_OFFSET) ||
        load_le32(boot_header + BOOT_IMAGE_LENGTH_OFFSET) > first_partition_total ||
        first_partition_load != load_le32(boot_header + BOOT_DEST_OFFSET) ||
        first_partition_exec != load_le32(boot_header + BOOT_EXEC_OFFSET) ||
        payload_end == 0U || payload_end > reader->size) {
        return XST_FAILURE;
    }

    *payload_size_out = payload_end;
    return XST_SUCCESS;
}

static int alternate_boot_header_present(const image_reader_t *reader,
                                         uint32_t start_offset,
                                         uint32_t end_offset)
{
    uint32_t offset;
    uint32_t last_offset;

    if (end_offset > reader->size) {
        end_offset = reader->size;
    }
    if (end_offset < (BOOT_HEADER_CHECKSUM_OFFSET + 4U)) {
        return 0;
    }
    last_offset = end_offset - (BOOT_HEADER_CHECKSUM_OFFSET + 4U);
    if (start_offset > last_offset) {
        return 0;
    }
    for (offset = start_offset; offset <= last_offset;
         offset += MULTIBOOT_ALIGNMENT) {
        if (boot_header_valid_at(reader, offset)) {
            return 1;
        }
        if (offset > (UINT32_MAX - MULTIBOOT_ALIGNMENT)) {
            break;
        }
    }
    return 0;
}

static int stream_crc_and_marker(const image_reader_t *reader,
                                 uint32_t size,
                                 uint32_t *crc_out,
                                 int *marker_found_out)
{
    const uint8_t *marker = (const uint8_t *)golden_self_update_recovery_marker;
    const uint32_t marker_len = (uint32_t)(sizeof(golden_self_update_recovery_marker) - 1U);
    uint32_t crc = crc32_init();
    uint32_t matched = 0U;
    uint32_t offset = 0U;
    int found = 0;

    while (offset < size) {
        uint32_t chunk = size - offset;
        uint32_t i;

        if (chunk > IO_CHUNK_SIZE) {
            chunk = IO_CHUNK_SIZE;
        }
        if (reader_read(reader, offset, g_source_buf, chunk) != XST_SUCCESS) {
            return XST_FAILURE;
        }
        crc = crc32_update(crc, g_source_buf, chunk);
        for (i = 0U; i < chunk && !found; ++i) {
            const uint8_t byte = g_source_buf[i];
            if (byte == marker[matched]) {
                ++matched;
                if (matched == marker_len) {
                    found = 1;
                }
            } else {
                matched = (byte == marker[0]) ? 1U : 0U;
            }
        }
        offset += chunk;
    }

    *crc_out = crc32_finish(crc);
    *marker_found_out = found;
    return XST_SUCCESS;
}

static int manifest_decode(const uint8_t bytes[GOLDEN_SELF_UPDATE_MANIFEST_SIZE],
                           golden_self_update_manifest_t *manifest)
{
    static const uint8_t magic[8] = { 'A', 'T', 'N', 'I', 'M', 'G', '1', 0 };
    uint32_t crc;

    if (memcmp(bytes, magic, sizeof(magic)) != 0) {
        return XST_FAILURE;
    }
    memcpy(manifest->magic, bytes, sizeof(manifest->magic));
    manifest->version = load_le32(bytes + 8U);
    manifest->role = load_le32(bytes + 12U);
    manifest->flags = load_le32(bytes + 16U);
    manifest->payload_size = load_le32(bytes + 20U);
    manifest->payload_crc32 = load_le32(bytes + 24U);
    manifest->manifest_crc32 = load_le32(bytes + 28U);
    crc = crc32_init();
    crc = crc32_update(crc, bytes, 28U);
    crc = crc32_finish(crc);
    if (manifest->version != GOLDEN_SELF_UPDATE_MANIFEST_VERSION ||
        (manifest->flags & ~GOLDEN_SELF_UPDATE_FLAG_RECOVERY) != 0U ||
        crc != manifest->manifest_crc32) {
        return XST_FAILURE;
    }
    return XST_SUCCESS;
}

static int target_preflight(const image_reader_t *reader,
                            target_image_t *target,
                            golden_self_update_result_t *out)
{
    uint8_t manifest_bytes[GOLDEN_SELF_UPDATE_MANIFEST_SIZE];
    uint32_t bootgen_size;
    uint32_t crc;
    int marker_found;

    if (reader->size <= GOLDEN_SELF_UPDATE_MANIFEST_SIZE ||
        reader->size > GOLDEN_SLOT_SIZE) {
        result_error(out, GSU_E_SOURCE_SIZE, "target size exceeds the 1 MiB golden slot");
        return XST_FAILURE;
    }
    if (reader_read(reader,
                    reader->size - GOLDEN_SELF_UPDATE_MANIFEST_SIZE,
                    manifest_bytes,
                    sizeof(manifest_bytes)) != XST_SUCCESS) {
        result_error(out, GSU_E_SOURCE_READ, "cannot read target manifest");
        return XST_FAILURE;
    }
    if (manifest_decode(manifest_bytes, &target->manifest) != XST_SUCCESS ||
        target->manifest.role != GOLDEN_SELF_UPDATE_ROLE_GOLDEN ||
        (target->manifest.flags & GOLDEN_SELF_UPDATE_FLAG_RECOVERY) == 0U ||
        target->manifest.payload_size !=
            (reader->size - GOLDEN_SELF_UPDATE_MANIFEST_SIZE)) {
        result_error(out, GSU_E_MANIFEST, "invalid golden target manifest");
        return XST_FAILURE;
    }
    if (bootgen_image_validate(reader, &bootgen_size) != XST_SUCCESS ||
        bootgen_size != target->manifest.payload_size) {
        result_error(out, GSU_E_IMAGE, "invalid target Bootgen image");
        return XST_FAILURE;
    }
    if (alternate_boot_header_present(reader, MULTIBOOT_ALIGNMENT,
                                      target->manifest.payload_size)) {
        result_error(out, GSU_E_IMAGE, "target contains an alternate multiboot header");
        return XST_FAILURE;
    }
    if (stream_crc_and_marker(reader, target->manifest.payload_size,
                              &crc, &marker_found) != XST_SUCCESS) {
        result_error(out, GSU_E_SOURCE_READ, "cannot read target payload");
        return XST_FAILURE;
    }
    if (crc != target->manifest.payload_crc32) {
        result_error(out, GSU_E_TARGET_CRC, "target payload CRC32 mismatch");
        return XST_FAILURE;
    }
    if (!marker_found) {
        result_error(out, GSU_E_TARGET_MARKER, "target recovery marker is missing");
        return XST_FAILURE;
    }

    target->total_size = reader->size;
    out->payload_size = target->manifest.payload_size;
    out->payload_crc32 = target->manifest.payload_crc32;
    return XST_SUCCESS;
}

static void flash_slot_reader_init(qspi_nor_t *nor,
                                   uint32_t slot_offset,
                                   flash_reader_ctx_t *flash_ctx,
                                   image_reader_t *reader)
{
    flash_ctx->nor = nor;
    flash_ctx->base = slot_offset;
    reader->read = flash_read;
    reader->ctx = flash_ctx;
    reader->size = GOLDEN_SLOT_SIZE;
}

static int primary_bootgen_preflight(qspi_nor_t *nor,
                                     golden_self_update_result_t *out)
{
    flash_reader_ctx_t flash_ctx;
    image_reader_t reader;
    uint32_t payload_size;

    flash_slot_reader_init(nor, PRIMARY_FLASH_OFFSET, &flash_ctx, &reader);
    if (!boot_header_valid_at(&reader, 0U) ||
        bootgen_image_validate(&reader, &payload_size) != XST_SUCCESS ||
        payload_size > GOLDEN_SLOT_SIZE) {
        result_error(out, GSU_E_PRIMARY_IMAGE,
                     "primary golden image is not bootable; trial slot preserved");
        return XST_FAILURE;
    }
    return XST_SUCCESS;
}

static int flash_golden_preflight(qspi_nor_t *nor,
                                  uint32_t slot_offset,
                                  target_image_t *target,
                                  image_reader_t *image_reader,
                                  flash_reader_ctx_t *flash_ctx,
                                  golden_self_update_result_t *out)
{
    image_reader_t slot_reader;
    uint32_t payload_size;

    flash_slot_reader_init(nor, slot_offset, flash_ctx, &slot_reader);
    if (!boot_header_valid_at(&slot_reader, 0U) ||
        bootgen_image_validate(&slot_reader, &payload_size) != XST_SUCCESS ||
        payload_size > (GOLDEN_SLOT_SIZE - GOLDEN_SELF_UPDATE_MANIFEST_SIZE)) {
        result_error(out, GSU_E_TRIAL_IMAGE, "trial golden Bootgen image is invalid");
        return XST_FAILURE;
    }

    *image_reader = slot_reader;
    image_reader->size = payload_size + GOLDEN_SELF_UPDATE_MANIFEST_SIZE;
    if (target_preflight(image_reader, target, out) != XST_SUCCESS) {
        return XST_FAILURE;
    }
    return XST_SUCCESS;
}

static int fallback_preflight(qspi_nor_t *nor,
                              uint32_t flash_capacity,
                              golden_self_update_result_t *out)
{
    flash_reader_ctx_t flash_ctx;
    image_reader_t reader;
    golden_self_update_manifest_t manifest;
    uint8_t manifest_bytes[GOLDEN_SELF_UPDATE_MANIFEST_SIZE];
    uint32_t payload_size;
    uint32_t crc;
    uint32_t available;
    int marker_found;

    if (flash_capacity <= FALLBACK_FLASH_OFFSET) {
        result_error(out, GSU_E_FALLBACK_IMAGE, "detected flash has no fallback region");
        return XST_FAILURE;
    }
    available = flash_capacity - FALLBACK_FLASH_OFFSET;
    if (available > FALLBACK_FLASH_SIZE) {
        available = FALLBACK_FLASH_SIZE;
    }
    flash_ctx.nor = nor;
    flash_ctx.base = FALLBACK_FLASH_OFFSET;
    reader.read = flash_read;
    reader.ctx = &flash_ctx;
    reader.size = available;

    if (bootgen_image_validate(&reader, &payload_size) != XST_SUCCESS ||
        payload_size > (FALLBACK_FLASH_SIZE - GOLDEN_SELF_UPDATE_MANIFEST_SIZE) ||
        !range_valid(payload_size, GOLDEN_SELF_UPDATE_MANIFEST_SIZE, reader.size)) {
        result_error(out, GSU_E_FALLBACK_IMAGE, "fallback Bootgen image is invalid");
        return XST_FAILURE;
    }
    if (reader_read(&reader, payload_size, manifest_bytes,
                    sizeof(manifest_bytes)) != XST_SUCCESS ||
        manifest_decode(manifest_bytes, &manifest) != XST_SUCCESS ||
        manifest.role != GOLDEN_SELF_UPDATE_ROLE_FIRMWARE ||
        (manifest.flags & GOLDEN_SELF_UPDATE_FLAG_RECOVERY) == 0U ||
        manifest.payload_size != payload_size) {
        result_error(out, GSU_E_FALLBACK_MANIFEST,
                     "fallback recovery manifest is invalid");
        return XST_FAILURE;
    }
    if (stream_crc_and_marker(&reader, payload_size,
                              &crc, &marker_found) != XST_SUCCESS ||
        crc != manifest.payload_crc32) {
        result_error(out, GSU_E_FALLBACK_CRC, "fallback payload CRC32 mismatch");
        return XST_FAILURE;
    }
    if (!marker_found) {
        result_error(out, GSU_E_FALLBACK_MARKER,
                     "fallback recovery marker is missing");
        return XST_FAILURE;
    }
    return XST_SUCCESS;
}

static int program_target_except_ident(qspi_nor_t *nor,
                                       uint32_t flash_offset,
                                       const image_reader_t *reader,
                                       uint32_t total_size)
{
    uint32_t offset = 0U;

    while (offset < total_size) {
        uint32_t chunk = total_size - offset;
        uint32_t before = 0U;
        uint32_t after_offset;

        if (chunk > IO_CHUNK_SIZE) {
            chunk = IO_CHUNK_SIZE;
        }
        if (reader_read(reader, offset, g_source_buf, chunk) != XST_SUCCESS) {
            return XST_FAILURE;
        }

        if (offset < BOOT_IDENT_WORD_OFFSET &&
            (offset + chunk) > BOOT_IDENT_WORD_OFFSET) {
            before = BOOT_IDENT_WORD_OFFSET - offset;
            if (before != 0U &&
                qspi_nor_program(nor, flash_offset + offset,
                                 g_source_buf, before) != XST_SUCCESS) {
                return XST_FAILURE;
            }
            after_offset = BOOT_IDENT_WORD_OFFSET + 4U;
            if ((offset + chunk) > after_offset &&
                qspi_nor_program(nor, flash_offset + after_offset,
                                 g_source_buf + (after_offset - offset),
                                 (offset + chunk) - after_offset) != XST_SUCCESS) {
                return XST_FAILURE;
            }
        } else if (offset >= BOOT_IDENT_WORD_OFFSET &&
                   offset < (BOOT_IDENT_WORD_OFFSET + 4U)) {
            after_offset = BOOT_IDENT_WORD_OFFSET + 4U;
            if ((offset + chunk) > after_offset &&
                qspi_nor_program(nor, flash_offset + after_offset,
                                 g_source_buf + (after_offset - offset),
                                 (offset + chunk) - after_offset) != XST_SUCCESS) {
                return XST_FAILURE;
            }
        } else if (qspi_nor_program(nor, flash_offset + offset,
                                    g_source_buf, chunk) != XST_SUCCESS) {
            return XST_FAILURE;
        }
        offset += chunk;
    }
    return XST_SUCCESS;
}

static int verify_target(qspi_nor_t *nor,
                         uint32_t flash_offset,
                         const image_reader_t *reader,
                         const target_image_t *target,
                         int ident_must_be_erased,
                         uint32_t *payload_crc_out)
{
    uint32_t offset = 0U;
    uint32_t crc = crc32_init();

    while (offset < target->total_size) {
        uint32_t chunk = target->total_size - offset;
        uint32_t i;

        if (chunk > IO_CHUNK_SIZE) {
            chunk = IO_CHUNK_SIZE;
        }
        if (reader_read(reader, offset, g_source_buf, chunk) != XST_SUCCESS ||
            qspi_nor_read(nor, flash_offset + offset,
                          g_flash_buf, chunk) != XST_SUCCESS) {
            return XST_FAILURE;
        }
        for (i = 0U; i < chunk; ++i) {
            const uint32_t absolute = offset + i;
            uint8_t expected = g_source_buf[i];
            if (ident_must_be_erased != 0 &&
                absolute >= BOOT_IDENT_WORD_OFFSET &&
                absolute < (BOOT_IDENT_WORD_OFFSET + 4U)) {
                expected = 0xFFU;
            }
            if (g_flash_buf[i] != expected) {
                return XST_FAILURE;
            }
        }
        if (ident_must_be_erased == 0 && offset < target->manifest.payload_size) {
            uint32_t crc_len = chunk;
            if ((offset + crc_len) > target->manifest.payload_size) {
                crc_len = target->manifest.payload_size - offset;
            }
            crc = crc32_update(crc, g_flash_buf, crc_len);
        }
        offset += chunk;
    }

    if (ident_must_be_erased == 0) {
        *payload_crc_out = crc32_finish(crc);
    }
    return XST_SUCCESS;
}

static int intermediate_slot_headers_invalid(qspi_nor_t *nor,
                                             uint32_t flash_offset)
{
    flash_reader_ctx_t flash_ctx;
    image_reader_t reader;

    flash_ctx.nor = nor;
    flash_ctx.base = flash_offset;
    reader.read = flash_read;
    reader.ctx = &flash_ctx;
    reader.size = GOLDEN_SLOT_SIZE;
    return alternate_boot_header_present(&reader, 0U, GOLDEN_SLOT_SIZE) ?
           XST_FAILURE : XST_SUCCESS;
}

static int final_slot_header_valid(qspi_nor_t *nor, uint32_t flash_offset)
{
    flash_reader_ctx_t flash_ctx;
    image_reader_t reader;

    flash_ctx.nor = nor;
    flash_ctx.base = flash_offset;
    reader.read = flash_read;
    reader.ctx = &flash_ctx;
    reader.size = GOLDEN_SLOT_SIZE;
    return boot_header_valid_at(&reader, 0U) ? XST_SUCCESS : XST_FAILURE;
}

static int qspi_preflight_init(qspi_nor_t *nor,
                               uint32_t *capacity_out,
                               golden_self_update_result_t *out)
{
    uint32_t capacity;

    if (qspi_nor_init(nor, QSPI_ADDRESS_BYTES, QSPI_ERASE_SIZE) != XST_SUCCESS) {
        result_error(out, GSU_E_QSPI_INIT, "QSPI initialization failed");
        return XST_FAILURE;
    }
    capacity = qspi_nor_capacity_bytes(nor);
    if (capacity < (FALLBACK_FLASH_OFFSET + FALLBACK_FLASH_SIZE)) {
        result_error(out, GSU_E_QSPI_INIT, "QSPI capacity does not cover the fixed flash layout");
        return XST_FAILURE;
    }
    log_printf("[GSU] Flash JEDEC %02X %02X %02X capacity=%lu\r\n",
               (unsigned int)nor->jedec_id[0],
               (unsigned int)nor->jedec_id[1],
               (unsigned int)nor->jedec_id[2],
               (unsigned long)capacity);
    *capacity_out = capacity;
    return XST_SUCCESS;
}

static int invalidate_slot_ident(qspi_nor_t *nor, uint32_t slot_offset)
{
    static const uint8_t zero_ident[4] = { 0U, 0U, 0U, 0U };
    uint8_t verify_ident[4];

    if (qspi_nor_program(nor, slot_offset + BOOT_IDENT_WORD_OFFSET,
                         zero_ident, sizeof(zero_ident)) != XST_SUCCESS ||
        qspi_nor_read(nor, slot_offset + BOOT_IDENT_WORD_OFFSET,
                      verify_ident, sizeof(verify_ident)) != XST_SUCCESS ||
        memcmp(verify_ident, zero_ident, sizeof(zero_ident)) != 0) {
        return XST_FAILURE;
    }
    return XST_SUCCESS;
}

static int install_slot_image(qspi_nor_t *nor,
                              uint32_t slot_offset,
                              const image_reader_t *reader,
                              const target_image_t *target,
                              const char *slot_name,
                              golden_self_update_result_t *out)
{
    uint8_t target_ident[4];
    uint32_t flash_crc = 0U;
    int ident_committed = 0;

    if (reader_read(reader, BOOT_IDENT_WORD_OFFSET,
                    target_ident, sizeof(target_ident)) != XST_SUCCESS) {
        result_error(out, GSU_E_SOURCE_READ, "cannot read source XLNX word");
        return XST_FAILURE;
    }

    out->flash_changed = 1;
    if (slot_offset == PRIMARY_FLASH_OFFSET) {
        log_puts("[GSU] Invalidating primary XLNX word before erase...\r\n");
        if (invalidate_slot_ident(nor, slot_offset) != XST_SUCCESS) {
            result_error(out, GSU_E_INVALIDATE,
                         "primary XLNX invalidation failed before erase");
            return XST_FAILURE;
        }
    }

    log_printf("[GSU] Erasing %s slot @0x%08lX...\r\n",
               slot_name, (unsigned long)slot_offset);
    if (qspi_nor_erase_region(nor, slot_offset, GOLDEN_SLOT_SIZE) != XST_SUCCESS) {
        result_error(out, GSU_E_ERASE, "golden slot erase failed");
        return XST_FAILURE;
    }

    log_printf("[GSU] Programming %s with XLNX word withheld...\r\n", slot_name);
    if (program_target_except_ident(nor, slot_offset, reader,
                                    target->total_size) != XST_SUCCESS) {
        result_error(out, GSU_E_PROGRAM, "golden slot programming failed");
        return XST_FAILURE;
    }
    if (verify_target(nor, slot_offset, reader, target, 1,
                      &flash_crc) != XST_SUCCESS) {
        result_error(out, GSU_E_VERIFY, "intermediate golden readback failed");
        return XST_FAILURE;
    }
    if (intermediate_slot_headers_invalid(nor, slot_offset) != XST_SUCCESS) {
        result_error(out, GSU_E_VERIFY,
                     "intermediate slot contains a valid multiboot header");
        return XST_FAILURE;
    }

    log_printf("[GSU] Committing %s XLNX word last...\r\n", slot_name);
    if (qspi_nor_program(nor, slot_offset + BOOT_IDENT_WORD_OFFSET,
                         target_ident, sizeof(target_ident)) != XST_SUCCESS) {
        result_error(out, GSU_E_FINAL_IDENT, "final golden XLNX programming failed");
        return XST_FAILURE;
    }
    ident_committed = 1;
    if (final_slot_header_valid(nor, slot_offset) != XST_SUCCESS) {
        if (invalidate_slot_ident(nor, slot_offset) != XST_SUCCESS &&
            slot_offset == PRIMARY_FLASH_OFFSET) {
            log_puts("[GSU] DO NOT POWER OFF: failed to clear the unverified primary XLNX word.\r\n");
        }
        result_error(out, GSU_E_FINAL_IDENT, "final golden boot header is invalid");
        return XST_FAILURE;
    }

    log_printf("[GSU] Verifying complete %s image...\r\n", slot_name);
    if (verify_target(nor, slot_offset, reader, target, 0,
                      &flash_crc) != XST_SUCCESS ||
        flash_crc != target->manifest.payload_crc32) {
        if (ident_committed != 0 &&
            invalidate_slot_ident(nor, slot_offset) != XST_SUCCESS) {
            if (slot_offset == PRIMARY_FLASH_OFFSET) {
                log_puts("[GSU] DO NOT POWER OFF: failed to clear the unverified primary XLNX word.\r\n");
            } else {
                log_puts("[GSU] WARNING: failed to clear the unverified trial XLNX word; primary is unchanged.\r\n");
            }
        }
        result_error(out, GSU_E_VERIFY, "final golden byte/CRC verification failed");
        return XST_FAILURE;
    }

    out->flash_crc32 = flash_crc;
    return XST_SUCCESS;
}

static int golden_self_update_common(const image_reader_t *reader,
                                     golden_self_update_result_t *out)
{
    target_image_t target;
    qspi_nor_t nor;
    uint32_t capacity;

    memset(&target, 0, sizeof(target));
    log_puts("[GSU] Preflighting golden target...\r\n");
    if (target_preflight(reader, &target, out) != XST_SUCCESS) {
        return XST_FAILURE;
    }
    if (qspi_preflight_init(&nor, &capacity, out) != XST_SUCCESS) {
        return XST_FAILURE;
    }

    log_puts("[GSU] Validating primary golden before touching trial...\r\n");
    if (primary_bootgen_preflight(&nor, out) != XST_SUCCESS) {
        return XST_FAILURE;
    }
    log_puts("[GSU] Validating recovery firmware fallback...\r\n");
    if (fallback_preflight(&nor, capacity, out) != XST_SUCCESS) {
        return XST_FAILURE;
    }

    if (install_slot_image(&nor, TRIAL_FLASH_OFFSET, reader, &target,
                           "trial", out) != XST_SUCCESS) {
        return XST_FAILURE;
    }

    out->updated = 1;
    out->verified = 1;
    out->reset_required = 1;
    out->boot_offset = TRIAL_FLASH_OFFSET;
    out->error_code = GSU_E_OK;
    out->error_msg[0] = '\0';
    log_printf("[GSU] Trial image verified size=%lu crc32=0x%08lX; boot @0x%08lX\r\n",
               (unsigned long)out->payload_size,
               (unsigned long)out->flash_crc32,
               (unsigned long)out->boot_offset);
    return XST_SUCCESS;
}

int golden_self_update_repair_primary(uint32_t current_multiboot_offset,
                                      uint32_t uart_base,
                                      uint32_t mirror_uart_base,
                                      golden_self_update_result_t *out)
{
    qspi_nor_t nor;
    flash_reader_ctx_t primary_ctx;
    flash_reader_ctx_t trial_header_ctx;
    flash_reader_ctx_t trial_image_ctx;
    image_reader_t primary_reader;
    image_reader_t trial_header_reader;
    image_reader_t trial_image_reader;
    target_image_t trial;
    uint32_t capacity;
    int primary_header_valid;
    int trial_header_valid;
    int repair_needed;

    if (out == NULL) {
        return XST_FAILURE;
    }
    result_init(out);
    g_log_uart = uart_base;
    g_log_mirror_uart = mirror_uart_base;
    memset(&trial, 0, sizeof(trial));

    if (qspi_preflight_init(&nor, &capacity, out) != XST_SUCCESS) {
        return XST_FAILURE;
    }
    flash_slot_reader_init(&nor, PRIMARY_FLASH_OFFSET,
                           &primary_ctx, &primary_reader);
    flash_slot_reader_init(&nor, TRIAL_FLASH_OFFSET,
                           &trial_header_ctx, &trial_header_reader);
    primary_header_valid = boot_header_valid_at(&primary_reader, 0U);
    trial_header_valid = boot_header_valid_at(&trial_header_reader, 0U);
    repair_needed = (current_multiboot_offset == TRIAL_FLASH_OFFSET) ||
                    (!primary_header_valid && trial_header_valid);

    if (!repair_needed) {
        if (!primary_header_valid) {
            result_error(out, GSU_E_PRIMARY_IMAGE,
                         "neither a running trial nor a valid primary permits repair");
            return XST_FAILURE;
        }
        out->error_code = GSU_E_OK;
        log_puts("[GSU] Primary golden header is valid; no repair needed.\r\n");
        return XST_SUCCESS;
    }
    if (!trial_header_valid) {
        result_error(out, GSU_E_TRIAL_IMAGE,
                     "trial boot was requested but its flash header is invalid");
        return XST_FAILURE;
    }

    log_puts("[GSU] Trial image is running; validating it before primary repair...\r\n");
    if (flash_golden_preflight(&nor, TRIAL_FLASH_OFFSET, &trial,
                               &trial_image_reader, &trial_image_ctx,
                               out) != XST_SUCCESS) {
        return XST_FAILURE;
    }
    log_puts("[GSU] Validating recovery firmware fallback before primary repair...\r\n");
    if (fallback_preflight(&nor, capacity, out) != XST_SUCCESS) {
        return XST_FAILURE;
    }

    out->found_source = 1;
    if (install_slot_image(&nor, PRIMARY_FLASH_OFFSET,
                           &trial_image_reader, &trial, "primary",
                           out) != XST_SUCCESS) {
        return XST_FAILURE;
    }

    out->updated = 1;
    out->verified = 1;
    out->reset_required = 1;
    out->boot_offset = PRIMARY_FLASH_OFFSET;
    out->error_code = GSU_E_OK;
    out->error_msg[0] = '\0';
    log_printf("[GSU] Primary repaired from trial size=%lu crc32=0x%08lX; reset @0x%08lX\r\n",
               (unsigned long)out->payload_size,
               (unsigned long)out->flash_crc32,
               (unsigned long)out->boot_offset);
    return XST_SUCCESS;
}

int golden_self_update_run(const char *source_path,
                           uint32_t uart_base,
                           uint32_t mirror_uart_base,
                           golden_self_update_result_t *out)
{
    FIL file;
    FRESULT fr;
    FSIZE_t file_size;
    file_reader_ctx_t file_ctx;
    image_reader_t reader;
    int rc;

    if (out == NULL) {
        return XST_FAILURE;
    }
    result_init(out);
    g_log_uart = uart_base;
    g_log_mirror_uart = mirror_uart_base;
    if (source_path == NULL || source_path[0] == '\0') {
        result_error(out, GSU_E_ARGUMENT, "invalid file update arguments");
        return XST_FAILURE;
    }

    fr = f_open(&file, source_path, FA_READ);
    if (fr != FR_OK) {
        result_error(out, GSU_E_SOURCE_OPEN, "cannot open golden source file");
        return XST_FAILURE;
    }
    out->found_source = 1;
    file_size = f_size(&file);
    if (file_size > (FSIZE_t)UINT32_MAX) {
        (void)f_close(&file);
        result_error(out, GSU_E_SOURCE_SIZE, "golden source file is too large");
        return XST_FAILURE;
    }

    file_ctx.file = &file;
    reader.read = file_read;
    reader.ctx = &file_ctx;
    reader.size = (uint32_t)file_size;
    rc = golden_self_update_common(&reader, out);
    fr = f_close(&file);
    if (rc != XST_SUCCESS) {
        return XST_FAILURE;
    }
    if (fr != FR_OK) {
        log_printf("[GSU] WARNING: golden update is verified, but source close failed fr=%d\r\n",
                   (int)fr);
    }

    /* Keep BOOT.BIN until the trial image has run and repaired primary A.
     * The caller starts updates only on command, so retaining the file cannot
     * start another update on its own. It also permits a clean retry if power
     * stops after B staging but before A repair. */
    log_printf("[GSU] Source file retained until primary repair completes\r\n");
    return XST_SUCCESS;
}

int golden_self_update_run_memory(const uint8_t *image,
                                  uint32_t image_size,
                                  uint32_t uart_base,
                                  uint32_t mirror_uart_base,
                                  golden_self_update_result_t *out)
{
    memory_reader_ctx_t memory_ctx;
    image_reader_t reader;

    if (out == NULL) {
        return XST_FAILURE;
    }
    result_init(out);
    g_log_uart = uart_base;
    g_log_mirror_uart = mirror_uart_base;
    if (image == NULL || image_size == 0U) {
        result_error(out, GSU_E_ARGUMENT, "invalid memory update arguments");
        return XST_FAILURE;
    }

    out->found_source = 1;
    memory_ctx.data = image;
    reader.read = memory_read;
    reader.ctx = &memory_ctx;
    reader.size = image_size;
    return golden_self_update_common(&reader, out);
}
