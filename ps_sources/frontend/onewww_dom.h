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

#define OW_NO_COLOR 0xFFFFU

enum { OW_D_INLINE = 0, OW_D_BLOCK, OW_D_NONE, OW_D_LIST_ITEM, OW_D_FLEX };
enum { OW_FLOAT_NONE = 0, OW_FLOAT_LEFT, OW_FLOAT_RIGHT };
enum { OW_SIDE_TOP = 1, OW_SIDE_RIGHT = 2, OW_SIDE_BOTTOM = 4, OW_SIDE_LEFT = 8 };
enum { OW_ALIGN_LEFT = 0, OW_ALIGN_CENTER, OW_ALIGN_RIGHT };

/* The style of an element after the cascade. Lengths are screen pixels. */
typedef struct onewww_style {
    uint16_t color;             /* text, 0RGB */
    uint16_t background;        /* 0RGB, or OW_NO_COLOR */
    uint8_t display;
    uint8_t font_px;            /* CSS pixels; 16 is the normal text size */
    uint8_t bold, italic, mono, underline, pre, nowrap, align, list_none;
    int16_t margin_top, margin_bottom, margin_left, margin_right;
    int16_t padding_top, padding_bottom, padding_left, padding_right;
    /* Pixels; a negative value is a percentage of the space; 0: not set. */
    int16_t width, max_width;
    uint8_t floating;           /* OW_FLOAT_* */
    uint8_t clear;              /* OW_SIDE_LEFT and OW_SIDE_RIGHT */
    uint8_t border;             /* OW_SIDE_*: sides with a visible border */
    uint8_t margin_auto;        /* OW_SIDE_LEFT and OW_SIDE_RIGHT */
    uint8_t flex_column;        /* a flex box that puts its items in a column */
} onewww_style_t;

typedef struct onewww_node {
    struct onewww_node *parent, *first, *last, *next, *prev;
    const char *text;       /* OW_TEXT: the text. Element: its attributes. */
    const char *id;         /* element: the id attribute, or "" */
    const char *classes;    /* element: the class attribute, or "" */
    onewww_style_t *style;  /* element: set by onewww_css_resolve() */
    uint32_t length;
    uint16_t tag;
} onewww_node_t;

typedef struct onewww_dom onewww_dom_t;

/* Optional. The parser and the layout call it often. It can give the
 * processor to other work for a time. A return value that is not zero
 * stops the page: the parser returns NULL, the layout returns -2. */
extern int (*onewww_pause)(void);

/* Parses a page. content_type can be NULL. Returns NULL without memory. */
onewww_dom_t *onewww_dom_parse(const uint8_t *html, size_t length,
                               const char *content_type);
void onewww_dom_free(onewww_dom_t *dom);
/* The root is the <html> element. Its one child is <body>, which holds
 * all the content (also what the page had in <head>). */
onewww_node_t *onewww_dom_root(onewww_dom_t *dom);
/* Memory that lives as long as the tree. NULL without memory. */
void *onewww_dom_alloc(onewww_dom_t *dom, size_t bytes);
const char *onewww_dom_title(const onewww_dom_t *dom);

/* The tag number of a name in any letter case, or OW_UNKNOWN. */
uint16_t onewww_tag_find(const char *name, size_t length);

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
