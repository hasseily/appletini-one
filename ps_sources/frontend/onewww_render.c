/* ONEWWW page renderer: lays a styled document tree out with proportional
 * fonts. Blocks, lists, tables with automatic column widths, floats,
 * links, form fields, pictures.
 *
 * The layout does not draw. It records what the page shows in a display
 * list: text runs, filled boxes and pictures, each with its place on the
 * page. onewww_compose() draws the part of the list that is in view into
 * a 640 x 400 true-colour picture. A scroll draws the list again at a
 * different row; a long page needs little memory.
 * No hardware access: the file also compiles on the host. */
#include "onewww_render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http_core.h"
#include "onewww_css.h"
#include "onewww_dom.h"
#include "onewww_font.h"

#define PAGE_WIDTH  640
#define MARGIN      12
#define MAX_ROWS    2000000U
#define INK_OF_ROW  OW_NO_COLOR         /* a line in a grey of the page ink */
enum { LV_LIGHT = 1, LV_DARK = 2, LV_INK = 3 };

/* ---- Display list ------------------------------------------------------ */

enum { D_RECT, D_TEXT, D_IMAGE };

typedef struct {
    int32_t y;                  /* page row of the top */
    int16_t x, w;
    uint16_t h;
    uint8_t kind, font;
    uint16_t colour;            /* 0RGB */
    uint16_t count;             /* D_TEXT: characters */
    uint32_t data;              /* D_TEXT: place in the character store;
                                 * D_IMAGE: number of the picture */
} ditem_t;

/* A page as a list. The name is from the time when the layout drew into
 * a surface of rows. */
typedef struct {
    ditem_t *item;
    uint32_t items, cap;
    uint16_t *code;             /* the characters of all text runs */
    uint32_t codes, code_cap;
    uint32_t rows;              /* height of the page */
    uint16_t paper, ink;        /* of the page */
} surface_t;

static surface_t s_page = { NULL, 0U, 0U, NULL, 0U, 0U, 0U, 0x0FFF, 0x0000 };

static int color_distance(uint16_t a, uint16_t b)
{
    const int dr = (int)((a >> 8) & 15) - (int)((b >> 8) & 15);
    const int dg = (int)((a >> 4) & 15) - (int)((b >> 4) & 15);
    const int db = (int)(a & 15) - (int)(b & 15);
    return (dr < 0 ? -dr : dr) + (dg < 0 ? -dg : dg) + (db < 0 ? -db : db);
}

static uint16_t mix(uint16_t a, uint16_t b, int part, int of)
{
    uint16_t out = 0;
    for (int shift = 0; shift <= 8; shift += 4) {
        const int ca = (a >> shift) & 15, cb = (b >> shift) & 15;
        out |= (uint16_t)(((ca * (of - part) + cb * part + of / 2) / of) << shift);
    }
    return out;
}

/* The page is at least this high. */
static int surface_need(surface_t *s, uint32_t rows)
{
    if (rows > MAX_ROWS) return -1;
    if (rows > s->rows) s->rows = rows;
    return 0;
}

static ditem_t *dl_new(surface_t *s)
{
    if (s->items == s->cap) {
        const uint32_t cap = s->cap ? s->cap * 2U : 1024U;
        ditem_t *bigger = realloc(s->item, (size_t)cap * sizeof(ditem_t));
        if (bigger == NULL) return NULL;
        s->item = bigger; s->cap = cap;
    }
    memset(&s->item[s->items], 0, sizeof(ditem_t));
    return &s->item[s->items++];
}

/* Adds a filled box. Returns its number: a caller that does not know the
 * height yet sets it later with dl_box(). -1 without memory. */
static int dl_rect(surface_t *s, int x, int y, int w, int h, uint16_t colour)
{
    ditem_t *it = dl_new(s);

    if (it == NULL) return -1;
    it->kind = D_RECT; it->colour = colour;
    it->x = (int16_t)x; it->y = y; it->w = (int16_t)(w < 0 ? 0 : w);
    it->h = (uint16_t)(h < 0 ? 0 : h > 65535 ? 65535 : h);
    return (int)(s->items - 1U);
}

/* Gives a box of dl_rect() its place and size. A high box is cut in
 * parts: the height of one entry has 16 bits. */
static void dl_box(surface_t *s, int index, int x, int y, int w, int h)
{
    if (index < 0 || (uint32_t)index >= s->items) return;
    s->item[index].x = (int16_t)x; s->item[index].y = y;
    s->item[index].w = (int16_t)(w < 0 ? 0 : w);
    s->item[index].h = (uint16_t)(h < 0 ? 0 : h > 65535 ? 65535 : h);
}

static void dl_text(surface_t *s, int x, int y, uint8_t font, uint16_t colour,
                    const uint16_t *codes, unsigned count)
{
    ditem_t *it;

    if (count == 0U) return;
    if (s->codes + count > s->code_cap) {
        uint32_t cap = s->code_cap ? s->code_cap * 2U : 8192U;
        uint16_t *bigger;
        while (cap < s->codes + count) cap *= 2U;
        bigger = realloc(s->code, (size_t)cap * sizeof(uint16_t));
        if (bigger == NULL) return;
        s->code = bigger; s->code_cap = cap;
    }
    it = dl_new(s);
    if (it == NULL) return;
    it->kind = D_TEXT; it->font = font; it->colour = colour;
    it->x = (int16_t)x; it->y = y;
    it->h = (uint16_t)(onewww_fonts[font].ascent + onewww_fonts[font].descent + 2U);
    it->count = (uint16_t)count; it->data = s->codes;
    memcpy(&s->code[s->codes], codes, count * sizeof(uint16_t));
    s->codes += count;
}

static void dl_image(surface_t *s, int x, int y, int w, int h, int image)
{
    ditem_t *it = dl_new(s);

    if (it == NULL) return;
    it->kind = D_IMAGE; it->data = (uint32_t)image;
    it->x = (int16_t)x; it->y = y; it->w = (int16_t)w; it->h = (uint16_t)h;
}

static void surface_free(surface_t *s)
{
    free(s->item); free(s->code);
    s->item = NULL; s->code = NULL;
    s->items = s->cap = s->codes = s->code_cap = s->rows = 0U;
}

/* Thin lines: borders, rules, underlines. A line in the "ink of the row"
 * is a grey between the paper behind it and the ink of the page. */
static void hline(surface_t *s, int x0, int x1, int y, unsigned level,
                  uint16_t colour, uint16_t paper)
{
    (void)dl_rect(s, x0, y, x1 - x0, 1,
                  colour == INK_OF_ROW ? mix(paper, s->ink, (int)level, 3) : colour);
}

static void vline(surface_t *s, int x, int y0, int y1, unsigned level,
                  uint16_t paper)
{
    (void)dl_rect(s, x, y0, 1, y1 - y0, mix(paper, s->ink, (int)level, 3));
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

static int text_width(const onewww_font_t *f, const uint16_t *cp, unsigned n)
{
    int w = 0;
    while (n-- != 0U) w += glyph_find(f, *cp++)->advance;
    return w;
}

/* Text that is too near to the colour of its paper cannot be read. */
static uint16_t readable(uint16_t colour, uint16_t paper)
{
    const int light = (int)((paper >> 8) & 15) * 3 + (int)((paper >> 4) & 15) * 6 +
                      (int)(paper & 15);
    if (color_distance(colour, paper) >= 8) return colour;
    return light > 75 ? 0x0000 : 0x0FFF;
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

typedef struct {
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
} doc_t;

static doc_t s_doc;

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

    if (href[0] == '#' || strncmp(href, "field:", 6U) == 0) {
        snprintf(url, sizeof(url), "%s", href);
    } else if (strncmp(href, "http://", 7U) == 0 || strncmp(href, "https://", 8U) == 0) {
        /* A complete address needs no base (the home page has none). */
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

static void doc_lists_free(doc_t *d)
{
    for (uint32_t i = 0; i < d->links; i++) free(d->href[i]);
    for (uint32_t i = 0; i < d->anchors; i++) free(d->anchor[i].name);
    free(d->href); free(d->link_row); free(d->rect); free(d->anchor);
    d->href = NULL; d->link_row = NULL; d->rect = NULL; d->anchor = NULL;
    d->links = d->links_cap = d->rects = d->rects_cap = 0U;
    d->anchors = d->anchors_cap = 0U;
}

/* ---- Pictures ---------------------------------------------------------- */

#define IMG_MAX     200U
#define IMG_STORE_W 640U        /* a picture is kept at most this large */
#define IMG_STORE_H 1600U
#define IMG_BYTES   (24U * 1024U * 1024U)

enum { IMG_WANTED, IMG_LOADED, IMG_FAILED };

typedef struct {
    const onewww_node_t *node;
    char *url;
    uint8_t *rgb;               /* w x h x 3, on the paper of the page */
    uint16_t w, h;              /* size of rgb */
    uint8_t *scaled;            /* a copy at the size on the page */
    uint16_t sw, sh;
    uint16_t natural_w, natural_h;
    uint8_t state;
} image_t;

static image_t s_img[IMG_MAX];
static unsigned s_imgs;
static size_t s_img_bytes, s_scaled_bytes;
#define SCALED_BYTES (12U * 1024U * 1024U)  /* all copies for the screen */

static void images_free(void)
{
    for (unsigned i = 0; i < s_imgs; i++) {
        free(s_img[i].url); free(s_img[i].rgb); free(s_img[i].scaled);
    }
    memset(s_img, 0, sizeof(s_img));
    s_imgs = 0U; s_img_bytes = 0U; s_scaled_bytes = 0U;
}

static int image_find(const onewww_node_t *node)
{
    for (unsigned i = 0; i < s_imgs; i++) {
        if (s_img[i].node == node) return (int)i;
    }
    return -1;
}

unsigned onewww_page_images(void) { return s_imgs; }

const char *onewww_page_image_url(unsigned image)
{
    return image < s_imgs && s_img[image].state == IMG_WANTED ? s_img[image].url : NULL;
}

void onewww_page_image_failed(unsigned image)
{
    if (image < s_imgs && s_img[image].state == IMG_WANTED) s_img[image].state = IMG_FAILED;
}

/* Makes an RGB copy of a size: averages when it shrinks. */
static uint8_t *scale_rgb(const uint8_t *src, unsigned comps, unsigned w, unsigned h,
                          unsigned tw, unsigned th)
{
    uint8_t *out = malloc((size_t)tw * th * 3U);
    const unsigned paper[3] = { ((s_page.paper >> 8) & 15U) * 17U,
                                ((s_page.paper >> 4) & 15U) * 17U,
                                (s_page.paper & 15U) * 17U };

    if (out == NULL) return NULL;
    for (unsigned ty = 0; ty < th; ty++) {
        const unsigned y0 = (unsigned)((uint64_t)ty * h / th);
        unsigned y1 = (unsigned)((uint64_t)(ty + 1U) * h / th);
        if (y1 <= y0) y1 = y0 + 1U;
        if (y1 > h) y1 = h;
        for (unsigned tx = 0; tx < tw; tx++) {
            const unsigned x0 = (unsigned)((uint64_t)tx * w / tw);
            unsigned x1 = (unsigned)((uint64_t)(tx + 1U) * w / tw);
            uint32_t sum[3] = { 0, 0, 0 }, count = 0;
            if (x1 <= x0) x1 = x0 + 1U;
            if (x1 > w) x1 = w;
            for (unsigned y = y0; y < y1; y++) {
                const uint8_t *p = src + ((size_t)y * w + x0) * comps;
                for (unsigned x = x0; x < x1; x++, p += comps) {
                    if (comps == 4U) {          /* on the paper of the page */
                        const unsigned a = p[3];
                        sum[0] += (p[0] * a + paper[0] * (255U - a)) / 255U;
                        sum[1] += (p[1] * a + paper[1] * (255U - a)) / 255U;
                        sum[2] += (p[2] * a + paper[2] * (255U - a)) / 255U;
                    } else { sum[0] += p[0]; sum[1] += p[1]; sum[2] += p[2]; }
                    count++;
                }
            }
            if (count == 0U) count = 1U;
            out[((size_t)ty * tw + tx) * 3U] = (uint8_t)(sum[0] / count);
            out[((size_t)ty * tw + tx) * 3U + 1U] = (uint8_t)(sum[1] / count);
            out[((size_t)ty * tw + tx) * 3U + 2U] = (uint8_t)(sum[2] / count);
        }
    }
    return out;
}

void onewww_page_image_data(unsigned image, const uint8_t *rgba,
                            unsigned width, unsigned height)
{
    image_t *im;
    unsigned w = width, h = height;

    if (image >= s_imgs || s_img[image].state != IMG_WANTED) return;
    im = &s_img[image];
    im->state = IMG_FAILED;
    if (rgba == NULL || width == 0U || height == 0U || width > 16384U || height > 16384U) return;
    if (w > IMG_STORE_W) { h = (unsigned)((uint64_t)h * IMG_STORE_W / w); w = IMG_STORE_W; }
    if (h > IMG_STORE_H) { w = (unsigned)((uint64_t)w * IMG_STORE_H / h); h = IMG_STORE_H; }
    if (w == 0U) w = 1U;
    if (h == 0U) h = 1U;
    if (s_img_bytes + (size_t)w * h * 3U > IMG_BYTES) return;
    im->rgb = scale_rgb(rgba, 4U, width, height, w, h);
    if (im->rgb == NULL) return;
    s_img_bytes += (size_t)w * h * 3U;
    im->w = (uint16_t)w; im->h = (uint16_t)h;
    im->natural_w = (uint16_t)(width > 4000U ? 4000U : width);
    im->natural_h = (uint16_t)(height > 4000U ? 4000U : height);
    im->state = IMG_LOADED;
}

/* Draws a picture into `rows` rows of the frame, at its size on the
 * page. The copy at that size is made at the first use and kept. */
static void image_blit(uint32_t *frame, int rows, unsigned image,
                       int x, int y, int w, int h)
{
    image_t *im;

    if (image >= s_imgs || w <= 0 || h <= 0) return;
    im = &s_img[image];
    if (im->rgb == NULL) return;
    if (im->scaled == NULL || im->sw != (uint16_t)w || im->sh != (uint16_t)h) {
        if (im->scaled != NULL) { s_scaled_bytes -= (size_t)im->sw * im->sh * 3U; free(im->scaled); }
        im->scaled = NULL;
        if (s_scaled_bytes + (size_t)w * (size_t)h * 3U > SCALED_BYTES) {
            /* Make room: the copies of the other pictures can be made again. */
            for (unsigned i = 0; i < s_imgs; i++) { free(s_img[i].scaled); s_img[i].scaled = NULL; }
            s_scaled_bytes = 0U;
        }
        im->scaled = scale_rgb(im->rgb, 3U, im->w, im->h, (unsigned)w, (unsigned)h);
        if (im->scaled == NULL) return;
        im->sw = (uint16_t)w; im->sh = (uint16_t)h;
        s_scaled_bytes += (size_t)w * (size_t)h * 3U;
    }
    for (int ry = y < 0 ? -y : 0; ry < h && y + ry < rows; ry++) {
        const uint8_t *src = im->scaled + (size_t)ry * (size_t)w * 3U;
        uint32_t *row = frame + (size_t)(y + ry) * PAGE_WIDTH;
        for (int rx = 0; rx < w; rx++, src += 3) {
            const int px = x + rx;
            if (px < 0 || px >= PAGE_WIDTH) continue;
            row[px] = 0xFF000000U | ((uint32_t)src[0] << 16) | ((uint32_t)src[1] << 8) | src[2];
        }
    }
}

/* ---- Inline layout ----------------------------------------------------- */

#define LINE_CODES 1024U
#define LINE_ITEMS 160U
#define MARKER_MAX 8U
#define NO_GAP     10000    /* the start of a column: no margin above */

typedef struct {
    uint16_t start, count;
    int16_t x, w;
    int32_t link;       /* -1: not a link */
    uint16_t colour;
    uint16_t back;      /* background of the word, or OW_NO_COLOR */
    uint8_t font, underline;
    int16_t image;      /* -1: text. Else a picture, w wide and ih high */
    uint16_t ih;
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
    uint16_t colour;
    uint16_t back;              /* background inside the line, or OW_NO_COLOR */
    uint8_t font, underline, pre, nowrap;
    uint8_t list;               /* 1 = bullets, 2 = numbers */
    int *counter;               /* number of the next <li> */
    int32_t link;
} text_t;

/* One column of text: the page, or a table cell. */
typedef struct {
    surface_t *s;               /* NULL: measure only */
    line_t *line;
    int x0, w, y;
    int indent, right;          /* space taken off the left and the right */
    int max_w;                  /* widest line */
    int max_word;               /* widest word */
    int depth;                  /* table nesting */
    int gap;                    /* margin rows after the last text */
    /* Floats: the space that they take from each side of the column, and
     * the row where that space ends. */
    int fl_edge, fl_bottom, fr_edge, fr_bottom;
    int stop_at;                /* a measure run ends when a line is wider */
    const struct onewww_node *float_root;   /* this float is the column */
    uint8_t over;
    uint16_t paper;
    uint16_t marker[MARKER_MAX];
    uint16_t marker_colour;
    uint8_t marker_len;
    uint8_t align, top_level;
} flow_t;

static const text_t s_plain = { 0x0000, OW_NO_COLOR, ONEWWW_FONT_BODY, 0, 0, 0, 0, NULL, -1 };

/* Set when onewww_pause asks to stop. The walk then ends quickly. */
static uint8_t s_abort;

static void pause_point(void)
{
    if (!s_abort && onewww_pause != NULL && onewww_pause()) s_abort = 1U;
}

/* Space taken off the left and the right of a line at the current row. */
static int left_inset(const flow_t *f)
{
    return f->y < f->fl_bottom && f->fl_edge > f->indent ? f->fl_edge : f->indent;
}

static int right_inset(const flow_t *f)
{
    return f->y < f->fr_bottom && f->fr_edge > f->right ? f->fr_edge : f->right;
}

/* Moves below the floats of the given sides. */
static void floats_clear(flow_t *f, unsigned sides)
{
    if ((sides & OW_SIDE_LEFT) && f->y < f->fl_bottom) f->y = f->fl_bottom;
    if ((sides & OW_SIDE_RIGHT) && f->y < f->fr_bottom) f->y = f->fr_bottom;
    if (f->s != NULL) (void)surface_need(f->s, (uint32_t)f->y);
}

static void line_reset(line_t *l)
{
    l->items = 0U; l->codes = 0U; l->width = 0; l->space = 0U;
}

static void flow_flush(flow_t *f, int force)
{
    line_t *l = f->line;
    const int left = left_inset(f);
    const int avail = f->w - left - right_inset(f);
    int ascent = 0, descent = 0, shift = 0, top, height;

    if (l->items == 0U && !force) { l->space = 0U; return; }
    pause_point();
    if (l->width + f->indent + f->right > f->max_w) {
        f->max_w = l->width + f->indent + f->right;
        if (f->s == NULL && f->stop_at != 0 && f->max_w > f->stop_at) f->over = 1U;
    }
    if (l->items == 0U) {
        ascent = onewww_fonts[ONEWWW_FONT_BODY].ascent;
        descent = onewww_fonts[ONEWWW_FONT_BODY].descent;
    }
    for (unsigned i = 0; i < l->items; i++) {
        const onewww_font_t *font = &onewww_fonts[l->item[i].font];
        if (l->item[i].image >= 0) {            /* a picture stands on the line */
            if ((int)l->item[i].ih > ascent) ascent = (int)l->item[i].ih;
            continue;
        }
        if (font->ascent > ascent) ascent = font->ascent;
        if (font->descent > descent) descent = font->descent;
    }
    height = ascent + descent + 2;
    top = f->y + 1;
    f->gap = 0;
    if (f->s == NULL || surface_need(f->s, (uint32_t)(f->y + height)) != 0) {
        f->y += height; f->marker_len = 0U;
        line_reset(l);
        return;
    }
    if (f->align == OW_ALIGN_CENTER) shift = (avail - l->width) / 2;
    else if (f->align == OW_ALIGN_RIGHT) shift = avail - l->width;
    if (shift < 0) shift = 0;
    if (f->marker_len != 0U && left >= 14) {
        const onewww_font_t *font = &onewww_fonts[ONEWWW_FONT_BODY];
        dl_text(f->s, f->x0 + left - 6 - text_width(font, f->marker, f->marker_len),
                top + ascent - font->ascent, ONEWWW_FONT_BODY,
                readable(f->marker_colour, f->paper), f->marker, f->marker_len);
    }
    f->marker_len = 0U;
    for (unsigned i = 0; i < l->items; i++) {
        const item_t *it = &l->item[i];
        const onewww_font_t *font = &onewww_fonts[it->font];
        const int x_start = f->x0 + left + shift + it->x;
        uint16_t colour = readable(it->colour, f->paper);

        if (it->image >= 0) {
            dl_image(f->s, x_start, top + ascent - (int)it->ih, it->w, (int)it->ih, it->image);
        } else {
            if (it->back != OW_NO_COLOR) {      /* a word on its own paper */
                (void)dl_rect(f->s, x_start - 3, f->y, it->w + 6, height, it->back);
                colour = readable(it->colour, it->back);
            }
            dl_text(f->s, x_start, top + ascent - font->ascent, it->font, colour,
                    &l->cp[it->start], it->count);
            if (it->underline) {
                (void)dl_rect(f->s, x_start, top + ascent + 2, it->w, 1, colour);
            }
        }
        if (it->link >= 0) {
            rect_add(x_start, x_start + it->w, f->y, height, (uint32_t)it->link);
        }
    }
    f->y += height;
    line_reset(l);
}

static void flow_word_end(flow_t *f, const text_t *tx)
{
    line_t *l = f->line;
    const onewww_font_t *font = &onewww_fonts[tx->font];
    int avail = f->w - left_inset(f) - right_inset(f);
    int space_w, w;
    item_t *it;

    if (l->word_len == 0U) return;
    w = text_width(font, l->word, l->word_len);
    if (w > f->max_word) f->max_word = w;
    /* A word that does not fit at the side of a float goes below it. */
    while (l->items == 0U && w > avail && (f->y < f->fl_bottom || f->y < f->fr_bottom)) {
        int below = f->y < f->fl_bottom ? f->fl_bottom : f->fr_bottom;
        if (f->y < f->fr_bottom && f->fr_bottom < below) below = f->fr_bottom;
        f->y = below;
        avail = f->w - left_inset(f) - right_inset(f);
    }
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
    it->font = tx->font;
    it->colour = tx->colour;
    it->back = tx->back;
    it->underline = tx->underline;
    it->link = tx->link;
    it->image = -1; it->ih = 0U;
    /* An underline or a background also covers the space between the
     * words of one run. */
    if ((it->underline || it->back != OW_NO_COLOR) && space_w != 0 && l->items > 1U &&
        l->item[l->items - 2U].underline == it->underline &&
        l->item[l->items - 2U].back == it->back &&
        l->item[l->items - 2U].link == it->link) {
        l->item[l->items - 2U].w = (int16_t)(l->item[l->items - 2U].w + space_w);
    }
    it->x = (int16_t)(l->width + space_w);
    it->w = (int16_t)w;
    l->width += space_w + w;
    l->word_len = 0U;
    l->space = 0U;
}

/* Puts a picture on the line, as a word of its width. */
static void flow_image(flow_t *f, const text_t *tx, int image, int w, int h)
{
    line_t *l = f->line;
    const onewww_font_t *font = &onewww_fonts[tx->font];
    int space_w;
    item_t *it;

    flow_word_end(f, tx);
    if (w > f->max_word) f->max_word = w;
    space_w = (l->space && l->items != 0U) ? glyph_find(font, ' ')->advance : 0;
    if (l->items != 0U &&
        l->width + space_w + w > f->w - left_inset(f) - right_inset(f)) {
        flow_flush(f, 0);
        space_w = 0;
    }
    if (l->items >= LINE_ITEMS) { flow_flush(f, 0); space_w = 0; }
    it = &l->item[l->items++];
    it->start = (uint16_t)l->codes; it->count = 0U;
    it->font = tx->font; it->colour = tx->colour; it->underline = 0U;
    it->back = OW_NO_COLOR;
    it->link = tx->link;
    it->image = (int16_t)image; it->ih = (uint16_t)h;
    it->x = (int16_t)(l->width + space_w);
    it->w = (int16_t)w;
    l->width += space_w + w;
    l->space = 0U;
}

static void flow_char(flow_t *f, const text_t *tx, uint16_t code)
{
    line_t *l = f->line;
    const int space = code == ' ' || code == '\n' || code == '\t' ||
                      code == '\r' || code == '\f';

    if (tx->pre) {
        if (code == '\n') { flow_word_end(f, tx); flow_flush(f, 1); return; }
        if (code == '\r') return;
        if (code == '\t') code = ' ';
    } else if (space && tx->nowrap) {
        /* The words stay on one line: the space is part of the word. */
        if (l->word_len == 0U || l->word[l->word_len - 1U] == ' ') return;
        code = ' ';
    } else if (space) {
        flow_word_end(f, tx);
        l->space = 1U;
        return;
    }
    /* Soft hyphen, zero-width characters and marks, byte order mark. */
    if (code == 0xAD || (code >= 0x200B && code <= 0x200F) || code == 0xFEFF ||
        code == 0x2060) return;
    if (code == 0xA0 || code == 0x2009 || code == 0x202F || code == 0x2002 ||
        code == 0x2003) code = ' ';
    if (l->word_len == sizeof(l->word) / sizeof(l->word[0])) flow_word_end(f, tx);
    l->word[l->word_len++] = code;
    if (tx->pre && code == ' ') flow_word_end(f, tx);
}

static void flow_text(flow_t *f, const text_t *tx, const char *text,
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
        flow_char(f, tx, code > 0xFFFFU ? '?' : (uint16_t)code);
    }
}

/* Ends the line. Margins between blocks do not add: the largest one
 * counts, as in a style sheet. */
static void flow_break(flow_t *f, const text_t *tx, int gap)
{
    flow_word_end(f, tx);
    flow_flush(f, 0);
    if (gap > f->gap) {
        f->y += gap - f->gap;
        if (f->s != NULL) (void)surface_need(f->s, (uint32_t)f->y);
        f->gap = gap;
    }
}

/* ---- Picture ----------------------------------------------------------- */

static const uint16_t s_picture_palette[16] = {
    0x0000, 0x0113, 0x0226, 0x034A, 0x047D, 0x07AF, 0x0FFF, 0x0FE8,
    0x0FB3, 0x0F71, 0x0C32, 0x0713, 0x0141, 0x0272, 0x04A4, 0x0888,
};

static void picture_rgb(int x, int y, int h, int *r, int *g, int *b)
{
    /* x is 0..319, y is 0..h-1. The page shows the picture with the same
     * scale in x and y, so a circle here is a circle on the screen. */
    const int sx = x - 220, sy = y - (h * 5) / 9;
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
    if (sx * sx + sy * sy < 22 * 22) { *r = 255; *g = 240; *b = 150; return; }
    if (t < 128) {
        *r = 34 + (t * 85) / 128; *g = 34 + (t * 136) / 128;
        *b = 102 + (t * 153) / 128;
    } else {
        *r = 119 + ((t - 128) * 136) / 128; *g = 170 + ((t - 128) * 68) / 128;
        *b = 255 - ((t - 128) * 119) / 128;
    }
}

/* Makes the built-in picture of the home page. Returns its number. */
static int builtin_image(const onewww_node_t *n)
{
    const int w = 320, h = 120;
    image_t *im;

    if (s_imgs == IMG_MAX) return -1;
    im = &s_img[s_imgs];
    im->rgb = malloc((size_t)w * h * 3U);
    if (im->rgb == NULL) return -1;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int r, g, b;
            picture_rgb(x, y, h, &r, &g, &b);
            im->rgb[(y * w + x) * 3] = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
            im->rgb[(y * w + x) * 3 + 1] = (uint8_t)(g < 0 ? 0 : g > 255 ? 255 : g);
            im->rgb[(y * w + x) * 3 + 2] = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
        }
    }
    im->node = n; im->w = (uint16_t)w; im->h = (uint16_t)h;
    im->natural_w = 616U; im->natural_h = 231U;
    im->state = IMG_LOADED;
    return (int)s_imgs++;
}

/* ---- Form fields ------------------------------------------------------- */

static const doc_t *view_doc(void);

#define FIELD_MAX      256U
#define FIELD_TEXT_MAX 512U

typedef struct {
    const onewww_node_t *node;
    char *value;                /* text fields: what the user typed */
    uint8_t type;               /* ONEWWW_FIELD_* */
    uint8_t checked;
    uint16_t option;            /* selected <option> of a list */
} field_t;

static field_t s_field[FIELD_MAX];
static unsigned s_fields;

static void fields_free(void)
{
    for (unsigned i = 0; i < s_fields; i++) free(s_field[i].value);
    memset(s_field, 0, sizeof(s_field));
    s_fields = 0U;
}

static int field_find(const onewww_node_t *node)
{
    for (unsigned i = 0; i < s_fields; i++) {
        if (s_field[i].node == node) return (int)i;
    }
    return -1;
}

static int attr_is(const onewww_node_t *n, const char *name, const char *want)
{
    char value[24];

    if (n->length == 0U || onewww_attr(n, name, value, sizeof(value)) < 0) return 0;
    for (char *p = value; *p != '\0'; p++) {
        if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + 32);
    }
    return strcmp(value, want) == 0;
}

static int attr_has(const onewww_node_t *n, const char *name)
{
    char value[4];
    return n->length != 0U && onewww_attr(n, name, value, sizeof(value)) >= 0;
}

/* The kind of a form control: ONEWWW_FIELD_*, 0 for a control that the
 * user cannot use here, -1 for a hidden input. */
static int field_kind(const onewww_node_t *n)
{
    if (n->tag == OW_SELECT) return ONEWWW_FIELD_LIST;
    if (n->tag == OW_TEXTAREA) return ONEWWW_FIELD_TEXT;
    if (n->tag == OW_BUTTON) {
        return attr_is(n, "type", "button") || attr_is(n, "type", "reset") ? 0 : ONEWWW_FIELD_SEND;
    }
    if (n->tag != OW_INPUT) return 0;
    if (attr_is(n, "type", "hidden")) return -1;
    if (attr_is(n, "type", "submit") || attr_is(n, "type", "image")) return ONEWWW_FIELD_SEND;
    if (attr_is(n, "type", "button") || attr_is(n, "type", "reset") ||
        attr_is(n, "type", "file")) return 0;
    if (attr_is(n, "type", "checkbox")) return ONEWWW_FIELD_CHECK;
    if (attr_is(n, "type", "radio")) return ONEWWW_FIELD_RADIO;
    if (attr_is(n, "type", "password")) return ONEWWW_FIELD_PASSWORD;
    return ONEWWW_FIELD_TEXT;
}

/* The n-th <option> below a node, in document order. */
static const onewww_node_t *option_at(const onewww_node_t *n, unsigned *index)
{
    for (const onewww_node_t *c = n->first; c != NULL; c = c->next) {
        const onewww_node_t *found;
        if (c->tag == OW_OPTION) {
            if (*index == 0U) return c;
            (*index)--;
        } else if (c->tag != OW_TEXT && (found = option_at(c, index)) != NULL) {
            return found;
        }
    }
    return NULL;
}

static unsigned option_count(const onewww_node_t *select)
{
    unsigned count = 0;
    for (;;) {
        unsigned index = count;
        if (option_at(select, &index) == NULL) return count;
        count++;
    }
}

/* The text of an element, with its white space collapsed. */
static void node_text(const onewww_node_t *n, char *out, size_t cap)
{
    size_t used = 0;

    for (const onewww_node_t *c = n != NULL ? n->first : NULL; c != NULL; c = c->next) {
        if (c->tag != OW_TEXT) continue;
        for (uint32_t i = 0; i < c->length && used + 1U < cap; i++) {
            const int space = (unsigned char)c->text[i] <= ' ';
            if (space && (used == 0U || out[used - 1U] == ' ')) continue;
            out[used++] = space ? ' ' : c->text[i];
        }
    }
    while (used != 0U && out[used - 1U] == ' ') used--;
    out[used] = '\0';
}

/* The number of a control, or -1. A draw pass notes each control that
 * shows; a measure pass only looks. */
static int field_for(const onewww_node_t *n, int create)
{
    static char text[FIELD_TEXT_MAX];
    int field = field_find(n);
    const int kind = field_kind(n);
    field_t *fd;

    if (field >= 0 || !create || kind <= 0 || s_fields == FIELD_MAX ||
        attr_has(n, "disabled")) return field;
    fd = &s_field[s_fields];
    fd->node = n; fd->type = (uint8_t)kind; fd->checked = (uint8_t)attr_has(n, "checked");
    fd->option = 0U; fd->value = NULL;
    if (kind == ONEWWW_FIELD_TEXT || kind == ONEWWW_FIELD_PASSWORD) {
        if (n->tag == OW_TEXTAREA) node_text(n, text, sizeof(text));
        else if (onewww_attr(n, "value", text, sizeof(text)) < 0) text[0] = '\0';
        fd->value = copy_text(text);
    } else if (kind == ONEWWW_FIELD_LIST) {
        const unsigned count = option_count(n);
        for (unsigned i = 0; i < count; i++) {
            unsigned index = i;
            if (attr_has(option_at(n, &index), "selected")) { fd->option = (uint16_t)i; break; }
        }
    }
    return (int)s_fields++;
}

static const onewww_node_t *form_of(const onewww_node_t *n)
{
    for (n = n->parent; n != NULL && n->tag != OW_FORM; n = n->parent) {}
    return n;
}

/* The text that a control shows. */
static void field_show(const onewww_node_t *n, int field, char *out, size_t cap)
{
    const int kind = field_kind(n);
    const field_t *fd = field >= 0 ? &s_field[field] : NULL;

    out[0] = '\0';
    if (kind == ONEWWW_FIELD_TEXT || kind == ONEWWW_FIELD_PASSWORD) {
        if (fd != NULL && fd->value != NULL) snprintf(out, cap, "%s", fd->value);
        else if (n->tag == OW_TEXTAREA) node_text(n, out, cap);
        else if (onewww_attr(n, "value", out, cap) < 0) out[0] = '\0';
    } else if (kind == ONEWWW_FIELD_LIST) {
        unsigned index = fd != NULL ? fd->option : 0U;
        node_text(option_at(n, &index), out, cap);
    } else if (kind == ONEWWW_FIELD_SEND) {
        if (onewww_attr(n, "value", out, cap) <= 0) snprintf(out, cap, "Send");
    }
}

int onewww_field_of_link(uint32_t link)
{
    const doc_t *d = view_doc();
    const char *href = link < d->links ? d->href[link] : "";
    int field = 0;

    if (strncmp(href, "field:", 6U) != 0) return -1;
    for (const char *p = href + 6; *p >= '0' && *p <= '9'; p++) field = field * 10 + (*p - '0');
    return (unsigned)field < s_fields ? field : -1;
}

int onewww_field_type(int field)
{
    return (unsigned)field < s_fields ? s_field[field].type : 0;
}

void onewww_field_label(int field, char *out, size_t cap)
{
    const onewww_node_t *n;

    out[0] = '\0';
    if ((unsigned)field >= s_fields || cap == 0U) return;
    n = s_field[field].node;
    if (onewww_attr(n, "placeholder", out, cap) > 0) return;
    if (onewww_attr(n, "aria-label", out, cap) > 0) return;
    if (onewww_attr(n, "title", out, cap) > 0) return;
    if (onewww_attr(n, "name", out, cap) > 0) return;
    snprintf(out, cap, "Text");
}

const char *onewww_field_value(int field)
{
    return (unsigned)field < s_fields && s_field[field].value != NULL ?
           s_field[field].value : "";
}

void onewww_field_set(int field, const char *text)
{
    char *copy;

    if ((unsigned)field >= s_fields) return;
    copy = copy_text(text);
    if (copy == NULL) return;
    free(s_field[field].value);
    s_field[field].value = copy;
}

void onewww_field_activate(int field)
{
    static char name[64], other[64];
    field_t *fd;

    if ((unsigned)field >= s_fields) return;
    fd = &s_field[field];
    if (fd->type == ONEWWW_FIELD_CHECK) fd->checked = !fd->checked;
    else if (fd->type == ONEWWW_FIELD_LIST) {
        const unsigned count = option_count(fd->node);
        fd->option = (uint16_t)(count != 0U ? (fd->option + 1U) % count : 0U);
    } else if (fd->type == ONEWWW_FIELD_RADIO) {
        /* One button of a group is on: those with the same name in the
         * same form. */
        if (onewww_attr(fd->node, "name", name, sizeof(name)) < 0) name[0] = '\0';
        for (unsigned i = 0; i < s_fields; i++) {
            if (s_field[i].type != ONEWWW_FIELD_RADIO ||
                form_of(s_field[i].node) != form_of(fd->node)) continue;
            if (onewww_attr(s_field[i].node, "name", other, sizeof(other)) < 0) other[0] = '\0';
            if (strcmp(name, other) == 0) s_field[i].checked = 0U;
        }
        fd->checked = 1U;
    }
}

int onewww_field_single(int field)
{
    const onewww_node_t *form;
    int texts = 0;

    if ((unsigned)field >= s_fields) return 0;
    form = form_of(s_field[field].node);
    if (form == NULL) return 0;
    for (unsigned i = 0; i < s_fields; i++) {
        if ((s_field[i].type == ONEWWW_FIELD_TEXT || s_field[i].type == ONEWWW_FIELD_PASSWORD) &&
            form_of(s_field[i].node) == form) texts++;
    }
    return texts == 1;
}

typedef struct {
    char *out;
    size_t used, cap;
    const onewww_node_t *pressed;       /* the button that sends the form */
    int failed;
} form_data_t;

static void encode(form_data_t *d, const char *text)
{
    static const char hex[] = "0123456789ABCDEF";

    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++) {
        if (d->used + 4U >= d->cap) { d->failed = 1; return; }
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
            *p == '-' || *p == '_' || *p == '.' || *p == '~') d->out[d->used++] = (char)*p;
        else if (*p == ' ') d->out[d->used++] = '+';
        else {
            d->out[d->used++] = '%';
            d->out[d->used++] = hex[*p >> 4];
            d->out[d->used++] = hex[*p & 15U];
        }
    }
    d->out[d->used] = '\0';
}

static void pair(form_data_t *d, const char *name, const char *value)
{
    if (name[0] == '\0') return;
    if (d->used != 0U && d->used + 2U < d->cap) d->out[d->used++] = '&';
    encode(d, name);
    if (d->used + 2U < d->cap) d->out[d->used++] = '=';
    d->out[d->used] = '\0';
    encode(d, value);
}

/* Adds the controls below a node that have a value to send. */
static void form_collect(form_data_t *d, const onewww_node_t *n)
{
    static char name[128], value[FIELD_TEXT_MAX];

    for (const onewww_node_t *c = n->first; c != NULL; c = c->next) {
        const int kind = c->tag == OW_TEXT ? 0 : field_kind(c);
        const int field = kind > 0 ? field_find(c) : -1;
        const field_t *fd = field >= 0 ? &s_field[field] : NULL;

        if (c->tag == OW_TEXT) continue;
        if (kind == 0 && c->tag != OW_INPUT && c->tag != OW_BUTTON) { form_collect(d, c); continue; }
        if (kind == 0 || attr_has(c, "disabled") ||
            onewww_attr(c, "name", name, sizeof(name)) <= 0) continue;
        if (kind < 0) {                                 /* hidden */
            if (onewww_attr(c, "value", value, sizeof(value)) < 0) value[0] = '\0';
            pair(d, name, value);
        } else if (kind == ONEWWW_FIELD_TEXT || kind == ONEWWW_FIELD_PASSWORD) {
            field_show(c, field, value, sizeof(value));
            pair(d, name, value);
        } else if (kind == ONEWWW_FIELD_CHECK || kind == ONEWWW_FIELD_RADIO) {
            if (fd != NULL ? fd->checked : attr_has(c, "checked")) {
                if (onewww_attr(c, "value", value, sizeof(value)) < 0) strcpy(value, "on");
                pair(d, name, value);
            }
        } else if (kind == ONEWWW_FIELD_LIST) {
            unsigned index = fd != NULL ? fd->option : 0U;
            const onewww_node_t *option = option_at(c, &index);
            if (option != NULL) {
                if (onewww_attr(option, "value", value, sizeof(value)) < 0) {
                    node_text(option, value, sizeof(value));
                }
                pair(d, name, value);
            }
        } else if (kind == ONEWWW_FIELD_SEND && c == d->pressed) {
            if (onewww_attr(c, "value", value, sizeof(value)) < 0) value[0] = '\0';
            pair(d, name, value);
        }
    }
}

int onewww_field_submit(int field, char *url, size_t url_cap,
                        char *body, size_t body_cap, int *post)
{
    static char action[HTTP_URL_MAX];
    const onewww_node_t *form;
    form_data_t d;
    char *cut;

    if ((unsigned)field >= s_fields || s_doc.base == NULL || url_cap < 16U || body_cap < 16U) return -1;
    form = form_of(s_field[field].node);
    if (form == NULL) return -1;
    memset(&d, 0, sizeof(d));
    d.out = body; d.cap = body_cap;
    body[0] = '\0';
    d.pressed = s_field[field].type == ONEWWW_FIELD_SEND ? s_field[field].node : NULL;
    for (unsigned i = 0; d.pressed == NULL && i < s_fields; i++) {     /* the first button */
        if (s_field[i].type == ONEWWW_FIELD_SEND && form_of(s_field[i].node) == form) {
            d.pressed = s_field[i].node;
        }
    }
    form_collect(&d, form);
    if (d.failed) return -1;
    if (onewww_attr(form, "action", action, sizeof(action)) <= 0) {
        snprintf(url, url_cap, "%s", s_doc.base);
    } else if (http_url_resolve(s_doc.base, action, url, url_cap) != 0) {
        return -1;
    }
    cut = strchr(url, '#');
    if (cut != NULL) *cut = '\0';
    *post = attr_is(form, "method", "post");
    if (!*post) {                       /* the data is the query of the address */
        cut = strchr(url, '?');
        if (cut != NULL) *cut = '\0';
        if (strlen(url) + 1U + d.used + 1U > url_cap) return -1;
        if (d.used != 0U) { strcat(url, "?"); strcat(url, body); }
        body[0] = '\0';
    }
    return 0;
}

/* ---- Document walk ----------------------------------------------------- */

#define MAX_COLS       24
#define MAX_TABLE_DEPTH 8
#define CELL_PAD       5

static void walk_children(flow_t *f, const onewww_node_t *parent, text_t tx);
static void walk_node(flow_t *f, const onewww_node_t *n, text_t tx);
static void walk_table(flow_t *f, const onewww_node_t *table);
static int column_run(flow_t *parent, const onewww_node_t *node, int self,
                      surface_t *s, int x0, int w, int y, int stop_at,
                      int *max_w, int *max_word);
static int flex_row(flow_t *f, const onewww_node_t *n, text_t tx);

static int attr_number(const onewww_node_t *n, const char *name, int fallback)
{
    char text[16];
    int value = 0, i = 0;

    if (n->length == 0U || onewww_attr(n, name, text, sizeof(text)) <= 0) return fallback;
    while (text[i] >= '0' && text[i] <= '9' && value < 10000) {
        value = value * 10 + (text[i++] - '0');
    }
    return i != 0 ? value : fallback;
}

static int is_hidden(const onewww_node_t *n)
{
    return n->style == NULL || n->style->display == OW_D_NONE;
}

/* Does an element have text that shows? */
static int has_text(const onewww_node_t *n)
{
    for (const onewww_node_t *c = n->first; c != NULL; c = c->next) {
        if (c->tag == OW_TEXT) {
            for (uint32_t i = 0; i < c->length; i++) {
                if ((unsigned char)c->text[i] > ' ') return 1;
            }
        } else if (!is_hidden(c) && has_text(c)) return 1;
    }
    return 0;
}

/* The text properties of an element, from its style. */
static void text_style(text_t *tx, const onewww_style_t *cs)
{
    if (cs->font_px >= 26U) tx->font = ONEWWW_FONT_H1;
    else if (cs->font_px >= 19U) tx->font = ONEWWW_FONT_H2;
    else if (cs->font_px <= 12U) tx->font = ONEWWW_FONT_SMALL;
    else if (cs->mono) tx->font = ONEWWW_FONT_MONO;
    else tx->font = (uint8_t)(ONEWWW_FONT_BODY + (cs->bold ? 1 : 0) + (cs->italic ? 2 : 0));
    tx->colour = cs->color; tx->underline = cs->underline;
    tx->pre = cs->pre; tx->nowrap = cs->nowrap;
}

static void flow_literal(flow_t *f, const text_t *tx, const char *text)
{
    flow_text(f, tx, text, strlen(text));
}

static void set_marker(flow_t *f, const text_t *tx)
{
    f->marker_colour = tx->colour;
    if (tx->list == 2U && tx->counter != NULL) {
        char text[MARKER_MAX];
        const int n = snprintf(text, sizeof(text), "%d.", (*tx->counter)++);
        f->marker_len = 0U;
        for (int i = 0; i < n && i < (int)MARKER_MAX; i++) {
            f->marker[f->marker_len++] = (uint8_t)text[i];
        }
    } else {
        f->marker[0] = 0x2022; f->marker_len = 1U;
    }
}

/* A block: margins, padding, alignment and paper around its content. */
static void walk_block(flow_t *f, const onewww_node_t *n, text_t tx,
                       const text_t *before)
{
    const onewww_style_t *cs = n->style;
    const int indent = f->indent, right = f->right;
    const uint8_t align = f->align;
    const uint16_t paper = f->paper;
    int y0, edge0, edge1, box0, box1, box = -1;

    flow_break(f, before, cs->margin_top);
    if (cs->clear != 0U) floats_clear(f, cs->clear);
    y0 = f->y;
    f->indent += cs->margin_left;
    f->right += cs->margin_right;
    if (n != f->float_root && n->tag != OW_TABLE) {
        /* width and max-width make the block narrower; auto margins say
         * on which side the free space goes. */
        const int space = f->w - f->indent - f->right;
        int want = cs->width > 0 ? cs->width : cs->width < 0 ? space * -cs->width / 100 : 0;
        const int most = cs->max_width > 0 ? cs->max_width :
                         cs->max_width < 0 ? space * -cs->max_width / 100 : 0;
        if (most > 0 && (want == 0 || want > most)) want = most;
        if (want >= 60 && want < space) {
            const int extra = space - want;
            if ((cs->margin_auto & OW_SIDE_LEFT) && (cs->margin_auto & OW_SIDE_RIGHT)) {
                f->indent += extra / 2; f->right += extra - extra / 2;
            } else if (cs->margin_auto & OW_SIDE_LEFT) f->indent += extra;
            else f->right += extra;
        }
    }
    edge0 = f->x0 + f->indent;
    edge1 = f->x0 + f->w - f->right;
    f->indent += cs->padding_left + ((cs->border & OW_SIDE_LEFT) && cs->padding_left < 4 ? 4 : 0);
    f->right += cs->padding_right + ((cs->border & OW_SIDE_RIGHT) && cs->padding_right < 4 ? 4 : 0);
    if (f->w - f->indent - f->right < 60) {     /* keep room for text */
        f->indent = indent; f->right = right;
        edge0 = f->x0 + indent; edge1 = f->x0 + f->w - right;
    }
    /* The paper of a block that touches the page margin goes on to the
     * edge of the screen, as on a page with no body margin. The text and
     * the borders keep the margin. */
    box0 = edge0; box1 = edge1;
    if (f->top_level && f->x0 == MARGIN && f->w == PAGE_WIDTH - 2 * MARGIN) {
        if (box0 == MARGIN) box0 = 0;
        if (box1 == PAGE_WIDTH - MARGIN) box1 = PAGE_WIDTH;
    }
    f->align = cs->align;
    if (cs->background != OW_NO_COLOR) {
        /* The box goes into the list before the content; its height is
         * known at the end. */
        f->paper = cs->background;
        if (f->s != NULL) box = dl_rect(f->s, box0, y0, box1 - box0, 0, cs->background);
    }
    if (cs->border & OW_SIDE_TOP) { f->y += 2; f->gap = 0; }
    if (cs->padding_top != 0) { f->y += cs->padding_top; f->gap = 0; }
    if (cs->display == OW_D_LIST_ITEM && !cs->list_none) set_marker(f, &tx);
    else f->marker_len = 0U;
    if (n->tag == OW_TABLE) {
        walk_table(f, n);
        f->gap = 0;
    } else if (cs->display == OW_D_FLEX && !cs->flex_column && flex_row(f, n, tx)) {
        f->gap = 0;
    } else {
        walk_children(f, n, tx);
    }
    flow_break(f, &tx, 0);
    f->marker_len = 0U;
    if (cs->padding_bottom != 0) { f->y += cs->padding_bottom; f->gap = 0; }
    if (cs->border & OW_SIDE_BOTTOM) { f->y += 2; f->gap = 0; }
    if (box >= 0) {
        (void)surface_need(f->s, (uint32_t)f->y);
        dl_box(f->s, box, box0, y0, box1 - box0, f->y - y0);
    }
    if (cs->border != 0U && f->s != NULL && surface_need(f->s, (uint32_t)f->y) == 0 &&
        f->y > y0) {
        if (cs->border & OW_SIDE_TOP) hline(f->s, edge0, edge1, y0, LV_LIGHT, INK_OF_ROW, f->paper);
        if (cs->border & OW_SIDE_BOTTOM) hline(f->s, edge0, edge1, f->y - 1, LV_LIGHT, INK_OF_ROW, f->paper);
        if (cs->border & OW_SIDE_LEFT) vline(f->s, edge0, y0, f->y, LV_LIGHT, f->paper);
        if (cs->border & OW_SIDE_RIGHT) vline(f->s, edge1 - 1, y0, f->y, LV_LIGHT, f->paper);
    }
    f->indent = indent; f->right = right; f->align = align; f->paper = paper;
    flow_break(f, &tx, cs->margin_bottom);
}

/* The picture of an <img>: its number, or -1. The layout notes each
 * picture that the page wants, so that the caller can get it. */
static int image_for(flow_t *f, const onewww_node_t *n)
{
    static char src[HTTP_URL_MAX], url[HTTP_URL_MAX];
    int image = image_find(n);

    if (image >= 0 || f->s == NULL || s_imgs == IMG_MAX || n->length == 0U) return image;
    if (s_doc.internal) {               /* the home page has one picture */
        return onewww_attr(n, "src", src, sizeof(src)) > 0 && strcmp(src, "builtin") == 0 ?
               builtin_image(n) : -1;
    }
    if (s_doc.base == NULL) return -1;
    if (onewww_attr(n, "src", src, sizeof(src)) <= 0 || strncmp(src, "data:", 5U) == 0) {
        /* A page that loads its pictures late keeps the address here. */
        if (onewww_attr(n, "data-src", src, sizeof(src)) <= 0) return -1;
    }
    if (strncmp(src, "data:", 5U) == 0 ||
        http_url_resolve(s_doc.base, src, url, sizeof(url)) != 0) return -1;
    /* A picture of one or two pixels counts visitors: leave it. */
    if (attr_number(n, "width", 100) < 4 || attr_number(n, "height", 100) < 4) return -1;
    s_img[s_imgs].url = copy_text(url);
    if (s_img[s_imgs].url == NULL) return -1;
    s_img[s_imgs].node = n; s_img[s_imgs].state = IMG_WANTED;
    return (int)s_imgs++;
}

/* The size of a picture on the page. Returns 0 when it cannot show.
 * The style sheet counts before the width and height attributes. A size
 * that is given for one side only keeps the proportions of the picture. */
static int image_box(const onewww_node_t *n, int image, int avail, int measure,
                     int column, int *w, int *h)
{
    const image_t *im = &s_img[image];
    const onewww_style_t *cs = n->style;
    const int32_t nw = im->natural_w, nh = im->natural_h;
    /* A measure run has no column width yet: a percentage then says
     * nothing, and the picture has its own size. */
    /* column: the picture floats and the column has its width already. */
    const int css_w = cs->width > 0 ? cs->width :
                      cs->width < 0 && column ? avail :
                      cs->width < 0 && !measure ? avail * -cs->width / 100 : 0;
    const int css_h = cs->height;
    const int most_w = cs->max_width > 0 ? cs->max_width :
                       cs->max_width < 0 && !measure ? avail * -cs->max_width / 100 : 0;
    int32_t dw, dh;

    if (im->state != IMG_LOADED || avail < 8 || nw == 0 || nh == 0) return 0;
    if (css_w > 0 && css_h > 0) { dw = css_w; dh = css_h; }
    else if (css_w > 0) { dw = css_w; dh = nh * dw / nw; }
    else if (css_h > 0) { dh = css_h; dw = nw * dh / nh; }
    else {
        const int attr_w = attr_number(n, "width", 0), attr_h = attr_number(n, "height", 0);
        if (attr_w > 0 && attr_h > 0) { dw = attr_w; dh = attr_h; }
        else if (attr_w > 0) { dw = attr_w; dh = nh * dw / nw; }
        else if (attr_h > 0) { dh = attr_h; dw = nw * dh / nh; }
        else { dw = nw; dh = nh; }
    }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    if (most_w > 0 && dw > most_w) { dh = dh * most_w / dw; dw = most_w; }
    if (cs->max_height > 0 && dh > cs->max_height) { dw = dw * cs->max_height / dh; dh = cs->max_height; }
    if (dw > avail) { dh = dh * avail / dw; dw = avail; }
    if (dh > 1200) { dw = dw * 1200 / dh; dh = 1200; }
    if (dw < 4 || dh < 4) return 0;
    *w = (int)dw; *h = (int)dh;
    return 1;
}

#define FLOAT_GAP 10
#define FLEX_GAP  8
#define FLEX_MAX  12

/* Puts a floating element at the side of the column. The lines that
 * follow go around it. Returns 0 when the element is too wide to float:
 * it is then a normal block. */
static int float_run(flow_t *f, const onewww_node_t *n, const text_t *before)
{
    const onewww_style_t *cs = n->style;
    const int total = f->w - f->indent - f->right;
    int width, pref = 0, x, bottom;

    flow_break(f, before, 0);
    if (f->y >= f->fl_bottom) f->fl_edge = 0;
    if (f->y >= f->fr_bottom) f->fr_edge = 0;
    if (cs->width > 0) width = cs->width;
    else if (cs->width < 0) width = total * -cs->width / 100;
    else {
        (void)column_run(f, n, 1, NULL, 0, 4000, 0, total, &pref, NULL);
        width = pref + 2;
    }
    if (cs->max_width > 0 && width > cs->max_width) width = cs->max_width;
    if (width < 8 || width > total * 3 / 4) return 0;
    if (width + FLOAT_GAP > f->w - left_inset(f) - right_inset(f)) {
        floats_clear(f, OW_SIDE_LEFT | OW_SIDE_RIGHT);
        f->fl_edge = f->fr_edge = 0;
    }
    if (cs->floating == OW_FLOAT_LEFT) x = f->x0 + left_inset(f);
    else x = f->x0 + f->w - right_inset(f) - width;
    bottom = column_run(f, n, 1, f->s, x, width, f->y, 0, NULL, NULL);
    if (cs->floating == OW_FLOAT_LEFT) {
        f->fl_edge = x - f->x0 + width + FLOAT_GAP;
        if (bottom + 4 > f->fl_bottom) f->fl_bottom = bottom + 4;
    } else {
        f->fr_edge = f->x0 + f->w - x + FLOAT_GAP;
        if (bottom + 4 > f->fr_bottom) f->fr_bottom = bottom + 4;
    }
    return 1;
}

/* A flex box: when its items fit side by side, each one is a column.
 * Returns 0 when they do not fit: the items are then blocks. */
static int flex_row(flow_t *f, const onewww_node_t *n, text_t tx)
{
    const onewww_node_t *item[FLEX_MAX];
    int pref[FLEX_MAX], count = 0, sum = 0, widest_word = 0, x, bottom;
    const int avail = f->w - left_inset(f) - right_inset(f);

    for (const onewww_node_t *c = n->first; c != NULL; c = c->next) {
        if (c->tag == OW_TEXT) {
            for (uint32_t i = 0; i < c->length; i++) {
                if ((unsigned char)c->text[i] > ' ') return 0;
            }
            continue;
        }
        if (c->style == NULL || c->style->display == OW_D_NONE) continue;
        if (count == FLEX_MAX) return 0;
        item[count++] = c;
    }
    if (count < 2) return 0;
    for (int i = 0; i < count; i++) {
        int word = 0;
        (void)column_run(f, item[i], 1, NULL, 0, 4000, 0, avail, &pref[i], &word);
        pref[i] += 2;
        sum += pref[i] + (i != 0 ? FLEX_GAP : 0);
        if (word > widest_word) widest_word = word;
        if (sum > avail) return 0;
    }
    if (f->s == NULL) {             /* measure only */
        if (f->indent + f->right + sum > f->max_w) f->max_w = f->indent + f->right + sum;
        if (widest_word > f->max_word) f->max_word = widest_word;
        return 1;
    }
    flow_break(f, &tx, 0);
    x = f->x0 + left_inset(f);
    bottom = f->y;
    for (int i = 0; i < count; i++) {
        const int end = column_run(f, item[i], 1, f->s, x, pref[i], f->y, 0, NULL, NULL);
        if (end > bottom) bottom = end;
        x += pref[i] + FLEX_GAP;
    }
    f->y = bottom;
    return 1;
}

static void walk_node(flow_t *f, const onewww_node_t *n, text_t tx)
{
    /* Each use ends before the walk goes deeper: one buffer is sufficient
     * and keeps the stack frames small. */
    static char text[HTTP_URL_MAX];
    const onewww_style_t *cs = n->style;
    const text_t before = tx;
    int counter;

    if (s_abort || f->over) return;
    if (n->tag == OW_TEXT) { flow_text(f, &tx, n->text, n->length); return; }
    if (cs == NULL || cs->display == OW_D_NONE) return;
    if (cs->floating != OW_FLOAT_NONE && f->s != NULL && n != f->float_root &&
        n->tag != OW_BR && float_run(f, n, &before)) return;
    if (f->s != NULL) {
        if (n->id[0] != '\0') anchor_add(n->id, f->y);
        if (n->tag == OW_A && n->length != 0U &&
            onewww_attr(n, "name", text, sizeof(text)) > 0) anchor_add(text, f->y);
    }
    flow_word_end(f, &before);
    text_style(&tx, cs);

    switch (n->tag) {
    case OW_BR:
        flow_flush(f, 1);
        return;
    case OW_WBR:
        return;
    case OW_HR:
        flow_break(f, &tx, 6);
        if (f->s != NULL && surface_need(f->s, (uint32_t)f->y + 1U) == 0) {
            hline(f->s, f->x0 + f->indent, f->x0 + f->w - f->right, f->y,
                  LV_LIGHT, INK_OF_ROW, f->paper);
        }
        f->y += 1; f->gap = 0;
        flow_break(f, &tx, 6);
        return;
    case OW_IMG:
        {
            int w, h;
            const int image = image_for(f, n);
            if (image >= 0 && image_box(n, image, f->w - left_inset(f) - right_inset(f),
                                        f->s == NULL, f->float_root == n, &w, &h)) {
                flow_image(f, &tx, image, w, h);
                return;
            }
        }
        tx.font = ONEWWW_FONT_SMALL;
        if (onewww_attr(n, "alt", text, 96U) > 0) {
            flow_literal(f, &tx, "["); flow_literal(f, &tx, text);
            flow_literal(f, &tx, "]");
        } else if (tx.link >= 0) {
            flow_literal(f, &tx, "[image]");
        }
        flow_word_end(f, &tx);
        return;
    case OW_INPUT: case OW_SELECT: case OW_TEXTAREA: case OW_BUTTON: {
        /* A form control: a word with a background that the user can
         * select as a link. Its address is "field:<number>". */
        const int kind = field_kind(n);
        const int field = kind > 0 ? field_for(n, f->s != NULL) : -1;
        const field_t *fd = field >= 0 ? &s_field[field] : NULL;
        char *out = text;
        size_t used = 0;
        int width = 0, shown = 0;

        if (kind < 0 || (n->tag == OW_BUTTON && !has_text(n))) return;
        tx.underline = 0U;
        tx.colour = readable(tx.colour, f->paper);
        tx.back = mix(f->paper, tx.colour, 1, kind == ONEWWW_FIELD_SEND || kind == 0 ? 3 : 6);
        if (kind > 0 && attr_has(n, "disabled")) tx.back = OW_NO_COLOR;
        if (fd != NULL && f->s != NULL) {
            char address[20];
            snprintf(address, sizeof(address), "field:%d", field);
            tx.link = link_add(address);
        } else if (kind > 0) {
            tx.link = f->s != NULL ? -1 : 0;
        }
        if (n->tag == OW_BUTTON) {              /* the text of the button is its content */
            tx.font = ONEWWW_FONT_BODY_B;
            flow_literal(f, &tx, "\xC2\xA0");
            walk_children(f, n, tx);
            flow_literal(f, &tx, "\xC2\xA0");
            flow_word_end(f, &tx);
            f->line->width += 3;
            return;
        }
        memcpy(out, "\xC2\xA0", 2U); used = 2U;
        if (kind == ONEWWW_FIELD_CHECK || kind == ONEWWW_FIELD_RADIO) {
            const int on = fd != NULL ? fd->checked : attr_has(n, "checked");
            used += (size_t)snprintf(out + used, 16U, "%s", kind == ONEWWW_FIELD_CHECK ?
                                     (on ? "\xE2\x9C\x93" : "\xC2\xA0\xC2\xA0") :
                                     (on ? "\xE2\x97\x8F" : "\xE2\x97\x8B"));
        } else {
            static char value[FIELD_TEXT_MAX];
            if (kind > 0) field_show(n, field, value, sizeof(value));
            else if (onewww_attr(n, "value", value, sizeof(value)) <= 0) strcpy(value, "Button");
            if (kind == ONEWWW_FIELD_TEXT || kind == ONEWWW_FIELD_PASSWORD) {
                width = n->tag == OW_TEXTAREA ? 30 : attr_number(n, "size", 18);
                if (width < 4) width = 4;
                if (width > 40) width = 40;
            }
            if (kind == ONEWWW_FIELD_SEND || kind == 0) tx.font = ONEWWW_FONT_BODY_B;
            for (const unsigned char *v = (const unsigned char *)value;
                 *v != '\0' && used + 8U < 400U && (width == 0 || shown < width); v++) {
                if ((*v & 0xC0U) != 0x80U) shown++;         /* a new character */
                if (kind == ONEWWW_FIELD_PASSWORD) {
                    if ((*v & 0xC0U) != 0x80U) { memcpy(out + used, "\xE2\x97\x8F", 3U); used += 3U; }
                } else if (*v <= ' ') { memcpy(out + used, "\xC2\xA0", 2U); used += 2U; }
                else out[used++] = (char)*v;
            }
            for (; shown < width; shown++) { memcpy(out + used, "\xC2\xA0", 2U); used += 2U; }
            if (kind == ONEWWW_FIELD_LIST) { memcpy(out + used, "\xC2\xA0\xE2\x96\xBE", 5U); used += 5U; }
        }
        memcpy(out + used, "\xC2\xA0", 3U);
        flow_literal(f, &tx, out);
        flow_word_end(f, &tx);
        f->line->width += 3;
        return;
    }
    case OW_A:
        if (n->length != 0U && onewww_attr(n, "href", text, sizeof(text)) > 0) {
            /* A measure pass needs only to know that this is a link. */
            tx.link = f->s != NULL ? link_add(text) : 0;
        }
        break;
    case OW_UL: case OW_OL:
        counter = attr_number(n, "start", 1);
        tx.list = n->tag == OW_OL ? 2U : 1U;
        tx.counter = &counter;
        break;
    default:
        break;
    }
    if (cs->display != OW_D_INLINE || n->tag == OW_TABLE ||
        cs->floating != OW_FLOAT_NONE) {
        walk_block(f, n, tx, &before);
    } else {
        f->line->width += cs->margin_left + cs->padding_left;
        if (cs->background != OW_NO_COLOR) tx.back = cs->background;
        walk_children(f, n, tx);
        flow_word_end(f, &tx);
        f->line->width += cs->margin_right + cs->padding_right;
    }
}

static void walk_children(flow_t *f, const onewww_node_t *parent, text_t tx)
{
    for (const onewww_node_t *c = parent->first; c != NULL; c = c->next) {
        walk_node(f, c, tx);
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
            if (n->tag == OW_TR && !is_hidden(n)) return n;
            if (group == NULL && is_row_group(n->tag) && n->first != NULL &&
                !is_hidden(n)) {
                group = n; n = n->first;
                if (n->tag == OW_TR && !is_hidden(n)) return n;
            }
        }
        if (group == NULL) return NULL;
        n = group->next; group = NULL;
    }
}

/* Lays a node out in a column of its own. self = 0: the children of a
 * table cell. self = 1: the node itself (a float or a flex item).
 * s = NULL measures only; such a run ends early when a line is wider than
 * stop_at. Returns the end row; gives the widths of the content. */
static int column_run(flow_t *parent, const onewww_node_t *node, int self,
                      surface_t *s, int x0, int w, int y, int stop_at,
                      int *max_w, int *max_word)
{
    const onewww_style_t *cs = node->style;
    const onewww_style_t *up = node->parent != NULL ? node->parent->style : NULL;
    flow_t f;
    text_t tx = s_plain;
    line_t *line = malloc(sizeof(*line));
    uint16_t paper = parent->paper;

    if (max_w != NULL) *max_w = 0;
    if (max_word != NULL) *max_word = 0;
    if (line == NULL) return y;
    memset(&f, 0, sizeof(f));
    memset(line, 0, sizeof(*line));
    f.s = s; f.line = line; f.x0 = x0; f.w = w; f.y = y;
    f.depth = parent->depth + 1; f.gap = NO_GAP; f.stop_at = stop_at;
    if (self) {
        f.float_root = node; f.paper = paper; f.align = parent->align;
        if (up != NULL) text_style(&tx, up);
        walk_node(&f, node, tx);
    } else {
        /* The paper of the cell, of its row, or of the column around it. */
        if (cs->background != OW_NO_COLOR) paper = cs->background;
        else if (up != NULL && up->background != OW_NO_COLOR) paper = up->background;
        f.paper = paper; f.align = cs->align;
        text_style(&tx, cs);
        walk_children(&f, node, tx);
    }
    flow_break(&f, &tx, 0);
    floats_clear(&f, OW_SIDE_LEFT | OW_SIDE_RIGHT);
    if (max_w != NULL) *max_w = f.max_w;
    if (max_word != NULL) *max_word = f.max_word;
    free(line);
    return f.y;
}

static int cell_run(flow_t *parent, const onewww_node_t *cell, surface_t *s,
                    int x0, int w, int y, int *max_w, int *max_word)
{
    return column_run(parent, cell, 0, s, x0, w, y, 0, max_w, max_word);
}

static void walk_table(flow_t *f, const onewww_node_t *table)
{
    const int avail = f->w - f->indent - f->right;
    int x0 = f->x0 + f->indent;
    int pref[MAX_COLS] = { 0 }, least[MAX_COLS] = { 0 }, width[MAX_COLS];
    int columns = 0, total = 0, total_least = 0, y, border, cell_border = 0;
    const onewww_node_t *row, *c;
    grid_t grid;

    /* A caption is a block before the grid. */
    for (c = table->first; c != NULL; c = c->next) {
        if (c->tag == OW_CAPTION) walk_node(f, c, s_plain);
    }
    y = f->y;
    /* Pass 1: the widest line and the widest word of each column. */
    memset(&grid, 0, sizeof(grid));
    for (row = row_next(table, NULL); row != NULL; row = row_next(table, row)) {
        int used;
        if (s_abort) break;
        for (c = row->first; c != NULL; c = c->next) {
            int span, start, w = 0, word = 0;
            if (c->tag != OW_TD && c->tag != OW_TH) continue;
            if (is_hidden(c)) continue;
            span = attr_number(c, "colspan", 1);
            start = grid_place(&grid, &span, attr_number(c, "rowspan", 1));
            if (start < 0) break;
            if (f->depth >= MAX_TABLE_DEPTH) continue;
            if (c->style->border != 0U) cell_border = 1;
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
        /* No grid: show the content as blocks. */
        for (c = table->first; c != NULL; c = c->next) {
            if (c->tag != OW_CAPTION) walk_node(f, c, s_plain);
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
        if (f->indent + f->right + total > f->max_w) f->max_w = f->indent + f->right + total;
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
    {
        /* A table with a width that is more than its content grows. */
        const onewww_style_t *ts = table->style;
        int want = ts->width > 0 ? ts->width : ts->width < 0 ? avail * -ts->width / 100 : 0;
        if (want > avail) want = avail;
        if (want > total && table != f->float_root) {
            const int extra = want - total, before = total;
            total = 0;
            for (int i = 0; i < columns; i++) {
                width[i] += extra * width[i] / before;
                total += width[i];
            }
        }
        border = attr_number(table, "border", 0) != 0 || cell_border;
        if (total < avail &&
            (((ts->margin_auto & OW_SIDE_LEFT) && (ts->margin_auto & OW_SIDE_RIGHT)) ||
             (table->parent != NULL && table->parent->style != NULL &&
              table->parent->style->align == OW_ALIGN_CENTER))) {
            x0 += (avail - total) / 2;
        }
    }

    /* Pass 2: draw. */
    memset(&grid, 0, sizeof(grid));
    for (row = row_next(table, NULL); row != NULL; row = row_next(table, row)) {
        int bottom = y + 2 * CELL_PAD;
        int edge[MAX_COLS + 1], edges = 0;
        const uint16_t row_paper = row->style != NULL ? row->style->background : OW_NO_COLOR;
        int box[MAX_COLS + 1], boxes = 0;

        if (s_abort || surface_need(f->s, (uint32_t)bottom) != 0) break;
        for (c = row->first; c != NULL; c = c->next) {
            int span, start, cx = x0, cw = 0, cy;
            if (c->tag != OW_TD && c->tag != OW_TH) continue;
            if (is_hidden(c)) continue;
            span = attr_number(c, "colspan", 1);
            start = grid_place(&grid, &span, attr_number(c, "rowspan", 1));
            if (start < 0 || start >= columns) break;
            for (int i = 0; i < start; i++) cx += width[i];
            for (int i = 0; i < span && start + i < columns; i++) cw += width[start + i];
            if (c->style->background != OW_NO_COLOR || row_paper != OW_NO_COLOR) {
                box[boxes] = dl_rect(f->s, cx, y, cw, 0, c->style->background != OW_NO_COLOR ?
                                     c->style->background : row_paper);
                if (boxes < MAX_COLS) boxes++;
            }
            cy = cell_run(f, c, f->s, cx + CELL_PAD, cw - 2 * CELL_PAD,
                          y + CELL_PAD / 2 + 1, NULL, NULL);
            if (cy + CELL_PAD / 2 + 1 > bottom) bottom = cy + CELL_PAD / 2 + 1;
            if (edges <= MAX_COLS) edge[edges++] = cx;
        }
        (void)grid_row_end(&grid);
        if (surface_need(f->s, (uint32_t)bottom + 1U) != 0) break;
        for (int i = 0; i < boxes; i++) {           /* the cells are as high as the row */
            if (box[i] >= 0) f->s->item[box[i]].h = (uint16_t)(bottom - y);
        }
        if (border) {
            hline(f->s, x0, x0 + total + 1, y, LV_LIGHT, INK_OF_ROW, f->paper);
            for (int i = 0; i < edges; i++) vline(f->s, edge[i], y, bottom, LV_LIGHT, f->paper);
            vline(f->s, x0 + total, y, bottom, LV_LIGHT, f->paper);
        }
        y = bottom;
    }
    if (border && surface_need(f->s, (uint32_t)y + 1U) == 0) {
        hline(f->s, x0, x0 + total + 1, y, LV_LIGHT, INK_OF_ROW, f->paper);
        y++;
    }
    f->y = y;
}

/* ---- Public ------------------------------------------------------------ */

#define MAX_SHEETS 16U

static struct {
    onewww_dom_t *dom;
    struct {
        const onewww_node_t *node;      /* <style>: its text is the sheet */
        char *url;                      /* <link>: the address ... */
        char *text;                     /* ... and the text, when it came */
        size_t length;
    } sheet[MAX_SHEETS];
    unsigned sheets;
    uint8_t styled;                     /* the elements have their styles */
    uint16_t paper, ink;                /* of the page */
} s_work;

/* While a page is laid out again (a picture came, a field changed), the
 * screen shows the layout before: the front copy. */
static struct {
    surface_t page;
    doc_t doc;
    uint8_t valid;
} s_front;

static void sheets_free(void)
{
    for (unsigned i = 0; i < s_work.sheets; i++) {
        free(s_work.sheet[i].url); free(s_work.sheet[i].text);
    }
    memset(s_work.sheet, 0, sizeof(s_work.sheet));
    s_work.sheets = 0U;
}

static void front_drop(void)
{
    if (!s_front.valid) return;
    surface_free(&s_front.page);
    doc_lists_free(&s_front.doc);
    s_front.valid = 0U;
}

/* The layout that is on the screen becomes the front copy. */
static void front_take(void)
{
    front_drop();
    s_front.page = s_page;
    s_front.doc = s_doc;
    s_front.valid = 1U;
    memset(&s_page, 0, sizeof(s_page));
    s_page.paper = s_work.paper; s_page.ink = s_work.ink;
    s_doc.href = NULL; s_doc.link_row = NULL; s_doc.rect = NULL; s_doc.anchor = NULL;
    s_doc.links = s_doc.links_cap = s_doc.rects = s_doc.rects_cap = 0U;
    s_doc.anchors = s_doc.anchors_cap = 0U;
}

/* A new layout stopped: the front copy is the page again. */
static void front_restore(void)
{
    surface_free(&s_page);
    doc_lists_free(&s_doc);
    s_page = s_front.page;
    s_doc = s_front.doc;
    s_front.valid = 0U;
}

void onewww_page_free(void)
{
    front_drop();
    surface_free(&s_page);
    doc_lists_free(&s_doc);
    memset(&s_doc, 0, sizeof(s_doc));
    sheets_free();
    images_free();
    fields_free();
    if (s_work.dom != NULL) onewww_dom_free(s_work.dom);
    memset(&s_work, 0, sizeof(s_work));
    s_work.paper = 0x0FFF;
    s_page.paper = 0x0FFF; s_page.ink = 0x0000;
}

/* Finds the style sheets of the page, in document order. */
static void find_sheets(const onewww_node_t *n)
{
    static char text[HTTP_URL_MAX], url[HTTP_URL_MAX];

    for (; n != NULL; n = n->next) {
        if (s_work.sheets == MAX_SHEETS) return;
        if (n->tag == OW_STYLE && n->first != NULL) {
            if (onewww_attr(n, "media", text, sizeof(text)) > 0 &&
                strstr(text, "print") != NULL && strstr(text, "screen") == NULL) continue;
            s_work.sheet[s_work.sheets++].node = n->first;
        } else if (n->tag == OW_LINK && s_doc.base != NULL) {
            if (onewww_attr(n, "rel", text, sizeof(text)) <= 0 ||
                (strstr(text, "stylesheet") == NULL && strstr(text, "STYLESHEET") == NULL) ||
                strstr(text, "alternate") != NULL) continue;
            if (onewww_attr(n, "media", text, sizeof(text)) > 0 &&
                strstr(text, "print") != NULL && strstr(text, "screen") == NULL &&
                strstr(text, "all") == NULL) continue;
            if (onewww_attr(n, "href", text, sizeof(text)) <= 0 ||
                http_url_resolve(s_doc.base, text, url, sizeof(url)) != 0) continue;
            s_work.sheet[s_work.sheets].url = copy_text(url);
            if (s_work.sheet[s_work.sheets].url != NULL) s_work.sheets++;
        } else if (n->tag != OW_TEXT && n->first != NULL) {
            find_sheets(n->first);
        }
    }
}

int onewww_page_parse(const uint8_t *html, size_t length,
                      const char *content_type, const char *base_url,
                      int internal)
{
    size_t n = 0;

    onewww_page_free();
    s_abort = 0U;
    s_work.dom = onewww_dom_parse(html, length, content_type);
    if (s_work.dom == NULL) return -1;
    s_doc.base = base_url; s_doc.internal = internal != 0;
    /* The title, with its white space collapsed. */
    for (const char *t = onewww_dom_title(s_work.dom);
         *t != '\0' && n + 1U < sizeof(s_doc.title); t++) {
        const int space = (unsigned char)*t <= ' ';
        if (space && (n == 0U || s_doc.title[n - 1U] == ' ')) continue;
        s_doc.title[n++] = space ? ' ' : *t;
    }
    s_doc.title[n] = '\0';
    find_sheets(onewww_dom_root(s_work.dom));
    return 0;
}

unsigned onewww_page_sheets(void) { return s_work.sheets; }

const char *onewww_page_sheet_url(unsigned sheet)
{
    return sheet < s_work.sheets && s_work.sheet[sheet].text == NULL ?
           s_work.sheet[sheet].url : NULL;
}

void onewww_page_sheet_text(unsigned sheet, const char *text, size_t length)
{
    if (sheet >= s_work.sheets || s_work.sheet[sheet].url == NULL ||
        s_work.sheet[sheet].text != NULL || length == 0U) return;
    s_work.sheet[sheet].text = malloc(length);
    if (s_work.sheet[sheet].text == NULL) return;
    memcpy(s_work.sheet[sheet].text, text, length);
    s_work.sheet[sheet].length = length;
}

/* Gives each element its style. Returns 0, -1 or -2 (stopped). */
static int styles_apply(void)
{
    onewww_css_t *css = onewww_css_new();
    onewww_node_t *root = onewww_dom_root(s_work.dom), *body = root->first;
    int result;

    if (css == NULL) return -1;
    for (unsigned i = 0; i < s_work.sheets; i++) {
        if (s_work.sheet[i].node != NULL) {
            onewww_css_add(css, s_work.sheet[i].node->text, s_work.sheet[i].node->length);
        } else if (s_work.sheet[i].text != NULL) {
            onewww_css_add(css, s_work.sheet[i].text, s_work.sheet[i].length);
        }
        pause_point();
    }
    result = s_abort ? -2 : onewww_css_resolve(css, s_work.dom);
    onewww_css_free(css);
    if (result != 0) return s_abort || result == -2 ? -2 : -1;
    sheets_free();
    /* The page has its own margin; the paper and ink of <body> (or of
     * <html>) are those of the page. */
    for (onewww_node_t *e = root; e != NULL; e = e == root ? body : NULL) {
        onewww_style_t *cs = e->style;
        cs->margin_top = cs->margin_bottom = cs->margin_left = cs->margin_right = 0;
        cs->padding_top = cs->padding_bottom = cs->padding_left = cs->padding_right = 0;
        cs->width = cs->max_width = 0; cs->border = 0U; cs->floating = OW_FLOAT_NONE;
        if (cs->background != OW_NO_COLOR) s_work.paper = cs->background;
        cs->background = OW_NO_COLOR;
        cs->display = OW_D_BLOCK;
    }
    s_work.ink = readable(body->style->color, s_work.paper);
    s_work.styled = 1U;
    return 0;
}

int onewww_page_layout(void)
{
    const int again = s_work.styled;
    flow_t f;
    line_t *line;
    int result = 0;

    if (s_work.dom == NULL) return -1;
    s_abort = 0U;
    if (again) {
        front_take();
    } else {
        result = styles_apply();
        if (result != 0) { s_abort = 0U; onewww_page_free(); return result; }
        s_page.paper = s_work.paper; s_page.ink = s_work.ink;
    }
    line = malloc(sizeof(*line));
    if (line == NULL) {
        if (again) front_restore(); else onewww_page_free();
        return -1;
    }
    memset(&f, 0, sizeof(f));
    memset(line, 0, sizeof(*line));
    f.s = &s_page; f.line = line; f.x0 = MARGIN; f.w = PAGE_WIDTH - 2 * MARGIN;
    f.top_level = 1U; f.gap = NO_GAP; f.paper = s_page.paper;
    walk_node(&f, onewww_dom_root(s_work.dom), s_plain);
    flow_break(&f, &s_plain, 0);
    floats_clear(&f, OW_SIDE_LEFT | OW_SIDE_RIGHT);
    (void)surface_need(&s_page, (uint32_t)f.y + 8U);
    free(line);
    if (s_abort) {
        s_abort = 0U;
        if (again) front_restore(); else onewww_page_free();
        return -2;
    }
    front_drop();
    return 0;
}

int onewww_page_build(const uint8_t *html, size_t length,
                      const char *content_type, const char *base_url,
                      int internal)
{
    if (onewww_page_parse(html, length, content_type, base_url, internal) != 0) {
        return -1;
    }
    return onewww_page_layout();
}

/* What the screen shows: the front copy while a new layout is made. */
static const surface_t *view_page(void) { return s_front.valid ? &s_front.page : &s_page; }
static const doc_t *view_doc(void) { return s_front.valid ? &s_front.doc : &s_doc; }

uint32_t onewww_page_rows(void) { return view_page()->rows; }
const char *onewww_page_title(void) { return s_doc.title; }
uint32_t onewww_link_count(void) { return view_doc()->links; }

const char *onewww_link_href(uint32_t link)
{
    const doc_t *d = view_doc();
    return link < d->links ? d->href[link] : "";
}

int onewww_link_at(int x, uint32_t row)
{
    const doc_t *d = view_doc();

    for (uint32_t i = 0; i < d->rects; i++) {
        const rect_t *r = &d->rect[i];
        if (row >= r->y && row < r->y + r->h && x >= r->x0 && x < r->x1) {
            return (int)r->link;
        }
    }
    return -1;
}

uint32_t onewww_link_row(uint32_t link)
{
    const doc_t *d = view_doc();
    return link < d->links ? d->link_row[link] : UINT32_MAX;
}

int onewww_link_near(uint32_t row)
{
    const doc_t *d = view_doc();
    int best = -1;
    uint32_t best_d = UINT32_MAX;

    for (uint32_t i = 0; i < d->links; i++) {
        uint32_t distance;
        if (d->link_row[i] == UINT32_MAX) continue;
        distance = d->link_row[i] > row ? d->link_row[i] - row : row - d->link_row[i];
        if (distance < best_d) { best_d = distance; best = (int)i; }
    }
    return best;
}

int onewww_anchor_row(const char *name)
{
    const doc_t *d = view_doc();

    for (uint32_t i = 0; i < d->anchors; i++) {
        if (strcmp(d->anchor[i].name, name) == 0) return (int)d->anchor[i].row;
    }
    return -1;
}

/* ---- Drawing ----------------------------------------------------------- */

#define STATUS_PAPER 0x0DDD

static uint32_t pixel_of(uint16_t c)
{
    return 0xFF000000U | (((c >> 8) & 15U) * 17U << 16) | (((c >> 4) & 15U) * 17U << 8) |
           ((c & 15U) * 17U);
}

/* Draws a character into `rows` rows of the picture. A pixel that the
 * letter covers in part is a mix with the colour that is there: smooth
 * edges on any background. Returns the advance. */
static int frame_glyph(uint32_t *frame, int rows, const onewww_font_t *f,
                       uint16_t code, int x, int top, uint32_t colour)
{
    const onewww_glyph_t *g = glyph_find(f, code);
    const uint8_t *bits = f->bits + g->offset;
    const unsigned stride = ((unsigned)g->w + 3U) >> 2;
    const uint32_t cr = (colour >> 16) & 255U, cg = (colour >> 8) & 255U, cb = colour & 255U;

    for (unsigned gy = 0; gy < g->h; gy++) {
        const int y = top + g->y + (int)gy;
        if (y < 0 || y >= rows) continue;
        for (unsigned gx = 0; gx < g->w; gx++) {
            const unsigned level =
                (bits[gy * stride + (gx >> 2)] >> (6U - 2U * (gx & 3U))) & 3U;
            const int px = x + g->x + (int)gx;
            uint32_t *dst;
            if (level == 0U || px < 0 || px >= PAGE_WIDTH) continue;
            dst = &frame[y * PAGE_WIDTH + px];
            if (level == 3U) *dst = colour;
            else {
                const uint32_t d = *dst;
                const uint32_t r = (d >> 16) & 255U, gr = (d >> 8) & 255U, b = d & 255U;
                const int n = (int)level;
                *dst = 0xFF000000U |
                       (uint32_t)((int)r + ((int)cr - (int)r) * n / 3) << 16 |
                       (uint32_t)((int)gr + ((int)cg - (int)gr) * n / 3) << 8 |
                       (uint32_t)((int)b + ((int)cb - (int)b) * n / 3);
            }
        }
    }
    return g->advance;
}

static void frame_rect(uint32_t *frame, int rows, int x, int y, int w, int h,
                       uint32_t colour)
{
    int x1 = x + w, y1 = y + h;

    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > PAGE_WIDTH) x1 = PAGE_WIDTH;
    if (y1 > rows) y1 = rows;
    for (; y < y1; y++) {
        uint32_t *row = frame + y * PAGE_WIDTH;
        for (int px = x; px < x1; px++) row[px] = colour;
    }
}

static int frame_text(uint32_t *frame, int rows, const char *text, int x,
                      int y, int limit, uint32_t colour, int draw)
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
            if (draw) (void)frame_glyph(frame, rows, font, 0x2026, x, y, colour);
            break;
        }
        if (draw) (void)frame_glyph(frame, rows, font, (uint16_t)code, x, y, colour);
        x += advance;
    }
    return x;
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
                    uint32_t *frame)
{
    const surface_t *page = view_page();
    const doc_t *doc = view_doc();
    const int view = (int)ONEWWW_PAGE_VIEW;
    const int32_t first = (int32_t)top, last = (int32_t)top + view;
    uint32_t *status = frame + (size_t)view * PAGE_WIDTH;
    int right_x = PAGE_WIDTH - 6;

    frame_rect(frame, view, 0, 0, PAGE_WIDTH, view, pixel_of(page->paper));
    for (uint32_t i = 0; i < page->items; i++) {
        const ditem_t *it = &page->item[i];
        const int y = (int)(it->y - first);

        if (it->y >= last || it->y + (int32_t)it->h <= first) continue;
        if (it->kind == D_RECT) {
            frame_rect(frame, view, it->x, y, it->w, it->h, pixel_of(it->colour));
        } else if (it->kind == D_TEXT) {
            const onewww_font_t *font = &onewww_fonts[it->font];
            const uint32_t colour = pixel_of(it->colour);
            int x = it->x;
            for (unsigned k = 0; k < it->count; k++) {
                x += frame_glyph(frame, view, font, page->code[it->data + k], x, y, colour);
            }
        } else {
            image_blit(frame, view, (unsigned)it->data, it->x, y, it->w, it->h);
        }
    }
    /* The selected link: its colours turn round. */
    for (uint32_t i = 0; selected >= 0 && i < doc->rects; i++) {
        const rect_t *rc = &doc->rect[i];
        if (rc->link != (uint32_t)selected) continue;
        for (int y = (int)((int32_t)rc->y - first); y < (int)((int32_t)rc->y - first) + rc->h; y++) {
            if (y < 0 || y >= view) continue;
            for (int x = rc->x0 - 1; x <= rc->x1; x++) {
                if (x >= 0 && x < PAGE_WIDTH) frame[y * PAGE_WIDTH + x] ^= 0x00FFFFFFU;
            }
        }
    }
    /* The status line. */
    frame_rect(status, (int)ONEWWW_STATUS_ROWS, 0, 0, PAGE_WIDTH,
               (int)ONEWWW_STATUS_ROWS, pixel_of(STATUS_PAPER));
    frame_rect(status, (int)ONEWWW_STATUS_ROWS, 0, 0, PAGE_WIDTH, 1, pixel_of(0x0555));
    if (right != NULL && right[0] != '\0') {
        right_x -= frame_text(status, (int)ONEWWW_STATUS_ROWS, right, 0, 2, PAGE_WIDTH, 0U, 0);
        (void)frame_text(status, (int)ONEWWW_STATUS_ROWS, right, right_x, 2, PAGE_WIDTH,
                         0xFF000000U, 1);
        right_x -= 12;
    }
    (void)frame_text(status, (int)ONEWWW_STATUS_ROWS, left, 6, 2, right_x - 8, 0xFF000000U, 1);
    /* The mouse pointer, over the page and the status line. */
    for (unsigned py = 0; pointer_x >= 0 && py < POINTER_ROWS; py++) {
        const int y = pointer_y + (int)py;
        if (y < 0 || y >= (int)ONEWWW_VIEW_ROWS) continue;
        for (int px = 0; s_pointer[py][px] != '\0'; px++) {
            const int x = pointer_x + px;
            if (s_pointer[py][px] == ' ' || x < 0 || x >= PAGE_WIDTH) continue;
            frame[y * PAGE_WIDTH + x] = s_pointer[py][px] == 'X' ? 0xFF000000U : 0xFFFFFFFFU;
        }
    }
}
