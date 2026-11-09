#include "pc_transporter_video.h"

#include <limits.h>
#include <stdbool.h>
#include <string.h>

#include "compositor_layout.h"
#include "linear_text_overlay_font.h"
#include "../lib/fb16.h"

#define CGA_SCANLINES     200U
#define CGA_SCAN_PIXELS   640U
#define CGA_ROW_BYTES      80U
#define CGA_GRAPHICS_RASTERS 2U
#define CGA_TEXT_COLS      80U
#define CGA_GLYPH_ROWS      8U
#define CGA_FONT_CHARS    128U
#define CGA_CHECKPOINT_ROWS 4U
#define CGA_BANK_MASK  0x1FFFU
#define CGA_TEXT_80COL   0x01U
#define CGA_GRAPHICS     0x02U
#define CGA_MONO_PALETTE 0x04U
#define CGA_HIGH_RES     0x10U
#define CGA_BLINK        0x20U

static const uint16_t cga_rgb[16] = {
    FB16_RGB(0x00, 0x00, 0x00), FB16_RGB(0x00, 0x00, 0xAA),
    FB16_RGB(0x00, 0xAA, 0x00), FB16_RGB(0x00, 0xAA, 0xAA),
    FB16_RGB(0xAA, 0x00, 0x00), FB16_RGB(0xAA, 0x00, 0xAA),
    FB16_RGB(0xAA, 0x55, 0x00), FB16_RGB(0xAA, 0xAA, 0xAA),
    FB16_RGB(0x55, 0x55, 0x55), FB16_RGB(0x55, 0x55, 0xFF),
    FB16_RGB(0x55, 0xFF, 0x55), FB16_RGB(0x55, 0xFF, 0xFF),
    FB16_RGB(0xFF, 0x55, 0x55), FB16_RGB(0xFF, 0x55, 0xFF),
    FB16_RGB(0xFF, 0xFF, 0x55), FB16_RGB(0xFF, 0xFF, 0xFF),
};

static bool cursor_visible(uint8_t start, uint32_t frame)
{
    switch ((start >> 5) & 3U) {
    case 1U: return false;
    case 2U: return (frame & 0x10U) == 0U;
    case 3U: return (frame & 0x20U) == 0U;
    default: return true;
    }
}

static void text_scanline(uint16_t *row, unsigned cols, unsigned line,
                          const pc_transporter_video_state_t *state,
                          const volatile uint8_t *vram, const uint8_t *font,
                          uint8_t *cells, uint32_t frame)
{
    const unsigned glyph_row = line % CGA_GLYPH_ROWS;
    const unsigned start = state->start_address + (line / CGA_GLYPH_ROWS) * cols;
    const bool blink = (state->mode & CGA_BLINK) != 0U;
    /* CGA gates the CRTC cursor with its own blink phase. */
    const bool cursor = (frame & 0x08U) != 0U &&
                        cursor_visible(state->cursor_start, frame) &&
                        glyph_row >= (state->cursor_start & 31U) &&
                        glyph_row <= (state->cursor_end & 31U);

    if (glyph_row == 0U) {
        for (unsigned col = 0; col < cols; col++) {
            const unsigned word = (start + col) & CGA_BANK_MASK;
            cells[2U * col] = vram[2U * word];
            cells[2U * col + 1U] = vram[2U * word + 1U];
        }
    }
    for (unsigned col = 0; col < cols; col++) {
        const unsigned word = (start + col) & CGA_BANK_MASK;
        const uint8_t attr = cells[2U * col + 1U];
        const uint16_t fg = cga_rgb[attr & 15U];
        const uint16_t bg = cga_rgb[(attr >> 4) & (blink ? 7U : 15U)];
        const uint8_t glyph = cells[2U * col];
        uint8_t bits;

        if (glyph < CGA_FONT_CHARS) {
            bits = font[glyph * CGA_GLYPH_ROWS + glyph_row];
        } else {
            /* The BIOS table ends at 7Fh. Keep box lines when scaling the
             * existing CP437 font from 16 rows to 8. */
            const uint8_t *extended = linear_text_overlay_cp437_font_8x16[glyph];
            bits = extended[glyph_row * 2U] | extended[glyph_row * 2U + 1U];
        }
        if (blink && (attr & 0x80U) != 0U && (frame & 0x10U) != 0U) {
            bits = 0U;
        }
        if (cursor && word == (state->cursor_address & CGA_BANK_MASK)) {
            bits = 0xFFU;
        }
        for (unsigned bit = 0; bit < 8U; bit++) {
            row[col * 8U + bit] = (bits & (0x80U >> bit)) != 0U ? fg : bg;
        }
    }
}

static void graphics_scanline(uint16_t *row, unsigned line, unsigned row_bytes,
                              unsigned rasters_per_row,
                              const pc_transporter_video_state_t *state,
                              const volatile uint8_t *vram)
{
    const volatile uint8_t *source = vram;
    unsigned start = line * row_bytes;
    unsigned address_mask = UINT_MAX;
    const unsigned color = state->color & 15U;

    if (state->graphics_linear == 0U) {
        const unsigned character_row = line / rasters_per_row;
        const unsigned raster = line - character_row * rasters_per_row;
        start = state->start_address * 2U + character_row * row_bytes;
        source += (raster & 1U) * (CGA_BANK_MASK + 1U);
        address_mask = CGA_BANK_MASK;
    }
    if ((state->mode & CGA_HIGH_RES) != 0U) {
        for (unsigned i = 0; i < row_bytes; i++) {
            const uint8_t bits = source[(start + i) & address_mask];
            for (unsigned bit = 0; bit < 8U; bit++) {
                row[i * 8U + bit] = cga_rgb[(bits & (0x80U >> bit)) != 0U ? color : 0U];
            }
        }
    } else {
        const unsigned intensity = (state->color & 0x10U) != 0U ? 8U : 0U;
        const bool palette_one = (state->color & 0x20U) != 0U;
        const bool mono = (state->mode & CGA_MONO_PALETTE) != 0U;
        const uint16_t palette[] = {
            cga_rgb[color],
            cga_rgb[intensity + (mono || palette_one ? 3U : 2U)],
            cga_rgb[intensity + (mono || !palette_one ? 4U : 5U)],
            cga_rgb[intensity + (mono || palette_one ? 7U : 6U)],
        };

        for (unsigned i = 0; i < row_bytes; i++) {
            const uint8_t bits = source[(start + i) & address_mask];
            for (unsigned pixel = 0; pixel < 4U; pixel++) {
                row[i * 4U + pixel] = palette[(bits >> (6U - 2U * pixel)) & 3U];
            }
        }
    }
}

void pc_transporter_video_draw(uint16_t *dest, unsigned stride,
                               unsigned width, unsigned height,
                               const pc_transporter_video_state_t *state,
                               const volatile uint8_t *vram,
                               const volatile uint8_t *font,
                               uint32_t frame, void (*checkpoint)(void))
{
    /* The compositor owns this workspace and calls once per frame. */
    static uint8_t glyphs[CGA_FONT_CHARS * CGA_GLYPH_ROWS];
    static uint8_t cells[2U * CGA_TEXT_COLS];
    static uint16_t native_row[COMP_OUT_MAX_WIDTH];
    static uint16_t scaled_row[COMP_OUT_MAX_WIDTH];
    static uint16_t source_x[COMP_OUT_MAX_WIDTH];
    const bool graphics = (state->mode & CGA_GRAPHICS) != 0U;
    const bool wide = (state->mode & (graphics ? CGA_HIGH_RES : CGA_TEXT_80COL)) != 0U;
    unsigned native_width = wide ? CGA_SCAN_PIXELS : CGA_SCAN_PIXELS / 2U;
    unsigned native_height = CGA_SCANLINES;
    unsigned row_bytes = CGA_ROW_BYTES;
    unsigned rasters_per_row = CGA_GRAPHICS_RASTERS;
    unsigned start = 0U;

    if (width == 0U || width > stride || width > COMP_OUT_MAX_WIDTH ||
        height == 0U || height > COMP_OUT_MAX_HEIGHT) {
        return;
    }
    if (graphics) {
        const unsigned programmed_bytes = state->horizontal_displayed * 2U;
        const unsigned programmed_rasters = (state->maximum_scanline & 31U) + 1U;
        const unsigned programmed_width = programmed_bytes * (wide ? 8U : 4U);
        const unsigned programmed_height = (state->vertical_displayed & 127U) *
                                            programmed_rasters;
        if (programmed_width != 0U && programmed_width <= COMP_OUT_MAX_WIDTH &&
            programmed_height != 0U && programmed_height <= COMP_OUT_MAX_HEIGHT) {
            native_width = programmed_width;
            native_height = programmed_height;
            row_bytes = programmed_bytes;
            rasters_per_row = programmed_rasters;
        } else if (state->graphics_linear != 0U) {
            return;
        }
    }
    if (checkpoint != NULL) {
        checkpoint();
    }
    if (!graphics) {
        for (unsigned i = 0; i < sizeof(glyphs); i++) {
            glyphs[i] = font[i];
        }
    }
    for (unsigned x = 0; x < width; x++) {
        source_x[x] = (uint16_t)((x * native_width) / width);
    }

    for (unsigned line = 0; line < native_height; line++) {
        const unsigned end = ((line + 1U) * height) / native_height;

        if (graphics) {
            graphics_scanline(native_row, line, row_bytes, rasters_per_row, state, vram);
        } else {
            text_scanline(native_row, native_width / 8U, line, state, vram,
                          glyphs, cells, frame);
        }
        if (end != start) {
            for (unsigned x = 0; x < width; x++) {
                scaled_row[x] = native_row[source_x[x]];
            }
            for (unsigned y = start; y < end; y++) {
                memcpy(dest + y * stride, scaled_row, width * sizeof(*dest));
            }
            start = end;
        }
        if (checkpoint != NULL && (line + 1U) % CGA_CHECKPOINT_ROWS == 0U) {
            checkpoint();
        }
    }
}
