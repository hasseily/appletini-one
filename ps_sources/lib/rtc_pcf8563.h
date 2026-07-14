/******************************************************************************
 * PCF8563 Real-Time Clock Driver
 *
 * @file    rtc_pcf8563.h
 * @brief   Driver for NXP PCF8563 I2C real-time clock with date/time utilities
 *
 * OVERVIEW
 * ========
 * This module provides a complete interface to the PCF8563 real-time clock,
 * including reading/writing time, date comparison, weekday calculation, and
 * formatted output. The RTC maintains date and time with backup battery support.
 *
 * HARDWARE REQUIREMENTS
 * =====================
 * - PCF8563DTR connected to I2C0 bus at 7-bit address 0x51
 * - Operating voltage: 1.0V to 5.5V (typically 3.3V)
 * - Backup battery: CR2032 or similar (optional, for time retention)
 * - Crystal: 32.768 kHz external (built into PCF8563 module)
 * - Clock output: Configurable (32.768 kHz, 1.024 kHz, 32 Hz, 1 Hz)
 *
 * RTC SPECIFICATIONS
 * ==================
 * - Time accuracy: ±30 ppm (±2.5 seconds/day) at 25°C
 * - Date range: 2000-2099 (century bit not used)
 * - Leap year correction: Automatic (through 2099)
 * - Power consumption: 0.9µA typical (timekeeping only)
 * - Battery backup: Years of operation on CR2032
 *
 * DATA FORMAT
 * ===========
 * All date/time values use human-readable decimal format:
 * - year: Full 4-digit year (2000-2099, e.g., 2026)
 * - month: 1-12 (January = 1, December = 12)
 * - day: 1-31
 * - hour: 0-23 (24-hour format)
 * - min: 0-59
 * - sec: 0-59
 * - wday: 0-6 (Sunday = 0, Monday = 1, ..., Saturday = 6)
 * - valid: 0 = invalid/error, 1 = valid data
 * - status: RTC_PCF8563_STATUS_* reason flags when valid = 0
 *
 * DEPENDENCIES
 * ============
 * - i2c.h: I2C communication wrapper (must call i2c_init() first)
 * - Xilinx XIicPs driver (xiicps.h)
 * - Standard C library (stdint.h, stddef.h, stdio.h for formatting)
 *
 * INITIALIZATION
 * ==============
 * The PCF8563 does not require explicit initialization. After power-on or
 * battery insertion, you should:
 * 1. Check if time is valid (read and check valid flag)
 * 2. If invalid or time < 2000, set the correct time
 * 3. Optionally configure CLKOUT frequency
 *
 * USAGE
 * =====
 * 1. Initialize I2C bus:
 *    ```c
 *    XIicPs i2c_inst;
 *    i2c_init(&i2c_inst);
 *    ```
 *
 * 2. Read current time:
 *    ```c
 *    rtc_pcf8563_time_t now;
 *    if (rtc_pcf8563_read_time(&i2c_inst, &now) == 0 && now.valid) {
 *        printf("Date: %04d-%02d-%02d\r\n", now.year, now.month, now.day);
 *        printf("Time: %02d:%02d:%02d\r\n", now.hour, now.min, now.sec);
 *    }
 *    ```
 *
 * 3. Set time (e.g., 2026-02-11 14:30:00):
 *    ```c
 *    rtc_pcf8563_time_t new_time = {
 *        .year = 2026,
 *        .month = 2,
 *        .day = 11,
 *        .hour = 14,
 *        .min = 30,
 *        .sec = 0,
 *        .wday = rtc_pcf8563_weekday_from_ymd(2026, 2, 11),
 *        .valid = 1
 *    };
 *    rtc_pcf8563_write_time(&i2c_inst, &new_time);
 *    ```
 *
 * EXAMPLE
 * =======
 * ```c
 * #include "i2c.h"
 * #include "rtc_pcf8563.h"
 * #include <stdio.h>
 *
 * int main(void) {
 *     XIicPs i2c_inst;
 *     rtc_pcf8563_time_t now;
 *     char time_str[64];
 *
 *     // Initialize I2C
 *     if (i2c_init(&i2c_inst) != 0) {
 *         printf("I2C init failed\r\n");
 *         return -1;
 *     }
 *
 *     // Read current time
 *     if (rtc_pcf8563_read_time(&i2c_inst, &now) != 0 || !now.valid) {
 *         printf("RTC read failed or invalid\r\n");
 *         return -1;
 *     }
 *
 *     // Check if time needs initialization (before 2020)
 *     if (rtc_pcf8563_is_before(&now, 2020, 1, 1)) {
 *         printf("RTC time is too old, setting to 2026-02-11 12:00:00\r\n");
 *         rtc_pcf8563_time_t new_time = {
 *             .year = 2026, .month = 2, .day = 11,
 *             .hour = 12, .min = 0, .sec = 0,
 *             .wday = rtc_pcf8563_weekday_from_ymd(2026, 2, 11),
 *             .valid = 1
 *         };
 *         rtc_pcf8563_write_time(&i2c_inst, &new_time);
 *         rtc_pcf8563_read_time(&i2c_inst, &now);
 *     }
 *
 *     // Display time in multiple formats
 *     printf("Raw: %04d-%02d-%02d %02d:%02d:%02d (wday=%d)\r\n",
 *            now.year, now.month, now.day,
 *            now.hour, now.min, now.sec, now.wday);
 *
 *     rtc_pcf8563_format(time_str, sizeof(time_str), &now);
 *     printf("Formatted: %s\r\n", time_str);
 *
 *     // Continuous time display
 *     while (1) {
 *         if (rtc_pcf8563_read_time(&i2c_inst, &now) == 0 && now.valid) {
 *             printf("\r%02d:%02d:%02d ", now.hour, now.min, now.sec);
 *         }
 *         usleep(500000);  // Update every 0.5 seconds
 *     }
 *
 *     return 0;
 * }
 * ```
 *
 * WEEKDAY CALCULATION
 * ===================
 * The driver includes automatic weekday calculation using Zeller's congruence.
 * You don't need to manually calculate weekday when setting time:
 * ```c
 * rtc_pcf8563_time_t t;
 * t.year = 2026;
 * t.month = 2;
 * t.day = 11;
 * t.wday = rtc_pcf8563_weekday_from_ymd(2026, 2, 11);  // Auto-calculates
 * ```
 *
 * DATE COMPARISON
 * ===============
 * Check if RTC time is before a specific date:
 * ```c
 * rtc_pcf8563_time_t now;
 * rtc_pcf8563_read_time(&i2c_inst, &now);
 *
 * if (rtc_pcf8563_is_before(&now, 2020, 1, 1)) {
 *     printf("Time is before 2020-01-01, needs initialization\r\n");
 * }
 * ```
 *
 * FORMATTED OUTPUT
 * ================
 * Use rtc_pcf8563_format() for human-readable output:
 * ```c
 * char buf[64];
 * rtc_pcf8563_format(buf, sizeof(buf), &time);
 * // Result: "2026-02-11 Wed 14:30:25"
 * ```
 *
 * ERROR HANDLING
 * ==============
 * - Functions return 0 on success, non-zero on I2C error
 * - Always check the valid flag before using time data
 * - Invalid time may indicate: power-on reset, no battery, I2C error
 * - If valid=0, all time fields should be considered unreliable
 *
 * BCD CONVERSION
 * ==============
 * The PCF8563 uses BCD (Binary-Coded Decimal) format internally, but this
 * driver handles all conversions automatically. All API functions use normal
 * decimal integers.
 *
 * CLOCK OUTPUT
 * ============
 * The PCF8563 can output a clock signal on the CLKOUT pin (connected to
 * i2c0_rtc_clkout in the FPGA). Configure using register 0x0D:
 * - 0x00: 32.768 kHz
 * - 0x01: 1.024 kHz
 * - 0x02: 32 Hz
 * - 0x03: 1 Hz
 * - 0x80: CLKOUT disabled
 *
 * PERFORMANCE
 * ===========
 * - Read time: ~3ms (I2C transaction of 7 bytes)
 * - Write time: ~4ms (I2C transaction of 8 bytes)
 * - Time accuracy: ±2.5 seconds per day at 25°C
 * - Battery life: 5-10 years on CR2032 (timekeeping only)
 *
 * NOTES
 * =====
 * - RTC maintains time even when PS is powered off (with backup battery)
 * - Leap years are automatically handled through 2099
 * - Century bit is not used (assumes 2000-2099 range)
 * - No alarm or timer functions implemented in this driver
 * - I2C address 0x51 is shared with AK4493 DAC (different registers)
 *
 * AUTHOR
 * ======
 * Part of the Appletini One project
 ******************************************************************************/

#ifndef RTC_PCF8563_H
#define RTC_PCF8563_H

#include <stdint.h>
#include <stddef.h>
#include "xiicps.h"

#define RTC_PCF8563_STATUS_VOLTAGE_LOW (1U << 0)
#define RTC_PCF8563_STATUS_BAD_FIELD   (1U << 1)

/**
 * @brief Date and time structure
 *
 * All fields use normal decimal format (not BCD).
 *
 * @field year   Full 4-digit year (2000-2099, e.g. 2026)
 * @field month  Month (1-12, January=1)
 * @field day    Day of month (1-31)
 * @field hour   Hour (0-23, 24-hour format)
 * @field min    Minute (0-59)
 * @field sec    Second (0-59)
 * @field wday   Day of week (0-6, Sunday=0, Monday=1, ..., Saturday=6)
 * @field valid  Data validity flag (0=invalid, 1=valid)
 * @field status Status flags explaining invalid data
 */
typedef struct {
    uint16_t year; /* full year, e.g. 2026 */
    uint8_t month; /* 1..12 */
    uint8_t day;   /* 1..31 */
    uint8_t hour;  /* 0..23 */
    uint8_t min;   /* 0..59 */
    uint8_t sec;   /* 0..59 */
    uint8_t wday;  /* 0..6, Sunday = 0 */
    uint8_t valid; /* 0 or 1 */
    uint8_t status; /* RTC_PCF8563_STATUS_* */
} rtc_pcf8563_time_t;

/**
 * @brief Read current date and time from RTC
 *
 * Reads all time/date registers and converts from BCD to decimal format.
 * Always check t->valid before using the returned time.
 *
 * @param i2c  Pointer to initialized XIicPs I2C instance
 * @param t    Pointer to rtc_pcf8563_time_t structure to receive time
 * @return     0 on success, non-zero on I2C communication error
 *
 * @note If voltage-low flag is set in RTC, valid will be 0 and status will
 *       include RTC_PCF8563_STATUS_VOLTAGE_LOW
 */
int rtc_pcf8563_read_time(XIicPs *i2c, rtc_pcf8563_time_t *t);

/**
 * @brief Write date and time to RTC
 *
 * Sets the RTC to the specified date and time. Automatically converts from
 * decimal to BCD format.
 *
 * @param i2c  Pointer to initialized XIicPs I2C instance
 * @param t    Pointer to rtc_pcf8563_time_t structure with time to write
 * @return     0 on success, non-zero on I2C communication error
 *
 * @note Use rtc_pcf8563_weekday_from_ymd() to calculate wday before calling
 * @note Clears voltage-low flag in RTC after successful write
 */
int rtc_pcf8563_write_time(XIicPs *i2c, const rtc_pcf8563_time_t *t);

/**
 * @brief Check if RTC time is before a specific date
 *
 * Compares RTC time against a reference date (year-month-day only).
 * Useful for detecting uninitialized or stale RTC time.
 *
 * @param t  Pointer to rtc_pcf8563_time_t structure to compare
 * @param y  Reference year (e.g., 2020)
 * @param m  Reference month (1-12)
 * @param d  Reference day (1-31)
 * @return   1 if t is before the reference date, 0 otherwise
 *
 * @note Only compares dates, ignores time of day
 */
int rtc_pcf8563_is_before(const rtc_pcf8563_time_t *t, uint16_t y, uint8_t m, uint8_t d);

/**
 * @brief Calculate day of week from year/month/day
 *
 * Uses Zeller's congruence algorithm to compute weekday.
 * Valid for Gregorian calendar dates.
 *
 * @param y  Year (e.g., 2026)
 * @param m  Month (1-12)
 * @param d  Day (1-31)
 * @return   Day of week (0-6, Sunday=0, Monday=1, ..., Saturday=6)
 *
 * @note Always use this when setting RTC time to ensure correct weekday
 */
uint8_t rtc_pcf8563_weekday_from_ymd(uint16_t y, uint8_t m, uint8_t d);

/**
 * @brief Format date and time as human-readable string
 *
 * Converts time to string format: "2026-02-11 Wed 14:30:25"
 *
 * @param dst      Destination buffer for formatted string
 * @param dst_len  Size of destination buffer (recommend >= 32 bytes)
 * @param t        Pointer to rtc_pcf8563_time_t structure to format
 *
 * @note If t->valid is 0, outputs "INVALID"
 * @note Weekday names: Sun, Mon, Tue, Wed, Thu, Fri, Sat
 */
void rtc_pcf8563_format(char *dst, size_t dst_len, const rtc_pcf8563_time_t *t);

#endif
