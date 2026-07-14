/*
 * gic_init.h -- One-shot GIC bring-up for the frontend application.
 *
 * Initialises the Zynq-7000 ScuGic, registers the IRQ exception
 * handler, and enables CPU interrupts. Other modules (smartport_service,
 * etc.) call gic_get() to attach their per-IRQ handlers without
 * bringing up a second GIC instance.
 */

#ifndef GIC_INIT_H
#define GIC_INIT_H

#include "xscugic.h"

/* Bring up the GIC and enable CPU exceptions. Returns 0 on success,
 * non-zero on error. Idempotent: subsequent calls are no-ops. */
int gic_init(void);

/* Accessor for the shared XScuGic instance. Returns NULL if gic_init()
 * has not yet succeeded. */
XScuGic *gic_get(void);

#endif /* GIC_INIT_H */
