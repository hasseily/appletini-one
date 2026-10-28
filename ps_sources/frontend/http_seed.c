#include "http_seed.h"
#include "../lib/crc32.h"
#include <string.h>

static void clear(void *data, size_t length)
{
    volatile uint8_t *p = data;
    while (length--) *p++ = 0;
}
int http_seed_path_private(const char *path)
{
    char name[16];
    size_t length = strlen(path);
    while (length && (path[length - 1] == '.' || path[length - 1] == ' ')) --length;
    if (length >= sizeof(name)) return 0;
    for (size_t i = 0; i < length; ++i)
        name[i] = path[i] >= 'a' && path[i] <= 'z' ? (char)(path[i] - 'a' + 'A') : path[i];
    name[length] = 0;
    return !strcmp(name,"/ATNETA.SED") || !strcmp(name,"/ATNETB.SED");
}
static uint32_t digest(const uint8_t *p)
{ return crc32_finish(crc32_update(crc32_init(), p, 64)); }
static uint64_t generation_of(const uint8_t *p)
{
    uint64_t n = 0;
    for (unsigned i = 0; i < 8; ++i) n |= (uint64_t)p[8U + i] << (8U * i);
    return n;
}
void http_seed_encode(uint8_t p[HTTP_SEED_RECORD], uint64_t generation,
                       const uint8_t seed[HTTP_SEED_BYTES])
{
    memcpy(p, "ATSEED01", 8);
    for (unsigned i = 0; i < 8; ++i) p[8U + i] = (uint8_t)(generation >> (8U * i));
    memcpy(p + 16, seed, HTTP_SEED_BYTES);
    uint32_t crc = digest(p);
    for (unsigned i = 0; i < 4; ++i) p[64U + i] = (uint8_t)(crc >> (8U * i));
    memcpy(p + 68, "DONE", 4);
}
static int valid(const uint8_t *p)
{
    uint32_t crc = 0;
    uint8_t nonzero = 0;
    for (unsigned i = 0; i < 4; ++i) crc |= (uint32_t)p[64U + i] << (8U * i);
    for (unsigned i = 16; i < 64; ++i) nonzero |= p[i];
    return !memcmp(p, "ATSEED01", 8) && !memcmp(p + 68, "DONE", 4) &&
           generation_of(p) && nonzero && digest(p) == crc;
}
int http_seed_select(http_seed_read_t read, void *ctx, uint8_t seed[HTTP_SEED_BYTES],
                       uint64_t *generation, unsigned *next_slot)
{
    uint8_t a[HTTP_SEED_RECORD], b[HTTP_SEED_RECORD];
    int result = -1;
    memset(seed, 0, HTTP_SEED_BYTES);
    if (read(ctx, 0, a, sizeof(a)) || read(ctx, 1, b, sizeof(b)) || !valid(a) || !valid(b)) goto done;
    uint64_t ga = generation_of(a), gb = generation_of(b);
    if ((ga == gb && memcmp(a, b, sizeof(a))) ||
        (ga > gb && ga - gb != 1U) || (gb > ga && gb - ga != 1U)) goto done;
    if (ga >= gb) { memcpy(seed, a + 16, HTTP_SEED_BYTES); *generation = ga; *next_slot = 1; }
    else { memcpy(seed, b + 16, HTTP_SEED_BYTES); *generation = gb; *next_slot = 0; }
    if (*generation == UINT64_MAX) { clear(seed, HTTP_SEED_BYTES); goto done; }
    result = 0;
done:
    clear(a, sizeof(a)); clear(b, sizeof(b)); return result;
}
int http_seed_commit(http_seed_read_t read, http_seed_write_t write, void *ctx,
                       unsigned slot, uint64_t generation, const uint8_t next[HTTP_SEED_BYTES])
{
    uint8_t record[HTTP_SEED_RECORD], check[HTTP_SEED_RECORD];
    int result = -1;
    if (slot > 1U || !generation) return -1;
    http_seed_encode(record, generation, next);
    if (write(ctx, slot, record, sizeof(record)) || read(ctx, slot, check, sizeof(check)) ||
        memcmp(record, check, sizeof(record))) goto done;
    result = 0;
done:
    clear(record, sizeof(record)); clear(check, sizeof(check)); return result;
}
