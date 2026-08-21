/* Persistent-intent ONE//e stand-alone supervisor client. */

#include "onee_service.h"

#include <stddef.h>

#include "card_control_regs.h"
#include "../lib/common.h"

#define ONEE_RUNTIME_RETRY_POLL_LIMIT 64U

static uint8_t g_manual_request;
static uint8_t g_lockout_latched;
static uint8_t g_runtime_started;
static uint8_t g_persisted_intent;
static uint8_t g_restore_pending;
static uint8_t g_persist_update_pending;
static uint8_t g_persist_update_value;
static uint32_t g_runtime_retry_polls;
static uint32_t g_status;
static onee_service_runtime_start_fn g_runtime_start;
static onee_service_runtime_suspend_fn g_runtime_suspend;
static onee_service_runtime_stop_fn g_runtime_stop;
static onee_service_runtime_running_fn g_runtime_running;
static void *g_runtime_ctx;
static onee_service_ui_pause_fn g_ui_pause;
static onee_service_ui_input_policy_fn g_ui_set_input_policy;
static onee_service_ui_input_released_fn g_ui_input_released;
static void *g_ui_ctx;
static uint8_t g_ui_menu_active;
static uint8_t g_ui_menu_paused;
static uint8_t g_ui_input_release_wait;
static uint8_t g_ui_fixed_mode;
static uint8_t g_ui_input_blocked;
static uint8_t g_ui_policy_published;

typedef enum {
    ONEE_START_CHECK_OK = 0,
    ONEE_START_CHECK_PL_MISSING,
    ONEE_START_CHECK_HAZARD,
    ONEE_START_CHECK_RUNTIME_UNBOUND,
    ONEE_START_CHECK_NOT_READY
} onee_start_check_t;

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

static onee_start_check_t onee_service_check_start(uint32_t status)
{
    if (onee_status_pl_ready(status) == 0U) {
        return ONEE_START_CHECK_PL_MISSING;
    }
    if (onee_status_has_hazard(status) != 0U) {
        return ONEE_START_CHECK_HAZARD;
    }
    if (g_runtime_start == NULL || g_runtime_suspend == NULL ||
        g_runtime_stop == NULL || g_runtime_running == NULL) {
        return ONEE_START_CHECK_RUNTIME_UNBOUND;
    }
    if (onee_status_can_start(status) == 0U) {
        return ONEE_START_CHECK_NOT_READY;
    }
    return ONEE_START_CHECK_OK;
}

static uint8_t onee_service_ui_selected(void)
{
    return (g_status & (CARD_CTRL_ONEE_STATUS_REQUEST_BIT |
                        CARD_CTRL_ONEE_STATUS_EFFECTIVE_BIT)) != 0U ? 1U : 0U;
}

static void onee_service_publish_input_policy(uint8_t fixed_mode,
                                              uint8_t blocked)
{
    fixed_mode = (fixed_mode != 0U) ? 1U : 0U;
    blocked = (blocked != 0U) ? 1U : 0U;
    if (g_ui_policy_published != 0U &&
        g_ui_fixed_mode == fixed_mode &&
        g_ui_input_blocked == blocked) {
        return;
    }
    g_ui_fixed_mode = fixed_mode;
    g_ui_input_blocked = blocked;
    g_ui_policy_published = 1U;
    if (g_ui_set_input_policy != NULL) {
        g_ui_set_input_policy(g_ui_ctx, fixed_mode, blocked);
    }
}

static void onee_service_update_ui_policy(void)
{
    if (onee_service_ui_selected() == 0U) {
        if (g_ui_menu_paused != 0U && g_ui_pause != NULL) {
            (void)g_ui_pause(g_ui_ctx, 0U);
        }
        g_ui_menu_paused = 0U;
        g_ui_input_release_wait = 0U;
        onee_service_publish_input_policy(0U, 0U);
        return;
    }

    if (g_ui_menu_active != 0U) {
        g_ui_input_release_wait = 0U;
        onee_service_publish_input_policy(1U, 1U);
        if (g_ui_menu_paused == 0U && g_ui_pause != NULL &&
            g_ui_pause(g_ui_ctx, 1U) != 0U) {
            g_ui_menu_paused = 1U;
        }
        return;
    }

    if (g_ui_menu_paused != 0U) {
        g_ui_input_release_wait = 1U;
        onee_service_publish_input_policy(1U, 1U);
        if (g_ui_input_released != NULL &&
            g_ui_input_released(g_ui_ctx) != 0U &&
            g_ui_pause != NULL && g_ui_pause(g_ui_ctx, 0U) != 0U) {
            g_ui_menu_paused = 0U;
            g_ui_input_release_wait = 0U;
            onee_service_publish_input_policy(1U, 0U);
        }
        return;
    }

    g_ui_input_release_wait = 0U;
    onee_service_publish_input_policy(1U, 0U);
}

static void onee_service_write_request(uint8_t enable)
{
    REG_WRITE(CARD_CTRL_ONEE_MODE_REG,
              (enable != 0U) ? CARD_CTRL_ONEE_CTRL_REQUEST_BIT : 0U);
}

static void onee_service_mark_persisted(uint8_t enable)
{
    const uint8_t value = (enable != 0U) ? 1U : 0U;

    if (g_persisted_intent == value &&
        (g_persist_update_pending == 0U ||
         g_persist_update_value == value)) {
        return;
    }
    g_persisted_intent = value;
    g_persist_update_value = value;
    g_persist_update_pending = 1U;
}

static void onee_service_force_persisted(uint8_t enable)
{
    const uint8_t value = (enable != 0U) ? 1U : 0U;

    /* The menu can have synced a new value before this service changes its
     * in-memory intent. A safety event or manual OFF must still queue OFF in
     * that window. */
    g_persisted_intent = value;
    g_persist_update_value = value;
    g_persist_update_pending = 1U;
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
    g_restore_pending = 0U;
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
    g_persisted_intent = 0U;
    g_restore_pending = 0U;
    g_persist_update_pending = 0U;
    g_persist_update_value = 0U;
    g_runtime_retry_polls = 0U;
    g_runtime_start = NULL;
    g_runtime_suspend = NULL;
    g_runtime_stop = NULL;
    g_runtime_running = NULL;
    g_runtime_ctx = NULL;
    g_ui_pause = NULL;
    g_ui_set_input_policy = NULL;
    g_ui_input_released = NULL;
    g_ui_ctx = NULL;
    g_ui_menu_active = 0U;
    g_ui_menu_paused = 0U;
    g_ui_input_release_wait = 0U;
    g_ui_fixed_mode = 0U;
    g_ui_input_blocked = 0U;
    g_ui_policy_published = 0U;

    /* A card boot starts with the PL request off. The saved intent is restored
     * later, after the global configuration file has been read. */
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

void onee_service_bind_ui_policy(
    onee_service_ui_pause_fn pause,
    onee_service_ui_input_policy_fn set_input_policy,
    onee_service_ui_input_released_fn input_released,
    void *ctx)
{
    g_ui_pause = pause;
    g_ui_set_input_policy = set_input_policy;
    g_ui_input_released = input_released;
    g_ui_ctx = ctx;
    g_ui_policy_published = 0U;
    onee_service_update_ui_policy();
}

void onee_service_sync_menu_policy(uint8_t menu_active)
{
    g_ui_menu_active = (menu_active != 0U) ? 1U : 0U;
    onee_service_update_ui_policy();
}

uint8_t onee_service_menu_paused(void)
{
    return g_ui_menu_paused;
}

uint8_t onee_service_input_release_wait(void)
{
    return g_ui_input_release_wait;
}

void onee_service_restore_persisted(uint8_t enable)
{
    /* A late SD-card attach may reload the global file. Never let that stale
     * file replace a newer manual action or Apple-event update which has not
     * yet reached storage. */
    if (g_manual_request != 0U || g_persist_update_pending != 0U ||
        g_lockout_latched != 0U) {
        return;
    }
    g_persisted_intent = (enable != 0U) ? 1U : 0U;
    g_restore_pending = g_persisted_intent;
    g_persist_update_pending = 0U;
    g_persist_update_value = g_persisted_intent;
}

uint8_t onee_service_persist_update_pending(uint8_t *enable)
{
    if (g_persist_update_pending == 0U) {
        return 0U;
    }
    if (enable != NULL) {
        *enable = g_persist_update_value;
    }
    return 1U;
}

void onee_service_persist_update_ack(uint8_t enable)
{
    if (g_persist_update_pending != 0U &&
        g_persist_update_value == ((enable != 0U) ? 1U : 0U)) {
        g_persist_update_pending = 0U;
    }
}

uint8_t onee_service_request_start(void)
{
    const uint32_t status = REG_READ(CARD_CTRL_ONEE_MODE_REG);

    g_status = status;
    if (onee_service_check_start(status) != ONEE_START_CHECK_OK) {
        /* The menu has already synced ON before asking us to raise REQUEST.
         * Every manual refusal must therefore queue a matching durable OFF,
         * not just refusals caused by live Apple-bus hazards. */
        onee_service_force_persisted(0U);
        onee_service_disarm(1U);
        return 0U;
    }

    g_lockout_latched = 0U;
    g_manual_request = 1U;
    g_restore_pending = 0U;
    g_runtime_retry_polls = 0U;
    onee_service_mark_persisted(1U);

    /* This high write runs only for a fresh user action. The only other high
     * write is the guarded one-shot restore in poll(). */
    onee_service_write_request(1U);
    return 1U;
}

void onee_service_request_stop(void)
{
    g_lockout_latched = 0U;
    /* The menu may have saved ON before a start request which this service
     * refused without first changing g_persisted_intent. Manual OFF must
     * still replace that staged value on storage. */
    onee_service_force_persisted(0U);
    onee_service_disarm(0U);
}

void onee_service_poll(void)
{
    g_status = REG_READ(CARD_CTRL_ONEE_MODE_REG);

    if (g_manual_request == 0U) {
        /* Keep stale PL state off before considering a one-shot restore. */
        if ((g_status & (CARD_CTRL_ONEE_STATUS_REQUEST_BIT |
                         CARD_CTRL_ONEE_STATUS_EFFECTIVE_BIT)) != 0U) {
            onee_service_write_request(0U);
        }
        if (g_runtime_started != 0U) {
            onee_service_stop_runtime();
        }

        if (g_restore_pending != 0U) {
            const onee_start_check_t start_check =
                onee_service_check_start(g_status);

            if (start_check == ONEE_START_CHECK_PL_MISSING) {
                /* A missing or wrong PL image cannot run ONE//e. Preserve the
                 * saved choice for a later good boot, but drive nothing. */
                return;
            }
            if (start_check == ONEE_START_CHECK_HAZARD) {
                /* An Apple-side event is the one condition which revokes the
                 * saved ON choice. The PL kill/lockout has already won. */
                onee_service_force_persisted(0U);
                onee_service_disarm(1U);
                return;
            }
            if (start_check != ONEE_START_CHECK_OK) {
                return;
            }

            /* One automatic high write is allowed per restored ON intent,
             * only after the exact same PL safety test as a manual start. */
            g_lockout_latched = 0U;
            g_manual_request = 1U;
            g_restore_pending = 0U;
            g_runtime_retry_polls = 0U;
            onee_service_write_request(1U);
        }
        return;
    }

    if (onee_status_pl_ready(g_status) == 0U) {
        /* Do not erase the saved choice for a missing or wrong PL image. The
         * current run is still terminal and cannot auto-restart this boot. */
        onee_service_disarm(1U);
        return;
    }

    if (onee_status_has_hazard(g_status) != 0U) {
        /* Activity cancels intent and latches the UI off. Quiet later does
         * not restart it; the operator must select the item again. */
        onee_service_force_persisted(0U);
        onee_service_disarm(1U);
        return;
    }

    /* The PL clears REQUEST when its sticky activity detector fires. Check
     * the echo as an event latch too: a short Apple-side pulse can finish and
     * the live activity bit can return quiet before this slow poll runs. Never
     * turn REQUEST back on here. Only a fresh menu action may do that. */
    if ((g_status & CARD_CTRL_ONEE_STATUS_REQUEST_BIT) == 0U) {
        onee_service_force_persisted(0U);
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
