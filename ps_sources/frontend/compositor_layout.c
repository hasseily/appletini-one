/*
 * compositor_layout.c -- Concrete slot tables for compositor_layout.h.
 *
 * Slot bases are centralized here so forward and reverse mappings share one
 * table. comp_out_addr_to_slot() decodes FB_LAST_LATCHED_REG from the PL.
 */

#include "compositor_layout.h"
#include "display_modes.h"

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

/* Only CPU0 changes output geometry. Apple source dimensions stay fixed. */
uint16_t comp_out_width = COMP_OUT_MAX_WIDTH;
uint16_t comp_out_height = COMP_OUT_MAX_HEIGHT;
comp_viewport_t comp_legacy_viewport = {400, 156, 1120, 768, 344, 92, 1232, 896, 2};
comp_viewport_t comp_shr_viewport = {320, 140, 1280, 800, 264, 76, 1392, 928, 2};
static uint8_t s_size_multiplier = 0U;

static void viewport_fit(comp_viewport_t *view, int output_w, int output_h,
                          int picture_w, int picture_h)
{
    const int fit_x = output_w / picture_w;
    const int fit_y = output_h / picture_h;
    const int max_scale = fit_x < fit_y ? fit_x : fit_y;
    const int scale = (s_size_multiplier != 0U && s_size_multiplier < max_scale)
        ? s_size_multiplier : max_scale;
    const int border_h = (int)COMP_APPLE_BORDER_H_PIXELS * scale;
    const int border_v = (int)COMP_APPLE_BORDER_V_LINES * 2 * scale;
    view->scale = (uint8_t)scale;
    view->width = picture_w * scale;
    view->height = picture_h * scale;
    view->x = (output_w - view->width) / 2;
    view->y = (output_h - view->height) / 2;
    view->border_x = view->x - border_h;
    view->border_y = view->y - border_v;
    view->border_width = view->width + 2 * border_h;
    view->border_height = view->height + 2 * border_v;
}

int comp_layout_set_output_size(uint16_t width, uint16_t height)
{
    if (width < COMP_APPLE_SHR_WIDTH || height < COMP_APPLE_SHR_HEIGHT ||
        width > COMP_OUT_MAX_WIDTH || height > COMP_OUT_MAX_HEIGHT) return 0;
    comp_out_width = width;
    comp_out_height = height;
    viewport_fit(&comp_legacy_viewport, width, height,
                   COMP_APPLE_WIDTH, COMP_APPLE_HEIGHT * 2);
    viewport_fit(&comp_shr_viewport, width, height,
                   COMP_APPLE_SHR_WIDTH, COMP_APPLE_SHR_HEIGHT);
    return 1;
}

void comp_layout_set_size_multiplier(uint8_t multiplier)
{
    s_size_multiplier = display_size_multiplier_clamp(multiplier);
    (void)comp_layout_set_output_size(comp_out_width, comp_out_height);
}

uint8_t comp_layout_size_multiplier(void)
{
    return s_size_multiplier;
}
