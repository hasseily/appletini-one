#include "cogs_catalog.h"

#include <ctype.h>
#include <string.h>

#include "net_json.h"

#define COGS_BLOCK_BYTES 512U
#define COGS_MAX_BLOCKS 65535U
#define COGS_2IMG_HEADER 64U

/* Keep the shared network JSON validator off the ARM stack. */
static net_json_t catalog_json;

enum {
    FIELD_TITLE,
    FIELD_BUNDLE,
    FIELD_URL,
    FIELD_BYTES,
    FIELD_BLOCKS,
    FIELD_CAT,
    FIELD_HOST,
    FIELD_COUNT
};

static unsigned hex_digit(uint8_t c)
{
    return c <= '9' ? (unsigned)(c - '0') :
        ((unsigned)c | 0x20U) - 'a' + 10U;
}

/* The JSON validator has already checked escape syntax and UTF-8. UI labels
 * use printable ASCII, so substitute unsupported characters before truncating. */
static int read_string(unsigned index, char *out, size_t capacity, int label)
{
    const net_json_token_t *token = &catalog_json.tokens[index];
    const uint8_t *source = catalog_json.source;
    size_t used = 0U;

    if ((token->type & 0x7FU) != NJ_STRING) {
        return 0;
    }
    for (unsigned at = token->start + 1U; at < token->end - 1U; ++at) {
        unsigned c = source[at];
        if (c == '\\') {
            c = source[++at];
            if (c == 'u') {
                c = 0U;
                for (unsigned i = 0U; i < 4U; ++i) {
                    c = (c << 4) | hex_digit(source[++at]);
                }
                if (c >= 0xD800U && c <= 0xDBFFU) {
                    at += 6U;
                }
            } else if (c != '"' && c != '\\' && c != '/') {
                c = 0U;
            }
        } else if (c >= 128U) {
            while ((source[at + 1U] & 0xC0U) == 0x80U) {
                ++at;
            }
        }
        if (c < 32U || c > 126U) {
            if (!label) {
                return 0;
            }
            c = '?';
        }
        if (used + 1U == capacity) {
            if (!label) {
                return 0;
            }
            break;
        }
        out[used++] = (char)c;
    }
    out[used] = '\0';
    return 1;
}

static int read_unsigned(unsigned index, uint32_t *out)
{
    const net_json_token_t *token = &catalog_json.tokens[index];
    uint32_t number = 0U;

    if (token->type != NJ_NUMBER) {
        return 0;
    }
    for (unsigned at = token->start; at < token->end; ++at) {
        unsigned digit = (unsigned)(catalog_json.source[at] - '0');
        if (digit > 9U || number > (UINT32_MAX - digit) / 10U) {
            return 0;
        }
        number = number * 10U + digit;
    }
    *out = number;
    return 1;
}

static int path_has_extension(const char *path, const char *extension)
{
    size_t length = strcspn(path, "?");
    size_t suffix = strlen(extension);
    if (length <= suffix || path[length - suffix - 1U] == '/') {
        return 0;
    }
    for (size_t i = 0U; i < suffix; ++i) {
        if (tolower((unsigned char)path[length - suffix + i]) != extension[i]) {
            return 0;
        }
    }
    return 1;
}

static int read_image(unsigned url_field, unsigned bytes_field, unsigned blocks_field,
                      cogs_catalog_entry_t *entry)
{
    http_url_t url;
    uint32_t blocks = 0U;
    uint32_t bytes = 0U;

    if (!read_string(url_field, entry->url, sizeof(entry->url), 0) ||
        strchr(entry->url, '#') || http_url_parse(entry->url, &url) || !url.tls) {
        return 0;
    }
    const int wrapped = path_has_extension(url.path, ".2mg");
    if (!wrapped && !path_has_extension(url.path, ".po") &&
        !path_has_extension(url.path, ".hdv")) {
        return 0;
    }
    if (blocks_field != 0U && (!read_unsigned(blocks_field, &blocks) ||
        blocks == 0U || blocks > COGS_MAX_BLOCKS)) {
        return 0;
    }
    const uint32_t block_bytes = blocks * COGS_BLOCK_BYTES;
    const uint32_t header = wrapped ? COGS_2IMG_HEADER : 0U;
    if (bytes_field != 0U) {
        if (!read_unsigned(bytes_field, &bytes)) {
            return 0;
        }
    } else {
        bytes = block_bytes + header;
    }
    if (bytes < COGS_BLOCK_BYTES + header || bytes > COGS_MAX_IMAGE_BYTES) {
        return 0;
    }
    if (wrapped ? bytes < block_bytes + header :
        (bytes % COGS_BLOCK_BYTES != 0U ||
         bytes / COGS_BLOCK_BYTES > COGS_MAX_BLOCKS ||
         (blocks != 0U && bytes != block_bytes))) {
        return 0;
    }
    entry->bytes = bytes;
    return 1;
}

static int read_entry(unsigned object, cogs_catalog_entry_t *entry)
{
    static const char *const keys[FIELD_COUNT] = {
        "name", "in", "url", "bytes", "blocks", "cat", "host"
    };
    unsigned fields[FIELD_COUNT] = {0};
    char category[2], host[4];

    if (catalog_json.tokens[object].type != NJ_OBJECT) {
        return -1;
    }
    for (unsigned at = object + 1U; at < catalog_json.tokens[object].next;) {
        unsigned value = at + 1U;
        char key[8];
        if (read_string(at, key, sizeof(key), 0)) {
            for (unsigned field = 0U; field < FIELD_COUNT; ++field) {
                if (strcmp(key, keys[field]) == 0) {
                    if (fields[field] != 0U) {
                        return -1;
                    }
                    fields[field] = value;
                    break;
                }
            }
        }
        at = catalog_json.tokens[value].next;
    }

    if (fields[FIELD_BUNDLE]) {
        const net_json_token_t *bundle = &catalog_json.tokens[fields[FIELD_BUNDLE]];
        /* A nonempty "in" marks a member. The collection disk has its own row. */
        if (bundle->type != NJ_STRING || bundle->end != bundle->start + 2U) {
            return 0;
        }
    }
    if (!fields[FIELD_CAT] || !read_string(fields[FIELD_CAT], category, sizeof(category), 0) ||
        strcmp(category, "G") != 0 || !fields[FIELD_HOST] ||
        !read_string(fields[FIELD_HOST], host, sizeof(host), 0) || strcmp(host, "any") != 0 ||
        !fields[FIELD_TITLE] || !read_string(fields[FIELD_TITLE], entry->title, sizeof(entry->title), 1) ||
        !entry->title[0] || !fields[FIELD_URL]) {
        return 0;
    }
    return read_image(fields[FIELD_URL], fields[FIELD_BYTES], fields[FIELD_BLOCKS], entry);
}

int cogs_catalog_parse(const uint8_t *json, size_t length, cogs_catalog_page_t *out)
{
    uint32_t pages = 0U;
    unsigned volumes = 0U;
    unsigned rows = 0U;
    uint8_t count = 0U;

    if (!out) {
        return -1;
    }
    out->pages = 0U;
    out->count = 0U;
    if (!json || length == 0U || length > NET_JSON_BYTES) {
        return -1;
    }
    net_json_init(&catalog_json, json);
    net_json_feed(&catalog_json, length, 1, length);
    if (!catalog_json.complete || catalog_json.bad ||
        catalog_json.tokens[0].type != NJ_OBJECT) {
        return -1;
    }
    for (unsigned at = 1U; at < catalog_json.tokens[0].next;) {
        unsigned value = at + 1U;
        char key[8];
        if (read_string(at, key, sizeof(key), 0)) {
            if (strcmp(key, "pages") == 0) {
                if (pages || !read_unsigned(value, &pages) || pages == 0U || pages > UINT16_MAX) {
                    return -1;
                }
            } else if (strcmp(key, "volumes") == 0) {
                if (volumes || catalog_json.tokens[value].type != NJ_ARRAY) {
                    return -1;
                }
                volumes = value;
            }
        }
        at = catalog_json.tokens[value].next;
    }
    if (pages == 0U || volumes == 0U) {
        return -1;
    }
    for (unsigned at = volumes + 1U; at < catalog_json.tokens[volumes].next;
         at = catalog_json.tokens[at].next) {
        if (++rows > COGS_PROVIDER_PAGE_CAPACITY) {
            return -1;
        }
        const int result = read_entry(at, &out->entries[count]);
        if (result < 0) {
            return -1;
        }
        count += (uint8_t)result;
    }
    out->pages = (uint16_t)pages;
    out->count = count;
    return 0;
}
