#include "golden_led.h"

#include "xiltimer.h"   /* XTime, XTime_GetTime, COUNTS_PER_SECOND */

/* Zynq-7000 PS GPIO controller (UG585 ch.14). The status LED is on MIO0,
 * which is bank 0, bit 0. */
#define GPIO_BASE              0xE000A000U
#define GPIO_MASK_DATA_0_LSW   (GPIO_BASE + 0x000U) /* masked write, MIO[15:0] */
#define GPIO_DIRM_0            (GPIO_BASE + 0x204U) /* direction, 1 = output    */
#define GPIO_OEN_0             (GPIO_BASE + 0x208U) /* output enable, 1 = drive */

#define GOLDEN_LED_BIT         (1U << 0)            /* MIO0 within bank 0 */

/* Error-code blink shaping. Finite repeats so the board still drops into
 * the serial monitor afterwards instead of blinking forever. Bump
 * ERROR_BLINK_REPEATS (or loop) if you want it persistent. */
#define ERROR_BLINK_REPEATS    4U
#define ERROR_BLINK_ON_MS      200U
#define ERROR_BLINK_OFF_MS     250U
#define ERROR_BLINK_GAP_MS     1300U

static uint8_t s_led_on = 0U;

static inline uint32_t reg_rd(uintptr_t addr)
{
    return *(volatile uint32_t *)addr;
}

static inline void reg_wr(uintptr_t addr, uint32_t val)
{
    *(volatile uint32_t *)addr = val;
}

static void led_apply(void)
{
    /* MASK_DATA_x_LSW: bits[31:16] mask (1 = leave bit unchanged),
     * bits[15:0] data. Mask everything except bit 0 so we touch only
     * MIO0 and never disturb the other GPIO/peripheral pins in bank 0. */
    reg_wr(GPIO_MASK_DATA_0_LSW,
           0xFFFE0000U | (s_led_on ? GOLDEN_LED_BIT : 0U));
}

void golden_led_init(void)
{
    /* MIO0 -> output, driver enabled. Read-modify-write so the other
     * bank-0 pins keep their direction/OE. */
    reg_wr(GPIO_DIRM_0, reg_rd(GPIO_DIRM_0) | GOLDEN_LED_BIT);
    reg_wr(GPIO_OEN_0,  reg_rd(GPIO_OEN_0)  | GOLDEN_LED_BIT);
    s_led_on = 0U;
    led_apply();
}

void golden_led_on(void)
{
    s_led_on = 1U;
    led_apply();
}

void golden_led_off(void)
{
    s_led_on = 0U;
    led_apply();
}

void golden_led_toggle(void)
{
    s_led_on ^= 1U;
    led_apply();
}

static void delay_ms(uint32_t ms)
{
    XTime start = 0U;
    XTime now = 0U;
    const XTime ticks =
        (XTime)(((uint64_t)COUNTS_PER_SECOND * (uint64_t)ms) / 1000ULL);

    XTime_GetTime(&start);
    do {
        XTime_GetTime(&now);
    } while ((now - start) < ticks);
}

void golden_led_error_code(int code)
{
    uint32_t blinks = (code > 0) ? (uint32_t)code : 1U;
    uint32_t rep;
    uint32_t i;

    for (rep = 0U; rep < ERROR_BLINK_REPEATS; ++rep) {
        for (i = 0U; i < blinks; ++i) {
            golden_led_on();
            delay_ms(ERROR_BLINK_ON_MS);
            golden_led_off();
            delay_ms(ERROR_BLINK_OFF_MS);
        }
        delay_ms(ERROR_BLINK_GAP_MS);
    }
    golden_led_off();
}
