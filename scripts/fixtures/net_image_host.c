/* Host check of the picture service: convert files, or run the built-in cases. */
#include "net_image.h"
#include "../../ps_sources/lib/lodepng.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t out[NET_IMAGE_OUT_MAX];

static unsigned white_pixels(unsigned w, unsigned h)
{
    unsigned row = (w + 6U) / 7U, count = 0;
    for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; ++x)
            count += (out[4U + y * row + x / 7U] >> (x % 7U)) & 1U;
    return count;
}

static void png_case(unsigned w, unsigned h, unsigned gray, unsigned alpha,
                     unsigned box_w, unsigned box_h, unsigned want_w, unsigned want_h,
                     unsigned low_percent, unsigned high_percent)
{
    unsigned char *rgba = malloc((size_t)w * h * 4U), *png = NULL;
    size_t png_size = 0, length = 0;
    for (size_t i = 0; i < (size_t)w * h; ++i) {
        rgba[i * 4U] = rgba[i * 4U + 1U] = rgba[i * 4U + 2U] = (unsigned char)gray;
        rgba[i * 4U + 3U] = (unsigned char)alpha;
    }
    assert(!lodepng_encode32(&png, &png_size, rgba, w, h));
    memset(out, 0xEE, sizeof(out));
    assert(!net_image_convert(png, png_size, box_w, box_h, out, &length));
    unsigned tw = out[0] | (out[1] << 8), th = out[2] | (out[3] << 8);
    assert(tw == want_w && th == want_h);
    assert(length == 4U + (size_t)((tw + 6U) / 7U) * th);
    unsigned percent = white_pixels(tw, th) * 100U / (tw * th);
    assert(percent >= low_percent && percent <= high_percent);
    /* Bit 7 and the padding pixels stay 0. */
    for (unsigned y = 0; y < th; ++y) {
        const uint8_t *row = out + 4U + y * ((tw + 6U) / 7U);
        for (unsigned b = 0; b < (tw + 6U) / 7U; ++b) assert(!(row[b] & 0x80U));
        for (unsigned x = tw; x < ((tw + 6U) / 7U) * 7U; ++x) assert(!((row[x / 7U] >> (x % 7U)) & 1U));
    }
    free(png);
    free(rgba);
}

int main(int argc, char **argv)
{
    size_t length = 0;
    if (argc >= 5) {
        FILE *f = fopen(argv[1], "rb");
        static uint8_t data[NET_IMAGE_ENCODED_MAX];
        size_t n;
        if (!f) return 2;
        n = fread(data, 1, sizeof(data), f);
        fclose(f);
        int result = net_image_convert(data, n, (unsigned)atoi(argv[3]), (unsigned)atoi(argv[4]), out, &length);
        printf("result=%d length=%u width=%u height=%u\n", result, (unsigned)length,
               out[0] | (out[1] << 8), out[2] | (out[3] << 8));
        if (result) return 1;
        f = fopen(argv[2], "wb");
        if (!f) return 2;
        fwrite(out, 1, length, f);
        fclose(f);
        return 0;
    }
    png_case(10, 10, 255, 255, 546, 144, 10, 10, 100, 100);   /* never grows */
    png_case(10, 10, 0, 255, 546, 144, 10, 10, 0, 0);
    png_case(10, 10, 0, 0, 546, 144, 10, 10, 100, 100);       /* transparent is white */
    png_case(1092, 100, 128, 255, 546, 144, 546, 50, 45, 56); /* width limit, mid gray */
    png_case(100, 1440, 64, 255, 546, 144, 10, 144, 20, 30);  /* height limit */
    png_case(4096, 1024, 200, 255, 560, 384, 560, 140, 73, 83);
    {
        static const uint8_t junk[64] = {'B', 'M', 1, 2, 3};
        static uint8_t big_png[33] = {137, 'P', 'N', 'G', 13, 10, 26, 10, 0, 0, 0, 13, 'I', 'H', 'D', 'R',
                                      0, 0, 0x20, 0, 0, 0, 0x20, 0, 8, 6, 0, 0, 0};
        static const uint8_t gif[] = {'G','I','F','8','9','a',2,0,2,0,0x80,0,0, 0,0,0, 255,255,255,
                                      0x2C,0,0,0,0,2,0,2,0,0, 2, 3,0x84,0x8F,0x05, 0, 0x3B};
        assert(net_image_convert(junk, sizeof(junk), 546, 144, out, &length) == NET_IMAGE_E_FORMAT);
        assert(net_image_convert(big_png, sizeof(big_png), 546, 144, out, &length) == NET_IMAGE_E_SIZE);
        assert(net_image_convert(junk, sizeof(junk), 561, 144, out, &length) == NET_IMAGE_E_SIZE);
        assert(net_image_convert(junk, 0, 546, 144, out, &length) == NET_IMAGE_E_FORMAT);
        int result = net_image_convert(gif, sizeof(gif), 546, 144, out, &length);
        assert(result == 0 && out[0] == 2 && out[2] == 2 && length == 6U);
    }
    puts("PASS picture service: size rules, transparency, gray level, padding, bad input, GIF");
    return 0;
}
