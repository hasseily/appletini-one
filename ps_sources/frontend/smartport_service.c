/* SmartPort service: PS-side command-execution backend for the
 * smartport_card on the PL.
 *
 * The 6502 ROM streams command bytes into the PL DATA FIFO, writes CTRL
 * to request execution, then polls CTRL bit7. The PS drains the FIFO,
 * performs the requested status / read / write operation, pushes the
 * response bytes to the OUT FIFO, and sets READY only after the full
 * response is available. The physical 6502 copies every payload byte through
 * the card FIFOs. */

#include "smartport_service.h"

#include <stdint.h>
#include <string.h>

#include "xil_cache.h"
#include "xil_mmu.h"
#include "xil_exception.h"
#include "xparameters.h"
#include "xscugic.h"
#include "ff.h"

#include "../lib/common.h"
#include "../lib/uart.h"
#include "gic_init.h"
#include <stdio.h>
#include "xil_mmu.h"

/* ------------------------------------------------------------------ */
/* MMIO layout                                                         */
/* ------------------------------------------------------------------ */

#define SP_BASE                 0x40020000U
#define SP_REG(idx)             (SP_BASE + ((idx) * 4U))   /* AxiSimple word stride */

/* a2retronet-model card registers (word indices on the AxiSimple bus).
 * See smartport_card.sv for the authoritative map. */
#define SP_R_STATUS             SP_REG(0)
#define SP_R_IN_HEAD            SP_REG(1)
#define SP_R_OUT_PUSH           SP_REG(2)
#define SP_R_CONTROL            SP_REG(3)
#define SP_R_SSS                SP_REG(4)

#define SP_ST_IN_COUNT(v)       ((v) & 0x7FFU)
#define SP_ST_EXEC_PENDING      (1UL << 28)
#define SP_ST_READY             (1UL << 29)

#define SP_CTL_POP_IN           1U
#define SP_CTL_CLR_IN           2U
#define SP_CTL_CLR_OUT          4U
#define SP_CTL_SET_READY        8U
#define SP_CTL_ACK_EXEC         16U

/* CTRL values the firmware writes to trigger execution. */
#define SP_FAMILY_PRODOS        0x01U
#define SP_FAMILY_SP            0x02U

/* SmartPort block and status staging cache in DDR. */
#define SP_BLOCK_SIZE           512U
#define SP_CACHE_DDR_BASE       0x3C000000U

/* RamFactor/Slinky-style volatile RAM disk: a 32 MB DDR-backed
 * SmartPort unit, pre-formatted as an empty ProDOS volume ("RAM32")
 * at mount. Contents do not survive power-off, like the originals.
 * 0x30000000-0x31FFFFFF is reserved DDR, above the AMP core-1 image
 * and below the SmartPort block cache / compositor windows; CPU-only
 * access, so default cached mapping is correct and fast. ProDOS block
 * count is 16-bit, so expose 65535 blocks and leave the final 512-byte
 * sector unavailable. */
#define SP_RAMDISK_BASE          0x30000000U
#define SP_RAMDISK_BLOCKS        65535U
#define SP_RAMDISK_BITMAP_BLOCK  6U
#define SP_RAMDISK_BITMAP_BLOCKS 16U
#define SP_RAMDISK_USED_BLOCKS   (SP_RAMDISK_BITMAP_BLOCK + SP_RAMDISK_BITMAP_BLOCKS)
#define SP_CACHE_BLOCK_COUNT    32U
#define SP_STATUS_DDR_BASE      (SP_CACHE_DDR_BASE + (SP_CACHE_BLOCK_COUNT * SP_BLOCK_SIZE))
#define SP_DMA_DDR_TLB_BYTES    0x00100000U
#define SP_MAX_DEVICES          8U

/* ------------------------------------------------------------------ */
/* AppleWin Harddisk.cpp constants                                     */
/* ------------------------------------------------------------------ */

#define BLK_CMD_STATUS          0x00U
#define BLK_CMD_READ            0x01U
#define BLK_CMD_WRITE           0x02U

#define SP_CMD_BASE             0x80U
#define SP_CMD_STATUS           0x80U
#define SP_CMD_READBLOCK        0x81U
#define SP_CMD_WRITEBLOCK       0x82U
#define SP_CMD_FORMAT           0x83U
#define SP_CMD_BUSYSTATUS       0xBFU

#define SP_STATUS_STATUS        0x00U
#define SP_STATUS_GETDIB        0x03U

#define ERR_DEVICE_OK           0x00U
#define ERR_BADCTL              0x21U
#define ERR_DEVICE_IO_ERROR     0x27U
#define ERR_DEVICE_NOT_CONNECTED 0x28U
#define ERR_NOWRITE             0x2BU

/* ------------------------------------------------------------------ */
/* IRQ wiring (smartport_irq is on IRQ_F2P[1] -> GIC SPI 62)           */
/* ------------------------------------------------------------------ */

#define SMARTPORT_IRQ_ID        62U

/* ------------------------------------------------------------------ */
/* Module state                                                        */
/* ------------------------------------------------------------------ */

#define SP_IMAGE_PATH_MAX       128U
#define SP_2MG_HEADER_SIZE      64U
#define SP_2MG_MAGIC            0x474D4932U  /* "2IMG", little endian */
#define SP_2MG_FORMAT_PRODOS    1U
#define SP_2MG_FLAG_LOCKED      0x80000000U

typedef struct {
    FIL      image_file;
    uint8_t  image_open;
    uint8_t  read_only;
    uint8_t  is_ram;           /* DDR-backed RAM disk, no file behind it */
    uint32_t image_data_offset;
    uint32_t image_blocks;
    char     image_path[SP_IMAGE_PATH_MAX];
} sp_device_t;

typedef struct {
    uint8_t  valid;
    uint8_t  device_index;
    uint32_t block_num;
    uint32_t last_used;
} sp_cache_entry_t;

static FATFS    g_fatfs;
static uint8_t  g_fs_mounted    = 0U;
static uint32_t g_uart_base     = 0U;
static uint8_t  g_smartport_slot = 0U;
static sp_device_t g_devices[SP_MAX_DEVICES];
static uint8_t g_devices_initialized = 0U;
static sp_cache_entry_t g_cache[SP_CACHE_BLOCK_COUNT];
static uint32_t g_cache_clock = 0U;
static uint8_t g_activity_device = 0U;
static uint32_t g_activity_status_count = 0U;
static uint32_t g_activity_read_count = 0U;
static uint32_t g_activity_write_count = 0U;
static uint8_t g_ramdisk_state = 0U;   /* 0 unknown, 1 mounted, 2 off */

static volatile uint32_t g_irq_count = 0U;

/* Command-pending counter. The ISR increments this for each smartport
 * IRQ; smartport_service_poll() services one command per call as long
 * as the count is non-zero, decrementing on completion. The PL holds
 * the Apple bus stalled until status is posted, so it won't fire a new
 * IRQ until we ack the previous one -- in practice the count is 0 or 1.
 *
 * Volatile + aligned 32-bit makes ISR/poll access atomic on Cortex-A9
 * without explicit critical sections. */
static volatile uint32_t g_cmd_pending_count = 0U;

static uint8_t  g_scratch[SP_BLOCK_SIZE];   /* per-command staging */
static uint8_t  g_cmd_buf[1024];            /* drained command frame */

/* ------------------------------------------------------------------ */
/* Low-level helpers                                                   */
/* ------------------------------------------------------------------ */

static inline uint32_t sp_hw_status(void)
{
    return REG_READ(SP_R_STATUS);
}

static inline void sp_ctl(uint32_t bits)
{
    REG_WRITE(SP_R_CONTROL, bits);
}

static inline void sp_push(uint8_t b)
{
    REG_WRITE(SP_R_OUT_PUSH, (uint32_t)b);
}

static void sp_push_buf(const uint8_t *p, uint32_t n)
{
    while (n--) {
        sp_push(*p++);
    }
}

/* Drain every queued command byte from the card's IN FIFO. The Apple-side ROM
 * queues the complete frame before writing CTRL, so execution sees it whole. */
static uint32_t sp_drain(uint8_t *buf, uint32_t max)
{
    uint32_t n = 0U;
    while (n < max) {
        const uint32_t head = REG_READ(SP_R_IN_HEAD);
        if ((head & 0x100U) == 0U) {
            break;
        }
        buf[n++] = (uint8_t)head;
        sp_ctl(SP_CTL_POP_IN);
    }
    return n;
}

static inline uint8_t sp_encode_status(uint8_t error_code)
{
    /* Mirrors smartport_card.sv encode_status_byte() but with the busy
     * bit forced clear (we only call this once the operation is done):
     *   {1'b0, error_code[5:0], (error_code != OK)}                  */
    uint8_t err_bit = (error_code != ERR_DEVICE_OK) ? 1U : 0U;
    return (uint8_t)((error_code & 0x3FU) << 1) | err_bit;
}

static uint8_t ascii_lower(uint8_t c)
{
    if (c >= (uint8_t)'A' && c <= (uint8_t)'Z') {
        return (uint8_t)(c + ((uint8_t)'a' - (uint8_t)'A'));
    }
    return c;
}

static uint8_t path_eq_char(char c)
{
    if (c == '\\') {
        c = '/';
    }
    return ascii_lower((uint8_t)c);
}

static uint8_t path_ieq(const char *a, const char *b)
{
    if (a == NULL || b == NULL || a[0] == '\0' || b[0] == '\0') {
        return 0U;
    }
    while (*a != '\0' && *b != '\0') {
        if (path_eq_char(*a) != path_eq_char(*b)) {
            return 0U;
        }
        ++a;
        ++b;
    }
    return (*a == '\0' && *b == '\0') ? 1U : 0U;
}

static uint32_t le32_load(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static const char *sp_default_path(uint8_t device)
{
    static const char * const paths[SP_MAX_DEVICES] = {
        "0:/DISK1.hdv",
        "0:/DISK2.hdv",
        "0:/DISK3.hdv",
        "0:/DISK4.hdv",
        "0:/DISK5.hdv",
        "0:/DISK6.hdv",
        "0:/DISK7.hdv",
        "0:/DISK8.hdv"
    };

    return (device < SP_MAX_DEVICES) ? paths[device] : "";
}

static void smartport_devices_ensure_defaults(void)
{
    uint8_t i;

    if (g_devices_initialized != 0U) {
        return;
    }

    memset(g_devices, 0, sizeof(g_devices));
    for (i = 0U; i < SP_MAX_DEVICES; ++i) {
        const char *path = sp_default_path(i);
        size_t len = strlen(path);
        if (len >= SP_IMAGE_PATH_MAX) {
            len = SP_IMAGE_PATH_MAX - 1U;
        }
        memcpy(g_devices[i].image_path, path, len);
        g_devices[i].image_path[len] = '\0';
        g_devices[i].read_only = 1U;
    }
    g_devices_initialized = 1U;
}

static uint8_t device_index_from_ptr(const sp_device_t *dev)
{
    return (uint8_t)(dev - g_devices);
}

static int service_device_to_index(uint8_t device, uint8_t *index_out)
{
    if (index_out == NULL) {
        return -1;
    }
    if (device == 0U || device > SP_MAX_DEVICES) {
        return -1;
    }
    *index_out = (uint8_t)(device - 1U);
    return 0;
}

static uint8_t sp_image_path_duplicate(uint8_t device_index, const char *path)
{
    uint8_t i;

    if (path == NULL || path[0] == '\0') {
        return 0U;
    }

    for (i = 0U; i < SP_MAX_DEVICES; ++i) {
        if (i != device_index &&
            path_ieq(path, g_devices[i].image_path) != 0U) {
            return 1U;
        }
    }
    return 0U;
}

static uint8_t smartport_present_count(void)
{
    uint8_t i;
    uint8_t count = 0U;

    for (i = 0U; i < SP_MAX_DEVICES; ++i) {
        if (g_devices[i].image_open != 0U) {
            count++;
        }
    }
    return count;
}

static uint8_t smartport_present_mask(void)
{
    uint8_t i;
    uint8_t mask = 0U;

    for (i = 0U; i < SP_MAX_DEVICES; ++i) {
        if (g_devices[i].image_open != 0U) {
            mask |= (uint8_t)(1U << i);
        }
    }
    return mask;
}

static uint32_t cache_addr_for_index(uint32_t index)
{
    return SP_CACHE_DDR_BASE + (index * SP_BLOCK_SIZE);
}

static uint8_t *cache_ptr_from_addr(uint32_t cache_addr)
{
    return (uint8_t *)(uintptr_t)cache_addr;
}

static void sp_cache_invalidate_device(uint8_t device_index)
{
    uint32_t i;

    for (i = 0U; i < SP_CACHE_BLOCK_COUNT; ++i) {
        if (device_index == SMARTPORT_SERVICE_ALL_DEVICES ||
            (g_cache[i].valid != 0U && g_cache[i].device_index == device_index)) {
            g_cache[i].valid = 0U;
        }
    }
}

static int ensure_sd_mounted(void)
{
    FRESULT fr;

    if (g_fs_mounted != 0U) {
        return 0;
    }

    fr = f_mount(&g_fatfs, "0:/", 1U);
    if (fr != FR_OK) {
        g_fs_mounted = 0U;
        return -(int)fr;
    }
    g_fs_mounted = 1U;
    return 0;
}

/* SmartPort payloads move exclusively through the card DATA FIFOs. */

static const char ID_STR_CTRL[13] = { 12, 'A','p','p','l','e','t','i','n','i',' ','S','P' };
static const char ID_STR_DEV [13] = { 12, 'A','p','p','l','e','t','i','n','i',' ','H','D' };

#define FW_VER_MAJOR  1U
#define FW_VER_MINOR  0U

/* Build a status block in g_scratch. Returns the length, or 0 if the
 * (unit, status_code) pair is unsupported. */
static uint16_t build_sp_status(sp_device_t *dev,
                                uint8_t unit,
                                uint8_t status_code)
{
    uint32_t blocks = (dev != NULL) ? dev->image_blocks : 0U;

    memset(g_scratch, 0, sizeof(g_scratch));

    if (unit == 0x00U) {
        /* Unit 0 = SmartPort controller. Always present. */
        switch (status_code) {
        case SP_STATUS_STATUS:
            g_scratch[0] = smartport_present_count();
            return 8U;
        case SP_STATUS_GETDIB: {
            uint16_t i;
            g_scratch[0] = smartport_present_count();
            for (i = 0U; i < 13U; ++i) {
                g_scratch[8U + i] = (uint8_t)ID_STR_CTRL[i];
            }
            for (i = 21U; i < 25U; ++i) g_scratch[i] = ' ';
            g_scratch[27] = FW_VER_MAJOR;
            g_scratch[28] = FW_VER_MINOR;
            return 29U;
        }
        default:
            return 0U;
        }
    }
    else if (dev != NULL && dev->image_open != 0U) {
        uint8_t general = dev->read_only ? 0xFCU : 0xF8U;
        uint16_t i;
        switch (status_code) {
        case SP_STATUS_STATUS:
            g_scratch[0] = general;
            g_scratch[1] = (uint8_t)(blocks       & 0xFFU);
            g_scratch[2] = (uint8_t)((blocks >> 8) & 0xFFU);
            g_scratch[3] = (uint8_t)((blocks >> 16) & 0xFFU);
            return 4U;
        case SP_STATUS_GETDIB:
            g_scratch[0] = general;
            g_scratch[1] = (uint8_t)(blocks       & 0xFFU);
            g_scratch[2] = (uint8_t)((blocks >> 8) & 0xFFU);
            g_scratch[3] = (uint8_t)((blocks >> 16) & 0xFFU);
            for (i = 0U; i < 13U; ++i) {
                g_scratch[4U + i] = (uint8_t)ID_STR_DEV[i];
            }
            for (i = 17U; i < 21U; ++i) g_scratch[i] = ' ';
            g_scratch[21] = 0x02U;       /* device type: hard disk */
            g_scratch[22] = 0x20U;       /* device subtype */
            g_scratch[23] = FW_VER_MAJOR;
            g_scratch[24] = FW_VER_MINOR;
            return 25U;
        default:
            return 0U;
        }
    }
    return 0U;
}

/* ------------------------------------------------------------------ */
/* Command execution                                                   */
/* ------------------------------------------------------------------ */

static int sp_cache_get_block(sp_device_t *dev,
                              uint32_t block_num,
                              uint8_t for_write,
                              uint32_t *cache_addr);
static void sp_ramdisk_refresh(uint32_t uart_base);

/* The slot is learned from the ROM-generated unit byte. The ProDOS BLK
 * protocol has one drive bit, so it can only address SP1
 * and SP2. Full eight-device support is through SmartPort units 1..8. */
static sp_device_t *device_for_blk_unit(uint8_t unit)
{
    uint8_t slot   = (uint8_t)((unit >> 4) & 0x07U);
    uint8_t drive  = (uint8_t)((unit >> 7) & 0x01U);

    if (slot == 0U) {
        return NULL;
    }
    if (g_smartport_slot == 0U) {
        g_smartport_slot = slot;
    }
    if (slot != g_smartport_slot) {
        return NULL;
    }
    return &g_devices[drive];
}

static sp_device_t *device_for_sp_unit(uint8_t unit)
{
    if (unit == 0U || unit > SP_MAX_DEVICES) {
        return NULL;
    }
    return &g_devices[unit - 1U];
}

static void smartport_note_activity(uint8_t command,
                                    const sp_device_t *dev,
                                    uint8_t result)
{
    if (result != ERR_DEVICE_OK || dev == NULL) {
        return;
    }

    g_activity_device = device_index_from_ptr(dev);
    switch (command) {
    case BLK_CMD_STATUS:
    case SP_CMD_STATUS:
        g_activity_status_count++;
        break;
    case BLK_CMD_READ:
    case SP_CMD_READBLOCK:
        g_activity_read_count++;
        break;
    case BLK_CMD_WRITE:
    case SP_CMD_WRITEBLOCK:
        g_activity_write_count++;
        break;
    default:
        break;
    }
}

/* Execute one command frame. Called at main-loop scope once the card
 * reports EXEC_PENDING: the firmware has streamed the entire frame
 * (params, plus 512 data bytes for block writes) into the IN FIFO and
 * is spinning on CTRL bit7. We drain, dispatch, push the response
 * into the OUT FIFO, and set READY as the final atomic step.
 *
 * Response conventions (see 6502_SMARTPORT.S):
 *   ProDOS STATUS : [rc, blocks_lo, blocks_hi]
 *   ProDOS READ   : [rc] + 512 data on rc==0
 *   ProDOS WRITE  : [rc]
 *   SP (any)      : [st]; SP STATUS adds [len_lo, len_hi, payload];
 *                   SP READBLOCK adds 512 data on st==0
 * rc/st are the raw ProDOS/SmartPort error codes (0 = OK): both the
 * RETCODE and GETSTS paths branch on nonzero. */

/* ------------------------------------------------------------------ */
/* Block push CRC log: every 512-byte block pushed to the Apple is     */
/* fingerprinted at push time. `spverify` later re-reads the same     */
/* blocks DIRECTLY from the image file (bypassing the block cache)    */
/* and compares -- catching stale cache slots or wrong-block serves   */
/* that no other instrument can see (main-memory content cannot be    */
/* dumped post-mortem).                                                */
/* ------------------------------------------------------------------ */

#define SP_CRCLOG_SIZE 1024U

typedef struct {
    uint8_t  dev;
    uint8_t  valid;
    uint8_t  is_write;
    uint8_t  pad;
    uint32_t block;
    uint32_t crc;
} sp_crclog_entry_t;

static sp_crclog_entry_t g_sp_crclog[SP_CRCLOG_SIZE];
static uint32_t g_sp_crclog_idx = 0U;

static uint32_t sp_block_crc(const uint8_t *buf)
{
    /* FNV-1a, 32-bit: cheap and plenty for corruption detection. */
    uint32_t h = 2166136261U;
    uint32_t i;
    for (i = 0U; i < SP_BLOCK_SIZE; ++i) {
        h = (h ^ buf[i]) * 16777619U;
    }
    return h;
}

static void sp_crclog_add2(uint8_t dev, uint32_t block, const uint8_t *buf,
                           uint8_t is_write)
{
    sp_crclog_entry_t *e = &g_sp_crclog[g_sp_crclog_idx % SP_CRCLOG_SIZE];
    e->dev = dev;
    e->valid = 1U;
    e->is_write = is_write;
    e->block = block;
    e->crc = sp_block_crc(buf);
    g_sp_crclog_idx++;
}

static void sp_crclog_add(uint8_t dev, uint32_t block, const uint8_t *buf)
{
    sp_crclog_add2(dev, block, buf, 0U);
}

static int sp_write_block_to_image(sp_device_t *dev,
                                   uint32_t block_num,
                                   const uint8_t *data)
{
    uint32_t cache_addr;
    UINT bw = 0U;

    if (sp_cache_get_block(dev, block_num, 1U, &cache_addr) != 0) {
        return -1;
    }
    memcpy(cache_ptr_from_addr(cache_addr), data, SP_BLOCK_SIZE);
    sp_crclog_add2(device_index_from_ptr(dev), block_num, data, 1U);
    if (dev->is_ram) {
        return 0;   /* the memcpy above IS the storage */
    }
    /* Persist to the SD image. For raw .hdv/.po this is just
     * block_num * 512; for .2mg image_data_offset skips the header. */
    if (f_lseek(&dev->image_file,
                (FSIZE_t)dev->image_data_offset +
                ((FSIZE_t)block_num * SP_BLOCK_SIZE)) != FR_OK ||
        f_write(&dev->image_file, data, SP_BLOCK_SIZE, &bw) != FR_OK ||
        bw != SP_BLOCK_SIZE ||
        f_sync(&dev->image_file) != FR_OK) {
        return -1;
    }
    return 0;
}

static void execute_command(void)
{
    const uint8_t family = (uint8_t)(REG_READ(SP_R_CONTROL) & 0xFFU);
    const uint32_t len = sp_drain(g_cmd_buf, sizeof(g_cmd_buf));
    uint8_t result = ERR_DEVICE_OK;
    uint8_t activity_cmd = 0U;
    sp_device_t *dev = NULL;

    sp_ctl(SP_CTL_CLR_OUT);

    if (family == SP_FAMILY_PRODOS && len >= 6U) {
        const uint8_t cmd  = g_cmd_buf[0];
        const uint8_t unit = g_cmd_buf[1];
        const uint32_t block_num =
            (uint32_t)g_cmd_buf[4] | ((uint32_t)g_cmd_buf[5] << 8);

        dev = device_for_blk_unit(unit);
        activity_cmd = cmd;

        switch (cmd) {
        case BLK_CMD_STATUS: {
            uint32_t blocks = 0U;
            if (dev == NULL || dev->image_open == 0U) {
                result = ERR_DEVICE_NOT_CONNECTED;
            } else {
                blocks = dev->image_blocks;
                if (blocks > 0xFFFFU) {
                    blocks = 0xFFFFU;
                }
            }
            sp_push(result);
            sp_push((uint8_t)(blocks & 0xFFU));
            sp_push((uint8_t)((blocks >> 8) & 0xFFU));
            break;
        }

        case BLK_CMD_READ: {
            uint32_t cache_addr;
            if (dev == NULL || dev->image_open == 0U) {
                result = ERR_DEVICE_NOT_CONNECTED;
            } else if (block_num >= dev->image_blocks) {
                result = ERR_DEVICE_IO_ERROR;
            } else if (sp_cache_get_block(dev, block_num, 0U,
                                          &cache_addr) != 0) {
                result = ERR_DEVICE_IO_ERROR;
            } else {
                sp_push(result);
                sp_push_buf(cache_ptr_from_addr(cache_addr), SP_BLOCK_SIZE);
                sp_crclog_add(device_index_from_ptr(dev), block_num, cache_ptr_from_addr(cache_addr));
                break;
            }
            sp_push(result);
            break;
        }

        case BLK_CMD_WRITE: {
            if (dev == NULL || dev->image_open == 0U) {
                result = ERR_DEVICE_NOT_CONNECTED;
            } else if (dev->read_only) {
                result = ERR_NOWRITE;
            } else if (block_num >= dev->image_blocks || len < 518U) {
                result = ERR_DEVICE_IO_ERROR;
            } else if (sp_write_block_to_image(dev, block_num,
                                               &g_cmd_buf[6]) != 0) {
                result = ERR_DEVICE_IO_ERROR;
            }
            sp_push(result);
            break;
        }

        default:
            result = ERR_BADCTL;
            sp_push(result);
            break;
        }
    }
    else if (family == SP_FAMILY_SP && len >= 10U) {
        const uint8_t cmd = g_cmd_buf[0];
        const uint8_t *list = &g_cmd_buf[1];   /* 9 cmdlist bytes */
        const uint8_t unit = list[1];
        const uint32_t block_num = (uint32_t)list[4] |
                                   ((uint32_t)list[5] << 8) |
                                   ((uint32_t)list[6] << 16);

        dev = device_for_sp_unit(unit);
        activity_cmd = (uint8_t)(SP_CMD_BASE | cmd);

        switch (cmd) {
        case 0x00: {   /* STATUS */
            const uint8_t status_code = list[4];
            uint16_t length;
            if (unit != 0U && (dev == NULL || dev->image_open == 0U)) {
                result = ERR_DEVICE_NOT_CONNECTED;
                sp_push(result);
                break;
            }
            length = build_sp_status(dev, unit, status_code);
            if (length == 0U) {
                result = ERR_BADCTL;
                sp_push(result);
                break;
            }
            sp_push(result);
            sp_push((uint8_t)(length & 0xFFU));
            sp_push((uint8_t)((length >> 8) & 0xFFU));
            sp_push_buf(g_scratch, length);
            break;
        }

        case 0x01: {   /* READ BLOCK */
            uint32_t cache_addr;
            if (dev == NULL || dev->image_open == 0U) {
                result = ERR_DEVICE_NOT_CONNECTED;
            } else if (block_num >= dev->image_blocks) {
                result = ERR_DEVICE_IO_ERROR;
            } else if (sp_cache_get_block(dev, block_num, 0U,
                                          &cache_addr) != 0) {
                result = ERR_DEVICE_IO_ERROR;
            } else {
                sp_push(result);
                sp_push_buf(cache_ptr_from_addr(cache_addr), SP_BLOCK_SIZE);
                sp_crclog_add(device_index_from_ptr(dev), block_num, cache_ptr_from_addr(cache_addr));
                break;
            }
            sp_push(result);
            break;
        }

        case 0x02: {   /* WRITE BLOCK: 512 data bytes follow the list */
            if (dev == NULL || dev->image_open == 0U) {
                result = ERR_DEVICE_NOT_CONNECTED;
            } else if (dev->read_only) {
                result = ERR_NOWRITE;
            } else if (block_num >= dev->image_blocks || len < 522U) {
                result = ERR_DEVICE_IO_ERROR;
            } else if (sp_write_block_to_image(dev, block_num,
                                               &g_cmd_buf[10]) != 0) {
                result = ERR_DEVICE_IO_ERROR;
            }
            sp_push(result);
            break;
        }

        case 0x03:     /* FORMAT: images arrive formatted */
            result = ERR_NOWRITE;
            sp_push(result);
            break;

        default:       /* INIT/OPEN/CLOSE/CONTROL/char READ/WRITE */
            result = ERR_BADCTL;
            sp_push(result);
            break;
        }
    }
    else {
        /* Malformed frame: answer with BADCTL so the firmware wait
         * terminates, and flush whatever partial state remains. */
        result = ERR_BADCTL;
        sp_push(result);
    }

    smartport_note_activity(activity_cmd, dev, result);
    /* READY last: the firmware may pop the instant bit7 rises. */
    sp_ctl(SP_CTL_SET_READY | SP_CTL_ACK_EXEC | SP_CTL_CLR_IN);
}

/* ------------------------------------------------------------------ */
/* IRQ handler                                                        */
/* ------------------------------------------------------------------ */

/* The ISR is intentionally minimal: bump the command-pending count
 * and return. The actual FatFs read/write + apple-bus DMA happens in
 * smartport_service_poll() at main-loop scope, where it can take
 * whatever millisecond budget SD I/O needs without holding the IRQ
 * line. */
static void smartport_isr(void *cb)
{
    (void)cb;
    g_irq_count++;
    g_cmd_pending_count++;
}

/* ------------------------------------------------------------------ */
/* Image loading                                                       */
/* ------------------------------------------------------------------ */

static void close_device(sp_device_t *dev)
{
    if (dev == NULL || dev->image_open == 0U) {
        return;
    }
    if (dev->is_ram != 0U) {
        dev->image_open = 0U;
        dev->is_ram = 0U;
        dev->image_blocks = 0U;
        dev->image_data_offset = 0U;
        dev->image_path[0] = '\0';
    } else {
        (void)f_close(&dev->image_file);
        dev->image_open = 0U;
    }
}

static int image_parse_layout(sp_device_t *dev,
                              FSIZE_t file_size,
                              uint32_t *data_offset,
                              uint32_t *data_bytes,
                              uint8_t *locked)
{
    uint8_t header[SP_2MG_HEADER_SIZE];
    UINT br = 0U;
    FRESULT fr;
    uint32_t raw_bytes = (file_size > (FSIZE_t)UINT32_MAX) ?
                         UINT32_MAX : (uint32_t)file_size;

    *data_offset = 0U;
    *data_bytes = raw_bytes - (raw_bytes % SP_BLOCK_SIZE);
    *locked = 0U;

    if (file_size < SP_2MG_HEADER_SIZE) {
        return 0;
    }

    fr = f_lseek(&dev->image_file, 0U);
    if (fr != FR_OK) {
        return -(int)fr;
    }
    fr = f_read(&dev->image_file, header, sizeof(header), &br);
    if (fr != FR_OK) {
        return -(int)fr;
    }
    if (br != sizeof(header) || le32_load(&header[0]) != SP_2MG_MAGIC) {
        return 0;
    }

    {
        const uint32_t image_format = le32_load(&header[12]);
        const uint32_t flags = le32_load(&header[16]);
        const uint32_t block_count = le32_load(&header[20]);
        const uint32_t image_offset = le32_load(&header[24]);
        uint32_t image_bytes = le32_load(&header[28]);
        uint32_t max_bytes;

        if (image_format != SP_2MG_FORMAT_PRODOS ||
            image_offset >= raw_bytes) {
            return -2;
        }

        max_bytes = raw_bytes - image_offset;
        if (image_bytes == 0U || image_bytes > max_bytes) {
            image_bytes = max_bytes;
        }
        if (block_count != 0U && block_count <= (image_bytes / SP_BLOCK_SIZE)) {
            image_bytes = block_count * SP_BLOCK_SIZE;
        }

        *data_offset = image_offset;
        *data_bytes = image_bytes - (image_bytes % SP_BLOCK_SIZE);
        *locked = ((flags & SP_2MG_FLAG_LOCKED) != 0U) ? 1U : 0U;
    }

    return 0;
}

static int load_device(sp_device_t *dev)
{
    FRESULT fr;
    FSIZE_t size;
    uint32_t data_offset;
    uint32_t data_bytes;
    uint8_t image_locked;
    int layout_rc;
    uint8_t device_index;

    if (dev == NULL) {
        return -1;
    }

    device_index = device_index_from_ptr(dev);
    close_device(dev);
    sp_cache_invalidate_device(device_index);

    dev->image_open = 0U;
    dev->read_only = 1U;
    dev->image_blocks = 0U;
    dev->image_data_offset = 0U;

    if (dev->image_path[0] == '\0') {
        return 0;
    }
    if (sp_image_path_duplicate(device_index, dev->image_path) != 0U) {
        return SMARTPORT_SERVICE_ERR_DUPLICATE_PATH;
    }

    {
        int mount_rc = ensure_sd_mounted();
        if (mount_rc != 0) {
            return mount_rc;
        }
    }

    fr = f_open(&dev->image_file, dev->image_path, FA_READ | FA_WRITE);
    if (fr == FR_DENIED || fr == FR_WRITE_PROTECTED) {
        fr = f_open(&dev->image_file, dev->image_path, FA_READ);
        dev->read_only = 1U;
    } else {
        dev->read_only = 0U;
    }
    if (fr != FR_OK) {
        return -(int)fr;
    }

    size = f_size(&dev->image_file);
    layout_rc = image_parse_layout(dev, size, &data_offset, &data_bytes, &image_locked);
    if (layout_rc != 0) {
        (void)f_close(&dev->image_file);
        return layout_rc;
    }

    dev->read_only = (dev->read_only || image_locked) ? 1U : 0U;
    dev->image_data_offset = data_offset;
    dev->image_blocks = data_bytes / SP_BLOCK_SIZE;
    dev->image_open = 1U;
    return 0;
}

static int load_all_devices(void)
{
    uint8_t i;
    uint8_t mounted = 0U;
    int first_error = 0;

    for (i = 0U; i < SP_MAX_DEVICES; ++i) {
        int rc = load_device(&g_devices[i]);
        if (rc == 0 && g_devices[i].image_open != 0U) {
            mounted = 1U;
        } else if (rc != 0 && first_error == 0) {
            first_error = rc;
        }
    }

    return (mounted != 0U || first_error == 0) ? 0 : first_error;
}

static int sp_cache_get_block(sp_device_t *dev,
                              uint32_t block_num,
                              uint8_t for_write,
                              uint32_t *cache_addr)
{
    uint8_t device_index;
    uint32_t i;
    uint32_t victim = 0U;
    uint32_t oldest = UINT32_MAX;
    uint8_t *dst;
    UINT br = 0U;
    FRESULT fr;

    (void)for_write;

    if (cache_addr == NULL || dev == NULL || dev->image_open == 0U ||
        block_num >= dev->image_blocks) {
        return -1;
    }
    if (dev->is_ram) {
        /* The block lives at a fixed DDR address; no cache slot, no
         * fill. cache_ptr_from_addr() is a flat DDR translation, so
         * consumers work unchanged. */
        *cache_addr = SP_RAMDISK_BASE + block_num * SP_BLOCK_SIZE;
        return 0;
    }

    device_index = device_index_from_ptr(dev);

    for (i = 0U; i < SP_CACHE_BLOCK_COUNT; ++i) {
        if (g_cache[i].valid != 0U &&
            g_cache[i].device_index == device_index &&
            g_cache[i].block_num == block_num) {
            g_cache[i].last_used = ++g_cache_clock;
            *cache_addr = cache_addr_for_index(i);
            return 0;
        }
        if (g_cache[i].valid == 0U) {
            victim = i;
            oldest = 0U;
            break;
        }
        if (g_cache[i].last_used < oldest) {
            oldest = g_cache[i].last_used;
            victim = i;
        }
    }

    (void)oldest;
    dst = cache_ptr_from_addr(cache_addr_for_index(victim));
    fr = f_lseek(&dev->image_file,
                 (FSIZE_t)dev->image_data_offset +
                 ((FSIZE_t)block_num * SP_BLOCK_SIZE));
    if (fr != FR_OK) {
        g_cache[victim].valid = 0U;
        return -(int)fr;
    }

    fr = f_read(&dev->image_file, dst, SP_BLOCK_SIZE, &br);
    if (fr != FR_OK || br != SP_BLOCK_SIZE) {
        g_cache[victim].valid = 0U;
        return (fr != FR_OK) ? -(int)fr : -(int)FR_DISK_ERR;
    }

    g_cache[victim].valid = 1U;
    g_cache[victim].device_index = device_index;
    g_cache[victim].block_num = block_num;
    g_cache[victim].last_used = ++g_cache_clock;
    *cache_addr = cache_addr_for_index(victim);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int smartport_service_init(uint32_t uart_base)
{
    int rc;
    XScuGic *gic;

    g_uart_base = uart_base;
    g_smartport_slot = 0U;
    smartport_devices_ensure_defaults();
    sp_cache_invalidate_device(SMARTPORT_SERVICE_ALL_DEVICES);

    /* Mark the cache/status DMA window non-cacheable so the PL DMA engine sees
     * current data. Xil_SetTlbAttributes operates on 1 MB pages on Zynq-7000. */
    Xil_SetTlbAttributes(SP_CACHE_DDR_BASE, NORM_NONCACHE);

    rc = load_all_devices();
    sp_ramdisk_refresh(uart_base);
    if (smartport_present_count() != 0U) {
        rc = 0;
    }
    /* Even if media load fails, we still register the IRQ handler so
     * status commands can return DEVICE_NOT_CONNECTED rather than
     * leaving the firmware spinning forever. */

    gic = gic_get();
    if (gic == NULL) {
        return -100;
    }
    if (XScuGic_Connect(gic,
                        SMARTPORT_IRQ_ID,
                        (Xil_InterruptHandler)smartport_isr,
                        NULL) != XST_SUCCESS) {
        return -100;
    }
    XScuGic_SetPriorityTriggerType(gic, SMARTPORT_IRQ_ID, 0xA0, 0x03);
    XScuGic_Enable(gic, SMARTPORT_IRQ_ID);

    return rc;
}

int smartport_service_set_image_path(uint8_t device, const char *path)
{
    uint8_t index;
    size_t len;

    smartport_devices_ensure_defaults();

    if (path == NULL || service_device_to_index(device, &index) != 0) {
        return -1;
    }

    len = strlen(path);
    if (len >= sizeof(g_devices[index].image_path)) {
        return -1;
    }
    if (sp_image_path_duplicate(index, path) != 0U) {
        return SMARTPORT_SERVICE_ERR_DUPLICATE_PATH;
    }

    memcpy(g_devices[index].image_path, path, len + 1U);
    return 0;
}

const char *smartport_service_get_image_path(uint8_t device)
{
    uint8_t index;

    smartport_devices_ensure_defaults();

    if (service_device_to_index(device, &index) != 0) {
        return "";
    }
    return g_devices[index].image_path;
}

void smartport_service_poll(void)
{
    sp_ramdisk_refresh(UART0_BASE);
    /* Run one queued command per poll. The PL stalls the Apple bus until status
     * is posted, so the count is normally 0 or 1. Limiting each poll prevents
     * an unexpected IRQ burst from starving other main-loop services. */
    if (g_cmd_pending_count == 0U &&
        (sp_hw_status() & SP_ST_EXEC_PENDING) == 0U) {
        return;
    }

    execute_command();

    /* Atomic decrement under brief CPU-IRQ disable so a spurious
     * second IRQ between read-modify-write doesn't get lost. */
    uint32_t cpsr;
    __asm__ volatile ("mrs %0, cpsr" : "=r"(cpsr));
    __asm__ volatile ("cpsid i");
    if (g_cmd_pending_count != 0U) {
        g_cmd_pending_count--;
    }
    if ((cpsr & 0x80) == 0) {
        __asm__ volatile ("cpsie i");
    }
}

int smartport_service_reset_media(uint8_t device)
{
    uint8_t index;

    smartport_devices_ensure_defaults();

    if (device == SMARTPORT_SERVICE_ALL_DEVICES) {
        uint8_t i;
        for (i = 0U; i < SP_MAX_DEVICES; ++i) {
            close_device(&g_devices[i]);
        }
        g_ramdisk_state = 0U;
        sp_cache_invalidate_device(SMARTPORT_SERVICE_ALL_DEVICES);
        if (g_fs_mounted) {
            (void)f_mount((FATFS *)0, "0:/", 0U);
            g_fs_mounted = 0U;
        }
        {
            int rc = load_all_devices();
            sp_ramdisk_refresh(g_uart_base);
            if (smartport_present_count() != 0U) {
                rc = 0;
            }
            return rc;
        }
    }

    if (service_device_to_index(device, &index) != 0) {
        return -1;
    }
    return load_device(&g_devices[index]);
}

int smartport_service_read_block(uint8_t device,
                                 uint32_t block_num,
                                 uint8_t *buffer,
                                 uint32_t count,
                                 uint32_t *actual_out)
{
    uint32_t available;
    uint32_t to_copy;
    uint32_t cache_addr;
    uint8_t *cache_ptr;
    sp_device_t *dev;
    uint8_t index;

    if (actual_out != NULL) *actual_out = 0U;
    if (buffer == NULL || count == 0U) return -1;
    if (service_device_to_index(device, &index) != 0) return -1;

    smartport_devices_ensure_defaults();
    dev = &g_devices[index];
    if (dev->image_open == 0U) return -1;
    if (block_num >= dev->image_blocks) return -1;
    if (sp_cache_get_block(dev, block_num, 0U, &cache_addr) != 0) return -1;

    available = SP_BLOCK_SIZE;
    to_copy   = (count < available) ? count : available;
    cache_ptr = cache_ptr_from_addr(cache_addr);

    memcpy(buffer, cache_ptr, to_copy);
    if (actual_out != NULL) *actual_out = to_copy;
    return 0;
}

int smartport_service_get_activity(smartport_activity_t *out)
{
    uint8_t device;

    if (out == NULL) {
        return -1;
    }

    smartport_devices_ensure_defaults();
    device = (g_activity_device < SP_MAX_DEVICES) ? g_activity_device : 0U;

    out->present_mask = smartport_present_mask();
    out->device = device;
    out->read_only = ((out->present_mask & (uint8_t)(1U << device)) != 0U &&
                      g_devices[device].read_only != 0U) ? 1U : 0U;
    out->status_count = g_activity_status_count;
    out->read_count = g_activity_read_count;
    out->write_count = g_activity_write_count;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Volatile RAM disk (RamFactor/Slinky style)                          */
/* ------------------------------------------------------------------ */

extern uint8_t appletini_config_sp_ramdisk(void);

static void sp_ramdisk_bitmap_mark_used(uint8_t *bitmap, uint32_t block)
{
    if (bitmap == NULL ||
        block >= (SP_RAMDISK_BITMAP_BLOCKS * SP_BLOCK_SIZE * 8U)) {
        return;
    }

    bitmap[block >> 3] &= (uint8_t)~(0x80U >> (block & 7U));
}

/* Write a minimal empty ProDOS volume into the buffer: zeroed boot
 * blocks, a 4-block directory chain named RAM32, and a bitmap marking
 * the system blocks used and the block beyond ProDOS' 16-bit limit
 * unavailable. */
static void sp_ramdisk_format(void)
{
    uint8_t *base = (uint8_t *)(uintptr_t)SP_RAMDISK_BASE;
    uint8_t *bitmap;
    uint8_t *blk;
    uint32_t i;

    memset(base, 0, SP_RAMDISK_USED_BLOCKS * SP_BLOCK_SIZE);

    /* directory chain 2 -> 3 -> 4 -> 5 */
    for (i = 2U; i <= 5U; ++i) {
        blk = base + i * SP_BLOCK_SIZE;
        blk[0] = (uint8_t)((i == 2U) ? 0U : (i - 1U));
        blk[1] = 0U;
        blk[2] = (uint8_t)((i == 5U) ? 0U : (i + 1U));
        blk[3] = 0U;
    }

    /* volume directory header (block 2, offset +4) */
    blk = base + 2U * SP_BLOCK_SIZE + 4U;
    blk[0x00] = 0xF5;                  /* storage $F, name len 5 */
    blk[0x01] = 'R';
    blk[0x02] = 'A';
    blk[0x03] = 'M';
    blk[0x04] = '3';
    blk[0x05] = '2';
    blk[0x1E] = 0xC3;                  /* access: D/RN/W/R */
    blk[0x1F] = 0x27;                  /* entry length */
    blk[0x20] = 0x0D;                  /* entries per block */
    blk[0x21] = 0x00;                  /* file count = 0 */
    blk[0x22] = 0x00;
    blk[0x23] = (uint8_t)SP_RAMDISK_BITMAP_BLOCK; /* bitmap at block 6 */
    blk[0x24] = 0x00;
    blk[0x25] = (uint8_t)(SP_RAMDISK_BLOCKS & 0xFFU);
    blk[0x26] = (uint8_t)(SP_RAMDISK_BLOCKS >> 8);

    /* bit set = free; mark boot/directory/bitmap blocks used */
    bitmap = base + SP_RAMDISK_BITMAP_BLOCK * SP_BLOCK_SIZE;
    memset(bitmap, 0xFF, SP_RAMDISK_BITMAP_BLOCKS * SP_BLOCK_SIZE);
    for (i = 0U; i < SP_RAMDISK_USED_BLOCKS; ++i) {
        sp_ramdisk_bitmap_mark_used(bitmap, i);
    }
    for (i = SP_RAMDISK_BLOCKS;
         i < (SP_RAMDISK_BITMAP_BLOCKS * SP_BLOCK_SIZE * 8U);
         ++i) {
        sp_ramdisk_bitmap_mark_used(bitmap, i);
    }
}

static void sp_ramdisk_refresh(uint32_t uart_base)
{
    const uint8_t want = appletini_config_sp_ramdisk();
    uint32_t i;

    if (want != 0U && g_ramdisk_state != 1U) {
        for (i = 0U; i < SP_MAX_DEVICES; ++i) {
            if (g_devices[i].image_open == 0U) {
                memset(&g_devices[i], 0, sizeof(g_devices[i]));
                g_devices[i].is_ram = 1U;
                g_devices[i].image_open = 1U;
                g_devices[i].image_blocks = SP_RAMDISK_BLOCKS;
                (void)snprintf(g_devices[i].image_path,
                               sizeof(g_devices[i].image_path),
                               "RAM32 (volatile)");
                sp_ramdisk_format();
                g_ramdisk_state = 1U;
                uart_puts(uart_base,
                          "smartport: RAM32 32MB ram disk mounted\r\n");
                return;
            }
        }
        g_ramdisk_state = 2U;          /* no free unit: stay off */
        uart_puts(uart_base,
                  "smartport: no free unit for ram disk\r\n");
    } else if (want == 0U && g_ramdisk_state == 1U) {
        for (i = 0U; i < SP_MAX_DEVICES; ++i) {
            if (g_devices[i].is_ram != 0U) {
                g_devices[i].image_open = 0U;
                g_devices[i].is_ram = 0U;
            }
        }
        g_ramdisk_state = 2U;
        uart_puts(uart_base,
                  "smartport: ram disk unmounted (contents dropped)\r\n");
    } else if (want == 0U) {
        g_ramdisk_state = 2U;
    }
}

int smartport_service_verify_crclog(uint32_t uart_base)
{
    static uint8_t vbuf[SP_BLOCK_SIZE] __attribute__((aligned(64)));
    uint32_t checked = 0U;
    uint32_t bad = 0U;
    uint32_t i;
    char line[112];

    for (i = 0U; i < SP_CRCLOG_SIZE; ++i) {
        const sp_crclog_entry_t *e = &g_sp_crclog[i];
        sp_device_t *dev;
        UINT br = 0U;
        uint32_t j;
        uint8_t is_last = 1U;
        if (e->valid == 0U) {
            continue;
        }
        /* Only the most recent event for this (dev, block) is
         * comparable against the current file -- earlier reads
         * legitimately predate later writes (the volume directory is
         * rewritten constantly). Ring order: idx grows monotonically,
         * slot i is older than slot j when it was written earlier. */
        for (j = 0U; j < SP_CRCLOG_SIZE; ++j) {
            const sp_crclog_entry_t *f2 = &g_sp_crclog[j];
            if (j == i || f2->valid == 0U ||
                f2->dev != e->dev || f2->block != e->block) {
                continue;
            }
            /* newer entry for same block? (ring: compare positions
             * relative to the write cursor; entries closer behind the
             * cursor are newer) */
            {
                const uint32_t cur = g_sp_crclog_idx % SP_CRCLOG_SIZE;
                const uint32_t age_i = (cur + SP_CRCLOG_SIZE - 1U - i) % SP_CRCLOG_SIZE;
                const uint32_t age_j = (cur + SP_CRCLOG_SIZE - 1U - j) % SP_CRCLOG_SIZE;
                if (age_j < age_i) {
                    is_last = 0U;
                    break;
                }
            }
        }
        if (is_last == 0U) {
            continue;
        }
        if (e->dev >= SP_MAX_DEVICES) {
            continue;
        }
        dev = &g_devices[e->dev];
        if (dev->image_open == 0U || dev->is_ram != 0U) {
            continue;
        }
        if (f_lseek(&dev->image_file,
                    (FSIZE_t)dev->image_data_offset +
                    (FSIZE_t)e->block * SP_BLOCK_SIZE) != FR_OK ||
            f_read(&dev->image_file, vbuf, SP_BLOCK_SIZE, &br) != FR_OK ||
            br != SP_BLOCK_SIZE) {
            (void)snprintf(line, sizeof(line),
                "spverify: dev%u blk %lu READ ERROR\r\n",
                e->dev, (unsigned long)e->block);
            uart_puts(uart_base, line);
            continue;
        }
        checked++;
        if (sp_block_crc(vbuf) != e->crc) {
            bad++;
            if (bad <= 16U) {
                (void)snprintf(line, sizeof(line),
                    "spverify: MISMATCH dev%u blk %lu (%s) logged=%08lX file=%08lX\r\n",
                    e->dev, (unsigned long)e->block,
                    e->is_write ? "W" : "R",
                    (unsigned long)e->crc,
                    (unsigned long)sp_block_crc(vbuf));
                uart_puts(uart_base, line);
            }
        }
    }
    (void)snprintf(line, sizeof(line),
        "spverify: %lu blocks checked, %lu mismatches (log writes %lu)\r\n",
        (unsigned long)checked, (unsigned long)bad,
        (unsigned long)g_sp_crclog_idx);
    uart_puts(uart_base, line);
    return (int)bad;
}
