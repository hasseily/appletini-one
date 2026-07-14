#include "no_slot_clock_control.h"

#include "../lib/common.h"

#ifndef APPLE_DEBUG_BASE
#define APPLE_DEBUG_BASE 0x40000000U
#endif

#define CARD_CTRL_FEATURE_ENABLE_REG (APPLE_DEBUG_BASE + 0x04U)
#define CARD_CTRL_NSC_TIME_LO_REG    (APPLE_DEBUG_BASE + 0x10U)
#define CARD_CTRL_NSC_TIME_HI_REG    (APPLE_DEBUG_BASE + 0x14U)
#define CARD_CTRL_NSC_WRITE_SEQ_REG  (APPLE_DEBUG_BASE + 0x18U)

#define CARD_CTRL_FEATURE_NO_SLOT_CLOCK_BIT (1UL << 0)

static uint32_t g_feature_enable_mask = 0U;
static uint32_t g_seen_write_seq = 0U;

static uint8_t bcd8(uint8_t value)
{
    return (uint8_t)(((value / 10U) << 4) | (value % 10U));
}

static uint8_t from_bcd8(uint8_t value)
{
    return (uint8_t)((value & 0x0FU) + (((value >> 4) & 0x0FU) * 10U));
}

static uint64_t pack_nsc_time(const rtc_pcf8563_time_t *time)
{
    const uint8_t year = (uint8_t)(time->year % 100U);
    const uint8_t wday = (uint8_t)((time->wday % 7U) + 1U);

    return ((uint64_t)bcd8(year) << 56) |
           ((uint64_t)bcd8(time->month) << 48) |
           ((uint64_t)bcd8(time->day) << 40) |
           ((uint64_t)bcd8(wday) << 32) |
           ((uint64_t)bcd8(time->hour) << 24) |
           ((uint64_t)bcd8(time->min) << 16) |
           ((uint64_t)bcd8(time->sec) << 8);
}

static int unpack_nsc_time(uint64_t packed, rtc_pcf8563_time_t *time)
{
    uint8_t raw_hour;
    uint8_t raw_wday;
    uint8_t hour;
    uint16_t year;

    if (time == NULL) {
        return -1;
    }

    raw_hour = (uint8_t)((packed >> 24) & 0xFFU);
    if ((raw_hour & 0x80U) != 0U) {
        hour = from_bcd8((uint8_t)(raw_hour & 0x1FU));
        if (hour == 12U) {
            hour = 0U;
        }
        if ((raw_hour & 0x20U) != 0U) {
            hour = (uint8_t)(hour + 12U);
        }
    } else {
        hour = from_bcd8((uint8_t)(raw_hour & 0x3FU));
    }

    year = (uint16_t)(2000U + from_bcd8((uint8_t)((packed >> 56) & 0xFFU)));
    raw_wday = (uint8_t)((packed >> 32) & 0x07U);

    time->year = year;
    time->month = from_bcd8((uint8_t)((packed >> 48) & 0x1FU));
    time->day = from_bcd8((uint8_t)((packed >> 40) & 0x3FU));
    time->hour = hour;
    time->min = from_bcd8((uint8_t)((packed >> 16) & 0x7FU));
    time->sec = from_bcd8((uint8_t)((packed >> 8) & 0x7FU));
    time->wday = (raw_wday >= 1U && raw_wday <= 7U)
                     ? (uint8_t)(raw_wday - 1U)
                     : rtc_pcf8563_weekday_from_ymd(year, time->month, time->day);
    time->status = 0U;
    time->valid = 0U;

    if (time->month < 1U || time->month > 12U ||
        time->day < 1U || time->day > 31U ||
        time->hour > 23U || time->min > 59U || time->sec > 59U) {
        time->status = RTC_PCF8563_STATUS_BAD_FIELD;
        return -1;
    }

    time->valid = 1U;
    return 0;
}

static void sync_feature_mask_from_hw(void)
{
    g_feature_enable_mask = REG_READ(CARD_CTRL_FEATURE_ENABLE_REG);
}

static void write_feature_mask(uint32_t feature_mask)
{
    g_feature_enable_mask = feature_mask;
    REG_WRITE(CARD_CTRL_FEATURE_ENABLE_REG, g_feature_enable_mask);
    sync_feature_mask_from_hw();
}

void no_slot_clock_control_init(void)
{
    sync_feature_mask_from_hw();
    g_seen_write_seq = REG_READ(CARD_CTRL_NSC_WRITE_SEQ_REG);
}

void no_slot_clock_control_set_enabled(uint8_t enable)
{
    uint32_t feature_mask;

    sync_feature_mask_from_hw();
    feature_mask = g_feature_enable_mask;
    if (enable != 0U) {
        feature_mask |= CARD_CTRL_FEATURE_NO_SLOT_CLOCK_BIT;
    } else {
        feature_mask &= ~CARD_CTRL_FEATURE_NO_SLOT_CLOCK_BIT;
    }
    write_feature_mask(feature_mask);
}

void no_slot_clock_control_publish_rtc(const rtc_pcf8563_time_t *time)
{
    uint64_t packed;

    if (time == NULL || time->valid == 0U) {
        return;
    }

    packed = pack_nsc_time(time);
    REG_WRITE(CARD_CTRL_NSC_TIME_LO_REG, (uint32_t)(packed & 0xFFFFFFFFULL));
    REG_WRITE(CARD_CTRL_NSC_TIME_HI_REG, (uint32_t)(packed >> 32));
}

int no_slot_clock_control_poll_apple_write(XIicPs *i2c, rtc_pcf8563_time_t *rtc)
{
    uint32_t seq;
    uint32_t seq_after;
    uint32_t lo;
    uint32_t hi;
    uint64_t packed;
    rtc_pcf8563_time_t written;
    int rc;

    if (i2c == NULL) {
        return -1;
    }

    seq = REG_READ(CARD_CTRL_NSC_WRITE_SEQ_REG);
    if (seq == g_seen_write_seq) {
        return 0;
    }

    lo = REG_READ(CARD_CTRL_NSC_TIME_LO_REG);
    hi = REG_READ(CARD_CTRL_NSC_TIME_HI_REG);
    seq_after = REG_READ(CARD_CTRL_NSC_WRITE_SEQ_REG);
    if (seq_after != seq) {
        lo = REG_READ(CARD_CTRL_NSC_TIME_LO_REG);
        hi = REG_READ(CARD_CTRL_NSC_TIME_HI_REG);
        seq = seq_after;
    }

    g_seen_write_seq = seq;
    packed = ((uint64_t)hi << 32) | lo;
    if (unpack_nsc_time(packed, &written) != 0) {
        return -1;
    }

    rc = rtc_pcf8563_write_time(i2c, &written);
    if (rc != 0) {
        return rc;
    }

    if (rtc != NULL) {
        *rtc = written;
    }
    no_slot_clock_control_publish_rtc(&written);
    return 1;
}
