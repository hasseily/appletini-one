#ifndef NET_SERVICE_H
#define NET_SERVICE_H
#include <stddef.h>
#include <stdint.h>

/* Version 1: unit zero, nine SmartPort list bytes. All integers are LE. */
#define NET_SELECTOR 0x82U
#define NET_LAST_SELECTOR 0x87U
#define NET_VERSION 1U
#define NET_DESCRIPTOR_SIZE 64U
#define NET_REPLY_HEADER 8U
#define NET_REQUEST_MAX 1012U
#define NET_START_HEADER 12U
#define NET_CAP_DOWNLOAD 0x10U
#define NET_CAP_IMAGE 0x20U
#define NET_CAP_PAGE 0x40U
#define NET_FLAG_RETAIN 1U
#define NET_FLAG_AUTO_DOWNLOAD 2U
#define NET_FLAG_FORCE_DOWNLOAD 4U
/* IMAGE: the card fetches a PNG, JPEG or GIF and converts it to 1-bit rows
 * (net_image.h). The flag stands alone, needs GET with no body, and uses
 * the four bytes of the filename field as box width and height (LE16 each).
 * Selector83 returns the finished picture after state DONE; before that it
 * returns MORE with no bytes. Results -120..-122 report a bad image. */
#define NET_FLAG_IMAGE 8U
/* PAGE: when the final response is HTML, the card filters it to a short
 * ASCII page (net_page.h). Selector83 then returns that page after state
 * DONE, addressed by offset; before that it returns MORE with no bytes.
 * Other content types stream as without the flag. PAGE can combine with
 * AUTO_DOWNLOAD, and works with any method. */
#define NET_FLAG_PAGE 16U
/* START flags live at byte2. Optional FORCE filename length occupies10..11;
 * its non-NUL bytes follow method/URL/headers/body. RETAIN cannot combine
 * with download flags; AUTO/FORCE require GET with no body. Descriptor48..61
 * contains state,knownflag,int16result,written32,total32,pathlength16.
 * Selector87 returns the completed FAT path, using the normal chunk header. */
enum { NET_START = 1, NET_CANCEL, NET_JSON };
enum { NET_IDLE, NET_QUEUED, NET_ACTIVE, NET_DONE, NET_FAILED, NET_CANCELLED };
enum { NET_MORE = 1, NET_END, NET_SOURCE_FAILED, NET_LOST };

#if defined(APPLETINI_HTTP_SERVICE) || defined(NET_SERVICE_HOST)
uint8_t net_service_control(const uint8_t *payload, size_t length);
uint16_t net_service_status(uint8_t selector, const uint8_t list[9],
                            uint8_t *out, size_t capacity);
void net_service_poll(void);
void net_service_reset(void);
/* A picture or page request is queued or its data is on the way. */
int net_service_image_active(void);
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
static inline int net_service_image_active(void) { return 0; }
static inline void net_service_set_via_hz(uint32_t hz) { (void)hz; }
static inline uint32_t net_service_json_max_us(void) { return 0; }
#endif
#endif
