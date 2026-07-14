#include "uart.h"
#include "common.h"

#define UART_MIRROR_BOTH     0      // write to both uart0 and uart1 at once

#define XUARTPS_CR_OFFSET        0x00
#define XUARTPS_BAUDGEN_OFFSET   0x18
#define XUARTPS_SR_OFFSET    0x2C
#define XUARTPS_FIFO_OFFSET  0x30
#define XUARTPS_BAUDDIV_OFFSET   0x34
#define XUARTPS_SR_RXEMPTY   0x00000002
#define XUARTPS_SR_TXFULL    0x00000010
#define XUARTPS_CR_RXRST         0x00000001U
#define XUARTPS_CR_TXRST         0x00000002U
#define XUARTPS_CR_RX_EN         0x00000004U
#define XUARTPS_CR_RX_DIS        0x00000008U
#define XUARTPS_CR_TX_EN         0x00000010U
#define XUARTPS_CR_TX_DIS        0x00000020U
#define XUARTPS_CR_TORST         0x00000040U
#define UART_DEFAULT_BOOT_BAUD   115200U
#define UART_FALLBACK_CLK_HZ     50000000U
#define UART_TX_TIMEOUT      100000U

static uint32_t uart_absdiff_u32(uint32_t a, uint32_t b)
{
    return (a > b) ? (a - b) : (b - a);
}

static uint32_t uart_infer_input_clk_hz(uint32_t base)
{
    const uint32_t brgr = REG_READ(base + XUARTPS_BAUDGEN_OFFSET) & 0xFFFFU;
    const uint32_t bdiv = REG_READ(base + XUARTPS_BAUDDIV_OFFSET) & 0xFFU;

    if (brgr != 0U) {
        const uint64_t clk = (uint64_t)UART_DEFAULT_BOOT_BAUD *
                             (uint64_t)brgr *
                             (uint64_t)(bdiv + 1U);
        if (clk >= 1000000ULL && clk <= 500000000ULL)
            return (uint32_t)clk;
    }

    return UART_FALLBACK_CLK_HZ;
}

static void uart_pick_baud_dividers(uint32_t in_clk_hz,
                                    uint32_t baud_hz,
                                    uint16_t *brgr_out,
                                    uint8_t *bdiv_out)
{
    uint32_t best_err = 0xFFFFFFFFU;
    uint16_t best_brgr = 0x028BU; /* Reset-ish safe fallback */
    uint8_t best_bdiv = 15U;

    if (baud_hz == 0U) {
        *brgr_out = best_brgr;
        *bdiv_out = best_bdiv;
        return;
    }

    for (uint32_t bdiv = 4U; bdiv <= 255U; ++bdiv) {
        const uint64_t denom = (uint64_t)baud_hz * (uint64_t)(bdiv + 1U);
        if (denom == 0ULL)
            continue;

        uint64_t brgr64 = ((uint64_t)in_clk_hz + (denom / 2ULL)) / denom;
        if (brgr64 < 1ULL)
            brgr64 = 1ULL;
        if (brgr64 > 65535ULL)
            continue;

        const uint64_t actual64 = (uint64_t)in_clk_hz /
                                  ((uint64_t)(bdiv + 1U) * brgr64);
        const uint32_t actual = (uint32_t)actual64;
        const uint32_t err = uart_absdiff_u32(actual, baud_hz);

        if (err < best_err) {
            best_err = err;
            best_brgr = (uint16_t)brgr64;
            best_bdiv = (uint8_t)bdiv;
            if (err == 0U)
                break;
        }
    }

    *brgr_out = best_brgr;
    *bdiv_out = best_bdiv;
}

void uart_putc_one(uint32_t base, char c)
{
    uint32_t to = UART_TX_TIMEOUT;
    while ((REG_READ(base + XUARTPS_SR_OFFSET) & XUARTPS_SR_TXFULL) && to--)
        ;
    if (to == 0U)
        return;
    REG_WRITE(base + XUARTPS_FIFO_OFFSET, (uint32_t)c);
}

void uart_putc(uint32_t base, char c)
{
    uart_putc_one(base, c);
#if UART_MIRROR_BOTH
    if (base == UART0_BASE)
        uart_putc_one(UART1_BASE, c);
#endif
}

void uart_puts(uint32_t base, const char *s)
{
    while (*s)
        uart_putc(base, *s++);
}

void uart_puthex(uint32_t base, uint32_t val)
{
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 7; i >= 0; i--)
        uart_putc(base, hex[(val >> (i * 4)) & 0xF]);
}

void uart_putdec(uint32_t base, uint32_t val)
{
    char buf[12];
    int i = 0;
    if (val == 0) { uart_putc(base, '0'); return; }
    while (val > 0) { buf[i++] = '0' + (val % 10); val /= 10; }
    while (i > 0) uart_putc(base, buf[--i]);
}

int uart_getc_nonblock(uint32_t base, char *c)
{
    if (REG_READ(base + XUARTPS_SR_OFFSET) & XUARTPS_SR_RXEMPTY)
        return 0;

    *c = (char)(REG_READ(base + XUARTPS_FIFO_OFFSET) & 0xFFU);
    return 1;
}

void uart_init_baud(uint32_t base, uint32_t baud)
{
    uint16_t brgr = 0U;
    uint8_t bdiv = 0U;
    const uint32_t in_clk_hz = uart_infer_input_clk_hz(base);

    uart_pick_baud_dividers(in_clk_hz, baud, &brgr, &bdiv);

    /* Conservative sequence: disable channel, program divisors, reset, enable. */
    REG_WRITE(base + XUARTPS_CR_OFFSET, XUARTPS_CR_RX_DIS | XUARTPS_CR_TX_DIS);
    REG_WRITE(base + XUARTPS_BAUDGEN_OFFSET, (uint32_t)brgr);
    REG_WRITE(base + XUARTPS_BAUDDIV_OFFSET, (uint32_t)bdiv);
    REG_WRITE(base + XUARTPS_CR_OFFSET, XUARTPS_CR_RXRST | XUARTPS_CR_TXRST);
    REG_WRITE(base + XUARTPS_CR_OFFSET,
              XUARTPS_CR_RX_EN | XUARTPS_CR_TX_EN | XUARTPS_CR_TORST);
}

void uart_init_both(uint32_t baud)
{
    uart_init_baud(UART0_BASE, baud);
    uart_init_baud(UART1_BASE, baud);
}
