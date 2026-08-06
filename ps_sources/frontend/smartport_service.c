/* SmartPort service: PS-side command-execution backend for the
 * smartport_card on the PL.
 *
 * The 6502 ROM streams command bytes into the PL DATA FIFO, writes CTRL
 * to request execution, then polls CTRL bit7. The PS drains the FIFO,
 * performs the requested status / read / write operation, pushes the
 * response bytes to the OUT FIFO, and sets READY only after the full
 * response is available. Native transfers copy each payload byte through the
 * card FIFOs. Eligible vTW block reads may instead copy straight into vTW
 * shadow RAM, while unsafe ranges keep the normal FIFO path. */

#include "smartport_service.h"

#include <stdint.h>
#include <string.h>

#include "xil_cache.h"
#include "xil_mmu.h"
#include "xil_exception.h"
#include "xparameters.h"
#include "xscugic.h"
#include "xiltimer.h"
#include "ff.h"

#include "../lib/common.h"
#include "../lib/psdma.h"
#include "../lib/uart.h"
#include "card_control_regs.h"
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
#define SP_R_OUT_PUSH4          SP_REG(5)
#define SP_R_IN_HEAD4           SP_REG(6)

#define SP_ST_IN_COUNT(v)       ((v) & 0x7FFU)
#define SP_ST_EXEC_PENDING      (1UL << 28)
#define SP_ST_READY             (1UL << 29)
#define SP_ST_EXEC_VTW          (1UL << 31)

#define SP_CTL_POP_IN           1U
#define SP_CTL_CLR_IN           2U
#define SP_CTL_CLR_OUT          4U
#define SP_CTL_SET_READY        8U
#define SP_CTL_ACK_EXEC         16U
#define SP_CTL_SET_DIRECT       32U
#define SP_CTL_POP_IN4          64U

/* SP_R_SSS packing, shared with smartport_card.sv. */
#define SP_SSS_80STORE_BIT      (1UL << 0)
#define SP_SSS_RAMRD_BIT        (1UL << 1)
#define SP_SSS_RAMWRT_BIT       (1UL << 2)
#define SP_SSS_ALTZP_BIT        (1UL << 3)
#define SP_SSS_PAGE2_BIT        (1UL << 6)
#define SP_SSS_HIRES_BIT        (1UL << 7)
#define SP_SSS_RAMWORKS_SHIFT   14U
#define SP_SSS_RAMWORKS_MASK    0x7FUL
#define SP_SSS_VTW_WIDE_BIT     (1UL << 21)

#define SP_VTW_SHADOW_AUX_BASE  0x10000UL
#define SP_VTW_POINTER_POLLS    64U
#define SP_VTW_POST_CREDIT_POLLS 4096U
#define SP_VTW_POST_FILL_MASK   0x3FFU
#define SP_VTW_POST_CAPACITY    508U
#define SP_VTW_POST_DRAIN_TIMEOUT_US 2000U
/* Flush must outwait the core's one-access lookahead parking (worst case a
 * posted write behind a full 512-deep queue draining at 1 MHz, ~512 us). */
#define SP_VTW_FLUSH_POLLS      16384U
#define SP_VTW_RELEASE_POLLS    16384U
#define SP_VTW_DMA_TIMEOUT_US   10000U
#define SP_VTW_ABORT_TIMEOUT_US 10000U
#define SP_VTW_LIVE_MASK        (CARD_CTRL_VTW_STATUS_BUS_OWNED | \
                                 CARD_CTRL_VTW_STATUS_ENABLE_EFF | \
                                 CARD_CTRL_VTW_STATUS_CORE_RUN)
#define SP_RAMWORKS_ENABLE_REG  CARD_CTRL_REG_ADDR(0x62U)

#define SP_VTW_COPY_FALLBACK    0U
#define SP_VTW_COPY_COMPLETE    1U
#define SP_VTW_COPY_FATAL       2U

/* CTRL values the firmware writes to trigger execution. */
#define SP_FAMILY_PRODOS        0x01U
#define SP_FAMILY_SP            0x02U
#define SP_FAMILY_PREFLIGHT_BIT 0x80U

/* SmartPort block and status staging cache in DDR. */
#define SP_BLOCK_SIZE           512U
#define SP_RESPONSE_MAX         (SP_BLOCK_SIZE + 3U)
#define SP_VTW_DIRECT_BYTES     SP_BLOCK_SIZE
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
#define SP_VTW_READAHEAD_BLOCKS 8U
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
static uint32_t g_vtw_direct_count = 0U;
static uint32_t g_vtw_direct_ramworks_count = 0U;
static uint32_t g_vtw_direct_posted_count = 0U;
static uint32_t g_vtw_direct_fallback_count = 0U;
static uint32_t g_vtw_fallback_range_count = 0U;
static uint32_t g_vtw_fallback_state_count = 0U;
static uint32_t g_vtw_fallback_map_count = 0U;
static uint32_t g_vtw_fallback_shadow_count = 0U;
static uint32_t g_vtw_fallback_post_count = 0U;
static uint32_t g_vtw_fallback_dma_count = 0U;
static uint32_t g_vtw_split_block_count = 0U;
static uint32_t g_vtw_split_span_count = 0U;
static uint32_t g_vtw_split_max_spans = 0U;
static uint32_t g_vtw_direct_write_count = 0U;
static uint32_t g_vtw_write_preflight_count = 0U;
static uint32_t g_vtw_write_stream_count = 0U;
static uint32_t g_vtw_write_fault_count = 0U;
static uint32_t g_cache_hit_count = 0U;
static uint32_t g_cache_miss_count = 0U;
static uint32_t g_cache_bypass_count = 0U;
static uint32_t g_post_credit_read_count = 0U;
static uint8_t g_ramdisk_state = 0U;   /* 0 unknown, 1 mounted, 2 off */

static volatile uint32_t g_irq_count = 0U;
static volatile XTime g_irq_tick = 0U;
static volatile uint8_t g_irq_tick_valid = 0U;

/* Updated only by foreground command execution. Times stay in global-timer
 * ticks until the UART asks for them, avoiding division in the hot path. */
static uint32_t g_latency_sample_count = 0U;
static XTime g_dispatch_ticks_last = 0U;
static XTime g_dispatch_ticks_total = 0U;
static XTime g_dispatch_ticks_max = 0U;
static XTime g_ready_ticks_last = 0U;
static XTime g_ready_ticks_total = 0U;
static XTime g_ready_ticks_max = 0U;

/* Command-pending counter. The ISR increments this for each smartport
 * IRQ; smartport_service_poll() services one command per call as long
 * as the count is non-zero, decrementing on completion. The PL holds
 * the Apple bus stalled until status is posted, so it won't fire a new
 * IRQ until we ack the previous one -- in practice the count is 0 or 1.
 *
 * Volatile + aligned 32-bit makes ISR/poll access atomic on Cortex-A9
 * without explicit critical sections. */
static volatile uint32_t g_cmd_pending_count = 0U;

static uint8_t  g_scratch[SP_BLOCK_SIZE] __attribute__((aligned(64)));
                                                /* per-command staging */
static uint8_t  g_cmd_buf[1024];            /* drained command frame */
static uint8_t  g_response[SP_RESPONSE_MAX] __attribute__((aligned(4)));
static uint32_t g_response_len;
static uint8_t  g_response_overflow;

typedef struct {
    uint8_t active;
    uint8_t family;
    uint8_t prefix_len;
    uint8_t direct;
    uint8_t prefix[10];
} sp_vtw_write_preflight_t;

static sp_vtw_write_preflight_t g_vtw_write_preflight;

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

static uint32_t sp_ticks_to_us(XTime ticks)
{
    uint64_t us;

    if (ticks == 0U || COUNTS_PER_SECOND == 0U) {
        return 0U;
    }
    us = ((uint64_t)ticks * 1000000ULL) /
         (uint64_t)COUNTS_PER_SECOND;
    return (us > UINT32_MAX) ? UINT32_MAX : (uint32_t)us;
}

static inline void sp_push(uint8_t b)
{
    REG_WRITE(SP_R_OUT_PUSH, (uint32_t)b);
}

static void sp_response_reset(void)
{
    g_response_len = 0U;
    g_response_overflow = 0U;
}

static void sp_response_append(uint8_t b)
{
    if (g_response_len < SP_RESPONSE_MAX) {
        g_response[g_response_len++] = b;
    } else {
        g_response_overflow = 1U;
    }
}

static void sp_response_append_buf(const uint8_t *p, uint32_t n)
{
    while (n-- != 0U) {
        sp_response_append(*p++);
    }
}

/* OUT is a word-wide response buffer, cleared to byte offset zero before
 * every command. Build the complete reply first, including its result and
 * length prefix, then emit aligned words followed only by a scalar tail.
 * READY is written after this function returns, so the Apple never observes
 * a partly built response. Native commands retain scalar writes; only vTW
 * needs the lower MMIO cost of packed words. */
static void sp_response_commit(uint8_t accelerated)
{
    const uint8_t *p = g_response;
    uint32_t n = g_response_len;

    if (g_response_overflow != 0U) {
        p = NULL;
        n = 0U;
        sp_push(ERR_DEVICE_IO_ERROR);
        return;
    }
    if (accelerated != 0U) {
        while (n >= 4U) {
            const uint32_t word = (uint32_t)p[0] |
                                  ((uint32_t)p[1] << 8) |
                                  ((uint32_t)p[2] << 16) |
                                  ((uint32_t)p[3] << 24);
            REG_WRITE(SP_R_OUT_PUSH4, word);
            p += 4;
            n -= 4U;
        }
    }
    while (n-- != 0U) {
        sp_push(*p++);
    }
}

/* Map one accelerated Apple write using the private vTW switch snapshot.
 * Banks 0/1 live in the vTW BRAM shadow. Banks 2..127 live in PSRAM and can
 * use the existing PS-DMA engine. Bank 128 crosses the available 8 MB PSRAM
 * chip and stays on the byte path. */
static int sp_vtw_memory_phys(uint16_t apple_addr,
                              uint32_t sss,
                              uint8_t write_access,
                              uint32_t *bank_out,
                              uint32_t *phys_out)
{
    const uint32_t aux_bank =
        ((sss >> SP_SSS_RAMWORKS_SHIFT) & SP_SSS_RAMWORKS_MASK) + 1U;
    uint32_t bank;

    if (bank_out == NULL || phys_out == NULL || apple_addr >= 0xC000U) {
        return -1;
    }

    if (apple_addr < 0x0200U) {
        bank = ((sss & SP_SSS_ALTZP_BIT) != 0U) ? aux_bank : 0U;
    } else {
        const uint8_t display_window =
            ((apple_addr >= 0x0400U && apple_addr <= 0x07FFU) ||
             (((sss & SP_SSS_HIRES_BIT) != 0U) &&
              apple_addr >= 0x2000U && apple_addr <= 0x3FFFU)) ? 1U : 0U;

        const uint32_t bank_bit = (write_access != 0U)
                                      ? SP_SSS_RAMWRT_BIT
                                      : SP_SSS_RAMRD_BIT;

        bank = ((sss & bank_bit) != 0U) ? aux_bank : 0U;
        if ((sss & SP_SSS_80STORE_BIT) != 0U && display_window != 0U) {
            bank = ((sss & SP_SSS_PAGE2_BIT) != 0U) ? aux_bank : 0U;
        }
    }

    if (bank > 127U) {
        return -1;
    }
    *bank_out = bank;
    *phys_out = (bank << 16) | (uint32_t)apple_addr;
    return 0;
}

/* Match vtw_is_video_window(). Main $6000-$9FFF is video only while the
 * command's private wide-main flag is set; AUX uses the full SHR window. */
static uint8_t sp_vtw_needs_video_post(uint16_t apple_addr,
                                       uint32_t phys,
                                       uint32_t sss)
{
    const uint8_t aux = ((phys & SP_VTW_SHADOW_AUX_BASE) != 0U) ? 1U : 0U;
    const uint8_t wide = ((sss & SP_SSS_VTW_WIDE_BIT) != 0U) ? 1U : 0U;

    return ((apple_addr >= 0x0400U && apple_addr <= 0x0BFFU) ||
            (apple_addr >= 0x2000U && apple_addr <= 0x5FFFU) ||
            ((aux != 0U || wide != 0U) &&
             apple_addr >= 0x6000U && apple_addr <= 0x9FFFU)) ? 1U : 0U;
}

static uint8_t sp_vtw_is_live(void)
{
    return ((REG_READ(CARD_CTRL_VTW_STATUS_REG) & SP_VTW_LIVE_MASK) ==
            SP_VTW_LIVE_MASK) ? 1U : 0U;
}

static uint8_t sp_vtw_release_core_hold(void)
{
    uint32_t i;

    REG_WRITE(CARD_CTRL_VTW_RW_FLUSH_REG, CARD_CTRL_VTW_RW_FLUSH_RELEASE_BIT);
    for (i = 0U; i < SP_VTW_RELEASE_POLLS; ++i) {
        const uint32_t status = REG_READ(CARD_CTRL_VTW_RW_FLUSH_REG);
        if ((status & (CARD_CTRL_VTW_RW_FLUSH_BUSY_BIT |
                       CARD_CTRL_VTW_RW_FLUSH_HELD_BIT)) == 0U) {
            return 1U;
        }
    }
    return 0U;
}

/* Freeze the soft core and flush+invalidate the vTW RamWorks line cache.
 * Success means the cache is clean AND the core is still frozen -- the
 * caller owns RamWorks until sp_vtw_release_core_hold(). A completion
 * without HELD means session teardown auto-released mid-request; a
 * timeout releases defensively so a PL fault can never leave the Apple
 * frozen. Both fail the direct path. */
static uint8_t sp_vtw_flush_ramworks_cache(void)
{
    uint32_t status = REG_READ(CARD_CTRL_VTW_RW_FLUSH_REG);
    const uint32_t before = status & CARD_CTRL_VTW_RW_FLUSH_COUNT_MASK;
    uint32_t i;

    if ((status & CARD_CTRL_VTW_RW_FLUSH_BUSY_BIT) != 0U) {
        /* No other CPU0 caller may own this single-command interface.
         * Close any stale transaction before allowing byte fallback. */
        return (sp_vtw_release_core_hold() != 0U)
                   ? SP_VTW_COPY_FALLBACK : SP_VTW_COPY_FATAL;
    }

    REG_WRITE(CARD_CTRL_VTW_RW_FLUSH_REG, CARD_CTRL_VTW_RW_FLUSH_REQ_BIT);
    for (i = 0U; i < SP_VTW_FLUSH_POLLS; ++i) {
        status = REG_READ(CARD_CTRL_VTW_RW_FLUSH_REG);
        if ((status & CARD_CTRL_VTW_RW_FLUSH_BUSY_BIT) == 0U &&
            (((status & CARD_CTRL_VTW_RW_FLUSH_COUNT_MASK) - before) &
             CARD_CTRL_VTW_RW_FLUSH_COUNT_MASK) == 1U) {
            return ((status & CARD_CTRL_VTW_RW_FLUSH_HELD_BIT) != 0U)
                       ? SP_VTW_COPY_COMPLETE : SP_VTW_COPY_FALLBACK;
        }
    }
    /* A pending request cancels immediately. An accepted dirty-line write
     * defers release until its PSRAM response arrives. In either case the
     * byte fallback is safe only after BUSY and HELD both clear. */
    return (sp_vtw_release_core_hold() != 0U)
               ? SP_VTW_COPY_FALLBACK : SP_VTW_COPY_FATAL;
}

static uint8_t sp_psdma_to_ramworks(uint32_t psram_addr,
                                    const uint8_t *data,
                                    uint32_t length)
{
    psdma_result_t rc;

    if (data == NULL || length == 0U ||
        (((uint32_t)(uintptr_t)data) & 7U) != 0U ||
        (length & 7U) != 0U ||
        psram_addr + length > 0x00800000UL) {
        return SP_VTW_COPY_FALLBACK;
    }

    /* The shared helper owns the one-command port, uses a real time limit,
     * and drains an accepted AXI/PSRAM operation before returning from a
     * timeout. Only a failed drain is fatal to the frozen vTW session. */
    rc = psdma_transfer(PSDMA_OWNER_SMARTPORT,
                        psram_addr,
                        (uint32_t)(uintptr_t)data,
                        length,
                        PSDMA_DDR_TO_MC,
                        SP_VTW_DMA_TIMEOUT_US,
                        SP_VTW_ABORT_TIMEOUT_US);
    if (rc == PSDMA_OK) {
        return SP_VTW_COPY_COMPLETE;
    }
    return (rc == PSDMA_ERR_ABORT)
               ? SP_VTW_COPY_FATAL : SP_VTW_COPY_FALLBACK;
}

/* Sticky: a PS-DMA engine timeout disables the RamWorks direct path for
 * the rest of the session rather than trusting the engine again. */
static uint8_t g_vtw_dma_fault = 0U;

static uint8_t sp_vtw_direct_ramworks_block(uint32_t psram_addr,
                                            const uint8_t *data,
                                            uint32_t length)
{
    uint8_t flush_rc;
    uint8_t dma_rc;

    if (g_vtw_dma_fault != 0U ||
        (REG_READ(SP_RAMWORKS_ENABLE_REG) & 1U) == 0U) {
        g_vtw_fallback_dma_count++;
        return SP_VTW_COPY_FALLBACK;
    }
    flush_rc = sp_vtw_flush_ramworks_cache();
    if (flush_rc != SP_VTW_COPY_COMPLETE) {
        g_vtw_fallback_dma_count++;
        return flush_rc;
    }
    /* The flush left the core frozen: nothing can touch RamWorks while
     * the DMA runs. A timeout first aborts and drains the DMA, so release
     * cannot expose a late writer to the resumed core. */
    dma_rc = sp_psdma_to_ramworks(psram_addr, data, length);
    if (dma_rc != SP_VTW_COPY_COMPLETE) {
        g_vtw_dma_fault = 1U;
        g_vtw_fallback_dma_count++;
    }
    if (dma_rc == SP_VTW_COPY_FATAL ||
        sp_vtw_release_core_hold() == 0U) {
        /* Keep the hold on a failed drain/release. Apple RES# remains the
         * hardware escape and auto-releases it; running fallback here could
         * corrupt RamWorks. */
        return SP_VTW_COPY_FATAL;
    }
    if (dma_rc == SP_VTW_COPY_FALLBACK) {
        return SP_VTW_COPY_FALLBACK;
    }
    if (sp_vtw_is_live() == 0U) {
        g_vtw_fallback_state_count++;
        return SP_VTW_COPY_FALLBACK;
    }
    g_vtw_direct_ramworks_count++;
    return SP_VTW_COPY_COMPLETE;
}

typedef struct {
    uint16_t apple_addr;
    uint16_t data_offset;
    uint16_t length;
    uint32_t bank;
    uint32_t phys;
} sp_vtw_span_t;

#define SP_VTW_MAX_SPANS 4U

static uint8_t sp_vtw_build_spans(uint16_t apple_addr,
                                  uint32_t sss,
                                  uint8_t write_access,
                                  sp_vtw_span_t *spans,
                                  uint32_t *span_count_out)
{
    uint32_t span_count = 0U;
    uint32_t bank;
    uint32_t phys;
    uint32_t i;

    if (spans == NULL || span_count_out == NULL || apple_addr < 0x0200U ||
        (uint32_t)apple_addr + SP_VTW_DIRECT_BYTES > 0x10000UL) {
        return 0U;
    }
    for (i = 0U; i < SP_VTW_DIRECT_BYTES; ++i) {
        const uint16_t current = (uint16_t)((uint32_t)apple_addr + i);

        if (sp_vtw_memory_phys(current, sss, write_access,
                               &bank, &phys) != 0) {
            return 0U;
        }
        if (span_count == 0U ||
            bank != spans[span_count - 1U].bank ||
            phys != spans[span_count - 1U].phys +
                    spans[span_count - 1U].length) {
            if (span_count == SP_VTW_MAX_SPANS) {
                return 0U;
            }
            spans[span_count].apple_addr = current;
            spans[span_count].data_offset = (uint16_t)i;
            spans[span_count].length = 1U;
            spans[span_count].bank = bank;
            spans[span_count].phys = phys;
            span_count++;
        } else {
            spans[span_count - 1U].length++;
        }
    }
    *span_count_out = span_count;
    return 1U;
}

/* Write one already-proved contiguous bank-0/1 span. Normal block buffers
 * and every MMU boundary are word aligned, so the packed path handles the
 * common case. The scalar path keeps odd third-party buffer addresses safe.
 * In both cases the final pointer proves that every byte landed. */
static uint8_t sp_vtw_shadow_write_span(const sp_vtw_span_t *span,
                                         const uint8_t *data)
{
    uint32_t i;
    uint32_t shadow_status;
    uint32_t shadow_before;
    uint32_t packed_words = 0U;

    if (span == NULL || data == NULL || span->length == 0U) {
        return SP_VTW_COPY_FALLBACK;
    }

    shadow_status = REG_READ(CARD_CTRL_VTW_SHADOW_DATA4_STATUS_REG);
    shadow_before = shadow_status & CARD_CTRL_VTW_SHADOW_DATA4_ACCEPT_MASK;
    REG_WRITE(CARD_CTRL_VTW_SHADOW_ADDR_REG, span->phys);

    if ((span->length & 3U) == 0U) {
        packed_words = (uint32_t)span->length / 4U;
        for (i = 0U; i < (uint32_t)span->length; i += 4U) {
            const uint32_t word =
                (uint32_t)data[i] |
                ((uint32_t)data[i + 1U] << 8) |
                ((uint32_t)data[i + 2U] << 16) |
                ((uint32_t)data[i + 3U] << 24);

            REG_WRITE(CARD_CTRL_VTW_SHADOW_DATA4_REG, word);
        }
    } else {
        for (i = 0U; i < (uint32_t)span->length; ++i) {
            REG_WRITE(CARD_CTRL_VTW_SHADOW_DATA_REG, data[i]);
        }
    }

    for (i = 0U; i < SP_VTW_POINTER_POLLS; ++i) {
        const uint32_t pointer =
            REG_READ(CARD_CTRL_VTW_SHADOW_ADDR_REG) & 0x3FFFFUL;

        shadow_status = REG_READ(CARD_CTRL_VTW_SHADOW_DATA4_STATUS_REG);
        if ((shadow_status & CARD_CTRL_VTW_SHADOW_DATA4_BUSY_BIT) == 0U &&
            pointer == span->phys + (uint32_t)span->length) {
            const uint32_t accepted_delta =
                ((shadow_status & CARD_CTRL_VTW_SHADOW_DATA4_ACCEPT_MASK) -
                 shadow_before) & CARD_CTRL_VTW_SHADOW_DATA4_ACCEPT_MASK;

            if (accepted_delta == packed_words) {
                return SP_VTW_COPY_COMPLETE;
            }
            break;
        }
    }

    /* Resetting the pointer cancels queued packed work before the ROM's
     * full byte-copy fallback resumes. Any bytes already written are then
     * overwritten with the same data. */
    REG_WRITE(CARD_CTRL_VTW_SHADOW_ADDR_REG, span->phys);
    g_vtw_fallback_shadow_count++;
    return SP_VTW_COPY_FALLBACK;
}

static uint8_t sp_vtw_post_span(const sp_vtw_span_t *span,
                                uint32_t sss,
                                const uint8_t *data,
                                uint8_t *posted_any)
{
    uint32_t i;
    uint32_t post_count = 0U;
    uint32_t post_before;
    uint32_t post_credits = 0U;

    for (i = 0U; i < (uint32_t)span->length; ++i) {
        if (sp_vtw_needs_video_post(
                (uint16_t)(span->apple_addr + i), span->phys + i, sss) != 0U) {
            post_count++;
        }
    }
    if (post_count == 0U) {
        return SP_VTW_COPY_COMPLETE;
    }

    post_before = REG_READ(CARD_CTRL_VTW_POST_STATUS_REG) &
                  CARD_CTRL_VTW_POST_ACCEPT_MASK;
    for (i = 0U; i < (uint32_t)span->length; ++i) {
        const uint16_t current = (uint16_t)(span->apple_addr + i);

        if (sp_vtw_needs_video_post(current, span->phys + i, sss) == 0U) {
            continue;
        }
        if (post_credits == 0U) {
            uint32_t poll;

            for (poll = 0U; poll < SP_VTW_POST_CREDIT_POLLS; ++poll) {
                const uint32_t fill =
                    REG_READ(CARD_CTRL_VTW_POST_STATS_REG) &
                    SP_VTW_POST_FILL_MASK;

                g_post_credit_read_count++;
                if (fill < SP_VTW_POST_CAPACITY) {
                    post_credits = SP_VTW_POST_CAPACITY - fill;
                    break;
                }
            }
            if (post_credits == 0U) {
                g_vtw_fallback_post_count++;
                return SP_VTW_COPY_FALLBACK;
            }
        }
        REG_WRITE(CARD_CTRL_VTW_POST_PUSH_REG,
                  ((uint32_t)data[i] << 16) | (uint32_t)current);
        post_credits--;
    }

    for (i = 0U; i < SP_VTW_POINTER_POLLS; ++i) {
        const uint32_t accepted =
            REG_READ(CARD_CTRL_VTW_POST_STATUS_REG) &
            CARD_CTRL_VTW_POST_ACCEPT_MASK;

        if (((accepted - post_before) & CARD_CTRL_VTW_POST_ACCEPT_MASK) ==
            post_count) {
            *posted_any = 1U;
            return SP_VTW_COPY_COMPLETE;
        }
    }
    g_vtw_fallback_post_count++;
    return SP_VTW_COPY_FALLBACK;
}

/* Do not let the accelerated Apple observe a completed video read while its
 * bytes are still waiting in the 1 MHz motherboard-write queue. Non-video
 * reads never call this fence. The queue holds at most 508 entries, so 2 ms
 * covers its worst normal drain with ample margin. */
static uint8_t sp_vtw_wait_video_posts_drained(void)
{
    XTime start;
    XTime now;

    XTime_GetTime(&start);
    do {
        if ((REG_READ(CARD_CTRL_VTW_POST_STATS_REG) &
             SP_VTW_POST_FILL_MASK) == 0U) {
            return SP_VTW_COPY_COMPLETE;
        }
        XTime_GetTime(&now);
    } while (sp_ticks_to_us(now - start) < SP_VTW_POST_DRAIN_TIMEOUT_US);

    g_vtw_fallback_post_count++;
    return SP_VTW_COPY_FALLBACK;
}

/* Copy one successful read response into vTW memory. Preflight all 512
 * addresses before the first write, but split the transfer when a legal
 * Apple MMU boundary changes banks. This keeps the fail-closed contract and
 * removes the common false fallback at $0400/$0800 and similar boundaries.
 * Video spans still enter the ordered posted queue, so motherboard RAM and
 * renderer capture see the same writes as the ROM byte loop. */
static uint8_t sp_vtw_direct_read_block(uint16_t apple_addr,
                                        uint32_t sss,
                                        const uint8_t *data)
{
    sp_vtw_span_t spans[SP_VTW_MAX_SPANS];
    uint32_t span_count = 0U;
    uint32_t i;
    uint8_t posted_any = 0U;

    if (data == NULL || apple_addr < 0x0200U ||
        (uint32_t)apple_addr + SP_VTW_DIRECT_BYTES > 0x10000UL) {
        g_vtw_fallback_range_count++;
        return SP_VTW_COPY_FALLBACK;
    }

    if (sp_vtw_is_live() == 0U) {
        g_vtw_fallback_state_count++;
        return SP_VTW_COPY_FALLBACK;
    }

    /* Preflight and coalesce contiguous physical runs. No state changes occur
     * until the complete logical block has a bounded span list. */
    if (sp_vtw_build_spans(apple_addr, sss, 1U,
                           spans, &span_count) == 0U) {
        g_vtw_fallback_map_count++;
        return SP_VTW_COPY_FALLBACK;
    }

    if (span_count > 1U) {
        g_vtw_split_block_count++;
        g_vtw_split_span_count += span_count;
        if (span_count > g_vtw_split_max_spans) {
            g_vtw_split_max_spans = span_count;
        }
    }

    for (i = 0U; i < span_count; ++i) {
        const uint8_t *span_data = data + spans[i].data_offset;
        uint8_t rc;

        if (spans[i].bank > 1U) {
            rc = sp_vtw_direct_ramworks_block(
                spans[i].phys, span_data, spans[i].length);
        } else {
            rc = sp_vtw_shadow_write_span(&spans[i], span_data);
            if (rc == SP_VTW_COPY_COMPLETE) {
                rc = sp_vtw_post_span(
                    &spans[i], sss, span_data, &posted_any);
            }
        }
        if (rc != SP_VTW_COPY_COMPLETE) {
            return rc;
        }
    }
    if (posted_any != 0U) {
        if (sp_vtw_wait_video_posts_drained() != SP_VTW_COPY_COMPLETE) {
            return SP_VTW_COPY_FALLBACK;
        }
        g_vtw_direct_posted_count++;
    }

    if (sp_vtw_is_live() == 0U) {
        g_vtw_fallback_state_count++;
        return SP_VTW_COPY_FALLBACK;
    }
    return SP_VTW_COPY_COMPLETE;
}

/* A write preflight may skip the Apple byte stream only when every source
 * byte has a direct, stable read path. The actual command repeats this proof;
 * a later hardware fault returns I/O error and never writes a partial block. */
static uint8_t sp_vtw_direct_source_eligible(uint16_t apple_addr,
                                              uint32_t sss)
{
    sp_vtw_span_t spans[SP_VTW_MAX_SPANS];
    uint32_t span_count;
    uint32_t i;

    if (sp_vtw_is_live() == 0U ||
        sp_vtw_build_spans(apple_addr, sss, 0U,
                           spans, &span_count) == 0U) {
        return 0U;
    }
    for (i = 0U; i < span_count; ++i) {
        if (spans[i].bank <= 1U) {
            if ((spans[i].length & 3U) != 0U) {
                return 0U;
            }
        } else if (g_vtw_dma_fault != 0U ||
                   (REG_READ(SP_RAMWORKS_ENABLE_REG) & 1U) == 0U ||
                   ((spans[i].phys | spans[i].data_offset |
                     spans[i].length) & 7U) != 0U) {
            return 0U;
        }
    }
    return 1U;
}

static uint8_t sp_vtw_shadow_read_span(const sp_vtw_span_t *span,
                                        uint8_t *data)
{
    uint32_t status;
    uint32_t before;
    uint32_t word_index;
    uint32_t poll;

    if (span == NULL || data == NULL || span->length == 0U ||
        (span->length & 3U) != 0U) {
        return SP_VTW_COPY_FALLBACK;
    }

    REG_WRITE(CARD_CTRL_VTW_SHADOW_ADDR_REG, span->phys);
    status = REG_READ(CARD_CTRL_VTW_SHADOW_READ4_STATUS_REG);
    before = status & CARD_CTRL_VTW_SHADOW_READ4_COUNT_MASK;

    for (word_index = 0U;
         word_index < (uint32_t)span->length / 4U;
         ++word_index) {
        uint32_t word;

        for (poll = 0U; poll < SP_VTW_POINTER_POLLS; ++poll) {
            status = REG_READ(CARD_CTRL_VTW_SHADOW_READ4_STATUS_REG);
            if ((status & CARD_CTRL_VTW_SHADOW_READ4_READY_BIT) != 0U) {
                break;
            }
        }
        if (poll == SP_VTW_POINTER_POLLS) {
            g_vtw_fallback_shadow_count++;
            return SP_VTW_COPY_FALLBACK;
        }

        REG_WRITE(CARD_CTRL_VTW_SHADOW_READ4_REG, 1U);
        for (poll = 0U; poll < SP_VTW_POINTER_POLLS; ++poll) {
            status = REG_READ(CARD_CTRL_VTW_SHADOW_READ4_STATUS_REG);
            if ((status & CARD_CTRL_VTW_SHADOW_READ4_BUSY_BIT) == 0U &&
                (((status & CARD_CTRL_VTW_SHADOW_READ4_COUNT_MASK) - before) &
                 CARD_CTRL_VTW_SHADOW_READ4_COUNT_MASK) == word_index + 1U) {
                break;
            }
        }
        if (poll == SP_VTW_POINTER_POLLS) {
            g_vtw_fallback_shadow_count++;
            return SP_VTW_COPY_FALLBACK;
        }

        word = REG_READ(CARD_CTRL_VTW_SHADOW_READ4_DATA_REG);
        data[word_index * 4U] = (uint8_t)word;
        data[word_index * 4U + 1U] = (uint8_t)(word >> 8);
        data[word_index * 4U + 2U] = (uint8_t)(word >> 16);
        data[word_index * 4U + 3U] = (uint8_t)(word >> 24);
    }

    if ((REG_READ(CARD_CTRL_VTW_SHADOW_ADDR_REG) & 0x3FFFFUL) !=
        span->phys + (uint32_t)span->length) {
        g_vtw_fallback_shadow_count++;
        return SP_VTW_COPY_FALLBACK;
    }
    return SP_VTW_COPY_COMPLETE;
}

static uint8_t sp_vtw_direct_ramworks_read(uint32_t psram_addr,
                                            uint8_t *data,
                                            uint32_t length)
{
    uint8_t flush_rc;
    psdma_result_t dma_rc;

    flush_rc = sp_vtw_flush_ramworks_cache();
    if (flush_rc != SP_VTW_COPY_COMPLETE) {
        g_vtw_fallback_dma_count++;
        return flush_rc;
    }

    /* Prevent a dirty cache line from replacing DMA data later, then drop
     * the stale CPU copy after the transfer. */
    Xil_DCacheFlushRange((UINTPTR)data, length);
    dma_rc = psdma_transfer(PSDMA_OWNER_SMARTPORT,
                            psram_addr,
                            (uint32_t)(uintptr_t)data,
                            length,
                            PSDMA_MC_TO_DDR,
                            SP_VTW_DMA_TIMEOUT_US,
                            SP_VTW_ABORT_TIMEOUT_US);
    if (dma_rc == PSDMA_OK) {
        Xil_DCacheInvalidateRange((UINTPTR)data, length);
    } else {
        g_vtw_dma_fault = 1U;
        g_vtw_fallback_dma_count++;
    }
    if (dma_rc == PSDMA_ERR_ABORT ||
        sp_vtw_release_core_hold() == 0U) {
        return SP_VTW_COPY_FATAL;
    }
    return (dma_rc == PSDMA_OK)
               ? SP_VTW_COPY_COMPLETE : SP_VTW_COPY_FALLBACK;
}

static uint8_t sp_vtw_direct_write_source(uint16_t apple_addr,
                                           uint32_t sss,
                                           uint8_t *data)
{
    sp_vtw_span_t spans[SP_VTW_MAX_SPANS];
    uint32_t span_count;
    uint32_t i;

    if (data == NULL ||
        sp_vtw_direct_source_eligible(apple_addr, sss) == 0U ||
        sp_vtw_build_spans(apple_addr, sss, 0U,
                           spans, &span_count) == 0U) {
        return SP_VTW_COPY_FALLBACK;
    }
    for (i = 0U; i < span_count; ++i) {
        uint8_t rc;
        uint8_t *span_data = data + spans[i].data_offset;

        if (spans[i].bank > 1U) {
            rc = sp_vtw_direct_ramworks_read(
                spans[i].phys, span_data, spans[i].length);
        } else {
            rc = sp_vtw_shadow_read_span(&spans[i], span_data);
        }
        if (rc != SP_VTW_COPY_COMPLETE) {
            return rc;
        }
    }
    return (sp_vtw_is_live() != 0U)
               ? SP_VTW_COPY_COMPLETE : SP_VTW_COPY_FALLBACK;
}

/* Drain every queued command byte from the card's IN FIFO. The Apple-side ROM
 * queues the complete frame before writing CTRL, so execution sees it whole. */
static uint32_t sp_drain(uint8_t *buf, uint32_t max, uint32_t available)
{
    uint32_t n = 0U;

    /* EXECUTE freezes the complete frame in the IN ring. Its pointer starts
     * aligned after CLR_IN, so move full words first and leave only the short
     * command tail on the old byte register. */
    while (available >= 4U && n + 4U <= max) {
        const uint32_t word = REG_READ(SP_R_IN_HEAD4);

        buf[n++] = (uint8_t)word;
        buf[n++] = (uint8_t)(word >> 8);
        buf[n++] = (uint8_t)(word >> 16);
        buf[n++] = (uint8_t)(word >> 24);
        sp_ctl(SP_CTL_POP_IN4);
        available -= 4U;
    }
    while (available != 0U && n < max) {
        const uint32_t head = REG_READ(SP_R_IN_HEAD);
        if ((head & 0x100U) == 0U) {
            break;
        }
        buf[n++] = (uint8_t)head;
        sp_ctl(SP_CTL_POP_IN);
        available--;
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
                              uint8_t allow_readahead,
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

    if (sp_cache_get_block(dev, block_num, 1U, 0U, &cache_addr) != 0) {
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
    XTime dispatch_tick;
    XTime irq_tick = 0U;
    uint8_t latency_valid;

    /* Capture dispatch before any MMIO or FIFO drain. The old placement
     * measured command-drain time as scheduler delay and hid the cost that
     * the counter was meant to expose. */
    XTime_GetTime(&dispatch_tick);
    latency_valid = g_irq_tick_valid;
    if (latency_valid != 0U) {
        irq_tick = g_irq_tick;
    }

    const uint32_t hw_status = sp_hw_status();
    const uint8_t accelerated =
        ((hw_status & SP_ST_EXEC_VTW) != 0U) ? 1U : 0U;
    const uint32_t sss_snapshot = REG_READ(SP_R_SSS);
    const uint8_t raw_family =
        (uint8_t)(REG_READ(SP_R_CONTROL) & 0xFFU);
    const uint8_t family = raw_family & (uint8_t)~SP_FAMILY_PREFLIGHT_BIT;
    uint32_t len = sp_drain(g_cmd_buf, sizeof(g_cmd_buf),
                            SP_ST_IN_COUNT(hw_status));
    uint8_t result = ERR_DEVICE_OK;
    uint8_t direct_completed = 0U;
    uint8_t direct_write_requested = 0U;
    uint8_t activity_cmd = 0U;
    sp_device_t *dev = NULL;

    sp_ctl(SP_CTL_CLR_OUT);
    sp_response_reset();

    /* The vTW ROM can preflight a block write before it spends 512 core
     * accesses streaming the payload. The preflight drains and saves the
     * command prefix. The next normal-family CTRL write supplies either no
     * bytes (direct shadow read) or only the 512-byte fallback payload. */
    if ((raw_family & SP_FAMILY_PREFLIGHT_BIT) == 0U &&
        g_vtw_write_preflight.active != 0U) {
        if (g_vtw_write_preflight.family == family &&
            len + g_vtw_write_preflight.prefix_len <= sizeof(g_cmd_buf)) {
            memmove(g_cmd_buf + g_vtw_write_preflight.prefix_len,
                    g_cmd_buf, len);
            memcpy(g_cmd_buf, g_vtw_write_preflight.prefix,
                   g_vtw_write_preflight.prefix_len);
            direct_write_requested =
                (g_vtw_write_preflight.direct != 0U && len == 0U) ? 1U : 0U;
            len += g_vtw_write_preflight.prefix_len;
        }
        memset(&g_vtw_write_preflight, 0, sizeof(g_vtw_write_preflight));
    }

    if ((raw_family & SP_FAMILY_PREFLIGHT_BIT) != 0U) {
        uint16_t buffer_addr = 0U;
        uint8_t prefix_len = 0U;
        uint8_t write_command = 0U;

        if (accelerated != 0U && family == SP_FAMILY_PRODOS && len >= 6U) {
            buffer_addr = (uint16_t)g_cmd_buf[2] |
                          ((uint16_t)g_cmd_buf[3] << 8);
            prefix_len = 6U;
            write_command = (g_cmd_buf[0] == BLK_CMD_WRITE) ? 1U : 0U;
        } else if (accelerated != 0U && family == SP_FAMILY_SP && len >= 10U) {
            buffer_addr = (uint16_t)g_cmd_buf[3] |
                          ((uint16_t)g_cmd_buf[4] << 8);
            prefix_len = 10U;
            write_command = (g_cmd_buf[0] == 0x02U) ? 1U : 0U;
        }

        memset(&g_vtw_write_preflight, 0, sizeof(g_vtw_write_preflight));
        if (write_command != 0U) {
            g_vtw_write_preflight.active = 1U;
            g_vtw_write_preflight.family = family;
            g_vtw_write_preflight.prefix_len = prefix_len;
            g_vtw_write_preflight.direct =
                sp_vtw_direct_source_eligible(buffer_addr, sss_snapshot);
            memcpy(g_vtw_write_preflight.prefix, g_cmd_buf, prefix_len);
            direct_completed = g_vtw_write_preflight.direct;
            g_vtw_write_preflight_count++;
        } else {
            result = ERR_BADCTL;
        }
        sp_response_append(result);
    }
    else if (family == SP_FAMILY_PRODOS && len >= 6U) {
        const uint8_t cmd  = g_cmd_buf[0];
        const uint8_t unit = g_cmd_buf[1];
        const uint16_t buffer_addr = (uint16_t)g_cmd_buf[2] |
                                     ((uint16_t)g_cmd_buf[3] << 8);
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
            sp_response_append(result);
            sp_response_append((uint8_t)(blocks & 0xFFU));
            sp_response_append((uint8_t)((blocks >> 8) & 0xFFU));
            break;
        }

        case BLK_CMD_READ: {
            uint32_t cache_addr;
            if (dev == NULL || dev->image_open == 0U) {
                result = ERR_DEVICE_NOT_CONNECTED;
            } else if (block_num >= dev->image_blocks) {
                result = ERR_DEVICE_IO_ERROR;
            } else if (sp_cache_get_block(dev, block_num, 0U, accelerated,
                                          &cache_addr) != 0) {
                result = ERR_DEVICE_IO_ERROR;
            } else {
                uint8_t direct_rc = SP_VTW_COPY_FALLBACK;

                sp_response_append(result);
                if (accelerated != 0U) {
                    direct_rc = sp_vtw_direct_read_block(
                        buffer_addr, sss_snapshot,
                        cache_ptr_from_addr(cache_addr));
                }
                if (direct_rc == SP_VTW_COPY_COMPLETE) {
                    direct_completed = 1U;
                    g_vtw_direct_count++;
                } else {
                    if (accelerated != 0U) {
                        g_vtw_direct_fallback_count++;
                    }
                    if (direct_rc == SP_VTW_COPY_FATAL) {
                        result = ERR_DEVICE_IO_ERROR;
                        g_response[0] = result;
                    } else {
                        sp_response_append_buf(
                            cache_ptr_from_addr(cache_addr), SP_BLOCK_SIZE);
                    }
                }
                sp_crclog_add(device_index_from_ptr(dev), block_num, cache_ptr_from_addr(cache_addr));
                break;
            }
            sp_response_append(result);
            break;
        }

        case BLK_CMD_WRITE: {
            const uint8_t *write_data = NULL;

            if (dev == NULL || dev->image_open == 0U) {
                result = ERR_DEVICE_NOT_CONNECTED;
            } else if (dev->read_only) {
                result = ERR_NOWRITE;
            } else if (block_num >= dev->image_blocks) {
                result = ERR_DEVICE_IO_ERROR;
            } else {
                if (direct_write_requested != 0U) {
                    const uint8_t direct_rc = sp_vtw_direct_write_source(
                        buffer_addr, sss_snapshot, g_scratch);

                    if (direct_rc == SP_VTW_COPY_COMPLETE) {
                        write_data = g_scratch;
                        g_vtw_direct_write_count++;
                    } else {
                        result = ERR_DEVICE_IO_ERROR;
                        g_vtw_write_fault_count++;
                    }
                } else if (len >= 518U) {
                    write_data = &g_cmd_buf[6];
                    if (accelerated != 0U) {
                        g_vtw_write_stream_count++;
                    }
                } else {
                    result = ERR_DEVICE_IO_ERROR;
                }
                if (write_data != NULL &&
                    sp_write_block_to_image(dev, block_num, write_data) != 0) {
                    result = ERR_DEVICE_IO_ERROR;
                }
            }
            sp_response_append(result);
            break;
        }

        default:
            result = ERR_BADCTL;
            sp_response_append(result);
            break;
        }
    }
    else if (family == SP_FAMILY_SP && len >= 10U) {
        const uint8_t cmd = g_cmd_buf[0];
        const uint8_t *list = &g_cmd_buf[1];   /* 9 cmdlist bytes */
        const uint8_t unit = list[1];
        const uint16_t buffer_addr = (uint16_t)list[2] |
                                     ((uint16_t)list[3] << 8);
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
                sp_response_append(result);
                break;
            }
            length = build_sp_status(dev, unit, status_code);
            if (length == 0U) {
                result = ERR_BADCTL;
                sp_response_append(result);
                break;
            }
            sp_response_append(result);
            sp_response_append((uint8_t)(length & 0xFFU));
            sp_response_append((uint8_t)((length >> 8) & 0xFFU));
            sp_response_append_buf(g_scratch, length);
            break;
        }

        case 0x01: {   /* READ BLOCK */
            uint32_t cache_addr;
            if (dev == NULL || dev->image_open == 0U) {
                result = ERR_DEVICE_NOT_CONNECTED;
            } else if (block_num >= dev->image_blocks) {
                result = ERR_DEVICE_IO_ERROR;
            } else if (sp_cache_get_block(dev, block_num, 0U, accelerated,
                                          &cache_addr) != 0) {
                result = ERR_DEVICE_IO_ERROR;
            } else {
                uint8_t direct_rc = SP_VTW_COPY_FALLBACK;

                sp_response_append(result);
                if (accelerated != 0U) {
                    direct_rc = sp_vtw_direct_read_block(
                        buffer_addr, sss_snapshot,
                        cache_ptr_from_addr(cache_addr));
                }
                if (direct_rc == SP_VTW_COPY_COMPLETE) {
                    direct_completed = 1U;
                    g_vtw_direct_count++;
                } else {
                    if (accelerated != 0U) {
                        g_vtw_direct_fallback_count++;
                    }
                    if (direct_rc == SP_VTW_COPY_FATAL) {
                        result = ERR_DEVICE_IO_ERROR;
                        g_response[0] = result;
                    } else {
                        sp_response_append_buf(
                            cache_ptr_from_addr(cache_addr), SP_BLOCK_SIZE);
                    }
                }
                sp_crclog_add(device_index_from_ptr(dev), block_num, cache_ptr_from_addr(cache_addr));
                break;
            }
            sp_response_append(result);
            break;
        }

        case 0x02: {   /* WRITE BLOCK: 512 data bytes follow the list */
            const uint8_t *write_data = NULL;

            if (dev == NULL || dev->image_open == 0U) {
                result = ERR_DEVICE_NOT_CONNECTED;
            } else if (dev->read_only) {
                result = ERR_NOWRITE;
            } else if (block_num >= dev->image_blocks) {
                result = ERR_DEVICE_IO_ERROR;
            } else {
                if (direct_write_requested != 0U) {
                    const uint8_t direct_rc = sp_vtw_direct_write_source(
                        buffer_addr, sss_snapshot, g_scratch);

                    if (direct_rc == SP_VTW_COPY_COMPLETE) {
                        write_data = g_scratch;
                        g_vtw_direct_write_count++;
                    } else {
                        result = ERR_DEVICE_IO_ERROR;
                        g_vtw_write_fault_count++;
                    }
                } else if (len >= 522U) {
                    write_data = &g_cmd_buf[10];
                    if (accelerated != 0U) {
                        g_vtw_write_stream_count++;
                    }
                } else {
                    result = ERR_DEVICE_IO_ERROR;
                }
                if (write_data != NULL &&
                    sp_write_block_to_image(dev, block_num, write_data) != 0) {
                    result = ERR_DEVICE_IO_ERROR;
                }
            }
            sp_response_append(result);
            break;
        }

        case 0x03:     /* FORMAT: images arrive formatted */
            result = ERR_NOWRITE;
            sp_response_append(result);
            break;

        default:       /* INIT/OPEN/CLOSE/CONTROL/char READ/WRITE */
            result = ERR_BADCTL;
            sp_response_append(result);
            break;
        }
    }
    else {
        /* Malformed frame: answer with BADCTL so the firmware wait
         * terminates, and flush whatever partial state remains. */
        result = ERR_BADCTL;
        sp_response_append(result);
    }

    if (g_response_overflow != 0U) {
        result = ERR_DEVICE_IO_ERROR;
    }
    sp_response_commit(accelerated);
    smartport_note_activity(activity_cmd, dev, result);
    /* READY last: the firmware may pop the instant bit7 rises. */
    sp_ctl(SP_CTL_SET_READY | SP_CTL_ACK_EXEC | SP_CTL_CLR_IN |
           ((direct_completed != 0U) ? SP_CTL_SET_DIRECT : 0U));

    if (latency_valid != 0U) {
        if (dispatch_tick >= irq_tick) {
            XTime ready_tick;
            XTime dispatch_delta = dispatch_tick - irq_tick;
            XTime ready_delta;

            XTime_GetTime(&ready_tick);
            ready_delta = ready_tick - irq_tick;
            g_dispatch_ticks_last = dispatch_delta;
            g_ready_ticks_last = ready_delta;
            g_dispatch_ticks_total += dispatch_delta;
            g_ready_ticks_total += ready_delta;
            if (dispatch_delta > g_dispatch_ticks_max) {
                g_dispatch_ticks_max = dispatch_delta;
            }
            if (ready_delta > g_ready_ticks_max) {
                g_ready_ticks_max = ready_delta;
            }
            g_latency_sample_count++;
        }
        g_irq_tick_valid = 0U;
    }
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
    XTime now;

    (void)cb;
    XTime_GetTime(&now);
    g_irq_tick = now;
    g_irq_tick_valid = 1U;
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
        memset(dev, 0, sizeof(*dev));
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
    int first_error = 0;

    for (i = 0U; i < SP_MAX_DEVICES; ++i) {
        int rc = load_device(&g_devices[i]);
        if (rc != 0 && first_error == 0) {
            first_error = rc;
        }
    }

    /* A mounted RAM32 or another good unit must not hide a failed file.
     * Callers that care about one unit can retry it directly; callers that
     * refresh all media need to know that the result was only partial. */
    return first_error;
}

static int sp_cache_get_block(sp_device_t *dev,
                              uint32_t block_num,
                              uint8_t for_write,
                              uint8_t allow_readahead,
                              uint32_t *cache_addr)
{
    uint8_t device_index;
    uint32_t i;
    uint32_t victim = 0U;
    uint32_t oldest = UINT32_MAX;
    uint32_t fill_index;
    uint32_t fill_blocks = 1U;
    uint32_t fill_slots = 1U;
    uint32_t fill_bytes;
    uint32_t stamp;
    uint8_t *dst;
    UINT br = 0U;
    FRESULT fr;

    if (cache_addr == NULL || dev == NULL || dev->image_open == 0U ||
        block_num >= dev->image_blocks) {
        return -1;
    }
    if (dev->is_ram) {
        /* The block lives at a fixed DDR address; no cache slot, no
         * fill. cache_ptr_from_addr() is a flat DDR translation, so
         * consumers work unchanged. */
        g_cache_bypass_count++;
        *cache_addr = SP_RAMDISK_BASE + block_num * SP_BLOCK_SIZE;
        return 0;
    }

    device_index = device_index_from_ptr(dev);

    for (i = 0U; i < SP_CACHE_BLOCK_COUNT; ++i) {
        if (g_cache[i].valid != 0U &&
            g_cache[i].device_index == device_index &&
            g_cache[i].block_num == block_num) {
            g_cache_hit_count++;
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
    g_cache_miss_count++;
    fill_index = victim;
    if (for_write == 0U && allow_readahead != 0U) {
        uint32_t blocks_left = dev->image_blocks - block_num;

        fill_blocks = (blocks_left < SP_VTW_READAHEAD_BLOCKS)
                          ? blocks_left : SP_VTW_READAHEAD_BLOCKS;
        fill_slots = SP_VTW_READAHEAD_BLOCKS;
        /* Keep each fill contiguous in DDR so FatFs performs one multi-block
         * read. Aligning the LRU victim to an eight-slot group preserves the
         * existing 16 KB cache size while giving vTW four read-ahead lines. */
        fill_index = victim & ~(SP_VTW_READAHEAD_BLOCKS - 1U);
    }

    for (i = 0U; i < fill_slots; ++i) {
        g_cache[fill_index + i].valid = 0U;
    }
    /* A read-ahead group re-loads blocks that may already sit in slots
     * outside it. Retire those copies: with duplicates present, a write
     * would update only the first match, and evicting that one later
     * would expose the stale twin to reads. */
    for (i = 0U; i < SP_CACHE_BLOCK_COUNT; ++i) {
        if ((i < fill_index || i >= fill_index + fill_slots) &&
            g_cache[i].valid != 0U &&
            g_cache[i].device_index == device_index &&
            g_cache[i].block_num >= block_num &&
            g_cache[i].block_num < block_num + fill_blocks) {
            g_cache[i].valid = 0U;
        }
    }

    fill_bytes = fill_blocks * SP_BLOCK_SIZE;
    dst = cache_ptr_from_addr(cache_addr_for_index(fill_index));
    fr = f_lseek(&dev->image_file,
                 (FSIZE_t)dev->image_data_offset +
                 ((FSIZE_t)block_num * SP_BLOCK_SIZE));
    if (fr != FR_OK) {
        return -(int)fr;
    }

    fr = f_read(&dev->image_file, dst, (UINT)fill_bytes, &br);
    if (fr != FR_OK || br != (UINT)fill_bytes) {
        return (fr != FR_OK) ? -(int)fr : -(int)FR_DISK_ERR;
    }

    stamp = ++g_cache_clock;
    for (i = 0U; i < fill_blocks; ++i) {
        g_cache[fill_index + i].valid = 1U;
        g_cache[fill_index + i].device_index = device_index;
        g_cache[fill_index + i].block_num = block_num + i;
        g_cache[fill_index + i].last_used = stamp;
    }
    *cache_addr = cache_addr_for_index(fill_index);
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

    /* RAM32 borrows an otherwise unused device entry. Remove it before
     * storing a real path: close_device() clears a RAM device, so copying the
     * path first would erase the new selection before load_device() opens it. */
    if (g_devices[index].is_ram != 0U) {
        close_device(&g_devices[index]);
        g_ramdisk_state = 0U;
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

void smartport_service_apple_reset(void)
{
    uint32_t cpsr;

    __asm__ volatile ("mrs %0, cpsr" : "=r"(cpsr));
    __asm__ volatile ("cpsid i");
    g_cmd_pending_count = 0U;
    g_irq_tick_valid = 0U;
    memset(&g_vtw_write_preflight, 0, sizeof(g_vtw_write_preflight));
    if ((cpsr & 0x80) == 0) {
        __asm__ volatile ("cpsie i");
    }
    /* The PL cleared the transport at the RES# edge; this sweep also
     * removes a response that a command mid-execution on this CPU pushed
     * after the release. Deliberately no SET_READY: the next command
     * starts from a clean, not-ready transport. */
    sp_ctl(SP_CTL_CLR_IN | SP_CTL_CLR_OUT | SP_CTL_ACK_EXEC);
}

void smartport_service_uart_status(uint32_t uart_base)
{
    char line[128];
    const uint32_t st = REG_READ(SP_R_STATUS);
    const uint32_t ctl = REG_READ(SP_R_CONTROL);
    const uint32_t samples = g_latency_sample_count;

    snprintf(line, sizeof(line),
             "sd: hw ready=%lu exec=%lu in=%lu out=%lu drypop=%lu lastctl=$%02lX\r\n",
             (unsigned long)((st >> 29) & 1U),
             (unsigned long)((st >> 28) & 1U),
             (unsigned long)(st & 0x7FFU),
             (unsigned long)((st >> 16) & 0x7FFU),
             (unsigned long)((ctl >> 8) & 0xFFFFU),
             (unsigned long)(ctl & 0xFFU));
    uart_puts(uart_base, line);
    snprintf(line, sizeof(line),
             "sd: svc irq=%lu pending=%lu status=%lu read=%lu write=%lu\r\n",
             (unsigned long)g_irq_count,
             (unsigned long)g_cmd_pending_count,
             (unsigned long)g_activity_status_count,
             (unsigned long)g_activity_read_count,
             (unsigned long)g_activity_write_count);
    uart_puts(uart_base, line);
    snprintf(line, sizeof(line),
             "sd: vtw direct=%lu rwblk=%lu postblk=%lu fallback=%lu\r\n",
             (unsigned long)g_vtw_direct_count,
             (unsigned long)g_vtw_direct_ramworks_count,
             (unsigned long)g_vtw_direct_posted_count,
             (unsigned long)g_vtw_direct_fallback_count);
    uart_puts(uart_base, line);
    snprintf(line, sizeof(line),
             "sd: vtw fb range=%lu state=%lu map=%lu shadow=%lu post=%lu dma=%lu\r\n",
             (unsigned long)g_vtw_fallback_range_count,
             (unsigned long)g_vtw_fallback_state_count,
             (unsigned long)g_vtw_fallback_map_count,
             (unsigned long)g_vtw_fallback_shadow_count,
             (unsigned long)g_vtw_fallback_post_count,
             (unsigned long)g_vtw_fallback_dma_count);
    uart_puts(uart_base, line);
    snprintf(line, sizeof(line),
             "sd: vtw split blocks=%lu spans=%lu max=%lu\r\n",
             (unsigned long)g_vtw_split_block_count,
             (unsigned long)g_vtw_split_span_count,
             (unsigned long)g_vtw_split_max_spans);
    uart_puts(uart_base, line);
    snprintf(line, sizeof(line),
             "sd: vtw write direct=%lu preflight=%lu stream=%lu fault=%lu\r\n",
             (unsigned long)g_vtw_direct_write_count,
             (unsigned long)g_vtw_write_preflight_count,
             (unsigned long)g_vtw_write_stream_count,
             (unsigned long)g_vtw_write_fault_count);
    uart_puts(uart_base, line);
    snprintf(line, sizeof(line),
             "sd: cache hit=%lu miss=%lu bypass=%lu post-credit-reads=%lu dma-owner=%u\r\n",
             (unsigned long)g_cache_hit_count,
             (unsigned long)g_cache_miss_count,
             (unsigned long)g_cache_bypass_count,
             (unsigned long)g_post_credit_read_count,
             (unsigned)psdma_current_owner());
    uart_puts(uart_base, line);
    snprintf(line, sizeof(line),
             "sd: lat dispatch us last=%lu avg=%lu max=%lu samples=%lu\r\n",
             (unsigned long)sp_ticks_to_us(g_dispatch_ticks_last),
             (unsigned long)sp_ticks_to_us(
                 (samples != 0U) ? g_dispatch_ticks_total / samples : 0U),
             (unsigned long)sp_ticks_to_us(g_dispatch_ticks_max),
             (unsigned long)samples);
    uart_puts(uart_base, line);
    snprintf(line, sizeof(line),
             "sd: lat ready us last=%lu avg=%lu max=%lu\r\n",
             (unsigned long)sp_ticks_to_us(g_ready_ticks_last),
             (unsigned long)sp_ticks_to_us(
                 (samples != 0U) ? g_ready_ticks_total / samples : 0U),
             (unsigned long)sp_ticks_to_us(g_ready_ticks_max));
    uart_puts(uart_base, line);
    snprintf(line, sizeof(line),
             "sd: init=%u fs=%u slot=%u ramdisk=%u\r\n",
             (unsigned)g_devices_initialized,
             (unsigned)g_fs_mounted,
             (unsigned)g_smartport_slot,
             (unsigned)g_ramdisk_state);
    uart_puts(uart_base, line);
    for (uint8_t i = 0U; i < SP_MAX_DEVICES; ++i) {
        const sp_device_t *dev = &g_devices[i];
        snprintf(line, sizeof(line),
                 "sd: SP%u open=%u ram=%u ro=%u blocks=%lu path=%.44s\r\n",
                 (unsigned)i + 1U,
                 (unsigned)dev->image_open,
                 (unsigned)dev->is_ram,
                 (unsigned)dev->read_only,
                 (unsigned long)dev->image_blocks,
                 dev->image_path);
        uart_puts(uart_base, line);
    }
}

void smartport_service_poll(void)
{
    const uint32_t vtw_status = REG_READ(CARD_CTRL_VTW_STATUS_REG);

    /* A DMA fault stays latched for one vTW takeover. Apple RESET keeps the
     * takeover alive, so clear it only after acceleration is truly stopped. */
    if ((vtw_status & (CARD_CTRL_VTW_STATUS_ENABLE_EFF |
                       CARD_CTRL_VTW_STATUS_CORE_RUN)) !=
        (CARD_CTRL_VTW_STATUS_ENABLE_EFF |
         CARD_CTRL_VTW_STATUS_CORE_RUN)) {
        g_vtw_dma_fault = 0U;
    }
    sp_ramdisk_refresh(UART0_BASE);
    /* Run one queued command per poll. The PL stalls the Apple bus until status
     * is posted, so the count is normally 0 or 1. Limiting each poll prevents
     * an unexpected IRQ burst from starving other main-loop services.
     *
     * Hardware EXEC_PENDING is the sole execution authority: a nonzero
     * pending count without it is stale (an Apple RES# aborted the
     * transaction). Executing anyway would parse an empty/partial frame
     * and push an orphan response, offsetting every later reply. */
    if ((sp_hw_status() & SP_ST_EXEC_PENDING) == 0U) {
        if (g_cmd_pending_count != 0U) {
            uint32_t stale_cpsr;
            __asm__ volatile ("mrs %0, cpsr" : "=r"(stale_cpsr));
            __asm__ volatile ("cpsid i");
            g_cmd_pending_count = 0U;
            g_irq_tick_valid = 0U;
            if ((stale_cpsr & 0x80) == 0) {
                __asm__ volatile ("cpsie i");
            }
        }
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

uint8_t smartport_service_has_pending(void)
{
    return (g_cmd_pending_count != 0U) ? 1U : 0U;
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
    if (sp_cache_get_block(dev, block_num, 0U, 0U, &cache_addr) != 0) return -1;

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
            /* A configured file that failed to open still owns its unit.
             * Taking it for RAM32 would hide the load error and could erase a
             * replacement path when the user selects new media. */
            if (g_devices[i].image_open == 0U &&
                g_devices[i].image_path[0] == '\0') {
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
                close_device(&g_devices[i]);
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
