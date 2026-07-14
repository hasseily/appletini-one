/******************************************************************************
 * AK4493SEQ Audio DAC Driver
 *
 * @file    dac_ak4493.h
 * @brief   High-level driver for Asahi Kasei AK4493SEQ 32-bit stereo audio DAC
 *
 * OVERVIEW
 * ========
 * This module provides initialization for the AK4493SEQ premium audio DAC,
 * configured for I2S 24-bit stereo audio at 48 kHz sample rate. The DAC
 * features 123 dB SNR and supports PCM rates up to 768 kHz.
 *
 * HARDWARE REQUIREMENTS
 * =====================
 * - AK4493SEQ connected to I2C0 bus at 7-bit address 0x51
 * - External MCLK: 12.288 MHz (256×Fs for 48 kHz)
 * - I2S audio interface: BCLK, LRCK, SDATA signals from PL/PS
 * - Power supplies: AVDD (analog), DVDD (digital), TVDD (interface)
 *
 * CONFIGURATION
 * =============
 * The driver configures the DAC for:
 * - Sample rate: 48 kHz (Normal Speed Mode)
 * - Audio format: I2S 24-bit
 * - External MCLK: 12.288 MHz
 * - Attenuation: 0 dB (full scale)
 * - Auto Clock Recovery enabled
 * - Soft mute disabled after initialization
 *
 * DEPENDENCIES
 * ============
 * - i2c.h: I2C communication wrapper (must call i2c_init() first)
 * - Xilinx XIicPs driver (xiicps.h)
 * - Audio clock generation in PL (provides I2S clocks to DAC)
 *
 * USAGE
 * =====
 * 1. Initialize I2C bus:
 *    ```c
 *    XIicPs i2c_inst;
 *    i2c_init(&i2c_inst);
 *    ```
 *
 * 2. Reset and initialize DAC:
 *    ```c
 *    int status = dac_reset_ak4493(&i2c_inst);
 *    if (status != 0) {
 *        printf("DAC reset failed: %d\r\n", status);
 *    }
 *    ```
 *
 * 3. Start audio streaming from PL:
 *    - DAC will automatically lock to LRCK and start converting
 *    - No further driver interaction needed during playback
 *
 * EXAMPLE
 * =======
 * ```c
 * #include "i2c.h"
 * #include "dac_ak4493.h"
 *
 * int main(void) {
 *     XIicPs i2c_inst;
 *
 *     // Initialize I2C bus (100 kHz)
 *     if (i2c_init(&i2c_inst) != 0) {
 *         printf("I2C init failed\r\n");
 *         return -1;
 *     }
 *
 *     // Reset and initialize audio DAC
 *     if (dac_reset_ak4493(&i2c_inst) != 0) {
 *         printf("DAC reset failed\r\n");
 *         return -1;
 *     }
 *
 *     printf("Audio DAC ready\r\n");
 *
 *     // Enable audio output from PL (e.g., via AXI register)
 *     // Audio will now play through the DAC
 *     return 0;
 * }
 * ```
 *
 * NOTES
 * =====
 * - The DAC is configured once at startup; no runtime adjustments needed
 * - Audio volume control should be implemented in the audio source (PL)
 * - The DAC automatically handles clock synchronization and PLL lock
 * - Soft reset is performed during initialization to ensure clean state
 *
 * AUTHOR
 * ======
 # Rikkles
 * Part of the Appletini project
 ******************************************************************************/

#ifndef DAC_AK4493_H
#define DAC_AK4493_H

#include <xiicps.h>

/**
 * @brief Reset and initialize the AK4493SEQ audio DAC via I2C
 *
 * Configures the DAC for I2S 24-bit audio at 48 kHz with external 12.288 MHz
 * MCLK. Mutes, powers down, asserts soft reset, rewrites all control registers,
 * powers up, releases reset, waits for settling, and unmutes the output.
 *
 * @param i2c  Pointer to initialized XIicPs I2C instance (must call i2c_init first)
 * @return     0 on success, non-zero on I2C communication error
 *
 * @note Call i2c_init() before calling this function
 * @note This function blocks during I2C transactions and DAC settling.
 */
int dac_reset_ak4493(XIicPs *i2c);

#endif
