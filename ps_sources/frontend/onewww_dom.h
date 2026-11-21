#ifndef ONEWWW_DOM_H
#define ONEWWW_DOM_H

#include <stddef.h>
#include <stdint.h>

/* ONEWWW document tree: a tolerant HTML parser for real web pages.
 * All text in the tree is UTF-8 with entities decoded. */

typedef enum {
    OW_TEXT = 0, OW_UNKNOWN,
    OW_HTML, OW_HEAD, OW_BODY, OW_TITLE, OW_SCRIPT, OW_STYLE, OW_META,
    OW_LINK, OW_BASE,
    OW_P, OW_DIV, OW_SPAN, OW_A, OW_B, OW_STRONG, OW_I, OW_EM, OW_U, OW_S,
    OW_CODE, OW_TT, OW_KBD, OW_SAMP, OW_VAR, OW_CITE, OW_PRE, OW_BR, OW_HR,
    OW_H1, OW_H2, OW_H3, OW_H4, OW_H5, OW_H6,
    OW_UL, OW_OL, OW_LI, OW_DL, OW_DT, OW_DD,
    OW_TABLE, OW_THEAD, OW_TBODY, OW_TFOOT, OW_TR, OW_TD, OW_TH, OW_CAPTION,
    OW_COL, OW_COLGROUP,
    OW_IMG, OW_FORM, OW_INPUT, OW_BUTTON, OW_SELECT, OW_OPTION, OW_TEXTAREA,
    OW_LABEL, OW_BLOCKQUOTE, OW_CENTER, OW_SMALL, OW_BIG, OW_SUB, OW_SUP,
    OW_FONT, OW_NAV, OW_HEADER, OW_FOOTER, OW_SECTION, OW_ARTICLE, OW_ASIDE,
    OW_MAIN, OW_FIGURE, OW_FIGCAPTION, OW_ADDRESS, OW_NOSCRIPT, OW_IFRAME,
    OW_SVG, OW_TEMPLATE, OW_WBR, OW_AREA, OW_EMBED, OW_SOURCE, OW_TRACK,
    OW_OBJECT, OW_VIDEO, OW_AUDIO, OW_CANVAS, OW_MAP, OW_DEL, OW_INS,
    OW_STRIKE, OW_MARK, OW_Q, OW_ABBR, OW_DETAILS, OW_SUMMARY, OW_FIELDSET,
    OW_LEGEND, OW_HGROUP, OW_DFN, OW_TAG_COUNT
} onewww_tag_t;

typedef struct onewww_node {
    struct onewww_node *parent, *first, *last, *next;
    const char *text;       /* OW_TEXT: the text. Element: its attributes. */
    uint32_t length;
    uint16_t tag;
} onewww_node_t;

typedef struct onewww_dom onewww_dom_t;

/* Parses a page. content_type can be NULL. Returns NULL without memory. */
onewww_dom_t *onewww_dom_parse(const uint8_t *html, size_t length,
                               const char *content_type);
void onewww_dom_free(onewww_dom_t *dom);
onewww_node_t *onewww_dom_root(onewww_dom_t *dom);
const char *onewww_dom_title(const onewww_dom_t *dom);

/* Copies the decoded value of an attribute. Returns its length, or -1
 * when the element does not have the attribute. */
int onewww_attr(const onewww_node_t *node, const char *name,
                char *out, size_t capacity);

typedef struct {
    const char *name;
    uint16_t code;
    uint8_t legacy;     /* valid without the final semicolon */
} onewww_entity_t;
extern const onewww_entity_t onewww_entities[];
extern const unsigned onewww_entity_count;

#endif
