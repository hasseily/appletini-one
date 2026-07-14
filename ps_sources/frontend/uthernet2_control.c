#include "uthernet2_control.h"

#include <stdio.h>
#include <string.h>

#include "card_control_regs.h"
#include "../lib/common.h"
#include "sleep.h"

#define W5100_REG_MR        0x0000U
#define W5100_REG_GAR       0x0001U
#define W5100_REG_SUBR      0x0005U
#define W5100_REG_SHAR      0x0009U
#define W5100_REG_SIPR      0x000FU
#define W5100_REG_RMSR      0x001AU
#define W5100_REG_TMSR      0x001BU
#define W5100S_REG_PHYSR    0x003CU
#define W5100S_REG_VERR     0x0080U

#define W5100_S0_MR         0x0400U
#define W5100_S0_CR         0x0401U
#define W5100_S0_IR         0x0402U
#define W5100_S0_SR         0x0403U
#define W5100_S0_TX_FSR     0x0420U
#define W5100_S0_TX_WR      0x0424U
#define W5100_S0_RX_RSR     0x0426U
#define W5100_S0_RX_RD      0x0428U
#define W5100_TX_BASE       0x4000U
#define W5100_RX_BASE       0x6000U
#define W5100_RAW_MASK      0x0FFFU

#define W5100_SOCKET_MEM_4_2_1_1 0x06U
#define W5100_S0_MR_MACRAW_MF    0x44U
#define W5100_S0_SR_MACRAW       0x42U
#define W5100_CR_OPEN            0x01U
#define W5100_CR_SEND            0x20U
#define W5100_CR_RECV            0x40U
#define W5100_CR_CLOSE           0x10U
#define W5100_IR_TIMEOUT         0x08U
#define W5100_IR_SENDOK          0x10U
#define W5100S_PHYSR_LINK        0x01U

#define DHCP_CLIENT_PORT  68U
#define DHCP_SERVER_PORT  67U
#define DHCP_PACKET_MAX   548U
#define DHCP_FRAME_MAX    600U
#define DHCP_MAGIC_COOKIE 0x63825363UL
#define DHCP_DISCOVER     1U
#define DHCP_OFFER        2U
#define DHCP_REQUEST      3U
#define DHCP_ACK          5U
#define DHCP_NAK          6U
#define DHCP_ATTEMPTS     3U

#define ETH_HEADER_LEN    14U
#define IPV4_HEADER_LEN   20U
#define UDP_HEADER_LEN    8U

#define UTHERNET2_CMD_TIMEOUT_POLLS 10000U
#define UTHERNET2_READY_POLL_USEC   20U
#define DHCP_POLL_USEC              10000U
#define LINK_WAIT_POLLS             500U
#define DHCP_WAIT_POLLS             200U

typedef struct {
    uint8_t message_type;
    uint8_t server_id[UTHERNET2_IPV4_LEN];
    uint8_t subnet[UTHERNET2_IPV4_LEN];
    uint8_t gateway[UTHERNET2_IPV4_LEN];
    uint8_t ip[UTHERNET2_IPV4_LEN];
    uint8_t has_server_id;
    uint8_t has_subnet;
    uint8_t has_gateway;
} dhcp_parse_t;

static void uthernet2_io_barrier(void)
{
    __asm__ volatile ("dsb sy" ::: "memory");
}

static void set_detail(char *detail, size_t detail_len, const char *text)
{
    if (detail != NULL && detail_len != 0U) {
        (void)snprintf(detail, detail_len, "%s", text != NULL ? text : "");
    }
}

void uthernet2_default_config(uthernet2_network_config_t *config)
{
    static const uthernet2_network_config_t defaults = {
        {0x02U, 0x41U, 0x50U, 0x50U, 0x4CU, 0x01U},
        {192U, 168U, 1U, 50U},
        {255U, 255U, 255U, 0U},
        {192U, 168U, 1U, 1U}
    };

    if (config != NULL) {
        *config = defaults;
    }
}

uint8_t uthernet2_mac_is_valid(const uint8_t mac[UTHERNET2_MAC_LEN])
{
    uint8_t nonzero = 0U;

    if (mac == NULL || (mac[0] & 0x01U) != 0U) {
        return 0U;
    }
    for (uint8_t i = 0U; i < UTHERNET2_MAC_LEN; ++i) {
        nonzero |= mac[i];
    }
    return nonzero != 0U;
}

static int uthernet2_command(uint16_t addr,
                             uint8_t write,
                             uint8_t wdata,
                             uint8_t *rdata)
{
    uint32_t status;

    for (uint32_t poll = 0U; poll < UTHERNET2_CMD_TIMEOUT_POLLS; ++poll) {
        status = REG_READ(CARD_CTRL_ETH_STATUS_REG);
        if ((status & CARD_CTRL_ETH_STATUS_READY) != 0U &&
            (status & CARD_CTRL_ETH_STATUS_BUSY) == 0U) {
            break;
        }
        if (poll == UTHERNET2_CMD_TIMEOUT_POLLS - 1U) {
            return -1;
        }
        usleep(UTHERNET2_READY_POLL_USEC);
    }

    REG_WRITE(CARD_CTRL_ETH_ADDR_REG, addr);
    REG_WRITE(CARD_CTRL_ETH_DATA_REG, wdata);
    uthernet2_io_barrier();
    REG_WRITE(CARD_CTRL_ETH_CMD_REG,
              CARD_CTRL_ETH_CMD_GO |
              (write != 0U ? CARD_CTRL_ETH_CMD_WRITE : 0U));
    uthernet2_io_barrier();

    for (uint32_t poll = 0U; poll < UTHERNET2_CMD_TIMEOUT_POLLS; ++poll) {
        status = REG_READ(CARD_CTRL_ETH_STATUS_REG);
        if ((status & CARD_CTRL_ETH_STATUS_DONE) != 0U) {
            if ((status & CARD_CTRL_ETH_STATUS_ERROR) != 0U) {
                return -2;
            }
            if (rdata != NULL) {
                *rdata = (uint8_t)((status >> CARD_CTRL_ETH_STATUS_RDATA_SHIFT) &
                                   CARD_CTRL_ETH_STATUS_RDATA_MASK);
            }
            return 0;
        }
    }
    return -3;
}

int uthernet2_read_reg(uint16_t addr, uint8_t *value)
{
    return value != NULL ? uthernet2_command(addr, 0U, 0U, value) : -1;
}

int uthernet2_write_reg(uint16_t addr, uint8_t value)
{
    return uthernet2_command(addr, 1U, value, NULL);
}

static int w5100_read(uint16_t addr, uint8_t *dst, uint16_t len)
{
    if (dst == NULL) {
        return -1;
    }
    for (uint16_t i = 0U; i < len; ++i) {
        if (uthernet2_read_reg((uint16_t)(addr + i), &dst[i]) != 0) {
            return -1;
        }
    }
    return 0;
}

static int w5100_write(uint16_t addr, const uint8_t *src, uint16_t len)
{
    if (src == NULL) {
        return -1;
    }
    for (uint16_t i = 0U; i < len; ++i) {
        if (uthernet2_write_reg((uint16_t)(addr + i), src[i]) != 0) {
            return -1;
        }
    }
    return 0;
}

static int w5100_read16(uint16_t addr, uint16_t *value)
{
    uint8_t bytes[2];

    if (value == NULL || w5100_read(addr, bytes, sizeof(bytes)) != 0) {
        return -1;
    }
    *value = (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
    return 0;
}

static int w5100_read16_stable(uint16_t addr, uint16_t *value)
{
    uint16_t previous;
    uint16_t current;

    if (value == NULL || w5100_read16(addr, &previous) != 0) {
        return -1;
    }
    for (uint8_t i = 0U; i < 8U; ++i) {
        if (w5100_read16(addr, &current) != 0) {
            return -1;
        }
        if (current == previous) {
            *value = current;
            return 0;
        }
        previous = current;
    }
    return -1;
}

static int w5100_write16(uint16_t addr, uint16_t value)
{
    const uint8_t bytes[2] = {
        (uint8_t)(value >> 8), (uint8_t)value
    };

    return w5100_write(addr, bytes, sizeof(bytes));
}

int uthernet2_read_network_config(uthernet2_network_config_t *config)
{
    if (config == NULL ||
        w5100_read(W5100_REG_SHAR, config->mac, sizeof(config->mac)) != 0 ||
        w5100_read(W5100_REG_SIPR, config->ip, sizeof(config->ip)) != 0 ||
        w5100_read(W5100_REG_SUBR, config->subnet, sizeof(config->subnet)) != 0 ||
        w5100_read(W5100_REG_GAR, config->gateway, sizeof(config->gateway)) != 0) {
        return -1;
    }
    return 0;
}

static int w5100_write_network_config(const uthernet2_network_config_t *config)
{
    if (w5100_write(W5100_REG_SHAR, config->mac, sizeof(config->mac)) != 0 ||
        w5100_write(W5100_REG_GAR, config->gateway, sizeof(config->gateway)) != 0 ||
        w5100_write(W5100_REG_SUBR, config->subnet, sizeof(config->subnet)) != 0 ||
        w5100_write(W5100_REG_SIPR, config->ip, sizeof(config->ip)) != 0) {
        return -1;
    }
    return 0;
}

int uthernet2_write_network_config(const uthernet2_network_config_t *config)
{
    return config != NULL && uthernet2_mac_is_valid(config->mac) != 0U ?
        w5100_write_network_config(config) : -1;
}

int uthernet2_test(uthernet2_test_result_t *result)
{
    if (result == NULL) {
        return -1;
    }
    memset(result, 0, sizeof(*result));
    if (uthernet2_read_reg(W5100S_REG_VERR, &result->version) != 0 ||
        uthernet2_read_reg(W5100S_REG_PHYSR, &result->physr) != 0 ||
        uthernet2_read_network_config(&result->config) != 0) {
        return -1;
    }
    result->link_up = (result->physr & W5100S_PHYSR_LINK) != 0U;
    return 0;
}

static int w5100_wait_link(void)
{
    uint8_t physr;

    for (uint16_t poll = 0U; poll < LINK_WAIT_POLLS; ++poll) {
        if (uthernet2_read_reg(W5100S_REG_PHYSR, &physr) != 0) {
            return -1;
        }
        if ((physr & W5100S_PHYSR_LINK) != 0U) {
            return 0;
        }
        usleep(DHCP_POLL_USEC);
    }
    return -1;
}

static int w5100_reset(void)
{
    if (uthernet2_write_reg(W5100_REG_MR, 0x80U) != 0) {
        return -1;
    }
    usleep(10000U);
    return 0;
}

static int w5100_command(uint16_t command_reg, uint8_t command)
{
    uint8_t value;

    if (uthernet2_write_reg(command_reg, command) != 0) {
        return -1;
    }
    for (uint16_t poll = 0U; poll < 100U; ++poll) {
        if (uthernet2_read_reg(command_reg, &value) != 0) {
            return -1;
        }
        if (value == 0U) {
            return 0;
        }
        usleep(1000U);
    }
    return -1;
}

static int w5100_close_sockets(void)
{
    int result = 0;

    for (uint8_t socket = 0U; socket < 4U; ++socket) {
        const uint16_t base = (uint16_t)(0x0400U + (uint16_t)socket * 0x0100U);

        if (w5100_command((uint16_t)(base + 1U), W5100_CR_CLOSE) != 0 ||
            uthernet2_write_reg((uint16_t)(base + 2U), 0xFFU) != 0) {
            result = -1;
        }
    }
    return result;
}

static int w5100_raw_open(const uint8_t mac[UTHERNET2_MAC_LEN])
{
    uint8_t state;

    if (w5100_close_sockets() != 0 ||
        uthernet2_write_reg(W5100_REG_RMSR, W5100_SOCKET_MEM_4_2_1_1) != 0 ||
        uthernet2_write_reg(W5100_REG_TMSR, W5100_SOCKET_MEM_4_2_1_1) != 0 ||
        w5100_write(W5100_REG_SHAR, mac, UTHERNET2_MAC_LEN) != 0 ||
        uthernet2_write_reg(W5100_S0_MR, W5100_S0_MR_MACRAW_MF) != 0 ||
        w5100_command(W5100_S0_CR, W5100_CR_OPEN) != 0 ||
        uthernet2_read_reg(W5100_S0_SR, &state) != 0 ||
        state != W5100_S0_SR_MACRAW) {
        return -1;
    }
    return 0;
}

static int w5100_ring_write(uint16_t base,
                            uint16_t pointer,
                            const uint8_t *src,
                            uint16_t len)
{
    while (len != 0U) {
        const uint16_t offset = (uint16_t)(pointer & W5100_RAW_MASK);
        uint16_t chunk = (uint16_t)((W5100_RAW_MASK + 1U) - offset);

        if (chunk > len) {
            chunk = len;
        }
        if (w5100_write((uint16_t)(base + offset), src, chunk) != 0) {
            return -1;
        }
        pointer = (uint16_t)(pointer + chunk);
        src += chunk;
        len = (uint16_t)(len - chunk);
    }
    return 0;
}

static int w5100_ring_read(uint16_t base,
                           uint16_t pointer,
                           uint8_t *dst,
                           uint16_t len)
{
    while (len != 0U) {
        const uint16_t offset = (uint16_t)(pointer & W5100_RAW_MASK);
        uint16_t chunk = (uint16_t)((W5100_RAW_MASK + 1U) - offset);

        if (chunk > len) {
            chunk = len;
        }
        if (w5100_read((uint16_t)(base + offset), dst, chunk) != 0) {
            return -1;
        }
        pointer = (uint16_t)(pointer + chunk);
        dst += chunk;
        len = (uint16_t)(len - chunk);
    }
    return 0;
}

static int w5100_raw_send(const uint8_t *frame, uint16_t len)
{
    uint16_t free_size = 0U;
    uint16_t write_pointer;
    uint8_t interrupt;

    for (uint16_t poll = 0U; poll < 200U; ++poll) {
        if (w5100_read16_stable(W5100_S0_TX_FSR, &free_size) != 0) {
            return -1;
        }
        if (free_size >= len) {
            break;
        }
        usleep(1000U);
    }
    if (free_size < len ||
        w5100_read16(W5100_S0_TX_WR, &write_pointer) != 0 ||
        w5100_ring_write(W5100_TX_BASE, write_pointer, frame, len) != 0 ||
        w5100_write16(W5100_S0_TX_WR, (uint16_t)(write_pointer + len)) != 0 ||
        uthernet2_write_reg(W5100_S0_IR,
                           W5100_IR_SENDOK | W5100_IR_TIMEOUT) != 0 ||
        w5100_command(W5100_S0_CR, W5100_CR_SEND) != 0) {
        return -1;
    }

    for (uint16_t poll = 0U; poll < 200U; ++poll) {
        if (uthernet2_read_reg(W5100_S0_IR, &interrupt) != 0) {
            return -1;
        }
        if ((interrupt & W5100_IR_SENDOK) != 0U) {
            (void)uthernet2_write_reg(W5100_S0_IR, W5100_IR_SENDOK);
            return 0;
        }
        if ((interrupt & W5100_IR_TIMEOUT) != 0U) {
            (void)uthernet2_write_reg(W5100_S0_IR, W5100_IR_TIMEOUT);
            return -1;
        }
        usleep(1000U);
    }
    return -1;
}

static int w5100_raw_receive(uint8_t *frame,
                             uint16_t capacity,
                             uint16_t *frame_len)
{
    uint8_t length_header[2];
    uint16_t received_size;
    uint16_t read_pointer;
    uint16_t packet_size;
    uint16_t copy_len;

    *frame_len = 0U;
    if (w5100_read16_stable(W5100_S0_RX_RSR, &received_size) != 0) {
        return -1;
    }
    if (received_size < 2U) {
        return 1;
    }
    if (w5100_read16(W5100_S0_RX_RD, &read_pointer) != 0 ||
        w5100_ring_read(W5100_RX_BASE,
                        read_pointer,
                        length_header,
                        sizeof(length_header)) != 0) {
        return -1;
    }
    packet_size = (uint16_t)(((uint16_t)length_header[0] << 8) |
                             length_header[1]);
    if (packet_size < 2U || packet_size > received_size) {
        return 1;
    }
    copy_len = (uint16_t)(packet_size - 2U);
    if (copy_len > capacity) {
        copy_len = capacity;
    }
    if (w5100_ring_read(W5100_RX_BASE,
                        (uint16_t)(read_pointer + 2U),
                        frame,
                        copy_len) != 0 ||
        w5100_write16(W5100_S0_RX_RD,
                      (uint16_t)(read_pointer + packet_size)) != 0 ||
        w5100_command(W5100_S0_CR, W5100_CR_RECV) != 0) {
        return -1;
    }
    *frame_len = copy_len;
    return 0;
}

static uint16_t get_be16(const uint8_t *src)
{
    return (uint16_t)(((uint16_t)src[0] << 8) | src[1]);
}

static void put_be16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value >> 8);
    dst[1] = (uint8_t)value;
}

static void put_be32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value >> 24);
    dst[1] = (uint8_t)(value >> 16);
    dst[2] = (uint8_t)(value >> 8);
    dst[3] = (uint8_t)value;
}

static uint16_t checksum(const uint8_t *data, uint16_t len)
{
    uint32_t sum = 0U;

    while (len >= 2U) {
        sum += get_be16(data);
        data += 2;
        len = (uint16_t)(len - 2U);
    }
    if (len != 0U) {
        sum += (uint16_t)data[0] << 8;
    }
    while ((sum >> 16) != 0U) {
        sum = (sum & 0xFFFFU) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

static uint16_t dhcp_build_packet(uint8_t *packet,
                                  uint8_t message_type,
                                  uint32_t xid,
                                  const uint8_t mac[UTHERNET2_MAC_LEN],
                                  const uint8_t *requested_ip,
                                  const uint8_t *server_id)
{
    uint16_t pos = 240U;

    memset(packet, 0, DHCP_PACKET_MAX);
    packet[0] = 1U;
    packet[1] = 1U;
    packet[2] = UTHERNET2_MAC_LEN;
    put_be32(&packet[4], xid);
    put_be16(&packet[10], 0x8000U);
    memcpy(&packet[28], mac, UTHERNET2_MAC_LEN);
    put_be32(&packet[236], DHCP_MAGIC_COOKIE);

#define DHCP_OPTION(code, len) do { packet[pos++] = (code); packet[pos++] = (len); } while (0)
    DHCP_OPTION(53U, 1U);
    packet[pos++] = message_type;
    if (message_type == DHCP_REQUEST) {
        DHCP_OPTION(54U, 4U);
        memcpy(&packet[pos], server_id, UTHERNET2_IPV4_LEN);
        pos = (uint16_t)(pos + UTHERNET2_IPV4_LEN);
        DHCP_OPTION(50U, 4U);
        memcpy(&packet[pos], requested_ip, UTHERNET2_IPV4_LEN);
        pos = (uint16_t)(pos + UTHERNET2_IPV4_LEN);
    } else {
        DHCP_OPTION(55U, 3U);
        packet[pos++] = 1U;
        packet[pos++] = 3U;
        packet[pos++] = 6U;
    }
    packet[pos++] = 255U;
#undef DHCP_OPTION
    return pos;
}

static uint8_t dhcp_parse_packet(const uint8_t *packet,
                                 uint16_t len,
                                 uint32_t xid,
                                 const uint8_t mac[UTHERNET2_MAC_LEN],
                                 dhcp_parse_t *out)
{
    uint8_t xid_bytes[4];
    uint16_t pos = 240U;

    put_be32(xid_bytes, xid);
    if (len < 241U || packet[0] != 2U || packet[1] != 1U ||
        packet[2] != UTHERNET2_MAC_LEN ||
        memcmp(&packet[4], xid_bytes, sizeof(xid_bytes)) != 0 ||
        memcmp(&packet[28], mac, UTHERNET2_MAC_LEN) != 0 ||
        memcmp(&packet[236], "\x63\x82\x53\x63", 4U) != 0) {
        return 0U;
    }

    memset(out, 0, sizeof(*out));
    memcpy(out->ip, &packet[16], UTHERNET2_IPV4_LEN);
    while (pos < len) {
        const uint8_t code = packet[pos++];
        uint8_t option_len;

        if (code == 255U) {
            break;
        }
        if (code == 0U) {
            continue;
        }
        if (pos >= len) {
            return 0U;
        }
        option_len = packet[pos++];
        if ((uint16_t)(pos + option_len) > len) {
            return 0U;
        }
        if (code == 53U && option_len >= 1U) {
            out->message_type = packet[pos];
        } else if (code == 54U && option_len >= 4U) {
            memcpy(out->server_id, &packet[pos], 4U);
            out->has_server_id = 1U;
        } else if (code == 1U && option_len >= 4U) {
            memcpy(out->subnet, &packet[pos], 4U);
            out->has_subnet = 1U;
        } else if (code == 3U && option_len >= 4U) {
            memcpy(out->gateway, &packet[pos], 4U);
            out->has_gateway = 1U;
        }
        pos = (uint16_t)(pos + option_len);
    }
    return out->message_type != 0U;
}

static uint16_t dhcp_build_frame(uint8_t *frame,
                                 const uint8_t mac[UTHERNET2_MAC_LEN],
                                 const uint8_t *payload,
                                 uint16_t payload_len,
                                 uint32_t xid)
{
    uint8_t *ip = &frame[ETH_HEADER_LEN];
    uint8_t *udp = &ip[IPV4_HEADER_LEN];
    const uint16_t ip_len = (uint16_t)(IPV4_HEADER_LEN + UDP_HEADER_LEN +
                                       payload_len);

    memset(frame, 0, DHCP_FRAME_MAX);
    memset(frame, 0xFF, 6U);
    memcpy(&frame[6], mac, UTHERNET2_MAC_LEN);
    put_be16(&frame[12], 0x0800U);

    ip[0] = 0x45U;
    put_be16(&ip[2], ip_len);
    put_be16(&ip[4], (uint16_t)xid);
    ip[8] = 64U;
    ip[9] = 17U;
    memset(&ip[16], 0xFF, UTHERNET2_IPV4_LEN);
    put_be16(&ip[10], checksum(ip, IPV4_HEADER_LEN));

    put_be16(&udp[0], DHCP_CLIENT_PORT);
    put_be16(&udp[2], DHCP_SERVER_PORT);
    put_be16(&udp[4], (uint16_t)(UDP_HEADER_LEN + payload_len));
    memcpy(&udp[UDP_HEADER_LEN], payload, payload_len);
    return (uint16_t)(ETH_HEADER_LEN + ip_len);
}

static uint8_t dhcp_frame_payload(const uint8_t *frame,
                                  uint16_t frame_len,
                                  const uint8_t **payload,
                                  uint16_t *payload_len)
{
    const uint8_t *ip;
    const uint8_t *udp;
    uint16_t ip_header_len;
    uint16_t ip_len;
    uint16_t udp_len;

    if (frame_len < ETH_HEADER_LEN + IPV4_HEADER_LEN + UDP_HEADER_LEN ||
        get_be16(&frame[12]) != 0x0800U) {
        return 0U;
    }
    ip = &frame[ETH_HEADER_LEN];
    ip_header_len = (uint16_t)(ip[0] & 0x0FU) * 4U;
    ip_len = get_be16(&ip[2]);
    if ((ip[0] >> 4) != 4U || ip_header_len < IPV4_HEADER_LEN ||
        ip[9] != 17U || (get_be16(&ip[6]) & 0x1FFFU) != 0U ||
        ip_len < ip_header_len + UDP_HEADER_LEN ||
        ETH_HEADER_LEN + ip_len > frame_len) {
        return 0U;
    }
    udp = &ip[ip_header_len];
    udp_len = get_be16(&udp[4]);
    if (get_be16(&udp[0]) != DHCP_SERVER_PORT ||
        get_be16(&udp[2]) != DHCP_CLIENT_PORT ||
        udp_len < UDP_HEADER_LEN || udp_len > ip_len - ip_header_len) {
        return 0U;
    }
    *payload = &udp[UDP_HEADER_LEN];
    *payload_len = (uint16_t)(udp_len - UDP_HEADER_LEN);
    return 1U;
}

static int dhcp_wait(uint8_t *frame,
                     uint32_t xid,
                     const uint8_t mac[UTHERNET2_MAC_LEN],
                     uint8_t wanted_type,
                     dhcp_parse_t *reply)
{
    uint8_t received = 0U;

    for (uint16_t poll = 0U; poll < DHCP_WAIT_POLLS; ++poll) {
        const uint8_t *payload;
        uint16_t frame_len;
        uint16_t payload_len;
        const int receive = w5100_raw_receive(frame,
                                               DHCP_FRAME_MAX,
                                               &frame_len);

        if (receive < 0) {
            return -1;
        }
        if (receive == 0) {
            received = 1U;
            if (dhcp_frame_payload(frame, frame_len, &payload, &payload_len) != 0U &&
                dhcp_parse_packet(payload, payload_len, xid, mac, reply) != 0U &&
                (reply->message_type == wanted_type ||
                 reply->message_type == DHCP_NAK)) {
                return 0;
            }
        }
        usleep(DHCP_POLL_USEC);
    }
    return received != 0U ? 2 : 1;
}

static int dhcp_fail(const uthernet2_network_config_t *previous,
                     char *detail,
                     size_t detail_len,
                     const char *message)
{
    (void)w5100_close_sockets();
    (void)w5100_write_network_config(previous);
    set_detail(detail, detail_len, message);
    return -1;
}

int uthernet2_dhcp_acquire(const uint8_t mac[UTHERNET2_MAC_LEN],
                           uthernet2_network_config_t *lease,
                           char *detail,
                           size_t detail_len)
{
    static uint32_t xid_seed = 0x41545001UL;
    uint8_t packet[DHCP_PACKET_MAX];
    uint8_t frame[DHCP_FRAME_MAX];
    uint16_t packet_len;
    uint16_t frame_len;
    uint32_t xid;
    uthernet2_network_config_t previous;
    dhcp_parse_t offer;
    dhcp_parse_t ack;
    int result = 1;

    if (lease == NULL) {
        set_detail(detail, detail_len, "INVALID DHCP ARGUMENT");
        return -1;
    }
    if (uthernet2_mac_is_valid(mac) == 0U) {
        set_detail(detail, detail_len, "DHCP INVALID SOURCE MAC");
        return -1;
    }
    if (uthernet2_read_network_config(&previous) != 0) {
        set_detail(detail, detail_len, "DHCP CONFIG SAVE FAILED");
        return -1;
    }
    if (w5100_reset() != 0) {
        return dhcp_fail(&previous, detail, detail_len, "DHCP RESET FAILED");
    }
    if (w5100_wait_link() != 0) {
        return dhcp_fail(&previous, detail, detail_len, "DHCP LINK DOWN");
    }
    if (w5100_raw_open(mac) != 0) {
        return dhcp_fail(&previous, detail, detail_len, "DHCP MACRAW OPEN FAILED");
    }

    xid_seed += 0x01010101UL;
    xid = xid_seed;
    memset(&offer, 0, sizeof(offer));
    for (uint8_t attempt = 0U; attempt < DHCP_ATTEMPTS; ++attempt) {
        packet_len = dhcp_build_packet(packet, DHCP_DISCOVER, xid, mac,
                                       NULL, NULL);
        frame_len = dhcp_build_frame(frame, mac, packet, packet_len, xid);
        if (w5100_raw_send(frame, frame_len) != 0) {
            result = -1;
            break;
        }
        result = dhcp_wait(frame, xid, mac, DHCP_OFFER, &offer);
        if (result <= 0) {
            break;
        }
    }
    if (result < 0) {
        return dhcp_fail(&previous, detail, detail_len, "DHCP DISCOVER FAILED");
    }
    if (result != 0 || offer.message_type != DHCP_OFFER ||
        offer.has_server_id == 0U) {
        return dhcp_fail(&previous,
                         detail,
                         detail_len,
                         result == 1 ? "DHCP OFFER NO RX" :
                                       "DHCP OFFER INVALID RX");
    }

    memset(&ack, 0, sizeof(ack));
    result = 1;
    for (uint8_t attempt = 0U; attempt < DHCP_ATTEMPTS; ++attempt) {
        packet_len = dhcp_build_packet(packet, DHCP_REQUEST, xid, mac,
                                       offer.ip, offer.server_id);
        frame_len = dhcp_build_frame(frame, mac, packet, packet_len, xid);
        if (w5100_raw_send(frame, frame_len) != 0) {
            result = -1;
            break;
        }
        result = dhcp_wait(frame, xid, mac, DHCP_ACK, &ack);
        if (result <= 0) {
            break;
        }
    }
    (void)w5100_close_sockets();
    if (result < 0) {
        return dhcp_fail(&previous, detail, detail_len, "DHCP REQUEST FAILED");
    }
    if (ack.message_type == DHCP_NAK) {
        return dhcp_fail(&previous, detail, detail_len, "DHCP NAK");
    }
    if (result != 0 || ack.message_type != DHCP_ACK) {
        return dhcp_fail(&previous,
                         detail,
                         detail_len,
                         result == 1 ? "DHCP ACK NO RX" :
                                       "DHCP ACK INVALID RX");
    }

    memcpy(lease->mac, mac, sizeof(lease->mac));
    memcpy(lease->ip, ack.ip, sizeof(lease->ip));
    if (ack.has_subnet != 0U) {
        memcpy(lease->subnet, ack.subnet, sizeof(lease->subnet));
    } else if (offer.has_subnet != 0U) {
        memcpy(lease->subnet, offer.subnet, sizeof(lease->subnet));
    } else {
        memset(lease->subnet, 0, sizeof(lease->subnet));
    }
    if (ack.has_gateway != 0U) {
        memcpy(lease->gateway, ack.gateway, sizeof(lease->gateway));
    } else if (offer.has_gateway != 0U) {
        memcpy(lease->gateway, offer.gateway, sizeof(lease->gateway));
    } else {
        memset(lease->gateway, 0, sizeof(lease->gateway));
    }
    if (uthernet2_write_network_config(lease) != 0) {
        set_detail(detail, detail_len, "DHCP LEASE WRITE FAILED");
        return -1;
    }
    set_detail(detail, detail_len, "DHCP LEASE ACQUIRED");
    return 0;
}
