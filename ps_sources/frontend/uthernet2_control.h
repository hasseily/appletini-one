#ifndef UTHERNET2_CONTROL_H
#define UTHERNET2_CONTROL_H

#include <stddef.h>
#include <stdint.h>

#define UTHERNET2_MAC_LEN 6U
#define UTHERNET2_IPV4_LEN 4U

/* Shared W5100S register and 4+2+1+1 KB socket-memory layout. Keep all users
 * on this one definition so raw DHCP and TCP services cannot drift apart. */
#define W5100_REG_RMSR           0x001AU
#define W5100_REG_TMSR           0x001BU
#define W5100_SOCKET_MEM_4_2_1_1 0x06U

#define W5100_SN_MR              0x00U
#define W5100_SN_CR              0x01U
#define W5100_SN_IR              0x02U
#define W5100_SN_SR              0x03U
#define W5100_SN_PORT            0x04U
#define W5100_SN_DIPR            0x0CU
#define W5100_SN_TX_FSR          0x20U
#define W5100_SN_TX_WR           0x24U
#define W5100_SN_RX_RSR          0x26U
#define W5100_SN_RX_RD           0x28U

#define W5100_SN_MR_TCP          0x01U
#define W5100_SN_MR_MACRAW_MF    0x44U
#define W5100_CR_OPEN            0x01U
#define W5100_CR_LISTEN          0x02U
#define W5100_CR_DISCON          0x08U
#define W5100_CR_CLOSE           0x10U
#define W5100_CR_SEND            0x20U
#define W5100_CR_RECV            0x40U
#define W5100_IR_CON             0x01U
#define W5100_IR_DISCON          0x02U
#define W5100_IR_RECV            0x04U
#define W5100_IR_TIMEOUT         0x08U
#define W5100_IR_SENDOK          0x10U
#define W5100_SR_CLOSED          0x00U
#define W5100_SR_INIT            0x13U
#define W5100_SR_LISTEN          0x14U
#define W5100_SR_ESTABLISHED     0x17U
#define W5100_SR_CLOSE_WAIT      0x1CU

typedef struct {
    uint8_t mac[UTHERNET2_MAC_LEN];
    uint8_t ip[UTHERNET2_IPV4_LEN];
    uint8_t subnet[UTHERNET2_IPV4_LEN];
    uint8_t gateway[UTHERNET2_IPV4_LEN];
} uthernet2_network_config_t;

typedef struct {
    uint8_t version;
    uint8_t physr;
    uint8_t link_up;
    uthernet2_network_config_t config;
} uthernet2_test_result_t;

void uthernet2_default_config(uthernet2_network_config_t *config);
uint8_t uthernet2_mac_is_valid(const uint8_t mac[UTHERNET2_MAC_LEN]);
int uthernet2_read_reg(uint16_t addr, uint8_t *value);
int uthernet2_write_reg(uint16_t addr, uint8_t value);
int uthernet2_read_block(uint16_t addr, uint8_t *dst, uint16_t len);
int uthernet2_write_block(uint16_t addr, const uint8_t *src, uint16_t len);
uint16_t uthernet2_w5100_socket_reg(uint8_t socket, uint16_t offset);
int uthernet2_w5100_read16(uint16_t addr, uint16_t *value);
int uthernet2_w5100_read16_stable(uint16_t addr, uint16_t *value);
int uthernet2_w5100_write16(uint16_t addr, uint16_t value);
int uthernet2_w5100_socket_command(uint8_t socket, uint8_t command);
int uthernet2_w5100_socket_status(uint8_t socket, uint8_t *status);
int uthernet2_w5100_socket_ring_read(uint8_t socket,
                                     uint16_t pointer,
                                     uint8_t *dst,
                                     uint16_t len);
int uthernet2_w5100_socket_ring_write(uint8_t socket,
                                      uint16_t pointer,
                                      const uint8_t *src,
                                      uint16_t len);
int uthernet2_read_network_config(uthernet2_network_config_t *config);
int uthernet2_write_network_config(const uthernet2_network_config_t *config);
int uthernet2_test(uthernet2_test_result_t *result);
int uthernet2_dhcp_start(const uint8_t mac[UTHERNET2_MAC_LEN],
                         char *detail,
                         size_t detail_len);
/* Returns 0 while pending, 1 after a lease is acquired, and -1 on failure. */
int uthernet2_dhcp_poll(uthernet2_network_config_t *lease,
                        char *detail,
                        size_t detail_len);
void uthernet2_dhcp_cancel(void);
int uthernet2_dhcp_acquire(const uint8_t mac[UTHERNET2_MAC_LEN],
                           uthernet2_network_config_t *lease,
                           char *detail,
                           size_t detail_len);

#endif
