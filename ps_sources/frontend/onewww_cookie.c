/* ONEWWW cookies. See onewww_cookie.h. No hardware access. */
#include "onewww_cookie.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define COOKIES     96U
#define NAME_MAX    96U
#define VALUE_MAX   2048U
#define DOMAIN_MAX  128U
#define PATH_MAX_   96U

typedef struct {
    char *name, *value;
    char domain[DOMAIN_MAX];
    char path[PATH_MAX_];
    uint8_t host_only, secure;
    uint32_t stamp;
} cookie_t;

static cookie_t s_cookie[COOKIES];
static uint32_t s_stamp;

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static void cookie_free(cookie_t *c)
{
    free(c->name); free(c->value);
    memset(c, 0, sizeof(*c));
}

void onewww_cookie_clear(void)
{
    for (unsigned i = 0; i < COOKIES; i++) cookie_free(&s_cookie[i]);
}

unsigned onewww_cookie_count(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < COOKIES; i++) n += s_cookie[i].name != NULL;
    return n;
}

static char *copy(const char *p, size_t n)
{
    char *out = malloc(n + 1U);
    if (out != NULL) { memcpy(out, p, n); out[n] = '\0'; }
    return out;
}

/* Does `host` belong to `domain` (the same name, or a name below it)? */
static int domain_match(const char *host, const char *domain)
{
    const size_t h = strlen(host), d = strlen(domain);

    if (d == 0U || h < d || strcmp(host + h - d, domain) != 0) return 0;
    return h == d || host[h - d - 1U] == '.';
}

static int word_is(const char *p, size_t n, const char *word)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (word[i] == '\0' || lower((unsigned char)p[i]) != word[i]) return 0;
    }
    return word[n] == '\0';
}

void onewww_cookie_store(const http_url_t *url, const char *text)
{
    const char *p = text, *end, *eq;
    char domain[DOMAIN_MAX], path[PATH_MAX_];
    size_t name_n, value_n;
    int host_only = 1, secure = 0, remove = 0;
    cookie_t *slot = NULL, *oldest = &s_cookie[0];

    if (url == NULL || text == NULL) return;
    while (*p == ' ') p++;
    end = strchr(p, ';');
    if (end == NULL) end = p + strlen(p);
    eq = memchr(p, '=', (size_t)(end - p));
    if (eq == NULL || eq == p) return;
    name_n = (size_t)(eq - p);
    value_n = (size_t)(end - eq - 1);
    while (name_n != 0U && p[name_n - 1U] == ' ') name_n--;
    if (name_n == 0U || name_n >= NAME_MAX || value_n >= VALUE_MAX) return;

    strncpy(domain, url->host, sizeof(domain) - 1U);
    domain[sizeof(domain) - 1U] = '\0';
    strcpy(path, "/");
    for (const char *a = end; *a == ';';) {         /* the attributes */
        const char *stop, *sep, *v;
        size_t key_n, v_n;
        a++;
        while (*a == ' ') a++;
        stop = strchr(a, ';');
        if (stop == NULL) stop = a + strlen(a);
        sep = memchr(a, '=', (size_t)(stop - a));
        key_n = (size_t)((sep != NULL ? sep : stop) - a);
        v = sep != NULL ? sep + 1 : stop;
        v_n = (size_t)(stop - v);
        if (word_is(a, key_n, "secure")) secure = 1;
        else if (word_is(a, key_n, "domain") && v_n != 0U) {
            if (*v == '.') { v++; v_n--; }
            if (v_n != 0U && v_n < sizeof(domain)) {
                for (size_t i = 0; i < v_n; i++) domain[i] = (char)lower((unsigned char)v[i]);
                domain[v_n] = '\0';
                host_only = 0;
            }
        } else if (word_is(a, key_n, "path") && v_n != 0U && v_n < sizeof(path) && *v == '/') {
            memcpy(path, v, v_n); path[v_n] = '\0';
        } else if (word_is(a, key_n, "max-age") && v_n != 0U && (*v == '-' || *v == '0')) {
            remove = 1;
        } else if (word_is(a, key_n, "expires") && v_n > 15U) {
            /* A date in the last century removes the cookie. Other dates
             * are not read: all cookies end with the browser. */
            for (size_t i = 0; i + 4U <= v_n; i++) {
                if (v[i] == '1' && v[i + 1U] == '9' && v[i + 2U] >= '0' && v[i + 2U] <= '9' &&
                    v[i + 3U] >= '0' && v[i + 3U] <= '9' && (i == 0U || v[i - 1U] < '0' || v[i - 1U] > '9')) {
                    remove = 1;
                }
            }
        }
        a = stop;
    }
    /* A site can set a cookie only for its own name or a name above it,
     * and not for a whole top-level name such as "com". */
    if (!host_only && (!domain_match(url->host, domain) || strchr(domain, '.') == NULL)) return;

    for (unsigned i = 0; i < COOKIES; i++) {
        cookie_t *c = &s_cookie[i];
        if (c->name == NULL) { if (slot == NULL) slot = c; continue; }
        if (c->stamp < oldest->stamp || oldest->name == NULL) oldest = c;
        if (strlen(c->name) == name_n && memcmp(c->name, p, name_n) == 0 &&
            strcmp(c->domain, domain) == 0 && strcmp(c->path, path) == 0) {
            cookie_free(c);
            slot = c;
            break;
        }
    }
    if (remove) return;
    if (slot == NULL) { cookie_free(oldest); slot = oldest; }
    slot->name = copy(p, name_n);
    slot->value = copy(eq + 1, value_n);
    if (slot->name == NULL || slot->value == NULL) { cookie_free(slot); return; }
    strcpy(slot->domain, domain);
    strcpy(slot->path, path);
    slot->host_only = (uint8_t)host_only; slot->secure = (uint8_t)secure;
    slot->stamp = ++s_stamp;
}

size_t onewww_cookie_load(const http_url_t *url, char *out, size_t capacity)
{
    size_t used = 0;

    if (url == NULL || capacity == 0U) return 0U;
    out[0] = '\0';
    for (unsigned i = 0; i < COOKIES; i++) {
        const cookie_t *c = &s_cookie[i];
        size_t path_n, need;
        if (c->name == NULL || (c->secure && !url->tls)) continue;
        if (c->host_only ? strcmp(url->host, c->domain) != 0 :
                           !domain_match(url->host, c->domain)) continue;
        path_n = strlen(c->path);
        if (strncmp(url->path, c->path, path_n) != 0) continue;
        if (path_n > 1U && c->path[path_n - 1U] != '/' && url->path[path_n] != '\0' &&
            url->path[path_n] != '/' && url->path[path_n] != '?') continue;
        need = strlen(c->name) + 1U + strlen(c->value) + (used != 0U ? 2U : 0U);
        if (used + need + 1U > capacity) continue;
        if (used != 0U) { memcpy(out + used, "; ", 2U); used += 2U; }
        memcpy(out + used, c->name, strlen(c->name)); used += strlen(c->name);
        out[used++] = '=';
        memcpy(out + used, c->value, strlen(c->value)); used += strlen(c->value);
        out[used] = '\0';
    }
    return used;
}
