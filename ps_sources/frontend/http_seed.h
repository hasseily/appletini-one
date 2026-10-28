#ifndef HTTP_SEED_H
#define HTTP_SEED_H
#include <stddef.h>
#include <stdint.h>
#define HTTP_SEED_BYTES 48U
#define HTTP_SEED_RECORD 72U
typedef int (*http_seed_read_t)(void *, unsigned, uint8_t *, size_t);
typedef int (*http_seed_write_t)(void *, unsigned, const uint8_t *, size_t);
int http_seed_path_private(const char *normalized_path);
void http_seed_encode(uint8_t record[HTTP_SEED_RECORD], uint64_t generation,
                       const uint8_t seed[HTTP_SEED_BYTES]);
int http_seed_select(http_seed_read_t read, void *ctx, uint8_t seed[HTTP_SEED_BYTES],
                       uint64_t *generation, unsigned *next_slot);
int http_seed_commit(http_seed_read_t read, http_seed_write_t write, void *ctx,
                       unsigned slot, uint64_t generation,
                       const uint8_t next[HTTP_SEED_BYTES]);
#endif
