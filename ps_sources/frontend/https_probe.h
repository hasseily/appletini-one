#ifndef HTTPS_PROBE_H
#define HTTPS_PROBE_H

#include <stdint.h>

enum {
    HTTPS_PROBE_USB1 = 0,
    HTTPS_PROBE_SMARTPORT = 1,
    HTTPS_PROBE_USB0 = 2,
    HTTPS_PROBE_SERVICE_COUNT = 3
};

#ifdef APPLETINI_HTTPS_PROBE
int https_probe_command(int argc, char **argv, uint32_t uart_base);
int https_probe_active(void);
void https_probe_poll(void);
void https_probe_service_tick(unsigned service);
#else
static inline int https_probe_command(int argc, char **argv, uint32_t uart_base)
{ (void)argc; (void)argv; (void)uart_base; return 0; }
static inline int https_probe_active(void) { return 0; }
static inline void https_probe_poll(void) {}
#ifdef APPLETINI_HTTP_BENCH
void https_probe_service_tick(unsigned service);
#else
static inline void https_probe_service_tick(unsigned service) { (void)service; }
#endif
#endif

#endif
