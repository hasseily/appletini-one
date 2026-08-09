/* Host-side unit test for the ImageWriter II interpreter.
 *
 * Compiled by scripts/test_ssc_card.py with a host C compiler against the
 * real ps_sources/frontend/imagewriter.c and ps_sources/lib/fb16.c (for
 * the shared 7x8 glyphs). Exercises text placement, bold, line spacing,
 * ESC G graphics, form feeds, and automatic page overflow.
 */

#include <stdio.h>
#include <string.h>

#include "imagewriter.h"

static uint8_t g_canvas[(size_t)IW_PAGE_WIDTH * IW_PAGE_HEIGHT];
static int g_pages;
static unsigned long g_last_page_ink;

static unsigned long ink_in_rect(const uint8_t *canvas,
                                 int x0, int y0, int x1, int y1)
{
    unsigned long count = 0;
    int x;
    int y;

    for (y = y0; y < y1; ++y) {
        for (x = x0; x < x1; ++x) {
            if (canvas[(size_t)y * IW_PAGE_WIDTH + x] != 0xFFU) {
                count++;
            }
        }
    }
    return count;
}

static void page_done(void *ctx, const uint8_t *canvas,
                      uint32_t width, uint32_t height)
{
    (void)ctx;
    g_pages++;
    g_last_page_ink = ink_in_rect(canvas, 0, 0, (int)width, (int)height);
}

static int g_failures;

#define REQUIRE(cond, what) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "FAIL: %s (line %d)\n", (what), __LINE__); \
            g_failures++; \
        } \
    } while (0)

static void feed_str(imagewriter_t *iw, const char *s)
{
    imagewriter_feed(iw, (const uint8_t *)s, (uint32_t)strlen(s));
}

int main(void)
{
    imagewriter_t iw;
    unsigned long plain_ink;
    unsigned long bold_ink;
    int i;

    REQUIRE(imagewriter_init(&iw, g_canvas, page_done, NULL) == 0, "init");

    /* Draft text lands in the first character cell and nowhere below. */
    feed_str(&iw, "A");
    REQUIRE(ink_in_rect(g_canvas, 0, 0, 20, 20) > 0, "glyph ink present");
    REQUIRE(ink_in_rect(g_canvas, 0, 20, IW_PAGE_WIDTH, IW_PAGE_HEIGHT) == 0,
            "no ink below the first text row");

    /* CR+LF moves to the next 1/6-inch line; the MSB strips by default,
     * so a high-bit CR ($8D) acts as a carriage return. */
    imagewriter_feed(&iw, (const uint8_t *)"\x8d\x0a" "B", 3);
    REQUIRE(ink_in_rect(g_canvas, 0, 24, 20, 44) > 0, "second line ink");

    /* Bold doubles the printed dots of the same glyph. */
    imagewriter_flush_page(&iw);
    g_pages = 0;
    imagewriter_reset(&iw);
    feed_str(&iw, "H");
    plain_ink = ink_in_rect(g_canvas, 0, 0, 40, 20);
    imagewriter_flush_page(&iw);
    imagewriter_reset(&iw);
    g_pages = 0;
    feed_str(&iw, "\x1b!H");
    bold_ink = ink_in_rect(g_canvas, 0, 0, 40, 20);
    REQUIRE(bold_ink > plain_ink, "bold prints more dots");
    feed_str(&iw, "\x1b\"");
    REQUIRE(iw.bold == 0U, "ESC \" cancels bold");

    /* ESC T nn sets line spacing in 1/144 inch; ESC A restores 1/6. */
    feed_str(&iw, "\x1bT12");
    REQUIRE(iw.line_spacing == 12, "ESC T12 spacing");
    feed_str(&iw, "\x1b" "A");
    REQUIRE(iw.line_spacing == 24, "ESC A spacing");

    /* ESC q selects 15 cpi with 120 dpi graphics. */
    feed_str(&iw, "\x1bq");
    REQUIRE(iw.char_advance == 192 && iw.dot_advance == 24, "ESC q pitch");
    feed_str(&iw, "\x1bN");

    /* ESC G with four full columns: a 16-pixel-tall bar at the head. */
    imagewriter_flush_page(&iw);
    imagewriter_reset(&iw);
    g_pages = 0;
    imagewriter_feed(&iw, (const uint8_t *)"\x1bG0004\xff\xff\xff\xff", 10);
    REQUIRE(ink_in_rect(g_canvas, 0, 0, 12, 16) >= 32, "ESC G bar ink");
    REQUIRE(ink_in_rect(g_canvas, 0, 16, IW_PAGE_WIDTH, IW_PAGE_HEIGHT) == 0,
            "ESC G stays in its dot rows");
    /* Graphics data must not be eaten as control characters: an ESC byte
     * inside the counted data is a dot column, not a command. */
    imagewriter_feed(&iw, (const uint8_t *)"\x1bG0001\x1b", 7);
    REQUIRE(iw.esc_state == IW_ESC_NONE, "graphics data is raw");

    /* FF emits the dirty page once; a second FF on a clean page is free. */
    imagewriter_feed(&iw, (const uint8_t *)"\x0c", 1);
    REQUIRE(g_pages == 1, "FF emitted the page");
    REQUIRE(g_last_page_ink > 0, "emitted page carried the ink");
    imagewriter_feed(&iw, (const uint8_t *)"\x0c", 1);
    REQUIRE(g_pages == 1, "clean FF emits nothing");

    /* Enough line feeds walk off the page bottom and emit automatically. */
    feed_str(&iw, "X");
    for (i = 0; i < 70; ++i) {
        imagewriter_feed(&iw, (const uint8_t *)"\x0a", 1);
    }
    REQUIRE(g_pages == 2, "page overflow emitted the page");

    /* US n feeds n lines in one control. */
    imagewriter_reset(&iw);
    imagewriter_feed(&iw, (const uint8_t *)"\x1f\x33", 2);   /* '3' -> 3 */
    REQUIRE(iw.y == 3 * 24, "US 3 fed three lines");

    /* ESC c returns to power-on state without ejecting. */
    feed_str(&iw, "\x1bT05\x1bq\x1b!\x1b" "c");
    REQUIRE(iw.line_spacing == 24 && iw.char_advance == 288 && iw.bold == 0U,
            "ESC c reinitializes");

    if (g_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    printf("IMAGEWRITER HOST PASS\n");
    return 0;
}
