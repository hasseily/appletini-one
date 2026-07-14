#include "crc32.h"

uint32_t crc32_init(void)
{
    return 0xFFFFFFFFU;
}

uint32_t crc32_update(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t i;
    size_t j;
    for (i = 0; i < len; ++i) {
        crc ^= (uint32_t)p[i];
        for (j = 0; j < 8U; ++j) {
            const uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1) ^ (0xEDB88320U & mask);
        }
    }
    return crc;
}

uint32_t crc32_finish(uint32_t crc)
{
    return crc ^ 0xFFFFFFFFU;
}
