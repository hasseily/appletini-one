#include "dac_ak4493.h"
#include "xstatus.h"
#include <stdio.h>
#include "i2c.h"
#include "sleep.h"

#define DAC_ADDR7 0x10U

/*==========================================================================
 * I2C0 (PS) — DAC control
 *==========================================================================*/

#define I2C0_BASE           0xE0004000
#define I2C_CR              (I2C0_BASE + 0x00)
#define I2C_SR              (I2C0_BASE + 0x04)
#define I2C_ADDR            (I2C0_BASE + 0x08)
#define I2C_DATA            (I2C0_BASE + 0x0C)
#define I2C_ISR             (I2C0_BASE + 0x10)
#define I2C_TX_SIZE         (I2C0_BASE + 0x14)

/* CR bits */
#define I2C_CR_DIV_A_MASK   (0x3FU << 14)
#define I2C_CR_DIV_B_MASK   (0x3FU << 8)
#define I2C_CR_MS           (1U << 5)
#define I2C_CR_CLR_FIFO     (1U << 6)
#define I2C_CR_HOLD         (1U << 4)
#define I2C_CR_ACKEN        (1U << 3)
#define I2C_CR_EN           (1U << 0)

/* SR bits */
#define I2C_SR_BA           (1U << 8)
#define I2C_SR_RXDV         (1U << 5)
#define I2C_SR_TXDV         (1U << 6)

/* ISR bits */
#define I2C_ISR_COMP        (1U << 0)
#define I2C_ISR_NACK        (1U << 2)
#define I2C_ISR_ARB_LOST    (1U << 9)

static int dac_write_reg(XIicPs *i2c, uint8_t reg, uint8_t value)
{
    const uint8_t payload[2] = {reg, value};
    return i2c_write_bytes(i2c, DAC_ADDR7, payload, 2);
}

int dac_reset_ak4493(XIicPs *i2c)
{
    /* AK4493SEQ init: 48 kHz, I2S 24-bit, external MCLK 12.288 MHz */
    /* Control 1: ACKS=1, EXDF=0, ECS=0, DIF=011 (I2S 24-bit), RSTN=0/1 */
    const uint8_t CTRL1_RST0 = 0x86;
    const uint8_t CTRL1_RST1 = 0x87;
    /* Control 2: DZFE=0, DZFM=0, SD=1, DFS=00, DEM=00, SMUTE=1/0 */
    const uint8_t CTRL2_MUTE   = 0x21;
    const uint8_t CTRL2_UNMUTE = 0x20;
    /* Control 6: PW=1 powers the analog/digital blocks; PW=0 leaves output Hi-Z. */
    const uint8_t CTRL6_POWER_OFF = 0x00;
    const uint8_t CTRL6_POWER_ON = 0x04;

    const uint8_t reset_seq[][2] = {
        {0x00, CTRL1_RST0},   /* Control 1: reset asserted */
        {0x01, CTRL2_MUTE},   /* Control 2: soft mute on */
        {0x02, 0x00},         /* Control 3 */
        {0x03, 0xFF},         /* LCH ATT: 0 dB */
        {0x04, 0xFF},         /* RCH ATT: 0 dB */
        {0x05, 0x00},         /* Control 4 */
        {0x06, 0x00},         /* DSD1 */
        {0x07, 0x00},         /* Control 5 */
        {0x08, 0x00},         /* Sound Control */
        {0x09, 0x00},         /* DSD2 */
        {0x0A, CTRL6_POWER_ON}, /* Control 6: power on */
        {0x0B, 0x00},         /* Control 7 (TEST=0) */
        {0x15, 0x00}          /* Control 8 */
    };

    int rc = dac_write_reg(i2c, 0x01, CTRL2_MUTE);
    if (rc != 0) return rc;

    rc = dac_write_reg(i2c, 0x0A, CTRL6_POWER_OFF);
    if (rc != 0) return rc;

    rc = dac_write_reg(i2c, 0x00, CTRL1_RST0);
    if (rc != 0) return rc;
    usleep(1000U);

    for (unsigned i = 0; i < sizeof(reset_seq)/sizeof(reset_seq[0]); i++) {
        rc = dac_write_reg(i2c, reset_seq[i][0], reset_seq[i][1]);
        if (rc != 0) return rc;
    }

    usleep(1000U);

    /* Release reset after the full register image is stable. */
    rc = dac_write_reg(i2c, 0x00, CTRL1_RST1);
    if (rc != 0) return rc;

    usleep(10000U);

    rc = dac_write_reg(i2c, 0x01, CTRL2_UNMUTE);
    if (rc != 0) return rc;

    return 0;
}
