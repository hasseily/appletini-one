#include "config_menu_internal.h"

#include <stdio.h>
#include <string.h>

#include "scanlines.h"

static const char *usb_binding_draw_label(uint32_t action)
{
    switch (action) {
    case CONFIG_MENU_USB_BIND_ACTION_UP:
        return "UP";
    case CONFIG_MENU_USB_BIND_ACTION_DOWN:
        return "DOWN";
    case CONFIG_MENU_USB_BIND_ACTION_LEFT:
        return "LEFT";
    case CONFIG_MENU_USB_BIND_ACTION_RIGHT:
        return "RIGHT";
    case CONFIG_MENU_USB_BIND_ACTION_TAB_UP:
        return "TAB UP";
    case CONFIG_MENU_USB_BIND_ACTION_TAB_DOWN:
        return "TAB DOWN";
    case CONFIG_MENU_USB_BIND_ACTION_SCREENSHOT_A2:
        return "PRTSCR A2";
    case CONFIG_MENU_USB_BIND_ACTION_SCREENSHOT_1080P:
        return "PRTSCR 1080P";
    case CONFIG_MENU_USB_BIND_ACTION_OK:
        return "OK";
    case CONFIG_MENU_USB_BIND_ACTION_BACK:
        return "BACK";
    case CONFIG_MENU_USB_BIND_ACTION_VTW_SPEED_TOGGLE:
        return "TW 1MHZ";
    case CONFIG_MENU_USB_BIND_ACTION_VTW_SPEED_UP:
        return "TW SPEED+";
    case CONFIG_MENU_USB_BIND_ACTION_VTW_SPEED_DOWN:
        return "TW SPEED-";
    case CONFIG_MENU_USB_BIND_ACTION_VTW_SLUG_TOGGLE:
        return "TW SLUG .05";
    default:
        return "";
    }
}

static const char *onee_fixed_binding_value(uint32_t action)
{
    switch (action) {
    case CONFIG_MENU_USB_BIND_ACTION_UP:
        return "Up";
    case CONFIG_MENU_USB_BIND_ACTION_DOWN:
        return "Down";
    case CONFIG_MENU_USB_BIND_ACTION_LEFT:
        return "Left";
    case CONFIG_MENU_USB_BIND_ACTION_RIGHT:
        return "Right";
    case CONFIG_MENU_USB_BIND_ACTION_TAB_UP:
        return "PgUp";
    case CONFIG_MENU_USB_BIND_ACTION_TAB_DOWN:
        return "PgDn";
    case CONFIG_MENU_USB_BIND_ACTION_OK:
        return "Enter/KP Enter";
    case CONFIG_MENU_USB_BIND_ACTION_BACK:
        return "Esc";
    case CONFIG_MENU_USB_BIND_ACTION_SCREENSHOT_A2:
        return "PrtSc";
    case CONFIG_MENU_USB_BIND_ACTION_SCREENSHOT_1080P:
        return "Shift+PrtSc";
    case CONFIG_MENU_USB_BIND_ACTION_VTW_SPEED_TOGGLE:
        return "KP 0";
    case CONFIG_MENU_USB_BIND_ACTION_VTW_SPEED_UP:
        return "KP +";
    case CONFIG_MENU_USB_BIND_ACTION_VTW_SPEED_DOWN:
        return "KP -";
    default:
        return "";
    }
}

static void hgr_draw_usb_binding_item(uint16_t *fb,
                                      int x,
                                      int y,
                                      int w,
                                      uint8_t focused,
                                      uint8_t dimmed,
                                      const char *label,
                                      uint32_t label_width,
                                      const char *value)
{
    char line[80];

    (void)snprintf(line, sizeof(line), "%-*s: %s", (int)label_width, label, value);
    if (dimmed != 0U) {
        hgr_draw_item_dimmed(fb, x, y, w, focused, line);
    } else {
        hgr_draw_item(fb, x, y, w, focused, line, HGR_WHITE);
    }
}

void config_menu_draw_boot_settings(uint16_t *fb,
                                    const config_menu_t *menu,
                                    int x,
                                    int y,
                                    int w)
{
    const uint8_t onee_fixed = config_menu_onee_fixed_bindings_active(menu);
    const char *heading_text = (onee_fixed != 0U) ?
        "ONE//e FIXED USB BINDINGS - READ ONLY" : "USB MENU BINDINGS";
    const int row_h = CMUI_ROW_H + CMUI_ROW_GAP;
    const int column_gap = 20;
    const int column_w = (w - (2 * column_gap)) / 3;
    static const uint8_t left_actions[] = {
        CONFIG_MENU_USB_BIND_ACTION_UP,
        CONFIG_MENU_USB_BIND_ACTION_DOWN,
        CONFIG_MENU_USB_BIND_ACTION_LEFT,
        CONFIG_MENU_USB_BIND_ACTION_RIGHT,
    };
    static const uint8_t middle_actions[] = {
        CONFIG_MENU_USB_BIND_ACTION_TAB_UP,
        CONFIG_MENU_USB_BIND_ACTION_TAB_DOWN,
        CONFIG_MENU_USB_BIND_ACTION_OK,
        CONFIG_MENU_USB_BIND_ACTION_BACK,
    };
    static const uint8_t right_actions[] = {
        CONFIG_MENU_USB_BIND_ACTION_SCREENSHOT_A2,
        CONFIG_MENU_USB_BIND_ACTION_SCREENSHOT_1080P,
        CONFIG_MENU_USB_BIND_ACTION_VTW_SPEED_TOGGLE,
        CONFIG_MENU_USB_BIND_ACTION_VTW_SPEED_UP,
        CONFIG_MENU_USB_BIND_ACTION_VTW_SPEED_DOWN,
        CONFIG_MENU_USB_BIND_ACTION_VTW_SLUG_TOGGLE
    };
    const int heading_y = y + (row_h * ((onee_fixed != 0U) ? 4 : 3));
    const int heading_text_w =
        ((int)strlen(heading_text) *
         FB16_BUILTIN_FONT_ADVANCE_X *
         HGR_TEXT_SCALE_X) / HGR_SCALE;
    const int heading_line_x = x + 2 + heading_text_w + 4;
    const int heading_line_w = w - (heading_line_x - x) - 2;
    const int reset_y = heading_y + row_h;
    const int binding_y = reset_y + row_h;
    const uint32_t left_label_w = 5U;
    const uint32_t middle_label_w = 8U;
    const uint32_t right_label_w = 12U;
    char menu_value[32];

    if (menu == NULL) {
        return;
    }

    hgr_draw_value_item(fb,
                        x,
                        y,
                        w,
                        (uint8_t)(menu->item_focus == 0U),
                        "Boot menu",
                        config_menu_boot_timeout_text(menu->boot_timeout_mode));
    hgr_draw_value_item(fb,
                        x,
                        y + row_h,
                        w,
                        (uint8_t)(menu->item_focus == 1U),
                        "Boot device",
                        config_menu_boot_device_text(menu->boot_device));
    hgr_draw_value_item(fb,
                        x,
                        y + (row_h * 2),
                        w,
                        (uint8_t)(menu->item_focus == CONFIG_MENU_BOOT_ONEE_ITEM),
                        "ONE//e standalone",
                        config_menu_onee_mode_text(menu));
    if (onee_fixed != 0U) {
        hgr_draw_value_item(
            fb,
            x,
            y + (row_h * 3),
            w,
            (uint8_t)(menu->item_focus ==
                      CONFIG_MENU_BOOT_ONEE_STANDARD_ITEM),
            "ONE//e video standard",
            config_menu_onee_video_standard_text(menu));
    }
    cmui_text(fb, x + 18, heading_y + 11, heading_text,
              CMUI_COLOR_ACCENT, CMUI_COLOR_BG, CMUI_BODY_SCALE);
    if (heading_line_w > 0) {
        fb16_fill_rect(fb,
                       heading_line_x,
                       heading_y + 24,
                       heading_line_w,
                       2,
                       CMUI_COLOR_BORDER_SOFT);
    }
    if (onee_fixed != 0U) {
        hgr_draw_item_dimmed(fb,
                             x,
                             reset_y,
                             w,
                             0U,
                             "RESET: Ctrl+Alt+Del");
    } else if (menu->usb_bindings_editable != 0U) {
        hgr_draw_item(fb,
                      x,
                      reset_y,
                      w,
                      (uint8_t)(menu->item_focus == CONFIG_MENU_BOOT_USB_BIND_RESET_ITEM),
                      "RESET USB BINDINGS",
                      HGR_WHITE);
    } else {
        hgr_draw_item_dimmed(fb,
                             x,
                             reset_y,
                             w,
                             (uint8_t)(menu->item_focus ==
                                       CONFIG_MENU_BOOT_USB_BIND_RESET_ITEM),
                             "RESET USB BINDINGS");
    }

    for (uint32_t i = 0U; i < (sizeof(left_actions) / sizeof(left_actions[0])); ++i) {
        const uint32_t action = left_actions[i];
        const char *value = (onee_fixed != 0U) ?
            onee_fixed_binding_value(action) :
            ((menu->usb_binding_capture == action) ?
                "Press USB" :
                config_menu_usb_binding_source_text(menu->usb_menu_bindings[action]));

        const uint8_t focused =
            (uint8_t)(menu->item_focus ==
                      config_menu_boot_usb_binding_item_for_action(action));

        if (onee_fixed == 0U && menu->usb_bindings_editable != 0U) {
            hgr_draw_usb_binding_item(
                fb,
                x,
                binding_y + (int)i * row_h,
                column_w,
                focused,
                0U,
                usb_binding_draw_label(action),
                left_label_w,
                value);
        } else {
            hgr_draw_usb_binding_item(
                fb,
                x,
                binding_y + (int)i * row_h,
                column_w,
                focused,
                1U,
                usb_binding_draw_label(action),
                left_label_w,
                value);
        }
    }

    for (uint32_t i = 0U; i < (sizeof(middle_actions) / sizeof(middle_actions[0])); ++i) {
        const uint32_t action = middle_actions[i];
        const char *value = (onee_fixed != 0U) ?
            onee_fixed_binding_value(action) :
            ((menu->usb_binding_capture == action) ?
                "Press USB" :
                config_menu_usb_binding_source_text(menu->usb_menu_bindings[action]));
        const uint8_t focused =
            (uint8_t)(menu->item_focus ==
                      config_menu_boot_usb_binding_item_for_action(action));

        if (onee_fixed == 0U && menu->usb_bindings_editable != 0U) {
            hgr_draw_usb_binding_item(
                fb,
                x + column_w + column_gap,
                binding_y + (int)i * row_h,
                column_w,
                focused,
                0U,
                usb_binding_draw_label(action),
                middle_label_w,
                value);
        } else {
            hgr_draw_usb_binding_item(
                fb,
                x + column_w + column_gap,
                binding_y + (int)i * row_h,
                column_w,
                focused,
                1U,
                usb_binding_draw_label(action),
                middle_label_w,
                value);
        }
    }

    // Draw the derived "MENU" binding at the top of the right column.
    if (onee_fixed != 0U) {
        (void)snprintf(menu_value, sizeof(menu_value), "Break");
    } else {
        (void)snprintf(menu_value,
                       sizeof(menu_value),
                       "Long %s",
                       config_menu_usb_binding_source_text(
                           config_menu_usb_open_close_binding_source(menu)));
    }
    hgr_draw_usb_binding_item(
        fb,
        x + (column_w + column_gap) * 2,
        binding_y,
        column_w,
        0U,
        1U,
        "MENU",
        right_label_w,
        menu_value);

    for (uint32_t i = 0U; i < (sizeof(right_actions) / sizeof(right_actions[0])); ++i) {
        const uint32_t action = right_actions[i];
        const char *value;
        const uint8_t focused =
            (uint8_t)(menu->item_focus ==
                      config_menu_boot_usb_binding_item_for_action(action));
        /* Spacer row between the screenshot pair and the TransWarp
         * bindings so the two groups read as separate blocks. */
        const int spacer = (i >= 2U) ? 1 : 0;
        const int row_y = binding_y + ((int)i + 1 + spacer) * row_h;

        if (onee_fixed != 0U &&
            action == CONFIG_MENU_USB_BIND_ACTION_VTW_SLUG_TOGGLE) {
            continue;
        }
        value = (onee_fixed != 0U) ?
            onee_fixed_binding_value(action) :
            ((menu->usb_binding_capture == action) ?
                "Press USB" :
                config_menu_usb_binding_source_text(menu->usb_menu_bindings[action]));

        if (onee_fixed == 0U && menu->usb_bindings_editable != 0U) {
            hgr_draw_usb_binding_item(
                fb,
                x + (column_w + column_gap) * 2,
                row_y,
                column_w,
                focused,
                0U,
                usb_binding_draw_label(action),
                right_label_w,
                value);
        } else {
            hgr_draw_usb_binding_item(
                fb,
                x + (column_w + column_gap) * 2,
                row_y,
                column_w,
                focused,
                1U,
                usb_binding_draw_label(action),
                right_label_w,
                value);
        }
    }
}

void config_menu_draw_video(uint16_t *fb,
                            const config_menu_t *menu,
                            int x,
                            int y,
                            int w)
{
    const int row_h = CMUI_ROW_H + CMUI_ROW_GAP;

    if (menu == NULL) {
        return;
    }

    void (*draw_exclusive_check)(uint16_t *, int, int, int,
                                 uint8_t, uint8_t, const char *) =
        (menu->border_flood != 0u) ?
            hgr_draw_check_item_dimmed : hgr_draw_check_item;
    void (*draw_exclusive_value)(uint16_t *, int, int, int,
                                 uint8_t, const char *, const char *) =
        (menu->border_flood != 0u) ?
            hgr_draw_value_item_dimmed : hgr_draw_value_item;

    /* Border and the two Video-7 options share one row. Debug and video mode
     * share another. Value controls keep a full line so their labels stay clear. */
    const int half_w = (w - 12) / 2;
    const int right_x = x + half_w + 12;
    const int right_w = w - half_w - 12;
    const int third_w = (w - 16) / 3;
    const int middle_x = x + third_w + 8;
    const int last_x = middle_x + third_w + 8;
    const int last_w = w - (third_w * 2) - 16;

    hgr_draw_value_item(fb,
                        x,
                        y,
                        w,
                        (uint8_t)(menu->item_focus == CONFIG_VIDEO_ITEM_OUTPUT),
                        "Video output",
                        config_menu_video_output_text(menu->video_output_mono));
    hgr_draw_value_item(fb,
                        x,
                        y + row_h,
                        w,
                        (uint8_t)(menu->item_focus == CONFIG_VIDEO_ITEM_VARIANT),
                        config_menu_video_variant_label(menu),
                        config_menu_video_variant_text(menu));
    hgr_draw_value_item(fb,
                        x,
                        y + (row_h * 2),
                        w,
                        (uint8_t)(menu->item_focus == CONFIG_VIDEO_ITEM_SCANLINES),
                        "Scanlines",
                        appletini_scanlines_name(menu->scanlines_mode));
    hgr_draw_video_blur_item(fb,
                             x,
                             y + (row_h * 3),
                             w,
                             (uint8_t)(menu->item_focus == CONFIG_VIDEO_ITEM_BLUR),
                             menu->video_blur_strength);
    hgr_draw_video_glow_item(fb,
                             x,
                             y + (row_h * 4),
                             w,
                             (uint8_t)(menu->item_focus == CONFIG_VIDEO_ITEM_GLOW),
                             menu->video_glow_strength);
    hgr_draw_video_ghosting_item(fb,
                                 x,
                                 y + (row_h * 5),
                                 w,
                                 (uint8_t)(menu->item_focus == CONFIG_VIDEO_ITEM_GHOSTING),
                                 menu->video_ghosting_strength);
    hgr_draw_check_item(fb,
                        x,
                        y + (row_h * 6),
                        third_w,
                        (uint8_t)(menu->item_focus == CONFIG_VIDEO_ITEM_BORDER),
                        menu->border_enabled,
                        "IIgs border (VidHD $C034)");
    hgr_draw_check_item(fb,
                        middle_x,
                        y + (row_h * 6),
                        third_w,
                        (uint8_t)(menu->item_focus == CONFIG_VIDEO_ITEM_VIDEO7),
                        menu->video7_auto_mono_enabled,
                        "Video-7 mono");
    hgr_draw_check_item(fb,
                        last_x,
                        y + (row_h * 6),
                        last_w,
                        (uint8_t)(menu->item_focus == CONFIG_VIDEO_ITEM_COL140M),
                        menu->dhgr_col140m_enabled,
                        "Video-7 MIX (COL140M)");
    hgr_draw_value_item(fb,
                        x,
                        y + (row_h * 7),
                        w,
                        (uint8_t)(menu->item_focus == CONFIG_VIDEO_ITEM_BORDER_COLOR),
                        "Border color",
                        config_menu_border_color_text(menu->border_color));
    hgr_draw_value_item(fb,
                        x,
                        y + (row_h * 8),
                        w,
                        (uint8_t)(menu->item_focus == CONFIG_VIDEO_ITEM_BORDER_FLOOD),
                        "Outside ring",
                        config_menu_border_outside_text(menu->border_flood));
    hgr_draw_value_item(fb,
                        x,
                        y + (row_h * 9),
                        w,
                        (uint8_t)(menu->item_focus == CONFIG_VIDEO_ITEM_ROM),
                        "Video ROM",
                        config_menu_video_rom_text(menu));
    draw_exclusive_check(fb,
                         x,
                         y + (row_h * 10),
                         w,
                         (uint8_t)(menu->item_focus == CONFIG_VIDEO_ITEM_SHOW_BEZEL),
                         menu->show_bezel,
                         "Show bezel");
    draw_exclusive_value(fb,
                         x,
                         y + (row_h * 11),
                         w,
                         (uint8_t)(menu->item_focus == CONFIG_VIDEO_ITEM_BEZEL),
                         "Bezel",
                         config_menu_bezel_text(menu));
    draw_exclusive_check(fb,
                         x,
                         y + (row_h * 12),
                         half_w,
                         (uint8_t)(menu->item_focus == CONFIG_VIDEO_ITEM_DEBUG),
                         menu->show_debugging,
                         "Show debugging");
    hgr_draw_check_item(fb,
                        right_x,
                        y + (row_h * 12),
                        right_w,
                        (uint8_t)(menu->item_focus == CONFIG_VIDEO_ITEM_BADGE),
                        menu->format_badge_enabled,
                        "Show video mode");
}

void config_menu_draw_clock(uint16_t *fb,
                            const config_menu_t *menu,
                            int x,
                            int y,
                            int w)
{
    char line[160];
    const int row_h = CMUI_ROW_H + CMUI_ROW_GAP;

    if (menu == NULL) {
        return;
    }

    hgr_draw_check_item(fb, x, y, w, (uint8_t)(menu->item_focus == 0U),
                        menu->clock_enabled, "Enable");

    hgr_draw_item(fb, x, y + row_h, w, (uint8_t)(menu->item_focus == 1U), "Read RTC", HGR_WHITE);

    (void)snprintf(line, sizeof(line), "%04u", (unsigned)menu->clock_time.year);
    hgr_draw_value_item(fb, x, y + (row_h * 2), w, (uint8_t)(menu->item_focus == 2U), "Year", line);
    (void)snprintf(line, sizeof(line), "%02u", (unsigned)menu->clock_time.month);
    hgr_draw_value_item(fb, x, y + (row_h * 3), w, (uint8_t)(menu->item_focus == 3U), "Month", line);
    (void)snprintf(line, sizeof(line), "%02u", (unsigned)menu->clock_time.day);
    hgr_draw_value_item(fb, x, y + (row_h * 4), w, (uint8_t)(menu->item_focus == 4U), "Day", line);
    (void)snprintf(line, sizeof(line), "%02u", (unsigned)menu->clock_time.hour);
    hgr_draw_value_item(fb, x, y + (row_h * 5), w, (uint8_t)(menu->item_focus == 5U), "Hour", line);
    (void)snprintf(line, sizeof(line), "%02u", (unsigned)menu->clock_time.min);
    hgr_draw_value_item(fb, x, y + (row_h * 6), w, (uint8_t)(menu->item_focus == 6U), "Minute", line);
    (void)snprintf(line, sizeof(line), "%02u", (unsigned)menu->clock_time.sec);
    hgr_draw_value_item(fb, x, y + (row_h * 7), w, (uint8_t)(menu->item_focus == 7U), "Second", line);
    hgr_draw_item(fb, x, y + (row_h * 8), w, (uint8_t)(menu->item_focus == 8U), "Write RTC", HGR_WHITE);

}
