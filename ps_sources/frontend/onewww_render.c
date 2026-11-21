/* ONEWWW page renderer: lays a document tree out with proportional
 * fonts and draws it into Super Hi-Res rows. Blocks, lists, tables with
 * automatic column widths, links.
 * No hardware access: the file also compiles on the host. */
#include "onewww_render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http_core.h"
#include "onewww_dom.h"
#include "onewww_font.h"

#define ROW_BYTES   160U
#define PAGE_WIDTH  640
#define MARGIN      12
#define ROW_CHUNK   512U
#define MAX_ROWS    60000U

/* Page palettes. A 640-mode row uses colours 0..3 of its palette; the
 * four colour groups of the 640 mode hold the same four colours.
 * A plain text row has paper, two greys and ink: four levels for smooth
 * letter edges. A row with link text (PAL_LINK) has paper, one grey, ink
 * and the link colour. */
enum { PAL_TEXT = 0, PAL_SHADE, PAL_BANNER, PAL_PICTURE, PAL_BASES };
#define PAL_LINK 0x80U
enum { LV_LIGHT = 1, LV_DARK = 2, LV_INK = 3 };

static const uint16_t s_text_palettes[3][2][4] = {
    { { 0x0FFF, 0x0AAA, 0x0555, 0x0000 }, { 0x0FFF, 0x0888, 0x0000, 0x002C } },
    { { 0x0DDD, 0x0999, 0x0444, 0x0000 }, { 0x0DDD, 0x0777, 0x0000, 0x002C } },
    { { 0x0137, 0x067A, 0x0ABD, 0x0FFF }, { 0x0137, 0x067A, 0x0ABD, 0x0FFF } },
};
static const uint16_t s_picture_palette[16] = {
    0x0000, 0x0113, 0x0226, 0x034A, 0x047D, 0x07AF, 0x0FFF, 0x0FE8,
    0x0FB3, 0x0F71, 0x0C32, 0x0713, 0x0141, 0x0272, 0x04A4, 0x0888,
};

typedef struct {
    uint8_t *px;
    uint8_t *scb;       /* 0x80 = 640 mode, 0 = 320 mode */
    uint8_t *pal;
    uint32_t rows;
    uint32_t cap;
    uint8_t fixed;
} surface_t;

static surface_t s_page;
static uint8_t s_status_px[ONEWWW_STATUS_ROWS * ROW_BYTES];
static uint8_t s_status_scb[ONEWWW_STATUS_ROWS];
static uint8_t s_status_pal[ONEWWW_STATUS_ROWS];

static int surface_need(surface_t *s, uint32_t rows)
{
    uint32_t cap;
    uint8_t *px, *scb, *pal;

    if (rows <= s->rows) return 0;
    if (rows > s->cap) {
        if (s->fixed || rows > MAX_ROWS) return -1;
        cap = (rows + ROW_CHUNK) & ~(ROW_CHUNK - 1U);
        px = realloc(s->px, (size_t)cap * ROW_BYTES);
        if (px == NULL) return -1;
        s->px = px;
        scb = realloc(s->scb, cap);
        if (scb == NULL) return -1;
        s->scb = scb;
        pal = realloc(s->pal, cap);
        if (pal == NULL) return -1;
        s->pal = pal;
        s->cap = cap;
    }
    memset(s->px + (size_t)s->rows * ROW_BYTES, 0,
           (size_t)(rows - s->rows) * ROW_BYTES);
    memset(s->scb + s->rows, 0x80, rows - s->rows);
    memset(s->pal + s->rows, PAL_TEXT, rows - s->rows);
    s->rows = rows;
    return 0;
}

static void put640(surface_t *s, int x, int y, uint8_t colour)
{
    uint8_t *p;
    unsigned shift;

    if (x < 0 || x >= PAGE_WIDTH || y < 0 || (uint32_t)y >= s->rows) return;
    p = s->px + (size_t)y * ROW_BYTES + ((unsigned)x >> 2);
    shift = 6U - 2U * ((unsigned)x & 3U);
    *p = (uint8_t)((*p & ~(3U << shift)) | ((unsigned)colour << shift));
}

/* Draws one pixel at a coverage level 1..3 in the palette of its row. */
static void put_level(surface_t *s, int x, int y, unsigned level, int link)
{
    if (y < 0 || (uint32_t)y >= s->rows) return;
    if ((s->pal[y] & PAL_LINK) != 0U) {
        level = level == LV_INK ? (link ? 3U : 2U) : 1U;
    }
    put640(s, x, y, (uint8_t)level);
}

static void hline(surface_t *s, int x0, int x1, int y, unsigned level,
                  int link)
{
    for (int x = x0; x < x1; x++) put_level(s, x, y, level, link);
}

static void vline(surface_t *s, int x, int y0, int y1, unsigned level)
{
    for (int y = y0; y < y1; y++) put_level(s, x, y, level, 0);
}

static void set_palette(surface_t *s, int y0, int y1, uint8_t base)
{
    for (int y = y0; y < y1; y++) {
        if (y >= 0 && (uint32_t)y < s->rows) {
            s->pal[y] = (uint8_t)(base | (base == PAL_BANNER ? 0U :
                                          (s->pal[y] & PAL_LINK)));
        }
    }
}

/* Changes plain text rows to the link palette and keeps their pixels:
 * the two greys become the one grey, ink stays ink. */
static void rows_link(surface_t *s, int y0, int y1)
{
    static const uint8_t map[4] = { 0U, 1U, 1U, 2U };

    for (int y = y0; y < y1; y++) {
        uint8_t *row;
        if (y < 0 || (uint32_t)y >= s->rows || s->scb[y] != 0x80U ||
            s->pal[y] == PAL_BANNER || (s->pal[y] & PAL_LINK) != 0U) continue;
        row = s->px + (size_t)y * ROW_BYTES;
        for (unsigned i = 0; i < ROW_BYTES; i++) {
            const uint8_t v = row[i];
            if (v != 0U) {
                row[i] = (uint8_t)((map[v >> 6] << 6) |
                                   (map[(v >> 4) & 3U] << 4) |
                                   (map[(v >> 2) & 3U] << 2) | map[v & 3U]);
            }
        }
        s->pal[y] |= PAL_LINK;
    }
}

static const onewww_glyph_t *glyph_find(const onewww_font_t *f, uint16_t code)
{
    int lo = 0, hi = (int)f->count - 1;

    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (f->glyphs[mid].code == code) return &f->glyphs[mid];
        if (f->glyphs[mid].code < code) lo = mid + 1; else hi = mid - 1;
    }
    /* The body face has the most characters. */
    if (f != &onewww_fonts[ONEWWW_FONT_BODY]) {
        return glyph_find(&onewww_fonts[ONEWWW_FONT_BODY], code);
    }
    return code == '?' ? &f->glyphs[0] : glyph_find(f, '?');
}

static int glyph_draw(surface_t *s, const onewww_font_t *f, uint16_t code,
                      int x, int top, int link)
{
    const onewww_glyph_t *g = glyph_find(f, code);
    const uint8_t *bits = f->bits + g->offset;
    const unsigned stride = ((unsigned)g->w + 3U) >> 2;

    for (unsigned gy = 0; gy < g->h; gy++) {
        for (unsigned gx = 0; gx < g->w; gx++) {
            const unsigned level =
                (bits[gy * stride + (gx >> 2)] >> (6U - 2U * (gx & 3U))) & 3U;
            if (level != 0U) {
                put_level(s, x + g->x + (int)gx, top + g->y + (int)gy,
                          level, link);
            }
        }
    }
    return g->advance;
}

static int text_width(const onewww_font_t *f, const uint16_t *cp, unsigned n)
{
    int w = 0;
    while (n-- != 0U) w += glyph_find(f, *cp++)->advance;
    return w;
}

/* ---- Page data --------------------------------------------------------- */

typedef struct {
    int16_t x0, x1;
    uint32_t y;
    uint16_t h;
    uint32_t link;
} rect_t;

typedef struct {
    char *name;
    uint32_t row;
} anchor_t;

static struct {
    char **href;
    uint32_t *link_row;         /* first row of each link */
    uint32_t links, links_cap;
    rect_t *rect;
    uint32_t rects, rects_cap;
    anchor_t *anchor;
    uint32_t anchors, anchors_cap;
    char title[128];
    const char *base;
    uint8_t internal;
} s_doc;

static void *grow(void *array, uint32_t count, uint32_t *cap, size_t item)
{
    void *bigger;

    if (count < *cap) return array;
    bigger = realloc(array, (size_t)(*cap ? *cap * 2U : 64U) * item);
    if (bigger == NULL) return NULL;
    *cap = *cap ? *cap * 2U : 64U;
    return bigger;
}

static char *copy_text(const char *text)
{
    char *copy = malloc(strlen(text) + 1U);
    if (copy != NULL) strcpy(copy, text);
    return copy;
}

/* Returns the link number, or -1 when the href cannot be followed. */
static int link_add(const char *href)
{
    char url[HTTP_URL_MAX];
    char **list;
    uint32_t *rows, cap = s_doc.links_cap;

    if (href[0] == '#') {
        snprintf(url, sizeof(url), "%s", href);
    } else if (s_doc.base == NULL ||
               http_url_resolve(s_doc.base, href, url, sizeof(url)) != 0) {
        return -1;
    }
    list = grow(s_doc.href, s_doc.links, &cap, sizeof(char *));
    if (list == NULL) return -1;
    s_doc.href = list;
    rows = realloc(s_doc.link_row, (size_t)cap * sizeof(uint32_t));
    if (rows == NULL) return -1;
    s_doc.link_row = rows; s_doc.links_cap = cap;
    list[s_doc.links] = copy_text(url);
    if (list[s_doc.links] == NULL) return -1;
    rows[s_doc.links] = UINT32_MAX;
    return (int)s_doc.links++;
}

static void rect_add(int x0, int x1, int y, int h, uint32_t link)
{
    rect_t *list = grow(s_doc.rect, s_doc.rects, &s_doc.rects_cap, sizeof(rect_t));

    if (list == NULL) return;
    s_doc.rect = list;
    list[s_doc.rects].x0 = (int16_t)x0; list[s_doc.rects].x1 = (int16_t)x1;
    list[s_doc.rects].y = (uint32_t)y; list[s_doc.rects].h = (uint16_t)h;
    list[s_doc.rects].link = link;
    s_doc.rects++;
    if (s_doc.link_row[link] == UINT32_MAX) s_doc.link_row[link] = (uint32_t)y;
}

static void anchor_add(const char *name, int row)
{
    anchor_t *list;

    if (name[0] == '\0') return;
    list = grow(s_doc.anchor, s_doc.anchors, &s_doc.anchors_cap, sizeof(anchor_t));
    if (list == NULL) return;
    s_doc.anchor = list;
    list[s_doc.anchors].name = copy_text(name);
    if (list[s_doc.anchors].name == NULL) return;
    list[s_doc.anchors].row = (uint32_t)row;
    s_doc.anchors++;
}

static void doc_free(void)
{
    for (uint32_t i = 0; i < s_doc.links; i++) free(s_doc.href[i]);
    for (uint32_t i = 0; i < s_doc.anchors; i++) free(s_doc.anchor[i].name);
    free(s_doc.href); free(s_doc.link_row); free(s_doc.rect); free(s_doc.anchor);
    memset(&s_doc, 0, sizeof(s_doc));
}

/* ---- Inline layout ----------------------------------------------------- */

#define LINE_CODES 1024U
#define LINE_ITEMS 160U
#define MARKER_MAX 8U

typedef struct {
    uint16_t start, count;
    int16_t x, w;
    int32_t link;       /* -1: not a link */
    uint8_t font;
} item_t;

typedef struct {
    uint16_t cp[LINE_CODES];
    item_t item[LINE_ITEMS];
    unsigned codes, items;
    int width;          /* pen position from the line start */
    uint16_t word[128];
    unsigned word_len;
    uint8_t space;      /* a space waits before the next word */
} line_t;

/* Text properties that an element gives to its content. */
typedef struct {
    uint8_t bold, italic, mono, small, pre;
    uint8_t font_block;         /* heading font, or 0 */
    uint8_t list;               /* 1 = bullets, 2 = numbers */
    int *counter;               /* number of the next <li> */
    int32_t link;
} style_t;

/* One column of text: the page, or a table cell. */
typedef struct {
    surface_t *s;               /* NULL: measure only */
    line_t *line;
    int x0, w, y;
    int indent;
    int max_w;                  /* widest line */
    int max_word;               /* widest word */
    int depth;                  /* table nesting */
    uint16_t marker[MARKER_MAX];
    uint8_t marker_len;
    uint8_t centre, gap, top_level, row_pal;
} flow_t;

static const style_t s_plain = { 0, 0, 0, 0, 0, 0, 0, NULL, -1 };

static uint8_t style_font(const style_t *st)
{
    if (st->font_block != 0U) return st->font_block;
    if (st->mono) return ONEWWW_FONT_MONO;
    if (st->small) return ONEWWW_FONT_SMALL;
    return (uint8_t)(ONEWWW_FONT_BODY + (st->bold ? 1 : 0) + (st->italic ? 2 : 0));
}

static void line_reset(line_t *l)
{
    l->items = 0U; l->codes = 0U; l->width = 0; l->space = 0U;
}

static void flow_flush(flow_t *f, int force)
{
    line_t *l = f->line;
    int ascent = 0, descent = 0, shift = 0, top, height;

    if (l->items == 0U && !force) { l->space = 0U; return; }
    if (l->width + f->indent > f->max_w) f->max_w = l->width + f->indent;
    if (l->items == 0U) {
        ascent = onewww_fonts[ONEWWW_FONT_BODY].ascent;
        descent = onewww_fonts[ONEWWW_FONT_BODY].descent;
    }
    for (unsigned i = 0; i < l->items; i++) {
        const onewww_font_t *font = &onewww_fonts[l->item[i].font];
        if (font->ascent > ascent) ascent = font->ascent;
        if (font->descent > descent) descent = font->descent;
    }
    height = ascent + descent + 2;
    top = f->y + 1;
    f->gap = 0U;
    if (f->s == NULL || surface_need(f->s, (uint32_t)(f->y + height)) != 0) {
        f->y += height; f->marker_len = 0U;
        line_reset(l);
        return;
    }
    if (f->top_level && f->row_pal != PAL_TEXT) {
        set_palette(f->s, f->y, f->y + height, f->row_pal);
    }
    for (unsigned i = 0; i < l->items; i++) {
        if (l->item[i].link >= 0) { rows_link(f->s, f->y, f->y + height); break; }
    }
    if (f->centre) shift = (f->w - f->indent - l->width) / 2;
    if (shift < 0) shift = 0;
    if (f->marker_len != 0U) {
        const onewww_font_t *font = &onewww_fonts[ONEWWW_FONT_BODY];
        int x = f->x0 + f->indent - 6 -
                text_width(font, f->marker, f->marker_len);
        for (unsigned k = 0; k < f->marker_len; k++) {
            x += glyph_draw(f->s, font, f->marker[k], x,
                            top + ascent - font->ascent, 0);
        }
        f->marker_len = 0U;
    }
    for (unsigned i = 0; i < l->items; i++) {
        const item_t *it = &l->item[i];
        const onewww_font_t *font = &onewww_fonts[it->font];
        const int x_start = f->x0 + f->indent + shift + it->x;
        int x = x_start;

        for (unsigned k = 0; k < it->count; k++) {
            x += glyph_draw(f->s, font, l->cp[it->start + k], x,
                            top + ascent - font->ascent, it->link >= 0);
        }
        if (it->link >= 0) {
            hline(f->s, x_start, x_start + it->w, top + ascent + 2, LV_INK, 1);
            rect_add(x_start, x_start + it->w, f->y, height, (uint32_t)it->link);
        }
    }
    f->y += height;
    line_reset(l);
}

static void flow_word_end(flow_t *f, const style_t *st)
{
    line_t *l = f->line;
    const uint8_t font_id = style_font(st);
    const onewww_font_t *font = &onewww_fonts[font_id];
    const int avail = f->w - f->indent;
    int space_w, w;
    item_t *it;

    if (l->word_len == 0U) return;
    w = text_width(font, l->word, l->word_len);
    if (w > f->max_word) f->max_word = w;
    space_w = (l->space && l->items != 0U) ? glyph_find(font, ' ')->advance : 0;
    if (l->items != 0U && l->width + space_w + w > avail) {
        flow_flush(f, 0);
        space_w = 0;
    }
    if (l->items >= LINE_ITEMS || l->codes + l->word_len > LINE_CODES) {
        flow_flush(f, 0);
        space_w = 0;
    }
    it = &l->item[l->items++];
    it->start = (uint16_t)l->codes;
    it->count = (uint16_t)l->word_len;
    memcpy(&l->cp[l->codes], l->word, l->word_len * sizeof(uint16_t));
    l->codes += l->word_len;
    it->font = font_id;
    it->link = st->link;
    /* A link underline also covers the space between its words. */
    if (it->link >= 0 && space_w != 0 && l->items > 1U &&
        l->item[l->items - 2U].link == it->link) {
        l->item[l->items - 2U].w = (int16_t)(l->item[l->items - 2U].w + space_w);
    }
    it->x = (int16_t)(l->width + space_w);
    it->w = (int16_t)w;
    l->width += space_w + w;
    l->word_len = 0U;
    l->space = 0U;
}

static void flow_char(flow_t *f, const style_t *st, uint16_t code)
{
    line_t *l = f->line;

    if (st->pre) {
        if (code == '\n') { flow_word_end(f, st); flow_flush(f, 1); return; }
        if (code == '\r') return;
        if (code == '\t') code = ' ';
    } else if (code == ' ' || code == '\n' || code == '\t' || code == '\r' ||
               code == '\f') {
        flow_word_end(f, st);
        l->space = 1U;
        return;
    }
    /* Soft hyphen, zero-width characters and marks, byte order mark. */
    if (code == 0xAD || (code >= 0x200B && code <= 0x200F) || code == 0xFEFF ||
        code == 0x2060) return;
    if (code == 0xA0 || code == 0x2009 || code == 0x202F || code == 0x2002 ||
        code == 0x2003) code = ' ';
    if (l->word_len == sizeof(l->word) / sizeof(l->word[0])) flow_word_end(f, st);
    l->word[l->word_len++] = code;
    if (st->pre && code == ' ') flow_word_end(f, st);
}

static void flow_text(flow_t *f, const style_t *st, const char *text,
                      size_t length)
{
    const unsigned char *p = (const unsigned char *)text, *end = p + length;

    while (p < end) {
        uint32_t code = *p++;
        int more = code >= 0xF0 ? 3 : code >= 0xE0 ? 2 : code >= 0xC0 ? 1 : 0;

        if (more != 0) code &= 0x3FU >> more;
        while (more-- > 0 && p < end && (*p & 0xC0) == 0x80) {
            code = (code << 6) | (*p++ & 0x3FU);
        }
        flow_char(f, st, code > 0xFFFFU ? '?' : (uint16_t)code);
    }
}

static void flow_break(flow_t *f, const style_t *st, int gap)
{
    flow_word_end(f, st);
    flow_flush(f, 0);
    if (gap != 0 && !f->gap) {
        f->y += gap;
        if (f->s != NULL) (void)surface_need(f->s, (uint32_t)f->y);
        f->gap = 1U;
    }
}

/* ---- Picture ----------------------------------------------------------- */

static void picture_rgb(int x, int y, int h, int *r, int *g, int *b)
{
    /* x is 0..319 (two screen pixels each), y is 0..h-1. */
    const int sx = 2 * x - 440, sy = y - (h * 5) / 9;
    const int hill = (h * 7) / 10 + (((x * 7) % 61) - 30) / 6 +
                     ((x - 160) * (x - 160)) / 1400;
    const int t = (y * 255) / h;

    if (x >= 8 && x < 8 + 16 * 6 && y >= h - 14 && y < h - 4) {
        const uint16_t c = s_picture_palette[(x - 8) / 6];
        *r = ((c >> 8) & 15) * 17; *g = ((c >> 4) & 15) * 17; *b = (c & 15) * 17;
        return;
    }
    if (y >= hill) {
        const int d = y - hill;
        *r = 20 + d; *g = 70 + 3 * d; *b = 20 + d;
        return;
    }
    if (sx * sx + sy * sy < 34 * 34) { *r = 255; *g = 240; *b = 150; return; }
    if (t < 128) {
        *r = 34 + (t * 85) / 128; *g = 34 + (t * 136) / 128;
        *b = 102 + (t * 153) / 128;
    } else {
        *r = 119 + ((t - 128) * 136) / 128; *g = 170 + ((t - 128) * 68) / 128;
        *b = 255 - ((t - 128) * 119) / 128;
    }
}

static void flow_picture(flow_t *f, int h)
{
    static int16_t err[2][322][3];
    surface_t *s = f->s;
    const int y0 = f->y;

    if (s == NULL || surface_need(s, (uint32_t)(y0 + h)) != 0) return;
    memset(err, 0, sizeof(err));
    for (int y = 0; y < h; y++) {
        int16_t (*cur)[3] = err[y & 1], (*next)[3] = err[(y + 1) & 1];
        uint8_t *row = s->px + (size_t)(y0 + y) * ROW_BYTES;

        memset(next, 0, sizeof(err[0]));
        s->scb[y0 + y] = 0x00;
        s->pal[y0 + y] = PAL_PICTURE;
        for (int x = 0; x < 320; x++) {
            int c[3], best = 0, best_d = 1 << 30;

            picture_rgb(x, y, h, &c[0], &c[1], &c[2]);
            for (int k = 0; k < 3; k++) {
                c[k] += cur[x + 1][k] / 16;
                if (c[k] < 0) c[k] = 0;
                if (c[k] > 255) c[k] = 255;
            }
            for (int i = 0; i < 16; i++) {
                const uint16_t p = s_picture_palette[i];
                const int dr = c[0] - ((p >> 8) & 15) * 17;
                const int dg = c[1] - ((p >> 4) & 15) * 17;
                const int db = c[2] - (p & 15) * 17;
                const int d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
                if (d < best_d) { best_d = d; best = i; }
            }
            for (int k = 0; k < 3; k++) {
                const uint16_t p = s_picture_palette[best];
                const int q = c[k] - ((p >> (8 - 4 * k)) & 15) * 17;
                cur[x + 2][k] = (int16_t)(cur[x + 2][k] + q * 7);
                next[x][k] = (int16_t)(next[x][k] + q * 3);
                next[x + 1][k] = (int16_t)(next[x + 1][k] + q * 5);
                next[x + 2][k] = (int16_t)(next[x + 2][k] + q);
            }
            if ((x & 1) == 0) row[x >> 1] = (uint8_t)(best << 4);
            else row[x >> 1] |= (uint8_t)best;
        }
    }
    f->y += h;
}

/* ---- Document walk ----------------------------------------------------- */

#define MAX_COLS       24
#define MAX_TABLE_DEPTH 8
#define CELL_PAD       5

static void walk_children(flow_t *f, const onewww_node_t *parent, style_t st);
static void walk_table(flow_t *f, const onewww_node_t *table);

static int attr_number(const onewww_node_t *n, const char *name, int fallback)
{
    char text[16];
    int value = 0, i = 0;

    if (onewww_attr(n, name, text, sizeof(text)) <= 0) return fallback;
    while (text[i] >= '0' && text[i] <= '9' && value < 10000) {
        value = value * 10 + (text[i++] - '0');
    }
    return i != 0 ? value : fallback;
}

static int text_has(const char *hay, size_t n, const char *needle)
{
    const size_t m = strlen(needle);

    for (size_t i = 0; i + m <= n; i++) {
        if (memcmp(hay + i, needle, m) == 0) return 1;
    }
    return 0;
}

static int is_hidden(const onewww_node_t *n)
{
    char text[256];
    int length;

    if (n->length == 0U) return 0;
    if (text_has(n->text, n->length, "hidden") &&
        (onewww_attr(n, "hidden", text, sizeof(text)) >= 0 ||
         (onewww_attr(n, "aria-hidden", text, sizeof(text)) > 0 &&
          strcmp(text, "true") == 0 && n->tag != OW_SPAN && n->tag != OW_I))) {
        return 1;
    }
    if (!text_has(n->text, n->length, "display")) return 0;
    length = onewww_attr(n, "style", text, sizeof(text));
    if (length <= 0) return 0;
    for (int i = 0, k = 0; i <= length; i++) {       /* remove the spaces */
        if (text[i] != ' ') text[k++] = text[i];
    }
    return strstr(text, "display:none") != NULL;
}

static void flow_literal(flow_t *f, const style_t *st, const char *text)
{
    flow_text(f, st, text, strlen(text));
}

static void set_marker(flow_t *f, const style_t *st)
{
    if (st->list == 2U && st->counter != NULL) {
        char text[MARKER_MAX];
        const int n = snprintf(text, sizeof(text), "%d.", (*st->counter)++);
        f->marker_len = 0U;
        for (int i = 0; i < n && i < (int)MARKER_MAX; i++) {
            f->marker[f->marker_len++] = (uint8_t)text[i];
        }
    } else {
        f->marker[0] = 0x2022; f->marker_len = 1U;
    }
}

static void walk_block(flow_t *f, const onewww_node_t *n, style_t st,
                       int gap, int indent)
{
    flow_break(f, &st, gap);
    f->indent += indent;
    walk_children(f, n, st);
    flow_break(f, &st, gap);
    f->indent -= indent;
}

static void walk_node(flow_t *f, const onewww_node_t *n, style_t st)
{
    /* Each use ends before the walk goes deeper: one buffer is sufficient
     * and keeps the stack frames small. */
    static char text[HTTP_URL_MAX];

    if (n->tag == OW_TEXT) { flow_text(f, &st, n->text, n->length); return; }
    if (is_hidden(n)) return;
    if (f->s != NULL && n->length != 0U &&
        (text_has(n->text, n->length, "id") || n->tag == OW_A)) {
        if (onewww_attr(n, "id", text, sizeof(text)) > 0) anchor_add(text, f->y);
        if (n->tag == OW_A && onewww_attr(n, "name", text, sizeof(text)) > 0) {
            anchor_add(text, f->y);
        }
    }
    switch (n->tag) {
    case OW_SCRIPT: case OW_STYLE: case OW_HEAD: case OW_TITLE: case OW_META:
    case OW_LINK: case OW_BASE: case OW_TEMPLATE: case OW_SVG: case OW_IFRAME:
    case OW_SELECT: case OW_OPTION: case OW_TEXTAREA: case OW_OBJECT:
    case OW_VIDEO: case OW_AUDIO: case OW_CANVAS: case OW_MAP: case OW_COL:
    case OW_COLGROUP: case OW_AREA: case OW_EMBED: case OW_SOURCE: case OW_TRACK:
        return;
    case OW_BR:
        flow_word_end(f, &st); flow_flush(f, 1);
        return;
    case OW_WBR:
        flow_word_end(f, &st);
        return;
    case OW_HR:
        flow_break(f, &st, 6);
        if (f->s != NULL && surface_need(f->s, (uint32_t)f->y + 1U) == 0) {
            hline(f->s, f->x0 + f->indent, f->x0 + f->w, f->y, LV_LIGHT, 0);
        }
        f->y += 1; f->gap = 0U;
        flow_break(f, &st, 6);
        return;
    case OW_IMG:
        if (s_doc.internal && f->top_level && f->s != NULL &&
            onewww_attr(n, "src", text, sizeof(text)) > 0 &&
            strcmp(text, "builtin") == 0) {
            flow_break(f, &st, 8);
            flow_picture(f, 120);
            f->gap = 0U;
            flow_break(f, &st, 8);
            return;
        }
        st.small = 1U; st.font_block = 0U; st.mono = 0U;
        if (onewww_attr(n, "alt", text, 96U) > 0) {
            flow_literal(f, &st, "["); flow_literal(f, &st, text);
            flow_literal(f, &st, "]");
        } else if (st.link >= 0) {
            flow_literal(f, &st, "[image]");
        }
        flow_word_end(f, &st);
        return;
    case OW_INPUT: {
        char type[16] = "text";
        (void)onewww_attr(n, "type", type, sizeof(type));
        if (strcmp(type, "hidden") == 0) return;
        if (onewww_attr(n, "value", text, 64U) < 0) text[0] = '\0';
        flow_word_end(f, &st);
        if (strcmp(type, "submit") == 0 || strcmp(type, "button") == 0 ||
            strcmp(type, "reset") == 0) {
            st.bold = 1U;
            flow_literal(f, &st, "[");
            flow_literal(f, &st, text[0] != '\0' ? text : "Send");
            flow_literal(f, &st, "]");
        } else if (strcmp(type, "checkbox") == 0 || strcmp(type, "radio") == 0) {
            /* Not shown before forms work: pages hide many of them. */
        } else {
            flow_literal(f, &st, "[");
            flow_literal(f, &st, text[0] != '\0' ? text :
                         "\xC2\xA0\xC2\xA0\xC2\xA0\xC2\xA0\xC2\xA0\xC2\xA0\xC2\xA0\xC2\xA0");
            flow_literal(f, &st, "]");
        }
        flow_word_end(f, &st);
        return;
    }
    case OW_BUTTON:
        st.bold = 1U;
        flow_literal(f, &st, "[");
        walk_children(f, n, st);
        flow_literal(f, &st, "]");
        flow_word_end(f, &st);
        return;
    case OW_B: case OW_STRONG:
        flow_word_end(f, &st); st.bold = 1U;
        walk_children(f, n, st); flow_word_end(f, &st);
        return;
    case OW_I: case OW_EM: case OW_CITE: case OW_VAR: case OW_DFN:
        flow_word_end(f, &st); st.italic = 1U;
        walk_children(f, n, st); flow_word_end(f, &st);
        return;
    case OW_CODE: case OW_TT: case OW_KBD: case OW_SAMP:
        flow_word_end(f, &st); st.mono = 1U;
        walk_children(f, n, st); flow_word_end(f, &st);
        return;
    case OW_SMALL: case OW_SUB: case OW_SUP:
        flow_word_end(f, &st); st.small = 1U;
        walk_children(f, n, st); flow_word_end(f, &st);
        return;
    case OW_A:
        flow_word_end(f, &st);
        if (onewww_attr(n, "href", text, sizeof(text)) > 0) {
            /* A measure pass needs only to know that this is a link. */
            st.link = f->s != NULL ? link_add(text) : 0;
        }
        walk_children(f, n, st); flow_word_end(f, &st);
        return;
    case OW_P: case OW_BLOCKQUOTE: case OW_FIGURE: case OW_DL:
        walk_block(f, n, st, 8, n->tag == OW_BLOCKQUOTE || n->tag == OW_FIGURE ? 24 : 0);
        return;
    case OW_DD:
        walk_block(f, n, st, 0, 24);
        return;
    case OW_PRE:
        st.pre = 1U; st.mono = 1U;
        walk_block(f, n, st, 8, 0);
        return;
    case OW_CENTER: {
        const uint8_t before = f->centre;
        flow_break(f, &st, 0);
        f->centre = 1U;
        walk_children(f, n, st);
        flow_break(f, &st, 0);
        f->centre = before;
        return;
    }
    case OW_H1:
        if (s_doc.internal && f->top_level && f->s != NULL) {
            flow_break(f, &st, 10);
            (void)surface_need(f->s, (uint32_t)f->y + 6U);
            set_palette(f->s, f->y, f->y + 6, PAL_BANNER);
            f->y += 6; f->row_pal = PAL_BANNER; f->centre = 1U;
            st.font_block = ONEWWW_FONT_H1;
            walk_children(f, n, st);
            flow_break(f, &st, 0);
            (void)surface_need(f->s, (uint32_t)f->y + 6U);
            set_palette(f->s, f->y, f->y + 6, PAL_BANNER);
            f->y += 6; f->row_pal = PAL_TEXT; f->centre = 0U; f->gap = 0U;
            flow_break(f, &st, 10);
            return;
        }
        st.font_block = ONEWWW_FONT_H1;
        walk_block(f, n, st, 12, 0);
        return;
    case OW_H2:
        st.font_block = ONEWWW_FONT_H2;
        walk_block(f, n, st, 10, 0);
        return;
    case OW_H3: case OW_H4: case OW_H5: case OW_H6: case OW_DT:
    case OW_CAPTION: case OW_LEGEND: case OW_SUMMARY:
        st.bold = 1U;
        walk_block(f, n, st, n->tag == OW_DT ? 0 : 8, 0);
        return;
    case OW_UL: case OW_OL: {
        int counter = attr_number(n, "start", 1);
        st.list = n->tag == OW_OL ? 2U : 1U;
        st.counter = &counter;
        walk_block(f, n, st, st.list != 0U && f->indent != 0 ? 0 : 6, 24);
        return;
    }
    case OW_LI:
        flow_break(f, &st, 0);
        if (f->indent == 0) {           /* an <li> without a list */
            f->indent = 24;
            set_marker(f, &st);
            walk_children(f, n, st);
            flow_break(f, &st, 0);
            f->indent = 0; f->marker_len = 0U;
            return;
        }
        set_marker(f, &st);
        st.list = 0U; st.counter = NULL;
        walk_children(f, n, st);
        flow_break(f, &st, 0);
        f->marker_len = 0U;
        return;
    case OW_TABLE:
        flow_break(f, &st, 6);
        walk_table(f, n);
        f->gap = 0U;
        flow_break(f, &st, 6);
        return;
    case OW_TR: case OW_TD: case OW_TH: case OW_DIV: case OW_SECTION:
    case OW_ARTICLE: case OW_ASIDE: case OW_NAV: case OW_HEADER: case OW_FOOTER:
    case OW_MAIN: case OW_FORM: case OW_FIELDSET: case OW_DETAILS:
    case OW_HGROUP: case OW_ADDRESS: case OW_FIGCAPTION: case OW_NOSCRIPT:
        walk_block(f, n, st, 0, 0);
        return;
    default:
        walk_children(f, n, st);
        return;
    }
}

static void walk_children(flow_t *f, const onewww_node_t *parent, style_t st)
{
    for (const onewww_node_t *c = parent->first; c != NULL; c = c->next) {
        walk_node(f, c, st);
    }
}

/* ---- Tables ------------------------------------------------------------ */

typedef struct {
    int busy[MAX_COLS];         /* rows that a rowspan still covers */
    int col;
} grid_t;

/* Returns the first column of a cell and steps past it, or -1. */
static int grid_place(grid_t *g, int *span, int rowspan)
{
    int start;

    while (g->col < MAX_COLS && g->busy[g->col] > 0) g->col++;
    if (g->col >= MAX_COLS) return -1;
    start = g->col;
    if (*span < 1) *span = 1;
    if (start + *span > MAX_COLS) *span = MAX_COLS - start;
    for (int i = 0; i < *span; i++) g->busy[start + i] = rowspan > 1 ? rowspan : 0;
    g->col += *span;
    return start;
}

static int grid_row_end(grid_t *g)
{
    int used = g->col;

    for (int i = 0; i < MAX_COLS; i++) {
        if (g->busy[i] > 0) { g->busy[i]--; if (i + 1 > used) used = i + 1; }
    }
    g->col = 0;
    return used;
}

static int is_row_group(uint16_t t)
{ return t == OW_THEAD || t == OW_TBODY || t == OW_TFOOT; }

/* Next row of a table in document order, or NULL. */
static const onewww_node_t *row_next(const onewww_node_t *table,
                                     const onewww_node_t *row)
{
    const onewww_node_t *n = row == NULL ? table->first : row->next;
    const onewww_node_t *group = row != NULL && row->parent != table ?
                                 row->parent : NULL;

    for (;;) {
        for (; n != NULL; n = n->next) {
            if (n->tag == OW_TR) return n;
            if (group == NULL && is_row_group(n->tag) && n->first != NULL) {
                group = n; n = n->first;
                if (n->tag == OW_TR) return n;
            }
        }
        if (group == NULL) return NULL;
        n = group->next; group = NULL;
    }
}

/* Lays one cell out. Returns its end row; gives its widths. */
static int cell_run(flow_t *parent, const onewww_node_t *cell, surface_t *s,
                    int x0, int w, int y, int *max_w, int *max_word)
{
    flow_t f;
    style_t st = s_plain;
    line_t *line = malloc(sizeof(*line));

    if (line == NULL) return y;
    memset(&f, 0, sizeof(f));
    memset(line, 0, sizeof(*line));
    f.s = s; f.line = line; f.x0 = x0; f.w = w; f.y = y;
    f.depth = parent->depth + 1; f.gap = 1U; f.row_pal = PAL_TEXT;
    if (cell->tag == OW_TH) st.bold = 1U;
    walk_children(&f, cell, st);
    flow_break(&f, &st, 0);
    if (max_w != NULL) *max_w = f.max_w;
    if (max_word != NULL) *max_word = f.max_word;
    free(line);
    return f.y;
}

static void walk_table(flow_t *f, const onewww_node_t *table)
{
    const int x0 = f->x0 + f->indent, avail = f->w - f->indent;
    int pref[MAX_COLS] = { 0 }, least[MAX_COLS] = { 0 }, width[MAX_COLS];
    int columns = 0, total = 0, total_least = 0, y = f->y, border;
    const onewww_node_t *row, *c;
    grid_t grid;
    style_t st = s_plain;

    /* A caption is a block before the grid. */
    for (c = table->first; c != NULL; c = c->next) {
        if (c->tag == OW_CAPTION) walk_node(f, c, st);
    }
    y = f->y;
    /* Pass 1: the widest line and the widest word of each column. */
    memset(&grid, 0, sizeof(grid));
    for (row = row_next(table, NULL); row != NULL; row = row_next(table, row)) {
        int used;
        for (c = row->first; c != NULL; c = c->next) {
            int span, start, w = 0, word = 0;
            if (c->tag != OW_TD && c->tag != OW_TH) continue;
            if (is_hidden(c)) continue;
            span = attr_number(c, "colspan", 1);
            start = grid_place(&grid, &span, attr_number(c, "rowspan", 1));
            if (start < 0) break;
            if (f->depth >= MAX_TABLE_DEPTH) continue;
            (void)cell_run(f, c, NULL, 0, 4000, 0, &w, &word);
            w += 2 * CELL_PAD + 1; word += 2 * CELL_PAD + 1;
            if (span == 1) {
                if (w > pref[start]) pref[start] = w;
                if (word > least[start]) least[start] = word;
            } else {            /* share a wide cell between its columns */
                int have = 0, have_least = 0;
                for (int i = 0; i < span; i++) {
                    have += pref[start + i]; have_least += least[start + i];
                }
                for (int i = 0; i < span; i++) {
                    if (w > have) pref[start + i] += (w - have + span - 1) / span;
                    if (word > have_least) {
                        least[start + i] += (word - have_least + span - 1) / span;
                    }
                }
            }
        }
        used = grid_row_end(&grid);
        if (used > columns) columns = used;
    }
    if (columns == 0 || f->depth >= MAX_TABLE_DEPTH) {
        /* No grid: show the cells as blocks. */
        for (c = table->first; c != NULL; c = c->next) {
            if (c->tag != OW_CAPTION) walk_node(f, c, st);
        }
        return;
    }
    for (int i = 0; i < columns; i++) {
        if (pref[i] < 2 * CELL_PAD + 1) pref[i] = 2 * CELL_PAD + 1;
        if (least[i] > pref[i]) least[i] = pref[i];
        if (least[i] < 2 * CELL_PAD + 1) least[i] = 2 * CELL_PAD + 1;
        total += pref[i]; total_least += least[i];
    }
    if (f->s == NULL) {         /* measure only: report and stop */
        if (f->indent + total > f->max_w) f->max_w = f->indent + total;
        if (total_least > f->max_word) f->max_word = total_least;
        return;
    }
    if (total <= avail) {
        memcpy(width, pref, sizeof(width));
    } else if (total_least >= avail) {
        for (int i = 0; i < columns; i++) {
            width[i] = least[i] * avail / total_least;
            if (width[i] < 2 * CELL_PAD + 2) width[i] = 2 * CELL_PAD + 2;
        }
    } else {
        const int spare = avail - total_least, wish = total - total_least;
        for (int i = 0; i < columns; i++) {
            width[i] = least[i] + (pref[i] - least[i]) * spare / wish;
        }
    }
    total = 0;
    for (int i = 0; i < columns; i++) total += width[i];
    border = attr_number(table, "border", 0) != 0;

    /* Pass 2: draw. */
    memset(&grid, 0, sizeof(grid));
    for (row = row_next(table, NULL); row != NULL; row = row_next(table, row)) {
        int bottom = y + 2 * CELL_PAD;
        int edge[MAX_COLS + 1], edges = 0;

        if (surface_need(f->s, (uint32_t)bottom) != 0) break;
        for (c = row->first; c != NULL; c = c->next) {
            int span, start, cx = x0, cw = 0, cy;
            if (c->tag != OW_TD && c->tag != OW_TH) continue;
            if (is_hidden(c)) continue;
            span = attr_number(c, "colspan", 1);
            start = grid_place(&grid, &span, attr_number(c, "rowspan", 1));
            if (start < 0 || start >= columns) break;
            for (int i = 0; i < start; i++) cx += width[i];
            for (int i = 0; i < span && start + i < columns; i++) cw += width[start + i];
            cy = cell_run(f, c, f->s, cx + CELL_PAD, cw - 2 * CELL_PAD,
                          y + CELL_PAD / 2 + 1, NULL, NULL);
            if (cy + CELL_PAD / 2 + 1 > bottom) bottom = cy + CELL_PAD / 2 + 1;
            if (edges <= MAX_COLS) edge[edges++] = cx;
        }
        (void)grid_row_end(&grid);
        if (surface_need(f->s, (uint32_t)bottom + 1U) != 0) break;
        if (border) {
            hline(f->s, x0, x0 + total + 1, y, LV_LIGHT, 0);
            for (int i = 0; i < edges; i++) vline(f->s, edge[i], y, bottom, LV_LIGHT);
            vline(f->s, x0 + total, y, bottom, LV_LIGHT);
        }
        y = bottom;
    }
    if (border && surface_need(f->s, (uint32_t)y + 1U) == 0) {
        hline(f->s, x0, x0 + total + 1, y, LV_LIGHT, 0);
        y++;
    }
    f->y = y;
}

/* ---- Public ------------------------------------------------------------ */

void onewww_page_free(void)
{
    free(s_page.px); free(s_page.scb); free(s_page.pal);
    memset(&s_page, 0, sizeof(s_page));
    doc_free();
}

int onewww_page_build(const uint8_t *html, size_t length,
                      const char *content_type, const char *base_url,
                      int internal)
{
    onewww_dom_t *dom;
    flow_t f;
    line_t *line;
    size_t n;

    onewww_page_free();
    dom = onewww_dom_parse(html, length, content_type);
    line = malloc(sizeof(*line));
    if (dom == NULL || line == NULL || surface_need(&s_page, ROW_CHUNK) != 0) {
        onewww_dom_free(dom); free(line); onewww_page_free();
        return -1;
    }
    s_page.rows = 0U;
    s_doc.base = base_url; s_doc.internal = internal != 0;
    /* The title, with its white space collapsed. */
    n = 0;
    for (const char *t = onewww_dom_title(dom); *t != '\0' && n + 1U < sizeof(s_doc.title); t++) {
        const int space = (unsigned char)*t <= ' ';
        if (space && (n == 0U || s_doc.title[n - 1U] == ' ')) continue;
        s_doc.title[n++] = space ? ' ' : *t;
    }
    s_doc.title[n] = '\0';

    memset(&f, 0, sizeof(f));
    memset(line, 0, sizeof(*line));
    f.s = &s_page; f.line = line; f.x0 = MARGIN; f.w = PAGE_WIDTH - 2 * MARGIN;
    f.top_level = 1U; f.row_pal = PAL_TEXT; f.gap = 1U;
    walk_children(&f, onewww_dom_root(dom), s_plain);
    flow_break(&f, &s_plain, 0);
    (void)surface_need(&s_page, (uint32_t)f.y + 8U);
    free(line);
    onewww_dom_free(dom);
    s_doc.base = NULL;
    return 0;
}

uint32_t onewww_page_rows(void) { return s_page.rows; }
const char *onewww_page_title(void) { return s_doc.title; }
uint32_t onewww_link_count(void) { return s_doc.links; }

const char *onewww_link_href(uint32_t link)
{
    return link < s_doc.links ? s_doc.href[link] : "";
}

int onewww_link_at(int x, uint32_t row)
{
    for (uint32_t i = 0; i < s_doc.rects; i++) {
        const rect_t *r = &s_doc.rect[i];
        if (row >= r->y && row < r->y + r->h && x >= r->x0 && x < r->x1) {
            return (int)r->link;
        }
    }
    return -1;
}

uint32_t onewww_link_row(uint32_t link)
{
    return link < s_doc.links ? s_doc.link_row[link] : UINT32_MAX;
}

int onewww_anchor_row(const char *name)
{
    for (uint32_t i = 0; i < s_doc.anchors; i++) {
        if (strcmp(s_doc.anchor[i].name, name) == 0) return (int)s_doc.anchor[i].row;
    }
    return -1;
}

static int status_text(surface_t *s, const char *text, int x, int limit,
                       int draw)
{
    const onewww_font_t *font = &onewww_fonts[ONEWWW_FONT_SMALL];
    const unsigned char *p = (const unsigned char *)text;

    while (p != NULL && *p != '\0') {
        uint32_t code = *p++;
        int more = code >= 0xF0 ? 3 : code >= 0xE0 ? 2 : code >= 0xC0 ? 1 : 0;
        int advance;

        if (more != 0) code &= 0x3FU >> more;
        while (more-- > 0 && (*p & 0xC0) == 0x80) code = (code << 6) | (*p++ & 0x3FU);
        if (code > 0xFFFFU) code = '?';
        advance = glyph_find(font, (uint16_t)code)->advance;
        if (x + advance > limit) {
            if (draw) (void)glyph_draw(s, font, 0x2026, x, 2, 0);
            break;
        }
        if (draw) (void)glyph_draw(s, font, (uint16_t)code, x, 2, 0);
        x += advance;
    }
    return x;
}

static void status_draw(const char *left, const char *right)
{
    surface_t s = { s_status_px, s_status_scb, s_status_pal,
                    ONEWWW_STATUS_ROWS, ONEWWW_STATUS_ROWS, 1U };
    int right_x = PAGE_WIDTH - 6;

    memset(s_status_px, 0, sizeof(s_status_px));
    memset(s_status_scb, 0x80, sizeof(s_status_scb));
    memset(s_status_pal, PAL_SHADE, sizeof(s_status_pal));
    hline(&s, 0, PAGE_WIDTH, 0, LV_DARK, 0);
    if (right != NULL && right[0] != '\0') {
        right_x -= status_text(&s, right, 0, PAGE_WIDTH, 0);
        (void)status_text(&s, right, right_x, PAGE_WIDTH, 1);
        right_x -= 12;
    }
    (void)status_text(&s, left, 6, right_x - 8, 1);
}

/* An arrow: 'X' is ink, '.' is paper. */
static const char *const s_pointer[] = {
    "X", "XX", "X.X", "X..X", "X...X", "X....X", "X.....X", "X......X",
    "X.......X", "X........X", "X.....XXXXX", "X..X..X", "X.X X..X",
    "XX  X..X", "X    X..X", "     X..X", "      XX",
};
#define POINTER_ROWS (sizeof(s_pointer) / sizeof(s_pointer[0]))

void onewww_compose(uint32_t top, const char *left, const char *right,
                    int selected, int pointer_x, int pointer_y,
                    uint8_t *fields)
{
    static const uint8_t magic[4] = { 0xD3, 0xC8, 0xD2, 0xB4 };  /* SHR4 */
    static uint8_t row_scb[ONEWWW_VIEW_ROWS], row_pal[ONEWWW_VIEW_ROWS];
    uint8_t slot_of[2][PAL_BASES * 2U];
    uint8_t next[2] = { 0U, 0U };

    memset(fields, 0, ONEWWW_FIELDS_BYTES);
    memset(slot_of, 0xFF, sizeof(slot_of));
    status_draw(left, right);
    for (uint32_t r = 0; r < ONEWWW_VIEW_ROWS; r++) {
        const uint32_t field = r & 1U, line = r >> 1;
        uint8_t *base = fields + field * ONEWWW_FIELD_BYTES;
        const uint8_t *px = NULL;
        uint8_t scb = 0x80, pal = PAL_TEXT, slot;
        unsigned base_pal, link;

        if (r >= ONEWWW_PAGE_VIEW) {
            const uint32_t sr = r - ONEWWW_PAGE_VIEW;
            px = s_status_px + sr * ROW_BYTES;
            scb = s_status_scb[sr]; pal = s_status_pal[sr];
        } else if (top + r < s_page.rows) {
            px = s_page.px + (size_t)(top + r) * ROW_BYTES;
            scb = s_page.scb[top + r]; pal = s_page.pal[top + r];
        }
        row_scb[r] = scb; row_pal[r] = pal;
        if (px != NULL) memcpy(base + line * ROW_BYTES, px, ROW_BYTES);
        base_pal = pal & 0x7FU; link = (pal & PAL_LINK) != 0U ? 1U : 0U;
        slot = slot_of[field][base_pal * 2U + link];
        if (slot == 0xFF) {
            uint8_t *entry;
            slot = next[field] < 16U ? next[field]++ : 0U;
            slot_of[field][base_pal * 2U + link] = slot;
            entry = base + 0x7E00U + (uint32_t)slot * 32U;
            for (unsigned i = 0; i < 16U; i++) {
                const uint16_t c = base_pal == PAL_PICTURE ?
                    s_picture_palette[i] :
                    s_text_palettes[base_pal][link][i & 3U];
                entry[2U * i] = (uint8_t)c;
                entry[2U * i + 1U] = (uint8_t)(c >> 8);
            }
        }
        base[0x7D00U + line] = (uint8_t)(scb | slot);
    }
    /* The selected link: paper and link colour change places. */
    for (uint32_t i = 0; selected >= 0 && i < s_doc.rects; i++) {
        const rect_t *rc = &s_doc.rect[i];
        if (rc->link != (uint32_t)selected || rc->y + rc->h <= top ||
            rc->y >= top + ONEWWW_PAGE_VIEW) continue;
        for (uint32_t y = rc->y < top ? top : rc->y;
             y < rc->y + rc->h && y < top + ONEWWW_PAGE_VIEW; y++) {
            const uint32_t r = y - top;
            uint8_t *row = fields + (r & 1U) * ONEWWW_FIELD_BYTES + (r >> 1) * ROW_BYTES;
            if ((row_pal[r] & PAL_LINK) == 0U || row_scb[r] != 0x80U) continue;
            for (int x = rc->x0 - 1; x <= rc->x1; x++) {
                unsigned shift, v;
                if (x < 0 || x >= PAGE_WIDTH) continue;
                shift = 6U - 2U * ((unsigned)x & 3U);
                v = (row[x >> 2] >> shift) & 3U;
                if (v == 0U || v == 3U) {
                    row[x >> 2] = (uint8_t)((row[x >> 2] & ~(3U << shift)) |
                                            ((v ^ 3U) << shift));
                }
            }
        }
    }
    /* The mouse pointer. */
    for (unsigned py = 0; pointer_x >= 0 && py < POINTER_ROWS; py++) {
        const int r = pointer_y + (int)py;
        uint8_t *row;
        if (r < 0 || r >= (int)ONEWWW_VIEW_ROWS) continue;
        row = fields + ((unsigned)r & 1U) * ONEWWW_FIELD_BYTES +
              ((unsigned)r >> 1) * ROW_BYTES;
        for (int px = 0; s_pointer[py][px] != '\0'; px++) {
            const int x = pointer_x + px;
            const char c = s_pointer[py][px];
            if (c == ' ' || x < 0 || x >= PAGE_WIDTH) continue;
            if (row_scb[r] == 0x80U) {
                const unsigned shift = 6U - 2U * ((unsigned)x & 3U);
                const unsigned ink = (row_pal[r] & PAL_LINK) != 0U ? 2U : 3U;
                row[x >> 2] = (uint8_t)((row[x >> 2] & ~(3U << shift)) |
                                        ((c == 'X' ? ink : 0U) << shift));
            } else {
                const unsigned shift = (x & 2) == 0 ? 4U : 0U;
                row[x >> 2] = (uint8_t)((row[x >> 2] & ~(15U << shift)) |
                                        ((c == 'X' ? 0U : 6U) << shift));
            }
        }
    }
    fields[0x7DF8U] = 1U;                                   /* interlace */
    memcpy(fields + 0x7DFCU, magic, sizeof(magic));
    memcpy(fields + ONEWWW_FIELD_BYTES + 0x7DFCU, magic, sizeof(magic));
}
