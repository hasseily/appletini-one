#include "config_menu_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PHASOR_AUDIO_STEP_COUNT 16

static const uint8_t k_phasor_pan_default[MOCKINGBOARD_CHANNEL_COUNT] = {
    11U, 5U, 11U,
    5U, 11U, 5U,
    11U, 5U, 11U,
    5U, 11U, 5U
};

static const char * const k_phasor_channel_labels[MOCKINGBOARD_CHANNEL_COUNT] = {
    "AY0 A",
    "AY0 B",
    "AY0 C",
    "AY1 A",
    "AY1 B",
    "AY1 C",
    "AY2 A",
    "AY2 B",
    "AY2 C",
    "AY3 A",
    "AY3 B",
    "AY3 C"
};

static const char * const k_phasor_audio_labels[PHASOR_AUDIO_CONTROL_COUNT] = {
    "Bass",
    "Mid",
    "Treble",
    "Volume"
};

static char phasor_ascii_lower(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return (char)(c + ('a' - 'A'));
    }
    return c;
}

static uint8_t phasor_str_ieq(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return 0U;
    }
    while (*a != '\0' && *b != '\0') {
        if (phasor_ascii_lower(*a) != phasor_ascii_lower(*b)) {
            return 0U;
        }
        a++;
        b++;
    }
    return (*a == '\0' && *b == '\0') ? 1U : 0U;
}

static uint8_t phasor_bool_text(const char *value)
{
    return (phasor_str_ieq(value, "on") != 0U) ? 1U : 0U;
}

static const char *phasor_bool_config(uint8_t enabled)
{
    return (enabled != 0U) ? "ON" : "OFF";
}

static uint8_t phasor_psg_mode_text(const char *value)
{
    if (value == NULL) {
        return PHASOR_PSG_MODE_AY8913;
    }
    if (phasor_str_ieq(value, "ym2149") != 0U) {
        return PHASOR_PSG_MODE_YM2149;
    }
    return PHASOR_PSG_MODE_AY8913;
}

static const char *phasor_psg_mode_config(uint8_t mode)
{
    return (mode == PHASOR_PSG_MODE_YM2149) ? "YM2149" : "AY8913";
}

static const char *phasor_psg_mode_label(uint8_t mode)
{
    return (mode == PHASOR_PSG_MODE_YM2149) ? "YM-2149" : "Original (AY-3-8913)";
}

static int8_t phasor_audio_clamp(int32_t value)
{
    if (value < PHASOR_AUDIO_CONTROL_MIN) {
        return (int8_t)PHASOR_AUDIO_CONTROL_MIN;
    }
    if (value > PHASOR_AUDIO_CONTROL_MAX) {
        return (int8_t)PHASOR_AUDIO_CONTROL_MAX;
    }
    return (int8_t)value;
}

static int8_t *phasor_audio_control_ptr(config_menu_t *menu, uint32_t control)
{
    if (menu == NULL) {
        return NULL;
    }
    switch ((phasor_audio_control_t)control) {
    case PHASOR_AUDIO_CONTROL_BASS:
        return &menu->phasor_bass;
    case PHASOR_AUDIO_CONTROL_MID:
        return &menu->phasor_mid;
    case PHASOR_AUDIO_CONTROL_TREBLE:
        return &menu->phasor_treble;
    case PHASOR_AUDIO_CONTROL_VOLUME:
        return &menu->phasor_volume;
    default:
        return NULL;
    }
}

static const int8_t *phasor_audio_control_cptr(const config_menu_t *menu,
                                               uint32_t control)
{
    if (menu == NULL) {
        return NULL;
    }
    switch ((phasor_audio_control_t)control) {
    case PHASOR_AUDIO_CONTROL_BASS:
        return &menu->phasor_bass;
    case PHASOR_AUDIO_CONTROL_MID:
        return &menu->phasor_mid;
    case PHASOR_AUDIO_CONTROL_TREBLE:
        return &menu->phasor_treble;
    case PHASOR_AUDIO_CONTROL_VOLUME:
        return &menu->phasor_volume;
    default:
        return NULL;
    }
}

static uint32_t phasor_pack_pan_word(const config_menu_t *menu,
                                     uint32_t first_channel)
{
    uint32_t packed = 0U;

    if (menu == NULL) {
        return 0U;
    }
    for (uint32_t i = 0U; i < 6U; ++i) {
        const uint32_t channel = first_channel + i;
        if (channel < MOCKINGBOARD_CHANNEL_COUNT) {
            packed |= ((uint32_t)config_menu_pan_clamp(
                menu->mockingboard_pan[channel])) << (i * 4U);
        }
    }
    return packed;
}

static uint8_t phasor_pan_channel_disabled(const config_menu_t *menu,
                                           uint32_t channel)
{
    return (uint8_t)(menu != NULL &&
                     menu->phasor_mockingboard_only != 0U &&
                     channel >= 6U);
}

static void config_menu_phasor_set_pan_defaults(config_menu_t *menu)
{
    for (uint32_t i = 0U; i < MOCKINGBOARD_CHANNEL_COUNT; ++i) {
        menu->mockingboard_pan[i] = k_phasor_pan_default[i];
    }
}

void config_menu_phasor_set_defaults(config_menu_t *menu)
{
    if (menu == NULL) {
        return;
    }
    config_menu_phasor_set_pan_defaults(menu);
    menu->phasor_bass = 0;
    menu->phasor_mid = 0;
    menu->phasor_treble = 0;
    menu->phasor_warmth = PHASOR_WARMTH_DEFAULT;
    menu->phasor_volume = 0;
    menu->phasor_psg_ay_mode = PHASOR_PSG_MODE_AY8913;
    menu->phasor_mockingboard_only = 0U;
}

void config_menu_phasor_apply(config_menu_t *menu)
{
    if (menu == NULL) {
        return;
    }
    if (menu->platform.set_phasor_pan != NULL) {
        menu->platform.set_phasor_pan(
            menu->platform.ctx,
            phasor_pack_pan_word(menu, 0U),
            phasor_pack_pan_word(menu, 6U));
    }
    menu->phasor_warmth = PHASOR_WARMTH_DEFAULT;
    if (menu->platform.set_phasor_audio != NULL) {
        menu->platform.set_phasor_audio(menu->platform.ctx,
                                        menu->phasor_bass,
                                        menu->phasor_mid,
                                        menu->phasor_treble,
                                        menu->phasor_warmth,
                                        menu->phasor_volume,
                                        menu->phasor_psg_ay_mode,
                                        menu->phasor_mockingboard_only);
    }
}

uint32_t config_menu_phasor_item_count(void)
{
    return PHASOR_PSG_MODE_FOCUS + 1U;
}

uint8_t config_menu_phasor_parse_setting(config_menu_t *menu,
                                         const char *key,
                                         const char *value)
{
    uint32_t index;

    if (menu == NULL || key == NULL || value == NULL) {
        return 0U;
    }

    if (strcmp(key, "phasor.slot4.enabled") == 0) {
        menu->mockingboard_slot4_enabled = phasor_bool_text(value);
        return 1U;
    }

    if (strncmp(key, "phasor.pan.", 11U) == 0) {
        char *end = NULL;
        index = (uint32_t)strtoul(key + 11U, &end, 10);
        if (end != NULL && *end == '\0' &&
            index >= 1U && index <= MOCKINGBOARD_CHANNEL_COUNT) {
            menu->mockingboard_pan[index - 1U] =
                config_menu_pan_clamp(strtoul(value, NULL, 10));
        }
        return 1U;
    }

    if (strcmp(key, "phasor.eq.bass") == 0) {
        menu->phasor_bass = phasor_audio_clamp(strtol(value, NULL, 10));
        return 1U;
    }
    if (strcmp(key, "phasor.eq.mid") == 0) {
        menu->phasor_mid = phasor_audio_clamp(strtol(value, NULL, 10));
        return 1U;
    }
    if (strcmp(key, "phasor.eq.treble") == 0) {
        menu->phasor_treble = phasor_audio_clamp(strtol(value, NULL, 10));
        return 1U;
    }
    if (strcmp(key, "phasor.warmth") == 0) {
        menu->phasor_warmth = PHASOR_WARMTH_DEFAULT;
        return 1U;
    }
    if (strcmp(key, "phasor.volume") == 0) {
        menu->phasor_volume = phasor_audio_clamp(strtol(value, NULL, 10));
        return 1U;
    }
    if (strcmp(key, "phasor.psg.mode") == 0) {
        menu->phasor_psg_ay_mode = phasor_psg_mode_text(value);
        return 1U;
    }
    if (strcmp(key, "phasor.mockingboard.only") == 0) {
        menu->phasor_mockingboard_only = phasor_bool_text(value);
        return 1U;
    }
    return 0U;
}

uint8_t config_menu_phasor_append_settings(const config_menu_t *menu,
                                           char *buffer,
                                           size_t buffer_len,
                                           int *len)
{
    uint8_t ok = 1U;

    if (menu == NULL) {
        return 0U;
    }

#define APPEND_PHASOR_CFG(...) do { \
        if (ok != 0U) { \
            ok = config_menu_appendf(buffer, buffer_len, len, __VA_ARGS__); \
        } \
    } while (0)

    APPEND_PHASOR_CFG("phasor.slot4.enabled=%s\n",
                      phasor_bool_config(menu->mockingboard_slot4_enabled));
    for (uint32_t channel = 0U; channel < MOCKINGBOARD_CHANNEL_COUNT; ++channel) {
        APPEND_PHASOR_CFG("phasor.pan.%u=%u\n",
                          (unsigned)(channel + 1U),
                          (unsigned)menu->mockingboard_pan[channel]);
    }
    APPEND_PHASOR_CFG("phasor.eq.bass=%d\n"
                      "phasor.eq.mid=%d\n"
                      "phasor.eq.treble=%d\n"
                      "phasor.warmth=%d\n"
                      "phasor.volume=%d\n"
                      "phasor.psg.mode=%s\n",
                      (int)menu->phasor_bass,
                      (int)menu->phasor_mid,
                      (int)menu->phasor_treble,
                      (int)menu->phasor_warmth,
                      (int)menu->phasor_volume,
                      phasor_psg_mode_config(menu->phasor_psg_ay_mode));
    APPEND_PHASOR_CFG("phasor.mockingboard.only=%s\n",
                      phasor_bool_config(menu->phasor_mockingboard_only));

#undef APPEND_PHASOR_CFG

    return ok;
}

uint8_t config_menu_phasor_adjust(config_menu_t *menu, int8_t delta)
{
    if (menu == NULL) {
        return 0U;
    }

    if (menu->item_focus >= PHASOR_PAN_FOCUS_BASE &&
        menu->item_focus < PHASOR_AUDIO_FOCUS_BASE) {
        const uint32_t channel = menu->item_focus - PHASOR_PAN_FOCUS_BASE;
        int32_t pan = (int32_t)menu->mockingboard_pan[channel] + (int32_t)delta;
        if (phasor_pan_channel_disabled(menu, channel) != 0U) {
            return 1U;
        }
        if (pan < 0) {
            pan = 0;
        } else if (pan > 15) {
            pan = 15;
        }
        if (menu->mockingboard_pan[channel] != (uint8_t)pan) {
            menu->mockingboard_pan[channel] = (uint8_t)pan;
            config_menu_phasor_apply(menu);
            config_menu_save_settings(menu);
        }
        return 1U;
    }

    if (menu->item_focus >= PHASOR_AUDIO_FOCUS_BASE &&
        menu->item_focus < PHASOR_PSG_MODE_FOCUS) {
        int8_t *control = phasor_audio_control_ptr(
            menu,
            menu->item_focus - PHASOR_AUDIO_FOCUS_BASE);
        int8_t next;
        if (control == NULL) {
            return 1U;
        }
        next = phasor_audio_clamp((int32_t)*control + (int32_t)delta);
        if (control != NULL && *control != next) {
            *control = next;
            config_menu_phasor_apply(menu);
            config_menu_save_settings(menu);
        }
        return 1U;
    }

    if (menu->item_focus == PHASOR_PSG_MODE_FOCUS) {
        const uint8_t next =
            (delta < 0) ? PHASOR_PSG_MODE_YM2149 : PHASOR_PSG_MODE_AY8913;
        if (menu->phasor_psg_ay_mode != next) {
            menu->phasor_psg_ay_mode = next;
            config_menu_phasor_apply(menu);
            config_menu_save_settings(menu);
        }
        return 1U;
    }

    return 0U;
}

void config_menu_phasor_activate(config_menu_t *menu)
{
    if (menu == NULL) {
        return;
    }

    if (menu->item_focus == PHASOR_SLOT_FOCUS) {
        menu->mockingboard_slot4_enabled = menu->mockingboard_slot4_enabled ? 0U : 1U;
        if (menu->platform.set_slot_enabled != NULL) {
            menu->platform.set_slot_enabled(menu->platform.ctx,
                                            MOCKINGBOARD_CONTROL_SLOT,
                                            menu->mockingboard_slot4_enabled);
        }
        config_menu_save_settings(menu);
        return;
    }

    if (menu->item_focus >= PHASOR_PAN_FOCUS_BASE &&
        menu->item_focus < PHASOR_AUDIO_FOCUS_BASE) {
        const uint32_t channel = menu->item_focus - PHASOR_PAN_FOCUS_BASE;
        if (phasor_pan_channel_disabled(menu, channel) != 0U) {
            return;
        }
        if (menu->mockingboard_pan[channel] != 8U) {
            menu->mockingboard_pan[channel] = 8U;
            config_menu_phasor_apply(menu);
            config_menu_save_settings(menu);
        }
        return;
    }

    if (menu->item_focus >= PHASOR_AUDIO_FOCUS_BASE &&
        menu->item_focus < PHASOR_PSG_MODE_FOCUS) {
        int8_t *control = phasor_audio_control_ptr(
            menu,
            menu->item_focus - PHASOR_AUDIO_FOCUS_BASE);
        if (control != NULL && *control != 0) {
            *control = 0;
            config_menu_phasor_apply(menu);
            config_menu_save_settings(menu);
        }
        return;
    }

    if (menu->item_focus == PHASOR_PSG_MODE_FOCUS) {
        menu->phasor_psg_ay_mode =
            (menu->phasor_psg_ay_mode == PHASOR_PSG_MODE_YM2149)
                ? PHASOR_PSG_MODE_AY8913
                : PHASOR_PSG_MODE_YM2149;
        config_menu_phasor_apply(menu);
        config_menu_save_settings(menu);
        return;
    }

    if (menu->item_focus == PHASOR_MOCKINGBOARD_ONLY_FOCUS) {
        menu->phasor_mockingboard_only =
            menu->phasor_mockingboard_only ? 0U : 1U;
        config_menu_phasor_apply(menu);
        config_menu_save_settings(menu);
        return;
    }

}

static void hgr_draw_phasor_pan_item(uint16_t *fb,
                                     int x,
                                     int y,
                                     int w,
                                     uint8_t focused,
                                     uint8_t dimmed,
                                     const char *label,
                                     uint8_t pan)
{
    char value[8];

    pan = config_menu_pan_clamp(pan);
    (void)snprintf(value, sizeof(value), "%u", (unsigned)pan);
    cmui_slider(fb,
                x,
                y,
                w,
                focused,
                dimmed,
                label,
                "L",
                "R",
                pan,
                15U,
                8U,
                value);
}

static void phasor_format_signed(char *dst, size_t dst_len, int8_t value)
{
    if (value > 0) {
        (void)snprintf(dst, dst_len, "+%d", (int)value);
    } else {
        (void)snprintf(dst, dst_len, "%d", (int)value);
    }
}

static void hgr_draw_phasor_audio_item(uint16_t *fb,
                                       int x,
                                       int y,
                                       int w,
                                       uint8_t focused,
                                       const char *label,
                                       int8_t value)
{
    char signed_value[8];
    const int clamped = (int)phasor_audio_clamp(value);
    const int marker = clamped - PHASOR_AUDIO_CONTROL_MIN;

    phasor_format_signed(signed_value, sizeof(signed_value), (int8_t)clamped);
    cmui_slider(fb,
                x,
                y,
                w,
                focused,
                0U,
                label,
                "-",
                "+",
                (uint32_t)marker,
                PHASOR_AUDIO_STEP_COUNT,
                (uint32_t)(0 - PHASOR_AUDIO_CONTROL_MIN),
                signed_value);
}

void config_menu_phasor_draw(uint16_t *fb,
                             const config_menu_t *menu,
                             int x,
                             int y,
                             int w)
{
    const int column_gap = 20;
    const int column_w = (w - column_gap) / 2;
    const int row_h = CMUI_ROW_H + CMUI_ROW_GAP;
    const int pan_y = y + (row_h * 2);
    const int audio_y = pan_y + (row_h * 6) + 24;
    const int psg_item_x = x + column_w + column_gap;

    if (menu == NULL) {
        return;
    }

    hgr_draw_check_item(fb, x, y, w,
                        (uint8_t)(menu->item_focus == PHASOR_SLOT_FOCUS),
                        menu->mockingboard_slot4_enabled, "Enable in Slot 4");

    hgr_draw_check_item(fb,
                        x,
                        y + row_h,
                        w,
                        (uint8_t)(menu->item_focus == PHASOR_MOCKINGBOARD_ONLY_FOCUS),
                        menu->phasor_mockingboard_only,
                        "Mockingboard Only");

    for (uint32_t i = 0U; i < MOCKINGBOARD_CHANNEL_COUNT; ++i) {
        const uint32_t column = (i < 6U) ? 0U : 1U;
        const uint32_t row = (i < 6U) ? i : (i - 6U);
        const int item_x = x + (int)column * (column_w + column_gap);
        const int item_y = pan_y + (int)row * row_h;
        const uint8_t dimmed = phasor_pan_channel_disabled(menu, i);
        hgr_draw_phasor_pan_item(fb,
                                 item_x,
                                 item_y,
                                 column_w,
                                 (uint8_t)(menu->item_focus == (i + PHASOR_PAN_FOCUS_BASE)),
                                 dimmed,
                                 k_phasor_channel_labels[i],
                                 menu->mockingboard_pan[i]);
    }

    for (uint32_t control = 0U; control < PHASOR_AUDIO_CONTROL_COUNT; ++control) {
        const int8_t *value = phasor_audio_control_cptr(menu, control);
        hgr_draw_phasor_audio_item(fb,
                                   x,
                                   audio_y + (int)control * row_h,
                                   column_w,
                                   (uint8_t)(menu->item_focus ==
                                             (PHASOR_AUDIO_FOCUS_BASE + control)),
                                   k_phasor_audio_labels[control],
                                   (value != NULL) ? *value : 0);
    }

    hgr_draw_value_item(fb,
                        psg_item_x,
                        audio_y,
                        column_w,
                        (uint8_t)(menu->item_focus == PHASOR_PSG_MODE_FOCUS),
                        "Volume Envelope",
                        phasor_psg_mode_label(menu->phasor_psg_ay_mode));

}
