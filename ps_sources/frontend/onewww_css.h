#ifndef ONEWWW_CSS_H
#define ONEWWW_CSS_H

#include <stddef.h>
#include <stdint.h>

#include "onewww_dom.h"

/* ONEWWW style engine: style sheets, selectors and the cascade.
 *
 * Supported selectors: tag, .class, #id, [attr], [attr=value] and the
 * ~= ^= $= *= forms, :first-child, :last-child, :only-child, :empty,
 * :root, :link, :not(simple), and the combinators space, >, + and ~.
 * A selector with another pseudo-class or a pseudo-element is ignored.
 * @media rules are evaluated for a screen of 640 pixels. */

typedef struct onewww_css onewww_css_t;

onewww_css_t *onewww_css_new(void);
void onewww_css_free(onewww_css_t *css);
/* Adds a style sheet. Sheets added later win a tie, as in a page. */
void onewww_css_add(onewww_css_t *css, const char *text, size_t length);
/* Gives each element of the tree its style. css can be NULL: only the
 * built-in rules and the style attributes then apply.
 * Returns 0, or -1 without memory or when onewww_pause asked to stop. */
int onewww_css_resolve(onewww_css_t *css, onewww_dom_t *dom);

typedef struct {
    const char *name;
    uint16_t color;
} onewww_css_color_t;
extern const onewww_css_color_t onewww_css_colors[];
extern const unsigned onewww_css_color_count;

#endif
