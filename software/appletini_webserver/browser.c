/* A2LYNX -- lynx-style text browser for the Appletini demo disk.
 *
 * Plain-HTTP browsing over ip65 (the Appletini virtual Uthernet).
 * The modern web is HTTPS, which a 6502 cannot terminate, so the
 * homepage is FrogFind (frogfind.com): a public proxy that serves any
 * site as simplified plain-HTTP HTML for vintage machines. https://
 * links are routed through it automatically.
 *
 * Renderer: single-pass HTML-subset parser that compacts the response
 * buffer in place (output never outruns input), word-wraps at the
 * screen width, collects <a href> spans, and indexes line starts for
 * scrolling. Comfortable at vTW speeds, usable at 1 MHz.
 *
 * Keys: up/down select link, RETURN/right follow, left/B back,
 * SPACE page down, P page up, G goto URL, S search, H home, R reload,
 * Q/ESC quit to the demo menu.
 */

#include <apple2.h>
#include <conio.h>
#include <ctype.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "appletini_net.h"
#include "ip65_min.h"

#define BUF_CAP    10240U
#define HREF_CAP   2048U
#define LINK_MAX   80U
#define LINE_MAX   250U
#define URL_CAP    128U
#define HIST_MAX   6U

#define SCREEN_W   80U
#define PAGE_ROWS  23U      /* rows 0..22 content, row 23 status */

#define HOME_URL   "http://frogfind.com/"
#define PROXY_PRE  "http://frogfind.com/read.php?a="
#define SEARCH_PRE "http://frogfind.com/?q="

/* Key codes from cgetc() on the Apple II. */
#define KEY_LEFT   0x08
#define KEY_RIGHT  0x15
#define KEY_DOWN   0x0A
#define KEY_UP     0x0B
#define KEY_RETURN 0x0D
#define KEY_ESC    0x1B

static char buf[BUF_CAP];           /* raw response, then rendered text */
static char href_pool[HREF_CAP];
static uint16_t href_used;

typedef struct {
    uint16_t text_off;              /* rendered-text offset of anchor */
    uint16_t line;                  /* filled by the line indexer */
    uint8_t col;
    uint8_t len;
    uint16_t href_off;
} link_t;

static link_t links[LINK_MAX];
static uint8_t link_count;

static uint16_t line_off[LINE_MAX];
static uint16_t line_count;
static uint16_t text_len;

static char cur_url[URL_CAP];
static char new_url[URL_CAP];
static char scratch[URL_CAP];
static char history[HIST_MAX][URL_CAP];
static uint8_t hist_count;

static uint16_t top_line;
static uint8_t sel_link;

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static uint8_t starts_with_ci(const char *s, const char *prefix)
{
    while (*prefix != '\0') {
        if (tolower(*s) != tolower(*prefix)) {
            return 0;
        }
        ++s;
        ++prefix;
    }
    return 1;
}

/* The generic cc65 Apple II conio library maps reverse lowercase to
 * $01-$1A.  With the //e alternate character set those bytes are MouseText,
 * not letters.  The enhanced character set keeps inverse lowercase at
 * $61-$7A.  Feed cputc() a high-ASCII character with reverse off to place
 * that exact screen byte while retaining the NMOS-6502 apple2 target. */
static void inverse_cputc(char c)
{
    if (c >= 'a' && c <= 'z') {
        revers(0);
        cputc((char)((uint8_t)c | 0x80U));
        revers(1);
    } else {
        cputc(c);
    }
}

static void inverse_cputs(const char *s)
{
    while (*s != '\0') {
        inverse_cputc(*s++);
    }
}

static void status_line(const char *left, const char *right)
{
    uint8_t x;

    gotoxy(0, 23);
    revers(1);
    for (x = 0; x < SCREEN_W; ++x) {
        cputc(' ');
    }
    gotoxy(0, 23);
    inverse_cputs(left);
    if (right != NULL) {
        x = (uint8_t)strlen(right);
        if (x < SCREEN_W) {
            gotoxy((uint8_t)(SCREEN_W - x), 23);
            inverse_cputs(right);
        }
    }
    revers(0);
}

/* Single-line editor on the status row; returns 0 on ESC/empty. */
static uint8_t prompt_line(const char *label, char *out, uint8_t cap)
{
    uint8_t len = 0;
    char c;

    status_line(label, NULL);
    gotoxy((uint8_t)strlen(label), 23);
    revers(1);
    cursor(1);
    for (;;) {
        c = (char)cgetc();
        if (c == KEY_RETURN) {
            break;
        }
        if (c == KEY_ESC) {
            len = 0;
            break;
        }
        if (c == KEY_LEFT || c == 0x7F) {
            if (len > 0U) {
                --len;
                cputc(KEY_LEFT);
                cputc(' ');
                cputc(KEY_LEFT);
            }
            continue;
        }
        if (c >= ' ' && c < 0x7F && len < (uint8_t)(cap - 1U)) {
            out[len] = c;
            ++len;
            inverse_cputc(c);
        }
    }
    cursor(0);
    revers(0);
    out[len] = '\0';
    return len != 0U;
}

/* ------------------------------------------------------------------ */
/* URL handling                                                        */
/* ------------------------------------------------------------------ */

/* Resolve href (relative to cur_url) into new_url. Returns 0 if the
 * link is not followable (mailto:, javascript:, fragment, empty). */
static uint8_t resolve_url(const char *href)
{
    uint16_t n = 0;
    const char *base;
    const char *path;
    const char *last_slash;

    if (href[0] == '\0' || href[0] == '#' ||
        starts_with_ci(href, "mailto:") ||
        starts_with_ci(href, "javascript:")) {
        return 0;
    }
    if (starts_with_ci(href, "https://")) {
        /* The 6502 cannot do TLS: route through the FrogFind proxy. */
        if (href == new_url) {
            return 1;               /* self-resolve: keep as typed */
        }
        strcpy(new_url, PROXY_PRE);
        n = (uint16_t)strlen(new_url);
        while (*href != '\0' && *href != '#' && n < URL_CAP - 1U) {
            new_url[n] = *href;
            ++n;
            ++href;
        }
        new_url[n] = '\0';
        return 1;
    }
    if (starts_with_ci(href, "http://")) {
        if (href != new_url) {
            while (*href != '\0' && *href != '#' && n < URL_CAP - 1U) {
                new_url[n] = *href;
                ++n;
                ++href;
            }
            new_url[n] = '\0';
        }
        return 1;
    }
    if (href[0] == '/' && href[1] == '/') {
        strcpy(scratch, "http:");
        strncat(scratch, href, URL_CAP - 7U);
        strcpy(new_url, scratch);
        goto strip_fragment;
    }

    /* Relative to the current URL. base = scheme://host */
    base = cur_url;
    path = strchr(base + 7, '/');       /* past "http://" */
    if (href[0] == '/') {
        /* host-absolute: keep scheme://host only */
        if (path == NULL) {
            path = base + strlen(base);
        }
        n = (uint16_t)(path - base);
    } else {
        /* directory-relative: keep through the last '/' of the path */
        last_slash = strrchr(base + 7, '/');
        if (last_slash == NULL) {
            n = (uint16_t)strlen(base);
        } else {
            n = (uint16_t)(last_slash - base) + 1U;
        }
    }
    if (n >= URL_CAP - 2U) {
        return 0;
    }
    /* href may alias new_url or href_pool: stage through scratch */
    memcpy(scratch, base, n);
    if (href[0] != '/' && strchr(base + 7, '/') == NULL) {
        scratch[n] = '/';
        ++n;
    }
    scratch[n] = '\0';
    strncat(scratch, href, (size_t)(URL_CAP - 1U - n));
    strcpy(new_url, scratch);

strip_fragment:
    {
        char *hash = strchr(new_url, '#');
        if (hash != NULL) {
            *hash = '\0';
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* HTTP: de-chunk and find the body                                    */
/* ------------------------------------------------------------------ */

static uint16_t find_body(uint16_t total, uint16_t *body_at)
{
    char *p = strstr(buf, "\r\n\r\n");
    uint16_t body;
    uint16_t len;
    uint8_t chunked = 0;
    char *h;

    if (p == NULL) {
        *body_at = 0;
        return total;
    }
    body = (uint16_t)(p - buf) + 4U;
    len = (uint16_t)(total - body);

    *p = '\0';                          /* terminate headers for search */
    for (h = buf; *h != '\0'; ++h) {
        if (*h == '\n' && starts_with_ci(h + 1, "transfer-encoding:") &&
            (strstr(h + 1, "hunked") != NULL)) {
            chunked = 1;
            break;
        }
    }

    if (chunked) {
        /* Collapse chunk framing in place. */
        uint16_t src = body;
        uint16_t dst = body;
        uint16_t chunk;
        char c;

        while (src < total) {
            chunk = 0;
            while (src < total && buf[src] != '\r' && buf[src] != '\n') {
                c = buf[src];
                ++src;
                if (c >= '0' && c <= '9') {
                    chunk = (uint16_t)(chunk << 4) + (uint16_t)(c - '0');
                } else if (c >= 'a' && c <= 'f') {
                    chunk = (uint16_t)(chunk << 4) +
                            (uint16_t)(c - 'a' + 10);
                } else if (c >= 'A' && c <= 'F') {
                    chunk = (uint16_t)(chunk << 4) +
                            (uint16_t)(c - 'A' + 10);
                }
            }
            if (src < total && buf[src] == '\r') {
                ++src;
            }
            if (src < total && buf[src] == '\n') {
                ++src;
            }
            if (chunk == 0U) {
                break;
            }
            while (chunk != 0U && src < total) {
                buf[dst] = buf[src];
                ++dst;
                ++src;
                --chunk;
            }
            if (src < total && buf[src] == '\r') {
                ++src;
            }
            if (src < total && buf[src] == '\n') {
                ++src;
            }
        }
        len = (uint16_t)(dst - body);
    }

    *body_at = body;
    return len;
}

/* Copy a redirect Location header into new_url; returns 0 if none.
 * Only valid after find_body() terminated the header block. */
static uint8_t find_location(void)
{
    char *h;
    uint16_t n = 0;

    for (h = buf; *h != '\0'; ++h) {
        if (*h == '\n' && starts_with_ci(h + 1, "location:")) {
            h += 10;
            while (*h == ' ') {
                ++h;
            }
            while (*h != '\0' && *h != '\r' && *h != '\n' &&
                   n < URL_CAP - 1U) {
                new_url[n] = *h;
                ++n;
                ++h;
            }
            new_url[n] = '\0';
            return n != 0U;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* HTML-subset renderer: compacts buf[body..body+len) to buf[0..)      */
/* ------------------------------------------------------------------ */

static uint16_t out_pos;
static uint8_t out_col;
static uint16_t last_space;         /* out_pos of last space this line */
static uint8_t pending_blank;
static uint8_t any_text;

static void emit_newline(void)
{
    buf[out_pos] = '\n';
    ++out_pos;
    out_col = 0;
    last_space = 0;
}

static void emit_break(void)
{
    if (out_col != 0U) {
        emit_newline();
    }
}

static void emit_blank(void)
{
    emit_break();
    pending_blank = 1;
}

static void emit_char(char c)
{
    uint16_t tail;

    if (c == ' ') {
        if (out_col == 0U) {
            return;                 /* no leading spaces */
        }
        if (buf[out_pos - 1U] == ' ') {
            return;                 /* collapse runs */
        }
    }
    if (pending_blank && c != ' ') {
        if (any_text) {
            emit_newline();
        }
        pending_blank = 0;
    }
    if (c == ' ') {
        last_space = out_pos;
    }
    buf[out_pos] = c;
    ++out_pos;
    ++out_col;
    any_text = 1;
    if (out_col >= SCREEN_W) {
        if (last_space != 0U &&
            (uint16_t)(out_pos - last_space) < SCREEN_W) {
            /* wrap at the last space of this line */
            tail = (uint16_t)(out_pos - last_space - 1U);
            buf[last_space] = '\n';
            out_col = (uint8_t)tail;
            last_space = 0;
        } else {
            emit_newline();
        }
    }
}

static void emit_string(const char *s)
{
    while (*s != '\0') {
        emit_char(*s);
        ++s;
    }
}

/* Entity decode: src points at '&'; returns chars consumed. */
static uint8_t entity(const char *src, uint16_t avail)
{
    static const char *names[] = {
        "&amp;", "&lt;", "&gt;", "&quot;", "&#39;", "&apos;",
        "&nbsp;", "&mdash;", "&ndash;", "&rsquo;", "&lsquo;",
        "&rdquo;", "&ldquo;", "&hellip;"
    };
    static const char chars[] = {
        '&', '<', '>', '"', '\'', '\'',
        ' ', '-', '-', '\'', '\'',
        '"', '"', '.'
    };
    uint8_t i;
    uint8_t n;
    uint16_t v;

    for (i = 0; i < sizeof(chars); ++i) {
        n = (uint8_t)strlen(names[i]);
        if (avail >= n && memcmp(src, names[i], n) == 0) {
            emit_char(chars[i]);
            return n;
        }
    }
    if (avail > 2U && src[1] == '#') {
        v = 0;
        n = 2;
        while (n < 8U && n < avail && isdigit(src[n])) {
            v = (uint16_t)(v * 10U) + (uint16_t)(src[n] - '0');
            ++n;
        }
        if (n < avail && src[n] == ';') {
            ++n;
        }
        emit_char((v >= 32U && v < 127U) ? (char)v : '?');
        return n;
    }
    emit_char('&');
    return 1;
}

/* Parse one tag at src (src[0]=='<'); returns chars consumed. */
static uint16_t tag(const char *src, uint16_t avail)
{
    uint16_t n = 1;
    uint8_t closing = 0;
    char name[12];
    uint8_t ni = 0;
    const char *p;
    const char *lim;
    const char *end;
    uint16_t span;
    uint8_t i;

    if (n < avail && src[n] == '/') {
        closing = 1;
        ++n;
    }
    /* comments: <!-- ... --> */
    if (avail > 3U && src[1] == '!' && src[2] == '-' && src[3] == '-') {
        end = strstr(src + 4, "-->");
        if (end == NULL) {
            return avail;
        }
        return (uint16_t)(end - src) + 3U;
    }
    while (n < avail && isalnum(src[n])) {
        if (ni < sizeof(name) - 1U) {
            name[ni] = (char)tolower(src[n]);
            ++ni;
        }
        ++n;
    }
    name[ni] = '\0';

    /* capture href from <a ...> */
    if (!closing && strcmp(name, "a") == 0) {
        p = src + n;
        lim = src + avail;
        while (p < lim && *p != '>') {
            if (starts_with_ci(p, "href=")) {
                char quote;
                uint16_t hn = 0;
                p += 5;
                quote = *p;
                if (quote == '"' || quote == '\'') {
                    ++p;
                } else {
                    quote = '\0';
                }
                if (link_count < LINK_MAX && href_used < HREF_CAP - 2U) {
                    links[link_count].href_off = href_used;
                    links[link_count].text_off = out_pos;
                    while (p < lim && *p != '\0' &&
                           ((quote != '\0') ? (*p != quote)
                                            : (*p != ' ' && *p != '>')) &&
                           href_used < HREF_CAP - 1U) {
                        href_pool[href_used] = *p;
                        ++href_used;
                        ++p;
                        ++hn;
                    }
                    href_pool[href_used] = '\0';
                    ++href_used;
                    if (hn != 0U) {
                        links[link_count].len = 0;  /* open marker */
                        ++link_count;
                    } else {
                        href_used = links[link_count].href_off;
                    }
                }
                break;
            }
            ++p;
        }
    } else if (closing && strcmp(name, "a") == 0) {
        if (link_count != 0U && links[link_count - 1U].len == 0U) {
            span = (uint16_t)(out_pos - links[link_count - 1U].text_off);
            if (span == 0U) {
                --link_count;       /* empty anchor */
            } else {
                links[link_count - 1U].len =
                    (span > 79U) ? 79U : (uint8_t)span;
            }
        }
    }

    /* skip to '>' */
    while (n < avail && src[n] != '>') {
        ++n;
    }
    if (n < avail) {
        ++n;
    }

    /* content-skipping tags */
    if (!closing && (strcmp(name, "script") == 0 ||
                     strcmp(name, "style") == 0)) {
        end = src + n;
        lim = src + avail;
        while (end + 1 < lim) {
            if (end[0] == '<' && end[1] == '/' &&
                starts_with_ci(end + 2, name)) {
                break;
            }
            ++end;
        }
        while (end < lim && *end != '>') {
            ++end;
        }
        if (end < lim) {
            ++end;
        }
        return (uint16_t)(end - src);
    }

    /* layout tags */
    if (strcmp(name, "p") == 0 ||
        (name[0] == 'h' && ni == 2U && name[1] >= '1' && name[1] <= '6')) {
        emit_blank();
    } else if (strcmp(name, "br") == 0 || strcmp(name, "div") == 0 ||
               strcmp(name, "tr") == 0 || strcmp(name, "table") == 0 ||
               strcmp(name, "ul") == 0 || strcmp(name, "ol") == 0 ||
               strcmp(name, "blockquote") == 0) {
        emit_break();
    } else if (!closing && strcmp(name, "li") == 0) {
        emit_break();
        emit_string("* ");
    } else if (strcmp(name, "td") == 0 || strcmp(name, "th") == 0) {
        emit_char(' ');
    } else if (!closing && strcmp(name, "img") == 0) {
        emit_string("[IMG]");
    } else if (!closing && strcmp(name, "hr") == 0) {
        emit_break();
        for (i = 0; i < 40U; ++i) {
            emit_char('-');
        }
        emit_break();
    }
    return n;
}

static void render(uint16_t body, uint16_t len)
{
    uint16_t src = body;
    uint16_t end = (uint16_t)(body + len);
    char c;

    out_pos = 0;
    out_col = 0;
    last_space = 0;
    pending_blank = 0;
    any_text = 0;
    link_count = 0;
    href_used = 0;

    while (src < end && out_pos < (uint16_t)(BUF_CAP - 4U)) {
        c = buf[src];
        if (c == '<') {
            src += tag(&buf[src], (uint16_t)(end - src));
        } else if (c == '&') {
            src += entity(&buf[src], (uint16_t)(end - src));
        } else if (c == '\r' || c == '\n' || c == '\t') {
            emit_char(' ');
            ++src;
        } else if ((uint8_t)c >= 0x80U) {
            /* UTF-8: one '?' per lead byte, drop continuations */
            if (((uint8_t)c & 0xC0U) != 0x80U) {
                emit_char('?');
            }
            ++src;
        } else if ((uint8_t)c < 0x20U) {
            ++src;
        } else {
            emit_char(c);
            ++src;
        }
    }
    text_len = out_pos;
}

/* Build the line index and locate links on their lines. */
static void index_lines(void)
{
    uint16_t i;
    uint16_t line;
    uint16_t start;
    uint16_t line_end;
    uint8_t li;

    line_off[0] = 0;
    line_count = 1;
    for (i = 0; i < text_len; ++i) {
        if (buf[i] == '\n' && line_count < LINE_MAX) {
            line_off[line_count] = (uint16_t)(i + 1U);
            ++line_count;
        }
    }
    for (li = 0; li < link_count; ++li) {
        for (line = 0; line + 1U < line_count; ++line) {
            if (line_off[line + 1U] > links[li].text_off) {
                break;
            }
        }
        start = line_off[line];
        links[li].line = line;
        links[li].col = (uint8_t)(links[li].text_off - start);
        line_end = (line + 1U < line_count)
                       ? (uint16_t)(line_off[line + 1U] - 1U)
                       : text_len;
        if ((uint16_t)links[li].col + links[li].len >
            (uint16_t)(line_end - start)) {
            links[li].len = (uint8_t)((line_end - start) - links[li].col);
        }
        if (links[li].len == 0U) {
            links[li].len = 1;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Display                                                             */
/* ------------------------------------------------------------------ */

static void draw_page(void)
{
    uint16_t row;
    uint16_t line;
    uint16_t start;
    uint16_t end;
    uint8_t x;
    uint8_t hi_col;
    uint8_t hi_end;

    for (row = 0; row < PAGE_ROWS; ++row) {
        line = (uint16_t)(top_line + row);
        gotoxy(0, (uint8_t)row);
        if (line >= line_count) {
            cclear(SCREEN_W);
            continue;
        }
        start = line_off[line];
        end = (line + 1U < line_count)
                  ? (uint16_t)(line_off[line + 1U] - 1U)
                  : text_len;
        /* is the selected link on this line? */
        hi_col = 0xFF;
        hi_end = 0xFF;
        if (sel_link < link_count && links[sel_link].line == line) {
            hi_col = links[sel_link].col;
            hi_end = (uint8_t)(hi_col + links[sel_link].len);
        }
        x = 0;
        while (start < end && x < SCREEN_W) {
            if (x == hi_col) {
                revers(1);
            }
            if (x == hi_end) {
                revers(0);
            }
            if (x >= hi_col && x < hi_end) {
                inverse_cputc(buf[start]);
            } else {
                cputc(buf[start]);
            }
            ++start;
            ++x;
        }
        revers(0);
        if (x < SCREEN_W) {
            cclear((uint8_t)(SCREEN_W - x));
        }
    }
    status_line(cur_url, "^V:LINK RET:GO B:BACK G:URL S:FIND Q:QUIT");
}

static void scroll_to_link(void)
{
    if (sel_link >= link_count) {
        return;
    }
    if (links[sel_link].line < top_line) {
        top_line = links[sel_link].line;
    } else if (links[sel_link].line >= (uint16_t)(top_line + PAGE_ROWS)) {
        top_line = (uint16_t)(links[sel_link].line - (PAGE_ROWS - 1U));
    }
}

/* ------------------------------------------------------------------ */
/* Page loading                                                        */
/* ------------------------------------------------------------------ */

static void load_page(void)
{
    uint16_t total;
    uint16_t body;
    uint16_t len;
    uint8_t redirects;

    for (redirects = 0; redirects < 5U; ++redirects) {
        status_line("LOADING: ", cur_url);
        total = url_download(cur_url, (const uint8_t *)buf, BUF_CAP - 1U);
        if (total == 0U) {
            strcpy(buf, "REQUEST FAILED: ");
            strcat(buf, ip65_strerror(ip65_error));
            strcat(buf, "\nPRESS G FOR A NEW URL, B FOR BACK, "
                        "H FOR HOME.");
            text_len = (uint16_t)strlen(buf);
            link_count = 0;
            index_lines();
            top_line = 0;
            sel_link = 0;
            clrscr();
            draw_page();
            return;
        }
        if (total >= BUF_CAP - 1U) {
            total = BUF_CAP - 1U;
        }
        buf[total] = '\0';

        /* 3xx? terminate headers, then chase Location */
        if (total > 12U && buf[9] == '3') {
            char *hdr_end = strstr(buf, "\r\n\r\n");
            if (hdr_end != NULL) {
                *hdr_end = '\0';
            }
            if (find_location() && resolve_url(new_url)) {
                strcpy(cur_url, new_url);
                continue;
            }
        }
        break;
    }

    len = find_body(total, &body);
    render(body, len);
    index_lines();
    top_line = 0;
    sel_link = 0;
    scroll_to_link();
    clrscr();
    draw_page();
}

static void push_history(void)
{
    if (hist_count == HIST_MAX) {
        memmove(history[0], history[1],
                (size_t)(HIST_MAX - 1U) * URL_CAP);
        --hist_count;
    }
    strcpy(history[hist_count], cur_url);
    ++hist_count;
}

static void go_back(void)
{
    if (hist_count == 0U) {
        return;
    }
    --hist_count;
    strcpy(cur_url, history[hist_count]);
    load_page();
}

/* Relaunch the demo menu instead of quitting to Bitsy Bye. exec()
 * only returns on failure; fall through to the normal ProDOS quit. */
static void exit_to_menu(void)
{
    exec("BASIC.SYSTEM", "STARTUP");
}

/* ------------------------------------------------------------------ */

int main(void)
{
    uint8_t status;
    char c;
    uint8_t i;

    videomode(VIDEOMODE_80COL);
    clrscr();
    cputs("A2LYNX - APPLETINI TEXT BROWSER\r\n\r\n");

    status = appletini_network_init();
    if (status != APPLETINI_NET_OK) {
        cputs(appletini_network_error(status));
        cputs("\r\nPRESS ANY KEY\r\n");
        (void)cgetc();
        exit_to_menu();
        return 1;
    }

    strcpy(cur_url, HOME_URL);
    load_page();

    for (;;) {
        c = (char)cgetc();
        switch (c) {
        case KEY_UP:
            if (sel_link > 0U) {
                --sel_link;
                scroll_to_link();
                draw_page();
            } else if (top_line > 0U) {
                --top_line;
                draw_page();
            }
            break;
        case KEY_DOWN:
            if (link_count != 0U &&
                sel_link < (uint8_t)(link_count - 1U)) {
                ++sel_link;
                scroll_to_link();
                draw_page();
            } else if ((uint16_t)(top_line + PAGE_ROWS) < line_count) {
                ++top_line;
                draw_page();
            }
            break;
        case ' ':
            if ((uint16_t)(top_line + PAGE_ROWS) < line_count) {
                top_line += PAGE_ROWS - 1U;
                draw_page();
            }
            break;
        case 'p':
        case 'P':
            if (top_line > 0U) {
                top_line = (top_line > (uint16_t)(PAGE_ROWS - 1U))
                               ? (uint16_t)(top_line - (PAGE_ROWS - 1U))
                               : 0U;
                draw_page();
            }
            break;
        case KEY_RETURN:
        case KEY_RIGHT:
            if (sel_link < link_count &&
                resolve_url(&href_pool[links[sel_link].href_off])) {
                push_history();
                strcpy(cur_url, new_url);
                load_page();
            }
            break;
        case KEY_LEFT:
        case 'b':
        case 'B':
            go_back();
            break;
        case 'g':
        case 'G':
            if (prompt_line("URL: ", new_url, URL_CAP)) {
                push_history();
                if (starts_with_ci(new_url, "http")) {
                    if (resolve_url(new_url)) {
                        strcpy(cur_url, new_url);
                    }
                } else {
                    strcpy(cur_url, "http://");
                    strncat(cur_url, new_url, URL_CAP - 9U);
                }
                load_page();
            } else {
                draw_page();
            }
            break;
        case 's':
        case 'S':
            if (prompt_line("SEARCH: ", new_url,
                            (uint8_t)(URL_CAP - sizeof(SEARCH_PRE)))) {
                push_history();
                for (i = 0; new_url[i] != '\0'; ++i) {
                    if (new_url[i] == ' ') {
                        new_url[i] = '+';
                    }
                }
                strcpy(cur_url, SEARCH_PRE);
                strcat(cur_url, new_url);
                load_page();
            } else {
                draw_page();
            }
            break;
        case 'h':
        case 'H':
            push_history();
            strcpy(cur_url, HOME_URL);
            load_page();
            break;
        case 'r':
        case 'R':
            load_page();
            break;
        case 'q':
        case 'Q':
        case KEY_ESC:
            clrscr();
            exit_to_menu();
            return 0;
        default:
            break;
        }
    }
}
