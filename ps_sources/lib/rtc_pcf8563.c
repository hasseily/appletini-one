#include "rtc_pcf8563.h"
#include "i2c.h"
#include <stdio.h>
#include "xstatus.h"

#define RTC_ADDR7 0x51U

#define RTC_REG_CTRL1           0x00U
#define RTC_REG_CTRL2           0x01U
#define RTC_REG_SECONDS         0x02U
#define RTC_REG_MINUTES         0x03U
#define RTC_REG_HOURS           0x04U
#define RTC_REG_DAY             0x05U
#define RTC_REG_WEEKDAY         0x06U
#define RTC_REG_MONTH_CENTURY   0x07U
#define RTC_REG_YEAR            0x08U

static uint8_t bcd_to_bin(uint8_t v)
{
    return (uint8_t)((v & 0x0FU) + (((v >> 4) & 0x0FU) * 10U));
}

static uint8_t bin_to_bcd(uint8_t v)
{
    return (uint8_t)(((v / 10U) << 4) | (v % 10U));
}

uint8_t rtc_pcf8563_weekday_from_ymd(uint16_t y, uint8_t m, uint8_t d)
{
    static const uint8_t t[] = {0U, 3U, 2U, 5U, 0U, 3U, 5U, 1U, 4U, 6U, 2U, 4U};
    if (m < 3U) y--;
    return (uint8_t)((y + y / 4U - y / 100U + y / 400U + t[m - 1U] + d) % 7U);
}

int rtc_pcf8563_read_time(XIicPs *i2c, rtc_pcf8563_time_t *t)
{
    if (!t) return -1;

    uint8_t r[7];
    int rc = i2c_read_bytes(i2c, RTC_ADDR7, RTC_REG_SECONDS, r, 7);
    t->valid = 0U;
    t->status = 0U;
    if (rc != 0) return rc;

    uint8_t vl = (uint8_t)((r[0] >> 7) & 0x01U);
    uint8_t century = (uint8_t)((r[5] >> 7) & 0x01U);
    uint8_t yy = bcd_to_bin(r[6]);

    t->sec   = bcd_to_bin((uint8_t)(r[0] & 0x7FU));
    t->min   = bcd_to_bin((uint8_t)(r[1] & 0x7FU));
    t->hour  = bcd_to_bin((uint8_t)(r[2] & 0x3FU));
    t->day   = bcd_to_bin((uint8_t)(r[3] & 0x3FU));
    t->wday  = (uint8_t)(r[4] & 0x07U);
    t->month = bcd_to_bin((uint8_t)(r[5] & 0x1FU));
    t->year  = (uint16_t)((century ? 1900U : 2000U) + yy);

    if (vl != 0U) {
        t->status |= RTC_PCF8563_STATUS_VOLTAGE_LOW;
    }
    if (t->month < 1U || t->month > 12U || t->day < 1U || t->day > 31U ||
        t->hour > 23U || t->min > 59U || t->sec > 59U || t->wday > 6U) {
        t->status |= RTC_PCF8563_STATUS_BAD_FIELD;
    }
    t->valid = (uint8_t)(t->status == 0U);

    return 0;
}

int rtc_pcf8563_write_time(XIicPs *i2c, const rtc_pcf8563_time_t *t)
{
    if (!t) return -1;

    uint8_t century = (uint8_t)(t->year < 2000U ? 1U : 0U);
    uint8_t yy = (uint8_t)(t->year % 100U);

    uint8_t wr[10];
    wr[0] = RTC_REG_CTRL1;
    wr[1] = 0x00U; /* normal mode */
    wr[2] = 0x00U; /* clear flags, no alarm/timer usage */
    wr[3] = bin_to_bcd((uint8_t)(t->sec % 60U));
    wr[4] = bin_to_bcd((uint8_t)(t->min % 60U));
    wr[5] = bin_to_bcd((uint8_t)(t->hour % 24U));
    wr[6] = bin_to_bcd((uint8_t)t->day);
    wr[7] = (uint8_t)(t->wday & 0x07U);
    wr[8] = (uint8_t)(bin_to_bcd((uint8_t)t->month) | (uint8_t)(century << 7));
    wr[9] = bin_to_bcd(yy);

    return i2c_write_bytes(i2c, RTC_ADDR7, wr, (int)sizeof(wr));
}

int rtc_pcf8563_is_before(const rtc_pcf8563_time_t *t, uint16_t y, uint8_t m, uint8_t d)
{
    if (!t || !t->valid) return 1;
    if (t->year < y) return 1;
    if (t->year > y) return 0;
    if (t->month < m) return 1;
    if (t->month > m) return 0;
    if (t->day < d) return 1;
    return 0;
}

void rtc_pcf8563_format(char *dst, size_t dst_len, const rtc_pcf8563_time_t *t)
{
    if (!dst || dst_len == 0U || !t) return;
    if (!t->valid) {
        snprintf(dst, dst_len, "RTC: invalid");
        return;
    }
    snprintf(dst, dst_len, "RTC GMT %04u-%02u-%02u %02u:%02u:%02u",
             (unsigned)t->year, (unsigned)t->month, (unsigned)t->day,
             (unsigned)t->hour, (unsigned)t->min, (unsigned)t->sec);
}
