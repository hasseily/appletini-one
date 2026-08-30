#ifndef APPLETINI_GOLDEN_SELF_UPDATE_H
#define APPLETINI_GOLDEN_SELF_UPDATE_H

#include <stdint.h>

#define GOLDEN_SELF_UPDATE_MANIFEST_SIZE          32U
#define GOLDEN_SELF_UPDATE_MANIFEST_VERSION       1U
#define GOLDEN_SELF_UPDATE_ROLE_GOLDEN             1U
#define GOLDEN_SELF_UPDATE_ROLE_FIRMWARE           2U
#define GOLDEN_SELF_UPDATE_FLAG_RECOVERY           0x00000001U

#define GOLDEN_SELF_UPDATE_RECOVERY_MARKER         "APPLETINI-GOLDEN-RECOVERY-V1"

/* The golden region holds a fixed primary/trial pair. Updates always stage in
 * the trial slot. Code which has booted from the trial slot may then repair the
 * primary slot while leaving the verified trial copy intact. */
#define GOLDEN_SELF_UPDATE_PRIMARY_OFFSET           0x00000000U
#define GOLDEN_SELF_UPDATE_TRIAL_OFFSET             0x00100000U
#define GOLDEN_SELF_UPDATE_SLOT_SIZE                0x00100000U

typedef struct {
    uint8_t magic[8];
    uint32_t version;
    uint32_t role;
    uint32_t flags;
    uint32_t payload_size;
    uint32_t payload_crc32;
    uint32_t manifest_crc32;
} golden_self_update_manifest_t;

typedef struct {
    int found_source;
    int flash_changed;
    int updated;
    int verified;
    int reset_required;
    uint32_t payload_size;
    uint32_t payload_crc32;
    uint32_t flash_crc32;
    uint32_t boot_offset;
    int error_code;
    char error_msg[96];
} golden_self_update_result_t;

extern const char golden_self_update_recovery_marker[];

/* FatFs must already be mounted. The source file is a raw Zynq Bootgen image
 * followed by a 32-byte golden_self_update_manifest_t trailer. */
int golden_self_update_run(const char *source_path,
                           uint32_t uart_base,
                           uint32_t mirror_uart_base,
                           golden_self_update_result_t *out);

/* image points to the same payload-plus-trailer form used by the file API. */
int golden_self_update_run_memory(const uint8_t *image,
                                  uint32_t image_size,
                                  uint32_t uart_base,
                                  uint32_t mirror_uart_base,
                                  golden_self_update_result_t *out);

/* current_multiboot_offset is the current QSPI image byte offset, not the raw
 * MULTIBOOT_ADDR register value. When a verified trial image is running, this
 * copies it into the primary slot with the primary XLNX word committed last.
 * A successful repair sets reset_required and boot_offset to the primary slot.
 */
int golden_self_update_repair_primary(uint32_t current_multiboot_offset,
                                      uint32_t uart_base,
                                      uint32_t mirror_uart_base,
                                      golden_self_update_result_t *out);

#endif
