#ifndef ONEWWW_RENDER_H
#define ONEWWW_RENDER_H

#include <stddef.h>
#include <stdint.h>

/* ONEWWW page renderer. The card lays a page out and draws it into a tall
 * surface of Super Hi-Res rows. Each row has 160 bytes and its own mode:
 * 640 mode with 4 colours for text (its own paper, ink and accent), or
 * 320 mode with 16 colours for a picture. onewww_compose() cuts a 640 x 400 view from the surface and
 * writes it as two SHR fields (interlace). */

#define ONEWWW_VIEW_ROWS    400U
#define ONEWWW_STATUS_ROWS  16U
#define ONEWWW_PAGE_VIEW    (ONEWWW_VIEW_ROWS - ONEWWW_STATUS_ROWS)
#define ONEWWW_FIELD_BYTES  0x8000U     /* Apple $2000-$9FFF of one bank */
#define ONEWWW_FIELDS_BYTES (2U * ONEWWW_FIELD_BYTES)

/* A page is made in two steps, so that the caller can get its linked
 * style sheets between them.
 *
 * onewww_page_parse() reads the HTML. base_url resolves the links and the
 * sheet addresses (NULL: only same-page links work); it must stay valid
 * until the layout ends. internal selects the built-in pages. Returns 0,
 * or -1 without memory or when onewww_pause stopped the parser.
 *
 * Then the page has onewww_page_sheets() style sheets in document order.
 * onewww_page_sheet_url() gives the address of a linked sheet that has no
 * text yet, or NULL (a <style> block, or a sheet that has its text).
 * onewww_page_sheet_text() gives a linked sheet its text; the text is
 * copied. A sheet without text is left out.
 *
 * onewww_page_layout() applies the styles and draws the page. Returns 0,
 * -1 without memory, or -2 when onewww_pause stopped it; the page is then
 * empty. While the layout runs, the rows and links made so far are valid:
 * a caller that gets control through onewww_pause can show them. */
int onewww_page_parse(const uint8_t *html, size_t length,
                      const char *content_type, const char *base_url,
                      int internal);
unsigned onewww_page_sheets(void);
const char *onewww_page_sheet_url(unsigned sheet);
void onewww_page_sheet_text(unsigned sheet, const char *text, size_t length);
int onewww_page_layout(void);
/* Both steps, without linked sheets. */
int onewww_page_build(const uint8_t *html, size_t length,
                      const char *content_type, const char *base_url,
                      int internal);
void onewww_page_free(void);
uint32_t onewww_page_rows(void);
const char *onewww_page_title(void);

/* Links have numbers 0..count-1 in document order. */
uint32_t onewww_link_count(void);
const char *onewww_link_href(uint32_t link);    /* absolute URL or "#name" */
uint32_t onewww_link_row(uint32_t link);        /* UINT32_MAX: not shown */
int onewww_link_at(int x, uint32_t row);        /* -1: no link there */
int onewww_anchor_row(const char *name);        /* -1: not found */

/* Writes the view that starts at page row `top`: bytes 0..$7FFF are AUX
 * $2000-$9FFF (even screen rows), bytes $8000..$FFFF are MAIN $2000-$9FFF
 * (odd screen rows). Each field has its SCBs, palettes and SHR4 magic;
 * AUX $9DF8 selects interlace. left and right are the status line texts.
 * selected is a link number or -1. pointer_x < 0 hides the pointer. */
void onewww_compose(uint32_t top, const char *left, const char *right,
                    int selected, int pointer_x, int pointer_y,
                    uint8_t *fields);

extern const char onewww_home_page[];

#endif
