#include "debug_overlay.h"

#include <stdio.h>
#include <string.h>

#include "../lib/fb16.h"
#include "apple_fb_handoff.h"
#include "card_control_regs.h"
#include "scanlines.h"
#include "video_ghosting.h"
#include "video_output.h"

#define HUD_TEXT_SCALE_X 1
#define HUD_TEXT_SCALE_Y 2
#define HUD_TEXT_W      (FB16_BUILTIN_FONT_ADVANCE_X * HUD_TEXT_SCALE_X)
#define HUD_LINE_H      18
#define HUD_PAD         8

#define HUD_TOP_X       8
#define HUD_TOP_W       (FB16_WIDTH - 16)
#define HUD_TOP_H       76
#define HUD_TOP_Y       (FB16_HEIGHT - HUD_TOP_H - 8)

#define HUD_LEFT_X      8
#define HUD_LEFT_Y      148
#define HUD_LEFT_W      304
#define HUD_LEFT_H      438

#define HUD_RIGHT_X     1608
#define HUD_RIGHT_Y     148
#define HUD_RIGHT_W     304
#define HUD_RIGHT_H     408

#define HUD_SOFT_X      HUD_RIGHT_X
#define HUD_SOFT_Y      (HUD_RIGHT_Y + HUD_RIGHT_H + 12)
#define HUD_SOFT_W      HUD_RIGHT_W
#define HUD_SOFT_H      216

#define HUD_BG          FB16_RGB(0x08, 0x0B, 0x10)
#define HUD_PANEL_BG    FB16_RGB(0x0E, 0x13, 0x1B)
#define HUD_BORDER      FB16_RGB(0x34, 0x48, 0x5C)
#define HUD_MUTED       FB16_RGB(0xA0, 0xAD, 0xB8)
#define HUD_ACCENT      FB16_RGB(0x62, 0xD8, 0xE8)
#define HUD_WARN        FB16_RGB(0xFF, 0xB0, 0x45)
#define HUD_GOOD        FB16_RGB(0x78, 0xE0, 0x9A)

static const debug_overlay_rect_t k_regions[] = {
    { HUD_TOP_X, HUD_TOP_Y, HUD_TOP_W, HUD_TOP_H },
    { HUD_LEFT_X, HUD_LEFT_Y, HUD_LEFT_W, HUD_LEFT_H },
    { HUD_RIGHT_X, HUD_RIGHT_Y, HUD_RIGHT_W, HUD_RIGHT_H },
    { HUD_SOFT_X, HUD_SOFT_Y, HUD_SOFT_W, HUD_SOFT_H }
};

uint32_t debug_overlay_region_count(void)
{
    return (uint32_t)(sizeof(k_regions) / sizeof(k_regions[0]));
}

debug_overlay_rect_t debug_overlay_region(uint32_t index)
{
    debug_overlay_rect_t empty = {0, 0, 0, 0};

    if (index >= debug_overlay_region_count()) {
        return empty;
    }
    return k_regions[index];
}

static void text_clipped(uint16_t *fb,
                         int x,
                         int y,
                         int w,
                         const char *text,
                         uint32_t fg,
                         uint32_t bg)
{
    char clipped[160];
    uint32_t max_chars;
    uint32_t i = 0U;

    if (fb == NULL || text == NULL || w <= 0) {
        return;
    }

    max_chars = (uint32_t)w / HUD_TEXT_W;
    if (max_chars > (uint32_t)(sizeof(clipped) - 1U)) {
        max_chars = (uint32_t)(sizeof(clipped) - 1U);
    }
    if (max_chars == 0U) {
        return;
    }

    while (text[i] != '\0' && i < max_chars &&
           i < (uint32_t)(sizeof(clipped) - 1U)) {
        clipped[i] = text[i];
        ++i;
    }
    if (text[i] != '\0' && max_chars >= 3U) {
        clipped[max_chars - 3U] = '.';
        clipped[max_chars - 2U] = '.';
        clipped[max_chars - 1U] = '.';
        clipped[max_chars] = '\0';
    } else {
        clipped[i] = '\0';
    }

    fb16_string_scaled_xy(fb,
                          x,
                          y,
                          clipped,
                          fg,
                          bg,
                          HUD_TEXT_SCALE_X,
                          HUD_TEXT_SCALE_Y);
}

static void panel(uint16_t *fb, int x, int y, int w, int h, const char *title)
{
    fb16_fill_rect(fb, x, y, w, h, HUD_PANEL_BG);
    fb16_rect(fb, x, y, w, h, HUD_BORDER);
    fb16_fill_rect(fb, x, y, w, 3, HUD_ACCENT);
    text_clipped(fb,
                 x + HUD_PAD,
                 y + HUD_PAD,
                 w - (2 * HUD_PAD),
                 title,
                 HUD_ACCENT,
                 HUD_PANEL_BG);
}

static void line(uint16_t *fb,
                 int x,
                 int y,
                 int w,
                 uint32_t row,
                 const char *text,
                 uint32_t color)
{
    text_clipped(fb,
                 x + HUD_PAD,
                 y + HUD_PAD + 22 + ((int)row * HUD_LINE_H),
                 w - (2 * HUD_PAD),
                 text,
                 color,
                 HUD_PANEL_BG);
}

static void switch_pair(uint16_t *fb,
                        int x,
                        int y,
                        int w,
                        uint32_t row,
                        const char *left,
                        uint32_t left_mask,
                        const char *right,
                        uint32_t right_mask,
                        uint32_t state)
{
    const int col_w = (w - (2 * HUD_PAD) - 8) / 2;
    const int row_y = y + HUD_PAD + 22 + ((int)row * HUD_LINE_H);
    char text[48];

    (void)snprintf(text,
                   sizeof(text),
                   "%-8s %u",
                   left,
                   (unsigned)((state & left_mask) != 0U));
    text_clipped(fb,
                 x + HUD_PAD,
                 row_y,
                 col_w,
                 text,
                 ((state & left_mask) != 0U) ? HUD_GOOD : HUD_MUTED,
                 HUD_PANEL_BG);

    (void)snprintf(text,
                   sizeof(text),
                   "%-8s %u",
                   right,
                   (unsigned)((state & right_mask) != 0U));
    text_clipped(fb,
                 x + HUD_PAD + col_w + 8,
                 row_y,
                 col_w,
                 text,
                 ((state & right_mask) != 0U) ? HUD_GOOD : HUD_MUTED,
                 HUD_PANEL_BG);
}

static void draw_vidhd_shr_state(uint16_t *fb,
                                 int x,
                                 int y,
                                 int w,
                                 uint32_t row,
                                 const debug_overlay_snapshot_t *s)
{
    char text[48];
    const uint32_t c029_addr = 0xC029U;
    const uint8_t shr_active =
        (s != NULL && s->apple_mode == APPLE_FB_DISPLAY_MODE_SHR) ? 1U : 0U;

    (void)snprintf(text,
                   sizeof(text),
                   "%04lX SHR %u",
                   (unsigned long)c029_addr,
                   (unsigned)shr_active);
    line(fb,
         x,
         y,
         w,
         row,
         text,
         (shr_active != 0U) ? HUD_GOOD : HUD_MUTED);
}

static const char *boot_device_name(uint8_t device)
{
    return (device == 1U) ? "Disk II" : "SmartPort";
}

static const char *mono_color_name(uint8_t color)
{
    switch (apple_video_mono_color_clamp(color)) {
    case APPLE_VIDEO_MONO_GREEN:
        return "Green";
    case APPLE_VIDEO_MONO_AMBER:
        return "Amber";
    case APPLE_VIDEO_MONO_BLACK:
        return "Black";
    case APPLE_VIDEO_MONO_WHITE:
    default:
        return "White";
    }
}

static const char *color_mode_name(uint8_t mode)
{
    switch (apple_video_color_mode_clamp(mode)) {
    case APPLE_VIDEO_COLOR_IDEALIZED:
        return "Idealized";
    case APPLE_VIDEO_COLOR_RGB:
        return "RGB";
    case APPLE_VIDEO_COLOR_TV:
        return "TV";
    case APPLE_VIDEO_COLOR_PAL_ACCURATE_COMPOSITE:
        return "PAL comp";
    case APPLE_VIDEO_COLOR_PAL_ACCURATE_TV:
        return "PAL TV";
    case APPLE_VIDEO_COLOR_COMPOSITE_MONITOR:
    default:
        return "Composite";
    }
}

static void format_temp(char *dst, size_t dst_len, const tmp102_temp_t *temp)
{
    int32_t centi;
    char sign = '+';

    if (dst == NULL || dst_len == 0U) {
        return;
    }
    if (temp == NULL || temp->valid == 0U) {
        (void)snprintf(dst, dst_len, "Temp N/A");
        return;
    }

    centi = temp->centi_c;
    if (centi < 0) {
        sign = '-';
        centi = -centi;
    }
    (void)snprintf(dst,
                   dst_len,
                   "Temp %c%ld.%02ld C",
                   sign,
                   (long)(centi / 100),
                   (long)(centi % 100));
}

static void format_rtc(char *dst, size_t dst_len, const rtc_pcf8563_time_t *rtc)
{
    if (dst == NULL || dst_len == 0U) {
        return;
    }
    if (rtc == NULL || rtc->valid == 0U) {
        (void)snprintf(dst, dst_len, "RTC invalid");
        return;
    }

    (void)snprintf(dst,
                   dst_len,
                   "RTC %04u-%02u-%02u %02u:%02u",
                   (unsigned)rtc->year,
                   (unsigned)rtc->month,
                   (unsigned)rtc->day,
                   (unsigned)rtc->hour,
                   (unsigned)rtc->min);
}

static void append_warning(char *dst,
                           size_t dst_len,
                           const char *text)
{
    size_t len;

    if (dst == NULL || dst_len == 0U || text == NULL || text[0] == '\0') {
        return;
    }
    len = strlen(dst);
    if (len >= dst_len - 1U) {
        return;
    }
    if (len != 0U) {
        (void)snprintf(dst + len, dst_len - len, " | %s", text);
    } else {
        (void)snprintf(dst, dst_len, "%s", text);
    }
}

static void build_warnings(const debug_overlay_snapshot_t *s,
                           char *dst,
                           size_t dst_len)
{
    const uint32_t fb_axi_err = (s->fb_debug >> 3) & 1U;
    const uint32_t fb_axi_err_count = s->fb_debug2 & 0xFFFFU;

    if (dst == NULL || dst_len == 0U) {
        return;
    }
    dst[0] = '\0';

    if (s->metadata_valid == 0U) {
        append_warning(dst, dst_len, "boot metadata invalid");
    }
    if (fb_axi_err != 0U || fb_axi_err_count != 0U) {
        append_warning(dst, dst_len, "framebuffer AXI errors");
    }
    if (s->usb_storage_attention != 0U) {
        append_warning(dst, dst_len, "USB0 storage attention");
    }
    if (s->hid_status.submit_error_count != 0U ||
        s->hid_status.transfer_error_count != 0U ||
        s->hid_status.last_error != 0) {
        append_warning(dst, dst_len, "USB1 HID errors");
    }
}

static void draw_header(uint16_t *fb, const debug_overlay_snapshot_t *s)
{
    char line_buf[192];
    char warnings[192];
    uint32_t status_color = HUD_GOOD;

    build_warnings(s, warnings, sizeof(warnings));
    if (warnings[0] != '\0') {
        status_color = HUD_WARN;
    }

    fb16_fill_rect(fb, HUD_TOP_X, HUD_TOP_Y, HUD_TOP_W, HUD_TOP_H, HUD_BG);
    fb16_rect(fb, HUD_TOP_X, HUD_TOP_Y, HUD_TOP_W, HUD_TOP_H, HUD_BORDER);
    fb16_fill_rect(fb, HUD_TOP_X, HUD_TOP_Y, HUD_TOP_W, 3, status_color);

    (void)snprintf(line_buf,
                   sizeof(line_buf),
                   "DEBUG  FW %s  FPS 1080p %lu.%02lu  Apple area %lu.%02lu  DRAW %lu us  OWNER %s",
                   s->firmware_version,
                   (unsigned long)(s->fps_x100 / 100U),
                   (unsigned long)(s->fps_x100 % 100U),
                   (unsigned long)(s->apple_fps_x100 / 100U),
                   (unsigned long)(s->apple_fps_x100 % 100U),
                   (unsigned long)s->draw_us,
                   (s->usb_menu_owned != 0U) ? "USB device" : "Apple keyboard");
    text_clipped(fb,
                 HUD_TOP_X + HUD_PAD,
                 HUD_TOP_Y + 12,
                 HUD_TOP_W - (2 * HUD_PAD),
                 line_buf,
                 FB16_COLOR_WHITE,
                 HUD_BG);

    if (warnings[0] != '\0') {
        (void)snprintf(line_buf, sizeof(line_buf), "WARN  %s", warnings);
        text_clipped(fb,
                     HUD_TOP_X + HUD_PAD,
                     HUD_TOP_Y + 40,
                     HUD_TOP_W - (2 * HUD_PAD),
                     line_buf,
                     HUD_WARN,
                     HUD_BG);
    }
}

static void draw_system(uint16_t *fb, const debug_overlay_snapshot_t *s)
{
    char text[192];
    int x = HUD_LEFT_X;
    int y = HUD_LEFT_Y;
    int w = HUD_LEFT_W;

    panel(fb, x, y, w, 142, "System");
    (void)snprintf(text, sizeof(text), "Boot %s", boot_device_name(s->boot_device));
    line(fb, x, y, w, 0U, text, FB16_COLOR_WHITE);
    (void)snprintf(text,
                   sizeof(text),
                   "Config %s%s",
                   (s->settings_loaded != 0U) ? "loaded" : "defaults",
                   (s->session_only != 0U) ? " session" : "");
    line(fb, x, y, w, 1U, text, HUD_MUTED);
    (void)snprintf(text,
                   sizeof(text),
                   "Golden %s",
                   (s->metadata_valid != 0U) ? s->golden_version : "unavailable");
    line(fb, x, y, w, 2U, text, (s->metadata_valid != 0U) ? HUD_MUTED : HUD_WARN);
    format_temp(text, sizeof(text), &s->temp);
    line(fb, x, y, w, 3U, text, FB16_COLOR_WHITE);
    format_rtc(text, sizeof(text), &s->rtc);
    line(fb, x, y, w, 4U, text, FB16_COLOR_WHITE);
    (void)snprintf(text,
                   sizeof(text),
                   "Bezel %s %s",
                   (s->show_bezel != 0U) ? "on" : "off",
                   s->bezel_status);
    line(fb, x, y, w, 5U, text, HUD_MUTED);
}

static void draw_input(uint16_t *fb, const debug_overlay_snapshot_t *s)
{
    char text[96];
    int x = HUD_LEFT_X;
    int y = HUD_LEFT_Y + 154;
    int w = HUD_LEFT_W;

    panel(fb, x, y, w, 112, "Input");
    (void)snprintf(text,
                   sizeof(text),
                   "Owner %s",
                   (s->usb_menu_owned != 0U) ? "USB device" : "Apple keyboard");
    line(fb, x, y, w, 0U, text, FB16_COLOR_WHITE);
    (void)snprintf(text,
                   sizeof(text),
                   "Menu %s  HID %s",
                   (s->config_menu_active != 0U) ? "open" : "closed",
                   (s->hid_status.ready != 0U) ? "ready" :
                   ((s->hid_status.started != 0U) ? "starting" : "off"));
    line(fb, x, y, w, 1U, text, HUD_MUTED);
    (void)snprintf(text,
                   sizeof(text),
                   "USB dev %u  Kbd %u  Mouse %u",
                   (unsigned)s->hid_status.active_count,
                   (unsigned)s->hid_status.keyboard_count,
                   (unsigned)s->hid_status.mouse_count);
    line(fb, x, y, w, 2U, text, HUD_MUTED);
    (void)snprintf(text,
                   sizeof(text),
                   "Capture %s  Reports %lu",
                   (s->hid_status.menu_capture != 0U) ? "menu" : "off",
                   (unsigned long)s->hid_status.report_count);
    line(fb, x, y, w, 3U, text, HUD_MUTED);
}

static void draw_storage(uint16_t *fb, const debug_overlay_snapshot_t *s)
{
    char text[128];
    uint8_t drive;
    uint8_t unit;
    uint8_t present;
    int x = HUD_LEFT_X;
    int y = HUD_LEFT_Y + 278;
    int w = HUD_LEFT_W;

    panel(fb, x, y, w, 160, "Storage");
    if (s->disk2_valid != 0U) {
        drive = (s->disk2_activity.drive < DISK2_DRIVE_COUNT) ?
            s->disk2_activity.drive : 0U;
        present = ((s->disk2_activity.present_mask & (uint8_t)(1U << drive)) != 0U) ?
            1U : 0U;
        (void)snprintf(text,
                       sizeof(text),
                       "Disk II D%u %s %s",
                       (unsigned)drive + 1U,
                       present ? "media" : "empty",
                       (s->disk2_activity.motor_on != 0U ||
                        s->disk2_activity.spinning != 0U) ? "motor" : "idle");
        line(fb, x, y, w, 0U, text, present ? FB16_COLOR_WHITE : HUD_MUTED);
        (void)snprintf(text,
                       sizeof(text),
                       "DII R %lu  W %lu%s",
                       (unsigned long)s->disk2_activity.read_count,
                       (unsigned long)s->disk2_activity.write_count,
                       s->disk2_read_only[drive] ? " locked" : "");
        line(fb, x, y, w, 1U, text, HUD_MUTED);
    } else {
        line(fb, x, y, w, 0U, "Disk II unavailable", HUD_MUTED);
    }

    if (s->smartport_valid != 0U) {
        unit = (s->smartport_activity.device < SMARTPORT_SERVICE_DEVICE_COUNT) ?
            s->smartport_activity.device : 0U;
        present = ((s->smartport_activity.present_mask & (uint8_t)(1U << unit)) != 0U) ?
            1U : 0U;
        (void)snprintf(text,
                       sizeof(text),
                       "SmartPort SP%u %s%s",
                       (unsigned)unit + 1U,
                       present ? "media" : "empty",
                       s->smartport_activity.read_only ? " locked" : "");
        line(fb, x, y, w, 2U, text, present ? FB16_COLOR_WHITE : HUD_MUTED);
        (void)snprintf(text,
                       sizeof(text),
                       "SP S %lu R %lu W %lu",
                       (unsigned long)s->smartport_activity.status_count,
                       (unsigned long)s->smartport_activity.read_count,
                       (unsigned long)s->smartport_activity.write_count);
        line(fb, x, y, w, 3U, text, HUD_MUTED);
    } else {
        line(fb, x, y, w, 2U, "SmartPort unavailable", HUD_MUTED);
    }

    (void)snprintf(text,
                   sizeof(text),
                   "USB0 cfg %lu  %s",
                   (unsigned long)s->usb_storage_stats.current_config,
                   (s->usb_storage_attention != 0U) ? "attention" : "ok");
    line(fb,
         x,
         y,
         w,
         4U,
         text,
         (s->usb_storage_attention != 0U) ? HUD_WARN : HUD_MUTED);
}

static void draw_video(uint16_t *fb, const debug_overlay_snapshot_t *s)
{
    char text[96];
    int x = HUD_RIGHT_X;
    int y = HUD_RIGHT_Y;
    int w = HUD_RIGHT_W;

    panel(fb, x, y, w, 150, "Video");
    (void)snprintf(text,
                   sizeof(text),
                   "Apple %s %s",
                   (s->apple_mode == APPLE_FB_DISPLAY_MODE_SHR) ? "SHR" : "legacy",
                   (s->apple_video_50hz != 0U) ? "50Hz" : "60Hz");
    line(fb, x, y, w, 0U, text, FB16_COLOR_WHITE);
    if (s->video_output_mono != 0U) {
        (void)snprintf(text,
                       sizeof(text),
                       "Output mono %s",
                       mono_color_name(s->video_mono_color));
    } else {
        (void)snprintf(text,
                       sizeof(text),
                       "Output %s",
                       color_mode_name(s->video_color_mode));
    }
    line(fb, x, y, w, 1U, text, HUD_MUTED);
    (void)snprintf(text,
                   sizeof(text),
                   "Scanlines %s",
                   appletini_scanlines_name(s->scanlines_mode));
    line(fb, x, y, w, 2U, text, HUD_MUTED);
    (void)snprintf(text,
                   sizeof(text),
                   "Video-7 mono %s",
                   (s->video7_auto_mono != 0U) ? "auto" : "off");
    line(fb, x, y, w, 3U, text, HUD_MUTED);
    (void)snprintf(text,
                   sizeof(text),
                   "Ghosting %s",
                   appletini_video_ghosting_name(s->video_ghosting_strength));
    line(fb, x, y, w, 4U, text, HUD_MUTED);
    (void)snprintf(text,
                   sizeof(text),
                   "RAMWorks bank %lu",
                   (unsigned long)(((s->softswitch_state >>
                                     CARD_CTRL_SOFTSW_RAMWORKS_BANK_SHIFT) &
                                    CARD_CTRL_SOFTSW_RAMWORKS_BANK_MASK) + 1U));
    line(fb, x, y, w, 5U, text, HUD_MUTED);
}

static void draw_softswitches(uint16_t *fb, const debug_overlay_snapshot_t *s)
{
    char text[96];
    const uint32_t state = s->softswitch_state;
    int x = HUD_SOFT_X;
    int y = HUD_SOFT_Y;
    int w = HUD_SOFT_W;

    panel(fb, x, y, w, HUD_SOFT_H, "Soft Switches");
    (void)snprintf(text,
                   sizeof(text),
                   "Raw 0x%06lX  RW %lu",
                   (unsigned long)(state & CARD_CTRL_SOFTSW_STATE_MASK),
                   (unsigned long)(((state >>
                                     CARD_CTRL_SOFTSW_RAMWORKS_BANK_SHIFT) &
                                    CARD_CTRL_SOFTSW_RAMWORKS_BANK_MASK) + 1U));
    line(fb, x, y, w, 0U, text, FB16_COLOR_WHITE);

    switch_pair(fb, x, y, w, 1U, "80STORE", CARD_CTRL_SOFTSW_80STORE_BIT,
                "RAMRD", CARD_CTRL_SOFTSW_RAMRD_BIT, state);
    switch_pair(fb, x, y, w, 2U, "RAMWRT", CARD_CTRL_SOFTSW_RAMWRT_BIT,
                "ALTZP", CARD_CTRL_SOFTSW_ALTZP_BIT, state);
    switch_pair(fb, x, y, w, 3U, "TEXT", CARD_CTRL_SOFTSW_TEXT_BIT,
                "MIXED", CARD_CTRL_SOFTSW_MIXED_BIT, state);
    switch_pair(fb, x, y, w, 4U, "PAGE2", CARD_CTRL_SOFTSW_PAGE2_BIT,
                "HIRES", CARD_CTRL_SOFTSW_HIRES_BIT, state);
    switch_pair(fb, x, y, w, 5U, "ALTCHAR", CARD_CTRL_SOFTSW_ALTCHARSET_BIT,
                "80COL", CARD_CTRL_SOFTSW_80COL_BIT, state);
    switch_pair(fb, x, y, w, 6U, "DHIRES", CARD_CTRL_SOFTSW_DHIRES_BIT,
                "LCBNK2", CARD_CTRL_SOFTSW_LCRAM_BANK2_BIT, state);
    switch_pair(fb, x, y, w, 7U, "LCRD", CARD_CTRL_SOFTSW_LCRAM_READ_BIT,
                "LCWRT", CARD_CTRL_SOFTSW_LCRAM_WRITE_BIT, state);
    draw_vidhd_shr_state(fb, x, y, w, 8U, s);
}

static void draw_performance(uint16_t *fb, const debug_overlay_snapshot_t *s)
{
    char text[96];
    const uint32_t fb_reader_state = s->fb_debug & 0x7U;
    const uint32_t fb_axi_err = (s->fb_debug >> 3) & 1U;
    const uint32_t fb_bursts = (s->fb_debug >> 8) & 0x3FFFFU;
    const uint32_t fb_axi_err_count = s->fb_debug2 & 0xFFFFU;
    int x = HUD_RIGHT_X;
    int y = HUD_RIGHT_Y + 162;
    int w = HUD_RIGHT_W;

    panel(fb, x, y, w, 240, "Pipeline");
    (void)snprintf(text,
                   sizeof(text),
                   "FPS comp %lu.%02lu render %lu.%02lu",
                   (unsigned long)(s->compositor_fps_x100 / 100U),
                   (unsigned long)(s->compositor_fps_x100 % 100U),
                   (unsigned long)(s->renderer_fps_x100 / 100U),
                   (unsigned long)(s->renderer_fps_x100 % 100U));
    line(fb, x, y, w, 0U, text, FB16_COLOR_WHITE);
    (void)snprintf(text,
                   sizeof(text),
                   "FPS blit %lu.%02lu hdmi %lu.%02lu",
                   (unsigned long)(s->apple_fps_x100 / 100U),
                   (unsigned long)(s->apple_fps_x100 % 100U),
                   (unsigned long)(s->fps_x100 / 100U),
                   (unsigned long)(s->fps_x100 % 100U));
    line(fb, x, y, w, 1U, text, FB16_COLOR_WHITE);
    (void)snprintf(text,
                   sizeof(text),
                   "Frames pub %lu seq %lu",
                   (unsigned long)s->compositor_frames_published,
                   (unsigned long)s->renderer_publish_seq);
    line(fb, x, y, w, 2U, text, HUD_MUTED);
    (void)snprintf(text,
                   sizeof(text),
                   "Skipped %lu  Blits %lu",
                   (unsigned long)s->compositor_frames_skipped,
                   (unsigned long)s->compositor_apple_blits);
    line(fb, x, y, w, 3U, text, HUD_MUTED);
    (void)snprintf(text,
                   sizeof(text),
                   "Comp UI %luus Apple %luus",
                   (unsigned long)s->compositor_ui_us,
                   (unsigned long)s->compositor_apple_us);
    line(fb, x, y, w, 4U, text, FB16_COLOR_WHITE);
    (void)snprintf(text,
                   sizeof(text),
                   "Sync %luus Total %luus",
                   (unsigned long)s->compositor_sync_us,
                   (unsigned long)s->compositor_total_us);
    line(fb, x, y, w, 5U, text, FB16_COLOR_WHITE);
    (void)snprintf(text,
                   sizeof(text),
                   "Apple drawn %lu suppress %lu",
                   (unsigned long)s->compositor_apple_drawn,
                   (unsigned long)s->compositor_suppress_apple);
    line(fb, x, y, w, 6U, text, HUD_MUTED);
    (void)snprintf(text,
                   sizeof(text),
                   "VBlank %lu",
                   (unsigned long)s->fb_vblank_count);
    line(fb, x, y, w, 7U, text, HUD_MUTED);
    (void)snprintf(text,
                   sizeof(text),
                   "FB state %lu bursts %lu",
                   (unsigned long)fb_reader_state,
                   (unsigned long)fb_bursts);
    line(fb, x, y, w, 8U, text, (fb_axi_err != 0U) ? HUD_WARN : HUD_MUTED);
    (void)snprintf(text,
                   sizeof(text),
                   "FB err %lu  latched %08lX",
                   (unsigned long)fb_axi_err_count,
                   (unsigned long)s->fb_last_latched);
    line(fb, x, y, w, 9U, text, (fb_axi_err_count != 0U) ? HUD_WARN : HUD_MUTED);
    (void)snprintf(text,
                   sizeof(text),
                   "UI frame %lu keys %lu",
                   (unsigned long)s->ui_frame_count,
                   (unsigned long)s->key_count);
    line(fb, x, y, w, 10U, text, HUD_MUTED);
}

void debug_overlay_draw(uint16_t *fb, const debug_overlay_snapshot_t *snapshot)
{
    if (fb == NULL || snapshot == NULL) {
        return;
    }

    draw_header(fb, snapshot);
    draw_system(fb, snapshot);
    draw_input(fb, snapshot);
    draw_storage(fb, snapshot);
    draw_video(fb, snapshot);
    draw_performance(fb, snapshot);
    draw_softswitches(fb, snapshot);
}
