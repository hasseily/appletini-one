#ifndef UTHERNET2_CONTROL_H
#define UTHERNET2_CONTROL_H

#include <stddef.h>
#include <stdint.h>

#define UTHERNET2_MAC_LEN 6U
#define UTHERNET2_IPV4_LEN 4U

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
int uthernet2_read_network_config(uthernet2_network_config_t *config);
int uthernet2_write_network_config(const uthernet2_network_config_t *config);
int uthernet2_test(uthernet2_test_result_t *result);
int uthernet2_dhcp_acquire(const uint8_t mac[UTHERNET2_MAC_LEN],
                           uthernet2_network_config_t *lease,
                           char *detail,
                           size_t detail_len);

#endif
