/******************************************************************************
 * Zynq PS I2C Communication Wrapper
 *
 * @file    i2c.h
 * @brief   Simplified I2C interface for PS I2C0 controller
 *
 * OVERVIEW
 * ========
 * This module provides a thin wrapper around the Xilinx XIicPs driver for
 * convenient I2C communication with peripheral devices. It simplifies the
 * initialization and provides blocking read/write functions with automatic
 * bus busy checking.
 *
 * HARDWARE CONFIGURATION
 * ======================
 * - I2C0 controller (PS MIO pins)
 * - Base address: 0xE0004000 (PS I2C0)
 * - Clock speed: 100 kHz (configurable in i2c_init)
 * - 7-bit addressing mode
 *
 * CONNECTED DEVICES (Appletini)
 * ===================================
 * - PCF8563 RTC: 7-bit address 0x51
 * - TMP102 Temperature Sensor: 7-bit address 0x48
 * - AK4493SEQ Audio DAC: 7-bit address 0x10
 *
 * DEPENDENCIES
 * ============
 * - Xilinx XIicPs driver (xiicps.h)
 * - xparameters.h for XPAR_XIICPS_0_BASEADDR
 * - xstatus.h for XST_SUCCESS
 *
 * USAGE
 * =====
 * 1. Initialize I2C controller once at startup:
 *    ```c
 *    XIicPs i2c_inst;
 *    if (i2c_init(&i2c_inst) != 0) {
 *        printf("I2C init failed\r\n");
 *        return -1;
 *    }
 *    ```
 *
 * 2. Use with peripheral drivers:
 *    ```c
 *    #include "i2c.h"
 *    #include "rtc_pcf8563.h"
 *    #include "tmp102.h"
 *    #include "dac_ak4493.h"
 *
 *    XIicPs i2c_inst;
 *
 *    // Initialize I2C bus
 *    i2c_init(&i2c_inst);
 *
 *    // Use peripheral drivers (they all take XIicPs pointer)
 *    rtc_pcf8563_init(&i2c_inst);
 *    tmp102_init(&i2c_inst);
 *    dac_reset_ak4493(&i2c_inst);
 *    ```
 *
 * 3. Direct I2C access (if needed):
 *    ```c
 *    // Write to device
 *    uint8_t write_data[] = {0x00, 0x12, 0x34};  // reg 0x00, values 0x12, 0x34
 *    i2c_write_bytes(&i2c_inst, 0x48, write_data, 3);
 *
 *    // Read from device
 *    uint8_t read_data[2];
 *    i2c_read_bytes(&i2c_inst, 0x48, 0x00, read_data, 2);
 *    ```
 *
 * EXAMPLE
 * =======
 * ```c
 * #include "i2c.h"
 * #include "rtc_pcf8563.h"
 * #include "tmp102.h"
 * #include <stdio.h>
 *
 * int main(void) {
 *     XIicPs i2c_inst;
 *     rtc_pcf8563_time_t rtc_time;
 *     tmp102_temp_t temp;
 *
 *     // Initialize I2C bus (must do this first!)
 *     if (i2c_init(&i2c_inst) != 0) {
 *         printf("ERROR: I2C initialization failed\r\n");
 *         return -1;
 *     }
 *
 *     printf("I2C initialized at 100 kHz\r\n");
 *
 *     // Now initialize all I2C peripherals
 *     if (tmp102_init(&i2c_inst) != 0) {
 *         printf("WARNING: Temperature sensor init failed\r\n");
 *     }
 *
 *     // Read temperature
 *     if (tmp102_read_temperature(&i2c_inst, &temp) == 0 && temp.valid) {
 *         printf("Temperature: %d.%02d C\r\n",
 *                temp.centi_c / 100, abs(temp.centi_c % 100));
 *     }
 *
 *     // Read RTC
 *     if (rtc_pcf8563_read_time(&i2c_inst, &rtc_time) == 0 && rtc_time.valid) {
 *         printf("Date: %04d-%02d-%02d %02d:%02d:%02d\r\n",
 *                rtc_time.year, rtc_time.month, rtc_time.day,
 *                rtc_time.hour, rtc_time.min, rtc_time.sec);
 *     }
 *
 *     return 0;
 * }
 * ```
 *
 * ERROR HANDLING
 * ==============
 * All functions return integer status:
 * - 0: Success
 * - -1: Invalid parameter (NULL pointer, invalid length)
 * - -2: I2C send failed (check device address, bus connection)
 * - -3: Bus busy timeout (1 million retries, ~1 second)
 * - -4: I2C receive failed (for read operations)
 *
 * Common failure causes:
 * - Device not connected or powered off
 * - Wrong I2C address (check 7-bit vs 8-bit addressing)
 * - SDA/SCL lines shorted or no pull-ups
 * - Bus contention (multiple masters)
 *
 * ADDRESSING
 * ==========
 * This driver uses 7-bit I2C addresses (NOT 8-bit with R/W bit).
 * Example: TMP102 datasheet says address is 0x90/0x91 (8-bit)
 *          → Use 0x48 (7-bit) with this driver
 *
 * Conversion: addr7 = addr8 >> 1
 *
 * BLOCKING BEHAVIOR
 * =================
 * All functions are blocking and include bus busy polling with timeout.
 * Maximum wait time: ~1 second per transaction (1 million iterations).
 * For time-critical applications, consider interrupt-driven XIicPs API.
 *
 * CLOCK SPEED
 * ===========
 * Default: 100 kHz (standard mode)
 * To change, modify i2c_init():
 * ```c
 * XIicPs_SetSClk(i2c, 400000);  // 400 kHz (fast mode)
 * ```
 *
 * NOTES
 * =====
 * - Must call i2c_init() before any peripheral driver functions
 * - One XIicPs instance can be shared by all peripheral drivers
 * - Functions are NOT thread-safe (no mutex protection)
 * - No repeated start support (simple write/read only)
 *
 * AUTHOR
 * ======
 * Part of the Appletini One project
 ******************************************************************************/

#ifndef I2C_H
#define I2C_H

#include "xiicps.h"

/**
 * @brief Initialize the Zynq PS I2C0 controller
 *
 * Configures I2C0 with 100 kHz clock speed and prepares for communication.
 * Must be called before any i2c_write_bytes() or i2c_read_bytes() calls.
 *
 * @param i2c  Pointer to uninitialized XIicPs instance
 * @return     0 on success, -1 if config lookup failed, -2 if init failed
 */
int i2c_init(XIicPs *i2c);

/**
 * @brief Write bytes to I2C device
 *
 * Performs a single I2C write transaction. The first byte is typically the
 * register address, followed by data bytes.
 *
 * @param i2c    Pointer to initialized XIicPs instance
 * @param addr7  7-bit I2C device address (NOT 8-bit)
 * @param data   Pointer to data buffer (first byte = register, then data)
 * @param len    Number of bytes to write (including register address byte)
 * @return       0 on success, -1 invalid param, -2 send failed, -3 bus timeout
 *
 * @note Blocks until bus is idle or timeout (1 million iterations)
 */
int i2c_write_bytes(XIicPs *i2c, uint8_t addr7, const uint8_t *data, int len);

/**
 * @brief Read bytes from I2C device register
 *
 * Performs write-then-read sequence: writes register address, then reads data.
 * This is the standard I2C read sequence for most devices.
 *
 * @param i2c    Pointer to initialized XIicPs instance
 * @param addr7  7-bit I2C device address (NOT 8-bit)
 * @param reg    Register address to read from
 * @param data   Pointer to buffer to receive data
 * @param len    Number of bytes to read
 * @return       0 on success, -1 invalid param, -2 reg write failed,
 *               -3 read failed, -4 bus timeout after read
 *
 * @note Blocks until bus is idle or timeout (1 million iterations per phase)
 */
int i2c_read_bytes(XIicPs *i2c, uint8_t addr7, uint8_t reg, uint8_t *data, int len);

#endif
