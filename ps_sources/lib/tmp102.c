#include "tmp102.h"

#include <stdio.h>
#include "xstatus.h"
#include "i2c.h"

#define TMP102_ADDR7 0x48U
#define TMP102_REG_TEMP   0x00U
#define TMP102_REG_CONFIG 0x01U

static int i2c_wait_idle(XIicPs *i2c)
{
    for (int i = 0; i < 1000000; i++) {
        if (!XIicPs_BusIsBusy(i2c))
            return 0;
    }
    return -1;
}

int tmp102_init(XIicPs *i2c)
{
    uint8_t cfg[2];
    int rc = i2c_read_bytes(i2c, TMP102_ADDR7, TMP102_REG_CONFIG, cfg, 2);
    if (rc != 0) return rc;
    return 0;
}

int tmp102_read_temperature(XIicPs *i2c, tmp102_temp_t *t)
{
    if (!t) return -1;

    uint8_t raw[2];
    int rc = i2c_read_bytes(i2c, TMP102_ADDR7, TMP102_REG_TEMP, raw, 2);
    if (rc != 0) {
        t->valid = 0U;
        return rc;
    }

    /* TMP102 default mode: 12-bit two's-complement, bits [15:4], 0.0625 C/LSB */
    int16_t raw16 = (int16_t)(((uint16_t)raw[0] << 8) | raw[1]);
    int16_t temp12 = (int16_t)(raw16 >> 4);
    if (temp12 & 0x0800) {
        temp12 = (int16_t)(temp12 | (int16_t)0xF000);
    }

    /* centi_c = temp12 * 6.25 => exact integer form temp12 * 625 / 100 */
    int32_t centi = ((int32_t)temp12 * 625) / 100;
    t->centi_c = (int16_t)centi;
    t->valid = 1U;
    return 0;
}

void tmp102_format(char *dst, size_t dst_len, const tmp102_temp_t *t)
{
    if (!dst || dst_len == 0U) return;
    if (!t || !t->valid) {
        snprintf(dst, dst_len, "TMP102: read error");
        return;
    }

    int32_t centi = t->centi_c;
    char sign = '+';
    if (centi < 0) {
        sign = '-';
        centi = -centi;
    }
    snprintf(dst, dst_len, "Temp: %c%ld.%02ld C",
             sign, (long)(centi / 100), (long)(centi % 100));
}
