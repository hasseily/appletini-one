#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "pc_transporter_video.h"
#include "linear_text_overlay_font.h"
#include "../ps_sources/lib/fb16.h"

#define LINES 200U
#define EXTENDED_WIDTH 768U
#define EXTENDED_LINES 240U
#define EXTENDED_ROW_BYTES 96U
#define STRIDE (EXTENDED_WIDTH + 2U)

static uint16_t pixels[STRIDE * (EXTENDED_LINES + 1U)];
static uint8_t vram[0x4000U];
static uint8_t font[128U * 8U];
static unsigned checkpoints;

static void count_checkpoint(void)
{
    checkpoints++;
}

static void test_text(void)
{
    pc_transporter_video_state_t state = {
        .mode = 0x29U, /* 80 columns, video and attribute blink enabled. */
        .start_address = 0x1FFFU,
        .cursor_start = 0x20U,
    };
    const uint16_t blue = FB16_RGB(0x00, 0x00, 0xAA);
    const uint16_t green = FB16_RGB(0x00, 0xAA, 0x00);
    const uint16_t gray = FB16_RGB(0xAA, 0xAA, 0xAA);

    vram[0x3FFEU] = 'A';
    vram[0x3FFFU] = 0x92U;
    vram[0U] = 0xB0U;
    vram[1U] = 0x07U;
    font['A' * 8U] = 0x80U;
    pixels[640] = 0xFFFFU;

    pc_transporter_video_draw(pixels, STRIDE, 640U, LINES, &state, vram, font, 0U, NULL);
    assert(pixels[0] == green && pixels[1] == blue);
    const uint8_t extended_bits = linear_text_overlay_cp437_font_8x16[0xB0U][0] |
                                  linear_text_overlay_cp437_font_8x16[0xB0U][1];
    assert(extended_bits != 0U && extended_bits != 0xFFU);
    for (unsigned x = 0; x < 8U; x++) {
        assert(pixels[8U + x] == ((extended_bits & (0x80U >> x)) != 0U ? gray : 0U));
    }
    assert(pixels[640] == 0xFFFFU);

    state.cursor_start = 1U;
    state.cursor_end = 1U;
    state.cursor_address = 0x1FFFU;
    pc_transporter_video_draw(pixels, STRIDE, 640U, LINES, &state, vram, font, 16U, NULL);
    assert(pixels[0] == blue && pixels[1] == blue);
    assert(pixels[STRIDE] == blue && pixels[STRIDE + 7U] == blue);
    pc_transporter_video_draw(pixels, STRIDE, 640U, LINES, &state, vram, font, 24U, NULL);
    assert(pixels[STRIDE] == green && pixels[STRIDE + 7U] == green);
}

static void test_control_panel_cursor(void)
{
    pc_transporter_video_state_t state = {
        .mode = 0x0DU,
        .cursor_address = 0x023FU,
        .cursor_end = 7U,
    };
    const uint16_t gray = FB16_RGB(0xAA, 0xAA, 0xAA);
    const unsigned first_pixel = 7U * 8U * STRIDE + 15U * 8U;
    const unsigned cell = 2U * state.cursor_address;
    memset(vram, 0, sizeof(vram));
    memset(font, 0, sizeof(font));
    vram[cell] = '1';
    vram[cell + 1U] = 0x07U;
    vram[cell + 2U] = '1';
    vram[cell + 3U] = 0x70U;
    font['1' * 8U] = 0x18U;

    for (unsigned frame = 0U; frame < 32U; frame += 8U) {
        pc_transporter_video_draw(pixels, STRIDE, 640U, LINES, &state, vram, font, frame, NULL);
        assert(pixels[first_pixel] == ((frame & 8U) != 0U ? gray : 0U));
        assert(pixels[first_pixel + 3U] == gray);
        assert(pixels[first_pixel + 8U] == gray);
        assert(pixels[first_pixel + 11U] == 0U);
    }
    state.cursor_start = 0x20U;
    pc_transporter_video_draw(pixels, STRIDE, 640U, LINES, &state, vram, font, 8U, NULL);
    assert(pixels[first_pixel] == 0U && pixels[first_pixel + 3U] == gray);
}

static void test_graphics(void)
{
    pc_transporter_video_state_t state = {
        .mode = 0x0AU, /* 320x200 with the first RGBI palette. */
        .color = 0x01U,
    };
    const uint16_t blue = FB16_RGB(0x00, 0x00, 0xAA);
    const uint16_t green = FB16_RGB(0x00, 0xAA, 0x00);
    const uint16_t red = FB16_RGB(0xAA, 0x00, 0x00);
    const uint16_t brown = FB16_RGB(0xAA, 0x55, 0x00);

    memset(vram, 0, sizeof(vram));
    vram[0U] = 0x1BU; /* Four pixels with indices 0, 1, 2 and 3. */
    vram[0x2000U] = 0x40U;
    pc_transporter_video_draw(pixels, STRIDE, 640U, LINES, &state, vram, font, 0U,
                              count_checkpoint);
    assert(checkpoints == 1U + LINES / 4U);
    assert(pixels[0] == blue && pixels[1] == blue);
    assert(pixels[2] == green && pixels[3] == green);
    assert(pixels[4] == red && pixels[6] == brown);
    assert(pixels[STRIDE] == green && pixels[STRIDE + 2U] == blue);

    state.mode = 0x1AU;
    state.color = 0x04U;
    vram[0U] = 0x80U;
    pc_transporter_video_draw(pixels, STRIDE, 640U, LINES, &state, vram, font, 0U, NULL);
    assert(pixels[0] == red && pixels[1] == 0U);
    assert(pixels[STRIDE] == 0U && pixels[STRIDE + 1U] == red);
}

static void test_cgademo_geometry(void)
{
    static uint8_t captured[EXTENDED_LINES * EXTENDED_ROW_BYTES];
    pc_transporter_video_state_t state = {
        .mode = 0x1AU,
        .color = 0x0FU,
        .horizontal_displayed = 0x30U,
        .vertical_displayed = 0x78U,
        .maximum_scanline = 0x01U,
    };

    for (unsigned i = 0; i < sizeof(vram); i++) {
        vram[i] = (uint8_t)((i * 29U) ^ (i >> 3) ^ (i >> 9));
    }
    for (unsigned y = 0; y < EXTENDED_LINES; y++) {
        const unsigned bank = (y & 1U) * 8192U;
        const unsigned start = (y / 2U) * EXTENDED_ROW_BYTES;
        for (unsigned i = 0; i < EXTENDED_ROW_BYTES; i++) {
            captured[y * EXTENDED_ROW_BYTES + i] = vram[bank + (start + i) % 8192U];
        }
    }
    for (unsigned linear = 0; linear < 2U; linear++) {
        state.graphics_linear = (uint8_t)linear;
        state.start_address = linear != 0U ? 0x0123U : 0U;
        const uint8_t *source = linear != 0U ? captured : vram;
        memset(pixels, 0xFF, sizeof(pixels));
        checkpoints = 0U;
        pc_transporter_video_draw(pixels, STRIDE, EXTENDED_WIDTH, EXTENDED_LINES,
                                  &state, source, font, 0U, count_checkpoint);
        assert(checkpoints == 1U + EXTENDED_LINES / 4U);
        for (unsigned y = 0; y < EXTENDED_LINES; y++) {
            for (unsigned x = 0; x < EXTENDED_WIDTH; x++) {
                const uint8_t byte = captured[y * EXTENDED_ROW_BYTES + x / 8U];
                const uint16_t expected = (byte & (0x80U >> (x % 8U))) != 0U ? 0xFFFFU : 0U;
                assert(pixels[y * STRIDE + x] == expected);
            }
            assert(pixels[y * STRIDE + EXTENDED_WIDTH] == 0xFFFFU);
            assert(pixels[y * STRIDE + EXTENDED_WIDTH + 1U] == 0xFFFFU);
        }
        assert(pixels[EXTENDED_LINES * STRIDE] == 0xFFFFU);
    }

    /* The same CRTC settings give 384 pixels in two-bit graphics mode. */
    state.graphics_linear = 0U;
    state.start_address = 0U;
    state.mode = 0x0AU;
    state.color = 0U;
    memset(vram, 0, sizeof(vram));
    vram[95U] = 0x01U;
    vram[96U] = 0x80U;
    pc_transporter_video_draw(pixels, STRIDE, 384U, EXTENDED_LINES,
                              &state, vram, font, 0U, NULL);
    assert(pixels[382U] == 0U && pixels[383U] == FB16_RGB(0x00, 0xAA, 0x00));
    assert(pixels[2U * STRIDE] == FB16_RGB(0xAA, 0x00, 0x00));

    state.graphics_linear = 1U;
    state.horizontal_displayed = 0U;
    pixels[0] = 0x5AA5U;
    checkpoints = 0U;
    pc_transporter_video_draw(pixels, STRIDE, 384U, EXTENDED_LINES,
                              &state, NULL, font, 0U, count_checkpoint);
    assert(pixels[0] == 0x5AA5U && checkpoints == 0U);
}

static void test_graphics_raster_address(void)
{
    const pc_transporter_video_state_t state = {
        .mode = 0x1AU,
        .color = 0x0FU,
        .start_address = 0x0FFFU,
        .horizontal_displayed = 1U,
        .vertical_displayed = 2U,
        .maximum_scanline = 2U,
    };
    static const unsigned first_white[] = {0U, 1U, 0U, 2U, 3U, 2U};

    memset(vram, 0, sizeof(vram));
    vram[0x1FFEU] = 0x80U;
    vram[0x3FFEU] = 0x40U;
    vram[0U] = 0x20U;
    vram[0x2000U] = 0x10U;
    pc_transporter_video_draw(pixels, STRIDE, 16U, 6U, &state, vram, font, 0U, NULL);
    for (unsigned y = 0; y < sizeof(first_white) / sizeof(first_white[0]); y++) {
        for (unsigned x = 0; x < 16U; x++) {
            assert(pixels[y * STRIDE + x] == (x == first_white[y] ? 0xFFFFU : 0U));
        }
    }
}

int main(void)
{
    test_text();
    test_control_panel_cursor();
    test_graphics();
    test_cgademo_geometry();
    test_graphics_raster_address();
    puts("test_pc_transporter_video: all checks passed");
    return 0;
}
