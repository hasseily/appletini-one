#ifndef NET_JSON_H
#define NET_JSON_H
#include <stddef.h>
#include <stdint.h>
#define NET_JSON_BYTES 32768U
#define NET_JSON_TOKENS 16385U
#define NET_JSON_PATH 1000U
enum { NJ_NONE, NJ_PENDING, NJ_FOUND, NJ_NO_DOCUMENT, NJ_BAD_DOCUMENT,
       NJ_BAD_PATH, NJ_MISSING };
enum { NJ_STRING = 1, NJ_NUMBER, NJ_BOOL, NJ_NULL, NJ_OBJECT, NJ_ARRAY };
typedef struct { uint16_t start, end, next; uint8_t type; } net_json_token_t;
typedef struct { uint16_t token; uint8_t expect; } net_json_frame_t;
typedef struct {
    net_json_token_t tokens[NET_JSON_TOKENS];
    net_json_frame_t stack[16];
    const uint8_t *source;
    uint32_t unicode, utf8_code, utf8_min;
    uint16_t count, at, current, high, literal_at;
    uint8_t depth, root, bad, complete, lex, escape, digits, utf8_left, number;
    uint8_t result, value_type, lookup_stage, mismatch;
    uint16_t selected, candidate, path_at, part_at, part_length, compare_at, decode_at;
    uint32_t index, array_at;
    uint16_t value_length;
    uint8_t value[NET_JSON_BYTES];
    char path[NET_JSON_PATH + 1U], part[NET_JSON_PATH + 1U];
} net_json_t;
void net_json_init(net_json_t *j, const uint8_t *source);
/* Incremental validation. The caller limits new bytes to its poll budget. */
void net_json_feed(net_json_t *j, size_t available, int final, size_t budget);
void net_json_lookup(net_json_t *j, const uint8_t *path, size_t length, int document);
/* At most budget source bytes or tree nodes, including string decoding. */
void net_json_poll(net_json_t *j, size_t budget);
#endif
