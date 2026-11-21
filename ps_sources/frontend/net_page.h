#ifndef NET_PAGE_H
#define NET_PAGE_H
#include <stddef.h>
#include <stdint.h>

/* Page service for small Apple browsers. The card reads a web page with a
 * real tokenizer and writes a short page in 7-bit ASCII that uses only:
 *
 *   <h1>..<h6> </h1>..</h6> <p> <br> <li> <a href="..."> </a>
 *   <img src="..." w=N h=N> <b id="..."> and the entities &lt; &amp;
 *
 * - <head>, scripts, styles, comments, SVG and hidden elements are gone.
 * - UTF-8 or Latin-1 text and all entities become ASCII.
 * - An image tag carries the real URL (src, data-src or srcset), and
 *   w=N and h=N when the page gives the display size: width and height
 *   attributes in pixels, a style attribute, or CSS rules (width, height,
 *   max-width, max-height; selectors by tag, class and id with at most
 *   one ancestor part; @media by screen width). Images stay inline.
 * - <b id> marks only the targets of #fragment links of the same page.
 * - A form becomes one link. Its URL holds the fields; "%s" stands for
 *   each field that the user must type. A POST form has the URL
 *   "post:" + action + " " + body (one space between them).
 */
#define NET_PAGE_RAW_MAX (4U * 1024U * 1024U)
#define NET_PAGE_OUT_MAX 65535U

/* 1: the content type is HTML, so the filter applies. */
int net_page_is_html(const char *content_type);
/* Returns the output length. *cut is 1 when the output did not fit; the
 * output then ends with a visible note. base_url is the final page URL. */
/* css: the text of the linked style sheets, or NULL. */
size_t net_page_filter(const uint8_t *in, size_t length, const char *content_type,
                       const char *base_url, const uint8_t *css, size_t css_length,
                       uint8_t *out, size_t capacity, int *cut);
/* The absolute URL of linked style sheet number `index` (0 = first) of the
 * page. Returns 0 when there is no such sheet. */
#define NET_PAGE_SHEETS 3U
#define NET_PAGE_CSS_MAX (1024U * 1024U)
int net_page_stylesheet(const uint8_t *in, size_t length, const char *base_url, unsigned index,
                        char *out, size_t capacity);
#endif
