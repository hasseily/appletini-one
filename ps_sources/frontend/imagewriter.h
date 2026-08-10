#ifndef IMAGEWRITER_H
#define IMAGEWRITER_H

/* ImageWriter II printer-language interpreter and page renderer.
 *
 * Pure model: no filesystem, no PL register, and no platform knowledge, so
 * a host harness can compile and drive it. printer_service.c feeds it the
 * SSC byte stream; each completed page comes back through the page_done
 * callback as an 8-bit ink-mask canvas. Zero is paper; the four low bits
 * record yellow, magenta, cyan, and black ribbon strikes. Keeping bitplanes
 * in one byte lets overprints mix without making the live page buffer larger.
 *
 * Geometry: US Letter at 144 dpi vertically (1 unit = 1/144 inch = 1 px)
 * and 2880 units/inch horizontally (20 units = 1 px), so every ImageWriter
 * pitch and the ESC T half-dot line spacing stay integral.
 */

#include <stdint.h>

#define IW_PAGE_DPI    144
#define IW_PAGE_WIDTH  1224          /*  8.5 in * 144 dpi */
#define IW_PAGE_HEIGHT 1584          /* 11.0 in * 144 dpi */

#define IW_XUNITS_PER_INCH 2880
#define IW_XUNITS_PER_PX   20
#define IW_YUNITS_PER_INCH 144

#define IW_INK_NONE     0x00U
#define IW_INK_YELLOW   0x01U
#define IW_INK_MAGENTA  0x02U
#define IW_INK_CYAN     0x04U
#define IW_INK_BLACK    0x08U

typedef void (*iw_page_done_fn)(void *ctx,
                                const uint8_t *canvas,
                                uint32_t width,
                                uint32_t height);

typedef enum {
    IW_ESC_NONE = 0,   /* plain character stream */
    IW_ESC_CMD,        /* saw ESC, waiting for the command byte */
    IW_ESC_PARAMS,     /* collecting fixed-length parameters */
    IW_ESC_TABLIST     /* ESC ( / ESC ) list, runs to the '.' terminator */
} iw_esc_state_t;

typedef struct {
    uint8_t *canvas;               /* one IW_INK_* mask per page pixel */
    uint8_t  page_dirty;
    uint8_t  ink_mask;             /* ribbon color selected by ESC K n */

    int32_t x;                     /* 1/2880 in from the paper edge */
    int32_t y;                     /* 1/144 in from the top of the page */
    int32_t left_margin;           /* 1/2880 in */
    int32_t page_len;              /* 1/144 in */
    int32_t line_spacing;          /* 1/144 in */
    int32_t char_advance;          /* 1/2880 in per character cell */
    int32_t dot_advance;           /* 1/2880 in per graphics dot column */

    uint8_t bold;
    uint8_t underline;
    uint8_t halfheight;
    uint8_t script;                /* 0 none, 1 superscript, 2 subscript */
    uint8_t doublewide;            /* SO latch */
    uint8_t doublewide_line;       /* one-line double width */
    uint8_t reverse_feed;
    uint8_t msb_pass;              /* ESC $ cancels the default bit-7 strip */

    iw_esc_state_t esc_state;
    uint8_t  esc_cmd;
    uint8_t  params_needed;
    uint8_t  params_have;
    uint8_t  params[8];

    uint32_t graphics_remaining;   /* pending ESC G/S/g data bytes */

    iw_page_done_fn page_done;
    void *page_done_ctx;
} imagewriter_t;

/* Attaches a caller-owned canvas buffer of IW_PAGE_WIDTH*IW_PAGE_HEIGHT
 * bytes (the frontend mallocs it once; the host harness can use a static).
 * Returns 0 on success, -1 on a NULL canvas. */
int imagewriter_init(imagewriter_t *iw,
                     uint8_t *canvas,
                     iw_page_done_fn page_done,
                     void *page_done_ctx);

/* Power-on state: pica, 1/6-inch line spacing, styles off, top of page.
 * The canvas is wiped only if it holds no committed content. */
void imagewriter_reset(imagewriter_t *iw);

void imagewriter_feed(imagewriter_t *iw, const uint8_t *data, uint32_t len);

/* Emits the current page if anything was printed on it (job end). */
void imagewriter_flush_page(imagewriter_t *iw);

uint8_t imagewriter_page_dirty(const imagewriter_t *iw);

#endif /* IMAGEWRITER_H */
