#ifndef APPLETINI_NET_H
#define APPLETINI_NET_H

#include <stdint.h>

#define APPLETINI_NET_OK          0U
#define APPLETINI_NET_NO_CARD     1U
#define APPLETINI_NET_INVALID_MAC 2U
#define APPLETINI_NET_INVALID_IP  3U
#define APPLETINI_NET_INIT_FAILED 4U

typedef struct {
    uint8_t mac[6];
    uint8_t ip[4];
    uint8_t subnet[4];
    uint8_t gateway[4];
} appletini_network_config_t;

extern appletini_network_config_t appletini_config;

uint8_t appletini_network_init(void);
const char *appletini_network_error(uint8_t status);

#endif
