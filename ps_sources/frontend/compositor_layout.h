/*
 * compositor_layout.h -- DDR memory map and triple-buffer layout for the
 * PS-driven compositor pipeline.
 *
 * Two independent triple buffers:
 *
 *   1. Output framebuffer ring (PS -> PL)
 *      Three slots of 1920x1080 RGB565 (4 MB each, contiguous). PS
 *      composites one frame per DVI vblank into a slot, flushes, then
 *      publishes by writing FB_BASE_ADDR_REG. PL fb_reader latches the
 *      new base at the next vblank-start and reports back via
 *      FB_LAST_LATCHED_REG.
 *
 *   2. Apple framebuffer ring (renderer -> compositor)
 *      Three 1 MB slots. Apple II frames use a 616x224 border ring
 *      around the 560x192 active region; VidHD SHR frames use
 *      640x400 BGRA32. The apple_cycle_renderer publishes whole frames
 *      with a mode tag so the compositor can choose the matching blit.
 *
 * RGB565 is the designed output depth of the whole video path: the DVI
 * pins are 5:6:5, fb_reader streams 565 pixels straight onto them, and
 * every compose-time store is 16-bit. The Apple frame ring stays BGRA32
 * (the cycle renderer's chroma pipeline is 8:8:8-native); the fb16 2x
 * blits narrow at the final store, so precision drops exactly once, at
 * the same width the wire carries. Halving the output ring's pixel size
 * roughly halves the scan-out reads, the compose writes, and the bezel
 * reads on a DDR bus shared with the egress, Disk II bridge, and SDD.
 *
 * Address map (PS DDR):
 *
 *   0x3E000000  Output FB slot 0   (4 MB)  ┐
 *   0x3E400000  Output FB slot 1   (4 MB)  │ contiguous trio
 *   0x3E800000  Output FB slot 2   (4 MB)  ┘
 *   0x3EC00000..0x3EFFFFFF  reserved/free
 *
 *   0x3F000000  egress producer pointer + status regs
 *   0x3F010000  egress ring (64 KB)
 *   0x3F020000  video character-ROM override buffer    (4 KB, NONCACHE)
 *   0x3F021000..0x3F0FFFFF  free
 *   0x3F100000  apple_cycle_egress shadow main bank    (64 KB)
 *   0x3F110000  apple_cycle_egress shadow aux bank     (64 KB)
 *   0x3F120000..0x3F2FFFFF  free
 *   0x3F300000  Apple FB slot 0    (1 MB)              ┐
 *   0x3F400000  Apple FB slot 1    (1 MB)              │ contiguous trio
 *   0x3F500000  Apple FB slot 2    (1 MB)              ┘
 *   0x3F600000..0x3FFFFFFF  free
 */

#ifndef COMPOSITOR_LAYOUT_H
#define COMPOSITOR_LAYOUT_H

#include <stdint.h>

/* ---------- Output framebuffer ring (PS -> PL) ------------------------- */

#define COMP_OUT_WIDTH         1920u
#define COMP_OUT_HEIGHT        1080u
#define COMP_OUT_BPP           2u    /* RGB565 */
#define COMP_OUT_STRIDE_BYTES  (COMP_OUT_WIDTH * COMP_OUT_BPP)         /* 3840 */
#define COMP_OUT_BYTES         (COMP_OUT_HEIGHT * COMP_OUT_STRIDE_BYTES) /* 4,147,200 */

#define COMP_OUT_SLOT_COUNT    3u

/* Slot bases. Filled by compositor_layout.c so the table sits in .rodata
 * exactly once and slot lookups go through a single source of truth. */
extern const uint32_t comp_out_slot_addr[COMP_OUT_SLOT_COUNT];

/* Reverse lookup: address -> slot index, or 0xFF if no match. The PL
 * surfaces the last-latched base address as a uint32; PS uses this to
 * map it back to a slot index for the safe-slot picker. */
uint8_t comp_out_addr_to_slot(uint32_t addr);

/* ---------- Apple framebuffer ring (renderer -> compositor) ------------ */

/* Apple active picture and fixed IIgs-style border ring. */
#define COMP_APPLE_WIDTH               560u
#define COMP_APPLE_HEIGHT              192u
#define COMP_APPLE_BORDER_H_CYCLES     2u
#define COMP_APPLE_BORDER_H_PIXELS     (COMP_APPLE_BORDER_H_CYCLES * 14u)
#define COMP_APPLE_BORDER_V_LINES      16u
#define COMP_APPLE_VISIBLE_WIDTH       (COMP_APPLE_WIDTH + \
                                        (2u * COMP_APPLE_BORDER_H_PIXELS))
#define COMP_APPLE_VISIBLE_HEIGHT      (COMP_APPLE_HEIGHT + \
                                        (2u * COMP_APPLE_BORDER_V_LINES))
#define COMP_APPLE_LEFT_BORDER_PIXELS  4u
#define COMP_APPLE_RIGHT_BORDER_PIXELS 4u
#define COMP_APPLE_BORDER_PIXELS       COMP_APPLE_LEFT_BORDER_PIXELS
#define COMP_APPLE_ROW_PIXELS          (COMP_APPLE_LEFT_BORDER_PIXELS + \
                                        COMP_APPLE_VISIBLE_WIDTH + \
                                        COMP_APPLE_RIGHT_BORDER_PIXELS)
#define COMP_APPLE_ACTIVE_X            (COMP_APPLE_LEFT_BORDER_PIXELS + \
                                        COMP_APPLE_BORDER_H_PIXELS)
#define COMP_APPLE_ACTIVE_Y            COMP_APPLE_BORDER_V_LINES
#define COMP_APPLE_BPP                 4u    /* BGRA32 */
#define COMP_APPLE_STRIDE_BYTES        (COMP_APPLE_ROW_PIXELS * COMP_APPLE_BPP)
#define COMP_APPLE_LEGACY_BYTES        (COMP_APPLE_VISIBLE_HEIGHT * COMP_APPLE_STRIDE_BYTES)

#define COMP_APPLE_SHR_WIDTH        640u
#define COMP_APPLE_SHR_HEIGHT       400u
#define COMP_APPLE_SHR_ROW_PIXELS   COMP_APPLE_SHR_WIDTH
#define COMP_APPLE_SHR_STRIDE_BYTES (COMP_APPLE_SHR_ROW_PIXELS * COMP_APPLE_BPP)
#define COMP_APPLE_SHR_BYTES        (COMP_APPLE_SHR_HEIGHT * COMP_APPLE_SHR_STRIDE_BYTES)

#define COMP_APPLE_SLOT_BYTES       0x00100000u
#define COMP_APPLE_BYTES            COMP_APPLE_SLOT_BYTES

#define COMP_APPLE_SLOT_COUNT   3u

extern const uint32_t comp_apple_slot_addr[COMP_APPLE_SLOT_COUNT];

/* ---------- Video character-ROM override buffer (CPU0 -> CPU1) --------- *
 *
 * CPU0 reads a user-selected //e video character ROM from the SD card,
 * validates it (size 4096/8192, not all 00/FF), and copies the primary 4 KB
 * bank here. CPU1's renderer rebuilds csbits from this buffer when the
 * handoff generation word changes (see apple_fb_handoff video-ROM gen);
 * generation 0 means "no override -> baked Enhanced US ROM".
 *
 * Lives in the 0x3F000000 NONCACHE DDR section so both cores see it
 * coherently with no cache maintenance. Keep this in sync with the address
 * map comment at the top of this file. */
#define APPLE_VIDEO_ROM_OVERRIDE_ADDR   0x3F020000u
#define APPLE_VIDEO_ROM_OVERRIDE_BYTES  4096u

/* ---------- Apple subwindow geometry inside the output frame ----------- *
 *
 * Fixed geometry. The Apple FB is 560x192; the subwindow is 1120x768
 * (2x horizontal, 4x vertical replication) centered horizontally at y=156.
 * Keep all related offsets and dimensions together in this section.
 */

#define COMP_SUBWIN_X_OFF    400u
#define COMP_SUBWIN_Y_OFF    156u
#define COMP_SUBWIN_WIDTH    1120u
#define COMP_SUBWIN_HEIGHT   768u

#define COMP_BORDER_X_OFF    344u
#define COMP_BORDER_Y_OFF    92u
#define COMP_BORDER_WIDTH    1232u
#define COMP_BORDER_HEIGHT   896u

#define COMP_SUBWIN_SHR_X_OFF    320u
#define COMP_SUBWIN_SHR_Y_OFF    140u
#define COMP_SUBWIN_SHR_WIDTH    1280u
#define COMP_SUBWIN_SHR_HEIGHT   800u

#endif /* COMPOSITOR_LAYOUT_H */
