/******************************************************************************
 * TMP102 Temperature Sensor Driver
 *
 * @file    tmp102.h
 * @brief   Driver for Texas Instruments TMP102 I2C temperature sensor
 *
 * OVERVIEW
 * ========
 * This module provides a simple interface to the TMP102 digital temperature
 * sensor with 12-bit resolution (0.0625°C per LSB). The driver returns
 * temperature in hundredths of degrees Celsius for easy integer math.
 *
 * HARDWARE REQUIREMENTS
 * =====================
 * - TMP102AIDRLR connected to I2C0 bus at 7-bit address 0x48
 * - Operating voltage: 1.4V to 3.6V (typically 3.3V)
 * - Temperature range: -40°C to +125°C
 * - Accuracy: ±0.5°C (-25°C to +85°C), ±3°C (full range)
 *
 * SENSOR SPECIFICATIONS
 * =====================
 * - Resolution: 12-bit (0.0625°C per LSB)
 * - Conversion time: ~26ms typical
 * - Power consumption: 10µA active, 0.5µA shutdown
 * - I2C speed: Up to 3.4 MHz
 *
 * DATA FORMAT
 * ===========
 * Temperature is returned as int16_t in hundredths of degrees Celsius:
 * - 2550 = 25.50°C
 * - -1025 = -10.25°C
 * - 0 = 0.00°C
 *
 * This format allows integer arithmetic while preserving 2 decimal places.
 *
 * DEPENDENCIES
 * ============
 * - i2c.h: I2C communication wrapper (must call i2c_init() first)
 * - Xilinx XIicPs driver (xiicps.h)
 * - Standard C library (stdint.h, stddef.h, stdio.h for formatting)
 *
 * USAGE
 * =====
 * 1. Initialize I2C bus:
 *    ```c
 *    XIicPs i2c_inst;
 *    i2c_init(&i2c_inst);
 *    ```
 *
 * 2. Initialize sensor:
 *    ```c
 *    if (tmp102_init(&i2c_inst) != 0) {
 *        printf("TMP102 init failed\r\n");
 *    }
 *    ```
 *
 * 3. Read temperature:
 *    ```c
 *    tmp102_temp_t temp;
 *    if (tmp102_read_temperature(&i2c_inst, &temp) == 0 && temp.valid) {
 *        printf("Temperature: %d.%02d C\r\n",
 *               temp.centi_c / 100, abs(temp.centi_c % 100));
 *    }
 *    ```
 *
 * EXAMPLE
 * =======
 * ```c
 * #include "i2c.h"
 * #include "tmp102.h"
 * #include <stdio.h>
 *
 * int main(void) {
 *     XIicPs i2c_inst;
 *     tmp102_temp_t temp;
 *     char temp_str[32];
 *
 *     // Initialize I2C
 *     if (i2c_init(&i2c_inst) != 0) {
 *         printf("I2C init failed\r\n");
 *         return -1;
 *     }
 *
 *     // Initialize temperature sensor
 *     if (tmp102_init(&i2c_inst) != 0) {
 *         printf("TMP102 init failed\r\n");
 *         return -1;
 *     }
 *
 *     // Read and display temperature
 *     while (1) {
 *         if (tmp102_read_temperature(&i2c_inst, &temp) == 0) {
 *             if (temp.valid) {
 *                 // Format as string
 *                 tmp102_format(temp_str, sizeof(temp_str), &temp);
 *                 printf("Temperature: %s\r\n", temp_str);
 *
 *                 // Or manual formatting
 *                 int whole = temp.centi_c / 100;
 *                 int frac = abs(temp.centi_c % 100);
 *                 printf("Raw: %d.%02d C\r\n", whole, frac);
 *             } else {
 *                 printf("Temperature invalid\r\n");
 *             }
 *         } else {
 *             printf("Read failed\r\n");
 *         }
 *
 *         // Wait 1 second
 *         usleep(1000000);
 *     }
 *
 *     return 0;
 * }
 * ```
 *
 * ERROR HANDLING
 * ==============
 * - tmp102_init() returns 0 on success, non-zero on I2C error
 * - tmp102_read_temperature() returns 0 on success, non-zero on error
 * - Check temp.valid flag before using temp.centi_c value
 * - Invalid readings may occur if sensor is not connected or I2C fails
 *
 * CONFIGURATION
 * =============
 * The driver configures the sensor for:
 * - Continuous conversion mode
 * - 12-bit resolution
 * - Normal conversion rate (~4 Hz)
 * - No alert/interrupt functionality
 *
 * PERFORMANCE
 * ===========
 * - Initialization: ~5ms (I2C transaction time)
 * - Read operation: ~2ms (I2C read of 2 bytes)
 * - Update rate: ~4 Hz (limited by sensor conversion time)
 *
 * NOTES
 * =====
 * - Temperature readings are always in Celsius
 * - Sensor has built-in thermal mass averaging
 * - No calibration required (factory calibrated)
 * - Extended mode (13-bit) is not enabled by default
 *
 * AUTHOR
 * ======
 * Part of the Appletini One project
 ******************************************************************************/

#ifndef TMP102_H
#define TMP102_H

#include <stdint.h>
#include <stddef.h>
#include "xiicps.h"

/**
 * @brief Temperature data structure
 *
 * @field centi_c  Temperature in hundredths of degrees Celsius
 *                 Example: 2550 = 25.50°C, -1025 = -10.25°C
 * @field valid    Data validity flag (0 = invalid, 1 = valid)
 */
typedef struct {
    int16_t centi_c; /* Temperature in 0.01 degC */
    uint8_t valid;   /* 0 or 1 */
} tmp102_temp_t;

/**
 * @brief Initialize the TMP102 temperature sensor
 *
 * Configures the sensor for continuous conversion mode with 12-bit resolution.
 * Must be called after i2c_init() and before reading temperature.
 *
 * @param i2c  Pointer to initialized XIicPs I2C instance
 * @return     0 on success, non-zero on I2C communication error
 */
int tmp102_init(XIicPs *i2c);

/**
 * @brief Read current temperature from TMP102
 *
 * Reads the temperature register and converts to hundredths of degrees Celsius.
 * Check t->valid before using t->centi_c value.
 *
 * @param i2c  Pointer to initialized XIicPs I2C instance
 * @param t    Pointer to tmp102_temp_t structure to receive temperature
 * @return     0 on success, non-zero on I2C communication error
 */
int tmp102_read_temperature(XIicPs *i2c, tmp102_temp_t *t);

/**
 * @brief Format temperature as human-readable string
 *
 * Converts temperature to string format: "25.50 C" or "-10.25 C"
 *
 * @param dst      Destination buffer for formatted string
 * @param dst_len  Size of destination buffer (recommend >= 16 bytes)
 * @param t        Pointer to tmp102_temp_t structure to format
 *
 * @note If t->valid is 0, outputs "INVALID"
 */
void tmp102_format(char *dst, size_t dst_len, const tmp102_temp_t *t);

#endif
