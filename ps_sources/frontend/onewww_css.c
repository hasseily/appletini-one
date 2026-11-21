/* ONEWWW style engine. See onewww_css.h. No hardware access. */
#include "onewww_css.h"

#include <stdlib.h>
#include <string.h>

#define CHUNK_BYTES  65536U
#define HASH_SIZE    1024U
#define MAX_PARTS    8
#define MAX_CLASSES  3
#define MAX_MATCH    256
#define MAX_DECLS    96
#define VALUE_MAX    256U
#define INLINE_MAX   2048U

enum {
    P_DISPLAY, P_COLOR, P_BACKGROUND, P_BOLD, P_ITALIC, P_FONT_SIZE, P_MONO,
    P_ALIGN, P_UNDERLINE, P_WHITE_SPACE, P_LIST_NONE,
    P_MARGIN_TOP, P_MARGIN_RIGHT, P_MARGIN_BOTTOM, P_MARGIN_LEFT,
    P_PADDING_TOP, P_PADDING_RIGHT, P_PADDING_BOTTOM, P_PADDING_LEFT,
    P_WIDTH, P_MAX_WIDTH, P_FLOAT, P_CLEAR, P_FLEX_COLUMN,
    P_BORDER_TOP, P_BORDER_RIGHT, P_BORDER_BOTTOM, P_BORDER_LEFT
};
/* Lengths: 1/64 pixel, 1/64 em, or a percentage. U_AUTO: an auto margin. */
enum { U_NONE, U_PX, U_EM, U_PERCENT, U_AUTO };
enum { WS_NORMAL, WS_PRE, WS_NOWRAP };

typedef struct {
    uint8_t prop, important, unit;
    int32_t value;
} decl_t;

enum { A_NONE, A_EXISTS, A_EQUAL, A_WORD, A_PREFIX, A_SUFFIX, A_CONTAINS, A_DASH };
enum { F_FIRST = 1, F_LAST = 2, F_EMPTY = 4, F_ROOT = 8, F_LINK = 16 };

typedef struct compound {
    const char *id;
    const char *cls[MAX_CLASSES];
    const char *attr_name, *attr_value;
    struct compound *not;
    uint16_t tag;                   /* 0: any tag */
    uint8_t classes, attr_op, flags;
    char combinator;                /* to the compound on the left */
} compound_t;

typedef struct rule {
    struct rule *next;
    const compound_t *parts;        /* parts[count - 1] is the subject */
    const decl_t *decls;
    uint32_t specificity, order;
    uint16_t decl_count;
    uint8_t count, author;
} rule_t;

typedef struct chunk {
    struct chunk *next;
    size_t used, size;
} chunk_t;

struct onewww_css {
    chunk_t *chunks;
    rule_t *by_hash[HASH_SIZE];
    rule_t *by_tag[OW_TAG_COUNT];
    rule_t *universal;
    uint32_t order;
    uint8_t failed, author;
};

/* The built-in rules: what a browser does without a style sheet. */
static const char s_default_sheet[] =
    "html,body,div,p,h1,h2,h3,h4,h5,h6,ul,ol,dl,dt,dd,blockquote,pre,form,"
    "fieldset,legend,address,center,section,article,aside,nav,header,footer,"
    "main,figure,figcaption,details,summary,hgroup,noscript,table,tr,td,th,"
    "caption,hr{display:block}"
    "li{display:list-item}"
    "head,script,style,title,meta,link,base,template,svg,iframe,select,option,"
    "textarea,object,video,audio,canvas,map,col,colgroup,area,embed,source,"
    "track,[hidden]{display:none}"
    "p,dl,blockquote,figure,pre{margin-top:10px;margin-bottom:10px}"
    "h1{font-size:2em;margin-top:12px;margin-bottom:10px}"
    "h2{font-size:1.5em;margin-top:12px;margin-bottom:8px}"
    "h3{font-size:1.17em;margin-top:10px;margin-bottom:6px}"
    "h4,h5,h6{margin-top:10px;margin-bottom:6px}"
    "h1,h2,h3,h4,h5,h6,b,strong,th,legend,summary{font-weight:bold}"
    "i,em,cite,var,dfn,address{font-style:italic}"
    "code,tt,kbd,samp,pre{font-family:monospace}"
    "pre{white-space:pre}"
    "small,sub,sup{font-size:.8em}"
    "big{font-size:1.2em}"
    "ul,ol{margin-top:6px;margin-bottom:6px;padding-left:24px}"
    "ul ul,ul ol,ol ul,ol ol{margin-top:0;margin-bottom:0}"
    "dd{margin-left:24px}"
    "blockquote,figure{margin-left:24px;margin-right:24px}"
    "table{margin-top:6px;margin-bottom:6px;text-align:left}"
    "caption{font-weight:bold;text-align:center}"
    "a:link{color:#02c;text-decoration:underline}"
    "u,ins{text-decoration:underline}"
    "center{text-align:center}";

/* ---- Memory ------------------------------------------------------------ */

static void *arena(onewww_css_t *css, size_t bytes)
{
    chunk_t *c = css->chunks;
    void *p;

    bytes = (bytes + 7U) & ~(size_t)7U;
    if (c == NULL || c->used + bytes > c->size) {
        const size_t size = bytes > CHUNK_BYTES ? bytes : CHUNK_BYTES;
        c = malloc(sizeof(*c) + size);
        if (c == NULL) { css->failed = 1U; return NULL; }
        c->next = css->chunks; c->used = 0U; c->size = size;
        css->chunks = c;
    }
    p = (uint8_t *)(c + 1) + c->used;
    c->used += bytes;
    return p;
}

static const char *keep(onewww_css_t *css, const char *text, size_t n)
{
    char *copy = arena(css, n + 1U);
    if (copy == NULL) return "";
    memcpy(copy, text, n);
    copy[n] = '\0';
    return copy;
}

/* ---- Text helpers ------------------------------------------------------ */

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int is_space(int c)
{ return c == ' ' || c == '\n' || c == '\t' || c == '\r' || c == '\f'; }
static int is_ident(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' || c >= 0x80;
}

static int word_is(const char *p, size_t n, const char *word)
{
    return strlen(word) == n && memcmp(p, word, n) == 0;
}

static int has(const char *p, size_t n, const char *needle)
{
    const size_t m = strlen(needle);
    for (size_t i = 0; i + m <= n; i++) {
        if (memcmp(p + i, needle, m) == 0) return 1;
    }
    return 0;
}

static uint32_t hash_text(char kind, const char *text)
{
    uint32_t h = 2166136261U ^ (uint8_t)kind;
    while (*text != '\0') h = (h ^ (uint8_t)*text++) * 16777619U;
    return h % HASH_SIZE;
}

/* ---- Values ------------------------------------------------------------ */

/* Reads a number. Returns its value times 64 and moves *pp. */
static int number(const char **pp, const char *end, int32_t *value)
{
    const char *p = *pp;
    int64_t whole = 0, frac = 0, scale = 1;
    int negative = 0, digits = 0;

    if (p < end && (*p == '-' || *p == '+')) negative = *p++ == '-';
    for (; p < end && *p >= '0' && *p <= '9'; p++, digits++) {
        if (whole < 100000) whole = whole * 10 + (*p - '0');
    }
    if (p < end && *p == '.') {
        for (p++; p < end && *p >= '0' && *p <= '9'; p++, digits++) {
            if (scale < 10000) { frac = frac * 10 + (*p - '0'); scale *= 10; }
        }
    }
    if (digits == 0) return 0;
    whole = whole * 64 + frac * 64 / scale;
    *value = (int32_t)(negative ? -whole : whole);
    *pp = p;
    return 1;
}

static int length(const char *p, size_t n, uint8_t *unit, int32_t *value)
{
    const char *end = p + n, *q = p;
    int32_t v;

    if (!number(&q, end, &v)) return 0;
    n = (size_t)(end - q);
    *unit = U_PX;
    if (n == 0U) { if (v != 0) return 0; }
    else if (word_is(q, n, "px")) {}
    else if (word_is(q, n, "em")) *unit = U_EM;
    else if (word_is(q, n, "rem")) v *= 16;
    else if (word_is(q, n, "pt")) v = v * 4 / 3;
    else if (word_is(q, n, "ch") || word_is(q, n, "ex")) v *= 8;
    else if (word_is(q, n, "vw")) v = v * 64 / 10;
    else if (word_is(q, n, "vh")) v = v * 4;
    else return 0;
    *value = v;
    return 1;
}

static int channel(const char **pp, const char *end, int *out)
{
    const char *p = *pp;
    int32_t v;

    while (p < end && (is_space(*p) || *p == ',' || *p == '/')) p++;
    if (!number(&p, end, &v)) return 0;
    if (p < end && *p == '%') { p++; v = v * 255 / 100; }
    *out = v / 64;
    *pp = p;
    return 1;
}

static int hex(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* Reads a colour. *out is 0RGB, or OW_NO_COLOR for a transparent one. */
static int color(const char *p, size_t n, uint16_t *out)
{
    int lo = 0, hi = (int)onewww_css_color_count - 1;
    char name[24];

    if (n == 0U) return 0;
    if (p[0] == '#') {
        int d[8];
        if (n != 4U && n != 5U && n != 7U && n != 9U) return 0;
        for (size_t i = 1; i < n; i++) {
            if ((d[i - 1U] = hex(p[i])) < 0) return 0;
        }
        if (n <= 5U) *out = (uint16_t)((d[0] << 8) | (d[1] << 4) | d[2]);
        else *out = (uint16_t)((d[0] << 8) | (d[2] << 4) | d[4]);
        if ((n == 5U && d[3] == 0) || (n == 9U && d[6] == 0 && d[7] == 0)) *out = OW_NO_COLOR;
        return 1;
    }
    if (n > 4U && (memcmp(p, "rgb(", 4U) == 0 || memcmp(p, "rgba(", 5U) == 0)) {
        const char *q = strchr(p, '(') + 1, *end = p + n;
        int r, g, b;
        int32_t a = 64;
        if (!channel(&q, end, &r) || !channel(&q, end, &g) || !channel(&q, end, &b)) return 0;
        while (q < end && (is_space(*q) || *q == ',' || *q == '/')) q++;
        (void)number(&q, end, &a);
        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (b > 255) b = 255;
        if (r < 0 || g < 0 || b < 0) return 0;
        *out = a == 0 ? OW_NO_COLOR : (uint16_t)(((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4));
        return 1;
    }
    if (word_is(p, n, "transparent")) { *out = OW_NO_COLOR; return 1; }
    if (n >= sizeof(name)) return 0;
    memcpy(name, p, n); name[n] = '\0';
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        const int cmp = strcmp(name, onewww_css_colors[mid].name);
        if (cmp == 0) { *out = onewww_css_colors[mid].color; return 1; }
        if (cmp < 0) hi = mid - 1; else lo = mid + 1;
    }
    return 0;
}

typedef struct { const char *p; size_t n; } token_t;

/* Splits a value at spaces and slashes that are not in parentheses. */
static int split(const char *v, size_t n, token_t *out, int max)
{
    int count = 0, depth = 0;
    size_t start = 0;

    for (size_t i = 0; i <= n; i++) {
        const char c = i < n ? v[i] : ' ';
        if (c == '(') depth++;
        else if (c == ')' && depth > 0) depth--;
        if (depth == 0 && (is_space(c) || c == '/' || (c == ',' && max > 8))) {
            if (i > start && count < max) { out[count].p = v + start; out[count++].n = i - start; }
            start = i + 1U;
        }
    }
    return count;
}

/* Copies a value in lower case, takes "!important" off and replaces
 * var(name, fallback) by the fallback. Returns the length, or 0. */
static size_t prepare(const char *v, size_t n, char *out, int *important)
{
    size_t used = 0;

    while (n != 0U && is_space(*v)) { v++; n--; }
    while (n != 0U && is_space(v[n - 1U])) n--;
    if (n >= VALUE_MAX) return 0U;
    for (size_t i = 0; i < n; i++) out[used++] = (char)lower((unsigned char)v[i]);
    out[used] = '\0';
    *important = 0;
    {
        char *bang = strchr(out, '!');
        if (bang != NULL) {
            *important = strstr(bang, "important") != NULL;
            *bang = '\0';
            used = strlen(out);
            while (used != 0U && is_space(out[used - 1U])) out[--used] = '\0';
        }
    }
    for (int round = 0; round < 4; round++) {
        char *var = strstr(out, "var("), *q, *comma = NULL;
        int depth = 0;
        if (var == NULL) break;
        for (q = var + 3; *q != '\0'; q++) {
            if (*q == '(') depth++;
            else if (*q == ')') { if (--depth == 0) break; }
            else if (*q == ',' && depth == 1 && comma == NULL) comma = q;
        }
        if (*q == '\0' || comma == NULL) return 0U;
        {
            char *from = comma + 1;
            size_t keep_n;
            while (from < q && is_space(*from)) from++;
            keep_n = (size_t)(q - from);
            memmove(var, from, keep_n);
            memmove(var + keep_n, q + 1, strlen(q + 1) + 1U);
        }
    }
    used = strlen(out);
    if (strstr(out, "calc(") != NULL || strstr(out, "env(") != NULL) return 0U;
    return used;
}

typedef struct {
    decl_t *list;
    int count, max;
    int important;
    /* Signs of a block that is on the page but not for the eye. */
    uint8_t away, clip, tiny, overflow;
} decls_t;

static void put(decls_t *d, uint8_t prop, uint8_t unit, int32_t value)
{
    if (d->count >= d->max) return;
    d->list[d->count].prop = prop;
    d->list[d->count].important = (uint8_t)d->important;
    d->list[d->count].unit = unit;
    d->list[d->count].value = value;
    d->count++;
}

static void put_length(decls_t *d, uint8_t prop, const token_t *t)
{
    uint8_t unit;
    int32_t value;

    if (word_is(t->p, t->n, "auto")) put(d, prop, U_AUTO, 0);
    else if (length(t->p, t->n, &unit, &value)) put(d, prop, unit, value);
}

/* width and max-width: a length or a percentage. */
static void put_size(decls_t *d, uint8_t prop, const token_t *t)
{
    uint8_t unit;
    int32_t value;
    const char *q = t->p;

    if (t->n > 1U && t->p[t->n - 1U] == '%') {
        if (number(&q, t->p + t->n - 1U, &value) && value > 0) {
            put(d, prop, U_PERCENT, value / 64);
        }
    } else if (length(t->p, t->n, &unit, &value) && value > 0) {
        put(d, prop, unit, value);
    } else if (word_is(t->p, t->n, "auto") || word_is(t->p, t->n, "none")) {
        put(d, prop, U_PX, 0);
    }
}

/* A border side shows when its value names a line style and no zero width. */
static int border_shows(const char *v, size_t n)
{
    if (has(v, n, "none") || has(v, n, "hidden")) return 0;
    if (n != 0U && v[0] == '0' && (n == 1U || is_space(v[1]) || v[1] == 'p')) return 0;
    return has(v, n, "solid") || has(v, n, "dashed") || has(v, n, "dotted") ||
           has(v, n, "double") || has(v, n, "groove") || has(v, n, "ridge") ||
           has(v, n, "inset") || has(v, n, "outset");
}

/* margin and padding: 1 to 4 values, in the order top right bottom left. */
static void put_box(decls_t *d, uint8_t first, const char *v, size_t n)
{
    static const uint8_t map[4][4] = {
        { 0, 0, 0, 0 }, { 0, 1, 0, 1 }, { 0, 1, 2, 1 }, { 0, 1, 2, 3 } };
    token_t t[4];
    const int count = split(v, n, t, 4);

    if (count < 1) return;
    for (int side = 0; side < 4; side++) {
        put_length(d, (uint8_t)(first + side), &t[map[count - 1][side]]);
    }
}

static void put_font_size(decls_t *d, const token_t *t)
{
    static const struct { const char *name; uint8_t unit; int32_t value; } words[] = {
        { "xx-small", U_PX, 9 * 64 }, { "x-small", U_PX, 10 * 64 },
        { "small", U_PX, 13 * 64 }, { "medium", U_PX, 16 * 64 },
        { "large", U_PX, 18 * 64 }, { "x-large", U_PX, 24 * 64 },
        { "xx-large", U_PX, 32 * 64 }, { "smaller", U_EM, 53 },
        { "larger", U_EM, 77 },
    };
    uint8_t unit;
    int32_t value;

    for (unsigned i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        if (word_is(t->p, t->n, words[i].name)) {
            put(d, P_FONT_SIZE, words[i].unit, words[i].value);
            return;
        }
    }
    if (t->n > 1U && t->p[t->n - 1U] == '%') {
        const char *q = t->p;
        if (number(&q, t->p + t->n - 1U, &value)) put(d, P_FONT_SIZE, U_EM, value / 100);
    } else if (length(t->p, t->n, &unit, &value) && value > 0) {
        put(d, P_FONT_SIZE, unit, value);
    }
}

static int is_mono(const char *v, size_t n)
{
    return has(v, n, "monospace") || has(v, n, "courier") || has(v, n, "consolas") ||
           has(v, n, "menlo") || has(v, n, "monaco");
}

static int weight_bold(const token_t *t, int *bold)
{
    if (word_is(t->p, t->n, "bold") || word_is(t->p, t->n, "bolder")) { *bold = 1; return 1; }
    if (word_is(t->p, t->n, "normal") || word_is(t->p, t->n, "lighter")) { *bold = 0; return 1; }
    if (t->n == 3U && t->p[0] >= '1' && t->p[0] <= '9' && t->p[1] == '0' && t->p[2] == '0') {
        *bold = t->p[0] >= '6';
        return 1;
    }
    return 0;
}

static void declaration(decls_t *d, const char *name, size_t name_n,
                        const char *v, size_t n)
{
    token_t t[12];
    const int count = split(v, n, t, 12);
    uint16_t c;
    int flag;

    if (count == 0) return;
    if (word_is(name, name_n, "display")) {
        int32_t value = OW_D_BLOCK;
        if (word_is(v, n, "none")) value = OW_D_NONE;
        else if (word_is(v, n, "inline") || word_is(v, n, "inline-block") ||
                 word_is(v, n, "inline-flex") || word_is(v, n, "inline-grid") ||
                 word_is(v, n, "contents") || word_is(v, n, "inline-table")) value = OW_D_INLINE;
        else if (word_is(v, n, "list-item")) value = OW_D_LIST_ITEM;
        else if (word_is(v, n, "flex") || word_is(v, n, "inline-flex")) value = OW_D_FLEX;
        put(d, P_DISPLAY, U_NONE, value);
    } else if (word_is(name, name_n, "visibility")) {
        if (word_is(v, n, "hidden") || word_is(v, n, "collapse")) put(d, P_DISPLAY, U_NONE, OW_D_NONE);
    } else if (word_is(name, name_n, "color")) {
        if (color(t[0].p, t[0].n, &c) && c != OW_NO_COLOR) put(d, P_COLOR, U_NONE, c);
    } else if (word_is(name, name_n, "background-color") || word_is(name, name_n, "background")) {
        /* The first colour of the value, also when it is in a gradient. */
        const char *p = v, *end = v + n;
        if (word_is(v, n, "none")) { put(d, P_BACKGROUND, U_NONE, OW_NO_COLOR); return; }
        while (p < end) {
            const char *q = p;
            while (q < end && (is_ident((unsigned char)*q) || *q == '#')) q++;
            if (q < end && *q == '(') {
                if (has(p, (size_t)(q - p), "gradient")) { p = q + 1; continue; }
                while (q < end && *q != ')') q++;       /* rgb(...), url(...) */
                if (q < end) q++;
                if (memcmp(p, "rgb", 3U) != 0) { p = q; continue; }
            }
            if (q > p && color(p, (size_t)(q - p), &c)) { put(d, P_BACKGROUND, U_NONE, c); return; }
            p = q > p ? q : p + 1;
        }
    } else if (word_is(name, name_n, "font-weight")) {
        if (weight_bold(&t[0], &flag)) put(d, P_BOLD, U_NONE, flag);
    } else if (word_is(name, name_n, "font-style")) {
        put(d, P_ITALIC, U_NONE, word_is(v, n, "italic") || has(v, n, "oblique"));
    } else if (word_is(name, name_n, "font-size")) {
        put_font_size(d, &t[0]);
    } else if (word_is(name, name_n, "font-family")) {
        put(d, P_MONO, U_NONE, is_mono(v, n));
    } else if (word_is(name, name_n, "font")) {
        int size_at = -1;
        put(d, P_ITALIC, U_NONE, 0);
        put(d, P_BOLD, U_NONE, 0);
        for (int i = 0; i < count && size_at < 0; i++) {
            if (word_is(t[i].p, t[i].n, "italic") || word_is(t[i].p, t[i].n, "oblique")) {
                put(d, P_ITALIC, U_NONE, 1);
            } else if (weight_bold(&t[i], &flag)) {
                put(d, P_BOLD, U_NONE, flag);
            } else if ((t[i].p[0] >= '0' && t[i].p[0] <= '9') || t[i].p[0] == '.' ||
                       has(t[i].p, t[i].n, "small") || has(t[i].p, t[i].n, "large") ||
                       word_is(t[i].p, t[i].n, "medium")) {
                size_at = i;
            }
        }
        if (size_at >= 0) {
            put_font_size(d, &t[size_at]);
            put(d, P_MONO, U_NONE, is_mono(v, n));
        }
    } else if (word_is(name, name_n, "text-align")) {
        put(d, P_ALIGN, U_NONE, word_is(v, n, "center") ? OW_ALIGN_CENTER :
            (word_is(v, n, "right") || word_is(v, n, "end")) ? OW_ALIGN_RIGHT : OW_ALIGN_LEFT);
    } else if (word_is(name, name_n, "text-decoration") ||
               word_is(name, name_n, "text-decoration-line")) {
        if (has(v, n, "underline")) put(d, P_UNDERLINE, U_NONE, 1);
        else if (has(v, n, "none")) put(d, P_UNDERLINE, U_NONE, 0);
    } else if (word_is(name, name_n, "white-space")) {
        put(d, P_WHITE_SPACE, U_NONE, (word_is(v, n, "pre") || word_is(v, n, "pre-wrap") ||
            word_is(v, n, "break-spaces")) ? WS_PRE : word_is(v, n, "nowrap") ? WS_NOWRAP : WS_NORMAL);
    } else if (word_is(name, name_n, "list-style") || word_is(name, name_n, "list-style-type")) {
        put(d, P_LIST_NONE, U_NONE, has(v, n, "none"));
    } else if (word_is(name, name_n, "margin")) {
        put_box(d, P_MARGIN_TOP, v, n);
    } else if (word_is(name, name_n, "padding")) {
        put_box(d, P_PADDING_TOP, v, n);
    } else if (word_is(name, name_n, "margin-top") || word_is(name, name_n, "margin-block-start")) {
        put_length(d, P_MARGIN_TOP, &t[0]);
    } else if (word_is(name, name_n, "margin-bottom") || word_is(name, name_n, "margin-block-end")) {
        put_length(d, P_MARGIN_BOTTOM, &t[0]);
    } else if (word_is(name, name_n, "margin-left") || word_is(name, name_n, "margin-inline-start")) {
        put_length(d, P_MARGIN_LEFT, &t[0]);
    } else if (word_is(name, name_n, "margin-right") || word_is(name, name_n, "margin-inline-end")) {
        put_length(d, P_MARGIN_RIGHT, &t[0]);
    } else if (word_is(name, name_n, "padding-top") || word_is(name, name_n, "padding-block-start")) {
        put_length(d, P_PADDING_TOP, &t[0]);
    } else if (word_is(name, name_n, "padding-bottom") || word_is(name, name_n, "padding-block-end")) {
        put_length(d, P_PADDING_BOTTOM, &t[0]);
    } else if (word_is(name, name_n, "padding-left") || word_is(name, name_n, "padding-inline-start")) {
        put_length(d, P_PADDING_LEFT, &t[0]);
    } else if (word_is(name, name_n, "padding-right") || word_is(name, name_n, "padding-inline-end")) {
        put_length(d, P_PADDING_RIGHT, &t[0]);
    } else if (word_is(name, name_n, "position")) {
        if (word_is(v, n, "absolute") || word_is(v, n, "fixed")) d->away = 1U;
    } else if (word_is(name, name_n, "clip") || word_is(name, name_n, "clip-path")) {
        if (!word_is(v, n, "auto") && !word_is(v, n, "none")) d->clip = 1U;
    } else if (word_is(name, name_n, "width") || word_is(name, name_n, "height")) {
        uint8_t unit;
        int32_t value;
        if (length(t[0].p, t[0].n, &unit, &value) && unit == U_PX && value <= 64) d->tiny = 1U;
        else if (name[0] == 'w') put_size(d, P_WIDTH, &t[0]);
    } else if (word_is(name, name_n, "max-width")) {
        put_size(d, P_MAX_WIDTH, &t[0]);
    } else if (word_is(name, name_n, "float")) {
        put(d, P_FLOAT, U_NONE, word_is(v, n, "left") ? OW_FLOAT_LEFT :
            word_is(v, n, "right") ? OW_FLOAT_RIGHT : OW_FLOAT_NONE);
    } else if (word_is(name, name_n, "clear")) {
        put(d, P_CLEAR, U_NONE, word_is(v, n, "left") ? OW_SIDE_LEFT :
            word_is(v, n, "right") ? OW_SIDE_RIGHT :
            word_is(v, n, "none") ? 0 : OW_SIDE_LEFT | OW_SIDE_RIGHT);
    } else if (word_is(name, name_n, "flex-direction") || word_is(name, name_n, "flex-flow")) {
        put(d, P_FLEX_COLUMN, U_NONE, has(v, n, "column"));
    } else if (word_is(name, name_n, "border") || word_is(name, name_n, "border-style")) {
        const int shows = name_n == 6U ? border_shows(v, n) :
                          !(has(v, n, "none") || has(v, n, "hidden"));
        if (name_n == 6U || !shows) {
            for (uint8_t side = 0; side < 4U; side++) put(d, (uint8_t)(P_BORDER_TOP + side), U_NONE, shows);
        }
    } else if (word_is(name, name_n, "border-top")) {
        put(d, P_BORDER_TOP, U_NONE, border_shows(v, n));
    } else if (word_is(name, name_n, "border-right")) {
        put(d, P_BORDER_RIGHT, U_NONE, border_shows(v, n));
    } else if (word_is(name, name_n, "border-bottom")) {
        put(d, P_BORDER_BOTTOM, U_NONE, border_shows(v, n));
    } else if (word_is(name, name_n, "border-left")) {
        put(d, P_BORDER_LEFT, U_NONE, border_shows(v, n));
    } else if (word_is(name, name_n, "border-width")) {
        if (word_is(v, n, "0") || word_is(v, n, "0px")) {
            for (uint8_t side = 0; side < 4U; side++) put(d, (uint8_t)(P_BORDER_TOP + side), U_NONE, 0);
        }
    } else if (word_is(name, name_n, "overflow")) {
        if (has(v, n, "hidden")) d->overflow = 1U;
    }
}

/* Reads "name: value; ..." into a list. Returns the number of entries. */
static int declarations(const char *p, const char *end, decl_t *list, int max)
{
    decls_t d;
    char name[40], value[VALUE_MAX];

    memset(&d, 0, sizeof(d));
    d.list = list; d.max = max - 1;
    while (p < end) {
        const char *colon, *stop;
        size_t name_n = 0, n;
        int depth = 0;
        char quote = 0;

        while (p < end && (is_space(*p) || *p == ';')) p++;
        colon = p;
        while (colon < end && *colon != ':' && *colon != ';' && *colon != '{') colon++;
        if (colon >= end) break;
        if (*colon != ':') {                /* not a declaration: skip it */
            if (*colon == '{') {
                for (depth = 0; colon < end; colon++) {
                    if (*colon == '{') depth++;
                    else if (*colon == '}' && --depth == 0) break;
                }
            }
            p = colon < end ? colon + 1 : end;
            continue;
        }
        for (stop = colon + 1; stop < end; stop++) {
            if (quote != 0) { if (*stop == quote) quote = 0; }
            else if (*stop == '"' || *stop == '\'') quote = *stop;
            else if (*stop == '(') depth++;
            else if (*stop == ')') { if (depth > 0) depth--; }
            else if (*stop == ';' && depth == 0) break;
        }
        while (p < colon && is_space(*p)) p++;
        for (; p < colon && !is_space(*p) && name_n + 1U < sizeof(name); p++) {
            name[name_n++] = (char)lower((unsigned char)*p);
        }
        n = prepare(colon + 1, (size_t)(stop - colon - 1), value, &d.important);
        if (n != 0U && name_n != 0U && name[0] != '-') declaration(&d, name, name_n, value, n);
        p = stop < end ? stop + 1 : end;
    }
    if (d.away && (d.clip || (d.tiny && d.overflow))) {
        d.max = max; d.important = 1;
        put(&d, P_DISPLAY, U_NONE, OW_D_NONE);
    }
    return d.count;
}

/* ---- Selectors --------------------------------------------------------- */

/* Reads a name with CSS escapes. Returns the bytes used. */
static size_t ident(const char *p, const char *end, char *out, size_t cap)
{
    const char *start = p;
    size_t n = 0;

    while (p < end && (is_ident((unsigned char)*p) || *p == '\\')) {
        if (*p == '\\' && p + 1 < end) p++;
        if (n + 1U < cap) out[n++] = *p;
        p++;
    }
    out[n] = '\0';
    return (size_t)(p - start);
}

/* Reads one compound selector. Returns the bytes used, or 0 when the
 * selector has a part that this engine does not support. */
static size_t compound(onewww_css_t *css, const char *p, const char *end,
                       compound_t *c, uint32_t *specificity, int nested)
{
    const char *start = p;
    char name[128];

    memset(c, 0, sizeof(*c));
    while (p < end) {
        size_t used;
        if (*p == '*') { p++; continue; }
        if (is_ident((unsigned char)*p) || *p == '\\') {
            if (p != start && p[-1] != '*') return 0U;
            used = ident(p, end, name, sizeof(name));
            for (char *q = name; *q != '\0'; q++) *q = (char)lower((unsigned char)*q);
            c->tag = onewww_tag_find(name, strlen(name));
            if (c->tag == OW_UNKNOWN) return 0U;
            *specificity += 1U;
            p += used;
        } else if (*p == '#' || *p == '.') {
            const char kind = *p++;
            used = ident(p, end, name, sizeof(name));
            if (used == 0U) return 0U;
            if (kind == '#') { c->id = keep(css, name, strlen(name)); *specificity += 0x10000U; }
            else {
                if (c->classes >= MAX_CLASSES) return 0U;
                c->cls[c->classes++] = keep(css, name, strlen(name));
                *specificity += 0x100U;
            }
            p += used;
        } else if (*p == '[') {
            const char *close = memchr(p, ']', (size_t)(end - p));
            const char *q = p + 1;
            if (close == NULL || c->attr_op != A_NONE) return 0U;
            used = ident(q, close, name, sizeof(name));
            if (used == 0U) return 0U;
            for (char *s = name; *s != '\0'; s++) *s = (char)lower((unsigned char)*s);
            c->attr_name = keep(css, name, strlen(name));
            q += used;
            c->attr_op = A_EXISTS;
            if (q < close) {
                switch (*q) {
                case '=': c->attr_op = A_EQUAL; break;
                case '~': c->attr_op = A_WORD; q++; break;
                case '^': c->attr_op = A_PREFIX; q++; break;
                case '$': c->attr_op = A_SUFFIX; q++; break;
                case '*': c->attr_op = A_CONTAINS; q++; break;
                case '|': c->attr_op = A_DASH; q++; break;
                default: return 0U;
                }
                if (q >= close || *q != '=') return 0U;
                q++;
                if (q < close && (*q == '"' || *q == '\'')) {
                    const char *e = memchr(q + 1, *q, (size_t)(close - q - 1));
                    if (e == NULL) return 0U;
                    c->attr_value = keep(css, q + 1, (size_t)(e - q - 1));
                } else {
                    const char *e = q;
                    while (e < close && !is_space(*e)) e++;
                    c->attr_value = keep(css, q, (size_t)(e - q));
                }
            }
            *specificity += 0x100U;
            p = close + 1;
        } else if (*p == ':') {
            if (p + 1 < end && p[1] == ':') return 0U;      /* pseudo-element */
            p++;
            used = ident(p, end, name, sizeof(name));
            for (char *q = name; *q != '\0'; q++) *q = (char)lower((unsigned char)*q);
            p += used;
            *specificity += 0x100U;
            if (strcmp(name, "first-child") == 0) c->flags |= F_FIRST;
            else if (strcmp(name, "last-child") == 0) c->flags |= F_LAST;
            else if (strcmp(name, "only-child") == 0) c->flags |= F_FIRST | F_LAST;
            else if (strcmp(name, "empty") == 0) c->flags |= F_EMPTY;
            else if (strcmp(name, "root") == 0) c->flags |= F_ROOT;
            else if (strcmp(name, "link") == 0 || strcmp(name, "any-link") == 0) c->flags |= F_LINK;
            else if (strcmp(name, "not") == 0 && !nested && p < end && *p == '(') {
                const char *close = memchr(p, ')', (size_t)(end - p));
                uint32_t inner = 0;
                compound_t *negated = arena(css, sizeof(*negated));
                const char *q = p + 1;
                if (close == NULL || negated == NULL || c->not != NULL) return 0U;
                while (q < close && is_space(*q)) q++;
                /* No element has the focus or the pointer on this screen:
                 * :not(:focus) and the like are always true. */
                if (q < close && *q == ':' &&
                    (has(q, (size_t)(close - q), "focus") || has(q, (size_t)(close - q), "hover") ||
                     has(q, (size_t)(close - q), "active") || has(q, (size_t)(close - q), "target") ||
                     has(q, (size_t)(close - q), "checked") || has(q, (size_t)(close - q), "visited"))) {
                    p = close + 1;
                    continue;
                }
                used = compound(css, q, close, negated, &inner, 1);
                q += used;
                while (q < close && is_space(*q)) q++;
                if (used == 0U || q != close) return 0U;
                c->not = negated;
                *specificity += inner - 0x100U;
                p = close + 1;
            } else return 0U;
        } else break;
    }
    return (size_t)(p - start);
}

static void rule_add(onewww_css_t *css, const char *p, const char *end,
                     const decl_t *decls, int decl_count)
{
    compound_t parts[MAX_PARTS], *copy;
    const compound_t *subject;
    rule_t *rule, **bucket;
    uint32_t specificity = 0;
    int count = 0;
    char combinator = 0;

    while (p < end) {
        size_t used;
        int spaces = 0;
        while (p < end && is_space(*p)) { p++; spaces = 1; }
        if (p >= end) break;
        if (*p == '>' || *p == '+' || *p == '~') {
            if (count == 0) return;
            combinator = *p++;
            while (p < end && is_space(*p)) p++;
        } else if (spaces && count != 0) combinator = ' ';
        if (count == MAX_PARTS) return;
        used = compound(css, p, end, &parts[count], &specificity, 0);
        if (used == 0U) return;
        parts[count].combinator = count == 0 ? 0 : combinator;
        if (count != 0 && combinator == 0) return;
        combinator = 0;
        count++;
        p += used;
    }
    if (count == 0) return;
    rule = arena(css, sizeof(*rule));
    copy = arena(css, sizeof(compound_t) * (size_t)count);
    if (rule == NULL || copy == NULL) return;
    memcpy(copy, parts, sizeof(compound_t) * (size_t)count);
    rule->parts = copy; rule->count = (uint8_t)count;
    rule->decls = decls; rule->decl_count = (uint16_t)decl_count;
    rule->specificity = specificity; rule->order = css->order++;
    rule->author = css->author;
    subject = &copy[count - 1];
    if (subject->id != NULL) bucket = &css->by_hash[hash_text('#', subject->id)];
    else if (subject->classes != 0U) bucket = &css->by_hash[hash_text('.', subject->cls[0])];
    else if (subject->tag != 0U) bucket = &css->by_tag[subject->tag];
    else bucket = &css->universal;
    rule->next = *bucket;
    *bucket = rule;
}

/* ---- Style sheets ------------------------------------------------------ */

static const char *block_end(const char *p, const char *end)
{
    int depth = 0;
    char quote = 0;

    for (; p < end; p++) {
        if (quote != 0) { if (*p == quote) quote = 0; else if (*p == '\\') p++; }
        else if (*p == '"' || *p == '\'') quote = *p;
        else if (*p == '{') depth++;
        else if (*p == '}' && --depth == 0) return p;
    }
    return end;
}

static int feature_ok(const char *p, size_t n)
{
    const char *colon = memchr(p, ':', n);
    uint8_t unit;
    int32_t value;
    size_t name_n, value_n;

    if (colon == NULL) return has(p, n, "color") || has(p, n, "min-width");
    name_n = (size_t)(colon - p);
    while (name_n != 0U && is_space(p[name_n - 1U])) name_n--;
    colon++;
    while (colon < p + n && is_space(*colon)) colon++;
    value_n = (size_t)(p + n - colon);
    while (value_n != 0U && is_space(colon[value_n - 1U])) value_n--;
    if (word_is(p, name_n, "min-width") || word_is(p, name_n, "max-width")) {
        if (!length(colon, value_n, &unit, &value)) return 0;
        if (unit == U_EM) value *= 16;
        return p[1] == 'i' ? 640 * 64 >= value : 640 * 64 <= value;
    }
    if (word_is(p, name_n, "orientation")) return word_is(colon, value_n, "landscape");
    if (word_is(p, name_n, "prefers-reduced-motion")) return word_is(colon, value_n, "reduce");
    if (word_is(p, name_n, "prefers-color-scheme")) return word_is(colon, value_n, "light");
    if (word_is(p, name_n, "hover") || word_is(p, name_n, "any-hover")) return word_is(colon, value_n, "none");
    return 0;
}

/* Is a media query list true for this screen? The text is lower case. */
static int media_ok(const char *p, size_t n)
{
    const char *end = p + n;

    while (p < end) {
        const char *comma = memchr(p, ',', (size_t)(end - p));
        const char *stop = comma != NULL ? comma : end;
        int ok = 1, negate = 0;
        while (p < stop) {
            const char *q;
            while (p < stop && is_space(*p)) p++;
            if (p >= stop) break;
            if (*p == '(') {
                q = memchr(p, ')', (size_t)(stop - p));
                if (q == NULL) { ok = 0; break; }
                if (!feature_ok(p + 1, (size_t)(q - p - 1))) ok = 0;
                p = q + 1;
                continue;
            }
            for (q = p; q < stop && !is_space(*q) && *q != '('; q++) {}
            if (word_is(p, (size_t)(q - p), "not")) negate = 1;
            else if (word_is(p, (size_t)(q - p), "print") || word_is(p, (size_t)(q - p), "speech")) ok = 0;
            else if (!word_is(p, (size_t)(q - p), "only") && !word_is(p, (size_t)(q - p), "and") &&
                     !word_is(p, (size_t)(q - p), "screen") && !word_is(p, (size_t)(q - p), "all")) ok = 0;
            p = q;
        }
        if (ok != negate) return 1;
        p = stop + 1;
    }
    return n == 0U;
}

static void sheet(onewww_css_t *css, const char *p, const char *end, int depth)
{
    decl_t list[MAX_DECLS];

    while (p < end && !css->failed) {
        const char *open, *close;
        while (p < end && (is_space(*p) || *p == ';' || *p == '}')) p++;
        if (p >= end) break;
        open = p;
        while (open < end && *open != '{' && *open != ';') open++;
        if (open >= end) break;
        if (*open == ';') { p = open + 1; continue; }       /* @import ...; */
        close = block_end(open, end);
        if (*p == '@') {
            char query[VALUE_MAX], at_name[12];
            size_t n = 0, name_n = 0;
            const char *q = p + 1;
            for (; q < open && is_ident((unsigned char)*q); q++) {
                if (name_n + 1U < sizeof(at_name)) at_name[name_n++] = (char)lower((unsigned char)*q);
            }
            if (word_is(at_name, name_n, "media") && depth < 4) {
                while (q < open && n + 1U < sizeof(query)) query[n++] = (char)lower((unsigned char)*q++);
                if (q == open) {            /* a longer query is not understood */
                    const char *text = query;
                    while (n != 0U && is_space(query[n - 1U])) n--;
                    while (n != 0U && is_space(*text)) { text++; n--; }
                    if (media_ok(text, n)) sheet(css, open + 1, close, depth + 1);
                }
            } else if (word_is(at_name, name_n, "layer") && depth < 4) {
                sheet(css, open + 1, close, depth + 1);
            }
        } else {
            const int count = declarations(open + 1, close, list, MAX_DECLS);
            if (count > 0) {
                decl_t *kept = arena(css, sizeof(decl_t) * (size_t)count);
                const char *s = p;
                if (kept == NULL) return;
                memcpy(kept, list, sizeof(decl_t) * (size_t)count);
                while (s < open) {              /* one rule for each selector */
                    const char *comma = s;
                    int parens = 0;
                    for (; comma < open; comma++) {
                        if (*comma == '(' || *comma == '[') parens++;
                        else if (*comma == ')' || *comma == ']') parens--;
                        else if (*comma == ',' && parens == 0) break;
                    }
                    {
                        const char *e = comma;
                        while (e > s && is_space(e[-1])) e--;
                        rule_add(css, s, e, kept, count);
                    }
                    s = comma + 1;
                }
            }
        }
        p = close < end ? close + 1 : end;
    }
}

void onewww_css_add(onewww_css_t *css, const char *text, size_t length)
{
    char *copy;
    size_t n = 0;

    if (css == NULL || length == 0U) return;
    copy = malloc(length + 1U);
    if (copy == NULL) return;
    for (size_t i = 0; i < length; i++) {       /* comments become spaces */
        if (text[i] == '/' && i + 1U < length && text[i + 1U] == '*') {
            for (i += 2U; i + 1U < length && !(text[i] == '*' && text[i + 1U] == '/'); i++) {}
            i++;
            copy[n++] = ' ';
        } else if (text[i] == '<' && i + 3U < length && memcmp(text + i, "<!--", 4U) == 0) {
            i += 3U;
        } else copy[n++] = text[i] == '\0' ? ' ' : text[i];
    }
    copy[n] = '\0';
    sheet(css, copy, copy + n, 0);
    free(copy);
}

onewww_css_t *onewww_css_new(void)
{
    onewww_css_t *css = calloc(1, sizeof(*css));

    if (css == NULL) return NULL;
    onewww_css_add(css, s_default_sheet, sizeof(s_default_sheet) - 1U);
    css->author = 1U;
    if (css->failed) { onewww_css_free(css); return NULL; }
    return css;
}

void onewww_css_free(onewww_css_t *css)
{
    if (css == NULL) return;
    while (css->chunks != NULL) {
        chunk_t *next = css->chunks->next;
        free(css->chunks);
        css->chunks = next;
    }
    free(css);
}

/* ---- Matching ---------------------------------------------------------- */

static const onewww_node_t *before(const onewww_node_t *e)
{
    for (e = e->prev; e != NULL && e->tag == OW_TEXT; e = e->prev) {}
    return e;
}

static const onewww_node_t *after(const onewww_node_t *e)
{
    for (e = e->next; e != NULL && e->tag == OW_TEXT; e = e->next) {}
    return e;
}

static int class_has(const char *classes, const char *name)
{
    const size_t n = strlen(name);
    const char *p = classes;

    while ((p = strstr(p, name)) != NULL) {
        if ((p == classes || is_space(p[-1])) && (p[n] == '\0' || is_space(p[n]))) return 1;
        p += n;
    }
    return 0;
}

static int attr_ok(const compound_t *c, const onewww_node_t *e)
{
    static char value[512];
    const char *want = c->attr_value != NULL ? c->attr_value : "";
    const size_t want_n = strlen(want);
    int n;

    if (e->length == 0U) return 0;
    n = onewww_attr(e, c->attr_name, value, sizeof(value));
    if (n < 0) return 0;
    switch (c->attr_op) {
    case A_EXISTS: return 1;
    case A_EQUAL: return strcmp(value, want) == 0;
    case A_WORD: return want_n != 0U && class_has(value, want);
    case A_PREFIX: return want_n != 0U && strncmp(value, want, want_n) == 0;
    case A_SUFFIX: return want_n != 0U && (size_t)n >= want_n && strcmp(value + n - want_n, want) == 0;
    case A_CONTAINS: return want_n != 0U && strstr(value, want) != NULL;
    case A_DASH: return strncmp(value, want, want_n) == 0 && (value[want_n] == '\0' || value[want_n] == '-');
    default: return 0;
    }
}

static int compound_ok(const compound_t *c, const onewww_node_t *e)
{
    if (c->tag != 0U && c->tag != e->tag) return 0;
    if (c->id != NULL && strcmp(c->id, e->id) != 0) return 0;
    for (unsigned i = 0; i < c->classes; i++) {
        if (!class_has(e->classes, c->cls[i])) return 0;
    }
    if ((c->flags & F_FIRST) && before(e) != NULL) return 0;
    if ((c->flags & F_LAST) && after(e) != NULL) return 0;
    if ((c->flags & F_EMPTY) && e->first != NULL) return 0;
    if ((c->flags & F_ROOT) && e->parent != NULL) return 0;
    if (c->flags & F_LINK) {
        static char href[8];
        if (e->tag != OW_A || onewww_attr(e, "href", href, sizeof(href)) < 0) return 0;
    }
    if (c->attr_op != A_NONE && !attr_ok(c, e)) return 0;
    if (c->not != NULL && compound_ok(c->not, e)) return 0;
    return 1;
}

/* Does part `at` of the rule match e, and the parts on its left match
 * the elements that the combinators name? */
static int rule_ok(const rule_t *rule, int at, const onewww_node_t *e)
{
    const compound_t *c = &rule->parts[at];

    if (!compound_ok(c, e)) return 0;
    if (at == 0) return 1;
    switch (c->combinator) {
    case '>':
        return e->parent != NULL && rule_ok(rule, at - 1, e->parent);
    case '+':
        e = before(e);
        return e != NULL && rule_ok(rule, at - 1, e);
    case '~':
        for (e = before(e); e != NULL; e = before(e)) {
            if (rule_ok(rule, at - 1, e)) return 1;
        }
        return 0;
    default:
        for (e = e->parent; e != NULL; e = e->parent) {
            if (rule_ok(rule, at - 1, e)) return 1;
        }
        return 0;
    }
}

typedef struct {
    uint64_t key;
    const rule_t *rule;
} match_t;

static void collect(const rule_t *rule, const onewww_node_t *e,
                    match_t *list, int *count)
{
    for (; rule != NULL; rule = rule->next) {
        uint64_t key;
        int at;
        if (*count >= MAX_MATCH || !rule_ok(rule, rule->count - 1, e)) continue;
        key = ((uint64_t)rule->author << 56) | ((uint64_t)rule->specificity << 28) | rule->order;
        for (at = (*count)++; at > 0 && list[at - 1].key > key; at--) list[at] = list[at - 1];
        list[at].key = key; list[at].rule = rule;
    }
}

/* ---- Cascade ----------------------------------------------------------- */

static int16_t to_size(const decl_t *d, int font_px)
{
    int32_t v;
    if (d->unit == U_PERCENT) return (int16_t)(d->value > 100 ? -100 : -d->value);
    v = d->unit == U_EM ? d->value * font_px / 64 : d->value / 64;
    return (int16_t)(v > 4000 ? 4000 : v < 0 ? 0 : v);
}

static void set_side(uint8_t *sides, uint8_t side, int on)
{
    *sides = (uint8_t)(on ? *sides | side : *sides & ~side);
}

static int16_t to_pixels(const decl_t *d, int font_px, int limit)
{
    int32_t v = d->unit == U_EM ? d->value * font_px / 64 :
                (d->unit == U_AUTO || d->unit == U_PERCENT) ? 0 : d->value / 64;
    if (v < 0) v = 0;
    if (v > limit) v = limit;
    return (int16_t)v;
}

static void apply(onewww_style_t *s, int parent_font, const decl_t *d, int count,
                  int important, int fonts)
{
    for (; count > 0; count--, d++) {
        if (d->important != important || (d->prop == P_FONT_SIZE) != fonts) continue;
        switch (d->prop) {
        case P_FONT_SIZE: {
            int32_t px = d->unit == U_EM ? d->value * parent_font / 64 : d->value / 64;
            s->font_px = (uint8_t)(px < 6 ? 6 : px > 96 ? 96 : px);
            break;
        }
        case P_DISPLAY: s->display = (uint8_t)d->value; break;
        case P_COLOR: s->color = (uint16_t)d->value; break;
        case P_BACKGROUND: s->background = (uint16_t)d->value; break;
        case P_BOLD: s->bold = (uint8_t)d->value; break;
        case P_ITALIC: s->italic = (uint8_t)d->value; break;
        case P_MONO: s->mono = (uint8_t)d->value; break;
        case P_ALIGN: s->align = (uint8_t)d->value; break;
        case P_UNDERLINE: s->underline = (uint8_t)d->value; break;
        case P_WHITE_SPACE:
            s->pre = d->value == WS_PRE; s->nowrap = d->value == WS_NOWRAP;
            break;
        case P_LIST_NONE: s->list_none = (uint8_t)d->value; break;
        case P_MARGIN_TOP: s->margin_top = to_pixels(d, s->font_px, 96); break;
        case P_MARGIN_BOTTOM: s->margin_bottom = to_pixels(d, s->font_px, 96); break;
        case P_MARGIN_LEFT:
            s->margin_left = to_pixels(d, s->font_px, 240);
            set_side(&s->margin_auto, OW_SIDE_LEFT, d->unit == U_AUTO);
            break;
        case P_MARGIN_RIGHT:
            s->margin_right = to_pixels(d, s->font_px, 240);
            set_side(&s->margin_auto, OW_SIDE_RIGHT, d->unit == U_AUTO);
            break;
        case P_WIDTH: s->width = to_size(d, s->font_px); break;
        case P_MAX_WIDTH: s->max_width = to_size(d, s->font_px); break;
        case P_FLOAT: s->floating = (uint8_t)d->value; break;
        case P_CLEAR: s->clear = (uint8_t)d->value; break;
        case P_FLEX_COLUMN: s->flex_column = (uint8_t)d->value; break;
        case P_BORDER_TOP: set_side(&s->border, OW_SIDE_TOP, d->value); break;
        case P_BORDER_RIGHT: set_side(&s->border, OW_SIDE_RIGHT, d->value); break;
        case P_BORDER_BOTTOM: set_side(&s->border, OW_SIDE_BOTTOM, d->value); break;
        case P_BORDER_LEFT: set_side(&s->border, OW_SIDE_LEFT, d->value); break;
        case P_PADDING_TOP: s->padding_top = to_pixels(d, s->font_px, 96); break;
        case P_PADDING_BOTTOM: s->padding_bottom = to_pixels(d, s->font_px, 96); break;
        case P_PADDING_LEFT: s->padding_left = to_pixels(d, s->font_px, 240); break;
        case P_PADDING_RIGHT: s->padding_right = to_pixels(d, s->font_px, 240); break;
        default: break;
        }
    }
}

/* Old attributes that set a style: align, bgcolor, <font color>. */
static void hints(onewww_style_t *s, const onewww_node_t *e)
{
    static char value[40];
    uint16_t c;
    int n;

    if (e->length < 5U) return;
    if (has(e->text, e->length, "align") &&
        (n = onewww_attr(e, "align", value, sizeof(value))) > 0) {
        for (char *q = value; *q != '\0'; q++) *q = (char)lower((unsigned char)*q);
        if (e->tag == OW_TABLE || e->tag == OW_IMG) {
            /* On a table or a picture, align puts the box at a side. */
            if (strcmp(value, "right") == 0) s->floating = OW_FLOAT_RIGHT;
            else if (strcmp(value, "left") == 0) s->floating = OW_FLOAT_LEFT;
            else if (strcmp(value, "center") == 0) s->margin_auto = OW_SIDE_LEFT | OW_SIDE_RIGHT;
        } else if (strcmp(value, "center") == 0 || strcmp(value, "middle") == 0) s->align = OW_ALIGN_CENTER;
        else if (strcmp(value, "right") == 0) s->align = OW_ALIGN_RIGHT;
        else if (strcmp(value, "left") == 0) s->align = OW_ALIGN_LEFT;
    }
    if (e->tag == OW_TABLE && has(e->text, e->length, "width") &&
        (n = onewww_attr(e, "width", value, sizeof(value))) > 0) {
        const char *q = value;
        int32_t v;
        if (number(&q, value + n, &v) && v > 0) {
            v /= 64;
            s->width = (int16_t)(*q == '%' ? (v > 100 ? -100 : -v) : (v > 4000 ? 4000 : v));
        }
    }
    if (has(e->text, e->length, "color")) {
        const char *name = e->tag == OW_FONT ? "color" : e->tag == OW_BODY ? "text" : NULL;
        for (int pass = 0; pass < 2; pass++) {
            const char *attr = pass == 0 ? name : "bgcolor";
            if (attr == NULL || (n = onewww_attr(e, attr, value, sizeof(value))) <= 0) continue;
            for (char *q = value; *q != '\0'; q++) *q = (char)lower((unsigned char)*q);
            if (!color(value, (size_t)n, &c) || c == OW_NO_COLOR) continue;
            if (pass == 0) s->color = c; else s->background = c;
        }
    }
}

static int style_element(onewww_css_t *css, onewww_dom_t *dom, onewww_node_t *e)
{
    static match_t list[MAX_MATCH];
    static char text[INLINE_MAX];
    decl_t own[MAX_DECLS];
    const onewww_style_t *up = e->parent != NULL ? e->parent->style : NULL;
    onewww_style_t *s = onewww_dom_alloc(dom, sizeof(*s));
    int count = 0, own_count = 0, first_author, parent_font;

    if (s == NULL) return -1;
    memset(s, 0, sizeof(*s));
    s->background = OW_NO_COLOR; s->font_px = 16U; s->display = OW_D_INLINE;
    if (up != NULL) {           /* the properties that an element inherits */
        s->color = up->color; s->font_px = up->font_px; s->bold = up->bold;
        s->italic = up->italic; s->mono = up->mono; s->underline = up->underline;
        s->pre = up->pre; s->nowrap = up->nowrap; s->align = up->align;
        s->list_none = up->list_none;
    }
    parent_font = s->font_px;
    e->style = s;

    if (e->id[0] != '\0') collect(css->by_hash[hash_text('#', e->id)], e, list, &count);
    for (const char *p = e->classes; *p != '\0';) {
        char name[128];
        size_t n = 0;
        while (is_space(*p)) p++;
        while (*p != '\0' && !is_space(*p)) { if (n + 1U < sizeof(name)) name[n++] = *p; p++; }
        name[n] = '\0';
        if (n != 0U) collect(css->by_hash[hash_text('.', name)], e, list, &count);
    }
    collect(css->by_tag[e->tag], e, list, &count);
    collect(css->universal, e, list, &count);
    for (first_author = 0; first_author < count && list[first_author].rule->author == 0U; first_author++) {}

    if (e->length >= 7U && has(e->text, e->length, "style") &&
        onewww_attr(e, "style", text, sizeof(text)) > 0) {
        own_count = declarations(text, text + strlen(text), own, MAX_DECLS);
    }
    for (int fonts = 1; fonts >= 0; fonts--) {
        for (int i = 0; i < first_author; i++) {
            apply(s, parent_font, list[i].rule->decls, list[i].rule->decl_count, 0, fonts);
        }
        if (!fonts) hints(s, e);
        for (int i = first_author; i < count; i++) {
            apply(s, parent_font, list[i].rule->decls, list[i].rule->decl_count, 0, fonts);
        }
        apply(s, parent_font, own, own_count, 0, fonts);
        for (int i = 0; i < count; i++) {
            apply(s, parent_font, list[i].rule->decls, list[i].rule->decl_count, 1, fonts);
        }
        apply(s, parent_font, own, own_count, 1, fonts);
    }
    return 0;
}

int onewww_css_resolve(onewww_css_t *css, onewww_dom_t *dom)
{
    onewww_css_t *own = NULL;
    onewww_node_t *e = onewww_dom_root(dom);
    unsigned steps = 0U;
    int result = 0;

    if (css == NULL) {
        css = own = onewww_css_new();
        if (css == NULL) return -1;
    }
    while (e != NULL && result == 0) {
        if (e->tag != OW_TEXT) {
            if (style_element(css, dom, e) != 0) result = -1;
            if ((++steps & 31U) == 0U && onewww_pause != NULL && onewww_pause()) result = -1;
        }
        if (e->first != NULL && e->tag != OW_TEXT) e = e->first;
        else {
            while (e != NULL && e->next == NULL) e = e->parent;
            if (e != NULL) e = e->next;
        }
    }
    onewww_css_free(own);
    return result;
}
