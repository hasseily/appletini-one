#include "config_menu_internal.h"
#include "display_modes.h"

#include <stdio.h>
#include <string.h>

#include "scanlines.h"
#include "video_blur.h"
#include "video_mono.h"
#include "video_pixel_mask.h"

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
        return "OUTPUT SCREEN";
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
    const char *heading_text = "USB MENU BINDINGS";
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
    const int fixed_menu_y = reset_y + row_h;
    const int binding_y = fixed_menu_y +
        ((onee_fixed != 0U) ? row_h : 0);
    const int right_row_offset = (onee_fixed != 0U) ? 0 : 1;
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
    if (menu->onee_mode_state == CONFIG_MENU_ONEE_MODE_LOCKED) {
        hgr_draw_value_item_dimmed(
            fb,
            x,
            y + (row_h * 2),
            w,
            (uint8_t)(menu->item_focus == CONFIG_MENU_BOOT_ONEE_ITEM),
            "ONE//e standalone",
            config_menu_onee_mode_text(menu));
    } else {
        hgr_draw_value_item(
            fb,
            x,
            y + (row_h * 2),
            w,
            (uint8_t)(menu->item_focus == CONFIG_MENU_BOOT_ONEE_ITEM),
            "ONE//e standalone",
            config_menu_onee_mode_text(menu));
    }
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
                             column_w,
                             0U,
                             "RESET: Ctrl+Alt+Del");
        hgr_draw_item_dimmed(fb,
                             x + column_w + column_gap,
                             reset_y,
                             column_w,
                             0U,
                             "OPEN APPLE: Left Alt");
        hgr_draw_item_dimmed(fb,
                             x + (column_w + column_gap) * 2,
                             reset_y,
                             column_w,
                             0U,
                             "CLOSED APPLE: Right Alt");
        hgr_draw_item_dimmed(fb,
                             x,
                             fixed_menu_y,
                             w,
                             0U,
                             "MENU: Pause/Break + Long OK");
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
        const char *value = (menu->usb_binding_capture == action) ?
            "Press USB" :
            config_menu_usb_binding_source_text(menu->usb_menu_bindings[action]);

        const uint8_t focused =
            (uint8_t)(menu->item_focus ==
                      config_menu_boot_usb_binding_item_for_action(action));

        if (menu->usb_bindings_editable != 0U) {
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
        const char *value = (menu->usb_binding_capture == action) ?
            "Press USB" :
            config_menu_usb_binding_source_text(menu->usb_menu_bindings[action]);
        const uint8_t focused =
            (uint8_t)(menu->item_focus ==
                      config_menu_boot_usb_binding_item_for_action(action));

        if (menu->usb_bindings_editable != 0U) {
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

    /* Outside ONE//e, MENU remains a derived row in the right column. */
    if (onee_fixed == 0U) {
        (void)snprintf(menu_value,
                       sizeof(menu_value),
                       "Long %s",
                       config_menu_usb_binding_source_text(
                           config_menu_usb_open_close_binding_source(menu)));
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
    }

    for (uint32_t i = 0U; i < (sizeof(right_actions) / sizeof(right_actions[0])); ++i) {
        const uint32_t action = right_actions[i];
        const char *value;
        const uint8_t focused =
            (uint8_t)(menu->item_focus ==
                      config_menu_boot_usb_binding_item_for_action(action));
        /* Spacer row between the screenshot pair and the TransWarp
         * bindings so the two groups read as separate blocks. */
        const int spacer = (i >= 2U) ? 1 : 0;
        const int row_y = binding_y +
            ((int)i + right_row_offset + spacer) * row_h;

        value = (menu->usb_binding_capture == action) ?
            "Press USB" :
            config_menu_usb_binding_source_text(menu->usb_menu_bindings[action]);

        if (menu->usb_bindings_editable != 0U) {
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

    /* Keep source settings, display effects, and border controls together.
     * Draw in focus order so the compact menu keeps the same grouping. */
    const int half_w = (w - 12) / 2;
    const int right_x = x + half_w + 12;
    const int right_w = w - half_w - 12;
    int label_w = (half_w * 46) / 100;
    const int long_value_label_w = half_w - 66 -
        cmui_text_width("PAL Accurate Composite", CMUI_BODY_SCALE);

    /* Use one anchor for both columns and full-width rows. Reserve enough
     * value space for every color mode without moving individual fields. */
    if (label_w > long_value_label_w) {
        label_w = long_value_label_w;
    }
    const uint8_t multiplier = config_menu_size_multiplier(menu);
    const uint8_t maximum = display_mode_max_multiplier(config_menu_output_mode(menu));
    const char *multiplier_text = (multiplier == 0U) ? "Max" :
        ((multiplier == 1U) ? "1x" : ((maximum < 2U) ? "2x (1x fit)" : "2x"));

    cmui_value_row_aligned(fb,
                        x,
                        y,
                        half_w,
                        (uint8_t)(menu->item_focus == CONFIG_VIDEO_ITEM_RESOLUTION),
                        0U,
                        "Output resolution",
                        display_mode_get(config_menu_output_mode(menu))->name, label_w);
    cmui_value_row_aligned(fb,
                        right_x,
                        y,
                        right_w,
                        (uint8_t)(menu->item_focus == CONFIG_VIDEO_ITEM_SIZE_MULTIPLIER),
                        0U,
                        "Size multiplier",
                        multiplier_text, label_w);
    cmui_value_row_aligned(fb,
                        x,
                        y + (row_h * 1),
                        half_w,
                        (uint8_t)(menu->item_focus == CONFIG_VIDEO_ITEM_OUTPUT),
                        0U,
                        "Video output",
                        config_menu_video_output_text(menu->video_output_mono), label_w);
    cmui_value_row_aligned(fb,
                        right_x,
                        y + (row_h * 1),
                        right_w,
                        (uint8_t)(menu->item_focus == CONFIG_VIDEO_ITEM_VARIANT),
                        0U,
                        config_menu_video_variant_label(menu),
                        config_menu_video_variant_text(menu), label_w);
    cmui_value_row_aligned(fb,
                        x,
                        y + (row_h * 2),
                        w,
                        (uint8_t)(menu->item_focus == CONFIG_VIDEO_ITEM_ROM),
                        0U,
                        "Video ROM",
                        config_menu_video_rom_text(menu), label_w);
    hgr_draw_check_item(fb,
                        x,
                        y + (row_h * 3),
                        half_w,
                        (uint8_t)(menu->item_focus == CONFIG_VIDEO_ITEM_VIDEO7),
                        menu->video7_auto_mono_enabled,
                        "Video-7 mono");
    hgr_draw_check_item(fb,
                        right_x,
                        y + (row_h * 3),
                        right_w,
                        (uint8_t)(menu->item_focus == CONFIG_VIDEO_ITEM_COL140M),
                        menu->dhgr_col140m_enabled,
                        "Video-7 MIX (COL140M)");
    cmui_value_row_aligned(fb,
                        x,
                        y + (row_h * 4),
                        half_w,
                        (uint8_t)(menu->item_focus == CONFIG_VIDEO_ITEM_SCANLINES),
                        0U,
                        "Scanlines",
                        appletini_scanlines_name(menu->scanlines_mode), label_w);
    cmui_value_row_aligned(fb,
                        right_x,
                        y + (row_h * 4),
                        right_w,
                        (uint8_t)(menu->item_focus == CONFIG_VIDEO_ITEM_PIXEL_MASK),
                        0U,
                        "Pixel mask",
                        appletini_video_pixel_mask_name(menu->video_pixel_mask), label_w);
    if (menu->video_output_mono != 0U) {
        cmui_value_row_aligned(fb, x, y + row_h * 5, half_w,
            (uint8_t)(menu->item_focus == CONFIG_VIDEO_ITEM_DOT_BLEED),
            0U,
            "Dot bleed", appletini_video_dot_bleed_name(menu->video_dot_bleed), label_w);
    }
    hgr_draw_video_blur_item(fb,
        menu->video_output_mono ? right_x : x, y + row_h * 5,
        menu->video_output_mono ? right_w : w,
        (uint8_t)(menu->item_focus == CONFIG_VIDEO_ITEM_BLUR),
        menu->video_blur_strength, label_w);
    hgr_draw_video_glow_item(fb,
                             x,
                             y + (row_h * 6),
                             half_w,
                             (uint8_t)(menu->item_focus == CONFIG_VIDEO_ITEM_GLOW),
                             menu->video_glow_strength, label_w);
    hgr_draw_video_ghosting_item(fb,
                                 right_x,
                                 y + (row_h * 6),
                                 right_w,
                                 (uint8_t)(menu->item_focus == CONFIG_VIDEO_ITEM_GHOSTING),
                                 menu->video_ghosting_strength, label_w);
    hgr_draw_check_item(fb,
                        x,
                        y + (row_h * 7),
                        half_w,
                        (uint8_t)(menu->item_focus == CONFIG_VIDEO_ITEM_BORDER),
                        menu->border_enabled,
                        "IIgs border");
    cmui_value_row_aligned(fb,
                        right_x,
                        y + (row_h * 7),
                        right_w,
                        (uint8_t)(menu->item_focus == CONFIG_VIDEO_ITEM_BORDER_COLOR),
                        0U,
                        "Border color",
                        config_menu_border_color_text(menu->border_color), label_w);
    cmui_value_row_aligned(fb,
                        x,
                        y + (row_h * 8),
                        w,
                        (uint8_t)(menu->item_focus == CONFIG_VIDEO_ITEM_BORDER_FLOOD),
                        0U,
                        "Outside ring",
                        config_menu_border_outside_text(menu->border_flood), label_w);
    draw_exclusive_check(fb,
                         x,
                         y + (row_h * 9),
                         w,
                         (uint8_t)(menu->item_focus == CONFIG_VIDEO_ITEM_SHOW_BEZEL),
                         menu->show_bezel,
                         "Show bezel");
    cmui_value_row_aligned(fb,
                         x,
                         y + (row_h * 10),
                         w,
                         (uint8_t)(menu->item_focus == CONFIG_VIDEO_ITEM_BEZEL),
                         (uint8_t)(menu->border_flood != 0U),
                         "Bezel",
                         config_menu_bezel_text(menu), label_w);
    draw_exclusive_check(fb,
                         x,
                         y + (row_h * 11),
                         half_w,
                         (uint8_t)(menu->item_focus == CONFIG_VIDEO_ITEM_DEBUG),
                         menu->show_debugging,
                         "Show debugging");
    hgr_draw_check_item(fb,
                        right_x,
                        y + (row_h * 11),
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
