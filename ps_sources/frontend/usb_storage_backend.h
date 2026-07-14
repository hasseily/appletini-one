/******************************************************************************
* USB mass-storage backing device interface for Appletini hardware tests.
******************************************************************************/

#ifndef USB_STORAGE_BACKEND_H
#define USB_STORAGE_BACKEND_H

#include "xil_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define USB_STORAGE_BLOCK_SIZE          512U
#define USB_STORAGE_RAM_DISK_SIZE       (32U * 1024U * 1024U)
#define USB_STORAGE_MAX_TRANSFER_BLOCKS 256U
#define USB_STORAGE_MAX_TRANSFER_BYTES  (USB_STORAGE_BLOCK_SIZE * USB_STORAGE_MAX_TRANSFER_BLOCKS)

extern u8 UsbStorageVirtFlash[USB_STORAGE_RAM_DISK_SIZE];

typedef struct {
    u64 read_bytes;
    u64 write_bytes;
    u64 read_ticks;
    u64 write_ticks;
    u64 last_read_ticks;
    u64 last_write_ticks;
    u64 max_read_ticks;
    u64 max_write_ticks;
    u32 read_ops;
    u32 write_ops;
    u32 read_failures;
    u32 write_failures;
    u32 last_read_blocks;
    u32 last_write_blocks;
    u32 max_read_blocks;
    u32 max_write_blocks;
    u32 guard_rejects;
    u32 reinit_attempts;
    u32 reinit_successes;
} usb_storage_backend_stats_t;

const char *usb_storage_backend_name(void);
s32 usb_storage_init(void);
int usb_storage_medium_ready(void);
s32 usb_storage_try_reinit(void);
u32 usb_storage_block_count(void);
s32 usb_storage_read(u32 block, u32 count, u8 *buffer);
s32 usb_storage_write(u32 block, u32 count, const u8 *buffer);
void usb_storage_get_backend_stats(usb_storage_backend_stats_t *stats);
void usb_storage_reset_backend_stats(void);

#ifdef __cplusplus
}
#endif

#endif
