#ifndef APPLETINI_FW_UPDATE_METADATA_H
#define APPLETINI_FW_UPDATE_METADATA_H

#include <stdint.h>

#define FW_UPDATE_METADATA_MAGIC    0x46574D54U /* "FWMT" */
#define FW_UPDATE_METADATA_VERSION  4U
#define FW_UPDATE_METADATA_STRLEN   32U

#define FW_UPDATE_METADATA_FLAG_FIRMWARE_VERIFIED 0x00000001U

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t length_bytes;
    uint32_t seq;

    uint32_t golden_offset;
    uint32_t golden_size;
    uint32_t firmware_offset;
    uint32_t firmware_size;

    uint32_t flags;

    char golden_version[FW_UPDATE_METADATA_STRLEN];
    char layout_label[FW_UPDATE_METADATA_STRLEN];
    char reserved0[FW_UPDATE_METADATA_STRLEN];
    char reserved1[FW_UPDATE_METADATA_STRLEN];

    uint32_t crc32;
} fw_update_metadata_t;

#endif
