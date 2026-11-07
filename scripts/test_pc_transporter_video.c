#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "pc_transporter_video.h"
#include "linear_text_overlay_font.h"
#include "../ps_sources/lib/fb16.h"

#define STRIDE 642U
#define LINES 200U

static uint16_t pixels[STRIDE * LINES];
static uint8_t vram[0x4000U];
static uint8_t font[128U * 8U];

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

    pc_transporter_video_draw(pixels, STRIDE, 640U, LINES, &state, vram, font, 0U);
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
    pc_transporter_video_draw(pixels, STRIDE, 640U, LINES, &state, vram, font, 16U);
    assert(pixels[0] == blue && pixels[1] == blue);
    assert(pixels[STRIDE] == green && pixels[STRIDE + 7U] == green);
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
    pc_transporter_video_draw(pixels, STRIDE, 640U, LINES, &state, vram, font, 0U);
    assert(pixels[0] == blue && pixels[1] == blue);
    assert(pixels[2] == green && pixels[3] == green);
    assert(pixels[4] == red && pixels[6] == brown);
    assert(pixels[STRIDE] == green && pixels[STRIDE + 2U] == blue);

    state.mode = 0x1AU;
    state.color = 0x04U;
    vram[0U] = 0x80U;
    pc_transporter_video_draw(pixels, STRIDE, 640U, LINES, &state, vram, font, 0U);
    assert(pixels[0] == red && pixels[1] == 0U);
    assert(pixels[STRIDE] == 0U && pixels[STRIDE + 1U] == red);
}

int main(void)
{
    test_text();
    test_graphics();
    puts("test_pc_transporter_video: all checks passed");
    return 0;
}
