#include <stdint.h>
#include <string.h>

#include "appletini_net.h"
#include "ip65_min.h"

#define U2_MODE (*(volatile uint8_t *)0xC094U)
#define U2_ARH  (*(volatile uint8_t *)0xC095U)
#define U2_ARL  (*(volatile uint8_t *)0xC096U)
#define U2_DATA (*(volatile uint8_t *)0xC097U)

#define W5100_GAR     0x0001U
#define W5100_SUBR    0x0005U
#define W5100_SHAR    0x0009U
#define W5100_SIPR    0x000FU
#define W5100_RMSR    0x001AU
#define W5100_TMSR    0x001BU
#define W5100_S0_BASE 0x0400U

#define APPLETINI_SLOT 1U

appletini_network_config_t appletini_config;

static void w5100_set_addr(uint16_t addr)
{
    U2_ARH = (uint8_t)(addr >> 8);
    U2_ARL = (uint8_t)addr;
}

static uint8_t w5100_read8(uint16_t addr)
{
    w5100_set_addr(addr);
    return U2_DATA;
}

static void w5100_write8(uint16_t addr, uint8_t value)
{
    w5100_set_addr(addr);
    U2_DATA = value;
}

static void w5100_read(uint16_t addr, uint8_t *dst, uint8_t len)
{
    w5100_set_addr(addr);
    while (len-- != 0U) {
        *dst++ = U2_DATA;
    }
}

static void w5100_write(uint16_t addr, const uint8_t *src, uint8_t len)
{
    w5100_set_addr(addr);
    while (len-- != 0U) {
        U2_DATA = *src++;
    }
}

static uint8_t bytes_are(const uint8_t *data, uint8_t len, uint8_t value)
{
    while (len-- != 0U) {
        if (*data++ != value) {
            return 0U;
        }
    }
    return 1U;
}

static void load_card_config(void)
{
    w5100_read(W5100_SHAR, appletini_config.mac,
               sizeof(appletini_config.mac));
    w5100_read(W5100_SIPR, appletini_config.ip,
               sizeof(appletini_config.ip));
    w5100_read(W5100_SUBR, appletini_config.subnet,
               sizeof(appletini_config.subnet));
    w5100_read(W5100_GAR, appletini_config.gateway,
               sizeof(appletini_config.gateway));
}

static void prepare_macraw(void)
{
    uint8_t socket;
    uint16_t base;
    uint16_t timeout;

    for (socket = 0U; socket < 4U; ++socket) {
        base = (uint16_t)(W5100_S0_BASE + (uint16_t)socket * 0x0100U);
        w5100_write8((uint16_t)(base + 1U), 0x10U);
        timeout = 0xFFFFU;
        while (w5100_read8((uint16_t)(base + 1U)) != 0U &&
               --timeout != 0U) {
        }
        w5100_write8((uint16_t)(base + 2U), 0xFFU);
    }
    w5100_write(W5100_SHAR, appletini_config.mac,
                sizeof(appletini_config.mac));
    w5100_write8(W5100_RMSR, 0x06U);
    w5100_write8(W5100_TMSR, 0x06U);
}

static void apply_stack_config(void)
{
    memcpy(cfg_mac, appletini_config.mac, sizeof(appletini_config.mac));
    memcpy(&cfg_ip, appletini_config.ip, sizeof(appletini_config.ip));
    memcpy(&cfg_netmask, appletini_config.subnet,
           sizeof(appletini_config.subnet));
    memcpy(&cfg_gateway, appletini_config.gateway,
           sizeof(appletini_config.gateway));
    /* The W5100 has no DNS register; use its saved gateway as resolver. */
    memcpy(&cfg_dns, appletini_config.gateway,
           sizeof(appletini_config.gateway));
}

static void apply_card_config(void)
{
    U2_MODE = 0x03U;
    w5100_write(W5100_SHAR, appletini_config.mac,
                sizeof(appletini_config.mac));
    w5100_write(W5100_GAR, appletini_config.gateway,
                sizeof(appletini_config.gateway));
    w5100_write(W5100_SUBR, appletini_config.subnet,
                sizeof(appletini_config.subnet));
    w5100_write(W5100_SIPR, appletini_config.ip,
                sizeof(appletini_config.ip));
}

uint8_t appletini_network_init(void)
{
    U2_MODE = 0x03U;
    if (U2_MODE != 0x03U) {
        return APPLETINI_NET_NO_CARD;
    }

    load_card_config();
    if ((appletini_config.mac[0] & 0x01U) != 0U ||
        bytes_are(appletini_config.mac, sizeof(appletini_config.mac), 0U) !=
            0U) {
        return APPLETINI_NET_INVALID_MAC;
    }
    if (bytes_are(appletini_config.ip, sizeof(appletini_config.ip), 0U) != 0U ||
        bytes_are(appletini_config.ip, sizeof(appletini_config.ip), 0xFFU) !=
            0U) {
        return APPLETINI_NET_INVALID_IP;
    }

    apply_stack_config();
    prepare_macraw();
    memcpy(&w5100[4], appletini_config.mac, sizeof(appletini_config.mac));
    if (ip65_init(APPLETINI_SLOT) != 0U) {
        return APPLETINI_NET_INIT_FAILED;
    }
    apply_card_config();
    return APPLETINI_NET_OK;
}

const char *appletini_network_error(uint8_t status)
{
    switch (status) {
    case APPLETINI_NET_NO_CARD:
        return "NO UTHERNET II IN APPLETINI SLOT 1";
    case APPLETINI_NET_INVALID_MAC:
        return "CARD HAS INVALID SAVED MAC";
    case APPLETINI_NET_INVALID_IP:
        return "CARD HAS NO VALID SAVED IP CONFIG";
    default:
        return "NETWORK STACK INITIALIZATION FAILED";
    }
}
