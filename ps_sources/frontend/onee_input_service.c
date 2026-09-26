/* USB keyboard/joystick input for ONE//e, and joystick input for vTW. */

#include "onee_input_service.h"

#include <stddef.h>
#include <string.h>

#include "card_control_regs.h"
#include "cherryusb_platform.h"
#include "onee_service.h"
#include "usb_util.h"
#include "usb_hid.h"
#include "../lib/common.h"

#define ONEE_INPUT_KEY_TRACK_COUNT 8U
#define ONEE_INPUT_QUEUE_DEPTH 32U
/* Use wall time because the frontend poll rate changes with display, storage,
 * and USB work. Poll counts made a short tap repeat on a fast main loop. */
#define ONEE_INPUT_REPEAT_DELAY_MS 500U
#define ONEE_INPUT_REPEAT_RATE_MS  100U

#define ONEE_INPUT_KEY_FIFO_REG       CARD_CTRL_REG_ADDR(0x5CU)
#define ONEE_INPUT_LIVE_REG           CARD_CTRL_REG_ADDR(0x5DU)
#define ONEE_INPUT_PADDLES_REG        CARD_CTRL_REG_ADDR(0x5EU)
#define ONEE_INPUT_CONTROL_REG        CARD_CTRL_REG_ADDR(0x5FU)

#define ONEE_INPUT_LIVE_ANY_KEY_BIT       (1UL << 0)
#define ONEE_INPUT_LIVE_OPEN_APPLE_BIT    (1UL << 1)
#define ONEE_INPUT_LIVE_CLOSED_APPLE_BIT  (1UL << 2)
#define ONEE_INPUT_LIVE_BUTTON_SHIFT      3U

#define ONEE_INPUT_CONTROL_OVERFLOW_CLEAR_BIT (1UL << 1)
#define ONEE_INPUT_CONTROL_FIFO_FLUSH_BIT     (1UL << 2)
#define ONEE_INPUT_CONTROL_RELEASE_LIVE_BIT   (1UL << 3)
#define ONEE_INPUT_FIFO_STATUS_FULL_BIT       (1UL << 12)
#define ONEE_INPUT_STATUS_ENABLED_BIT         (1UL << 9)
#define ONEE_INPUT_STATUS_SIGNATURE_SHIFT     24U
#define ONEE_INPUT_STATUS_SIGNATURE           0xE1U

#define ONEE_INPUT_NEUTRAL_PADDLES 0x80808080UL

#define HID_MOD_LCTRL  (1U << 0)
#define HID_MOD_LSHIFT (1U << 1)
#define HID_MOD_LALT   (1U << 2)
#define HID_MOD_LGUI   (1U << 3)
#define HID_MOD_RCTRL  (1U << 4)
#define HID_MOD_RSHIFT (1U << 5)
#define HID_MOD_RALT   (1U << 6)
#define HID_MOD_RGUI   (1U << 7)
#define HID_MOD_CTRL   (HID_MOD_LCTRL | HID_MOD_RCTRL)
#define HID_MOD_SHIFT  (HID_MOD_LSHIFT | HID_MOD_RSHIFT)
#define HID_MOD_ALT    (HID_MOD_LALT | HID_MOD_RALT)

typedef struct {
    uint8_t keyboard_seen;
    uint8_t modifier;
    uint8_t key_count;
    uint8_t keys[ONEE_INPUT_KEY_TRACK_COUNT];
    uint32_t key_press_order[ONEE_INPUT_KEY_TRACK_COUNT];
    uint8_t reset_chord_down;
    uint8_t reset_delete_consumed;
    uint8_t reset_modifier_consumed;
    uint8_t joystick_seen;
    uint8_t joystick_buttons;
    uint8_t joystick_axis_valid;
    uint8_t joystick_axes[ONEE_INPUT_AXIS_COUNT];
} onee_input_slot_t;

static onee_input_slot_t g_slots[ONEE_INPUT_DEVICE_SLOT_COUNT];
static uint8_t g_key_queue[ONEE_INPUT_QUEUE_DEPTH];
static uint8_t g_key_queue_read;
static uint8_t g_key_queue_write;
static uint8_t g_key_queue_count;
static uint32_t g_key_queue_drop_count;
static uint8_t g_caps_lock;
static uint8_t g_session_active;
static uint8_t g_live_dirty;
static uint8_t g_paddles_dirty;
static uint8_t g_vtw_joystick_active;
static uint8_t g_vtw_joystick_dirty;
static uint8_t g_input_blocked;
static onee_input_joystick_config_t g_joystick_config;
static uint8_t g_cold_reboot_pending;
static uint32_t g_repeat_press_sequence;
static uint32_t g_repeat_deadline_ms;
static uint8_t g_repeat_valid;
static uint8_t g_repeat_slot;
static uint8_t g_repeat_usage;

static uint8_t onee_key_in_report(uint8_t key,
                                  const uint8_t *keys,
                                  uint32_t count)
{
    if (keys == NULL || key <= HID_KBD_USAGE_ERRUNDEF) {
        return 0U;
    }
    for (uint32_t i = 0U; i < count; ++i) {
        if (keys[i] == key) {
            return 1U;
        }
    }
    return 0U;
}

static uint8_t onee_input_bridge_active(void)
{
    uint32_t mode;
    uint32_t input_status;

    if (onee_service_state() != ONEE_SERVICE_STATE_RUNNING) {
        return 0U;
    }
    mode = REG_READ(CARD_CTRL_ONEE_MODE_REG);
    if ((mode & CARD_CTRL_ONEE_STATUS_EFFECTIVE_BIT) == 0U) {
        return 0U;
    }
    input_status = REG_READ(ONEE_INPUT_CONTROL_REG);
    return (((input_status >> ONEE_INPUT_STATUS_SIGNATURE_SHIFT) & 0xFFU) ==
                ONEE_INPUT_STATUS_SIGNATURE &&
            (input_status & ONEE_INPUT_STATUS_ENABLED_BIT) != 0U) ? 1U : 0U;
}

static void onee_key_queue_clear(void)
{
    g_key_queue_read = 0U;
    g_key_queue_write = 0U;
    g_key_queue_count = 0U;
}

static void onee_key_queue_push(uint8_t code)
{
    if (code == 0U) {
        return;
    }
    if (g_key_queue_count >= ONEE_INPUT_QUEUE_DEPTH) {
        ++g_key_queue_drop_count;
        return;
    }
    g_key_queue[g_key_queue_write] = (uint8_t)(code & 0x7FU);
    g_key_queue_write = (uint8_t)((g_key_queue_write + 1U) %
                                  ONEE_INPUT_QUEUE_DEPTH);
    ++g_key_queue_count;
}

static uint8_t onee_ascii_from_usage(uint8_t usage,
                                     uint8_t modifier,
                                     uint8_t caps_lock)
{
    static const uint8_t unshifted_number[10] = {
        '1', '2', '3', '4', '5', '6', '7', '8', '9', '0'
    };
    static const uint8_t shifted_number[10] = {
        '!', '@', '#', '$', '%', '^', '&', '*', '(', ')'
    };
    static const uint8_t unshifted_punctuation[12] = {
        '-', '=', '[', ']', '\\', '#', ';', '\'', '`', ',', '.', '/'
    };
    static const uint8_t shifted_punctuation[12] = {
        '_', '+', '{', '}', '|', '~', ':', '"', '~', '<', '>', '?'
    };
    const uint8_t ctrl = (uint8_t)((modifier & HID_MOD_CTRL) != 0U);
    const uint8_t shift = (uint8_t)((modifier & HID_MOD_SHIFT) != 0U);

    if (usage >= HID_KBD_USAGE_A && usage < HID_KBD_USAGE_A + 26U) {
        uint8_t letter = (uint8_t)(usage - HID_KBD_USAGE_A);
        if (ctrl != 0U) {
            return (uint8_t)(letter + 1U);
        }
        return (uint8_t)(((shift ^ caps_lock) != 0U ? 'A' : 'a') + letter);
    }
    if (usage >= HID_KBD_USAGE_1 && usage <= HID_KBD_USAGE_0) {
        uint8_t index = (uint8_t)(usage - HID_KBD_USAGE_1);
        return (shift != 0U) ? shifted_number[index] :
                              unshifted_number[index];
    }
    if (usage >= HID_KBD_USAGE_HYPHEN && usage <= HID_KBD_USAGE_DIV) {
        uint8_t index = (uint8_t)(usage - HID_KBD_USAGE_HYPHEN);
        uint8_t result = (shift != 0U) ? shifted_punctuation[index] :
                                        unshifted_punctuation[index];
        if (ctrl != 0U) {
            switch (result) {
            case '[': return 0x1BU;
            case '\\': return 0x1CU;
            case ']': return 0x1DU;
            case '^': return 0x1EU;
            case '_': return 0x1FU;
            default: break;
            }
        }
        return result;
    }

    switch (usage) {
    case HID_KBD_USAGE_ENTER:     return 0x0DU;
    case HID_KBD_USAGE_ESCAPE:    return 0x1BU;
    case HID_KBD_USAGE_DELETE:    return 0x08U;
    case HID_KBD_USAGE_TAB:       return 0x09U;
    case HID_KBD_USAGE_SPACE:     return 0x20U;
    case HID_KBD_USAGE_DELFWD:    return 0x7FU;
    case HID_KBD_USAGE_RIGHT:     return 0x15U;
    case HID_KBD_USAGE_LEFT:      return 0x08U;
    case HID_KBD_USAGE_DOWN:      return 0x0AU;
    case HID_KBD_USAGE_UP:        return 0x0BU;
    case HID_KBD_USAGE_KPDDIV:    return '/';
    case HID_KBD_USAGE_KPDMUL:    return '*';
    case HID_KBD_USAGE_KPDHMINUS: return '-';
    case HID_KBD_USAGE_KPDPLUS:   return '+';
    case HID_KBD_USAGE_KPDEMTER:  return 0x0DU;
    case HID_KBD_USAGE_KPD0:      return '0';
    case HID_KBD_USAGE_KPDDECIMALPT: return '.';
    default:
        if (usage >= HID_KBD_USAGE_KPD1 &&
            usage < HID_KBD_USAGE_KPD1 + 9U) {
            return (uint8_t)('1' + usage - HID_KBD_USAGE_KPD1);
        }
        return 0U;
    }
}

static void onee_repeat_cancel(void)
{
    g_repeat_valid = 0U;
    g_repeat_slot = 0U;
    g_repeat_usage = 0U;
    g_repeat_deadline_ms = 0U;
}

static uint8_t onee_time_reached(uint32_t now, uint32_t deadline)
{
    return ((int32_t)(now - deadline) >= 0) ? 1U : 0U;
}

static void onee_repeat_clear_press_orders(void)
{
    for (uint8_t slot = 0U; slot < ONEE_INPUT_DEVICE_SLOT_COUNT; ++slot) {
        memset(g_slots[slot].key_press_order,
               0,
               sizeof(g_slots[slot].key_press_order));
    }
    g_repeat_press_sequence = 0U;
    onee_repeat_cancel();
}

static uint8_t onee_repeatable_usage(uint8_t usage,
                                     uint8_t modifier)
{
    if (usage == HID_KBD_USAGE_CAPSLOCK ||
        usage == HID_KBD_USAGE_PAUSE) {
        return 0U;
    }
    return (onee_ascii_from_usage(usage, modifier, g_caps_lock) != 0U) ?
           1U : 0U;
}

static void onee_repeat_reselect(void)
{
    uint32_t best_order = 0U;
    uint8_t best_slot = 0U;
    uint8_t best_usage = 0U;

    if (g_session_active == 0U) {
        onee_repeat_cancel();
        return;
    }
    for (uint8_t slot = 0U; slot < ONEE_INPUT_DEVICE_SLOT_COUNT; ++slot) {
        const onee_input_slot_t *input = &g_slots[slot];
        for (uint8_t key = 0U; key < input->key_count; ++key) {
            const uint32_t order = input->key_press_order[key];
            const uint8_t usage = input->keys[key];
            if (order > best_order &&
                onee_repeatable_usage(usage, input->modifier) != 0U) {
                best_order = order;
                best_slot = slot;
                best_usage = usage;
            }
        }
    }
    if (best_order == 0U) {
        onee_repeat_cancel();
        return;
    }
    if (g_repeat_valid == 0U ||
        g_repeat_slot != best_slot ||
        g_repeat_usage != best_usage) {
        g_repeat_valid = 1U;
        g_repeat_slot = best_slot;
        g_repeat_usage = best_usage;
        g_repeat_deadline_ms = cherryusb_baremetal_ms() +
                               ONEE_INPUT_REPEAT_DELAY_MS;
    }
}

static void onee_repeat_poll(void)
{
    uint32_t now;
    uint8_t code;

    if (g_repeat_valid == 0U ||
        g_repeat_slot >= ONEE_INPUT_DEVICE_SLOT_COUNT) {
        return;
    }
    now = cherryusb_baremetal_ms();
    if (onee_time_reached(now, g_repeat_deadline_ms) == 0U) {
        return;
    }

    /* Repeat uses the key's current modifier byte and the current Caps Lock
     * state. Changing Shift or Control while a key stays down therefore
     * changes the next repeated character without creating a new key edge. */
    code = onee_ascii_from_usage(g_repeat_usage,
                                  g_slots[g_repeat_slot].modifier,
                                  g_caps_lock);
    if (code == 0U ||
        onee_repeatable_usage(g_repeat_usage,
                              g_slots[g_repeat_slot].modifier) == 0U) {
        onee_repeat_reselect();
        return;
    }
    onee_key_queue_push(code);
    /* Schedule from now, not the old deadline. If other frontend work stalls
     * polling, resume with one repeat instead of a queue-filling catch-up. */
    g_repeat_deadline_ms = now + ONEE_INPUT_REPEAT_RATE_MS;
}

static uint8_t onee_normalize_axis(int32_t value,
                                   int32_t logical_min,
                                   int32_t logical_max)
{
    int64_t position;
    int64_t span;

    if (logical_max <= logical_min) {
        return 0x80U;
    }
    if (value < logical_min) {
        value = logical_min;
    } else if (value > logical_max) {
        value = logical_max;
    }
    position = (int64_t)value - (int64_t)logical_min;
    span = (int64_t)logical_max - (int64_t)logical_min;
    return (uint8_t)((position * 255 + (span / 2)) / span);
}

static uint32_t onee_live_word(void)
{
    uint32_t live = 0U;
    uint8_t joystick_owner = ONEE_INPUT_DEVICE_SLOT_COUNT;

    for (uint8_t i = 0U; i < ONEE_INPUT_DEVICE_SLOT_COUNT; ++i) {
        const onee_input_slot_t *slot = &g_slots[i];
        if (slot->keyboard_seen != 0U) {
            uint8_t live_key = 0U;

            for (uint8_t key = 0U; key < slot->key_count; ++key) {
                if (slot->keys[key] != HID_KBD_USAGE_DELFWD ||
                    slot->reset_delete_consumed == 0U) {
                    live_key = 1U;
                    break;
                }
            }
            if (live_key != 0U ||
                (slot->modifier & (HID_MOD_CTRL | HID_MOD_SHIFT)) != 0U) {
                live |= ONEE_INPUT_LIVE_ANY_KEY_BIT;
            }
            if ((slot->modifier & HID_MOD_LALT) != 0U) {
                live |= ONEE_INPUT_LIVE_OPEN_APPLE_BIT;
            }
            if ((slot->modifier & HID_MOD_RALT) != 0U) {
                live |= ONEE_INPUT_LIVE_CLOSED_APPLE_BIT;
            }
        }
        if (joystick_owner == ONEE_INPUT_DEVICE_SLOT_COUNT &&
            slot->joystick_seen != 0U) {
            joystick_owner = i;
        }
    }
    if (joystick_owner != ONEE_INPUT_DEVICE_SLOT_COUNT &&
        g_input_blocked == 0U) {
        live |= (uint32_t)(g_slots[joystick_owner].joystick_buttons & 0x07U)
                << ONEE_INPUT_LIVE_BUTTON_SHIFT;
    }
    return live;
}

static uint8_t onee_axis_or_neutral(const onee_input_slot_t *slot,
                                    onee_input_axis_t preferred,
                                    onee_input_axis_t fallback)
{
    if ((slot->joystick_axis_valid & (uint8_t)(1U << preferred)) != 0U) {
        return slot->joystick_axes[preferred];
    }
    if ((slot->joystick_axis_valid & (uint8_t)(1U << fallback)) != 0U) {
        return slot->joystick_axes[fallback];
    }
    return 0x80U;
}

static uint8_t onee_mapped_paddle(const onee_input_slot_t *slot, uint8_t paddle)
{
    static const onee_input_axis_t preferred[ONEE_INPUT_PADDLE_COUNT] = {
        ONEE_INPUT_AXIS_X, ONEE_INPUT_AXIS_Y,
        ONEE_INPUT_AXIS_RX, ONEE_INPUT_AXIS_RY
    };
    static const onee_input_axis_t fallback[ONEE_INPUT_PADDLE_COUNT] = {
        ONEE_INPUT_AXIS_X, ONEE_INPUT_AXIS_Y,
        ONEE_INPUT_AXIS_Z, ONEE_INPUT_AXIS_RZ
    };
    const onee_input_joystick_paddle_config_t *config =
        &g_joystick_config.paddle[paddle];
    onee_input_axis_t axis;
    int32_t value;
    int32_t distance;
    int32_t side;
    int32_t deadzone;
    int32_t magnitude;

    if (config->source == ONEE_INPUT_JOYSTICK_SOURCE_OFF) {
        return 0x80U;
    }
    if (config->source == ONEE_INPUT_JOYSTICK_SOURCE_AUTO) {
        axis = preferred[paddle];
        if ((slot->joystick_axis_valid & (uint8_t)(1U << axis)) == 0U) {
            axis = fallback[paddle];
        }
    } else {
        axis = (onee_input_axis_t)(config->source -
                                 ONEE_INPUT_JOYSTICK_SOURCE_X);
    }
    /* Missing/disabled axes stay centered even when inversion is selected. */
    if ((slot->joystick_axis_valid & (uint8_t)(1U << axis)) == 0U) {
        return 0x80U;
    }
    value = onee_axis_or_neutral(slot, axis, axis);
    if (config->invert != 0U) {
        value = 255 - value;
    }
    distance = value - 128;
    side = (distance < 0) ? 128 : 127;
    magnitude = (distance < 0) ? -distance : distance;
    deadzone = (side * config->deadzone_percent + 50) / 100;
    if (magnitude <= deadzone) {
        return 0x80U;
    }
    magnitude = ((magnitude - deadzone) * side + (side - deadzone) / 2) /
                (side - deadzone);
    magnitude = (magnitude * config->sensitivity_percent + 50) / 100;
    if (magnitude > side) {
        magnitude = side;
    }
    return (uint8_t)((distance < 0) ? 128 - magnitude : 128 + magnitude);
}

static uint32_t onee_paddles_word(void)
{
    const onee_input_slot_t *slot = NULL;

    for (uint8_t i = 0U; i < ONEE_INPUT_DEVICE_SLOT_COUNT; ++i) {
        if (g_slots[i].joystick_seen != 0U) {
            slot = &g_slots[i];
            break;
        }
    }
    if (slot == NULL) {
        return ONEE_INPUT_NEUTRAL_PADDLES;
    }
    return (uint32_t)onee_mapped_paddle(slot, 0U) |
           ((uint32_t)onee_mapped_paddle(slot, 1U) << 8) |
           ((uint32_t)onee_mapped_paddle(slot, 2U) << 16) |
           ((uint32_t)onee_mapped_paddle(slot, 3U) << 24);
}

static void vtw_joystick_poll(void)
{
    const uint32_t status = REG_READ(CARD_CTRL_VTW_JOYSTICK_CONTROL_REG);
    uint32_t control = 0U;

    if (((status >> CARD_CTRL_VTW_JOYSTICK_SIGNATURE_SHIFT) & 0xFFU) !=
            CARD_CTRL_VTW_JOYSTICK_SIGNATURE ||
        (status & CARD_CTRL_VTW_JOYSTICK_ENABLED_BIT) == 0U) {
        g_vtw_joystick_active = 0U;
        return;
    }
    if (g_vtw_joystick_active == 0U) {
        g_vtw_joystick_active = 1U;
        g_vtw_joystick_dirty = 1U;
    }
    /* Match the paddle owner's slot. Use only raw joystick buttons: Alt,
     * Apple keys, and keyboard events belong to the ONE//e bridge. */
    for (uint8_t i = 0U; i < ONEE_INPUT_DEVICE_SLOT_COUNT; ++i) {
        if (g_slots[i].joystick_seen != 0U) {
            /* Menus keep the virtual port present and centered rather than
             * exposing physical inputs while guest delivery is blocked. */
            control = CARD_CTRL_VTW_JOYSTICK_PRESENT_BIT;
            if (g_input_blocked == 0U) {
                control |= (uint32_t)(g_slots[i].joystick_buttons &
                                     CARD_CTRL_VTW_JOYSTICK_BUTTON_MASK) <<
                           CARD_CTRL_VTW_JOYSTICK_BUTTON_SHIFT;
            }
            break;
        }
    }
    /* The mode may leave and return between polls. Hardware clears presence
     * then, so compare the live commit state even without a fresh report. */
    if (g_vtw_joystick_dirty == 0U &&
        (status & CARD_CTRL_VTW_JOYSTICK_STATE_MASK) == control) {
        return;
    }
    /* CONTROL commits both values on one PL edge; the first write only
     * stages paddles, including neutral values when the last owner leaves. */
    REG_WRITE(CARD_CTRL_VTW_JOYSTICK_PADDLES_REG,
              (g_input_blocked != 0U) ? ONEE_INPUT_NEUTRAL_PADDLES :
                                       onee_paddles_word());
    REG_WRITE(CARD_CTRL_VTW_JOYSTICK_CONTROL_REG, control);
    g_vtw_joystick_dirty = 0U;
}

static void onee_input_session_stop(void)
{
    g_session_active = 0U;
    g_caps_lock = 0U;
    g_cold_reboot_pending = 0U;
    onee_key_queue_clear();
    onee_repeat_clear_press_orders();
}

void onee_input_service_default_joystick_config(
    onee_input_joystick_config_t *config)
{
    if (config == NULL) {
        return;
    }
    memset(config, 0, sizeof(*config));
    for (uint8_t i = 0U; i < ONEE_INPUT_PADDLE_COUNT; ++i) {
        config->paddle[i].source = ONEE_INPUT_JOYSTICK_SOURCE_AUTO;
        config->paddle[i].sensitivity_percent = 100U;
    }
}

void onee_input_service_set_joystick_config(
    const onee_input_joystick_config_t *config)
{
    onee_input_joystick_config_t next;

    onee_input_service_default_joystick_config(&next);
    if (config != NULL) {
        for (uint8_t i = 0U; i < ONEE_INPUT_PADDLE_COUNT; ++i) {
            next.paddle[i] = config->paddle[i];
            if (next.paddle[i].source > ONEE_INPUT_JOYSTICK_SOURCE_OFF) {
                next.paddle[i].source = ONEE_INPUT_JOYSTICK_SOURCE_AUTO;
            }
            next.paddle[i].invert = (next.paddle[i].invert != 0U) ? 1U : 0U;
            if (next.paddle[i].sensitivity_percent <
                ONEE_INPUT_JOYSTICK_SENSITIVITY_MIN) {
                next.paddle[i].sensitivity_percent =
                    ONEE_INPUT_JOYSTICK_SENSITIVITY_MIN;
            } else if (next.paddle[i].sensitivity_percent >
                       ONEE_INPUT_JOYSTICK_SENSITIVITY_MAX) {
                next.paddle[i].sensitivity_percent =
                    ONEE_INPUT_JOYSTICK_SENSITIVITY_MAX;
            }
            if (next.paddle[i].deadzone_percent >
                ONEE_INPUT_JOYSTICK_DEADZONE_MAX) {
                next.paddle[i].deadzone_percent =
                    ONEE_INPUT_JOYSTICK_DEADZONE_MAX;
            }
        }
    }
    g_joystick_config = next;
    g_paddles_dirty = 1U;
    g_vtw_joystick_dirty = 1U;
}

void onee_input_service_get_joystick_config(
    onee_input_joystick_config_t *config)
{
    if (config != NULL) {
        *config = g_joystick_config;
    }
}

void onee_input_service_get_joystick_snapshot(
    onee_input_joystick_snapshot_t *snapshot)
{
    uint32_t paddles;

    if (snapshot == NULL) {
        return;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->owner_slot = ONEE_INPUT_DEVICE_SLOT_COUNT;
    snapshot->active = g_vtw_joystick_active;
    memset(snapshot->axis, 0x80, sizeof(snapshot->axis));
    for (uint8_t i = 0U; i < ONEE_INPUT_DEVICE_SLOT_COUNT; ++i) {
        const onee_input_slot_t *slot = &g_slots[i];
        if (slot->joystick_seen == 0U) {
            continue;
        }
        snapshot->connected = 1U;
        snapshot->owner_slot = i;
        snapshot->axis_valid_mask = slot->joystick_axis_valid;
        snapshot->buttons = slot->joystick_buttons;
        for (uint8_t axis = 0U; axis < ONEE_INPUT_AXIS_COUNT; ++axis) {
            snapshot->axis[axis] = onee_axis_or_neutral(
                slot, (onee_input_axis_t)axis, (onee_input_axis_t)axis);
        }
        break;
    }
    paddles = onee_paddles_word();
    for (uint8_t i = 0U; i < ONEE_INPUT_PADDLE_COUNT; ++i) {
        snapshot->paddles[i] = (uint8_t)(paddles >> (i * 8U));
    }
}

void onee_input_service_init(void)
{
    memset(g_slots, 0, sizeof(g_slots));
    memset(g_key_queue, 0, sizeof(g_key_queue));
    g_key_queue_drop_count = 0U;
    g_caps_lock = 0U;
    g_session_active = 0U;
    g_live_dirty = 1U;
    g_paddles_dirty = 1U;
    g_vtw_joystick_active = 0U;
    g_vtw_joystick_dirty = 1U;
    g_input_blocked = 0U;
    onee_input_service_default_joystick_config(&g_joystick_config);
    g_cold_reboot_pending = 0U;
    g_repeat_press_sequence = 0U;
    onee_repeat_cancel();
    onee_key_queue_clear();
}

void onee_input_service_poll(void)
{
    uint32_t fifo_status;

    vtw_joystick_poll();
    if (onee_input_bridge_active() == 0U) {
        if (g_session_active != 0U) {
            onee_input_session_stop();
        }
        return;
    }

    if (g_session_active == 0U) {
        g_session_active = 1U;
        g_caps_lock = 0U;
        g_cold_reboot_pending = 0U;
        onee_key_queue_clear();
        onee_repeat_clear_press_orders();
        REG_WRITE(ONEE_INPUT_CONTROL_REG,
                  ONEE_INPUT_CONTROL_OVERFLOW_CLEAR_BIT |
                  ONEE_INPUT_CONTROL_FIFO_FLUSH_BIT |
                  ONEE_INPUT_CONTROL_RELEASE_LIVE_BIT);
        g_live_dirty = 1U;
        g_paddles_dirty = 1U;
    }

    if (g_live_dirty != 0U) {
        REG_WRITE(ONEE_INPUT_LIVE_REG, onee_live_word());
        g_live_dirty = 0U;
    }
    if (g_paddles_dirty != 0U) {
        REG_WRITE(ONEE_INPUT_PADDLES_REG,
                  (g_input_blocked != 0U) ? ONEE_INPUT_NEUTRAL_PADDLES :
                                           onee_paddles_word());
        g_paddles_dirty = 0U;
    }
    onee_repeat_poll();

    if (g_key_queue_count == 0U) {
        return;
    }
    fifo_status = REG_READ(ONEE_INPUT_KEY_FIFO_REG);
    if ((fifo_status & ONEE_INPUT_FIFO_STATUS_FULL_BIT) == 0U) {
        REG_WRITE(ONEE_INPUT_KEY_FIFO_REG, g_key_queue[g_key_queue_read]);
        g_key_queue_read = (uint8_t)((g_key_queue_read + 1U) %
                                     ONEE_INPUT_QUEUE_DEPTH);
        --g_key_queue_count;
    }
}

uint8_t onee_input_service_take_cold_reboot_request(void)
{
    const uint8_t pending = g_cold_reboot_pending;

    g_cold_reboot_pending = 0U;
    return pending;
}

void onee_input_service_prepare_cold_reboot(void)
{
    onee_key_queue_clear();
    onee_repeat_clear_press_orders();
    if (onee_input_bridge_active() != 0U) {
        REG_WRITE(ONEE_INPUT_CONTROL_REG,
                  ONEE_INPUT_CONTROL_FIFO_FLUSH_BIT |
                  ONEE_INPUT_CONTROL_RELEASE_LIVE_BIT);
    }
    g_live_dirty = 1U;
    g_paddles_dirty = 1U;
}

uint8_t onee_input_service_keyboard_report(uint8_t slot_index,
                                           uint8_t modifier,
                                           const uint8_t *keys,
                                           uint32_t key_count)
{
    onee_input_slot_t *slot;
    uint8_t next_keys[ONEE_INPUT_KEY_TRACK_COUNT];
    uint32_t next_press_order[ONEE_INPUT_KEY_TRACK_COUNT];
    uint8_t next_count = 0U;
    uint8_t reset_chord;
    uint8_t apple_modifier;
    uint8_t delete_down;

    if (slot_index >= ONEE_INPUT_DEVICE_SLOT_COUNT ||
        (keys == NULL && key_count != 0U)) {
        return 0U;
    }
    if (key_count > ONEE_INPUT_KEY_TRACK_COUNT) {
        key_count = ONEE_INPUT_KEY_TRACK_COUNT;
    }
    slot = &g_slots[slot_index];
    memset(next_keys, 0, sizeof(next_keys));
    memset(next_press_order, 0, sizeof(next_press_order));
    for (uint32_t i = 0U; i < key_count; ++i) {
        const uint8_t usage = keys[i];
        if (usage <= HID_KBD_USAGE_ERRUNDEF ||
            usage > HID_KBD_USAGE_MAX ||
            onee_key_in_report(usage, next_keys, next_count) != 0U) {
            continue;
        }
        next_keys[next_count++] = usage;
    }
    for (uint8_t next = 0U; next < next_count; ++next) {
        for (uint8_t old = 0U; old < slot->key_count; ++old) {
            if (next_keys[next] == slot->keys[old]) {
                next_press_order[next] = slot->key_press_order[old];
                break;
            }
        }
    }
    delete_down = onee_key_in_report(HID_KBD_USAGE_DELFWD,
                                     next_keys,
                                     next_count);
    reset_chord = (uint8_t)(g_session_active != 0U &&
        (modifier & HID_MOD_CTRL) != 0U &&
        (modifier & HID_MOD_ALT) != 0U &&
        delete_down != 0U);

    /* Once Ctrl+Alt+Delete forms, keep each chord member hidden from the
     * Apple side until that member is released. This covers every release
     * order and prevents Alt from appearing as an Apple key after reset. */
    slot->reset_modifier_consumed &= modifier;
    if (delete_down == 0U) {
        slot->reset_delete_consumed = 0U;
    }
    if (reset_chord != 0U) {
        slot->reset_modifier_consumed |=
            (uint8_t)(modifier & (HID_MOD_CTRL | HID_MOD_ALT));
        slot->reset_delete_consumed = 1U;
    }
    apple_modifier =
        (uint8_t)(modifier & (uint8_t)~slot->reset_modifier_consumed);

    if (reset_chord != 0U && slot->reset_chord_down == 0U) {
        /* A reset supersedes any translated key which has not reached PL. */
        onee_key_queue_clear();
        g_cold_reboot_pending = 1U;
    }

    if (slot->reset_delete_consumed != 0U) {
        for (uint8_t next = 0U; next < next_count; ++next) {
            if (next_keys[next] == HID_KBD_USAGE_DELFWD) {
                next_press_order[next] = 0U;
            }
        }
    }

    if (g_session_active != 0U) {
        for (uint32_t i = 0U; i < next_count; ++i) {
            const uint8_t usage = next_keys[i];
            uint8_t code;

            if (onee_key_in_report(usage,
                                   slot->keys,
                                   slot->key_count) != 0U) {
                continue;
            }
            if (usage == HID_KBD_USAGE_CAPSLOCK) {
                g_caps_lock ^= 1U;
                continue;
            }
            if (usage == HID_KBD_USAGE_DELFWD &&
                slot->reset_delete_consumed != 0U) {
                continue;
            }
            code = onee_ascii_from_usage(usage,
                                         apple_modifier,
                                         g_caps_lock);
            onee_key_queue_push(code);
            if (code != 0U) {
                ++g_repeat_press_sequence;
                if (g_repeat_press_sequence == 0U) {
                    ++g_repeat_press_sequence;
                }
                next_press_order[i] = g_repeat_press_sequence;
            }
        }
    }

    slot->keyboard_seen = 1U;
    slot->modifier = apple_modifier;
    slot->reset_chord_down = reset_chord;
    slot->key_count = next_count;
    memset(slot->keys, 0, sizeof(slot->keys));
    if (next_count != 0U) {
        memcpy(slot->keys, next_keys, next_count);
        memcpy(slot->key_press_order,
               next_press_order,
               next_count * sizeof(next_press_order[0]));
    }
    if (next_count < ONEE_INPUT_KEY_TRACK_COUNT) {
        memset(&slot->key_press_order[next_count],
               0,
               (ONEE_INPUT_KEY_TRACK_COUNT - next_count) *
                   sizeof(slot->key_press_order[0]));
    }
    onee_repeat_reselect();
    g_live_dirty = 1U;
    return reset_chord;
}

void onee_input_service_joystick_report(
    uint8_t slot_index,
    const onee_input_joystick_report_t *report)
{
    onee_input_slot_t *slot;

    if (slot_index >= ONEE_INPUT_DEVICE_SLOT_COUNT || report == NULL) {
        return;
    }
    slot = &g_slots[slot_index];
    slot->joystick_seen = 1U;
    if (report->buttons_valid != 0U) {
        slot->joystick_buttons = (uint8_t)(report->buttons & 0x07U);
    }
    for (uint8_t axis = 0U; axis < ONEE_INPUT_AXIS_COUNT; ++axis) {
        if ((report->axis_valid_mask & (uint8_t)(1U << axis)) == 0U) {
            continue;
        }
        slot->joystick_axes[axis] = onee_normalize_axis(
            report->axis[axis],
            report->logical_min[axis],
            report->logical_max[axis]);
        slot->joystick_axis_valid |= (uint8_t)(1U << axis);
    }
    g_live_dirty = 1U;
    g_paddles_dirty = 1U;
    g_vtw_joystick_dirty = 1U;
}

void onee_input_service_disconnect(uint8_t slot_index)
{
    if (slot_index >= ONEE_INPUT_DEVICE_SLOT_COUNT) {
        return;
    }
    if (g_slots[slot_index].joystick_seen != 0U) {
        g_vtw_joystick_dirty = 1U;
    }
    memset(&g_slots[slot_index], 0, sizeof(g_slots[slot_index]));
    onee_repeat_reselect();
    g_live_dirty = 1U;
    g_paddles_dirty = 1U;
}

void onee_input_service_release_keyboard(void)
{
    for (uint8_t i = 0U; i < ONEE_INPUT_DEVICE_SLOT_COUNT; ++i) {
        /* The keyboard fields precede the saved joystick state. */
        memset(&g_slots[i], 0, offsetof(onee_input_slot_t, joystick_seen));
    }
    onee_key_queue_clear();
    g_cold_reboot_pending = 0U;
    onee_repeat_clear_press_orders();
    g_live_dirty = 1U;
}

void onee_input_service_set_blocked(uint8_t blocked)
{
    blocked = (blocked != 0U) ? 1U : 0U;
    if (g_input_blocked == blocked) {
        return;
    }
    g_input_blocked = blocked;
    onee_input_service_release_keyboard();
    g_paddles_dirty = 1U;
    g_vtw_joystick_dirty = 1U;
}

void onee_input_service_release_all(void)
{
    memset(g_slots, 0, sizeof(g_slots));
    onee_input_service_release_keyboard();
    g_paddles_dirty = 1U;
    g_vtw_joystick_dirty = 1U;
}
