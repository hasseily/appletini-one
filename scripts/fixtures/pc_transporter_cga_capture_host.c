#define _GNU_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include "xil_cache.h"
#include "xil_mmu.h"
#include "xiltimer.h"
#include "pc_transporter_cga_capture.h"
#include "pc_transporter_regs.h"
#include "pc_transporter_service.h"

#define MODE_STATE 0x00010F1AU
#define GEOMETRY   0x00017830U
#define CRTC_LOW   0xF00F0000U
#define ROW_BYTES  96U
#define HEIGHT     240U
#define START_BYTE 0x1FE0U
#define BANK_BYTES 0x2000U
#define VRAM_BYTES (2U * BANK_BYTES)

static uint8_t *const vram = (uint8_t *)(uintptr_t)(PCT_DDR_BASE + 0xB8000U);
static uint32_t status;
static uint32_t mode_state = MODE_STATE;
static uint32_t geometry = GEOMETRY;
static uint32_t crtc_low = CRTC_LOW;
static uint32_t beam;
static unsigned beam_reads;
static unsigned mmio_reads;
static bool change_beam_during_copy;
static bool change_mode_during_copy;
static bool race_claim;
static XTime now;

void Xil_SetTlbAttributes(uintptr_t address, unsigned attrs)
{
    assert(address == PCT_DDR_BASE && attrs == NORM_NONCACHE);
}

void Xil_DCacheFlushRange(uintptr_t address, unsigned length)
{
    assert(address == PCT_DDR_BASE + 0xC0000U && length != 0U);
}

void XTime_GetTime(XTime *value)
{
    *value = now;
}

void host_capture_sync(void)
{
    if (race_claim) {
        volatile uint32_t *published = (volatile uint32_t *)(uintptr_t)(PCT_DDR_BASE + 0xC0000U);
        *published += 4U;
        race_claim = false;
    }
}

uint32_t Xil_In32(uintptr_t address)
{
    mmio_reads++;
    switch (address) {
    case PCT_STATUS: return status;
    case PCT_CGA_STATE: return mode_state;
    case PCT_CGA_GEOMETRY: return geometry;
    case PCT_CGA_CRTC_LOW: return crtc_low;
    case PCT_CGA_RASTER:
        beam_reads++;
        if (beam_reads == 2U) {
            if (change_beam_during_copy) {
                return beam | (1U << 14);
            }
            if (change_mode_during_copy) {
                mode_state += 0x10000U;
            }
        }
        return beam;
    default:
        assert(!"unexpected capture MMIO address");
        return 0U;
    }
}

static void set_line(unsigned frame, unsigned line)
{
    now += ((uint64_t)(COUNTS_PER_SECOND) * 70U) / 1000000U;
    beam = (frame << 16) | ((line / 2U) << 5) | (line & 1U);
    beam_reads = 0U;
}

static void poll_line(unsigned frame, unsigned line)
{
    set_line(frame, line);
    pc_transporter_cga_capture_poll();
}

static uint8_t pattern(unsigned address, unsigned seed)
{
    return (uint8_t)(address ^ (address >> 5) ^ (address >> 8) ^ (seed * 7U));
}

static void seed_vram(unsigned seed)
{
    for (unsigned offset = 0; offset < VRAM_BYTES; offset++) {
        vram[offset] = pattern(offset, seed);
    }
}

static void check_pixels(const volatile uint8_t *frame, unsigned seed)
{
    assert(frame != NULL);
    for (unsigned y = 0; y < HEIGHT; y++) {
        for (unsigned x = 0; x < ROW_BYTES; x++) {
            const unsigned address = (y & 1U) * BANK_BYTES +
                ((START_BYTE + (y / 2U) * ROW_BYTES + x) & (BANK_BYTES - 1U));
            assert(frame[y * ROW_BYTES + x] == pattern(address, seed));
        }
    }
}

static void check_stats(uint32_t expected_completed, uint32_t expected_dropped)
{
    uint32_t completed;
    uint32_t dropped;
    pc_transporter_cga_capture_get_stats(&completed, &dropped);
    assert(completed == expected_completed && dropped == expected_dropped);
}

int main(void)
{
    void *base = (void *)(uintptr_t)PCT_DDR_BASE;
#ifdef _WIN32
    assert(VirtualAlloc(base, 0x100000U, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) == base);
#else
    assert(mmap(base, 0x100000U, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == base);
#endif
    pc_transporter_cga_capture_reset();
    pc_transporter_cga_capture_init();
    pc_transporter_cga_capture_poll();
    assert(mmio_reads == 1U);
    for (unsigned i = 0; i < 5U; i++) {
        poll_line(0U, i);
    }
    assert(mmio_reads == 1U);

    status = PCT_ENABLED;
    now += (uint64_t)(COUNTS_PER_SECOND) / 1000U;
    seed_vram(1U);
    for (unsigned line = 0; line < HEIGHT; line++) {
        poll_line(1U, line);
        if (line == 0U) {
            /* A second active sample cannot replace the captured row. */
            vram[START_BYTE] ^= 0xFFU;
            poll_line(1U, 0U);
            assert(beam_reads == 1U);
            vram[START_BYTE] ^= 0xFFU;
        }
    }
    check_stats(1U, 0U);
    assert(pc_transporter_cga_capture_claim(MODE_STATE, GEOMETRY ^ 1U, CRTC_LOW) == NULL);
    race_claim = true;
    assert(pc_transporter_cga_capture_claim(MODE_STATE, GEOMETRY, CRTC_LOW) == NULL);
    const volatile uint8_t *held = pc_transporter_cga_capture_claim(MODE_STATE, GEOMETRY, CRTC_LOW);
    check_pixels(held, 1U);

    for (unsigned frame = 2U; frame <= 4U; frame++) {
        seed_vram(frame);
        for (unsigned line = 0; line < HEIGHT; line++) {
            poll_line(frame, line);
        }
        check_pixels(held, 1U);
    }
    pc_transporter_cga_capture_release();
    const volatile uint8_t *latest = pc_transporter_cga_capture_claim(MODE_STATE, GEOMETRY, CRTC_LOW);
    assert(latest != held);
    check_pixels(latest, 4U);
    pc_transporter_cga_capture_release();

    poll_line(5U, 1U);
    poll_line(5U, 2U);
    poll_line(6U, 0U);
    poll_line(6U, 2U);
    change_beam_during_copy = true;
    poll_line(7U, 0U);
    change_beam_during_copy = false;
    poll_line(7U, 1U);
    check_stats(4U, 3U);
    latest = pc_transporter_cga_capture_claim(MODE_STATE, GEOMETRY, CRTC_LOW);
    check_pixels(latest, 4U);
    pc_transporter_cga_capture_release();

    for (unsigned line = 0; line + 1U < HEIGHT; line++) {
        poll_line(8U, line);
    }
    change_mode_during_copy = true;
    poll_line(8U, HEIGHT - 1U);
    check_stats(4U, 4U);
    assert(pc_transporter_cga_capture_claim(MODE_STATE, GEOMETRY, CRTC_LOW) == NULL);

    puts("PC Transporter CGA capture passed");
    return 0;
}
