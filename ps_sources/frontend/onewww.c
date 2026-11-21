/* ONEWWW service, CPU0 side. See onewww.h. */
#include "onewww.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../lib/lodepng.h"
#include "apple_fb_handoff.h"
#include "compositor_layout.h"
#include "http_service.h"
#include "onewww_render.h"

#define OP_OPEN  1U
#define OP_CLOSE 2U
#define OP_KEY   3U
#define OP_MOUSE 4U
#define SP_OK     0x00U
#define SP_BADCTL 0x21U
#define SP_BUSY   0x2FU
#define STEP_ROWS 32
#define PAGE_STEP ((int32_t)ONEWWW_PAGE_VIEW - STEP_ROWS)
#define BODY_MAX  (4U * 1024U * 1024U)
#define PAGE_MAX  (16U * 1024U * 1024U)     /* a page after gzip */
#define HISTORY   16U

enum { M_CLOSED, M_VIEW, M_INPUT, M_LOADING };

static const char s_request_headers[] =
    "User-Agent: Mozilla/5.0 (compatible; ONEWWW/1.0; Appletini; Apple II)\r\n"
    "Accept: text/html,text/plain;q=0.9,*/*;q=0.5\r\n"
    "Accept-Language: en\r\n";

static uint8_t s_fields[ONEWWW_FIELDS_BYTES];
static struct {
    uint8_t mode, dirty, release, owner, push, pointer, buttons;
    uint32_t top, restore_top;
    int32_t selected, px, py;
    char url[HTTP_URL_MAX];         /* the shown page; "" is the home page */
    char pending[HTTP_URL_MAX];     /* the page that is loading */
    char fragment[128];
    char input[HTTP_URL_MAX];
    uint32_t input_length;
    char message[200];
    uint8_t *body;
    size_t body_length, body_capacity;
    uint16_t body_status;
    uint32_t progress;
    char history[HISTORY][HTTP_URL_MAX];
    uint32_t history_top[HISTORY];
    uint32_t history_count;
} s;

/* ---- View -------------------------------------------------------------- */

static int32_t top_max(void)
{
    const uint32_t rows = onewww_page_rows();
    return rows > ONEWWW_PAGE_VIEW ? (int32_t)(rows - ONEWWW_PAGE_VIEW) : 0;
}

static void scroll_to(int32_t top)
{
    if (top > top_max()) top = top_max();
    if (top < 0) top = 0;
    if ((uint32_t)top != s.top) { s.top = (uint32_t)top; s.dirty = 1U; }
}

static void say(const char *text)
{
    snprintf(s.message, sizeof(s.message), "%s", text);
    s.dirty = 1U;
}

static void show_home(void)
{
    (void)onewww_page_build((const uint8_t *)onewww_home_page,
                            strlen(onewww_home_page), NULL, NULL, 1);
    s.url[0] = '\0'; s.top = 0U; s.selected = -1; s.mode = M_VIEW; s.dirty = 1U;
}

/* Shows a short page that the card writes itself. */
static void show_note(const char *heading, const char *detail)
{
    static char page[HTTP_URL_MAX + 400U];
    const int n = snprintf(page, sizeof(page),
        "<title>%s</title><h2>%s</h2><p>%s</p><p>%s</p>"
        "<p><small>B: back. G: go to an address. H: home page.</small></p>",
        heading, heading, detail, s.pending);
    (void)onewww_page_build((const uint8_t *)page, (size_t)(n > 0 ? n : 0),
                            NULL, NULL, 0);
    s.top = 0U; s.selected = -1; s.mode = M_VIEW; s.dirty = 1U;
}

static void select_link(int32_t link)
{
    const uint32_t row = link >= 0 ? onewww_link_row((uint32_t)link) : UINT32_MAX;

    s.selected = link; s.dirty = 1U;
    if (row == UINT32_MAX) return;
    if (row < s.top || row + 20U > s.top + ONEWWW_PAGE_VIEW) {
        scroll_to((int32_t)row - (int32_t)ONEWWW_PAGE_VIEW / 3);
    }
}

static void step_link(int direction)
{
    const int32_t count = (int32_t)onewww_link_count();
    int32_t link = s.selected;

    if (count == 0) return;
    if (link >= 0 && onewww_link_row((uint32_t)link) != UINT32_MAX &&
        onewww_link_row((uint32_t)link) + 20U > s.top &&
        onewww_link_row((uint32_t)link) < s.top + ONEWWW_PAGE_VIEW) {
        link += direction;
    } else if (direction > 0) {         /* first link in the view or below */
        for (link = 0; link < count &&
             (onewww_link_row((uint32_t)link) == UINT32_MAX ||
              onewww_link_row((uint32_t)link) < s.top); link++) {}
    } else {                            /* last link in the view or above */
        for (link = count - 1; link >= 0 &&
             (onewww_link_row((uint32_t)link) == UINT32_MAX ||
              onewww_link_row((uint32_t)link) >= s.top + ONEWWW_PAGE_VIEW); link--) {}
    }
    while (link >= 0 && link < count &&
           onewww_link_row((uint32_t)link) == UINT32_MAX) link += direction;
    if (link >= 0 && link < count) select_link(link);
}

/* ---- Loading ----------------------------------------------------------- */

static void body_free(void)
{
    free(s.body); s.body = NULL;
    s.body_length = s.body_capacity = 0U;
}

/* Gives the HTTP engine back. A busy Ethernet release needs more calls. */
static void owner_drop(void)
{
    if (s.owner && (http_service_getowner() != HTTP_OWNER_WWW ||
                    http_service_cancel_owned(HTTP_OWNER_WWW) == 0)) {
        s.owner = 0U;
    }
}

static void go(const char *url, int push)
{
    char full[HTTP_URL_MAX];
    http_request_t request = { full, "GET", s_request_headers, NULL, 0, 0 };
    char *hash;
    int result;

    if (s.owner) { owner_drop(); if (s.owner) { say("The network is busy. Try again."); return; } }
    if (url[0] == '\0') { s.pending[0] = '\0'; }
    snprintf(full, sizeof(full), "%s%s",
             url[0] == '\0' || strstr(url, "://") != NULL ? "" : "https://", url);
    s.fragment[0] = '\0';
    hash = strchr(full, '#');
    if (hash != NULL) {
        snprintf(s.fragment, sizeof(s.fragment), "%s", hash + 1);
        *hash = '\0';
    }
    if (push && s.history_count == HISTORY) {
        memmove(s.history[0], s.history[1], sizeof(s.history[0]) * (HISTORY - 1U));
        memmove(&s.history_top[0], &s.history_top[1], sizeof(uint32_t) * (HISTORY - 1U));
        s.history_count--;
    }
    s.push = (uint8_t)push;
    if (full[0] == '\0') {              /* the home page */
        if (push) {
            strcpy(s.history[s.history_count], s.url);
            s.history_top[s.history_count++] = s.top;
        }
        show_home();
        scroll_to((int32_t)s.restore_top); s.restore_top = 0U;
        return;
    }
    snprintf(s.pending, sizeof(s.pending), "%s", full);
    body_free();
    s.body_status = 0U; s.progress = 0U;
    result = http_service_start_owned(&request, HTTP_OWNER_WWW);
    if (result != 0) {
        char text[80];
        snprintf(text, sizeof(text), result == HTTP_E_ARGUMENT ?
                 "This address is not correct." :
                 "The network is busy (%d). Try again.", result);
        s.restore_top = 0U;
        say(text);
        return;
    }
    s.owner = 1U; s.mode = M_LOADING; s.message[0] = '\0'; s.dirty = 1U;
}

static const char *failure_text(int result)
{
    switch (result) {
    case HTTP_E_DNS: return "The name of the site was not found.";
    case HTTP_E_TIMEOUT: return "The site did not answer in time.";
    case HTTP_E_NETWORK_NOT_READY: return "The network is not ready. Examine the cable and the network settings.";
    case HTTP_E_TRUST: return "The certificate of the site is not trusted.";
    case HTTP_E_TIME: return "The clock of the card is not set. HTTPS needs the date.";
    case HTTP_E_SEED: return "HTTPS needs an SD card that the card can write.";
    case HTTP_E_STORAGE_BUSY: return "The SD card is busy.";
    case HTTP_E_REDIRECT: return "The site sent too many redirects.";
    default: return "The page did not load.";
    }
}

/* Replaces a gzip body by its content. Returns 0 on success. */
static int gunzip(void)
{
    LodePNGDecompressSettings settings = lodepng_default_decompress_settings;
    unsigned char *out = NULL;
    size_t out_size = 0U, at = 10U;
    uint8_t flags;

    if (s.body_length < 18U || s.body[2] != 8U) return -1;
    flags = s.body[3];
    if (flags & 4U) {
        if (at + 2U > s.body_length) return -1;
        at += 2U + (size_t)(s.body[at] | (s.body[at + 1U] << 8));
    }
    if (flags & 8U) { while (at < s.body_length && s.body[at++] != 0U) {} }
    if (flags & 16U) { while (at < s.body_length && s.body[at++] != 0U) {} }
    if (flags & 2U) at += 2U;
    if (at + 8U >= s.body_length) return -1;
    settings.max_output_size = PAGE_MAX;
    if (lodepng_inflate(&out, &out_size, s.body + at, s.body_length - at - 8U,
                        &settings) != 0U && out_size == 0U) {
        free(out);
        return -1;
    }
    body_free();
    s.body = out; s.body_length = s.body_capacity = out_size;
    return 0;
}

static int type_is(const char *content_type, const char *prefix)
{
    const size_t n = strlen(prefix);
    for (size_t i = 0; i < n; i++) {
        char c = content_type[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c != prefix[i]) return 0;
    }
    return 1;
}

/* Changes plain text to a page: one <pre> block. */
static int plain_to_html(void)
{
    size_t extra = 16U, n = 0U;
    uint8_t *page;

    for (size_t i = 0; i < s.body_length; i++) {
        if (s.body[i] == '<' || s.body[i] == '&') extra += 4U;
    }
    page = malloc(s.body_length + extra);
    if (page == NULL) return -1;
    memcpy(page, "<pre>", 5U); n = 5U;
    for (size_t i = 0; i < s.body_length; i++) {
        if (s.body[i] == '<') { memcpy(page + n, "&lt;", 4U); n += 4U; }
        else if (s.body[i] == '&') { memcpy(page + n, "&amp;", 5U); n += 5U; }
        else page[n++] = s.body[i];
    }
    body_free();
    s.body = page; s.body_length = s.body_capacity = n;
    return 0;
}

static void load_done(const http_status_t *status)
{
    char text[160];
    const char *type = status->content_type;
    const uint32_t old_top = s.top;
    char old_url[HTTP_URL_MAX];

    strcpy(old_url, s.url);
    if (s.body_length >= 3U && s.body[0] == 0x1FU && s.body[1] == 0x8BU &&
        gunzip() != 0) {
        body_free();
        show_note("The page did not load", "The compressed data has a fault.");
        return;
    }
    if (type[0] != '\0' && type_is(type, "text/plain")) {
        if (plain_to_html() != 0) s.body_length = 0U;
    } else if (type[0] != '\0' && !type_is(type, "text/html") &&
               !type_is(type, "application/xhtml")) {
        body_free();
        snprintf(text, sizeof(text), "This version cannot show the type %.100s.", type);
        show_note("Not a page", text);
        return;
    }
    if (s.body_length == 0U) {
        body_free();
        snprintf(text, sizeof(text), "The site answered with status %u and no text.",
                 (unsigned)status->status);
        show_note("Empty page", text);
        return;
    }
    snprintf(s.url, sizeof(s.url), "%s", status->url);
    if (onewww_page_build(s.body, s.body_length, type, s.url, 0) != 0) {
        body_free();
        strcpy(s.url, old_url);
        show_note("The page did not load", "The card does not have sufficient memory.");
        return;
    }
    body_free();
    if (s.push) {
        strcpy(s.history[s.history_count], old_url);
        s.history_top[s.history_count++] = old_top;
    }
    s.top = 0U; s.selected = -1; s.mode = M_VIEW; s.dirty = 1U;
    if (s.fragment[0] != '\0') {
        const int row = onewww_anchor_row(s.fragment);
        if (row >= 0) scroll_to(row);
    } else {
        scroll_to((int32_t)s.restore_top);
    }
    s.restore_top = 0U;
    if (status->status >= 400U) {
        snprintf(text, sizeof(text), "The site answered with status %u.",
                 (unsigned)status->status);
        say(text);
    }
}

static void load_poll(void)
{
    uint8_t piece[4096];
    http_status_t status;
    size_t n;

    if (http_service_getowner() != HTTP_OWNER_WWW) {
        s.owner = 0U; s.mode = M_VIEW;
        say("The network stopped the request.");
        return;
    }
    http_service_status(&status);
    if (status.status != s.body_status) {       /* a redirect starts again */
        s.body_status = status.status; s.body_length = 0U;
    }
    while ((n = http_service_read_owned(HTTP_OWNER_WWW, piece, sizeof(piece))) != 0U) {
        if (s.body_length + n > BODY_MAX) continue;    /* keep the first part */
        if (s.body_length + n > s.body_capacity) {
            size_t wanted = s.body_capacity ? s.body_capacity * 2U : 65536U;
            uint8_t *bigger;
            while (wanted < s.body_length + n) wanted *= 2U;
            bigger = realloc(s.body, wanted);
            if (bigger == NULL) continue;
            s.body = bigger; s.body_capacity = wanted;
        }
        memcpy(s.body + s.body_length, piece, n);
        s.body_length += n;
    }
    if ((uint32_t)(s.body_length >> 14) != s.progress) {
        s.progress = (uint32_t)(s.body_length >> 14); s.dirty = 1U;
    }
    http_service_status(&status);
    if (status.state == HTTP_S_FAILED) {
        owner_drop();
        body_free();
        s.restore_top = 0U;
        show_note("The page did not load", failure_text(status.result));
    } else if (status.state == HTTP_S_DONE && status.buffered == 0U) {
        owner_drop();
        load_done(&status);
    }
}

/* ---- Screen ------------------------------------------------------------ */

static uint8_t cpu1_ready(void)
{
    return apple_fb_direct_ack_get() == apple_fb_direct_gen_get();
}

static void publish(uint32_t mode)
{
    uint32_t generation = apple_fb_direct_gen_get() + 1U;
    if (generation == 0U) generation = 1U;
    apple_fb_direct_publish(generation, mode);
}

static void present(void)
{
    char left[HTTP_URL_MAX + 64U], right[24];
    const int32_t limit = top_max();
    int hover = -1;

    if (s.pointer && s.py < (int32_t)ONEWWW_PAGE_VIEW) {
        hover = onewww_link_at(s.px, s.top + (uint32_t)s.py);
    }
    if (s.mode == M_INPUT) {
        snprintf(left, sizeof(left), "Go to: %s_", s.input);
    } else if (s.mode == M_LOADING) {
        snprintf(left, sizeof(left), "Loading (%lu KiB, Escape stops): %s",
                 (unsigned long)(s.body_length >> 10), s.pending);
    } else if (s.message[0] != '\0') {
        snprintf(left, sizeof(left), "%s", s.message);
    } else if (hover >= 0 || s.selected >= 0) {
        snprintf(left, sizeof(left), "%s",
                 onewww_link_href((uint32_t)(hover >= 0 ? hover : s.selected)));
    } else {
        const char *title = onewww_page_title();
        snprintf(left, sizeof(left), "%s%s%s", title,
                 title[0] != '\0' && s.url[0] != '\0' ? " - " : "", s.url);
    }
    snprintf(right, sizeof(right), "%ld%%",
             limit > 0 ? (long)((int64_t)s.top * 100 / limit) : 100L);
    onewww_compose(s.top, left, right, s.selected,
                   s.pointer ? (int)s.px : -1, (int)s.py, s_fields);
    /* The staging area is not cached: no flush is necessary. */
    memcpy((void *)(uintptr_t)APPLE_DIRECT_SHR_STAGING_ADDR, s_fields,
           sizeof(s_fields));
    publish(APPLE_FB_DIRECT_SHOW);
}

void onewww_poll(void)
{
    if (s.owner && s.mode != M_LOADING) owner_drop();
    if (s.mode == M_LOADING) load_poll();
    if (!cpu1_ready()) return;
    if (s.release) {
        s.release = 0U;
        publish(APPLE_FB_DIRECT_RELEASE);
        return;
    }
    if (s.mode == M_CLOSED || !s.dirty) return;
    s.dirty = 0U;
    present();
}

/* ---- Input ------------------------------------------------------------- */

static void follow(int32_t link)
{
    const char *href;

    if (link < 0) return;
    href = onewww_link_href((uint32_t)link);
    if (href[0] == '#') {
        const int row = onewww_anchor_row(href + 1);
        if (row >= 0) scroll_to(row);
        else if (href[1] == '\0') scroll_to(0);
        return;
    }
    go(href, 1);
}

static void key_view(uint8_t code)
{
    const int32_t top = (int32_t)s.top;

    if (code >= 'a' && code <= 'z') code = (uint8_t)(code - 0x20U);
    if (s.message[0] != '\0') { s.message[0] = '\0'; s.dirty = 1U; }
    switch (code) {
    case 0x0A: case 'J': scroll_to(top + STEP_ROWS); break;
    case 0x0B: case 'K': scroll_to(top - STEP_ROWS); break;
    case ' ': scroll_to(top + PAGE_STEP); break;
    case 'P': scroll_to(top - PAGE_STEP); break;
    case 'T': scroll_to(0); break;
    case 'E': scroll_to(top_max()); break;
    case 0x09: case 0x15: case 'N': step_link(1); break;
    case 0x08: step_link(-1); break;
    case 0x0D: follow(s.selected); break;
    case 'G':
        s.mode = M_INPUT; s.input[0] = '\0'; s.input_length = 0U; s.dirty = 1U;
        break;
    case 'B':
        if (s.history_count != 0U) {
            s.history_count--;
            s.restore_top = s.history_top[s.history_count];
            go(s.history[s.history_count], 0);
        } else say("There is no page before this one.");
        break;
    case 'R': s.restore_top = s.top; go(s.url, 0); break;
    case 'H': go("", 1); break;
    case 'Q': onewww_reset(); break;
    default: break;
    }
}

static void key(uint8_t code)
{
    code &= 0x7FU;
    if (s.mode == M_LOADING) {
        if (code == 0x1BU) {
            owner_drop();
            body_free();
            s.mode = M_VIEW; s.restore_top = 0U;
            say("Stopped.");
        }
    } else if (s.mode == M_INPUT) {
        if (code == 0x0DU) {
            s.mode = M_VIEW; s.dirty = 1U;
            if (s.input_length != 0U) go(s.input, 1);
        } else if (code == 0x1BU) {
            s.mode = M_VIEW; s.dirty = 1U;
        } else if (code == 0x08U || code == 0x7FU) {
            if (s.input_length != 0U) s.input[--s.input_length] = '\0';
            s.dirty = 1U;
        } else if (code > 0x20U && code < 0x7FU &&
                   s.input_length + 1U < sizeof(s.input)) {
            s.input[s.input_length++] = (char)code;
            s.input[s.input_length] = '\0';
            s.dirty = 1U;
        }
    } else if (s.mode == M_VIEW) {
        key_view(code);
    }
}

static void mouse(int32_t x, int32_t y, uint8_t buttons)
{
    const uint8_t pressed = (uint8_t)(buttons & ~s.buttons & 1U);

    if (!s.pointer || x != s.px || y != s.py) s.dirty = 1U;
    s.pointer = 1U; s.px = x; s.py = y; s.buttons = buttons;
    if (pressed && s.mode == M_VIEW && y < (int32_t)ONEWWW_PAGE_VIEW) {
        const int link = onewww_link_at(x, s.top + (uint32_t)y);
        if (link >= 0) { s.selected = link; s.dirty = 1U; follow(link); }
    }
}

uint8_t onewww_control(const uint8_t *payload, uint16_t length)
{
    if (payload == NULL || length < 3U || payload[0] != 1U) return SP_BADCTL;
    switch (payload[1]) {
    case OP_OPEN:
        if (length != 3U) return SP_BADCTL;
        s.release = 0U; s.history_count = 0U; s.pointer = 0U; s.message[0] = '\0';
        show_home();
        if (onewww_page_rows() == 0U) { s.mode = M_CLOSED; return SP_BUSY; }
        break;
    case OP_CLOSE:
        if (length != 3U) return SP_BADCTL;
        onewww_reset();
        break;
    case OP_KEY:
        if (length != 3U || s.mode == M_CLOSED) return SP_BADCTL;
        key(payload[2]);
        break;
    case OP_MOUSE:
        if (length != 7U || s.mode == M_CLOSED) return SP_BADCTL;
        mouse((int32_t)(payload[2] | (payload[3] << 8)),
              (int32_t)(payload[4] | (payload[5] << 8)), payload[6]);
        break;
    default:
        return SP_BADCTL;
    }
    onewww_poll();
    return SP_OK;
}

uint16_t onewww_status(uint8_t *out, uint16_t capacity)
{
    const uint32_t rows = onewww_page_rows();

    if (capacity < 16U) return 0U;
    memcpy(out, "ATNW", 4U);
    out[4] = 1U;
    out[5] = s.mode;
    out[6] = (uint8_t)apple_fb_direct_ack_get();
    out[7] = 0U;
    out[8] = (uint8_t)rows; out[9] = (uint8_t)(rows >> 8);
    out[10] = (uint8_t)(rows >> 16); out[11] = (uint8_t)(rows >> 24);
    out[12] = (uint8_t)s.top; out[13] = (uint8_t)(s.top >> 8);
    out[14] = (uint8_t)(s.top >> 16); out[15] = (uint8_t)(s.top >> 24);
    return 16U;
}

void onewww_reset(void)
{
    if (s.mode == M_CLOSED) return;
    owner_drop();
    body_free();
    onewww_page_free();
    s.mode = M_CLOSED; s.dirty = 0U; s.release = 1U;
}
