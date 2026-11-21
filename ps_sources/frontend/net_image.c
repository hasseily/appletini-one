#include "net_image.h"
#include "../lib/lodepng.h"
#include <stdlib.h>
#include <string.h>

/* JPEG (baseline and progressive) and GIF come from stb_image. PNG uses
 * lodepng, which the firmware already has. Web images are not trusted:
 * check the declared size before any decoder allocates memory. */
#define STBI_NO_STDIO
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#define STBI_NO_FAILURE_STRINGS
#define STBI_MAX_DIMENSIONS NET_IMAGE_SIDE_MAX
#define STBI_ASSERT(x) ((void)0)
#define STB_IMAGE_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wunused-value"
#endif
#include "../../third_party/stb/stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

static int size_ok(unsigned w, unsigned h)
{
    return w && h && w <= NET_IMAGE_SIDE_MAX && h <= NET_IMAGE_SIDE_MAX &&
           (uint32_t)w * h <= NET_IMAGE_PIXELS_MAX;
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static unsigned luma_of(const uint8_t *p)
{
    return (77U * p[0] + 150U * p[1] + 29U * p[2] + 128U) >> 8;
}

/* RGBA to luminance on white (255) or black (0) paper. */
static unsigned gray_of(const uint8_t *p, unsigned paper)
{
    unsigned alpha = p[3];
    return (luma_of(p) * alpha + paper * (255U - alpha) + 127U) / 255U;
}

/* A light drawing on a transparent background is made for a dark page:
 * on white paper it would vanish. Give it black paper. */
static unsigned paper_of(const uint8_t *rgba, size_t pixels)
{
    uint64_t weighted = 0, alpha = 0;
    for (size_t i = 0; i < pixels; ++i, rgba += 4) {
        weighted += (uint64_t)luma_of(rgba) * rgba[3];
        alpha += rgba[3];
    }
    if (alpha >= (uint64_t)pixels * 255U * 7U / 8U) return 255U; /* mostly opaque */
    return alpha && weighted / alpha >= 160U ? 0U : 255U;
}

int net_image_convert(const uint8_t *data, size_t length, unsigned box_w, unsigned box_h,
                      uint8_t out[NET_IMAGE_OUT_MAX], size_t *out_length)
{
    static const uint8_t png_signature[8] = {137, 'P', 'N', 'G', 13, 10, 26, 10};
    unsigned char *rgba = NULL;
    unsigned w = 0, h = 0, tw, th, row_bytes, paper;
    int16_t *errors;
    int result = 0;

    if (!data || !out || !out_length || !box_w || !box_h ||
        box_w > NET_IMAGE_BOX_W_MAX || box_h > NET_IMAGE_BOX_H_MAX) return NET_IMAGE_E_SIZE;
    *out_length = 0;
    if (length > NET_IMAGE_ENCODED_MAX) return NET_IMAGE_E_SIZE;
    if (length >= 33U && !memcmp(data, png_signature, 8)) {
        if (memcmp(data + 12, "IHDR", 4)) return NET_IMAGE_E_FORMAT;
        if (!size_ok(be32(data + 16), be32(data + 20))) return NET_IMAGE_E_SIZE;
        if (lodepng_decode32(&rgba, &w, &h, data, length)) {
            free(rgba);
            return NET_IMAGE_E_FORMAT;
        }
    } else {
        int sw = 0, sh = 0, components = 0;
        if (length > 0x7FFFFFFFU ||
            !stbi_info_from_memory(data, (int)length, &sw, &sh, &components)) return NET_IMAGE_E_FORMAT;
        if (sw <= 0 || sh <= 0 || !size_ok((unsigned)sw, (unsigned)sh)) return NET_IMAGE_E_SIZE;
        rgba = stbi_load_from_memory(data, (int)length, &sw, &sh, &components, 4);
        if (!rgba) return NET_IMAGE_E_FORMAT;
        w = (unsigned)sw;
        h = (unsigned)sh;
    }
    if (!size_ok(w, h)) {
        free(rgba);
        return NET_IMAGE_E_SIZE;
    }

    paper = paper_of(rgba, (size_t)w * h);
    tw = w;
    th = h;
    if (tw > box_w) {
        th = (unsigned)((uint32_t)h * box_w / w);
        tw = box_w;
    }
    if (th > box_h) {
        tw = (unsigned)((uint32_t)w * box_h / h);
        th = box_h;
    }
    if (!tw) tw = 1;
    if (!th) th = 1;
    row_bytes = (tw + 6U) / 7U;

    /* Two error rows for Floyd-Steinberg, with one guard cell at each end. */
    errors = calloc(2U * (tw + 2U), sizeof(*errors));
    if (!errors) {
        free(rgba);
        return NET_IMAGE_E_MEMORY;
    }
    out[0] = (uint8_t)tw;
    out[1] = (uint8_t)(tw >> 8);
    out[2] = (uint8_t)th;
    out[3] = (uint8_t)(th >> 8);
    memset(out + NET_IMAGE_HEADER, 0, (size_t)row_bytes * th);
    for (unsigned y = 0; y < th; ++y) {
        int16_t *now = errors + (y & 1U) * (tw + 2U) + 1;
        int16_t *next = errors + ((y + 1U) & 1U) * (tw + 2U) + 1;
        uint8_t *row = out + NET_IMAGE_HEADER + (size_t)y * row_bytes;
        unsigned y0 = (unsigned)((uint32_t)y * h / th);
        unsigned y1 = (unsigned)((uint32_t)(y + 1U) * h / th);
        if (y1 <= y0) y1 = y0 + 1U;
        memset(next - 1, 0, (tw + 2U) * sizeof(*next));
        for (unsigned x = 0; x < tw; ++x) {
            unsigned x0 = (unsigned)((uint32_t)x * w / tw);
            unsigned x1 = (unsigned)((uint32_t)(x + 1U) * w / tw);
            uint32_t sum = 0;
            int value, error;
            if (x1 <= x0) x1 = x0 + 1U;
            for (unsigned sy = y0; sy < y1; ++sy) {
                const uint8_t *p = rgba + ((size_t)sy * w + x0) * 4U;
                for (unsigned sx = x0; sx < x1; ++sx, p += 4) sum += gray_of(p, paper);
            }
            value = (int)(sum / ((y1 - y0) * (x1 - x0))) + now[x] / 16;
            if (value >= 128) {
                row[x / 7U] |= (uint8_t)(1U << (x % 7U));
                error = value - 255;
            } else {
                error = value;
            }
            /* Signed index: x - 1 reaches the guard cell before the row. */
            now[(int)x + 1] = (int16_t)(now[(int)x + 1] + error * 7);
            next[(int)x - 1] = (int16_t)(next[(int)x - 1] + error * 3);
            next[(int)x] = (int16_t)(next[(int)x] + error * 5);
            next[(int)x + 1] = (int16_t)(next[(int)x + 1] + error);
        }
    }
    free(errors);
    free(rgba);
    *out_length = NET_IMAGE_HEADER + (size_t)row_bytes * th;
    return result;
}
