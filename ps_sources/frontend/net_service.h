#ifndef NET_SERVICE_H
#define NET_SERVICE_H
#include <stddef.h>
#include <stdint.h>

/* Version 1: unit zero, nine SmartPort list bytes. All integers are LE. */
#define NET_SELECTOR 0x82U
#define NET_LAST_SELECTOR 0x86U
#define NET_VERSION 1U
#define NET_DESCRIPTOR_SIZE 64U
#define NET_REPLY_HEADER 8U
#define NET_REQUEST_MAX 1012U
#define NET_START_HEADER 12U
enum { NET_START = 1, NET_CANCEL, NET_JSON };
enum { NET_IDLE, NET_QUEUED, NET_ACTIVE, NET_DONE, NET_FAILED, NET_CANCELLED };
enum { NET_MORE = 1, NET_END, NET_SOURCE_FAILED, NET_LOST };

#if defined(APPLETINI_HTTP_SERVICE) || defined(NET_SERVICE_HOST)
uint8_t net_service_control(const uint8_t *payload, size_t length);
uint16_t net_service_status(uint8_t selector, const uint8_t list[9],
                            uint8_t *out, size_t capacity);
void net_service_poll(void);
void net_service_reset(void);
/* Zero for an external Apple clock; ONE//e returns its profile's VIA clock. */
void net_service_set_via_hz(uint32_t hz);
uint32_t net_service_json_max_us(void);
#else
static inline uint8_t net_service_control(const uint8_t *p,size_t n)
{ (void)p;(void)n;return 0x21; }
static inline uint16_t net_service_status(uint8_t s,const uint8_t l[9],uint8_t *p,size_t n)
{ (void)s;(void)l;(void)p;(void)n;return 0; }
static inline void net_service_poll(void) {}
static inline void net_service_reset(void) {}
static inline void net_service_set_via_hz(uint32_t hz) { (void)hz; }
static inline uint32_t net_service_json_max_us(void) { return 0; }
#endif
#endif
