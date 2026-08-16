#ifndef ONEE_SERVICE_H
#define ONEE_SERVICE_H

#include <stdint.h>

typedef enum {
    ONEE_SERVICE_STATE_OFF = 0,
    ONEE_SERVICE_STATE_RUNNING,
    ONEE_SERVICE_STATE_LOCKED
} onee_service_state_t;

/* The stand-alone CPU boot path binds these hooks. It loads and releases the
 * soft 65C02 without using the normal vTW host takeover path, physical /DMA,
 * physical RESET, or a prior slot-7 handoff. */
typedef uint8_t (*onee_service_runtime_start_fn)(void *ctx);
typedef void (*onee_service_runtime_stop_fn)(void *ctx);
typedef uint8_t (*onee_service_runtime_running_fn)(void *ctx);

void onee_service_init(void);
void onee_service_bind_runtime(onee_service_runtime_start_fn start,
                               onee_service_runtime_stop_fn stop,
                               onee_service_runtime_running_fn running,
                               void *ctx);

/* These are operator actions. request_start() is the only firmware path
 * allowed to write the PL request bit high. */
uint8_t onee_service_request_start(void);
void onee_service_request_stop(void);

/* Polling can stop a session, but never starts or restarts one. */
void onee_service_poll(void);
onee_service_state_t onee_service_state(void);
uint32_t onee_service_status(void);

#endif /* ONEE_SERVICE_H */
