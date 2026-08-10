/* ImageWriter II printer-language interpreter and page renderer.
 *
 * Command coverage follows the ImageWriter II manual, cross-checked against
 * the GSport imagewriter emulation: draft text in every fixed pitch, bold,
 * underline, half-height, super/subscript, double width, ESC G/S/g/V dot
 * graphics, line-spacing and positioning controls, and form handling. The
 * LQ-only and user-defined-character commands are consumed and ignored.
 *
 * Text uses the shared 7x8 dot font (fb16_builtin_font_glyph); glyph dot
 * columns are spaced at one eighth of the current character pitch, which is
 * how the real printhead compresses the same 8-column cell from 9 to 17 cpi.
 */

#include "imagewriter.h"

#include <string.h>

#include "../lib/fb16.h"

#define IW_X_LIMIT (IW_PAGE_WIDTH * IW_XUNITS_PER_PX)

#define IW_GLYPH_COLS   8
#define IW_GLYPH_ROWS   8
#define IW_ROW_PITCH_PX 2          /* dot rows sit 1/72 in apart */

static void iw_wipe_canvas(imagewriter_t *iw)
{
    memset(iw->canvas, IW_INK_NONE,
           (size_t)IW_PAGE_WIDTH * IW_PAGE_HEIGHT);
    iw->page_dirty = 0U;
}

static void iw_emit_page(imagewriter_t *iw)
{
    if (iw->page_dirty == 0U) {
        return;
    }
    if (iw->page_done != NULL) {
        iw->page_done(iw->page_done_ctx, iw->canvas,
                      IW_PAGE_WIDTH, IW_PAGE_HEIGHT);
    }
    iw_wipe_canvas(iw);
}

int imagewriter_init(imagewriter_t *iw,
                     uint8_t *canvas,
                     iw_page_done_fn page_done,
                     void *page_done_ctx)
{
    if (canvas == NULL) {
        return -1;
    }
    memset(iw, 0, sizeof(*iw));
    iw->canvas = canvas;
    iw->page_done = page_done;
    iw->page_done_ctx = page_done_ctx;
    iw_wipe_canvas(iw);
    imagewriter_reset(iw);
    return 0;
}

void imagewriter_reset(imagewriter_t *iw)
{
    iw->x = 0;
    iw->y = 0;
    iw->left_margin = 0;
    iw->page_len = IW_PAGE_HEIGHT;
    iw->line_spacing = 24;                 /* 1/6 inch */
    iw->char_advance = 288;                /* pica, 10 cpi */
    iw->dot_advance = 36;                  /* 80 dpi graphics */
    iw->bold = 0U;
    iw->underline = 0U;
    iw->halfheight = 0U;
    iw->script = 0U;
    iw->doublewide = 0U;
    iw->doublewide_line = 0U;
    iw->reverse_feed = 0U;
    iw->msb_pass = 0U;
    iw->ink_mask = IW_INK_BLACK;
    iw->esc_state = IW_ESC_NONE;
    iw->graphics_remaining = 0U;
}

uint8_t imagewriter_page_dirty(const imagewriter_t *iw)
{
    return iw->page_dirty;
}

void imagewriter_flush_page(imagewriter_t *iw)
{
    iw_emit_page(iw);
    iw->y = 0;
    iw->x = iw->left_margin;
}

/* A filled printhead dot: 2x2 page pixels (1/72 in), doubled horizontally
 * for double-width printing. Clips at the paper edges. */
static void iw_dot(imagewriter_t *iw, int32_t x_units, int32_t y_px,
                   int32_t w_px, int32_t h_px)
{
    int32_t x_px = x_units / IW_XUNITS_PER_PX;
    int32_t x_end = x_px + w_px;
    int32_t y_end = y_px + h_px;
    int32_t yy;

    if (x_px < 0) {
        x_px = 0;
    }
    if (y_px < 0) {
        y_px = 0;
    }
    if (x_end > IW_PAGE_WIDTH) {
        x_end = IW_PAGE_WIDTH;
    }
    if (y_end > IW_PAGE_HEIGHT) {
        y_end = IW_PAGE_HEIGHT;
    }
    if (x_px >= x_end || y_px >= y_end) {
        return;
    }
    for (yy = y_px; yy < y_end; ++yy) {
        uint8_t *row = &iw->canvas[(size_t)yy * IW_PAGE_WIDTH + (size_t)x_px];
        int32_t xx;

        for (xx = 0; xx < x_end - x_px; ++xx) {
            row[xx] |= iw->ink_mask;
        }
    }
    iw->page_dirty = 1U;
}

static void iw_start_new_page(imagewriter_t *iw)
{
    iw_emit_page(iw);
    if (iw->y >= iw->page_len) {
        iw->y -= iw->page_len;
    } else {
        iw->y = 0;
    }
}

static void iw_line_feed(imagewriter_t *iw)
{
    if (iw->reverse_feed != 0U) {
        iw->y -= iw->line_spacing;
        if (iw->y < 0) {
            iw->y = 0;
        }
        return;
    }
    iw->y += iw->line_spacing;
    while (iw->y >= iw->page_len) {
        iw_start_new_page(iw);
    }
    iw->doublewide_line = 0U;
}

static int32_t iw_effective_advance(const imagewriter_t *iw)
{
    int32_t adv = iw->char_advance;

    if (iw->doublewide != 0U || iw->doublewide_line != 0U) {
        adv *= 2;
    }
    return adv;
}

static void iw_draw_char(imagewriter_t *iw, uint8_t ch)
{
    const uint8_t *glyph = fb16_builtin_font_glyph(ch);
    const int32_t adv = iw_effective_advance(iw);
    const int32_t col_pitch = adv / IW_GLYPH_COLS;
    const uint8_t wide =
        (iw->doublewide != 0U || iw->doublewide_line != 0U) ? 1U : 0U;
    const int32_t dot_w = wide ? 4 : 2;
    int32_t row_pitch = IW_ROW_PITCH_PX;
    int32_t y_base = iw->y;
    int32_t row;
    int32_t col;

    if (iw->x + adv > IW_X_LIMIT) {
        iw->x = iw->left_margin;
        iw_line_feed(iw);
    }

    if (iw->halfheight != 0U || iw->script != 0U) {
        row_pitch = 1;
        if (iw->halfheight != 0U || iw->script == 2U) {
            y_base += IW_GLYPH_ROWS;   /* bottom half of the cell */
        }
    }

    if (glyph != NULL) {
        for (row = 0; row < IW_GLYPH_ROWS; ++row) {
            const uint8_t bits = glyph[row];
            for (col = 0; col < 7; ++col) {
                if ((bits & (uint8_t)(1U << col)) != 0U) {
                    const int32_t dot_x = iw->x + col * col_pitch;
                    const int32_t dot_y = y_base + row * row_pitch;
                    iw_dot(iw, dot_x, dot_y, dot_w, row_pitch);
                    if (iw->bold != 0U) {
                        iw_dot(iw, dot_x + col_pitch / 2, dot_y,
                               dot_w, row_pitch);
                    }
                }
            }
        }
    }
    if (iw->underline != 0U) {
        iw_dot(iw, iw->x,
               iw->y + IW_GLYPH_ROWS * IW_ROW_PITCH_PX,
               adv / IW_XUNITS_PER_PX, 2);
    }
    iw->x += adv;
}

static void iw_draw_graphics_column(imagewriter_t *iw, uint8_t bits)
{
    int32_t bit;

    for (bit = 0; bit < 8; ++bit) {
        if ((bits & (uint8_t)(1U << bit)) != 0U) {
            /* Bit 0 is the top dot on the ImageWriter head. */
            iw_dot(iw, iw->x, iw->y + bit * IW_ROW_PITCH_PX,
                   2, IW_ROW_PITCH_PX);
        }
    }
    iw->x += iw->dot_advance;
    if (iw->x >= IW_X_LIMIT) {
        iw->x = iw->left_margin;
    }
}

static void iw_set_pitch(imagewriter_t *iw,
                         int32_t char_advance,
                         int32_t dot_advance)
{
    iw->char_advance = char_advance;
    iw->dot_advance = dot_advance;
}

static uint32_t iw_param_digits(const imagewriter_t *iw, uint8_t count)
{
    uint32_t value = 0U;
    uint8_t i;

    for (i = 0U; i < count; ++i) {
        uint8_t ch = iw->params[i];
        if (ch == ' ') {
            ch = '0';
        }
        if (ch < '0' || ch > '9') {
            ch = '0';
        }
        value = value * 10U + (uint32_t)(ch - '0');
    }
    return value;
}

/* Parameter byte counts for the fixed-length ESC commands; 0xFF marks a
 * command with no parameters (executed immediately). */
static uint8_t iw_esc_param_count(uint8_t cmd)
{
    switch (cmd) {
    case 'K': case 'a': case 'l': case 's': case 't':
    case '=': case '@': case 0x19:
        return 1U;
    case 'D': case 'T': case 'Z':
        return 2U;
    case 'L': case 'g': case 'u':
        return 3U;
    case 'G': case 'S': case 'F': case 'H': case 'C':
    case 'R':                              /* 3 digits + repeat char */
        return 4U;
    case 'V':                              /* 4 digits + column byte */
        return 5U;
    case 'U':                              /* 4 digits + 3 column bytes */
        return 7U;
    default:
        return 0U;
    }
}

static void iw_exec_immediate(imagewriter_t *iw, uint8_t cmd)
{
    switch (cmd) {
    case '!': iw->bold = 1U; break;
    case '"': iw->bold = 0U; break;
    case '$': iw->msb_pass = 1U; break;
    case 'X': iw->underline = 1U; break;
    case 'Y': iw->underline = 0U; break;
    case 'w': iw->halfheight = 1U; break;
    case 'W': iw->halfheight = 0U; break;
    case 'x': iw->script = 1U; break;
    case 'y': iw->script = 2U; break;
    case 'z': iw->script = 0U; break;
    case 'f': iw->reverse_feed = 0U; break;
    case 'r': iw->reverse_feed = 1U; break;
    case 'A': iw->line_spacing = 24; break;          /* 1/6 inch */
    case 'B': iw->line_spacing = 18; break;          /* 1/8 inch */
    case 'n': iw_set_pitch(iw, 320, 40); break;      /* 9 cpi,  72 dpi */
    case 'N': iw_set_pitch(iw, 288, 36); break;      /* 10 cpi, 80 dpi */
    case 'E': iw_set_pitch(iw, 240, 30); break;      /* 12 cpi, 96 dpi */
    case 'e': iw_set_pitch(iw, 215, 27); break;      /* 13.4 cpi, 107 dpi */
    case 'q': iw_set_pitch(iw, 192, 24); break;      /* 15 cpi, 120 dpi */
    case 'Q': iw_set_pitch(iw, 169, 21); break;      /* 17 cpi, 136 dpi */
    case 'p': iw_set_pitch(iw, 240, 20); break;      /* proportional, 144 dpi */
    case 'P': iw_set_pitch(iw, 216, 18); break;      /* proportional, 160 dpi */
    case 'c':
        /* Initialize printer: state only, paper stays where it is. */
        iw->left_margin = 0;
        iw->line_spacing = 24;
        iw_set_pitch(iw, 288, 36);
        iw->bold = 0U;
        iw->underline = 0U;
        iw->halfheight = 0U;
        iw->script = 0U;
        iw->doublewide = 0U;
        iw->doublewide_line = 0U;
        iw->reverse_feed = 0U;
        iw->msb_pass = 0U;
        iw->ink_mask = IW_INK_BLACK;
        iw->x = 0;
        break;
    default:
        /* 0/1-6/</>/?/O/o/k/m/M/'/I/+/-/... : tabs, print direction,
         * identity, and LQ font control -- consumed without effect. */
        break;
    }
}

static void iw_exec_params(imagewriter_t *iw)
{
    switch (iw->esc_cmd) {
    case 'G':
    case 'S':
        iw->graphics_remaining = iw_param_digits(iw, 4U);
        break;
    case 'C':
        /* LQ hi-res graphics: consume the data so it cannot print. */
        iw->graphics_remaining = 0U;
        break;
    case 'g':
        iw->graphics_remaining = iw_param_digits(iw, 3U) * 8U;
        break;
    case 'V': {
        uint32_t count = iw_param_digits(iw, 4U);
        while (count-- > 0U) {
            iw_draw_graphics_column(iw, iw->params[4]);
        }
        break;
    }
    case 'R': {
        uint32_t count = iw_param_digits(iw, 3U);
        uint8_t ch = iw->params[3];
        if (iw->msb_pass == 0U) {
            ch &= 0x7FU;
        }
        while (count-- > 0U) {
            iw_draw_char(iw, ch);
        }
        break;
    }
    case 'F':
        iw->x = iw->left_margin +
                (int32_t)iw_param_digits(iw, 4U) * iw->dot_advance;
        if (iw->x > IW_X_LIMIT) {
            iw->x = IW_X_LIMIT;
        }
        break;
    case 'L':
        iw->left_margin = (int32_t)iw_param_digits(iw, 3U) * iw->char_advance;
        if (iw->left_margin > IW_X_LIMIT) {
            iw->left_margin = IW_X_LIMIT;
        }
        if (iw->x < iw->left_margin) {
            iw->x = iw->left_margin;
        }
        break;
    case 'T': {
        int32_t spacing = (int32_t)iw_param_digits(iw, 2U);
        if (spacing > 0) {
            iw->line_spacing = spacing;
        }
        break;
    }
    case 'H': {
        int32_t len = (int32_t)iw_param_digits(iw, 4U);
        if (len > 0) {
            iw->page_len = (len > IW_PAGE_HEIGHT) ? IW_PAGE_HEIGHT : len;
        }
        break;
    }
    case 'K':
        /* Four-color ribbon selection. The three secondary colors use the
         * same two-pass combinations as the printer. A later strike at the
         * same dot ORs into the canvas and mixes in the same way. */
        switch (iw->params[0]) {
        case '0': iw->ink_mask = IW_INK_BLACK; break;
        case '1': iw->ink_mask = IW_INK_YELLOW; break;
        case '2': iw->ink_mask = IW_INK_MAGENTA; break;
        case '3': iw->ink_mask = IW_INK_CYAN; break;
        case '4': iw->ink_mask = IW_INK_YELLOW | IW_INK_MAGENTA; break;
        case '5': iw->ink_mask = IW_INK_YELLOW | IW_INK_CYAN; break;
        case '6': iw->ink_mask = IW_INK_MAGENTA | IW_INK_CYAN; break;
        default: break;
        }
        break;
    case 0x19:      /* cut-sheet feeder control */
    case 'a': case 'l': case 's': case 't': case '=': case '@':
    case 'D': case 'Z': case 'u': case 'U':
    default:
        break;
    }
}

static void iw_feed_byte(imagewriter_t *iw, uint8_t ch)
{
    if (iw->graphics_remaining > 0U) {
        iw->graphics_remaining--;
        iw_draw_graphics_column(iw, ch);
        return;
    }

    switch (iw->esc_state) {
    case IW_ESC_CMD:
        if (ch == '(' || ch == ')') {
            iw->esc_state = IW_ESC_TABLIST;
            return;
        }
        iw->params_needed = iw_esc_param_count(ch);
        if (iw->params_needed == 0U) {
            iw->esc_state = IW_ESC_NONE;
            iw_exec_immediate(iw, ch);
        } else {
            iw->esc_cmd = ch;
            iw->params_have = 0U;
            iw->esc_state = IW_ESC_PARAMS;
        }
        return;

    case IW_ESC_PARAMS:
        iw->params[iw->params_have++] = ch;
        if (iw->params_have >= iw->params_needed) {
            iw->esc_state = IW_ESC_NONE;
            iw_exec_params(iw);
        }
        return;

    case IW_ESC_TABLIST:
        /* "nnn,nnn,nnn." -- the terminator is the period. */
        if (ch == '.' || (ch != ',' && (ch < '0' || ch > '9'))) {
            iw->esc_state = IW_ESC_NONE;
        }
        return;

    case IW_ESC_NONE:
    default:
        break;
    }

    if (iw->msb_pass == 0U) {
        ch &= 0x7FU;
    }

    if (ch >= 0x20U && ch != 0x7FU) {
        iw_draw_char(iw, ch);
        return;
    }

    switch (ch) {
    case 0x08:      /* BS */
        iw->x -= iw_effective_advance(iw);
        if (iw->x < iw->left_margin) {
            iw->x = iw->left_margin;
        }
        break;
    case 0x09: {    /* HT: default stops every 8 columns */
        const int32_t adv = iw->char_advance;
        int32_t cols = (iw->x - iw->left_margin) / adv;
        cols = ((cols / 8) + 1) * 8;
        iw->x = iw->left_margin + cols * adv;
        if (iw->x > IW_X_LIMIT) {
            iw->x = iw->left_margin;
            iw_line_feed(iw);
        }
        break;
    }
    case 0x0A:      /* LF */
    case 0x0B:      /* VT */
        iw_line_feed(iw);
        break;
    case 0x0C:      /* FF */
        if (iw->page_dirty != 0U) {
            iw_emit_page(iw);
        }
        iw->y = 0;
        break;
    case 0x0D:      /* CR */
        iw->x = iw->left_margin;
        iw->doublewide_line = 0U;
        break;
    case 0x0E:      /* SO: double width for this line */
        iw->doublewide_line = 1U;
        break;
    case 0x0F:      /* SI */
    case 0x14:      /* DC4 */
        iw->doublewide_line = 0U;
        iw->doublewide = 0U;
        break;
    case 0x1B:
        iw->esc_state = IW_ESC_CMD;
        break;
    case 0x1F:      /* US n: feed 1-15 lines */
        iw->esc_cmd = 0x1FU;
        iw->params_needed = 1U;
        iw->params_have = 0U;
        iw->esc_state = IW_ESC_PARAMS;
        break;
    default:
        /* NUL/BEL/DC1/DC2/DC3/CAN and the rest: no effect on paper. */
        break;
    }
}

void imagewriter_feed(imagewriter_t *iw, const uint8_t *data, uint32_t len)
{
    uint32_t i;

    for (i = 0U; i < len; ++i) {
        /* US collects its count through the parameter machinery. */
        if (iw->esc_state == IW_ESC_PARAMS && iw->esc_cmd == 0x1FU) {
            uint32_t lines = (uint32_t)(data[i] & 0x0FU);
            iw->esc_state = IW_ESC_NONE;
            while (lines-- > 0U) {
                iw_line_feed(iw);
            }
            continue;
        }
        iw_feed_byte(iw, data[i]);
    }
}
