/******************************************************************************
* SD-card-backed USB mass-storage backend.
*
* All access goes through the xilffs disk_* API because xilffs exclusively
* owns the static XSdPs SdInstance[] in the BSP's diskio.c. f_mount may call
* disk_initialize and reconfigure the controller, so a second XSdPs instance
* could retain invalid block-size, sector-count, and clock state.
*
* Routing through disk_read/disk_write/disk_ioctl makes xilffs the single
* authority on the SD controller. disk_initialize is idempotent (checks
* STA_NOINIT), so calling it from multiple init paths is safe.
******************************************************************************/

#include "diskio.h"
#include "xil_cache.h"
#include "xstatus.h"
#include "xil_printf.h"
#include "sleep.h"
#include "xiltimer.h"
#include "xpseudo_asm.h"
#include "usb_storage_backend.h"
#include <string.h>

/* Cold-boot card detect / power-up settling can let the first
 * disk_initialize fail. Retry a few times with a delay between attempts
 * before giving up. */
#define SD_INIT_MAX_ATTEMPTS 4
#define SD_INIT_RETRY_USEC   200000U
#define SD_PDRV              0U

/* ARM CPSR mode field; 0x12 = IRQ mode. SD I/O must only ever run from
 * the main loop (xilffs/XSdPs polled I/O is not reentrant and is shared
 * with SmartPort/Disk2/config FATFS use, all of which are main-loop). */
#define CPSR_MODE_MASK 0x1FU
#define CPSR_MODE_IRQ  0x12U

static u32 SdBlockCount;
static u32 SdExportBaseBlock;
static u32 SdExportBlockCount;
static int SdReady;
static volatile int SdIoBusy;
static u8 ProbeBlock[USB_STORAGE_BLOCK_SIZE] __attribute__ ((aligned(32)));
static usb_storage_backend_stats_t SdStats;

/* Returns nonzero when SD I/O is allowed in the current context and
 * marks it busy; pairs with sd_io_end(). Rejections are counted so a
 * context violations (SD I/O from ISR context or reentry) are visible in
 * the `:usb` stats instead of silently corrupting XSdPs state. */
static int sd_io_begin(void)
{
    if ((mfcpsr() & CPSR_MODE_MASK) == CPSR_MODE_IRQ) {
        SdStats.guard_rejects++;
        return 0;
    }
    if (SdIoBusy != 0) {
        SdStats.guard_rejects++;
        return 0;
    }
    SdIoBusy = 1;
    return 1;
}

static void sd_io_end(void)
{
    SdIoBusy = 0;
}

static void update_backend_stats(u8 IsWrite,
                                 u32 Count,
                                 u64 Ticks,
                                 s32 Status)
{
    usb_storage_backend_stats_t *Stats = &SdStats;
    u64 Bytes = (u64)Count * (u64)USB_STORAGE_BLOCK_SIZE;

    if (IsWrite != 0U) {
        Stats->write_ops++;
        Stats->last_write_blocks = Count;
        Stats->last_write_ticks = Ticks;
        if (Count > Stats->max_write_blocks) {
            Stats->max_write_blocks = Count;
        }
        if (Ticks > Stats->max_write_ticks) {
            Stats->max_write_ticks = Ticks;
        }
        if (Status == XST_SUCCESS) {
            Stats->write_bytes += Bytes;
            Stats->write_ticks += Ticks;
        } else {
            Stats->write_failures++;
        }
    } else {
        Stats->read_ops++;
        Stats->last_read_blocks = Count;
        Stats->last_read_ticks = Ticks;
        if (Count > Stats->max_read_blocks) {
            Stats->max_read_blocks = Count;
        }
        if (Ticks > Stats->max_read_ticks) {
            Stats->max_read_ticks = Ticks;
        }
        if (Status == XST_SUCCESS) {
            Stats->read_bytes += Bytes;
            Stats->read_ticks += Ticks;
        } else {
            Stats->read_failures++;
        }
    }
}

static void invalidate_buffer(const void *Buffer, u32 Len)
{
    UINTPTR Addr;
    UINTPTR InvalidateAddr;
    u32 InvalidateLen;

    if ((Buffer == NULL) || (Len == 0U)) {
        return;
    }

    Addr = (UINTPTR)Buffer;
    InvalidateAddr = Addr & ~(UINTPTR)31U;
    InvalidateLen = (u32)((Addr - InvalidateAddr) + Len);
    InvalidateLen = (InvalidateLen + 31U) & ~31U;
    Xil_DCacheInvalidateRange((unsigned int)InvalidateAddr, InvalidateLen);
}

static s32 sd_read_blocks(u32 Block, u32 Count, u8 *Buffer)
{
    DRESULT dr;

    if (!sd_io_begin()) {
        return XST_FAILURE;
    }
    dr = disk_read(SD_PDRV, Buffer, (LBA_t)Block, (UINT)Count);
    sd_io_end();

    if (dr != RES_OK) {
        return XST_FAILURE;
    }
    /* disk_read uses ADMA2; invalidate so CPU sees the DMA payload. The
     * BSP driver does its own invalidate, but only when it knows the
     * caller's buffer alignment. Belt-and-suspenders. */
    invalidate_buffer(Buffer, Count * USB_STORAGE_BLOCK_SIZE);
    return XST_SUCCESS;
}

static s32 sd_write_blocks(u32 Block, u32 Count, const u8 *Buffer)
{
    UINTPTR Addr;
    UINTPTR FlushAddr;
    u32 FlushLen;
    DRESULT dr;

    Addr = (UINTPTR)Buffer;
    FlushAddr = Addr & ~(UINTPTR)31U;
    FlushLen = (u32)((Addr - FlushAddr) + (Count * USB_STORAGE_BLOCK_SIZE));
    FlushLen = (FlushLen + 31U) & ~31U;
    Xil_DCacheFlushRange((unsigned int)FlushAddr, FlushLen);

    if (!sd_io_begin()) {
        return XST_FAILURE;
    }
    dr = disk_write(SD_PDRV, Buffer, (LBA_t)Block, (UINT)Count);
    sd_io_end();
    return (dr == RES_OK) ? XST_SUCCESS : XST_FAILURE;
}

static u16 le16_at(const u8 *Buffer)
{
    return (u16)Buffer[0] | ((u16)Buffer[1] << 8);
}

static u32 le32_at(const u8 *Buffer)
{
    return (u32)Buffer[0] |
           ((u32)Buffer[1] << 8) |
           ((u32)Buffer[2] << 16) |
           ((u32)Buffer[3] << 24);
}

const char *usb_storage_backend_name(void)
{
    return "raw SD card";
}

static s32 sd_attach_internal(int InitAttempts, int ProbeAttempts, u32 RetryUsec)
{
    DSTATUS st = STA_NOINIT;
    DWORD sectors = 0;
    s32 Status;
    int Attempt;
    DRESULT IoctlResult;

    SdReady = 0;

    for (Attempt = 0; Attempt < InitAttempts; Attempt++) {
        if (!sd_io_begin()) {
            return XST_FAILURE;
        }
        st = disk_initialize(SD_PDRV);
        sd_io_end();
        if ((st & STA_NOINIT) == 0U) {
            break;
        }
        xil_printf("SD backend: disk_initialize attempt %d st=0x%02x\r\n",
                   Attempt, (unsigned)st);
        if ((RetryUsec != 0U) && (Attempt + 1 < InitAttempts)) {
            usleep(RetryUsec);
        }
    }
    if ((st & STA_NOINIT) != 0U) {
        return XST_FAILURE;
    }

    if (!sd_io_begin()) {
        return XST_FAILURE;
    }
    IoctlResult = disk_ioctl(SD_PDRV, GET_SECTOR_COUNT, &sectors);
    sd_io_end();
    if (IoctlResult != RES_OK) {
        xil_printf("SD backend: GET_SECTOR_COUNT failed\r\n");
        return XST_FAILURE;
    }
    SdBlockCount = (u32)sectors;
    SdExportBaseBlock = 0U;
    SdExportBlockCount = SdBlockCount;
    SdReady = 1;

    Status = XST_FAILURE;
    for (Attempt = 0; Attempt < ProbeAttempts; Attempt++) {
        Status = sd_read_blocks(0U, 1U, ProbeBlock);
        if (Status == XST_SUCCESS) {
            break;
        }
        xil_printf("SD backend: block 0 read attempt %d failed %d\r\n",
                   Attempt, (int)Status);
        if ((RetryUsec != 0U) && (Attempt + 1 < ProbeAttempts)) {
            usleep(RetryUsec);
        }
    }
    if (Status != XST_SUCCESS) {
        SdReady = 0;
        return Status;
    }

    {
        const u32 Part0Offset = 0x1BEU;
        u16 MbrSig = le16_at(&ProbeBlock[510]);
        u8 Part0Type = ProbeBlock[Part0Offset + 4U];
        u32 Part0Lba = le32_at(&ProbeBlock[Part0Offset + 8U]);
        u32 Part0Blocks = le32_at(&ProbeBlock[Part0Offset + 12U]);

        xil_printf("SD backend: block0=%02x %02x %02x %02x sig=%04x p0 type=%02x lba=%u count=%u\r\n",
                   ProbeBlock[0], ProbeBlock[1], ProbeBlock[2], ProbeBlock[3],
                   (unsigned)MbrSig, (unsigned)Part0Type,
                   (unsigned)Part0Lba, (unsigned)Part0Blocks);

        if ((MbrSig == 0xAA55U) && (Part0Type != 0U) && (Part0Lba < SdBlockCount)) {
            Status = sd_read_blocks(Part0Lba, 1U, ProbeBlock);
            if (Status == XST_SUCCESS) {
                u16 BootSig = le16_at(&ProbeBlock[510]);
                u16 ReservedSectors = le16_at(&ProbeBlock[14]);
                u16 FsInfoSector = le16_at(&ProbeBlock[48]);
                xil_printf("SD backend: p0 boot=%02x %02x %02x %02x %02x %02x %02x %02x sig=%04x\r\n",
                           ProbeBlock[0], ProbeBlock[1], ProbeBlock[2], ProbeBlock[3],
                           ProbeBlock[4], ProbeBlock[5], ProbeBlock[6], ProbeBlock[7],
                           (unsigned)BootSig);
                xil_printf("SD backend: p0 bpb bps=%u spc=%u rsv=%u fats=%u spf=%u root=%u fsinfo=%u backup=%u flags=%04x ver=%04x\r\n",
                           (unsigned)le16_at(&ProbeBlock[11]),
                           (unsigned)ProbeBlock[13],
                           (unsigned)le16_at(&ProbeBlock[14]),
                           (unsigned)ProbeBlock[16],
                           (unsigned)le32_at(&ProbeBlock[36]),
                           (unsigned)le32_at(&ProbeBlock[44]),
                           (unsigned)le16_at(&ProbeBlock[48]),
                           (unsigned)le16_at(&ProbeBlock[50]),
                           (unsigned)le16_at(&ProbeBlock[40]),
                           (unsigned)le16_at(&ProbeBlock[42]));
                if ((FsInfoSector != 0U) && ((Part0Lba + FsInfoSector) < SdBlockCount) &&
                    (sd_read_blocks(Part0Lba + FsInfoSector, 1U, ProbeBlock) == XST_SUCCESS)) {
                    xil_printf("SD backend: p0 fsinfo lead=%08x struc=%08x free=%u next=%u trail=%08x\r\n",
                               (unsigned)le32_at(&ProbeBlock[0]),
                               (unsigned)le32_at(&ProbeBlock[484]),
                               (unsigned)le32_at(&ProbeBlock[488]),
                               (unsigned)le32_at(&ProbeBlock[492]),
                               (unsigned)le32_at(&ProbeBlock[508]));
                }
                if ((ReservedSectors != 0U) && ((Part0Lba + ReservedSectors) < SdBlockCount) &&
                    (sd_read_blocks(Part0Lba + ReservedSectors, 1U, ProbeBlock) == XST_SUCCESS)) {
                    xil_printf("SD backend: p0 fat0[0]=%08x fat0[1]=%08x\r\n",
                               (unsigned)le32_at(&ProbeBlock[0]),
                               (unsigned)le32_at(&ProbeBlock[4]));
                }
                if ((BootSig == 0xAA55U) &&
                    (Part0Blocks != 0U) &&
                    (Part0Blocks <= (SdBlockCount - Part0Lba))) {
                    SdExportBaseBlock = Part0Lba;
                    SdExportBlockCount = Part0Blocks;
                    xil_printf("SD backend: exporting partition 0 base=%u blocks=%u MB=%u\r\n",
                               (unsigned)SdExportBaseBlock,
                               (unsigned)SdExportBlockCount,
                               (unsigned)(SdExportBlockCount / 2048U));
                }
            } else {
                xil_printf("SD backend: p0 read failed lba=%u rc=%d\r\n",
                           (unsigned)Part0Lba, (int)Status);
            }
        }
    }

    xil_printf("SD backend: blocks=%u (0x%08x) MB=%u\r\n",
               (unsigned)SdBlockCount, (unsigned)SdBlockCount,
               (unsigned)(SdBlockCount / 2048U));
    return XST_SUCCESS;
}

s32 usb_storage_init(void)
{
    return sd_attach_internal(SD_INIT_MAX_ATTEMPTS, SD_INIT_MAX_ATTEMPTS,
                              SD_INIT_RETRY_USEC);
}

int usb_storage_medium_ready(void)
{
    return SdReady;
}

/* One quick attach attempt, no settling sleeps. Called from the main-loop
 * poll (rate-limited there) so a card that was absent or not yet settled
 * at boot -- or was hot-removed -- comes back without a reboot. */
s32 usb_storage_try_reinit(void)
{
    if (SdReady) {
        return XST_SUCCESS;
    }
    SdStats.reinit_attempts++;
    if (sd_attach_internal(1, 1, 0U) != XST_SUCCESS) {
        return XST_FAILURE;
    }
    SdStats.reinit_successes++;
    return XST_SUCCESS;
}

/* On an I/O failure, ask the disk layer whether the medium is actually
 * gone (card removed / controller back to uninitialized). Dropping
 * SdReady routes the host to medium-not-present + recovery instead of an
 * endless string of failing commands against a dead card. */
static void sd_note_io_failure(void)
{
    DSTATUS st;

    if (!sd_io_begin()) {
        return;
    }
    st = disk_status(SD_PDRV);
    sd_io_end();
    if ((st & (STA_NOINIT | STA_NODISK)) != 0U) {
        if (SdReady) {
            xil_printf("SD backend: medium lost st=0x%02x\r\n", (unsigned)st);
        }
        SdReady = 0;
    }
}

u32 usb_storage_block_count(void)
{
    return SdReady ? SdExportBlockCount : 0U;
}

s32 usb_storage_read(u32 block, u32 count, u8 *buffer)
{
    XTime T0;
    XTime T1;
    s32 Status;

    if (!SdReady || (count == 0U) ||
        (block >= SdExportBlockCount) || (count > (SdExportBlockCount - block))) {
        update_backend_stats(0U, count, 0U, XST_FAILURE);
        return XST_FAILURE;
    }

    XTime_GetTime(&T0);
    Status = sd_read_blocks(SdExportBaseBlock + block, count, buffer);
    XTime_GetTime(&T1);
    update_backend_stats(0U, count, (u64)(T1 - T0), Status);
    if (Status != XST_SUCCESS) {
        sd_note_io_failure();
    }
    return Status;
}

s32 usb_storage_write(u32 block, u32 count, const u8 *buffer)
{
    XTime T0;
    XTime T1;
    s32 Status;

    if (!SdReady || (count == 0U) ||
        (block >= SdExportBlockCount) || (count > (SdExportBlockCount - block))) {
        update_backend_stats(1U, count, 0U, XST_FAILURE);
        return XST_FAILURE;
    }

    XTime_GetTime(&T0);
    Status = sd_write_blocks(SdExportBaseBlock + block, count, buffer);
    XTime_GetTime(&T1);
    update_backend_stats(1U, count, (u64)(T1 - T0), Status);
    if (Status != XST_SUCCESS) {
        sd_note_io_failure();
    }
    return Status;
}

void usb_storage_get_backend_stats(usb_storage_backend_stats_t *stats)
{
    if (stats != NULL) {
        *stats = SdStats;
    }
}

void usb_storage_reset_backend_stats(void)
{
    memset(&SdStats, 0, sizeof(SdStats));
}
