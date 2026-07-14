/*
 * apple_fb_handoff.c -- See apple_fb_handoff.h.
 *
 * Cross-core 3-slot framebuffer handoff between CPU1 (writer, runs
 * the renderer) and CPU0 (reader, runs the compositor) under AMP.
 *
 * Zynq OCM is not snooped by the SCU like DDR, so the handoff must not
 * depend on cross-core exclusive operations. State is split into one
 * writer-owned half and one reader-owned half. Volatile loads/stores with
 * `dmb sy` provide ordering without cross-core compare-exchange.
 *
 *   writer_state (CPU1 writes, CPU0 reads):
 *     uint32_t published_slot;   // slot, display mode, and border color
 *     uint32_t publish_seq;      // monotonic, increments on each publish
 *
 *   reader_state (CPU0 writes, CPU1 reads):
 *     uint32_t reader_active;    // slot the compositor is currently
 *                                // displaying (or 0xFF before first claim)
 *
 * Writer logic on each on_frame_end:
 *   1. publish_seq++ (sequence is updated AFTER pixel writes via dmb).
 *   2. published_slot = the slot we just painted.
 *   3. Pick the NEXT writer slot: any of {0,1,2} that's neither
 *      `published_slot` (just published; reader will likely claim it)
 *      nor `reader_active` (the reader's current display slot, if any).
 *      With 3 slots and at most 2 to avoid, exactly one is free.
 *
 * Reader logic on each compositor tick:
 *   1. Snapshot publish_seq. If unchanged since last claim, return
 *      the same slot we displayed last time (frame-hold under
 *      reader-faster-than-writer).
 *   2. Otherwise: read published_slot, set reader_active = that slot,
 *      remember publish_seq as last_claimed_seq. Return the slot.
 *
 * Memory layout (high OCM, marked strongly ordered on both cores
 * by the handoff init functions so reads/writes hit a single coherent
 * non-cacheable region):
 *
 *   0xFFFF1000  published_slot   (uint32_t)
 *   0xFFFF1004  publish_seq      (uint32_t)
 *   0xFFFF1008  reader_active    (uint32_t)
 *   0xFFFF100C  video_settings   (uint32_t)
 *   0xFFFF1010  display_mode     (uint32_t)
 *   0xFFFF1014  video_rom_gen    (uint32_t)  CPU0 writes, CPU1 reads
 */

#include <stdint.h>

#include "xil_cache.h"
#include "xil_mmu.h"

#include "apple_fb_handoff.h"

/* High-OCM addresses for the shared state. Both cores point at
 * the same physical memory; the handoff init functions mark the
 * containing 1 MB section strongly ordered on each core's MMU table. */
#define HANDOFF_PUBLISHED_SLOT_ADDR  0xFFFF1000U
#define HANDOFF_PUBLISH_SEQ_ADDR     0xFFFF1004U
#define HANDOFF_READER_ACTIVE_ADDR   0xFFFF1008U
#define HANDOFF_VIDEO_SETTINGS_ADDR  0xFFFF100CU
#define HANDOFF_DISPLAY_MODE_ADDR   0xFFFF1010U
/* Video character-ROM override generation. CPU0 bumps it after copying a
 * validated ROM into the APPLE_VIDEO_ROM_OVERRIDE_ADDR buffer (or sets 0 for
 * "no override"); CPU1 rebuilds csbits when it sees the value change. */
#define HANDOFF_VIDEO_ROM_GEN_ADDR   0xFFFF1014U

#define READER_NONE    0xFFu
#define PUBLISHED_SLOT_MASK          0x000000FFu
#define PUBLISHED_DISPLAY_MODE_SHIFT 8u
#define PUBLISHED_DISPLAY_MODE_MASK  (1u << PUBLISHED_DISPLAY_MODE_SHIFT)
#define PUBLISHED_BORDER_COLOR_SHIFT 9u
#define PUBLISHED_BORDER_COLOR_MASK  (0xFu << PUBLISHED_BORDER_COLOR_SHIFT)

static volatile uint32_t *const s_published_slot =
    (volatile uint32_t *)HANDOFF_PUBLISHED_SLOT_ADDR;
static volatile uint32_t *const s_publish_seq =
    (volatile uint32_t *)HANDOFF_PUBLISH_SEQ_ADDR;
static volatile uint32_t *const s_reader_active =
    (volatile uint32_t *)HANDOFF_READER_ACTIVE_ADDR;
static volatile uint32_t *const s_video_settings =
    (volatile uint32_t *)HANDOFF_VIDEO_SETTINGS_ADDR;
static volatile uint32_t *const s_display_mode =
    (volatile uint32_t *)HANDOFF_DISPLAY_MODE_ADDR;
static volatile uint32_t *const s_video_rom_gen =
    (volatile uint32_t *)HANDOFF_VIDEO_ROM_GEN_ADDR;

/* Per-core local state. Each core has its own copy in its own .bss
 * (CPU0 in lower DDR, CPU1 in upper DDR -- the per-app static).
 * These don't need to be coherent. */
static uint32_t s_writer_current_slot = 0u;   /* CPU1 only */
static uint32_t s_reader_last_seq     = 0u;   /* CPU0 only */
static uint8_t  s_reader_current_slot = APPLE_FB_NO_SLOT; /* CPU0 only */
static uint32_t s_reader_display_mode = APPLE_FB_DISPLAY_MODE_LEGACY; /* CPU0 only */
static uint8_t  s_reader_border_color = APPLE_VIDEO_IIGS_BORDER_DEFAULT; /* CPU0 only */

static inline uint32_t handoff_normalize_display_mode(uint32_t display_mode)
{
    return (display_mode == APPLE_FB_DISPLAY_MODE_SHR) ?
        APPLE_FB_DISPLAY_MODE_SHR : APPLE_FB_DISPLAY_MODE_LEGACY;
}

static inline uint32_t handoff_pack_published(uint32_t slot,
                                              uint32_t display_mode,
                                              uint8_t border_color)
{
    const uint32_t mode = handoff_normalize_display_mode(display_mode);
    return (slot & PUBLISHED_SLOT_MASK) |
           (mode << PUBLISHED_DISPLAY_MODE_SHIFT) |
           ((uint32_t)apple_video_iigs_border_color_clamp(border_color) <<
            PUBLISHED_BORDER_COLOR_SHIFT);
}

static inline uint32_t handoff_published_slot(uint32_t published)
{
    return published & PUBLISHED_SLOT_MASK;
}

static inline uint32_t handoff_published_mode(uint32_t published)
{
    return ((published & PUBLISHED_DISPLAY_MODE_MASK) != 0u) ?
        APPLE_FB_DISPLAY_MODE_SHR : APPLE_FB_DISPLAY_MODE_LEGACY;
}

static inline uint8_t handoff_published_border_color(uint32_t published)
{
    return (uint8_t)((published & PUBLISHED_BORDER_COLOR_MASK) >>
                     PUBLISHED_BORDER_COLOR_SHIFT);
}

/* Pick the slot that is neither `a` nor `b`. With 3 slots and
 * `a != b`, exactly one of {0,1,2} is the answer. */
static inline uint32_t pick_third(uint32_t a, uint32_t b)
{
    /* If b is sentinel (READER_NONE = 0xFF), it's not a real slot
     * to avoid; just pick any slot != a. */
    if (b >= 3u) {
        return (a == 0u) ? 1u : 0u;
    }
    if (a == b) {
        /* Defensive: shouldn't happen, but pick something sane. */
        return (a == 0u) ? 1u : 0u;
    }
    return 3u - a - b;
}

static void handoff_map_shared_ocm(void)
{
    /* Map the high-OCM 1 MB section STRONG_ORDERED (0xC02). The BSP's
     * inner-cacheable, non-shareable mapping permits each core to retain a
     * different L1 value. Strong ordering is appropriate because these words
     * are touched only once per frame. */
    Xil_SetTlbAttributes(0xFFF00000U, STRONG_ORDERED);

    /* Discard L1 lines created before the strongly ordered mapping so no
     * cached value can shadow the shared words. */
    Xil_DCacheInvalidateRange(0xFFF00000U, 0x100000U);
}

void apple_fb_handoff_init(void)
{
    handoff_map_shared_ocm();

    /* Initial state. Writer starts at slot 0 (will paint into it
     * first). No frame published yet -- publish_seq=0 signals
     * to the reader that nothing is ready. reader_active is sentinel
     * READER_NONE so the writer's pick_third treats it as "no
     * constraint". */
    *s_published_slot = handoff_pack_published(
        0u, APPLE_FB_DISPLAY_MODE_LEGACY, APPLE_VIDEO_IIGS_BORDER_DEFAULT);
    *s_publish_seq    = 0u;
    *s_reader_active  = READER_NONE;
    *s_video_settings = APPLE_VIDEO_SETTINGS_DEFAULT;
    *s_display_mode   = APPLE_FB_DISPLAY_MODE_LEGACY;
    *s_video_rom_gen  = 0u;   /* 0 = no override -> baked Enhanced US ROM */
    __asm__ volatile ("dsb sy" ::: "memory");

    s_writer_current_slot = 0u;
    s_reader_last_seq     = 0u;
    s_reader_current_slot = APPLE_FB_NO_SLOT;
    s_reader_display_mode = APPLE_FB_DISPLAY_MODE_LEGACY;
    s_reader_border_color = APPLE_VIDEO_IIGS_BORDER_DEFAULT;
}

void apple_fb_handoff_secondary_init(void)
{
    handoff_map_shared_ocm();
    __asm__ volatile ("dsb sy" ::: "memory");

    s_writer_current_slot = 0u;
    s_reader_last_seq     = 0u;
    s_reader_current_slot = APPLE_FB_NO_SLOT;
    s_reader_display_mode = APPLE_FB_DISPLAY_MODE_LEGACY;
    s_reader_border_color = APPLE_VIDEO_IIGS_BORDER_DEFAULT;
}

uint32_t apple_fb_handoff_state(void)
{
    /* Pack the diagnostic fields into the UART status word. Bits:
     *   [1:0]   reader_active (0..2 or 3=sentinel)
     *   [3:2]   published_slot (writer's last-published)
     *   [5:4]   writer_current (writer's *next* paint target -- not
     *           in shared state, so we leave 0)
     *   [6]     "frame ready" (1 if publish_seq != 0)
     *   [7]     "first publish done" (1 if publish_seq != 0)
     */
    uint32_t pub = handoff_published_slot(*s_published_slot);
    uint32_t ra  = *s_reader_active;
    uint32_t seq = *s_publish_seq;
    uint32_t out = (pub & 0x3u) << 2;
    out |= ((ra >= 3u ? 0x3u : ra) & 0x3u);
    if (seq != 0u) out |= (1u << 6) | (1u << 7);
    return out;
}

void apple_fb_video_settings_set(uint32_t settings)
{
    *s_video_settings = apple_video_settings_normalize(settings);
    __asm__ volatile ("dmb sy" ::: "memory");
}

uint32_t apple_fb_video_settings_get(void)
{
    const uint32_t settings = *s_video_settings;
    __asm__ volatile ("dmb sy" ::: "memory");
    return settings;
}

void apple_fb_video_rom_gen_set(uint32_t gen)
{
    *s_video_rom_gen = gen;
    __asm__ volatile ("dmb sy" ::: "memory");
}

uint32_t apple_fb_video_rom_gen_get(void)
{
    const uint32_t gen = *s_video_rom_gen;
    __asm__ volatile ("dmb sy" ::: "memory");
    return gen;
}

uint8_t apple_fb_writer_slot(void)
{
    return (uint8_t)s_writer_current_slot;
}

void apple_fb_writer_publish(void)
{
    apple_fb_writer_publish_mode(APPLE_FB_DISPLAY_MODE_LEGACY);
}

void apple_fb_writer_publish_mode(uint32_t display_mode)
{
    apple_fb_writer_publish_frame(display_mode, APPLE_VIDEO_IIGS_BORDER_DEFAULT);
}

void apple_fb_writer_publish_frame(uint32_t display_mode, uint8_t border_color)
{
    const uint32_t normalized_mode = handoff_normalize_display_mode(display_mode);

    /* Make the slot's pixel writes visible system-wide before
     * publishing the index. on_frame_end already did dsb sy after
     * its pixel emits; this redundant barrier is cheap. */
    __asm__ volatile ("dsb sy" ::: "memory");

    *s_display_mode = normalized_mode;
    __asm__ volatile ("dmb sy" ::: "memory");

    /* Publish: write the slot index, then bump the sequence number.
     * Order matters: a reader that sees seq advance MUST see the
     * matching published slot and mode value. dmb between them. */
    *s_published_slot =
        handoff_pack_published(s_writer_current_slot, normalized_mode, border_color);
    __asm__ volatile ("dmb sy" ::: "memory");
    *s_publish_seq    = *s_publish_seq + 1u;
    __asm__ volatile ("dsb sy" ::: "memory");

    /* Pick the next writer slot: anything that is neither the slot
     * we just published nor the slot the reader is currently
     * displaying. Reader-active is updated by the reader and may
     * be one frame stale here -- the worst case is we pick a slot
     * the reader has just claimed, which causes one frame of
     * tearing on the reader's blit before it picks up the next
     * publish. Acceptable. */
    uint32_t reader_active = *s_reader_active;
    s_writer_current_slot = pick_third(s_writer_current_slot, reader_active);
}

uint8_t apple_fb_reader_claim(void)
{
    uint32_t seq = *s_publish_seq;

    /* No frame published yet -- caller should skip the blit. */
    if (seq == 0u) {
        return APPLE_FB_NO_SLOT;
    }

    /* If sequence has not advanced since our last claim, return the
     * same slot. Caller re-blits stale-but-stable last frame. */
    if (seq == s_reader_last_seq && s_reader_current_slot != APPLE_FB_NO_SLOT) {
        return s_reader_current_slot;
    }

    /* New frame available. Snapshot the slot and ack the sequence. */
    uint32_t published = *s_published_slot;
    uint32_t slot = handoff_published_slot(published);
    if (slot >= 3u) {
        /* Defensive: garbage from uninitialized memory. Skip. */
        return APPLE_FB_NO_SLOT;
    }

    s_reader_last_seq     = seq;
    s_reader_current_slot = (uint8_t)slot;
    s_reader_display_mode = handoff_published_mode(published);
    s_reader_border_color = handoff_published_border_color(published);

    /* Tell the writer which slot the reader is now displaying so
     * it can avoid stomping it on the next paint. */
    *s_reader_active = slot;
    __asm__ volatile ("dmb sy" ::: "memory");

    return s_reader_current_slot;
}

uint32_t apple_fb_reader_display_mode(void)
{
    return s_reader_display_mode;
}

uint8_t apple_fb_reader_border_color(void)
{
    return s_reader_border_color;
}

uint32_t apple_fb_reader_publish_seq(void)
{
    const uint32_t seq = *s_publish_seq;
    __asm__ volatile ("dmb sy" ::: "memory");
    return seq;
}

uint32_t apple_fb_reader_published_display_mode(void)
{
    const uint32_t seq = *s_publish_seq;
    __asm__ volatile ("dmb sy" ::: "memory");
    if (seq == 0u) {
        return APPLE_FB_DISPLAY_MODE_LEGACY;
    }

    const uint32_t published = *s_published_slot;
    __asm__ volatile ("dmb sy" ::: "memory");
    return handoff_published_mode(published);
}
