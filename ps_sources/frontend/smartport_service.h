#ifndef SMARTPORT_SERVICE_H
#define SMARTPORT_SERVICE_H

#include <stdint.h>

/* Initialize the SmartPort service. Mounts the SD card, opens the disk
 * image, and registers the smartport_irq handler with the shared GIC.
 * Returns 0 on success or a negative error code (most negatives are
 * negated FatFS FRESULT values; -100 indicates GIC registration
 * failure). The card is usable for status commands even if media mount
 * fails -- it will just report DEVICE_NOT_CONNECTED to the host. */
int smartport_service_init(uint32_t uart_base);

#define SMARTPORT_SERVICE_DEVICE_COUNT 8U
#define SMARTPORT_SERVICE_ALL_DEVICES  0xFFU
#define SMARTPORT_SERVICE_ERR_DUPLICATE_PATH (-3)

/* Select the SD-card image used by a SmartPort unit. Device numbers are SmartPort units 1..8,
 * matching the Apple-visible SP1..SP8 numbering.
 * Passing an empty path disables that unit. A non-empty path is rejected with
 * SMARTPORT_SERVICE_ERR_DUPLICATE_PATH if another unit already uses the same
 * image path.
 * The path must be a FatFS path such as
 * "0:/DISK1.hdv", "0:/DISK1.2mg", or "0:/DISK1.2img". Call
 * smartport_service_reset_media() after changing it if the service is already
 * running. */
int smartport_service_set_image_path(uint8_t device, const char *path);
const char *smartport_service_get_image_path(uint8_t device);

/* Polling entry point. The IRQ only records that a command is pending; FatFs,
 * cache-fill, writeback, and Apple-bus DMA work runs from main-loop context. */
void smartport_service_poll(void);

/* Reread the disk image (e.g. after the SD card has been swapped).
 * Returns 0 on success, negative FatFS code on failure. */
int smartport_service_reset_media(uint8_t device);

/* Read up to count bytes starting at logical block_num*512 from the mounted
 * disk image into buffer. *actual_out gets the actual bytes read. Returns 0
 * on success, negative FatFS code otherwise. */
int smartport_service_read_block(uint8_t device,
                                 uint32_t block_num,
                                 uint8_t *buffer,
                                 uint32_t count,
                                 uint32_t *actual_out);

typedef struct {
    uint8_t present_mask;
    uint8_t device;
    uint8_t read_only;
    uint32_t status_count;
    uint32_t read_count;
    uint32_t write_count;
} smartport_activity_t;

int smartport_service_get_activity(smartport_activity_t *out);

#define SMARTPORT_DMA_LOG_DEPTH 64U


/* Diagnostic readback of an Apple-addressed range through the same
 * soft-switch decoder used for SmartPort DMA. decode_rw_is_read must match
 * the destination transfer direction; use 1 for READBLOCK destinations. */

#endif

int smartport_service_verify_crclog(uint32_t uart_base);
