#ifndef ONEWWW_COOKIE_H
#define ONEWWW_COOKIE_H

#include <stddef.h>

#include "http_core.h"

/* ONEWWW cookies. The card keeps them in memory while the browser is
 * open: a login stays open from page to page. Nothing goes to the SD
 * card, and a new start of the browser has no cookies.
 *
 * The two functions with a URL are the hooks of http_core: the HTTP
 * engine calls them for each request and each Set-Cookie header, also for
 * the requests of a redirect. */
void onewww_cookie_clear(void);
/* Takes one Set-Cookie value that came from `url`. */
void onewww_cookie_store(const http_url_t *url, const char *set_cookie);
/* Writes the value of the Cookie header for a request to `url`.
 * Returns its length; 0: no cookie. */
size_t onewww_cookie_load(const http_url_t *url, char *out, size_t capacity);
unsigned onewww_cookie_count(void);

#endif
