#ifndef APPLETINI_MEMORY_API_HW_H
#define APPLETINI_MEMORY_API_HW_H

#include "memory_api.h"

/* PUBLISH_SHR is optional: STATUS probes the engine capability; CONTROL
 * validates active capture and egress under the hold before any writes. */
extern const memory_api_backend_t memory_api_hardware;
/* Snapshot the reset generation before draining a SmartPort request. */
void memory_api_hw_prepare(uint8_t accelerated);
/* A reset cancels the transport too. Do not publish an orphan response. */
uint8_t memory_api_hw_response_valid(void);

#endif
