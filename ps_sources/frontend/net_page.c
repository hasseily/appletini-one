#include "net_page.h"
#include <string.h>

#define ATTR_MAX 24
#define FRAGMENT_MAX 256
#define FORM_URL_MAX 232   /* The Apple URL buffer holds 255 characters. */
#define FORM_LABEL_MAX 60
#define NOTE "<p>[Page cut: too long]"
#define RULE_MAX 256
#define ELEMENT_DEPTH 64
#define ELEMENT_CLASSES 6

typedef struct { const uint8_t *name, *value; size_t name_length, value_length; } attr_t;

/* A style rule that can size an image, and an open element. Zero: any. */
typedef struct {
    uint32_t tag, id, cls[2], a_tag, a_id, a_cls;
    uint16_t size[4], weight;
    uint8_t mask;
} css_rule_t;
typedef struct { uint32_t tag, id, cls[ELEMENT_CLASSES]; } element_t;

typedef struct {
    const uint8_t *in;
    size_t length, at;
    uint8_t *out;
    size_t capacity, used;
    int cut, latin1;
    int space, line_start;      /* pending space; nothing on this line yet */
    int anchor_open;
    const char *base;
    uint32_t fragments[FRAGMENT_MAX];
    unsigned fragment_count;
    attr_t attrs[ATTR_MAX];
    unsigned attr_count;
    /* Form in progress. */
    int form, form_post, form_fields, form_typed, in_select, option_taken;
    char form_action[FORM_URL_MAX], form_body[FORM_URL_MAX], form_label[FORM_LABEL_MAX];
    char select_name[64], select_value[64];
    css_rule_t rules[RULE_MAX];
    unsigned rule_count, depth;
    element_t open[ELEMENT_DEPTH];
} page_t;

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int is_space(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
static int is_alnum(int c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/* Case-blind compare of a counted string with a lower-case C string. */
static int same(const uint8_t *p, size_t n, const char *word)
{
    size_t i;
    for (i = 0; i < n; ++i) if (!word[i] || lower(p[i]) != word[i]) return 0;
    return !word[n];
}
static int prefix(const uint8_t *p, size_t n, const char *word)
{
    size_t i;
    for (i = 0; word[i]; ++i) if (i >= n || lower(p[i]) != word[i]) return 0;
    return 1;
}
static int in_list(const uint8_t *p, size_t n, const char *const *list)
{
    for (; *list; ++list) if (same(p, n, *list)) return 1;
    return 0;
}
static const uint8_t *find_text(const uint8_t *p, size_t n, const char *word)
{
    size_t w = strlen(word), i;
    if (w > n) return NULL;
    for (i = 0; i + w <= n; ++i) if (prefix(p + i, n - i, word)) return p + i;
    return NULL;
}

int net_page_is_html(const char *content_type)
{
    size_t n = content_type ? strlen(content_type) : 0;
    return prefix((const uint8_t *)content_type, n, "text/html") ||
           prefix((const uint8_t *)content_type, n, "application/xhtml+xml");
}

/* ---- output ---- */
static void raw(page_t *p, const char *text, size_t n)
{
    if (p->cut) return;
    if (n > p->capacity - p->used) { p->cut = 1; return; }
    memcpy(p->out + p->used, text, n);
    p->used += n;
}
static void raw_text(page_t *p, const char *text) { raw(p, text, strlen(text)); }

/* One visible character, with space collapse and escapes. */
static void text_char(page_t *p, int c)
{
    if (c <= ' ') {
        if (!p->line_start) p->space = 1;
        return;
    }
    if (p->space) { raw(p, " ", 1); p->space = 0; }
    p->line_start = 0;
    if (c == '<') raw(p, "&lt;", 4);
    else if (c == '&') raw(p, "&amp;", 5);
    else {
        char one = (char)(c < 127 ? c : '?');
        raw(p, &one, 1);
    }
}
static void text_word(page_t *p, const char *text) { while (*text) text_char(p, (uint8_t)*text++); }

/* A block tag ends the line. Repeats of <br> add nothing on the Apple. */
static void block(page_t *p, const char *tag)
{
    if (p->line_start && !strcmp(tag, "<br>")) return;
    raw_text(p, tag);
    p->space = 0;
    p->line_start = 1;
}

/* ---- characters ---- */
static const char *translit(uint32_t c)
{
    static const char *const latin[96] = {
        " ", "!", "c", "GBP", "$", "Y", "|", "S", "\"", "(c)", "a", "<<", "-", "", "(R)", "-",
        "deg", "+/-", "2", "3", "'", "u", "P", ".", ",", "1", "o", ">>", "1/4", "1/2", "3/4", "?",
        "A", "A", "A", "A", "A", "A", "AE", "C", "E", "E", "E", "E", "I", "I", "I", "I",
        "D", "N", "O", "O", "O", "O", "O", "x", "O", "U", "U", "U", "U", "Y", "Th", "ss",
        "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
        "d", "n", "o", "o", "o", "o", "o", "/", "o", "u", "u", "u", "u", "y", "th", "y"};
    if (c >= 0xA0 && c <= 0xFF) return latin[c - 0xA0];
    switch (c) {
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2212: return "-";
    case 0x2014: case 0x2015: return "--";
    case 0x2018: case 0x2019: case 0x201A: case 0x2032: return "'";
    case 0x201C: case 0x201D: case 0x201E: case 0x2033: return "\"";
    case 0x2022: case 0x00B7: case 0x25CF: case 0x25A0: return "*";
    case 0x2026: return "...";
    case 0x2039: return "<";
    case 0x203A: return ">";
    case 0x20AC: return "EUR";
    case 0x2122: return "(TM)";
    case 0x2190: return "<-";
    case 0x2192: return "->";
    case 0x2191: return "^";
    case 0x2193: return "v";
    case 0x00D7: return "x";
    case 0x2002: case 0x2003: case 0x2009: case 0x200A: case 0x202F: case 0x3000: return " ";
    case 0x200B: case 0x200C: case 0x200D: case 0x2060: case 0xFEFF: case 0x00AD: return "";
    case 0x0152: return "OE";
    case 0x0153: return "oe";
    case 0x2713: case 0x2714: return "v";
    case 0x2717: case 0x2718: return "x";
    default: return "?";
    }
}
static void code_point(page_t *p, uint32_t c)
{
    if (c < 127) text_char(p, (int)c);
    else if (c >= 0x80 && c < 0xA0) text_char(p, '?');
    else {
        const char *s = translit(c);
        if (s[0] == ' ' && !s[1]) text_char(p, ' ');
        else text_word(p, s);
    }
}
/* Next code point of the input text, per the page charset. */
static uint32_t next_char(page_t *p)
{
    uint32_t c = p->in[p->at++];
    unsigned more;
    if (c < 0x80 || p->latin1) return c;
    if ((c & 0xE0) == 0xC0) { c &= 0x1F; more = 1; }
    else if ((c & 0xF0) == 0xE0) { c &= 0x0F; more = 2; }
    else if ((c & 0xF8) == 0xF0) { c &= 0x07; more = 3; }
    else return 0xFFFD;
    while (more--) {
        if (p->at >= p->length || (p->in[p->at] & 0xC0) != 0x80) return 0xFFFD;
        c = (c << 6) | (p->in[p->at++] & 0x3F);
    }
    return c;
}

/* Decode the entity that starts after '&' at s[0..n). Returns the bytes
 * used (0: not an entity) and the code point. */
static size_t entity(const uint8_t *s, size_t n, uint32_t *value)
{
    static const struct { const char *name; uint16_t code; } names[] = {
        {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", 0xA0},
        {"copy", 0xA9}, {"reg", 0xAE}, {"trade", 0x2122}, {"mdash", 0x2014}, {"ndash", 0x2013},
        {"hellip", 0x2026}, {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"ldquo", 0x201C},
        {"rdquo", 0x201D}, {"laquo", 0xAB}, {"raquo", 0xBB}, {"bull", 0x2022}, {"middot", 0xB7},
        {"euro", 0x20AC}, {"pound", 0xA3}, {"times", 0xD7}, {"rarr", 0x2192}, {"larr", 0x2190},
        {"deg", 0xB0}, {"szlig", 0xDF}, {"shy", 0xAD}, {"ensp", 0x2002}, {"emsp", 0x2003},
        {"thinsp", 0x2009}, {"zwj", 0x200D}, {"zwnj", 0x200C}, {"sect", 0xA7}, {"para", 0xB6},
        {"cent", 0xA2}, {"yen", 0xA5}, {"plusmn", 0xB1}, {"frac12", 0xBD}, {"iexcl", 0xA1},
        {"iquest", 0xBF}, {"aelig", 0xE6}, {"AElig", 0xC6}, {"oslash", 0xF8}, {"Oslash", 0xD8},
        {"aring", 0xE5}, {"Aring", 0xC5}, {"ccedil", 0xE7}, {"Ccedil", 0xC7},
        {"ntilde", 0xF1}, {"Ntilde", 0xD1}};
    static const char *const marks[] = {"acute", "grave", "circ", "uml", "tilde", NULL};
    size_t i = 0, k;
    uint32_t c = 0;
    if (n && s[0] == '#') {
        int hex = n > 1 && (s[1] == 'x' || s[1] == 'X');
        i = hex ? 2 : 1;
        k = i;
        for (; i < n && i < 10; ++i) {
            int d = lower(s[i]);
            if (d >= '0' && d <= '9') d -= '0';
            else if (hex && d >= 'a' && d <= 'f') d -= 'a' - 10;
            else break;
            c = c * (hex ? 16U : 10U) + (uint32_t)d;
        }
        if (i == k) return 0;
        if (i < n && s[i] == ';') ++i;
        *value = c ? c : 0xFFFD;
        return i;
    }
    while (i < n && i < 10 && is_alnum(s[i])) ++i;
    if (!i) return 0;
    for (k = 0; k < sizeof(names) / sizeof(names[0]); ++k)
        if (strlen(names[k].name) == i && !memcmp(s, names[k].name, i)) {
            *value = names[k].code;
            return i + (i < n && s[i] == ';');
        }
    /* eacute, Ograve, uuml ...: the letter itself is the ASCII form. */
    for (k = 0; marks[k]; ++k)
        if (i == 1 + strlen(marks[k]) && !memcmp(s + 1, marks[k], i - 1)) {
            *value = s[0];
            return i + (i < n && s[i] == ';');
        }
    if (i < n && s[i] == ';') { *value = 0xFFFD; return i + 1; }
    return 0;
}

/* ---- attributes ---- */
static const attr_t *attr(const page_t *p, const char *name)
{
    unsigned i;
    for (i = 0; i < p->attr_count; ++i)
        if (same(p->attrs[i].name, p->attrs[i].name_length, name)) return &p->attrs[i];
    return NULL;
}
/* Copy an attribute value as a URL or field text: entities decoded, spaces
 * at the ends removed, 7-bit only. Returns the length; 0 when too long. */
static size_t value_text(const attr_t *a, char *out, size_t capacity)
{
    size_t i = 0, n = 0, end;
    if (!a || !capacity) return 0;
    end = a->value_length;
    while (i < end && is_space(a->value[i])) ++i;
    while (end > i && is_space(a->value[end - 1])) --end;
    while (i < end) {
        uint32_t c = a->value[i++];
        if (c == '&') {
            uint32_t decoded;
            size_t used = entity(a->value + i, end - i, &decoded);
            if (used) { c = decoded; i += used; }
        }
        if (c < ' ' || c > 126) c = c == '\t' || c == '\n' || c == '\r' ? ' ' : '?';
        if (n + 1 >= capacity) { out[0] = 0; return 0; }
        out[n++] = (char)c;
    }
    out[n] = 0;
    return n;
}
/* Append to a C string inside a fixed buffer. 0 when it does not fit. */
static int add(char *buffer, size_t capacity, const char *text)
{
    size_t have = strlen(buffer), n = strlen(text);
    if (have + n >= capacity) return 0;
    memcpy(buffer + have, text, n + 1);
    return 1;
}
static int add_encoded(char *buffer, size_t capacity, const char *text)
{
    static const char hex[] = "0123456789ABCDEF";
    char piece[4];
    for (; *text; ++text) {
        uint8_t c = (uint8_t)*text;
        if (is_alnum(c) || c == '-' || c == '_' || c == '.' || c == '~') { piece[0] = (char)c; piece[1] = 0; }
        else if (c == ' ') { piece[0] = '+'; piece[1] = 0; }
        else { piece[0] = '%'; piece[1] = hex[c >> 4]; piece[2] = hex[c & 15]; piece[3] = 0; }
        if (!add(buffer, capacity, piece)) return 0;
    }
    return 1;
}
/* Write a quoted attribute value. */
static void quoted(page_t *p, const char *value)
{
    raw(p, "\"", 1);
    for (; *value; ++value) {
        if (*value == '"') raw(p, "%22", 3);
        else if (*value == '<') raw(p, "%3C", 3);
        else if (*value == '>') raw(p, "%3E", 3);
        else raw(p, value, 1);
    }
    raw(p, "\"", 1);
}

static uint32_t hash_bytes(const uint8_t *s, size_t n)
{
    uint32_t h = 2166136261U;
    while (n--) { h ^= *s++; h *= 16777619U; }
    return h ? h : 1U;
}
/* Collect the targets of href="#name" links, so that only those ids stay. */
static void find_fragments(page_t *p)
{
    size_t i = 0;
    while (i + 6 < p->length && p->fragment_count < FRAGMENT_MAX) {
        const uint8_t *s;
        size_t start;
        if (!prefix(p->in + i, p->length - i, "href=")) { ++i; continue; }
        i += 5;
        if (i < p->length && (p->in[i] == '"' || p->in[i] == '\'')) ++i;
        if (i >= p->length || p->in[i] != '#') continue;
        start = ++i;
        s = p->in;
        while (i < p->length && s[i] != '"' && s[i] != '\'' && s[i] != '>' && !is_space(s[i])) ++i;
        if (i > start) p->fragments[p->fragment_count++] = hash_bytes(s + start, i - start);
    }
}
static int is_fragment(const page_t *p, const char *name)
{
    uint32_t h = hash_bytes((const uint8_t *)name, strlen(name));
    unsigned i;
    for (i = 0; i < p->fragment_count; ++i) if (p->fragments[i] == h) return 1;
    return 0;
}

/* ---- URLs ---- */
/* Absolute URL of a form action. No dot-segment work: the server or the
 * Apple resolver does that. 0 when the result does not fit. */
static int absolute(const char *base, const char *ref, char *out, size_t capacity)
{
    const char *scheme_end = strstr(base, "://"), *path, *stop;
    size_t n;
    out[0] = 0;
    if (strstr(ref, "://")) return add(out, capacity, ref);
    if (!scheme_end) return 0;
    path = strchr(scheme_end + 3, '/');
    stop = base + strcspn(base, "?#");
    if (ref[0] == '/' && ref[1] == '/') n = (size_t)(scheme_end + 1 - base);
    else if (ref[0] == '/') n = path && path < stop ? (size_t)(path - base) : (size_t)(stop - base);
    else if (!ref[0] || ref[0] == '?' || ref[0] == '#') n = (size_t)(stop - base);
    else {
        const char *slash = stop;
        while (slash > scheme_end + 3 && slash[-1] != '/') --slash;
        if (slash == scheme_end + 3) {
            /* The base has no path: host + "/" + ref. */
            if ((size_t)(stop - base) + 1 >= capacity) return 0;
            memcpy(out, base, (size_t)(stop - base));
            out[stop - base] = '/';
            out[stop - base + 1] = 0;
            return add(out, capacity, ref);
        }
        n = (size_t)(slash - base);
    }
    if (n >= capacity) return 0;
    memcpy(out, base, n);
    out[n] = 0;
    return ref[0] == '#' ? 1 : add(out, capacity, ref);
}

/* ---- forms ---- */
static void form_field(page_t *p, const char *name, const char *value, int typed)
{
    if (!p->form || !name[0]) return;
    if (p->form_body[0] && !add(p->form_body, sizeof(p->form_body), "&")) { p->form = -1; return; }
    if (!add_encoded(p->form_body, sizeof(p->form_body), name) ||
        !add(p->form_body, sizeof(p->form_body), "=") ||
        !(typed ? add(p->form_body, sizeof(p->form_body), "%s")
                : add_encoded(p->form_body, sizeof(p->form_body), value))) { p->form = -1; return; }
    ++p->form_fields;
    if (typed) {
        ++p->form_typed;
        if (p->form_label[0]) (void)add(p->form_label, sizeof(p->form_label), ", ");
        (void)add(p->form_label, sizeof(p->form_label), name);
    }
}
static void form_end(page_t *p)
{
    char url[FORM_URL_MAX * 2 + 8];
    int form = p->form;
    p->form = 0;
    p->in_select = 0;
    if (form <= 0 || !p->form_fields) return;
    url[0] = 0;
    if (p->form_post) {
        if (!add(url, sizeof(url), "post:") || !add(url, sizeof(url), p->form_action) ||
            !add(url, sizeof(url), " ") || !add(url, sizeof(url), p->form_body)) return;
    } else {
        p->form_action[strcspn(p->form_action, "?#")] = 0;
        if (!add(url, sizeof(url), p->form_action) || !add(url, sizeof(url), "?") ||
            !add(url, sizeof(url), p->form_body)) return;
    }
    if (strlen(url) > FORM_URL_MAX) return;
    if (p->anchor_open) { raw_text(p, "</a>"); p->anchor_open = 0; }
    block(p, "<br>");
    raw_text(p, "<a href=");
    quoted(p, url);
    raw_text(p, ">");
    p->line_start = 0;
    if (p->form_typed) {
        text_word(p, "[Form: ");
        text_word(p, p->form_label);
        text_word(p, "]");
    } else text_word(p, "[Send form]");
    raw_text(p, "</a>");
    block(p, "<br>");
}
static void form_begin(page_t *p)
{
    char action[FORM_URL_MAX];
    const attr_t *method = attr(p, "method");
    form_end(p);
    (void)value_text(attr(p, "action"), action, sizeof(action));
    p->form_post = method && same(method->value, method->value_length, "post");
    p->form_fields = p->form_typed = 0;
    p->form_body[0] = p->form_label[0] = 0;
    p->form = absolute(p->base, action, p->form_action, sizeof(p->form_action)) ? 1 : -1;
}
static void form_input(page_t *p)
{
    static const char *const typed[] = {"text", "search", "email", "url", "tel", "number", "password", NULL};
    char name[64], value[128];
    const attr_t *type = attr(p, "type");
    if (!p->form) return;
    (void)value_text(attr(p, "name"), name, sizeof(name));
    (void)value_text(attr(p, "value"), value, sizeof(value));
    if (!type || in_list(type->value, type->value_length, typed)) form_field(p, name, value, 1);
    else if (same(type->value, type->value_length, "hidden")) form_field(p, name, value, 0);
    else if ((same(type->value, type->value_length, "checkbox") ||
              same(type->value, type->value_length, "radio")) && attr(p, "checked"))
        form_field(p, name, attr(p, "value") ? value : "on", 0);
}

/* ---- tokenizer ---- */
/* Parse the tag at in[at] (just after '<' and an optional '/'). Returns the
 * name; fills attrs; leaves at after '>'. */
static size_t parse_tag(page_t *p, const uint8_t **name)
{
    size_t start = p->at, n;
    while (p->at < p->length && !is_space(p->in[p->at]) && p->in[p->at] != '>' && p->in[p->at] != '/') ++p->at;
    *name = p->in + start;
    n = p->at - start;
    p->attr_count = 0;
    for (;;) {
        attr_t a;
        while (p->at < p->length && (is_space(p->in[p->at]) || p->in[p->at] == '/')) ++p->at;
        if (p->at >= p->length) break;
        if (p->in[p->at] == '>') { ++p->at; break; }
        a.name = p->in + p->at;
        while (p->at < p->length && !is_space(p->in[p->at]) && p->in[p->at] != '=' &&
               p->in[p->at] != '>' && p->in[p->at] != '/') ++p->at;
        a.name_length = (size_t)(p->in + p->at - a.name);
        if (!a.name_length) { ++p->at; continue; } /* stray '=' */
        a.value = p->in + p->at;
        a.value_length = 0;
        while (p->at < p->length && is_space(p->in[p->at])) ++p->at;
        if (p->at < p->length && p->in[p->at] == '=') {
            ++p->at;
            while (p->at < p->length && is_space(p->in[p->at])) ++p->at;
            if (p->at < p->length && (p->in[p->at] == '"' || p->in[p->at] == '\'')) {
                uint8_t quote = p->in[p->at++];
                a.value = p->in + p->at;
                while (p->at < p->length && p->in[p->at] != quote) ++p->at;
                a.value_length = (size_t)(p->in + p->at - a.value);
                if (p->at < p->length) ++p->at;
            } else {
                a.value = p->in + p->at;
                while (p->at < p->length && !is_space(p->in[p->at]) && p->in[p->at] != '>') ++p->at;
                a.value_length = (size_t)(p->in + p->at - a.value);
            }
        }
        if (p->attr_count < ATTR_MAX) p->attrs[p->attr_count++] = a;
    }
    return n;
}
/* Skip to after the end tag </name ...>. Raw text: no nesting. */
static void skip_raw(page_t *p, const uint8_t *name, size_t n)
{
    while (p->at + 2 + n <= p->length) {
        if (p->in[p->at] == '<' && p->in[p->at + 1] == '/' && p->at + 2 + n <= p->length) {
            size_t i;
            for (i = 0; i < n && lower(p->in[p->at + 2 + i]) == lower(name[i]); ++i) {}
            if (i == n && (p->at + 2 + n == p->length || !is_alnum(p->in[p->at + 2 + n]))) {
                while (p->at < p->length && p->in[p->at] != '>') ++p->at;
                if (p->at < p->length) ++p->at;
                return;
            }
        }
        ++p->at;
    }
    p->at = p->length;
}
/* Skip an element and all that it holds, with nested elements of the same
 * name. Scripts inside are raw text. */
static void skip_element(page_t *p, const uint8_t *name, size_t n)
{
    unsigned depth = 1;
    while (p->at < p->length && depth) {
        const uint8_t *found;
        size_t found_length;
        int closing;
        if (p->in[p->at] != '<') { ++p->at; continue; }
        if (p->at + 3 < p->length && !memcmp(p->in + p->at, "<!--", 4)) {
            const uint8_t *end = find_text(p->in + p->at + 4, p->length - p->at - 4, "-->");
            p->at = end ? (size_t)(end + 3 - p->in) : p->length;
            continue;
        }
        closing = p->at + 1 < p->length && p->in[p->at + 1] == '/';
        if (p->at + 1 + (size_t)closing >= p->length || !is_alnum(p->in[p->at + 1 + closing]) ) { ++p->at; continue; }
        p->at += 1 + (size_t)closing;
        found_length = parse_tag(p, &found);
        if (!closing && (same(found, found_length, "script") || same(found, found_length, "style")))
            skip_raw(p, found, found_length);
        else if (found_length == n) {
            size_t i;
            for (i = 0; i < n && lower(found[i]) == lower(name[i]); ++i) {}
            if (i == n) depth += closing ? -1 : 1;
        }
    }
}

static int hidden(const page_t *p)
{
    const attr_t *style = attr(p, "style"), *aria = attr(p, "aria-hidden");
    if (attr(p, "hidden")) return 1;
    if (aria && same(aria->value, aria->value_length, "true")) return 1;
    if (style) {
        const uint8_t *found = find_text(style->value, style->value_length, "display");
        if (found) {
            size_t rest = style->value_length - (size_t)(found - style->value);
            found += 7; rest -= 7;
            while (rest && (is_space(*found) || *found == ':')) { ++found; --rest; }
            if (prefix(found, rest, "none")) return 1;
        }
    }
    return 0;
}

/* The URL of an image: src, a lazy-load attribute, or the first srcset URL. */
static size_t image_url(const page_t *p, char *out, size_t capacity)
{
    static const char *const names[] = {"src", "data-src", "data-lazy-src", "data-original", NULL};
    const attr_t *set;
    unsigned i;
    size_t n;
    for (i = 0; names[i]; ++i) {
        const attr_t *a = attr(p, names[i]);
        if (!a || prefix(a->value, a->value_length, "data:")) continue;
        n = value_text(a, out, capacity);
        if (n) return n;
    }
    set = attr(p, "srcset");
    if (!set) set = attr(p, "data-srcset");
    if (set) {
        attr_t first = *set;
        size_t k = 0;
        while (k < first.value_length && is_space(first.value[k])) ++k;
        first.value += k;
        first.value_length -= k;
        for (k = 0; k < first.value_length && !is_space(first.value[k]) && first.value[k] != ','; ++k) {}
        first.value_length = k;
        if (!prefix(first.value, first.value_length, "data:")) return value_text(&first, out, capacity);
    }
    out[0] = 0;
    return 0;
}
/* A width or height attribute in pixels; 0 when absent or not in pixels. */
static unsigned pixel_attr(const page_t *p, const char *name)
{
    const attr_t *a = attr(p, name);
    unsigned value = 0;
    size_t i, n;
    if (!a) return 0;
    n = a->value_length;
    if (n > 2 && lower(a->value[n - 2]) == 'p' && lower(a->value[n - 1]) == 'x') n -= 2;
    if (!n || n > 4 || a->value[0] == '0') return 0;
    for (i = 0; i < n; ++i) {
        if (a->value[i] < '0' || a->value[i] > '9') return 0;
        value = value * 10U + (unsigned)(a->value[i] - '0');
    }
    return value;
}

/* ---- CSS image sizes ----
 * Only what gives an image its size: width, height, max-width and
 * max-height, from rules whose last selector part can match an <img> by
 * tag, class or id, with at most one ancestor part (".hero img"). */
enum { CSS_W, CSS_H, CSS_MAXW, CSS_MAXH, CSS_PROPS };

static uint32_t hash_lower(const uint8_t *s, size_t n)
{
    uint32_t h = 2166136261U;
    while (n--) { h ^= (uint32_t)lower(*s++); h *= 16777619U; }
    return h ? h : 1U;
}
static int is_ident(int c) { return is_alnum(c) || c == '-' || c == '_'; }

/* A CSS length in screen pixels; 0 when it is not usable. */
static unsigned css_length(const uint8_t *s, size_t n, int horizontal)
{
    unsigned long whole = 0, fraction = 0, scale = 1, value;
    size_t i = 0;
    while (i < n && is_space(s[i])) ++i;
    if (i >= n || ((s[i] < '0' || s[i] > '9') && s[i] != '.')) return 0;
    for (; i < n && s[i] >= '0' && s[i] <= '9'; ++i) if (whole < 100000UL) whole = whole * 10UL + (unsigned long)(s[i] - '0');
    if (i < n && s[i] == '.')
        for (++i; i < n && s[i] >= '0' && s[i] <= '9'; ++i)
            if (scale < 1000UL) { fraction = fraction * 10UL + (unsigned long)(s[i] - '0'); scale *= 10UL; }
    value = whole * 1000UL + fraction * 1000UL / scale;       /* thousandths */
    if (prefix(s + i, n - i, "px") || i == n || is_space(s[i]) || s[i] == '!') value /= 1000UL;
    else if (prefix(s + i, n - i, "rem") || prefix(s + i, n - i, "em")) value = value * 16UL / 1000UL;
    else if (horizontal && (s[i] == '%' || prefix(s + i, n - i, "vw"))) value = value * 546UL / 100000UL;
    else return 0;
    return value && value <= 9999UL ? (unsigned)value : 0U;
}
/* Read the size properties of a declaration block. Returns a bit mask. */
static unsigned css_declarations(const uint8_t *s, size_t n, uint16_t out[CSS_PROPS])
{
    static const char *const names[CSS_PROPS] = {"width", "height", "max-width", "max-height"};
    unsigned found = 0;
    size_t i = 0;
    while (i < n) {
        size_t name, name_end, value;
        unsigned k;
        while (i < n && (is_space(s[i]) || s[i] == ';')) ++i;
        name = i;
        while (i < n && s[i] != ':' && s[i] != ';') ++i;
        if (i >= n || s[i] != ':') continue;
        name_end = i;
        while (name_end > name && is_space(s[name_end - 1])) --name_end;
        value = ++i;
        while (i < n && s[i] != ';') ++i;
        for (k = 0; k < CSS_PROPS; ++k)
            if (same(s + name, name_end - name, names[k])) {
                unsigned pixels = css_length(s + value, i - value, k == CSS_W || k == CSS_MAXW);
                if (pixels) { out[k] = (uint16_t)pixels; found |= 1U << k; }
            }
    }
    return found;
}
/* One selector part: tag, #id and up to two classes. 0: not supported. */
static int css_compound(const uint8_t *s, size_t n, uint32_t *tag, uint32_t *id, uint32_t cls[2], unsigned *weight)
{
    size_t i = 0, start;
    unsigned classes = 0;
    *tag = *id = cls[0] = cls[1] = 0;
    start = i;
    while (i < n && is_ident(s[i])) ++i;
    if (i > start) { *tag = hash_lower(s + start, i - start); *weight += 1; }
    while (i < n) {
        uint8_t kind = s[i++];
        if (kind != '.' && kind != '#') return 0;   /* pseudo-class, attribute, '*' */
        start = i;
        while (i < n && is_ident(s[i])) ++i;
        if (i == start) return 0;
        if (kind == '#') { *id = hash_bytes(s + start, i - start); *weight += 100; }
        else {
            if (classes < 2) cls[classes] = hash_bytes(s + start, i - start);
            ++classes;
            *weight += 10;
        }
    }
    return classes <= 2;
}
static void css_rule(page_t *p, const uint8_t *selector, size_t n, const uint8_t *body, size_t body_length)
{
    uint16_t sizes[CSS_PROPS] = {0, 0, 0, 0};
    unsigned mask = css_declarations(body, body_length, sizes);
    size_t at = 0;
    if (!mask) return;
    while (at < n && p->rule_count < RULE_MAX) {
        css_rule_t rule;
        size_t end = at, last, before_end, before;
        unsigned weight = 0;
        while (end < n && selector[end] != ',') ++end;
        last = end;
        while (last > at && is_space(selector[last - 1])) --last;
        { /* trim, then split off the last part */
            size_t first = at, part = last;
            while (first < last && is_space(selector[first])) ++first;
            while (part > first && !is_space(selector[part - 1]) && selector[part - 1] != '>') --part;
            memset(&rule, 0, sizeof(rule));
            if (last > part && css_compound(selector + part, last - part, &rule.tag, &rule.id, rule.cls, &weight) &&
                (!rule.tag || rule.tag == hash_lower((const uint8_t *)"img", 3))) {
                int usable = 1;
                before_end = part;
                while (before_end > first && (is_space(selector[before_end - 1]) || selector[before_end - 1] == '>')) --before_end;
                before = before_end;
                while (before > first && !is_space(selector[before - 1]) && selector[before - 1] != '>') --before;
                if (before_end > before) {
                    uint32_t ancestor_cls[2];
                    usable = css_compound(selector + before, before_end - before, &rule.a_tag, &rule.a_id,
                                          ancestor_cls, &weight);
                    rule.a_cls = ancestor_cls[0];
                    if (ancestor_cls[1]) usable = 0;
                }
                /* A bare "img" rule or one with an id or class; never a rule
                 * for all elements. */
                if (usable && (rule.tag || rule.id || rule.cls[0])) {
                    memcpy(rule.size, sizes, sizeof(sizes));
                    rule.mask = (uint8_t)mask;
                    rule.weight = (uint16_t)weight;
                    p->rules[p->rule_count++] = rule;
                }
            }
        }
        at = end + 1;
    }
}
/* "(max-width: 600px)" and "(min-width: ...)" against a 546-pixel screen.
 * A query with neither applies (for example "screen"). "print" does not. */
static int css_media(const uint8_t *s, size_t n)
{
    const uint8_t *found;
    if (find_text(s, n, "print") && !find_text(s, n, "screen")) return 0;
    found = find_text(s, n, "max-width");
    if (found) {
        size_t rest = n - (size_t)(found - s) - 9;
        found += 9;
        while (rest && (is_space(*found) || *found == ':')) { ++found; --rest; }
        if (css_length(found, rest, 0) && css_length(found, rest, 0) < 546U) return 0;
    }
    found = find_text(s, n, "min-width");
    if (found) {
        size_t rest = n - (size_t)(found - s) - 9;
        found += 9;
        while (rest && (is_space(*found) || *found == ':')) { ++found; --rest; }
        if (css_length(found, rest, 0) > 546U) return 0;
    }
    return 1;
}
static void css_parse(page_t *p, const uint8_t *s, size_t n, unsigned depth)
{
    size_t i = 0;
    while (i < n) {
        size_t start, open, close;
        unsigned level;
        while (i < n && (is_space(s[i]) || s[i] == '}' || s[i] == ';')) ++i;
        if (i + 1 < n && s[i] == '/' && s[i + 1] == '*') {
            const uint8_t *end = find_text(s + i + 2, n - i - 2, "*/");
            i = end ? (size_t)(end + 2 - s) : n;
            continue;
        }
        start = i;
        while (i < n && s[i] != '{' && s[i] != ';') ++i;
        if (i >= n) break;
        if (s[i] == ';') { ++i; continue; }           /* @import, @charset */
        open = i++;
        for (level = 1; i < n && level; ++i) {
            if (s[i] == '{') ++level;
            else if (s[i] == '}') --level;
        }
        close = level ? n : i - 1;
        if (s[start] == '@') {
            if (prefix(s + start, open - start, "@media") && depth < 3 &&
                css_media(s + start + 6, open - start - 6))
                css_parse(p, s + open + 1, close - open - 1, depth + 1);
        } else css_rule(p, s + start, open - start, s + open + 1, close - open - 1);
    }
}
/* Read all <style> blocks of the page before the first tag is filtered. */
static void find_styles(page_t *p)
{
    size_t i = 0;
    while (i + 7 < p->length) {
        const uint8_t *end;
        size_t start;
        if (p->in[i] != '<' || !prefix(p->in + i + 1, p->length - i - 1, "style") ||
            is_alnum(p->in[i + 6])) { ++i; continue; }
        while (i < p->length && p->in[i] != '>') ++i;
        start = i < p->length ? i + 1 : i;
        end = find_text(p->in + start, p->length - start, "</style");
        if (!end) break;
        css_parse(p, p->in + start, (size_t)(end - (p->in + start)), 0);
        i = (size_t)(end - p->in) + 7;
    }
}

/* Open elements, for the ancestor part of a selector. */
static void element_fill(const page_t *p, const uint8_t *name, size_t n, element_t *e)
{
    const attr_t *id = attr(p, "id"), *classes = attr(p, "class");
    unsigned count = 0;
    memset(e, 0, sizeof(*e));
    e->tag = hash_lower(name, n);
    if (id && id->value_length) e->id = hash_bytes(id->value, id->value_length);
    if (classes) {
        size_t i = 0;
        while (i < classes->value_length && count < ELEMENT_CLASSES) {
            size_t start;
            while (i < classes->value_length && is_space(classes->value[i])) ++i;
            start = i;
            while (i < classes->value_length && !is_space(classes->value[i])) ++i;
            if (i > start) e->cls[count++] = hash_bytes(classes->value + start, i - start);
        }
    }
}
static int element_has(const element_t *e, uint32_t cls)
{
    unsigned i;
    for (i = 0; i < ELEMENT_CLASSES; ++i) if (e->cls[i] == cls) return 1;
    return 0;
}
static void element_push(page_t *p, const uint8_t *name, size_t n)
{
    static const char *const empty[] = {"img", "br", "hr", "input", "meta", "link", "area", "base",
                                        "col", "embed", "source", "track", "wbr", NULL};
    if (in_list(name, n, empty) || p->depth >= ELEMENT_DEPTH) return;
    element_fill(p, name, n, &p->open[p->depth++]);
}
static void element_pop(page_t *p, const uint8_t *name, size_t n)
{
    uint32_t tag = hash_lower(name, n);
    unsigned i = p->depth;
    while (i && p->open[i - 1].tag != tag) --i;
    if (i) p->depth = i - 1;
}
/* The display size of the current <img>: HTML attributes, then the style
 * rules by weight and order, then its own style attribute. */
static void image_size(page_t *p, unsigned *w, unsigned *h)
{
    element_t self;
    uint16_t size[CSS_PROPS] = {0, 0, 0, 0}, weight[CSS_PROPS] = {0, 0, 0, 0}, own[CSS_PROPS] = {0, 0, 0, 0};
    const attr_t *style = attr(p, "style");
    unsigned r, k, mask;
    element_fill(p, (const uint8_t *)"img", 3, &self);
    size[CSS_W] = (uint16_t)pixel_attr(p, "width");
    size[CSS_H] = (uint16_t)pixel_attr(p, "height");
    for (r = 0; r < p->rule_count; ++r) {
        const css_rule_t *rule = &p->rules[r];
        if ((rule->id && rule->id != self.id) || (rule->cls[0] && !element_has(&self, rule->cls[0])) ||
            (rule->cls[1] && !element_has(&self, rule->cls[1]))) continue;
        if (rule->a_tag || rule->a_id || rule->a_cls) {
            unsigned d = p->depth;
            for (; d; --d) {
                const element_t *e = &p->open[d - 1];
                if ((!rule->a_tag || rule->a_tag == e->tag) && (!rule->a_id || rule->a_id == e->id) &&
                    (!rule->a_cls || element_has(e, rule->a_cls))) break;
            }
            if (!d) continue;
        }
        for (k = 0; k < CSS_PROPS; ++k)
            if ((rule->mask & (1U << k)) && rule->weight >= weight[k]) {
                size[k] = rule->size[k];
                weight[k] = rule->weight;
            }
    }
    mask = style ? css_declarations(style->value, style->value_length, own) : 0;
    for (k = 0; k < CSS_PROPS; ++k) if (mask & (1U << k)) size[k] = own[k];
    *w = size[CSS_W];
    *h = size[CSS_H];
    if (size[CSS_MAXW] && (!*w || *w > size[CSS_MAXW])) *w = size[CSS_MAXW];
    if (size[CSS_MAXH] && (!*h || *h > size[CSS_MAXH])) *h = size[CSS_MAXH];
}
static void number_attr(page_t *p, const char *text, unsigned value)
{
    char digits[8];
    unsigned n = sizeof(digits);
    if (!value) return;
    digits[--n] = 0;
    while (value) { digits[--n] = (char)('0' + value % 10U); value /= 10U; }
    raw_text(p, text);
    raw_text(p, digits + n);
}

int net_page_stylesheet(const uint8_t *in, size_t length, const char *base_url, unsigned index,
                        char *out, size_t capacity)
{
    static page_t scan;
    char href[256];
    page_t *p = &scan;
    memset(p, 0, sizeof(*p));
    p->in = in;
    p->length = length;
    while (p->at + 5 < p->length) {
        const uint8_t *name;
        const attr_t *rel, *media;
        if (p->in[p->at] != '<' || !prefix(p->in + p->at + 1, p->length - p->at - 1, "link") ||
            is_alnum(p->in[p->at + 5])) { ++p->at; continue; }
        ++p->at;
        (void)parse_tag(p, &name);
        rel = attr(p, "rel");
        media = attr(p, "media");
        if (!rel || !find_text(rel->value, rel->value_length, "stylesheet") ||
            (media && find_text(media->value, media->value_length, "print")) ||
            !value_text(attr(p, "href"), href, sizeof(href)) ||
            !absolute(base_url ? base_url : "", href, out, capacity)) continue;
        if (!index--) return 1;
    }
    return 0;
}
static int tiny(const page_t *p, const char *name)
{
    const attr_t *a = attr(p, name);
    return a && a->value_length == 1 && a->value[0] >= '0' && a->value[0] <= '3';
}

static void start_tag(page_t *p, const uint8_t *name, size_t n)
{
    static const char *const dropped[] = {"svg", "template", "iframe", "object", "canvas", "video",
                                          "audio", "map", "datalist", "dialog", NULL};
    static const char *const raw_text_tags[] = {"script", "style", "title", "textarea", NULL};
    static const char *const hideable[] = {"div", "span", "section", "nav", "a", "ul", "ol", "table",
                                           "form", "button", "label", "aside", "header", "footer",
                                           "details", "figure", NULL};
    static const char *const lines[] = {"div", "tr", "ul", "ol", "table", "dl", "dt", "dd", "header",
                                        "footer", "nav", "main", "section", "article", "aside",
                                        "address", "figure", "figcaption", "pre", "details",
                                        "summary", "hr", "br", "center", "fieldset", "caption", NULL};
    char value[256], id[64];
    unsigned width, height;

    if (in_list(name, n, raw_text_tags)) {
        if (same(name, n, "textarea")) {
            (void)value_text(attr(p, "name"), id, sizeof(id));
            form_field(p, id, "", 1);
        }
        skip_raw(p, name, n);
        return;
    }
    if (in_list(name, n, dropped) || (in_list(name, n, hideable) && hidden(p))) {
        skip_element(p, name, n);
        return;
    }
    element_push(p, name, n);
    if (p->in_select) {
        if (same(name, n, "option") && (!p->option_taken || attr(p, "selected"))) {
            (void)value_text(attr(p, "value"), p->select_value, sizeof(p->select_value));
            p->option_taken = 1;
        }
        return;
    }
    /* Keep the targets of same-page links. */
    if ((value_text(attr(p, "id"), id, sizeof(id)) && is_fragment(p, id)) ||
        (same(name, n, "a") && value_text(attr(p, "name"), id, sizeof(id)) && is_fragment(p, id))) {
        raw_text(p, "<b id=");
        quoted(p, id);
        raw_text(p, ">");
    }
    if (n == 2 && lower(name[0]) == 'h' && name[1] >= '1' && name[1] <= '6') {
        char tag[5] = {'<', 'h', (char)name[1], '>', 0};
        block(p, tag);
    } else if (same(name, n, "p") || same(name, n, "blockquote")) block(p, "<p>");
    else if (same(name, n, "li")) block(p, "<li>");
    else if (in_list(name, n, lines)) block(p, "<br>");
    else if (same(name, n, "td") || same(name, n, "th")) text_char(p, ' ');
    else if (same(name, n, "a")) {
        static const char *const dead[] = {"javascript:", "mailto:", "tel:", "data:", "sms:", NULL};
        const attr_t *href = attr(p, "href"), *download = attr(p, "download");
        unsigned i;
        if (p->anchor_open) { raw_text(p, "</a>"); p->anchor_open = 0; }
        if (!href || !value_text(href, value, sizeof(value))) return;
        for (i = 0; dead[i]; ++i) if (prefix((const uint8_t *)value, strlen(value), dead[i])) return;
        if (p->space) { raw(p, " ", 1); p->space = 0; }
        raw_text(p, "<a href=");
        quoted(p, value);
        if (download) {
            raw_text(p, " download");
            if (value_text(download, value, 128)) { raw_text(p, "="); quoted(p, value); }
        }
        raw_text(p, ">");
        p->anchor_open = 1;
    } else if (same(name, n, "img")) {
        size_t length = image_url(p, value, sizeof(value));
        const char *dot = length ? strrchr(value, '.') : NULL;
        int svg = dot && prefix((const uint8_t *)dot, strlen(dot), ".svg");
        if (length && !svg && !tiny(p, "width") && !tiny(p, "height")) {
            /* Inline, as in the source: no line break around the picture. */
            if (p->space) { raw(p, " ", 1); p->space = 0; }
            raw_text(p, "<img src=");
            quoted(p, value);
            image_size(p, &width, &height);
            number_attr(p, " w=", width);
            number_attr(p, " h=", height);
            raw_text(p, ">");
            p->line_start = 0;
        } else if (value_text(attr(p, "alt"), value, 80)) {
            text_char(p, '[');
            text_word(p, value);
            text_char(p, ']');
        }
    } else if (same(name, n, "form")) form_begin(p);
    else if (same(name, n, "input")) form_input(p);
    else if (same(name, n, "select")) {
        if (p->form) {
            (void)value_text(attr(p, "name"), p->select_name, sizeof(p->select_name));
            p->select_value[0] = 0;
            p->in_select = 1;
            p->option_taken = 0;
        } else skip_element(p, name, n);
    }
}
static void end_tag(page_t *p, const uint8_t *name, size_t n)
{
    static const char *const lines[] = {"div", "tr", "ul", "ol", "table", "dl", "dd", "header", "footer",
                                        "nav", "main", "section", "article", "aside", "address",
                                        "figure", "figcaption", "pre", "details", "summary", "p",
                                        "blockquote", "li", "center", "fieldset", "caption", NULL};
    element_pop(p, name, n);
    if (p->in_select) {
        if (same(name, n, "select")) {
            p->in_select = 0;
            form_field(p, p->select_name, p->select_value, 0);
        }
        return;
    }
    if (n == 2 && lower(name[0]) == 'h' && name[1] >= '1' && name[1] <= '6') {
        char tag[6] = {'<', '/', 'h', (char)name[1], '>', 0};
        raw_text(p, tag);
        p->space = 0;
        p->line_start = 1;
    } else if (same(name, n, "a")) {
        if (p->anchor_open) { raw_text(p, "</a>"); p->anchor_open = 0; }
    } else if (same(name, n, "form")) form_end(p);
    else if (in_list(name, n, lines)) block(p, "<br>");
}

size_t net_page_filter(const uint8_t *in, size_t length, const char *content_type,
                       const char *base_url, const uint8_t *css, size_t css_length_bytes,
                       uint8_t *out, size_t capacity, int *cut)
{
    static page_t page; /* Too large for the stack; the caller is not re-entrant. */
    page_t *p = &page;
    const uint8_t *charset;
    size_t head = length < 2048U ? length : 2048U, type_length = content_type ? strlen(content_type) : 0;

    memset(p, 0, sizeof(*p));
    if (cut) *cut = 0;
    if (!in || !out || capacity <= sizeof(NOTE)) return 0;
    p->in = in;
    p->length = length;
    p->out = out;
    p->capacity = capacity - (sizeof(NOTE) - 1U);
    p->base = base_url ? base_url : "";
    p->line_start = 1;
    charset = find_text((const uint8_t *)content_type, type_length, "charset=");
    if (charset) type_length -= (size_t)(charset - (const uint8_t *)content_type);
    else {
        charset = find_text(in, head, "charset=");
        type_length = charset ? head - (size_t)(charset - in) : 0;
    }
    if (charset) {
        charset += 8; type_length -= 8;
        if (type_length && (*charset == '"' || *charset == '\'')) { ++charset; --type_length; }
        p->latin1 = prefix(charset, type_length, "iso-8859-1") || prefix(charset, type_length, "windows-1252") ||
                    prefix(charset, type_length, "latin1") || prefix(charset, type_length, "us-ascii");
    }
    if (length >= 3 && in[0] == 0xEF && in[1] == 0xBB && in[2] == 0xBF) p->at = 3;
    find_fragments(p);
    /* Linked sheets come first in most pages, then the <style> blocks. */
    if (css && css_length_bytes) css_parse(p, css, css_length_bytes, 0);
    find_styles(p);

    while (p->at < p->length && !p->cut) {
        uint8_t c = p->in[p->at];
        if (c == '<') {
            const uint8_t *name;
            size_t n;
            int closing;
            if (p->at + 3 < p->length && !memcmp(p->in + p->at, "<!--", 4)) {
                const uint8_t *end = find_text(p->in + p->at + 4, p->length - p->at - 4, "-->");
                p->at = end ? (size_t)(end + 3 - p->in) : p->length;
                continue;
            }
            if (p->at + 1 < p->length && (p->in[p->at + 1] == '!' || p->in[p->at + 1] == '?')) {
                while (p->at < p->length && p->in[p->at] != '>') ++p->at;
                if (p->at < p->length) ++p->at;
                continue;
            }
            closing = p->at + 1 < p->length && p->in[p->at + 1] == '/';
            if (p->at + 1 + (size_t)closing < p->length && is_alnum(p->in[p->at + 1 + closing])) {
                p->at += 1 + (size_t)closing;
                n = parse_tag(p, &name);
                if (closing) end_tag(p, name, n);
                else start_tag(p, name, n);
                continue;
            }
            ++p->at;
            if (!p->in_select) text_char(p, '<');
        } else if (c == '&') {
            uint32_t value;
            size_t used = entity(p->in + p->at + 1, p->length - p->at - 1, &value);
            p->at += 1 + used;
            if (!p->in_select) code_point(p, used ? value : '&');
        } else {
            uint32_t value = next_char(p);
            if (!p->in_select) code_point(p, value);
        }
    }
    if (!p->cut) form_end(p);
    if (!p->cut && p->anchor_open) raw_text(p, "</a>");
    if (p->cut) {
        /* Do not leave half a tag: cut back to the last complete one. */
        size_t open = p->used;
        while (open && p->out[open - 1] != '<' && p->out[open - 1] != '>') --open;
        if (open && p->out[open - 1] == '<') p->used = open - 1;
        memcpy(p->out + p->used, NOTE, sizeof(NOTE) - 1U);
        p->used += sizeof(NOTE) - 1U;
        if (cut) *cut = 1;
    }
    return p->used;
}
