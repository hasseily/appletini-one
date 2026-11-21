/* ONEWWW document tree. See onewww_dom.h. No hardware access. */
#include "onewww_dom.h"

#include <stdlib.h>
#include <string.h>

#define CHUNK_BYTES 65536U
#define MAX_DEPTH   96

typedef struct chunk {
    struct chunk *next;
    size_t used, size;
} chunk_t;

struct onewww_dom {
    chunk_t *chunks;
    onewww_node_t *root;
    const char *title;
    uint8_t failed;
    uint8_t latin1;
};

static const char *const s_names[OW_TAG_COUNT] = {
    "", "",
    "html", "head", "body", "title", "script", "style", "meta",
    "link", "base",
    "p", "div", "span", "a", "b", "strong", "i", "em", "u", "s",
    "code", "tt", "kbd", "samp", "var", "cite", "pre", "br", "hr",
    "h1", "h2", "h3", "h4", "h5", "h6",
    "ul", "ol", "li", "dl", "dt", "dd",
    "table", "thead", "tbody", "tfoot", "tr", "td", "th", "caption",
    "col", "colgroup",
    "img", "form", "input", "button", "select", "option", "textarea",
    "label", "blockquote", "center", "small", "big", "sub", "sup",
    "font", "nav", "header", "footer", "section", "article", "aside",
    "main", "figure", "figcaption", "address", "noscript", "iframe",
    "svg", "template", "wbr", "area", "embed", "source", "track",
    "object", "video", "audio", "canvas", "map", "del", "ins",
    "strike", "mark", "q", "abbr", "details", "summary", "fieldset",
    "legend", "hgroup", "dfn",
};

static void *arena(onewww_dom_t *dom, size_t bytes)
{
    chunk_t *c = dom->chunks;
    void *p;

    bytes = (bytes + 7U) & ~(size_t)7U;
    if (c == NULL || c->used + bytes > c->size) {
        const size_t size = bytes > CHUNK_BYTES ? bytes : CHUNK_BYTES;
        c = malloc(sizeof(*c) + size);
        if (c == NULL) { dom->failed = 1U; return NULL; }
        c->next = dom->chunks; c->used = 0U; c->size = size;
        dom->chunks = c;
    }
    p = (uint8_t *)(c + 1) + c->used;
    c->used += bytes;
    return p;
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int is_space(int c)
{ return c == ' ' || c == '\n' || c == '\t' || c == '\r' || c == '\f'; }
static int is_name(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == ':';
}

static int same_name(const char *p, size_t n, const char *name)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (name[i] == '\0' || lower((unsigned char)p[i]) != name[i]) return 0;
    }
    return name[n] == '\0';
}

static uint16_t tag_find(const char *p, size_t n)
{
    for (uint16_t t = OW_HTML; t < OW_TAG_COUNT; t++) {
        if (same_name(p, n, s_names[t])) return t;
    }
    return OW_UNKNOWN;
}

static size_t put_utf8(char *out, uint32_t code)
{
    if (code < 0x80U) { out[0] = (char)code; return 1U; }
    if (code < 0x800U) {
        out[0] = (char)(0xC0U | (code >> 6));
        out[1] = (char)(0x80U | (code & 0x3FU));
        return 2U;
    }
    if (code > 0xFFFFU) code = '?';
    if (code < 0x80U) { out[0] = (char)code; return 1U; }
    out[0] = (char)(0xE0U | (code >> 12));
    out[1] = (char)(0x80U | ((code >> 6) & 0x3FU));
    out[2] = (char)(0x80U | (code & 0x3FU));
    return 3U;
}

/* Windows-1252 code points for bytes $80-$9F. */
static const uint16_t s_cp1252[32] = {
    0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x8D, 0x017D, 0x8F,
    0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D, 0x017E, 0x0178,
};

/* Decodes one entity at p ('&'). Returns the bytes used, or 0. */
static size_t entity(const char *p, const char *end, uint32_t *code)
{
    const char *q = p + 1;
    size_t n = 0;
    int lo = 0, hi;

    if (q < end && *q == '#') {
        uint32_t v = 0;
        int digits = 0;
        q++;
        if (q < end && (*q == 'x' || *q == 'X')) {
            for (q++; q < end; q++, digits++) {
                const int c = lower((unsigned char)*q);
                if (c >= '0' && c <= '9') v = v * 16U + (uint32_t)(c - '0');
                else if (c >= 'a' && c <= 'f') v = v * 16U + (uint32_t)(c - 'a' + 10);
                else break;
                if (v > 0x10FFFFU) v = 0x10FFFFU;
            }
        } else {
            for (; q < end && *q >= '0' && *q <= '9'; q++, digits++) {
                v = v * 10U + (uint32_t)(*q - '0');
                if (v > 0x10FFFFU) v = 0x10FFFFU;
            }
        }
        if (digits == 0) return 0U;
        if (q < end && *q == ';') q++;
        if (v >= 0x80U && v < 0xA0U) v = s_cp1252[v - 0x80U];
        if (v == 0U) v = 0xFFFDU;
        *code = v;
        return (size_t)(q - p);
    }
    while (q + n < end && n < 32U &&
           ((q[n] >= 'a' && q[n] <= 'z') || (q[n] >= 'A' && q[n] <= 'Z') ||
            (q[n] >= '0' && q[n] <= '9'))) n++;
    if (n == 0U) return 0U;
    hi = (int)onewww_entity_count - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        const onewww_entity_t *e = &onewww_entities[mid];
        int cmp = strncmp(q, e->name, n);
        if (cmp == 0 && e->name[n] != '\0') cmp = -1;
        if (cmp == 0) {
            const int semicolon = q + n < end && q[n] == ';';
            if (!semicolon && !e->legacy) return 0U;
            *code = e->code;
            return 1U + n + (semicolon ? 1U : 0U);
        }
        if (cmp < 0) hi = mid - 1; else lo = mid + 1;
    }
    return 0U;
}

/* Copies text to the arena as UTF-8 with entities decoded. */
static const char *decode(onewww_dom_t *dom, const char *p, const char *end,
                          int entities, uint32_t *length)
{
    /* Latin-1 bytes grow to at most 3 bytes; entities never grow. */
    char *out = arena(dom, (size_t)(end - p) * (dom->latin1 ? 3U : 1U) + 4U);
    size_t n = 0;

    if (out == NULL) { *length = 0U; return ""; }
    while (p < end) {
        const unsigned char c = (unsigned char)*p;
        uint32_t code;
        size_t used;
        if (c == '&' && entities && (used = entity(p, end, &code)) != 0U) {
            n += put_utf8(out + n, code);
            p += used;
        } else if (c >= 0x80U && dom->latin1) {
            n += put_utf8(out + n, c < 0xA0U ? s_cp1252[c - 0x80U] : c);
            p++;
        } else {
            out[n++] = (char)(c == 0U ? ' ' : c);
            p++;
        }
    }
    out[n] = '\0';
    *length = (uint32_t)n;
    return out;
}

static onewww_node_t *node_new(onewww_dom_t *dom, onewww_node_t *parent,
                               uint16_t tag)
{
    onewww_node_t *n = arena(dom, sizeof(*n));

    if (n == NULL) return NULL;
    memset(n, 0, sizeof(*n));
    n->tag = tag; n->parent = parent; n->text = "";
    if (parent != NULL) {
        if (parent->last != NULL) parent->last->next = n; else parent->first = n;
        parent->last = n;
    }
    return n;
}

static int is_void(uint16_t t)
{
    return t == OW_BR || t == OW_HR || t == OW_IMG || t == OW_INPUT ||
           t == OW_META || t == OW_LINK || t == OW_BASE || t == OW_AREA ||
           t == OW_COL || t == OW_EMBED || t == OW_SOURCE || t == OW_TRACK ||
           t == OW_WBR;
}

static int closes_p(uint16_t t)
{
    switch (t) {
    case OW_P: case OW_DIV: case OW_UL: case OW_OL: case OW_DL: case OW_TABLE:
    case OW_H1: case OW_H2: case OW_H3: case OW_H4: case OW_H5: case OW_H6:
    case OW_PRE: case OW_BLOCKQUOTE: case OW_FORM: case OW_HR: case OW_SECTION:
    case OW_ARTICLE: case OW_ASIDE: case OW_NAV: case OW_HEADER: case OW_FOOTER:
    case OW_MAIN: case OW_ADDRESS: case OW_CENTER: case OW_FIGURE:
    case OW_FIGCAPTION: case OW_LI: case OW_DD: case OW_DT: case OW_FIELDSET:
    case OW_DETAILS: case OW_SUMMARY: case OW_HGROUP:
        return 1;
    default:
        return 0;
    }
}

static int is_cell(uint16_t t) { return t == OW_TD || t == OW_TH; }
static int is_section(uint16_t t)
{ return t == OW_THEAD || t == OW_TBODY || t == OW_TFOOT; }

/* Finds the nearest open element with one of two tags. The search stops
 * at a wall tag. Returns its parent (the new current node), or NULL. */
static onewww_node_t *close_open(onewww_node_t *cur, uint16_t a, uint16_t b,
                                 int wall_cells, uint16_t wall1, uint16_t wall2)
{
    for (onewww_node_t *n = cur; n != NULL && n->parent != NULL; n = n->parent) {
        if (n->tag == a || n->tag == b) return n->parent;
        if (n->tag == wall1 || n->tag == wall2 || n->tag == OW_TABLE) return NULL;
        if (wall_cells && (is_cell(n->tag) || n->tag == OW_CAPTION)) return NULL;
    }
    return NULL;
}

static onewww_node_t *implied_close(onewww_node_t *cur, uint16_t t)
{
    onewww_node_t *up = NULL;

    if (closes_p(t)) up = close_open(cur, OW_P, OW_P, 1, OW_BODY, OW_BODY);
    if (up != NULL) cur = up;
    up = NULL;
    if (t == OW_LI) up = close_open(cur, OW_LI, OW_LI, 1, OW_UL, OW_OL);
    else if (t == OW_DT || t == OW_DD) up = close_open(cur, OW_DT, OW_DD, 1, OW_DL, OW_DL);
    else if (t == OW_OPTION) up = close_open(cur, OW_OPTION, OW_OPTION, 1, OW_SELECT, OW_SELECT);
    else if (t == OW_A) up = close_open(cur, OW_A, OW_A, 1, OW_BODY, OW_BODY);
    else if (is_cell(t)) up = close_open(cur, OW_TD, OW_TH, 0, OW_TR, OW_TR);
    else if (t == OW_TR) {
        up = close_open(cur, OW_TR, OW_TR, 0, OW_TABLE, OW_TABLE);
    } else if (is_section(t)) {
        up = close_open(cur, OW_THEAD, OW_TBODY, 0, OW_TABLE, OW_TABLE);
        if (up == NULL) up = close_open(cur, OW_TFOOT, OW_TFOOT, 0, OW_TABLE, OW_TABLE);
    }
    return up != NULL ? up : cur;
}

static onewww_node_t *end_tag(onewww_node_t *cur, uint16_t t)
{
    const int table_part = t == OW_TABLE || t == OW_TR || is_cell(t) ||
                           is_section(t) || t == OW_CAPTION || t == OW_BODY ||
                           t == OW_HTML;

    if (t == OW_UNKNOWN || t == OW_BODY || t == OW_HTML) return cur;
    for (onewww_node_t *n = cur; n != NULL && n->parent != NULL; n = n->parent) {
        if (n->tag == t) return n->parent;
        /* A stray inline end tag must not close a table cell. */
        if (!table_part && (is_cell(n->tag) || n->tag == OW_TABLE ||
                            n->tag == OW_CAPTION)) break;
        if (n->tag == OW_TABLE && t != OW_TABLE) break;
    }
    return cur;
}

static int depth_of(const onewww_node_t *n)
{
    int d = 0;
    for (; n != NULL; n = n->parent) d++;
    return d;
}

static const char *find_close(const char *p, const char *end, const char *name)
{
    const size_t n = strlen(name);

    for (; p + n + 2U <= end; p++) {
        if (p[0] == '<' && p[1] == '/' && same_name(p + 2, n, name)) return p;
    }
    return end;
}

static int has_text(const char *hay, size_t n, const char *needle)
{
    const size_t m = strlen(needle);

    for (size_t i = 0; i + m <= n; i++) {
        size_t k = 0;
        while (k < m && lower((unsigned char)hay[i + k]) == needle[k]) k++;
        if (k == m) return 1;
    }
    return 0;
}

static int charset_latin1(const uint8_t *html, size_t length,
                          const char *content_type)
{
    const char *where = content_type;
    size_t n = content_type != NULL ? strlen(content_type) : 0U;

    if (!has_text(where != NULL ? where : "", n, "charset=")) {
        where = (const char *)html;
        n = length < 2048U ? length : 2048U;
        if (!has_text(where, n, "charset=")) return 0;
    }
    return has_text(where, n, "iso-8859-1") || has_text(where, n, "windows-1252") ||
           has_text(where, n, "latin1") || has_text(where, n, "iso-8859-15");
}

onewww_dom_t *onewww_dom_parse(const uint8_t *html, size_t length,
                               const char *content_type)
{
    onewww_dom_t *dom = calloc(1, sizeof(*dom));
    const char *p = (const char *)html, *end = p + length;
    onewww_node_t *cur;

    if (dom == NULL) return NULL;
    dom->latin1 = (uint8_t)charset_latin1(html, length, content_type);
    dom->title = "";
    dom->root = cur = node_new(dom, NULL, OW_BODY);
    if (cur == NULL) { onewww_dom_free(dom); return NULL; }

    while (p < end && !dom->failed) {
        const char *q;
        if (*p != '<') {
            onewww_node_t *t;
            q = memchr(p, '<', (size_t)(end - p));
            if (q == NULL) q = end;
            t = node_new(dom, cur, OW_TEXT);
            if (t != NULL) t->text = decode(dom, p, q, 1, &t->length);
            p = q;
            continue;
        }
        if (p + 3 < end && p[1] == '!' && p[2] == '-' && p[3] == '-') {
            for (q = p + 4; q + 2 < end && !(q[0] == '-' && q[1] == '-' && q[2] == '>'); q++) {}
            p = q + 2 < end ? q + 3 : end;
            continue;
        }
        if (p + 1 < end && (p[1] == '!' || p[1] == '?')) {
            q = memchr(p, '>', (size_t)(end - p));
            p = q != NULL ? q + 1 : end;
            continue;
        }
        if (p + 1 < end && p[1] == '/') {
            size_t n = 0;
            q = p + 2;
            while (q + n < end && is_name((unsigned char)q[n])) n++;
            if (n != 0U) cur = end_tag(cur, tag_find(q, n));
            q = memchr(q, '>', (size_t)(end - q));
            p = q != NULL ? q + 1 : end;
            continue;
        }
        if (p + 1 < end && ((p[1] >= 'a' && p[1] <= 'z') || (p[1] >= 'A' && p[1] <= 'Z'))) {
            const char *name = p + 1, *attrs;
            size_t n = 0;
            uint16_t tag;
            onewww_node_t *node;
            char quote = 0;

            while (name + n < end && is_name((unsigned char)name[n])) n++;
            tag = tag_find(name, n);
            attrs = name + n;
            for (q = attrs; q < end; q++) {
                if (quote != 0) { if (*q == quote) quote = 0; }
                else if (*q == '"' || *q == '\'') quote = *q;
                else if (*q == '>') break;
            }
            if (q >= end && quote != 0) {       /* an unclosed quote */
                q = memchr(attrs, '>', (size_t)(end - attrs));
                if (q == NULL) q = end;
            }
            p = q < end ? q + 1 : end;
            if (tag == OW_HTML || tag == OW_HEAD || tag == OW_BODY) continue;
            if (tag == OW_SCRIPT || tag == OW_STYLE) {
                q = find_close(p, end, s_names[tag]);
                q = memchr(q, '>', (size_t)(end - q));
                p = q != NULL ? q + 1 : end;
                continue;
            }
            cur = implied_close(cur, tag);
            node = node_new(dom, depth_of(cur) < MAX_DEPTH ? cur : cur->parent, tag);
            if (node == NULL) break;
            node->text = decode(dom, attrs, q, 0, &node->length);
            if (tag == OW_TITLE || tag == OW_TEXTAREA) {
                onewww_node_t *t = node_new(dom, node, OW_TEXT);
                q = find_close(p, end, s_names[tag]);
                if (t != NULL) t->text = decode(dom, p, q, 1, &t->length);
                if (tag == OW_TITLE && t != NULL && dom->title[0] == '\0') dom->title = t->text;
                q = memchr(q, '>', (size_t)(end - q));
                p = q != NULL ? q + 1 : end;
                continue;
            }
            if (!is_void(tag) && !(q > attrs && q[-1] == '/' && tag == OW_UNKNOWN) &&
                depth_of(cur) < MAX_DEPTH) cur = node;
            continue;
        }
        {   /* A lone '<' is text. */
            onewww_node_t *t = node_new(dom, cur, OW_TEXT);
            if (t != NULL) t->text = decode(dom, p, p + 1, 0, &t->length);
            p++;
        }
    }
    if (dom->failed) { onewww_dom_free(dom); return NULL; }
    return dom;
}

void onewww_dom_free(onewww_dom_t *dom)
{
    if (dom == NULL) return;
    while (dom->chunks != NULL) {
        chunk_t *next = dom->chunks->next;
        free(dom->chunks);
        dom->chunks = next;
    }
    free(dom);
}

onewww_node_t *onewww_dom_root(onewww_dom_t *dom) { return dom->root; }
const char *onewww_dom_title(const onewww_dom_t *dom) { return dom->title; }

int onewww_attr(const onewww_node_t *node, const char *name,
                char *out, size_t capacity)
{
    const char *p = node->text, *end = p + node->length;

    if (node->tag == OW_TEXT || capacity == 0U) return -1;
    while (p < end) {
        const char *key, *value, *value_end;
        size_t key_len = 0;

        while (p < end && (is_space((unsigned char)*p) || *p == '/')) p++;
        key = p;
        while (p < end && !is_space((unsigned char)*p) && *p != '=' && *p != '/') { p++; key_len++; }
        if (key_len == 0U) { if (p < end) p++; continue; }
        while (p < end && is_space((unsigned char)*p)) p++;
        value = value_end = p;
        if (p < end && *p == '=') {
            p++;
            while (p < end && is_space((unsigned char)*p)) p++;
            if (p < end && (*p == '"' || *p == '\'')) {
                const char quote = *p++;
                value = p;
                while (p < end && *p != quote) p++;
                value_end = p;
                if (p < end) p++;
            } else {
                value = p;
                while (p < end && !is_space((unsigned char)*p)) p++;
                value_end = p;
            }
        }
        if (same_name(key, key_len, name)) {
            size_t n = 0;
            while (value < value_end && n + 4U < capacity) {
                uint32_t code;
                size_t used;
                if (*value == '&' && (used = entity(value, value_end, &code)) != 0U) {
                    n += put_utf8(out + n, code);
                    value += used;
                } else out[n++] = *value++;
            }
            out[n] = '\0';
            return (int)n;
        }
    }
    return -1;
}
