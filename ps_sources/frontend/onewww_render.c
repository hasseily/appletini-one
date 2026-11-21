/* ONEWWW page renderer: lays a styled document tree out with proportional
 * fonts and draws it into Super Hi-Res rows. Blocks, lists, tables with
 * automatic column widths, links, colours.
 * No hardware access: the file also compiles on the host. */
#include "onewww_render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http_core.h"
#include "onewww_css.h"
#include "onewww_dom.h"
#include "onewww_font.h"

#define ROW_BYTES   160U
#define PAGE_WIDTH  640
#define MARGIN      12
#define ROW_CHUNK   512U
#define MAX_ROWS    60000U

/* Row colours. A 640-mode row shows four colours; the four colour groups
 * of the 640 mode hold the same four. Each text row has its own paper and
 * ink. A row with one text colour has two mixed colours between paper and
 * ink, for smooth letter edges. A row with a second text colour (the
 * accent, for example a link) has one mixed colour, ink and the accent.
 * The first text on a row sets its paper and ink. */
typedef struct {
    uint16_t paper, ink, accent;        /* 0RGB; accent OW_NO_COLOR: none */
} rowpal_t;

#define PAL_MAX     2048U
#define PAL_STATUS  0x7FFFU             /* a row of the status line */
#define PAL_PIC     0x8000U             /* + n: a 320-mode row of picture n */
#define PIC_MAX     48U                 /* sixteen-colour pictures on a page */
#define SCB_640     0x80U
#define SCB_USED    0x01U               /* the row has its colours */
#define INK_OF_ROW  OW_NO_COLOR         /* draw with the ink of the row */
enum { LV_LIGHT = 1, LV_DARK = 2, LV_INK = 3 };

static const uint16_t s_picture_palette[16] = {
    0x0000, 0x0113, 0x0226, 0x034A, 0x047D, 0x07AF, 0x0FFF, 0x0FE8,
    0x0FB3, 0x0F71, 0x0C32, 0x0713, 0x0141, 0x0272, 0x04A4, 0x0888,
};

typedef struct {
    uint8_t *px;
    uint8_t *scb;       /* SCB_640 and SCB_USED */
    uint16_t *pal;      /* index in s_pal, PAL_STATUS, or PAL_PIC + n */
    uint32_t rows;
    uint32_t cap;
    uint8_t fixed;
} surface_t;

static surface_t s_page;
static uint8_t s_status_px[ONEWWW_STATUS_ROWS * ROW_BYTES];
static uint8_t s_status_scb[ONEWWW_STATUS_ROWS];
static uint16_t s_status_pal[ONEWWW_STATUS_ROWS];

/* Entry 0 is the page: its paper and ink are those of <body>. */
static rowpal_t s_pal[PAL_MAX] = { { 0x0FFF, 0x0000, OW_NO_COLOR } };
static uint32_t s_pals = 1U;
/* The sixteen colours of each picture that has rows of its own. */
static uint16_t s_pic[PIC_MAX][16];
static unsigned s_pics;

static int color_distance(uint16_t a, uint16_t b)
{
    const int dr = (int)((a >> 8) & 15) - (int)((b >> 8) & 15);
    const int dg = (int)((a >> 4) & 15) - (int)((b >> 4) & 15);
    const int db = (int)(a & 15) - (int)(b & 15);
    return (dr < 0 ? -dr : dr) + (dg < 0 ? -dg : dg) + (db < 0 ? -db : db);
}

static uint16_t pal_get(uint16_t paper, uint16_t ink, uint16_t accent)
{
    uint32_t best = 0U;
    int best_d = 1 << 30;

    for (uint32_t i = s_pals; i-- > 0U;) {       /* new entries are likely */
        const rowpal_t *p = &s_pal[i];
        int d;
        if (p->paper == paper && p->ink == ink && p->accent == accent) return (uint16_t)i;
        d = color_distance(p->paper, paper) * 4 + color_distance(p->ink, ink) +
            (p->accent == accent ? 0 : 2);
        if (d < best_d) { best_d = d; best = i; }
    }
    if (s_pals == PAL_MAX) return (uint16_t)best;
    s_pal[s_pals].paper = paper; s_pal[s_pals].ink = ink; s_pal[s_pals].accent = accent;
    return (uint16_t)s_pals++;
}

static int surface_need(surface_t *s, uint32_t rows)
{
    uint32_t cap;
    uint8_t *px, *scb;
    uint16_t *pal;

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
        pal = realloc(s->pal, (size_t)cap * sizeof(uint16_t));
        if (pal == NULL) return -1;
        s->pal = pal;
        s->cap = cap;
    }
    memset(s->px + (size_t)s->rows * ROW_BYTES, 0,
           (size_t)(rows - s->rows) * ROW_BYTES);
    memset(s->scb + s->rows, SCB_640, rows - s->rows);
    memset(s->pal + s->rows, 0, (size_t)(rows - s->rows) * sizeof(uint16_t));
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

/* Gives a row its colours when it has none. */
static void row_claim(surface_t *s, int y, uint16_t paper, uint16_t ink)
{
    if (y < 0 || (uint32_t)y >= s->rows || (s->scb[y] & SCB_USED) != 0U ||
        s->pal[y] >= PAL_STATUS) return;
    s->pal[y] = pal_get(paper, ink, OW_NO_COLOR);
    s->scb[y] |= SCB_USED;
}

/* Returns the pixel values of a row for the coverage levels 1..3 of a
 * text colour, or NULL for a row that cannot take text. A row with one
 * text colour changes to a row with an accent when a second one comes. */
static const uint8_t *row_map(surface_t *s, int y, uint16_t colour,
                              uint16_t paper)
{
    static const uint8_t plain[4] = { 0U, 1U, 2U, 3U };
    static const uint8_t as_ink[4] = { 0U, 1U, 1U, 2U };
    static const uint8_t as_accent[4] = { 0U, 1U, 1U, 3U };
    const rowpal_t *p;

    if (y < 0 || (uint32_t)y >= s->rows || s->pal[y] >= PAL_PIC) return NULL;
    if (s->pal[y] == PAL_STATUS) return plain;
    row_claim(s, y, paper, colour == INK_OF_ROW ? s_pal[0].ink : colour);
    p = &s_pal[s->pal[y]];
    if (p->accent == OW_NO_COLOR) {
        uint8_t *row;
        if (colour == INK_OF_ROW || colour == p->ink) return plain;
        row = s->px + (size_t)y * ROW_BYTES;
        for (unsigned i = 0; i < ROW_BYTES; i++) {      /* keep the pixels */
            const uint8_t v = row[i];
            if (v != 0U) {
                row[i] = (uint8_t)((as_ink[v >> 6] << 6) | (as_ink[(v >> 4) & 3U] << 4) |
                                   (as_ink[(v >> 2) & 3U] << 2) | as_ink[v & 3U]);
            }
        }
        s->pal[y] = pal_get(p->paper, p->ink, colour);
        return as_accent;
    }
    if (colour == INK_OF_ROW || colour == p->ink) return as_ink;
    if (colour == p->accent) return as_accent;
    return color_distance(colour, p->accent) < color_distance(colour, p->ink) ?
           as_accent : as_ink;
}

static void hline(surface_t *s, int x0, int x1, int y, unsigned level,
                  uint16_t colour, uint16_t paper)
{
    const uint8_t *map = row_map(s, y, colour, paper);
    if (map == NULL) return;
    for (int x = x0; x < x1; x++) put640(s, x, y, map[level]);
}

static void vline(surface_t *s, int x, int y0, int y1, unsigned level,
                  uint16_t paper)
{
    for (int y = y0; y < y1; y++) {
        const uint8_t *map = row_map(s, y, INK_OF_ROW, paper);
        if (map != NULL) put640(s, x, y, map[level]);
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
                      int x, int top, uint16_t colour, uint16_t paper)
{
    const onewww_glyph_t *g = glyph_find(f, code);
    const uint8_t *bits = f->bits + g->offset;
    const unsigned stride = ((unsigned)g->w + 3U) >> 2;

    for (unsigned gy = 0; gy < g->h; gy++) {
        const int y = top + g->y + (int)gy;
        const uint8_t *map = row_map(s, y, colour, paper);
        if (map == NULL) continue;
        for (unsigned gx = 0; gx < g->w; gx++) {
            const unsigned level =
                (bits[gy * stride + (gx >> 2)] >> (6U - 2U * (gx & 3U))) & 3U;
            if (level != 0U) put640(s, x + g->x + (int)gx, y, map[level]);
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

    if (href[0] == '#') {
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
    uint8_t *rgb;               /* w x h x 3, on white paper */
    uint16_t w, h;              /* size of rgb */
    uint16_t natural_w, natural_h;
    uint8_t state;
} image_t;

static image_t s_img[IMG_MAX];
static unsigned s_imgs;
static size_t s_img_bytes;

static void images_free(void)
{
    for (unsigned i = 0; i < s_imgs; i++) { free(s_img[i].url); free(s_img[i].rgb); }
    memset(s_img, 0, sizeof(s_img));
    s_imgs = 0U; s_img_bytes = 0U;
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
                    if (comps == 4U) {          /* on white paper */
                        const unsigned a = p[3];
                        sum[0] += (p[0] * a + 255U * (255U - a)) / 255U;
                        sum[1] += (p[1] * a + 255U * (255U - a)) / 255U;
                        sum[2] += (p[2] * a + 255U * (255U - a)) / 255U;
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

static int luminance(const uint8_t *p)
{
    return (77 * p[0] + 150 * p[1] + 29 * p[2] + 128) >> 8;
}

static int luminance12(uint16_t c)
{
    return (77 * (int)((c >> 8) & 15) + 150 * (int)((c >> 4) & 15) + 29 * (int)(c & 15)) * 17 >> 8;
}

/* A picture in text rows: the grey levels between the paper and the ink
 * of each row, with error diffusion. */
static void draw_gray(surface_t *s, const image_t *im, int x, int y, int w, int h,
                      uint16_t paper)
{
    uint8_t *rgb;
    int16_t *err;

    if (w <= 0 || h <= 0 || surface_need(s, (uint32_t)(y + h)) != 0) return;
    rgb = scale_rgb(im->rgb, 3U, im->w, im->h, (unsigned)w, (unsigned)h);
    err = calloc((size_t)(w + 2) * 2U, sizeof(int16_t));
    if (rgb == NULL || err == NULL) { free(rgb); free(err); return; }
    for (int ry = 0; ry < h; ry++) {
        int16_t *cur = err + (ry & 1) * (w + 2), *next = err + ((ry + 1) & 1) * (w + 2);
        const uint8_t *map = row_map(s, y + ry, INK_OF_ROW, paper);
        const rowpal_t *p;
        int lp, li, steps;

        memset(next, 0, (size_t)(w + 2) * sizeof(int16_t));
        if (map == NULL) continue;
        p = &s_pal[s->pal[y + ry]];
        lp = luminance12(p->paper); li = luminance12(p->ink);
        steps = p->accent == OW_NO_COLOR ? 3 : 2;
        if (li == lp) continue;
        for (int rx = 0; rx < w; rx++) {
            int level = luminance(rgb + ((size_t)ry * w + rx) * 3U) + cur[rx + 1] / 16;
            int t = (level - lp) * 255 / (li - lp), q, shown, e;
            if (t < 0) t = 0;
            if (t > 255) t = 255;
            q = (t * steps + 127) / 255;
            shown = lp + (li - lp) * q / steps;
            e = level - shown;
            cur[rx + 2] = (int16_t)(cur[rx + 2] + e * 7);
            next[rx] = (int16_t)(next[rx] + e * 3);
            next[rx + 1] = (int16_t)(next[rx + 1] + e * 5);
            next[rx + 2] = (int16_t)(next[rx + 2] + e);
            put640(s, x + rx, y + ry, steps == 3 ? (uint8_t)q : (uint8_t)(q == 2 ? 2 : q));
        }
    }
    free(rgb); free(err);
}

/* Sixteen colours for a picture: the paper and fifteen from a median cut
 * of its colours. lut gives the palette entry for each 12-bit colour. */
static void quantize(const uint8_t *rgb, size_t pixels, uint16_t paper,
                     uint16_t pal[16], uint8_t *lut)
{
    static uint32_t hist[4096];
    struct { uint8_t lo[3], hi[3]; uint32_t count; } box[15];
    int boxes = 1;

    memset(hist, 0, sizeof(hist));
    for (size_t i = 0; i < pixels; i++, rgb += 3) {
        hist[((unsigned)(rgb[0] >> 4) << 8) | ((unsigned)(rgb[1] >> 4) << 4) | (rgb[2] >> 4)]++;
    }
    memset(box, 0, sizeof(box));
    box[0].hi[0] = box[0].hi[1] = box[0].hi[2] = 15U;
    box[0].count = (uint32_t)pixels;
    while (boxes < 15) {
        int pick = -1, axis = 0, range = 0;
        uint32_t half = 0, sum = 0;
        unsigned cut;
        for (int i = 0; i < boxes; i++) {       /* the box with the most pixels */
            int r = 0;
            for (int k = 0; k < 3; k++) {
                if (box[i].hi[k] - box[i].lo[k] > r) r = box[i].hi[k] - box[i].lo[k];
            }
            if (r > 0 && (pick < 0 || box[i].count > box[pick].count)) pick = i;
        }
        if (pick < 0) break;
        for (int k = 0; k < 3; k++) {
            if (box[pick].hi[k] - box[pick].lo[k] > range) {
                range = box[pick].hi[k] - box[pick].lo[k]; axis = k;
            }
        }
        half = box[pick].count / 2U;
        for (cut = box[pick].lo[axis]; cut < box[pick].hi[axis]; cut++) {
            unsigned c[3];
            for (c[0] = box[pick].lo[0]; c[0] <= box[pick].hi[0]; c[0]++) {
                for (c[1] = box[pick].lo[1]; c[1] <= box[pick].hi[1]; c[1]++) {
                    for (c[2] = box[pick].lo[2]; c[2] <= box[pick].hi[2]; c[2]++) {
                        if (c[axis] == cut) sum += hist[(c[0] << 8) | (c[1] << 4) | c[2]];
                    }
                }
            }
            if (sum >= half && sum != 0U) break;
        }
        if (cut >= box[pick].hi[axis]) cut = box[pick].hi[axis] - 1U;
        box[boxes] = box[pick];
        box[pick].hi[axis] = (uint8_t)cut;
        box[boxes].lo[axis] = (uint8_t)(cut + 1U);
        box[boxes].count = box[boxes].count > sum ? box[boxes].count - sum : 0U;
        box[pick].count = sum;
        boxes++;
    }
    pal[0] = paper;
    for (int i = 0; i < 15; i++) {
        uint32_t total = 0, sum[3] = { 0, 0, 0 };
        unsigned c[3];
        pal[i + 1] = paper;
        if (i >= boxes) continue;
        for (c[0] = box[i].lo[0]; c[0] <= box[i].hi[0]; c[0]++) {
            for (c[1] = box[i].lo[1]; c[1] <= box[i].hi[1]; c[1]++) {
                for (c[2] = box[i].lo[2]; c[2] <= box[i].hi[2]; c[2]++) {
                    const uint32_t n = hist[(c[0] << 8) | (c[1] << 4) | c[2]];
                    total += n; sum[0] += n * c[0]; sum[1] += n * c[1]; sum[2] += n * c[2];
                }
            }
        }
        if (total != 0U) {
            pal[i + 1] = (uint16_t)((((sum[0] + total / 2U) / total) << 8) |
                                    (((sum[1] + total / 2U) / total) << 4) |
                                    ((sum[2] + total / 2U) / total));
        }
    }
    for (unsigned c = 0; c < 4096U; c++) {
        int best = 0, best_d = 1 << 30;
        for (int i = 0; i < 16; i++) {
            const int dr = (int)((c >> 8) & 15) - (int)((pal[i] >> 8) & 15);
            const int dg = (int)((c >> 4) & 15) - (int)((pal[i] >> 4) & 15);
            const int db = (int)(c & 15) - (int)(pal[i] & 15);
            const int d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
            if (d < best_d) { best_d = d; best = i; }
        }
        lut[c] = (uint8_t)best;
    }
}

/* A picture in rows of its own: 320 mode, sixteen colours. x and w are
 * screen pixels; the picture has w / 2 pixels in a row. */
static void draw_band(surface_t *s, const image_t *im, int x, int y, int w, int h,
                      uint16_t paper)
{
    static uint8_t lut[4096];
    const int tw = w / 2, x320 = x / 2;
    uint8_t *rgb;
    int16_t *err;
    uint16_t *pal;

    if (tw <= 0 || h <= 0 || s_pics >= PIC_MAX ||
        surface_need(s, (uint32_t)(y + h)) != 0) return;
    rgb = scale_rgb(im->rgb, 3U, im->w, im->h, (unsigned)tw, (unsigned)h);
    err = calloc((size_t)(tw + 2) * 6U, sizeof(int16_t));
    if (rgb == NULL || err == NULL) { free(rgb); free(err); return; }
    pal = s_pic[s_pics];
    quantize(rgb, (size_t)tw * h, paper, pal, lut);
    for (int ry = 0; ry < h; ry++) {
        int16_t *cur = err + (ry & 1) * (tw + 2) * 3, *next = err + ((ry + 1) & 1) * (tw + 2) * 3;
        uint8_t *row = s->px + (size_t)(y + ry) * ROW_BYTES;

        memset(next, 0, (size_t)(tw + 2) * 3U * sizeof(int16_t));
        memset(row, 0, ROW_BYTES);
        s->scb[y + ry] = SCB_USED;                  /* 320 mode */
        s->pal[y + ry] = (uint16_t)(PAL_PIC + s_pics);
        for (int rx = 0; rx < tw; rx++) {
            const uint8_t *p = rgb + ((size_t)ry * tw + rx) * 3U;
            const int px = x320 + rx;
            int c[3];
            unsigned best;
            for (int k = 0; k < 3; k++) {
                c[k] = p[k] + cur[(rx + 1) * 3 + k] / 16;
                if (c[k] < 0) c[k] = 0;
                if (c[k] > 255) c[k] = 255;
            }
            best = lut[((unsigned)(c[0] >> 4) << 8) | ((unsigned)(c[1] >> 4) << 4) | (unsigned)(c[2] >> 4)];
            for (int k = 0; k < 3; k++) {
                const int q = c[k] - (int)((pal[best] >> (8 - 4 * k)) & 15) * 17;
                cur[(rx + 2) * 3 + k] = (int16_t)(cur[(rx + 2) * 3 + k] + q * 7);
                next[rx * 3 + k] = (int16_t)(next[rx * 3 + k] + q * 3);
                next[(rx + 1) * 3 + k] = (int16_t)(next[(rx + 1) * 3 + k] + q * 5);
                next[(rx + 2) * 3 + k] = (int16_t)(next[(rx + 2) * 3 + k] + q);
            }
            if (px < 0 || px >= 320) continue;
            if ((px & 1) == 0) row[px >> 1] = (uint8_t)((row[px >> 1] & 0x0FU) | (best << 4));
            else row[px >> 1] = (uint8_t)((row[px >> 1] & 0xF0U) | best);
        }
    }
    s_pics++;
    free(rgb); free(err);
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

static const text_t s_plain = { 0x0000, ONEWWW_FONT_BODY, 0, 0, 0, 0, NULL, -1 };

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
    /* A large picture that is alone on its line gets rows of its own:
     * 320 mode with sixteen colours. */
    if (f->s != NULL && f->top_level && l->items == 1U && l->item[0].image >= 0 &&
        l->item[0].w >= 160 && f->y >= f->fl_bottom && f->y >= f->fr_bottom &&
        s_pics < PIC_MAX) {
        const item_t *it = &l->item[0];
        int x = f->x0 + left;
        if (f->align == OW_ALIGN_CENTER) x += (avail - it->w) / 2;
        else if (f->align == OW_ALIGN_RIGHT) x += avail - it->w;
        draw_band(f->s, &s_img[it->image], x, f->y, it->w, (int)it->ih, f->paper);
        if (it->link >= 0) rect_add(x, x + it->w, f->y, (int)it->ih, (uint32_t)it->link);
        f->y += (int)it->ih; f->marker_len = 0U;
        line_reset(l);
        return;
    }
    if (f->s == NULL || surface_need(f->s, (uint32_t)(f->y + height)) != 0) {
        f->y += height; f->marker_len = 0U;
        line_reset(l);
        return;
    }
    if (f->align == OW_ALIGN_CENTER) shift = (avail - l->width) / 2;
    else if (f->align == OW_ALIGN_RIGHT) shift = avail - l->width;
    if (shift < 0) shift = 0;
    {
        /* The rows of the line take the paper of the block. Their ink is
         * the text colour that covers the most width. */
        uint16_t colour[4], ink = s_pal[0].ink;
        int width[4], colours = 0, best = -1;
        for (unsigned i = 0; i < l->items; i++) {
            int k = 0;
            if (l->item[i].image >= 0) continue;    /* a picture has no text colour */
            while (k < colours && colour[k] != l->item[i].colour) k++;
            if (k == colours) {
                if (colours == 4) continue;
                colour[colours] = l->item[i].colour; width[colours++] = 0;
            }
            width[k] += l->item[i].w;
        }
        for (int k = 0; k < colours; k++) {
            if (best < 0 || width[k] > width[best]) best = k;
        }
        if (best >= 0) ink = colour[best];
        for (int y = f->y; y < f->y + height; y++) {
            row_claim(f->s, y, f->paper, readable(ink, f->paper));
        }
    }
    if (f->marker_len != 0U) {
        const onewww_font_t *font = &onewww_fonts[ONEWWW_FONT_BODY];
        int x = f->x0 + left - 6 - text_width(font, f->marker, f->marker_len);
        for (unsigned k = 0; k < f->marker_len && left >= 14; k++) {
            x += glyph_draw(f->s, font, f->marker[k], x, top + ascent - font->ascent,
                            readable(f->marker_colour, f->paper), f->paper);
        }
        f->marker_len = 0U;
    }
    for (unsigned i = 0; i < l->items; i++) {
        const item_t *it = &l->item[i];
        const onewww_font_t *font = &onewww_fonts[it->font];
        const int x_start = f->x0 + left + shift + it->x;
        const uint16_t colour = readable(it->colour, f->paper);
        int x = x_start;

        if (it->image >= 0) {
            draw_gray(f->s, &s_img[it->image], x_start, top + ascent - (int)it->ih,
                      it->w, (int)it->ih, f->paper);
        }
        for (unsigned k = 0; k < it->count; k++) {
            x += glyph_draw(f->s, font, l->cp[it->start + k], x,
                            top + ascent - font->ascent, colour, f->paper);
        }
        if (it->underline) {
            hline(f->s, x_start, x_start + it->w, top + ascent + 2, LV_INK,
                  colour, f->paper);
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
    it->underline = tx->underline;
    it->link = tx->link;
    it->image = -1; it->ih = 0U;
    /* An underline also covers the space between the words of one run. */
    if (it->underline && space_w != 0 && l->items > 1U &&
        l->item[l->items - 2U].underline && l->item[l->items - 2U].link == it->link) {
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

    if (s == NULL || s_pics >= PIC_MAX || surface_need(s, (uint32_t)(y0 + h)) != 0) return;
    memcpy(s_pic[s_pics], s_picture_palette, sizeof(s_pic[0]));
    memset(err, 0, sizeof(err));
    for (int y = 0; y < h; y++) {
        int16_t (*cur)[3] = err[y & 1], (*next)[3] = err[(y + 1) & 1];
        uint8_t *row = s->px + (size_t)(y0 + y) * ROW_BYTES;

        memset(next, 0, sizeof(err[0]));
        s->scb[y0 + y] = SCB_USED;      /* 320 mode */
        s->pal[y0 + y] = (uint16_t)(PAL_PIC + s_pics);
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
    s_pics++;
    f->y += h;
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

/* The text properties of an element, from its style. */
static void text_style(text_t *tx, const onewww_style_t *cs)
{
    if (cs->font_px >= 26U) tx->font = ONEWWW_FONT_H1;
    else if (cs->font_px >= 19U) tx->font = ONEWWW_FONT_H2;
    else if (cs->mono) tx->font = ONEWWW_FONT_MONO;
    else if (cs->font_px <= 12U) tx->font = ONEWWW_FONT_SMALL;
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

/* Gives the rows of a block that have no text the paper of the block. */
static void paper_rows(flow_t *f, int y0, int y1, uint16_t ink)
{
    if (f->s == NULL) return;
    (void)surface_need(f->s, (uint32_t)y1);
    for (int y = y0; y < y1; y++) row_claim(f->s, y, f->paper, readable(ink, f->paper));
}

/* A block: margins, padding, alignment and paper around its content. */
static void walk_block(flow_t *f, const onewww_node_t *n, text_t tx,
                       const text_t *before)
{
    const onewww_style_t *cs = n->style;
    const int indent = f->indent, right = f->right;
    const uint8_t align = f->align;
    const uint16_t paper = f->paper;
    int y0, edge0, edge1;

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
    f->align = cs->align;
    if (cs->background != OW_NO_COLOR) f->paper = cs->background;
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
    if (cs->background != OW_NO_COLOR) paper_rows(f, y0, f->y, tx.colour);
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

    if (image >= 0 || f->s == NULL || s_imgs == IMG_MAX || s_doc.base == NULL ||
        n->length == 0U) return image;
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

/* The size of a picture on the page. Returns 0 when it cannot show. */
static int image_box(const onewww_node_t *n, int image, int avail, int *w, int *h)
{
    const image_t *im = &s_img[image];
    const onewww_style_t *cs = n->style;
    const int attr_w = attr_number(n, "width", 0), attr_h = attr_number(n, "height", 0);
    int dw, dh;

    if (im->state != IMG_LOADED || avail < 8) return 0;
    if (cs->width > 0) dw = cs->width;
    else if (cs->width < 0) dw = avail * -cs->width / 100;
    else dw = attr_w > 0 ? attr_w : im->natural_w;
    if (cs->width == 0 && attr_w > 0 && attr_h > 0) dh = attr_h;
    else dh = (int)((int32_t)im->natural_h * dw / im->natural_w);
    if (cs->max_width > 0 && dw > cs->max_width) { dh = dh * cs->max_width / dw; dw = cs->max_width; }
    if (dw > avail) { dh = (int)((int32_t)dh * avail / dw); dw = avail; }
    if (dh > 1200) { dw = (int)((int32_t)dw * 1200 / dh); dh = 1200; }
    if (dw < 4 || dh < 4) return 0;
    *w = dw; *h = dh;
    return 1;
}

#define FLOAT_GAP 10
#define FLEX_GAP  12
#define FLEX_MAX  8

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
        if (s_doc.internal && f->top_level && f->s != NULL &&
            onewww_attr(n, "src", text, sizeof(text)) > 0 &&
            strcmp(text, "builtin") == 0) {
            flow_break(f, &tx, 8);
            flow_picture(f, 120);
            f->gap = 0;
            flow_break(f, &tx, 8);
            return;
        }
        {
            int w, h;
            const int image = image_for(f, n);
            if (image >= 0 && image_box(n, image, f->w - f->indent - f->right, &w, &h)) {
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
    case OW_INPUT: {
        char type[16] = "text";
        (void)onewww_attr(n, "type", type, sizeof(type));
        if (strcmp(type, "hidden") == 0) return;
        if (onewww_attr(n, "value", text, 64U) < 0) text[0] = '\0';
        if (strcmp(type, "submit") == 0 || strcmp(type, "button") == 0 ||
            strcmp(type, "reset") == 0) {
            tx.font = ONEWWW_FONT_BODY_B;
            flow_literal(f, &tx, "[");
            flow_literal(f, &tx, text[0] != '\0' ? text : "Send");
            flow_literal(f, &tx, "]");
        } else if (strcmp(type, "checkbox") == 0 || strcmp(type, "radio") == 0) {
            /* Not shown before forms work: pages hide many of them. */
        } else {
            flow_literal(f, &tx, "[");
            flow_literal(f, &tx, text[0] != '\0' ? text :
                         "\xC2\xA0\xC2\xA0\xC2\xA0\xC2\xA0\xC2\xA0\xC2\xA0\xC2\xA0\xC2\xA0");
            flow_literal(f, &tx, "]");
        }
        flow_word_end(f, &tx);
        return;
    }
    case OW_BUTTON:
        tx.font = ONEWWW_FONT_BODY_B;
        flow_literal(f, &tx, "[");
        walk_children(f, n, tx);
        flow_literal(f, &tx, "]");
        flow_word_end(f, &tx);
        return;
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
        uint16_t row_paper = row->style != NULL ? row->style->background : OW_NO_COLOR;
        uint16_t row_ink = 0x0000;

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
            cy = cell_run(f, c, f->s, cx + CELL_PAD, cw - 2 * CELL_PAD,
                          y + CELL_PAD / 2 + 1, NULL, NULL);
            if (cy + CELL_PAD / 2 + 1 > bottom) bottom = cy + CELL_PAD / 2 + 1;
            if (edges <= MAX_COLS) edge[edges++] = cx;
            if (row_paper == OW_NO_COLOR && c->style->background != OW_NO_COLOR) {
                row_paper = c->style->background; row_ink = c->style->color;
            }
        }
        (void)grid_row_end(&grid);
        if (surface_need(f->s, (uint32_t)bottom + 1U) != 0) break;
        if (row_paper != OW_NO_COLOR) {
            const uint16_t paper = f->paper;        /* rows without text */
            f->paper = row_paper;
            paper_rows(f, y, bottom, row_ink);
            f->paper = paper;
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

/* While a page is laid out again (its pictures came), the screen shows
 * the layout before: the front copy. */
static struct {
    surface_t page;
    doc_t doc;
    rowpal_t pal[PAL_MAX];
    uint32_t pals;
    uint16_t pic[PIC_MAX][16];
    unsigned pics;
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

static void live_reset(void)
{
    s_pals = 1U; s_pics = 0U;
    s_pal[0].paper = s_work.paper; s_pal[0].ink = s_work.ink; s_pal[0].accent = OW_NO_COLOR;
}

static void front_drop(void)
{
    if (!s_front.valid) return;
    free(s_front.page.px); free(s_front.page.scb); free(s_front.page.pal);
    doc_lists_free(&s_front.doc);
    s_front.valid = 0U;
}

/* The layout that is on the screen becomes the front copy. */
static void front_take(void)
{
    front_drop();
    s_front.page = s_page;
    s_front.doc = s_doc;
    memcpy(s_front.pal, s_pal, sizeof(rowpal_t) * s_pals);
    s_front.pals = s_pals;
    memcpy(s_front.pic, s_pic, sizeof(s_pic[0]) * s_pics);
    s_front.pics = s_pics;
    s_front.valid = 1U;
    memset(&s_page, 0, sizeof(s_page));
    s_doc.href = NULL; s_doc.link_row = NULL; s_doc.rect = NULL; s_doc.anchor = NULL;
    s_doc.links = s_doc.links_cap = s_doc.rects = s_doc.rects_cap = 0U;
    s_doc.anchors = s_doc.anchors_cap = 0U;
    live_reset();
}

/* A new layout stopped: the front copy is the page again. */
static void front_restore(void)
{
    free(s_page.px); free(s_page.scb); free(s_page.pal);
    doc_lists_free(&s_doc);
    s_page = s_front.page;
    s_doc = s_front.doc;
    memcpy(s_pal, s_front.pal, sizeof(rowpal_t) * s_front.pals);
    s_pals = s_front.pals;
    memcpy(s_pic, s_front.pic, sizeof(s_pic[0]) * s_front.pics);
    s_pics = s_front.pics;
    s_front.valid = 0U;
}

void onewww_page_free(void)
{
    front_drop();
    free(s_page.px); free(s_page.scb); free(s_page.pal);
    memset(&s_page, 0, sizeof(s_page));
    doc_lists_free(&s_doc);
    memset(&s_doc, 0, sizeof(s_doc));
    sheets_free();
    images_free();
    if (s_work.dom != NULL) onewww_dom_free(s_work.dom);
    memset(&s_work, 0, sizeof(s_work));
    s_work.paper = 0x0FFF;
    live_reset();
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
        live_reset();
    }
    line = malloc(sizeof(*line));
    if (line == NULL || surface_need(&s_page, ROW_CHUNK) != 0) {
        free(line);
        if (again) front_restore(); else onewww_page_free();
        return -1;
    }
    s_page.rows = 0U;
    memset(&f, 0, sizeof(f));
    memset(line, 0, sizeof(*line));
    f.s = &s_page; f.line = line; f.x0 = MARGIN; f.w = PAGE_WIDTH - 2 * MARGIN;
    f.top_level = 1U; f.gap = NO_GAP; f.paper = s_pal[0].paper;
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

#define STATUS_PAPER 0x0DDD

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
            if (draw) (void)glyph_draw(s, font, 0x2026, x, 2, 0x0000, STATUS_PAPER);
            break;
        }
        if (draw) (void)glyph_draw(s, font, (uint16_t)code, x, 2, 0x0000, STATUS_PAPER);
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
    memset(s_status_scb, SCB_640 | SCB_USED, sizeof(s_status_scb));
    for (unsigned y = 0; y < ONEWWW_STATUS_ROWS; y++) s_status_pal[y] = PAL_STATUS;
    hline(&s, 0, PAGE_WIDTH, 0, LV_DARK, 0x0000, STATUS_PAPER);
    if (right != NULL && right[0] != '\0') {
        right_x -= status_text(&s, right, 0, PAGE_WIDTH, 0);
        (void)status_text(&s, right, right_x, PAGE_WIDTH, 1);
        right_x -= 12;
    }
    (void)status_text(&s, left, 6, right_x - 8, 1);
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
    static const rowpal_t status_pal = { STATUS_PAPER, 0x0000, OW_NO_COLOR };
    static uint8_t row_scb[ONEWWW_VIEW_ROWS], row_accent[ONEWWW_VIEW_ROWS];
    static uint16_t row_pal[ONEWWW_VIEW_ROWS];
    const surface_t *page = view_page();
    const doc_t *doc = view_doc();
    const rowpal_t *pals = s_front.valid ? s_front.pal : s_pal;
    const uint16_t (*pics)[16] = s_front.valid ? s_front.pic : s_pic;
    uint16_t slot_pal[2][16];
    uint8_t slots[2] = { 0U, 0U };

    memset(fields, 0, ONEWWW_FIELDS_BYTES);
    status_draw(left, right);
    for (uint32_t r = 0; r < ONEWWW_VIEW_ROWS; r++) {
        const uint32_t field = r & 1U, line = r >> 1;
        uint8_t *base = fields + field * ONEWWW_FIELD_BYTES;
        const uint8_t *px = NULL;
        const rowpal_t *p;
        uint8_t scb = SCB_640;
        uint16_t pal = 0U;
        unsigned slot;

        if (r >= ONEWWW_PAGE_VIEW) {
            const uint32_t sr = r - ONEWWW_PAGE_VIEW;
            px = s_status_px + sr * ROW_BYTES;
            scb = s_status_scb[sr]; pal = s_status_pal[sr];
        } else if (top + r < page->rows) {
            px = page->px + (size_t)(top + r) * ROW_BYTES;
            scb = page->scb[top + r]; pal = page->pal[top + r];
        }
        scb &= SCB_640;
        p = pal == PAL_STATUS ? &status_pal : pal < PAL_PIC ? &pals[pal] : NULL;
        row_scb[r] = scb; row_pal[r] = pal;
        row_accent[r] = p != NULL && p->accent != OW_NO_COLOR;
        if (px != NULL) memcpy(base + line * ROW_BYTES, px, ROW_BYTES);
        for (slot = 0; slot < slots[field] && slot_pal[field][slot] != pal; slot++) {}
        if (slot == slots[field] && slots[field] == 16U) {
            /* All 16 palettes of this field are in use: take the nearest. */
            int best_d = 1 << 30;
            slot = 0U;
            for (unsigned i = 0; i < 16U && p != NULL; i++) {
                const uint16_t other = slot_pal[field][i];
                const rowpal_t *o = other == PAL_STATUS ? &status_pal :
                                    other < PAL_PIC ? &pals[other] : NULL;
                int d;
                if (o == NULL) continue;
                d = color_distance(o->paper, p->paper) * 4 + color_distance(o->ink, p->ink) +
                    ((o->accent != OW_NO_COLOR) != (p->accent != OW_NO_COLOR) ? 40 : 0);
                if (d < best_d) { best_d = d; slot = i; row_accent[r] = o->accent != OW_NO_COLOR; }
            }
        } else if (slot == slots[field]) {
            uint8_t *entry = base + 0x7E00U + (uint32_t)slot * 32U;
            uint16_t four[4] = { 0, 0, 0, 0 };
            slot_pal[field][slots[field]++] = pal;
            if (p != NULL) {
                four[0] = p->paper;
                if (p->accent == OW_NO_COLOR) {
                    four[1] = mix(p->paper, p->ink, 1, 3);
                    four[2] = mix(p->paper, p->ink, 2, 3);
                    four[3] = p->ink;
                } else {
                    four[1] = mix(p->paper, p->ink, 1, 2);
                    four[2] = p->ink;
                    four[3] = p->accent;
                }
            }
            for (unsigned i = 0; i < 16U; i++) {
                const uint16_t c = p == NULL ? pics[pal - PAL_PIC][i] : four[i & 3U];
                entry[2U * i] = (uint8_t)c;
                entry[2U * i + 1U] = (uint8_t)(c >> 8);
            }
        }
        base[0x7D00U + line] = (uint8_t)(scb | slot);
    }
    /* The selected link: paper and the last colour change places. */
    for (uint32_t i = 0; selected >= 0 && i < doc->rects; i++) {
        const rect_t *rc = &doc->rect[i];
        if (rc->link != (uint32_t)selected || rc->y + rc->h <= top ||
            rc->y >= top + ONEWWW_PAGE_VIEW) continue;
        for (uint32_t y = rc->y < top ? top : rc->y;
             y < rc->y + rc->h && y < top + ONEWWW_PAGE_VIEW; y++) {
            const uint32_t r = y - top;
            uint8_t *row = fields + (r & 1U) * ONEWWW_FIELD_BYTES + (r >> 1) * ROW_BYTES;
            if (row_scb[r] != SCB_640) {
                /* A picture link: a frame in the paper colour's opposite. */
                if (y == rc->y || y + 1U == rc->y + rc->h) {
                    for (int x = rc->x0 / 2; x < rc->x1 / 2 && x < 320; x++) {
                        if (x >= 0) row[x >> 1] ^= (x & 1) ? 0x0FU : 0xF0U;
                    }
                }
                continue;
            }
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
        unsigned dark = 0U, light = 0U;
        if (r < 0 || r >= (int)ONEWWW_VIEW_ROWS) continue;
        row = fields + ((unsigned)r & 1U) * ONEWWW_FIELD_BYTES +
              ((unsigned)r >> 1) * ROW_BYTES;
        if (row_scb[r] != SCB_640 && row_pal[r] >= PAL_PIC) {
            const uint16_t *pic = pics[row_pal[r] - PAL_PIC];
            for (unsigned i = 1; i < 16U; i++) {    /* darkest and lightest */
                if (luminance12(pic[i]) < luminance12(pic[dark])) dark = i;
                if (luminance12(pic[i]) > luminance12(pic[light])) light = i;
            }
        }
        for (int px = 0; s_pointer[py][px] != '\0'; px++) {
            const int x = pointer_x + px;
            const char c = s_pointer[py][px];
            if (c == ' ' || x < 0 || x >= PAGE_WIDTH) continue;
            if (row_scb[r] == SCB_640) {
                const unsigned shift = 6U - 2U * ((unsigned)x & 3U);
                const unsigned ink = row_accent[r] ? 2U : 3U;
                row[x >> 2] = (uint8_t)((row[x >> 2] & ~(3U << shift)) |
                                        ((c == 'X' ? ink : 0U) << shift));
            } else {
                const unsigned shift = (x & 2) == 0 ? 4U : 0U;
                row[x >> 2] = (uint8_t)((row[x >> 2] & ~(15U << shift)) |
                                        ((c == 'X' ? dark : light) << shift));
            }
        }
    }
    fields[0x7DF8U] = 1U;                                   /* interlace */
    memcpy(fields + 0x7DFCU, magic, sizeof(magic));
    memcpy(fields + ONEWWW_FIELD_BYTES + 0x7DFCU, magic, sizeof(magic));
}
