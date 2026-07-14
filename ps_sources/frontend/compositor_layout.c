/*
 * compositor_layout.c -- Concrete slot tables for compositor_layout.h.
 *
 * Slot bases are centralized here so forward and reverse mappings share one
 * table. comp_out_addr_to_slot() decodes FB_LAST_LATCHED_REG from the PL.
 */

#include "compositor_layout.h"

const uint32_t comp_out_slot_addr[COMP_OUT_SLOT_COUNT] = {
    /* 1920x1080 RGB565 = 4,147,200 bytes; 4 MB spacing keeps each slot
     * MMU-section aligned and the whole ring contiguous inside
     * 0x3E000000..0x3EBFFFFF. */
    0x3E000000u,   /* slot 0: 4 MB */
    0x3E400000u,   /* slot 1: 4 MB */
    0x3E800000u    /* slot 2: 4 MB */
};

const uint32_t comp_apple_slot_addr[COMP_APPLE_SLOT_COUNT] = {
    /* 0x3F300000-0x3F5FFFFF avoids the egress shadow banks at
     * 0x3F100000/0x3F110000 and provides three 1 MB slots for VidHD SHR. */
    0x3F300000u,   /* slot 0: 1 MB */
    0x3F400000u,   /* slot 1: 1 MB */
    0x3F500000u    /* slot 2: 1 MB */
};

uint8_t comp_out_addr_to_slot(uint32_t addr)
{
    for (uint8_t i = 0u; i < COMP_OUT_SLOT_COUNT; ++i) {
        if (comp_out_slot_addr[i] == addr) {
            return i;
        }
    }
    return 0xFFu;
}
