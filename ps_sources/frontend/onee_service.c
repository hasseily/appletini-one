/* Session-only ONE//e stand-alone supervisor client. */

#include "onee_service.h"

#include <stddef.h>

#include "card_control_regs.h"
#include "../lib/common.h"

#define ONEE_RUNTIME_RETRY_POLL_LIMIT 64U

static uint8_t g_manual_request;
static uint8_t g_lockout_latched;
static uint8_t g_runtime_started;
static uint32_t g_runtime_retry_polls;
static uint32_t g_status;
static onee_service_runtime_start_fn g_runtime_start;
static onee_service_runtime_suspend_fn g_runtime_suspend;
static onee_service_runtime_stop_fn g_runtime_stop;
static onee_service_runtime_running_fn g_runtime_running;
static void *g_runtime_ctx;

static uint32_t onee_inhibit_reason(uint32_t status)
{
    return (status >> CARD_CTRL_ONEE_STATUS_INHIBIT_SHIFT) &
           CARD_CTRL_ONEE_STATUS_INHIBIT_MASK;
}

static uint8_t onee_status_pl_ready(uint32_t status)
{
    const uint32_t signature =
        (status >> CARD_CTRL_ONEE_STATUS_SIGNATURE_SHIFT) &
        CARD_CTRL_ONEE_STATUS_SIGNATURE_MASK;

    return ((status & CARD_CTRL_ONEE_STATUS_HDL_PRESENT_BIT) != 0U &&
            signature == CARD_CTRL_ONEE_STATUS_SIGNATURE) ? 1U : 0U;
}

static uint8_t onee_status_has_hazard(uint32_t status)
{
    const uint32_t reason = onee_inhibit_reason(status);

    if ((status & (CARD_CTRL_ONEE_STATUS_ACTIVITY_BIT |
                   CARD_CTRL_ONEE_STATUS_APPLE_POWER_BIT |
                   CARD_CTRL_ONEE_STATUS_LOCKOUT_BIT)) != 0U) {
        return 1U;
    }

    return (reason == CARD_CTRL_ONEE_INHIBIT_RESET ||
            reason == CARD_CTRL_ONEE_INHIBIT_APPLE_POWER ||
            reason == CARD_CTRL_ONEE_INHIBIT_APPLE_ACTIVITY ||
            reason == CARD_CTRL_ONEE_INHIBIT_ACTIVITY_LOCKOUT) ? 1U : 0U;
}

static uint8_t onee_status_can_start(uint32_t status)
{
    const uint32_t reason = onee_inhibit_reason(status);

    if (onee_status_pl_ready(status) == 0U ||
        onee_status_has_hazard(status) != 0U ||
        (status & CARD_CTRL_ONEE_STATUS_QUIET_BIT) == 0U ||
        (status & CARD_CTRL_ONEE_STATUS_RESELECT_ARMED_BIT) == 0U ||
        (status & (CARD_CTRL_ONEE_STATUS_REQUEST_BIT |
                   CARD_CTRL_ONEE_STATUS_EFFECTIVE_BIT |
                   CARD_CTRL_ONEE_STATUS_SELECTED_BIT |
                   CARD_CTRL_ONEE_STATUS_ISOLATED_BIT)) != 0U) {
        return 0U;
    }

    return (reason == CARD_CTRL_ONEE_INHIBIT_NONE ||
            reason == CARD_CTRL_ONEE_INHIBIT_MANUAL_OFF) ? 1U : 0U;
}

static void onee_service_write_request(uint8_t enable)
{
    REG_WRITE(CARD_CTRL_ONEE_MODE_REG,
              (enable != 0U) ? CARD_CTRL_ONEE_CTRL_REQUEST_BIT : 0U);
}

static void onee_service_suspend_runtime(void)
{
    if (g_runtime_suspend != NULL) {
        g_runtime_suspend(g_runtime_ctx);
    }
    g_runtime_started = 0U;
}

static void onee_service_stop_runtime(void)
{
    if (g_runtime_stop != NULL) {
        g_runtime_stop(g_runtime_ctx);
    }
    g_runtime_started = 0U;
}

static void onee_service_disarm(uint8_t lock_out)
{
    g_manual_request = 0U;
    g_runtime_retry_polls = 0U;
    if (lock_out != 0U) {
        g_lockout_latched = 1U;
    }
    /* A terminal disarm clears session-only state, including a speed choice
     * queued before the soft core managed to start. */
    onee_service_stop_runtime();
    onee_service_write_request(0U);
}

void onee_service_init(void)
{
    g_manual_request = 0U;
    g_lockout_latched = 0U;
    g_runtime_started = 0U;
    g_runtime_retry_polls = 0U;
    g_runtime_start = NULL;
    g_runtime_suspend = NULL;
    g_runtime_stop = NULL;
    g_runtime_running = NULL;
    g_runtime_ctx = NULL;

    /* A card boot always starts with the session request off. */
    onee_service_write_request(0U);
    g_status = REG_READ(CARD_CTRL_ONEE_MODE_REG);
}

void onee_service_bind_runtime(onee_service_runtime_start_fn start,
                               onee_service_runtime_suspend_fn suspend,
                               onee_service_runtime_stop_fn stop,
                               onee_service_runtime_running_fn running,
                               void *ctx)
{
    if (g_runtime_started != 0U) {
        onee_service_stop_runtime();
    }
    g_runtime_start = start;
    g_runtime_suspend = suspend;
    g_runtime_stop = stop;
    g_runtime_running = running;
    g_runtime_ctx = ctx;
}

uint8_t onee_service_request_start(void)
{
    const uint32_t status = REG_READ(CARD_CTRL_ONEE_MODE_REG);

    g_status = status;
    if (g_runtime_start == NULL || g_runtime_suspend == NULL ||
        g_runtime_stop == NULL || g_runtime_running == NULL ||
        onee_status_can_start(status) == 0U) {
        onee_service_disarm(1U);
        return 0U;
    }

    g_lockout_latched = 0U;
    g_manual_request = 1U;
    g_runtime_retry_polls = 0U;

    /* This is the sole high write. It runs only for a fresh user action. */
    onee_service_write_request(1U);
    return 1U;
}

void onee_service_request_stop(void)
{
    g_lockout_latched = 0U;
    onee_service_disarm(0U);
}

void onee_service_poll(void)
{
    g_status = REG_READ(CARD_CTRL_ONEE_MODE_REG);

    if (g_manual_request == 0U) {
        /* Keep stale PL state off. This path never writes a high request. */
        if ((g_status & (CARD_CTRL_ONEE_STATUS_REQUEST_BIT |
                         CARD_CTRL_ONEE_STATUS_EFFECTIVE_BIT)) != 0U) {
            onee_service_write_request(0U);
        }
        if (g_runtime_started != 0U) {
            onee_service_stop_runtime();
        }
        return;
    }

    if (onee_status_pl_ready(g_status) == 0U ||
        onee_status_has_hazard(g_status) != 0U) {
        /* Activity cancels intent and latches the UI off. Quiet later does
         * not restart it; the operator must select the item again. */
        onee_service_disarm(1U);
        return;
    }

    /* The PL clears REQUEST when its sticky activity detector fires. Check
     * the echo as an event latch too: a short Apple-side pulse can finish and
     * the live activity bit can return quiet before this slow poll runs. Never
     * turn REQUEST back on here. Only a fresh menu action may do that. */
    if ((g_status & CARD_CTRL_ONEE_STATUS_REQUEST_BIT) == 0U) {
        onee_service_disarm(1U);
        return;
    }

    if ((g_status & CARD_CTRL_ONEE_STATUS_EFFECTIVE_BIT) != 0U) {
        if (g_runtime_started == 0U) {
            if (g_runtime_retry_polls != 0U) {
                --g_runtime_retry_polls;
                return;
            }
            if (g_runtime_start(g_runtime_ctx) == 0U) {
                /* A soft-core start fault is not an Apple-presence event.
                 * Keep the manual selection and physical isolation latched,
                 * force the failed runtime off, and try it again later. */
                onee_service_suspend_runtime();
                g_runtime_retry_polls = ONEE_RUNTIME_RETRY_POLL_LIMIT;
                return;
            }
            g_runtime_started = 1U;
            g_runtime_retry_polls = 0U;
        }
        if (g_runtime_running(g_runtime_ctx) == 0U) {
            /* Keep the selected mode. The next poll restarts the private
             * soft machine while REQUEST and the safety gate remain valid. */
            onee_service_suspend_runtime();
            g_runtime_retry_polls = 0U;
        }
        return;
    }

    /* REQUEST is still high and the guard reports no Apple hazard. Keep the
     * user's selection while isolation settles. There is no software expiry:
     * only the PL activity latch, an explicit manual OFF, or missing safety
     * logic may clear the selected mode. */
    if (g_runtime_started != 0U) {
        onee_service_suspend_runtime();
    }
    g_runtime_retry_polls = 0U;
}

onee_service_state_t onee_service_state(void)
{
    const uint32_t reason = onee_inhibit_reason(g_status);

    if (g_lockout_latched != 0U ||
        onee_status_pl_ready(g_status) == 0U ||
        onee_status_has_hazard(g_status) != 0U) {
        return ONEE_SERVICE_STATE_LOCKED;
    }
    if (g_manual_request != 0U &&
        (g_status & CARD_CTRL_ONEE_STATUS_EFFECTIVE_BIT) != 0U &&
        g_runtime_started != 0U && g_runtime_running != NULL &&
        g_runtime_running(g_runtime_ctx) != 0U) {
        return ONEE_SERVICE_STATE_RUNNING;
    }
    if (g_manual_request != 0U) {
        return ONEE_SERVICE_STATE_OFF;
    }
    if ((g_status & CARD_CTRL_ONEE_STATUS_QUIET_BIT) != 0U &&
        (g_status & CARD_CTRL_ONEE_STATUS_RESELECT_ARMED_BIT) != 0U &&
        (reason == CARD_CTRL_ONEE_INHIBIT_NONE ||
         reason == CARD_CTRL_ONEE_INHIBIT_MANUAL_OFF)) {
        return ONEE_SERVICE_STATE_OFF;
    }
    return ONEE_SERVICE_STATE_LOCKED;
}

uint32_t onee_service_status(void)
{
    return g_status;
}
