#ifndef APPLETINI_NUMBER_PARSE_H
#define APPLETINI_NUMBER_PARSE_H

#include <stddef.h>
#include <stdint.h>

/* Parse an unsigned 32-bit value in decimal or with a 0x/0X hex prefix.
 * Reject signs, spaces, empty values, and overflow. */
static inline int appletini_parse_u32(const char *text, uint32_t *out)
{
    uint32_t base = 10U;
    uint32_t value = 0U;
    uint32_t digits = 0U;

    if (text == NULL || out == NULL || text[0] == '\0') {
        return -1;
    }
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16U;
        text += 2;
    }

    while (*text != '\0') {
        uint32_t digit;

        if (*text >= '0' && *text <= '9') {
            digit = (uint32_t)(*text - '0');
        } else if (*text >= 'a' && *text <= 'f') {
            digit = (uint32_t)(*text - 'a') + 10U;
        } else if (*text >= 'A' && *text <= 'F') {
            digit = (uint32_t)(*text - 'A') + 10U;
        } else {
            return -1;
        }
        if (digit >= base || value > ((UINT32_MAX - digit) / base)) {
            return -1;
        }
        value = (value * base) + digit;
        ++digits;
        ++text;
    }

    if (digits == 0U) {
        return -1;
    }
    *out = value;
    return 0;
}

#endif
