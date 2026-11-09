#include "pc_transporter_cga_capture.h"

#include <stdbool.h>
#include <stddef.h>

#include "xil_cache.h"
#include "xil_io.h"
#include "xil_mmu.h"
#include "xiltimer.h"

#include "pc_transporter_memory.h"
#include "pc_transporter_regs.h"

#define CAPTURE_CONTROL_OFFSET 0xC0000U
#define CAPTURE_SLOT_BYTES    0x10000U
#define CAPTURE_SLOT_COUNT    3U
#define CAPTURE_SLOT_MASK     3U
#define CGA_BANK_BYTES        (PCT_CGA_RAM_BYTES / 2U)
#define CGA_GRAPHICS_ENABLE   0x0AU
#define CGA_RASTER_HBLANK     (1U << 14)
#define CGA_RASTER_ADJUST     (1U << 15)
#define CAPTURE_RUNNING_MASK  (PCT_ENABLED | PCT_RESET | PCT_HALT | PCT_FAULT)
#define CAPTURE_STATUS_MASK   (CAPTURE_RUNNING_MASK | 0x00FF0000U)
#define CAPTURE_CONFIG_US     500U
#define CAPTURE_RASTER_US     6U
#define CAPTURE_TICKS(us)     (((uint64_t)(COUNTS_PER_SECOND) * (us)) / 1000000ULL)

typedef struct {
    uint32_t state;
    uint32_t geometry;
    uint32_t crtc_low;
} capture_key_t;

typedef struct {
    /* Slots are numbered 1 through 3. Zero means no publication or claim. */
    uint32_t published;
    uint32_t reader;
    uint32_t completed;
    uint32_t dropped;
    capture_key_t key[CAPTURE_SLOT_COUNT];
} capture_control_t;

typedef struct {
    capture_key_t key;
    uint32_t status;
    unsigned row_bytes;
    unsigned rows;
    unsigned rasters_per_row;
    unsigned start_byte;
} capture_mode_t;

static volatile capture_control_t *const shared =
    (volatile capture_control_t *)(uintptr_t)(PCT_DDR_BASE + CAPTURE_CONTROL_OFFSET);
static const volatile uint8_t *const vram =
    (const volatile uint8_t *)(uintptr_t)(PCT_DDR_BASE + PCT_CGA_RAM_BASE);

/* Only CPU1 updates this state. */
static capture_mode_t capture_mode;
static bool capture_enabled;
static bool frame_seen;
static bool frame_done;
static bool collecting;
static uint16_t frame_sequence;
static unsigned writer_slot;
static unsigned next_line;
static uint32_t publication_sequence;
static XTime next_config_probe;
static XTime next_raster_probe;

static inline void capture_sync(void)
{
    __asm__ volatile ("dsb sy" ::: "memory");
}

static volatile uint8_t *capture_slot(unsigned slot)
{
    return (volatile uint8_t *)(uintptr_t)(PCT_DDR_BASE + CAPTURE_CONTROL_OFFSET +
                                           slot * CAPTURE_SLOT_BYTES);
}

void pc_transporter_cga_capture_reset(void)
{
    shared->published = 0U;
    shared->reader = 0U;
    shared->completed = 0U;
    shared->dropped = 0U;
    /* CPU0 may still have its startup cacheable mapping at this point. */
    Xil_DCacheFlushRange((uintptr_t)shared, sizeof(*shared));
    capture_sync();
}

void pc_transporter_cga_capture_init(void)
{
    Xil_SetTlbAttributes(PCT_DDR_BASE, NORM_NONCACHE);
    capture_sync();
    capture_enabled = false;
    frame_seen = false;
    frame_done = false;
    collecting = false;
    publication_sequence = shared->published & ~CAPTURE_SLOT_MASK;
    shared->published = 0U;
    next_config_probe = 0U;
    next_raster_probe = 0U;
}

static bool keys_match(capture_key_t a, capture_key_t b)
{
    return a.state == b.state && a.geometry == b.geometry && a.crtc_low == b.crtc_low;
}

static bool read_capture_mode(capture_mode_t *mode)
{
    const uint32_t status = Xil_In32(PCT_STATUS) & CAPTURE_STATUS_MASK;
    if ((status & CAPTURE_RUNNING_MASK) != PCT_ENABLED) {
        return false;
    }
    const uint32_t state = Xil_In32(PCT_CGA_STATE);
    if ((state & CGA_GRAPHICS_ENABLE) != CGA_GRAPHICS_ENABLE) {
        return false;
    }
    const uint32_t geometry = Xil_In32(PCT_CGA_GEOMETRY);
    const unsigned row_bytes = (geometry & 0xFFU) * 2U;
    const unsigned rows = (geometry >> 8) & 0x7FU;
    const unsigned rasters_per_row = ((geometry >> 16) & 0x1FU) + 1U;
    if (row_bytes * rows <= CGA_BANK_BYTES ||
        row_bytes * rows * rasters_per_row > CAPTURE_SLOT_BYTES) {
        return false;
    }
    const uint32_t crtc_low = Xil_In32(PCT_CGA_CRTC_LOW);
    if (state != Xil_In32(PCT_CGA_STATE) ||
        status != (Xil_In32(PCT_STATUS) & CAPTURE_STATUS_MASK)) {
        return false;
    }
    *mode = (capture_mode_t) {
        .key = {state, geometry, crtc_low},
        .status = status,
        .row_bytes = row_bytes,
        .rows = rows,
        .rasters_per_row = rasters_per_row,
        .start_byte = (((crtc_low >> 16) & 0xFFU) << 8 | (crtc_low >> 24)) * 2U,
    };
    return true;
}

static void drop_frame(void)
{
    if (!frame_done) {
        shared->dropped++;
        frame_done = true;
    }
    collecting = false;
}

static bool refresh_capture_mode(void)
{
    capture_mode_t mode;
    if (!read_capture_mode(&mode)) {
        if (collecting) {
            drop_frame();
        }
        if (capture_enabled) {
            shared->published = 0U;
        }
        capture_enabled = false;
        return false;
    }
    const bool unchanged = capture_enabled && mode.status == capture_mode.status &&
                           keys_match(mode.key, capture_mode.key);
    if (!unchanged) {
        if (collecting) {
            drop_frame();
        }
        shared->published = 0U;
        capture_mode = mode;
        capture_enabled = true;
    }
    return unchanged;
}

static void copy_bytes(volatile uint8_t *dest, const volatile uint8_t *src, unsigned bytes)
{
    /* CRTC addresses and line sizes are even. Use aligned word copies for
     * the common geometry to reduce uncached DDR transactions. */
    if ((((uintptr_t)dest | (uintptr_t)src) & 3U) == 0U) {
        while (bytes >= 4U) {
            *(volatile uint32_t *)dest = *(const volatile uint32_t *)src;
            dest += 4U;
            src += 4U;
            bytes -= 4U;
        }
    }
    while (bytes != 0U) {
        *(volatile uint16_t *)dest = *(const volatile uint16_t *)src;
        dest += 2U;
        src += 2U;
        bytes -= 2U;
    }
}

static void capture_scanline(unsigned row, unsigned raster)
{
    const unsigned row_bytes = capture_mode.row_bytes;
    const unsigned offset = (capture_mode.start_byte + row * row_bytes) &
                            (CGA_BANK_BYTES - 1U);
    const volatile uint8_t *bank = vram + (raster & 1U) * CGA_BANK_BYTES;
    volatile uint8_t *dest = capture_slot(writer_slot) + next_line * row_bytes;
    const unsigned first = (row_bytes < CGA_BANK_BYTES - offset) ?
                            row_bytes : (CGA_BANK_BYTES - offset);
    copy_bytes(dest, bank + offset, first);
    copy_bytes(dest + first, bank, row_bytes - first);
}

void pc_transporter_cga_capture_poll(void)
{
    XTime now;
    XTime_GetTime(&now);
    if (now >= next_config_probe) {
        next_config_probe = now + CAPTURE_TICKS(CAPTURE_CONFIG_US);
        (void)refresh_capture_mode();
    }
    if (!capture_enabled || now < next_raster_probe) {
        return;
    }
    next_raster_probe = now + CAPTURE_TICKS(CAPTURE_RASTER_US);

    const uint32_t beam = Xil_In32(PCT_CGA_RASTER);
    const uint16_t frame = (uint16_t)(beam >> 16);
    const unsigned row = (beam >> 5) & 0x7FU;
    const unsigned raster = beam & 0x1FU;
    if (!frame_seen || frame != frame_sequence) {
        if (frame_seen && !frame_done) {
            drop_frame();
        }
        frame_sequence = frame;
        frame_seen = true;
        frame_done = false;
    }
    if (frame_done) {
        return;
    }
    if ((beam & CGA_RASTER_ADJUST) != 0U || row >= capture_mode.rows) {
        drop_frame();
        return;
    }
    if ((beam & CGA_RASTER_HBLANK) != 0U || raster >= capture_mode.rasters_per_row) {
        return;
    }
    if (!collecting) {
        if (row != 0U || raster != 0U) {
            drop_frame();
            return;
        }
        const unsigned published_slot = shared->published & CAPTURE_SLOT_MASK;
        const unsigned reader_slot = shared->reader;
        for (writer_slot = 1U; writer_slot <= CAPTURE_SLOT_COUNT; writer_slot++) {
            if (writer_slot != published_slot && writer_slot != reader_slot) {
                break;
            }
        }
        next_line = 0U;
        collecting = true;
    }

    const unsigned line = row * capture_mode.rasters_per_row + raster;
    if (line > next_line) {
        drop_frame();
    }
    if (line != next_line) {
        return;
    }
    capture_scanline(row, raster);
    if (Xil_In32(PCT_CGA_RASTER) != beam) {
        return;
    }
    next_line++;
    if (next_line != capture_mode.rows * capture_mode.rasters_per_row) {
        return;
    }

    if (refresh_capture_mode()) {
        shared->key[writer_slot - 1U] = capture_mode.key;
        capture_sync();
        publication_sequence += CAPTURE_SLOT_COUNT + 1U;
        shared->published = publication_sequence | writer_slot;
        shared->completed++;
        frame_done = true;
        collecting = false;
    }
}

const volatile uint8_t *pc_transporter_cga_capture_claim(uint32_t state,
                                                        uint32_t geometry,
                                                        uint32_t crtc_low)
{
    const capture_key_t requested = {state, geometry, crtc_low};
    if (shared->reader != 0U) {
        return NULL;
    }
    const uint32_t publication = shared->published;
    const unsigned slot = publication & CAPTURE_SLOT_MASK;
    if (slot == 0U) {
        return NULL;
    }
    shared->reader = slot;
    capture_sync();
    if (shared->published == publication && keys_match(shared->key[slot - 1U], requested)) {
        return capture_slot(slot);
    }
    pc_transporter_cga_capture_release();
    return NULL;
}

void pc_transporter_cga_capture_release(void)
{
    capture_sync();
    shared->reader = 0U;
}

void pc_transporter_cga_capture_get_stats(uint32_t *completed, uint32_t *dropped)
{
    *completed = shared->completed;
    *dropped = shared->dropped;
}
