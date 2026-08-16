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
typedef void (*onee_service_runtime_suspend_fn)(void *ctx);
typedef void (*onee_service_runtime_stop_fn)(void *ctx);
typedef uint8_t (*onee_service_runtime_running_fn)(void *ctx);

void onee_service_init(void);
void onee_service_bind_runtime(onee_service_runtime_start_fn start,
                               onee_service_runtime_suspend_fn suspend,
                               onee_service_runtime_stop_fn stop,
                               onee_service_runtime_running_fn running,
                               void *ctx);

/* Restore the global saved selection after the configuration file is read.
 * A restored ON intent stays pending until the PL safety guard is valid,
 * quiet, and reselect-armed. It never bypasses the guard. */
void onee_service_restore_persisted(uint8_t enable);

/* The configuration owner polls this edge-triggered update and acknowledges
 * it only after the global file has been written. A failed write therefore
 * leaves the update pending for a later retry. */
uint8_t onee_service_persist_update_pending(uint8_t *enable);
void onee_service_persist_update_ack(uint8_t enable);

/* These are operator actions. Besides request_start(), the sole high write is
 * the one-shot guarded restore performed by poll() after a card reboot. */
uint8_t onee_service_request_start(void);
void onee_service_request_stop(void);

/* Polling may issue one restored request, or restart the private soft-machine
 * runtime while the original PL request remains high. Apple activity or a
 * lost PL request clears the saved intent and requires a fresh operator
 * action. Missing safety logic keeps all outputs off without erasing the
 * saved choice. */
void onee_service_poll(void);
onee_service_state_t onee_service_state(void);
uint32_t onee_service_status(void);

#endif /* ONEE_SERVICE_H */
