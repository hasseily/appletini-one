#ifndef APPLETINI_IP65_MIN_H
#define APPLETINI_IP65_MIN_H

#include <stdint.h>

#define HTTPD_RESPONSE_200_HTML 2U
#define HTTPD_RESPONSE_404      4U

extern uint8_t cfg_mac[6];
extern uint32_t cfg_ip;
extern uint32_t cfg_netmask;
extern uint32_t cfg_gateway;
extern uint32_t cfg_dns;
extern uint8_t ip65_error;
extern uint8_t w5100[];

uint8_t __fastcall__ ip65_init(uint8_t eth_init);
char *__fastcall__ ip65_strerror(uint8_t err_code);
uint16_t __fastcall__ url_download(const char *url,
                                   const uint8_t *buf,
                                   uint16_t len);
void __fastcall__ httpd_start(
    uint16_t port,
    void __fastcall__ (*callback)(uint32_t client,
                                  const char *method,
                                  const char *path));
void __fastcall__ httpd_send_response(uint8_t response_type,
                                      const uint8_t *buf,
                                      uint16_t len);

#endif
