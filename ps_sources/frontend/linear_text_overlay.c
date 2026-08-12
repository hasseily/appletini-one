#include "linear_text_overlay.h"

#include <stddef.h>
#include <stdint.h>

#include "xiltimer.h"

#include "../lib/common.h"
#include "compositor_layout.h"
#include "linear_text_overlay_font.h"

#define RGB565_CONST(r, g, b) \
    (uint16_t)((((uint16_t)(r) >> 3) << 11) | \
               (((uint16_t)(g) >> 2) << 5) | \
               ((uint16_t)(b) >> 3))

static const uint16_t s_vga_palette[16] = {
    RGB565_CONST(0x00, 0x00, 0x00),
    RGB565_CONST(0x00, 0x00, 0xAA),
    RGB565_CONST(0x00, 0xAA, 0x00),
    RGB565_CONST(0x00, 0xAA, 0xAA),
    RGB565_CONST(0xAA, 0x00, 0x00),
    RGB565_CONST(0xAA, 0x00, 0xAA),
    RGB565_CONST(0xAA, 0x55, 0x00),
    RGB565_CONST(0xAA, 0xAA, 0xAA),
    RGB565_CONST(0x55, 0x55, 0x55),
    RGB565_CONST(0x55, 0x55, 0xFF),
    RGB565_CONST(0x55, 0xFF, 0x55),
    RGB565_CONST(0x55, 0xFF, 0xFF),
    RGB565_CONST(0xFF, 0x55, 0x55),
    RGB565_CONST(0xFF, 0x55, 0xFF),
    RGB565_CONST(0xFF, 0xFF, 0x55),
    RGB565_CONST(0xFF, 0xFF, 0xFF)
};

static linear_text_overlay_config_t s_active;
static uint8_t s_visible;
static uint8_t s_frame_ack_pending;

static const volatile uint8_t *shadow_slot(uint8_t slot)
{
    const uintptr_t addr = (slot != 0U) ? LTO_SHADOW_SLOT1_ADDR
                                       : LTO_SHADOW_SLOT0_ADDR;
    return (const volatile uint8_t *)addr;
}

static void read_armed_config(linear_text_overlay_config_t *cfg,
                              uint32_t state)
{
    const uint32_t arm0 = REG_READ(LTO_R_ARM0);
    const uint32_t arm1 = REG_READ(LTO_R_ARM1);
    const uint32_t arm2 = REG_READ(LTO_R_ARM2);

    cfg->base = (uint16_t)(arm0 & 0xFFFFU);
    cfg->config = (uint8_t)((arm0 >> 16) & 0xFFU);
    cfg->cols = (uint8_t)(arm0 >> 24);
    cfg->rows = (uint8_t)(arm1 & 0xFFU);
    cfg->origin_x = (uint16_t)((arm1 >> 8) & 0xFFFFU);
    cfg->origin_y = (uint16_t)(((arm2 & 0xFFU) << 8) |
                              ((arm1 >> 24) & 0xFFU));
    cfg->scale = (uint8_t)((arm2 >> 8) & 0xFFU);
    cfg->fill_char = (uint8_t)((arm2 >> 16) & 0xFFU);
    cfg->fill_attr = (uint8_t)(arm2 >> 24);
    cfg->slot = ((state & LTO_STATE_ARMED_SLOT) != 0U) ? 1U : 0U;
}

static uint8_t blink_phase_on(void)
{
    XTime now;
    const XTime half_second = (XTime)COUNTS_PER_SECOND / 2U;

    XTime_GetTime(&now);
    if (half_second == 0U) {
        return 1U;
    }
    return (((now / half_second) & 1U) == 0U) ? 1U : 0U;
}

static void process_frame_request(void)
{
    const uint32_t state = REG_READ(LTO_R_STATE);
    uint32_t command;

    if (s_frame_ack_pending != 0U) {
        return;
    }
    if ((state & LTO_STATE_FRAME_REQUEST) == 0U) {
        return;
    }
    command = (state & LTO_STATE_FRAME_CMD_MASK) >>
              LTO_STATE_FRAME_CMD_SHIFT;
    if (command == LTO_FRAME_SHOW) {
        if ((state & LTO_STATE_SHOW_DRAINED) == 0U) {
            return;
        }
        read_armed_config(&s_active, state);
        s_visible = 1U;
    } else {
        s_visible = 0U;
    }
    s_frame_ack_pending = 1U;
}

void linear_text_overlay_init(void)
{
    s_visible = 0U;
    s_frame_ack_pending = 0U;
}

uint8_t linear_text_overlay_poll_frame_latch(uint8_t prior_publish_latched)
{
    uint32_t state;
    uint8_t result = 0U;

    if (s_frame_ack_pending != 0U && prior_publish_latched != 0U) {
        __asm__ volatile ("dsb sy" ::: "memory");
        REG_WRITE(LTO_R_CONTROL, LTO_CTL_ACK_FRAME);
        s_frame_ack_pending = 0U;
    }

    state = REG_READ(LTO_R_STATE);
    /* Apple reset clears the PL state at once. Drop any CPU-local frame
     * state as soon as no frame command remains to carry it to scanout. */
    if ((state & (LTO_STATE_VISIBLE | LTO_STATE_FRAME_REQUEST)) == 0U) {
        s_visible = 0U;
        s_frame_ack_pending = 0U;
    }
    if ((state & LTO_STATE_FRAME_REQUEST) != 0U &&
        s_frame_ack_pending == 0U) {
        result |= LTO_POLL_REFRESH;
    }
    if (s_frame_ack_pending != 0U) {
        result |= LTO_POLL_WAIT_LATCH;
    }
    return result;
}

static const uint8_t *glyph_for(uint8_t value,
                                uint8_t cp437,
                                uint8_t font_height,
                                uint8_t *underline)
{
    if (cp437 != 0U) {
        *underline = 0U;
        return (font_height == 16U) ?
               linear_text_overlay_cp437_font_8x16[value] :
               linear_text_overlay_cp437_font_8x14[value];
    }

    *underline = (value & 0x80U) != 0U ? 1U : 0U;
    value &= 0x7FU;
    if (value < 0x20U) {
        return (font_height == 16U) ?
               linear_text_overlay_dec_font_8x16[value] :
               linear_text_overlay_dec_font_8x14[value];
    }
    if (value == 0x7FU) {
        value = 0x20U;
    }
    return (font_height == 16U) ?
           linear_text_overlay_cp437_font_8x16[value] :
           linear_text_overlay_cp437_font_8x14[value];
}

void linear_text_overlay_draw(uint16_t *framebuffer, uint8_t shr_canvas)
{
    const volatile uint8_t *cells;
    uint32_t cursor;
    uint8_t cursor_x;
    uint8_t cursor_y;
    uint8_t cursor_ctrl;
    uint8_t blink_on;
    uint8_t scale_x;
    uint8_t scale_y;
    uint8_t font_height;
    uint32_t cell_width;
    uint32_t cell_height;
    uint32_t canvas_x_offset;
    uint32_t canvas_y_offset;
    uint32_t canvas_width;
    uint32_t canvas_height;

    process_frame_request();
    if (framebuffer == NULL || s_visible == 0U) {
        return;
    }

    scale_x = s_active.scale & 0x0FU;
    scale_y = (s_active.scale >> 4) & 0x0FU;
    if (scale_x == 0U || scale_y == 0U) {
        return;
    }
    font_height = ((s_active.config & LTO_CONFIG_FONT_8X16) != 0U) ?
                  16U : 14U;
    cell_width = 8U * scale_x;
    cell_height = (uint32_t)font_height * scale_y;
    if (shr_canvas != 0U) {
        canvas_x_offset = COMP_SUBWIN_SHR_X_OFF;
        canvas_y_offset = COMP_SUBWIN_SHR_Y_OFF;
        canvas_width = COMP_SUBWIN_SHR_WIDTH;
        canvas_height = COMP_SUBWIN_SHR_HEIGHT;
    } else {
        canvas_x_offset = COMP_SUBWIN_X_OFF;
        canvas_y_offset = COMP_SUBWIN_Y_OFF;
        canvas_width = COMP_SUBWIN_WIDTH;
        canvas_height = COMP_SUBWIN_HEIGHT;
    }
    cells = shadow_slot(s_active.slot);
    cursor = REG_READ(LTO_R_CURSOR);
    cursor_x = (uint8_t)(cursor & 0xFFU);
    cursor_y = (uint8_t)((cursor >> 8) & 0xFFU);
    cursor_ctrl = (uint8_t)((cursor >> 16) & 0xFFU);
    blink_on = blink_phase_on();

    for (uint32_t row = 0U; row < s_active.rows; ++row) {
        const uint32_t cell_y = (uint32_t)s_active.origin_y +
                                (row * cell_height);
        if (cell_y >= canvas_height) {
            break;
        }
        for (uint32_t col = 0U; col < s_active.cols; ++col) {
            const uint32_t cell_x = (uint32_t)s_active.origin_x +
                                    (col * cell_width);
            const uint32_t cell_index = 2U *
                (row * (uint32_t)s_active.cols + col);
            const uint8_t ch = cells[cell_index];
            const uint8_t attr = cells[cell_index + 1U];
            uint8_t fg = attr & 0x0FU;
            uint8_t bg;
            uint8_t attr_blink = 0U;
            uint8_t underline;
            uint8_t cursor_here;
            uint8_t cursor_shape;
            const uint8_t *glyph;

            if (cell_x >= canvas_width) {
                break;
            }
            if ((s_active.config & LTO_CONFIG_BLINK_ATTR) != 0U) {
                bg = (attr >> 4) & 0x07U;
                attr_blink = (attr >> 7) & 0x01U;
            } else {
                bg = (attr >> 4) & 0x0FU;
            }

            glyph = glyph_for(ch,
                (s_active.config & LTO_CONFIG_CP437) != 0U,
                font_height,
                &underline);
            cursor_here = ((cursor_ctrl & LTO_CURSOR_ENABLE) != 0U &&
                           cursor_x == col && cursor_y == row &&
                           (((cursor_ctrl & LTO_CURSOR_BLINK) == 0U) ||
                            blink_on != 0U)) ? 1U : 0U;
            cursor_shape = (cursor_ctrl & LTO_CURSOR_SHAPE_MASK) >>
                           LTO_CURSOR_SHAPE_SHIFT;
            if (cursor_here != 0U && cursor_shape == 0U) {
                const uint8_t swap = fg;
                fg = bg;
                bg = swap;
            }

            for (uint32_t gy = 0U; gy < font_height; ++gy) {
                uint8_t bits = glyph[gy];
                const uint32_t py0 = cell_y + gy * scale_y;

                if (underline != 0U && gy == (uint32_t)font_height - 2U) {
                    bits = 0xFFU;
                }
                if (cursor_here != 0U && cursor_shape == 1U &&
                    gy == (uint32_t)font_height - 2U) {
                    bits = 0xFFU;
                }
                if (attr_blink != 0U && blink_on == 0U) {
                    bits = 0U;
                }

                for (uint32_t sy = 0U; sy < scale_y; ++sy) {
                    const uint32_t canvas_y = py0 + sy;
                    uint16_t *dst;
                    if (canvas_y >= canvas_height) {
                        continue;
                    }
                    dst = framebuffer +
                          (canvas_y_offset + canvas_y) *
                              COMP_OUT_WIDTH +
                          canvas_x_offset + cell_x;
                    for (uint32_t gx = 0U; gx < 8U; ++gx) {
                        uint8_t set = (bits & (0x80U >> gx)) != 0U;
                        if (cursor_here != 0U && cursor_shape == 2U &&
                            gx == 0U) {
                            set = 1U;
                        }
                        for (uint32_t sx = 0U; sx < scale_x; ++sx) {
                            const uint32_t canvas_x = cell_x +
                                gx * scale_x + sx;
                            if (canvas_x >= canvas_width) {
                                continue;
                            }
                            if (set != 0U) {
                                dst[gx * scale_x + sx] = s_vga_palette[fg];
                            } else if ((s_active.config &
                                       LTO_CONFIG_TRANSPARENT) == 0U) {
                                dst[gx * scale_x + sx] = s_vga_palette[bg];
                            }
                        }
                    }
                }
            }
        }
    }
}
