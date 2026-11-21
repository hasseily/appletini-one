#ifndef ONEWWW_FONT_H
#define ONEWWW_FONT_H

#include <stdint.h>

/* Screen fonts for ONEWWW, baked by scripts/onewww_bake_font.py.
 * A glyph bitmap has 2 bits for each pixel (0 paper, 1 light, 2 dark, 3 ink),
 * rows padded to a byte, first pixel in the two high bits. x and y give
 * the bitmap origin from the pen position and the ascender line. */
typedef struct {
    uint16_t code;
    uint32_t offset;
    uint8_t  w;
    uint8_t  h;
    int8_t   x;
    int8_t   y;
    uint8_t  advance;
} onewww_glyph_t;

typedef struct {
    const onewww_glyph_t *glyphs;   /* sorted by code */
    const uint8_t        *bits;
    uint16_t              count;
    uint8_t               ascent;
    uint8_t               descent;
} onewww_font_t;

enum {
    ONEWWW_FONT_BODY = 0,
    ONEWWW_FONT_BODY_B,
    ONEWWW_FONT_BODY_I,
    ONEWWW_FONT_BODY_BI,
    ONEWWW_FONT_H1,
    ONEWWW_FONT_H2,
    ONEWWW_FONT_MONO,
    ONEWWW_FONT_SMALL,
    ONEWWW_FONT_COUNT
};

extern const onewww_font_t onewww_fonts[ONEWWW_FONT_COUNT];

#endif
