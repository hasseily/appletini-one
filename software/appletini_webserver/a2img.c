/* A2IMG -- streaming internet image viewer (SHR4 PAL256).
 *
 * Fetches any web image through the wsrv.nl proxy (plain HTTP; the
 * proxy terminates TLS to the origin and resizes/transcodes server
 * side) as a 160x200 GIF, and decodes it STRAIGHT out of the TCP
 * segments into aux SHR memory -- the 30 KB GIF never exists in Apple
 * RAM. The GIF's 256-color palette maps 1:1 onto SHR4 PAL256: palette
 * to aux $9E00 (as $GB, $2R entries -- the $2 nibble is the PAL256
 * submode tag), each pixel index to one SHR byte. The screen turns on
 * as soon as the palette arrives, so the image paints line by line
 * while it downloads.
 *
 * Needs the SHR4 firmware to decode PAL256 (plain-SHR firmware shows
 * noise), a //e with aux memory, and the Ethernet port. 160x200 shows
 * line-doubled as the full 640x400 SHR output. A true 400-line double
 * field would need main $2000-$9FFF, where this program lives; the
 * LZW tables alone outgrow what is left, so PAL256 stays single-field.
 *
 * Keys when done: G new URL, R reload, any other key to the menu.
 */

#include <apple2.h>
#include <conio.h>
#include <ctype.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "appletini_net.h"
#include "ip65_min.h"

#define PROXY_HOST "wsrv.nl"
#define DEFAULT_URL \
    "https://wallpapers-clan.com/wp-content/uploads/2024/02/" \
    "marvel-spiderman-in-rain-comics-desktop-wallpaper-cover.jpg"

#define IMG_W      160U
#define IMG_H      200U
#define URL_CAP    180U
#define REQ_CAP    420U

#define NEWVIDEO   (*(volatile uint8_t *)0xC029)

/* glue exports (a2img_net.s) */
extern uint8_t *net_data_ptr;
extern uint16_t net_data_len;
extern uint8_t net_closed;
extern uint16_t net_out_len;
extern uint16_t aux_dst;
extern uint16_t aux_len;
extern uint8_t aux_val;
uint8_t __fastcall__ net_resolve(const char *host);
uint8_t __fastcall__ net_connect(uint16_t port);
uint8_t __fastcall__ net_send(const uint8_t *buf);
void net_close(void);
void net_poll(void);
void __fastcall__ aux_copy(const uint8_t *src);
void aux_fill(void);

/* ------------------------------------------------------------------ */
/* Decoder state                                                       */
/* ------------------------------------------------------------------ */

enum {
    ST_HTTP, ST_SIG, ST_GCT, ST_BLOCK, ST_EXT_LABEL, ST_EXT_LEN,
    ST_EXT_DATA, ST_IMGDESC, ST_LCT, ST_LZW_MIN, ST_SUB_LEN,
    ST_SUB_DATA, ST_DONE, ST_ERR
};

static uint8_t state;
static const char *err_msg;
static uint8_t progressed;

static uint8_t hold[16];        /* small accumulation buffer */
static uint8_t hold_n;
static uint8_t hold_want;

static uint8_t http_match;      /* \r\n\r\n progress */

static uint16_t pal_entries;
static uint16_t pal_byte;       /* 0..entries*3-1 */
static uint8_t pal_r, pal_g;
static uint8_t pal_buf[512];

static uint16_t ext_left;

static uint8_t img_w;           /* clipped to IMG_W */
static uint16_t src_w;          /* full source width */
static uint16_t src_h;
static uint8_t interlaced;

/* LZW */
static uint16_t prefix[4096];
static uint8_t suffix[4096];
static uint8_t stack[4096];
static uint16_t clear_code, end_code, next_code, prev_code;
static uint8_t min_code, cur_width;
static uint8_t got_end;
static uint8_t first_char;
static unsigned long bitbuf;
static uint8_t nbits;
static uint8_t sub_left;

/* raster */
static uint8_t line_buf[IMG_W];
static uint16_t lx;             /* x within source row */
static uint16_t rows_done;
static uint16_t out_row;        /* screen row (interlace-mapped) */
static uint8_t pass;

static char url[URL_CAP];
static uint8_t req[REQ_CAP];

static const uint8_t pass_start[4] = { 0, 4, 2, 1 };
static const uint8_t pass_step[4] = { 8, 8, 4, 2 };

/* ------------------------------------------------------------------ */

static void fail(const char *msg)
{
    if (state != ST_ERR) {
        err_msg = msg;
        state = ST_ERR;
    }
}

static void setup_display(void)
{
    static const uint8_t magic[4] = { 0xD3, 0xC8, 0xD2, 0xB4 };
    static const uint8_t zeros[4] = { 0, 0, 0, 0 };

    aux_dst = 0x9E00;
    aux_len = 512;
    aux_copy(pal_buf);
    aux_dst = 0x9D00;               /* SCBs: 320 mode, palette 0 */
    aux_len = 200;
    aux_val = 0;
    aux_fill();
    aux_dst = 0x9DF8;               /* ctrl: no paged mode */
    aux_len = 4;
    aux_copy(zeros);
    aux_dst = 0x9DFC;
    aux_len = 4;
    aux_copy(magic);
    aux_dst = 0x2000;               /* clear the pixel field */
    aux_len = 0x7D00;
    aux_val = 0;
    aux_fill();
    NEWVIDEO = 0xC1;                /* SHR on: paint live from here */
}

static void palette_done(void)
{
    /* entries < 256 leave the rest black ($20 submode tag intact) */
    uint16_t i;
    for (i = pal_entries * 2U; i < 512U; i += 2U) {
        pal_buf[i] = 0x00;
        pal_buf[i + 1U] = 0x20;
    }
    setup_display();
}

static void pal_feed(uint8_t b)
{
    uint8_t which = (uint8_t)(pal_byte % 3U);
    uint16_t entry = pal_byte / 3U;

    if (which == 0U) {
        pal_r = b;
    } else if (which == 1U) {
        pal_g = b;
    } else {
        /* $GB then $2R: the 2 nibble tags every entry PAL256 */
        pal_buf[entry * 2U] =
            (uint8_t)((pal_g & 0xF0U) | (b >> 4));
        pal_buf[entry * 2U + 1U] = (uint8_t)(0x20U | (pal_r >> 4));
    }
    ++pal_byte;
    if (pal_byte == pal_entries * 3U) {
        palette_done();
        state = ST_BLOCK;
    }
}

static void flush_line(void)
{
    if (out_row < IMG_H) {
        aux_dst = (uint16_t)(0x2000U + 160U * out_row);
        aux_len = IMG_W;
        aux_copy(line_buf);
    }
    memset(line_buf, 0, IMG_W);
    ++rows_done;
    if (interlaced) {
        out_row += pass_step[pass];
        while (out_row >= src_h && pass < 3U) {
            ++pass;
            out_row = pass_start[pass];
        }
    } else {
        ++out_row;
    }
    lx = 0;
}

static void emit(uint8_t px)
{
    if (lx < img_w) {
        line_buf[lx] = px;
    }
    ++lx;
    if (lx >= src_w) {
        flush_line();
    }
}

static void lzw_restart(void)
{
    next_code = (uint16_t)(end_code + 1U);
    cur_width = (uint8_t)(min_code + 1U);
    prev_code = 0xFFFFU;
}

static void lzw_code(uint16_t code)
{
    uint16_t c;
    uint16_t sp;

    if (code == clear_code) {
        lzw_restart();
        return;
    }
    if (code == end_code) {
        got_end = 1;
        return;
    }
    if (prev_code == 0xFFFFU) {
        if (code >= clear_code) {
            fail("BAD LZW STREAM");
            return;
        }
        first_char = (uint8_t)code;
        emit(first_char);
        prev_code = code;
        return;
    }

    c = code;
    sp = 0;
    if (code >= next_code) {
        if (code > next_code) {
            fail("BAD LZW CODE");
            return;
        }
        stack[sp] = first_char;         /* KwKwK case */
        ++sp;
        c = prev_code;
    }
    while (c > end_code) {
        if (sp >= 4095U) {
            fail("LZW CHAIN LOOP");     /* corrupted table */
            return;
        }
        stack[sp] = suffix[c];
        ++sp;
        c = prefix[c];
    }
    first_char = (uint8_t)c;
    stack[sp] = first_char;
    ++sp;
    while (sp != 0U) {
        --sp;
        emit(stack[sp]);
    }
    if (next_code < 4096U) {
        prefix[next_code] = prev_code;
        suffix[next_code] = first_char;
        ++next_code;
        if (next_code == (uint16_t)(1U << cur_width) && cur_width < 12U) {
            ++cur_width;
        }
    }
    prev_code = code;
}

static void lzw_feed(uint8_t b)
{
    uint16_t code;

    if (got_end || state == ST_ERR) {
        return;
    }
    bitbuf |= (unsigned long)b << nbits;
    nbits += 8U;
    while (nbits >= cur_width) {
        code = (uint16_t)(bitbuf & ((1U << cur_width) - 1U));
        bitbuf >>= cur_width;
        nbits -= cur_width;
        lzw_code(code);
        if (got_end || state == ST_ERR) {
            return;
        }
    }
}

/* One byte through the whole HTTP + GIF state machine. */
static void step(uint8_t b)
{
    switch (state) {
    case ST_HTTP:
        /* scan for the \r\n\r\n that ends the response headers */
        if (b == '\r') {
            http_match = (http_match == 2U) ? 3U : 1U;
        } else if (b == '\n') {
            if (http_match == 3U) {
                state = ST_SIG;
                hold_n = 0;
                hold_want = 13;
            } else {
                http_match = (http_match == 1U) ? 2U : 0U;
            }
        } else {
            http_match = 0;
        }
        break;

    case ST_SIG:
        hold[hold_n] = b;
        ++hold_n;
        if (hold_n < hold_want) {
            break;
        }
        if (memcmp(hold, "GIF8", 4) != 0) {
            fail("NOT A GIF (BAD URL?)");
            break;
        }
        src_w = (uint16_t)(hold[6] | ((uint16_t)hold[7] << 8));
        src_h = (uint16_t)(hold[8] | ((uint16_t)hold[9] << 8));
        if ((hold[10] & 0x80U) == 0U) {
            fail("GIF HAS NO PALETTE");
            break;
        }
        pal_entries = (uint16_t)(2U << (hold[10] & 7U));
        pal_byte = 0;
        state = ST_GCT;
        break;

    case ST_GCT:
    case ST_LCT:
        pal_feed(b);
        break;

    case ST_BLOCK:
        if (b == 0x21U) {
            state = ST_EXT_LABEL;
        } else if (b == 0x2CU) {
            state = ST_IMGDESC;
            hold_n = 0;
            hold_want = 9;
        } else if (b == 0x3BU) {
            state = (rows_done != 0U) ? ST_DONE : ST_ERR;
            if (state == ST_ERR) {
                err_msg = "EMPTY GIF";
            }
        }
        /* stray zeros between blocks: ignore */
        break;

    case ST_EXT_LABEL:
        state = ST_EXT_LEN;
        break;

    case ST_EXT_LEN:
        if (b == 0U) {
            state = ST_BLOCK;
        } else {
            ext_left = b;
            state = ST_EXT_DATA;
        }
        break;

    case ST_EXT_DATA:
        --ext_left;
        if (ext_left == 0U) {
            state = ST_EXT_LEN;
        }
        break;

    case ST_IMGDESC:
        hold[hold_n] = b;
        ++hold_n;
        if (hold_n < hold_want) {
            break;
        }
        src_w = (uint16_t)(hold[4] | ((uint16_t)hold[5] << 8));
        src_h = (uint16_t)(hold[6] | ((uint16_t)hold[7] << 8));
        if (src_w == 0U || src_w > 320U) {
            fail("IMAGE TOO WIDE");
            break;
        }
        img_w = (src_w > IMG_W) ? (uint8_t)IMG_W : (uint8_t)src_w;
        interlaced = (uint8_t)((hold[8] & 0x40U) != 0U);
        pass = 0;
        out_row = interlaced ? pass_start[0] : 0U;
        rows_done = 0;
        lx = 0;
        memset(line_buf, 0, IMG_W);
        if ((hold[8] & 0x80U) != 0U) {
            pal_entries = (uint16_t)(2U << (hold[8] & 7U));
            pal_byte = 0;
            state = ST_LCT;     /* replaces the palette, re-arms */
        } else {
            state = ST_LZW_MIN;
        }
        break;

    case ST_LZW_MIN:
        if (b < 2U || b > 8U) {
            fail("BAD LZW MIN CODE");
            break;
        }
        min_code = b;
        clear_code = (uint16_t)(1U << b);
        end_code = (uint16_t)(clear_code + 1U);
        got_end = 0;
        bitbuf = 0;
        nbits = 0;
        lzw_restart();
        state = ST_SUB_LEN;
        break;

    case ST_SUB_LEN:
        if (b == 0U) {
            state = ST_BLOCK;
        } else {
            sub_left = b;
            state = ST_SUB_DATA;
        }
        break;

    case ST_SUB_DATA:
        lzw_feed(b);
        --sub_left;
        if (sub_left == 0U) {
            state = ST_SUB_LEN;
        }
        break;

    default:
        break;
    }
}

/* TCP segment arrival (called from the ip65 callback in the glue). */
void img_feed(void)
{
    const uint8_t *p = net_data_ptr;
    uint16_t n = net_data_len;

    progressed = 1;

    while (n != 0U) {
        step(*p);
        ++p;
        --n;
        if (state == ST_ERR) {
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Request building                                                    */
/* ------------------------------------------------------------------ */

static uint16_t req_len;

static void req_text(const char *s)
{
    while (*s != '\0' && req_len < REQ_CAP - 1U) {
        req[req_len] = (uint8_t)*s;
        ++req_len;
        ++s;
    }
}

static void req_url_encoded(const char *s)
{
    static const char hex[] = "0123456789ABCDEF";
    char c;

    while ((c = *s) != '\0' && req_len < REQ_CAP - 4U) {
        if (c == ' ' || c == '&' || c == '+' || c == '%' ||
            c == '#' || c == '?') {
            req[req_len] = '%';
            req[req_len + 1U] = (uint8_t)hex[((uint8_t)c) >> 4];
            req[req_len + 2U] = (uint8_t)hex[((uint8_t)c) & 0x0FU];
            req_len += 3U;
        } else {
            req[req_len] = (uint8_t)c;
            ++req_len;
        }
        ++s;
    }
}

static void build_request(void)
{
    req_len = 0;
    req_text("GET /?url=");
    req_url_encoded(url);
    req_text("&w=160&h=200&fit=cover&output=gif HTTP/1.0\r\nHost: ");
    req_text(PROXY_HOST);
    req_text("\r\nUser-Agent: A2IMG\r\nConnection: close\r\n\r\n");
}

/* ------------------------------------------------------------------ */

static void reset_decoder(void)
{
    state = ST_HTTP;
    http_match = 0;
    err_msg = NULL;
    rows_done = 0;
    got_end = 0;
    net_closed = 0;
}

static uint8_t fetch(void)
{
    uint16_t idle;

    NEWVIDEO = 0x01;                /* text while we set up */
    clrscr();
    cputs("A2IMG - INTERNET IMAGE VIEWER (SHR4)\r\n\r\n");
    cputs("URL: ");
    cputs(url);
    cputs("\r\n\r\nVIA HTTP://" PROXY_HOST "  160X200 PAL256\r\n");

    reset_decoder();
    build_request();

    cputs("\r\nRESOLVING " PROXY_HOST "...\r\n");
    if (net_resolve(PROXY_HOST) != 0U) {
        cputs("DNS FAILED: ");
        cputs(ip65_strerror(ip65_error));
        return 0;
    }
    cputs("CONNECTING...\r\n");
    if (net_connect(80U) != 0U) {
        cputs("CONNECT FAILED: ");
        cputs(ip65_strerror(ip65_error));
        return 0;
    }
    net_out_len = req_len;
    if (net_send(req) != 0U) {
        cputs("SEND FAILED: ");
        cputs(ip65_strerror(ip65_error));
        net_close();
        return 0;
    }
    cputs("STREAMING (IMAGE PAINTS AS IT LOADS)...\r\n");

    idle = 0;
    while (state != ST_DONE && state != ST_ERR && !net_closed) {
        net_poll();
        if (progressed) {
            progressed = 0;
            idle = 0;
        }
        ++idle;
        if (idle == 0U) {           /* 65536 polls with no progress */
            break;
        }
        if (kbhit()) {
            cgetc();
            net_close();
            NEWVIDEO = 0x01;
            cputs("\r\nCANCELLED.\r\n");
            return 0;
        }
    }
    net_close();

    if (state == ST_ERR) {
        NEWVIDEO = 0x01;
        cputs("\r\nERROR: ");
        cputs(err_msg != NULL ? err_msg : "STREAM");
        cputs("\r\n");
        return 0;
    }
    if (rows_done == 0U) {
        NEWVIDEO = 0x01;
        cputs("\r\nNO IMAGE DATA (PROXY ERROR?)\r\n");
        return 0;
    }
    return 1;
}

static uint8_t prompt_url(void)
{
    uint8_t len = 0;
    char c;

    NEWVIDEO = 0x01;
    clrscr();
    cputs("IMAGE URL (HTTP OR HTTPS):\r\n\r\n");
    cursor(1);
    for (;;) {
        c = (char)cgetc();
        if (c == '\r') {
            break;
        }
        if (c == 0x1B) {
            len = 0;
            break;
        }
        if ((c == 0x08 || c == 0x7F) && len > 0U) {
            --len;
            cputc(0x08);
            cputc(' ');
            cputc(0x08);
            continue;
        }
        if (c >= ' ' && c < 0x7F && len < URL_CAP - 1U) {
            url[len] = c;
            ++len;
            cputc(c);
        }
    }
    cursor(0);
    url[len] = '\0';
    return len != 0U;
}

static void exit_to_menu(void)
{
    exec("BASIC.SYSTEM", "STARTUP");
}

int main(void)
{
    uint8_t status;
    char c;

    clrscr();
    cputs("A2IMG - INTERNET IMAGE VIEWER\r\n\r\n");
    status = appletini_network_init();
    if (status != APPLETINI_NET_OK) {
        cputs(appletini_network_error(status));
        cputs("\r\nPRESS ANY KEY\r\n");
        (void)cgetc();
        exit_to_menu();
        return 1;
    }

    strcpy(url, DEFAULT_URL);
    for (;;) {
        if (fetch() == 0U) {
            cputs("\r\nG=NEW URL  R=RETRY  OTHER=MENU\r\n");
        }
        c = (char)cgetc();
        if (c == 'g' || c == 'G') {
            if (prompt_url() == 0U) {
                break;
            }
        } else if (c == 'r' || c == 'R') {
            /* reload same URL */
        } else {
            break;
        }
    }

    NEWVIDEO = 0x01;
    clrscr();
    exit_to_menu();
    return 0;
}
