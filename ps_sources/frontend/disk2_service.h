#ifndef DISK2_SERVICE_H
#define DISK2_SERVICE_H

#include <stdint.h>

#define DISK2_DRIVE_COUNT 2U
#define DISK2_IMAGE_PATH_MAX 128U
/* Track staging uses a non-cacheable DDR megabyte through the HP3
 * disk2_ddr_bridge. The card register holds a line offset from this base,
 * and PL AXI reads therefore see current staging data without cache flushes. */
#define DISK2_STAGING_BASE_OFFSET 0x00000000U
#define DISK2_DDR_STAGING_BASE  0x3D800000U
#define DISK2_WOZ_TMAP_SIZE 160U

typedef enum {
    DISK2_IMAGE_NONE = 0,
    DISK2_IMAGE_WOZ,
    DISK2_IMAGE_NIB,
    DISK2_IMAGE_DSK,
    DISK2_IMAGE_DO,
    DISK2_IMAGE_PO
} disk2_image_format_t;

typedef struct {
    uint8_t present;
    uint8_t read_only;
    disk2_image_format_t format;
    uint32_t file_size;
    uint32_t track_count;
    uint32_t logical_blocks;
    uint8_t woz_version;
    uint8_t woz_optimal_bit_timing;
    uint8_t woz_tmap[DISK2_WOZ_TMAP_SIZE];
    uint32_t woz_info_offset;
    uint32_t woz_tmap_offset;
    uint32_t woz_trks_offset;
    uint32_t woz_trks_size;
} disk2_image_info_t;

typedef struct {
    uint8_t loaded;
    uint8_t drive;
    uint8_t track;
    uint8_t qtrack;
    uint8_t first_addr16_valid;
    uint8_t first_addr16_volume;
    uint8_t first_addr16_track;
    uint8_t first_addr16_sector;
    uint8_t first_addr16_checksum;
    uint8_t first_addr16_checksum_ok;
    uint32_t length;
    uint32_t addr16_count;
    uint32_t addr13_count;
    uint32_t data_count;
    uint32_t first_addr16;
    uint32_t first_addr13;
    uint32_t first_data;
} disk2_track_scan_t;

typedef struct {
    uint8_t present_mask;
    uint8_t enabled;
    uint8_t motor_on;
    uint8_t spinning;
    uint8_t drive;
    uint8_t phase;
    uint8_t write_busy;
    uint8_t write_dirty;
    uint8_t write_drive;
    uint8_t write_qtrack;
    uint32_t io_count;
    uint32_t read_count;
    uint32_t write_count;
} disk2_activity_t;

int disk2_service_init(uint32_t uart_base);
void disk2_service_set_enabled(uint8_t enabled);
void disk2_service_poll(void);

/* Force-flush a pending dirty track to the image file, ignoring the
 * motor-on deferral. For use when another SD owner is about to take over
 * (USB0 SD remote mount): CP/M's PCPI BIOS can hold the drive enabled
 * indefinitely, which would otherwise leave the file stale while the
 * host rewrites the card. Returns 0 when nothing is pending (or the
 * flush landed), negative when the caller should retry. */
int disk2_service_flush_dirty_now(void);

int disk2_service_set_image_path(uint8_t drive, const char *path);
const char *disk2_service_get_image_path(uint8_t drive);

int disk2_service_reset_media(uint8_t drive);
int disk2_service_get_image_info(uint8_t drive, disk2_image_info_t *out);
int disk2_service_set_woz_write_enable(uint8_t drive, uint8_t enable);
uint8_t disk2_service_get_woz_write_enable(uint8_t drive);
int disk2_service_verify_staged(uint32_t *first_mismatch,
                                uint32_t *mismatch_count);
int disk2_service_scan_loaded_track(disk2_track_scan_t *out);
int disk2_service_scan_file_track(uint8_t drive, uint8_t track, disk2_track_scan_t *out);
int disk2_service_wozscan_loaded_track(disk2_track_scan_t *out);
int disk2_service_wozscan_file_track(uint8_t drive, uint8_t track, disk2_track_scan_t *out);
int disk2_service_get_activity(disk2_activity_t *out);
const char *disk2_service_format_name(disk2_image_format_t format);

#endif
