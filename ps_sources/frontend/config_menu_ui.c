#include "config_menu_ui.h"

#include "config_menu_logo_png.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "../lib/lodepng.h"

static uint32_t *s_cmui_logo_pixels;
static unsigned s_cmui_logo_w;
static unsigned s_cmui_logo_h;
static uint8_t s_cmui_logo_ready;
static uint8_t s_cmui_logo_failed;

static int cmui_scale(int scale)
{
    return (scale > 0) ? scale : CMUI_BODY_SCALE;
}

static int cmui_text_y_for_scale(int row_y, int scale)
{
    return row_y + ((CMUI_ROW_H -
                     (FB16_BUILTIN_FONT_HEIGHT * cmui_scale(scale))) / 2);
}

static int cmui_fit_scale(const char *text, int w, int preferred_scale)
{
    int scale = cmui_scale(preferred_scale);

    while (scale > 1 && cmui_text_width(text, scale) > w) {
        scale--;
    }
    return scale;
}

static void cmui_text_fit_row(uint16_t *fb,
                              int x,
                              int row_y,
                              int w,
                              const char *text,
                              uint32_t fg,
                              uint32_t bg,
                              int preferred_scale)
{
    const int scale = cmui_fit_scale(text, w, preferred_scale);

    cmui_text_clipped(fb,
                      x,
                      cmui_text_y_for_scale(row_y, scale),
                      w,
                      text,
                      fg,
                      bg,
                      scale);
}

static void cmui_copy_clipped(char *dst,
                              size_t dst_len,
                              const char *src,
                              uint32_t max_chars)
{
    uint32_t i = 0U;

    if (dst == NULL || dst_len == 0U) {
        return;
    }
    if (src == NULL) {
        src = "";
    }
    if (max_chars > (uint32_t)(dst_len - 1U)) {
        max_chars = (uint32_t)(dst_len - 1U);
    }
    if (max_chars == 0U) {
        dst[0] = '\0';
        return;
    }

    while (src[i] != '\0' && i < max_chars) {
        dst[i] = src[i];
        i++;
    }
    if (src[i] != '\0' && max_chars >= 3U) {
        dst[max_chars - 3U] = '.';
        dst[max_chars - 2U] = '.';
        dst[max_chars - 1U] = '.';
        dst[max_chars] = '\0';
    } else {
        dst[i] = '\0';
    }
}

static uint8_t cmui_logo_load(void)
{
    unsigned char *rgba = NULL;
    uint32_t *pixels;
    unsigned w = 0U;
    unsigned h = 0U;
    unsigned error;
    size_t pixel_count;

    if (s_cmui_logo_ready != 0U) {
        return 1U;
    }
    if (s_cmui_logo_failed != 0U) {
        return 0U;
    }

    error = lodepng_decode32(&rgba, &w, &h,
                             config_menu_logo_png,
                             config_menu_logo_png_len);
    if (error != 0U || w != config_menu_logo_png_width ||
        h != config_menu_logo_png_height || w == 0U || h == 0U) {
        free(rgba);
        s_cmui_logo_failed = 1U;
        return 0U;
    }

    pixel_count = (size_t)w * (size_t)h;
    pixels = (uint32_t *)malloc(pixel_count * sizeof(uint32_t));
    if (pixels == NULL) {
        free(rgba);
        s_cmui_logo_failed = 1U;
        return 0U;
    }

    for (size_t i = 0U; i < pixel_count; ++i) {
        const uint32_t r = rgba[(i * 4U) + 0U];
        const uint32_t g = rgba[(i * 4U) + 1U];
        const uint32_t b = rgba[(i * 4U) + 2U];
        const uint32_t a = rgba[(i * 4U) + 3U];

        pixels[i] = (a << 24U) | (r << 16U) | (g << 8U) | b;
    }

    free(rgba);
    s_cmui_logo_pixels = pixels;
    s_cmui_logo_w = w;
    s_cmui_logo_h = h;
    s_cmui_logo_ready = 1U;
    return 1U;
}

/* Blend a straight-alpha BGRA32 logo pixel over a 565 destination.
 * The destination widens 5/6/5 -> 8/8/8 (replicate-high), the blend
 * runs in 8-bit, and the result packs back to 565. */
static uint16_t cmui_alpha_blend(uint32_t src, uint16_t dst565)
{
    const uint32_t a = (src >> 24U) & 0xFFU;
    const uint32_t inv = 255U - a;
    uint32_t dst;
    uint32_t r;
    uint32_t g;
    uint32_t b;

    if (a == 0U) {
        return dst565;
    }
    if (a == 255U) {
        return fb16_from_bgra32(src);
    }

    dst = fb16_to_bgra32(dst565);
    r = ((((src >> 16U) & 0xFFU) * a) +
         (((dst >> 16U) & 0xFFU) * inv) + 127U) / 255U;
    g = ((((src >> 8U) & 0xFFU) * a) +
         (((dst >> 8U) & 0xFFU) * inv) + 127U) / 255U;
    b = (((src & 0xFFU) * a) +
         ((dst & 0xFFU) * inv) + 127U) / 255U;
    return FB16_RGB(r, g, b);
}

static uint8_t cmui_logo_draw(uint16_t *fb, int x, int y)
{
    if (fb == NULL || cmui_logo_load() == 0U) {
        return 0U;
    }

    for (unsigned row = 0U; row < s_cmui_logo_h; ++row) {
        const int dy = y + (int)row;
        if ((unsigned)dy >= (unsigned)FB16_HEIGHT) {
            continue;
        }
        for (unsigned col = 0U; col < s_cmui_logo_w; ++col) {
            const int dx = x + (int)col;
            uint16_t *dst;
            uint32_t src;

            if ((unsigned)dx >= (unsigned)FB16_WIDTH) {
                continue;
            }
            src = s_cmui_logo_pixels[((size_t)row * (size_t)s_cmui_logo_w) +
                                      (size_t)col];
            if ((src >> 24U) == 0U) {
                continue;
            }
            dst = &fb[((size_t)dy * (size_t)FB16_WIDTH) + (size_t)dx];
            *dst = cmui_alpha_blend(src, *dst);
        }
    }
    return 1U;
}

void cmui_screen_rects(cmui_rect_t *nav,
                       cmui_rect_t *body,
                       cmui_rect_t *footer)
{
    const int footer_y = CMUI_SCREEN_H - CMUI_MARGIN_Y - CMUI_FOOTER_H;
    const int nav_y = CMUI_MARGIN_Y + CMUI_HEADER_H;
    const int nav_h = footer_y - nav_y - 18;
    const int body_x = CMUI_MARGIN_X + CMUI_NAV_W + CMUI_NAV_GAP;
    const int body_w = CMUI_SCREEN_W - body_x - CMUI_MARGIN_X;

    if (nav != NULL) {
        nav->x = CMUI_MARGIN_X;
        nav->y = nav_y;
        nav->w = CMUI_NAV_W;
        nav->h = nav_h;
    }
    if (body != NULL) {
        body->x = body_x;
        body->y = nav_y;
        body->w = body_w;
        body->h = nav_h;
    }
    if (footer != NULL) {
        footer->x = CMUI_MARGIN_X;
        footer->y = footer_y;
        footer->w = CMUI_SCREEN_W - (2 * CMUI_MARGIN_X);
        footer->h = CMUI_FOOTER_H;
    }
}

void cmui_clear(uint16_t *fb)
{
    fb16_fill_rect(fb, 0, 0, FB16_WIDTH, FB16_HEIGHT, CMUI_COLOR_BG);
}

void cmui_panel(uint16_t *fb, const cmui_rect_t *rect, uint32_t bg)
{
    if (rect == NULL || rect->w <= 0 || rect->h <= 0) {
        return;
    }

    fb16_fill_rect(fb, rect->x, rect->y, rect->w, rect->h, bg);
    fb16_rect(fb, rect->x, rect->y, rect->w, rect->h, CMUI_COLOR_BORDER_SOFT);
}

void cmui_header(uint16_t *fb,
                 const char *brand,
                 const char *version,
                 uint8_t usb_owned)
{
    const char *badge = (usb_owned != 0U) ? "ACTIVE" : "BOOT MENU";
    const char *owner = (usb_owned != 0U) ? "USB device" : "Apple keyboard";
    const int logo_x = CMUI_MARGIN_X;
    const int logo_y = CMUI_MARGIN_Y;
    const int badge_w = cmui_text_width(badge, CMUI_BODY_SCALE) + 48;
    const int badge_h = 30;
    const int badge_x = CMUI_SCREEN_W - CMUI_MARGIN_X - badge_w;
    const int badge_y = CMUI_MARGIN_Y + 2;
    const int owner_w = cmui_text_width(owner, CMUI_BODY_SCALE);
    const int version_w = (version != NULL) ?
        cmui_text_width(version, CMUI_SMALL_SCALE) : 0;
    const int divider_y = CMUI_MARGIN_Y + CMUI_HEADER_H - 26;

    if (cmui_logo_draw(fb, logo_x, logo_y) == 0U) {
        cmui_text(fb, logo_x, logo_y, brand, CMUI_COLOR_TEXT, CMUI_COLOR_BG,
                  CMUI_BRAND_SCALE);
    }

    fb16_fill_rect(fb, badge_x, badge_y, badge_w, badge_h,
                   CMUI_COLOR_ROW_ACTIVE);
    fb16_rect(fb, badge_x, badge_y, badge_w, badge_h,
              CMUI_COLOR_BORDER_SOFT);
    fb16_fill_circle(fb,
                     badge_x + 16,
                     badge_y + (badge_h / 2),
                     5,
                     (usb_owned != 0U) ? CMUI_COLOR_WARN :
                     CMUI_COLOR_ACCENT);
    cmui_text(fb,
              badge_x + 32,
              badge_y + 7,
              badge,
              (usb_owned != 0U) ? CMUI_COLOR_WARN : CMUI_COLOR_ACCENT,
              CMUI_COLOR_ROW_ACTIVE,
              CMUI_BODY_SCALE);
    cmui_text(fb,
              CMUI_SCREEN_W - CMUI_MARGIN_X - owner_w,
              badge_y + badge_h + 8,
              owner,
              CMUI_COLOR_TEXT,
              CMUI_COLOR_BG,
              CMUI_BODY_SCALE);
    if (version_w > 0) {
        cmui_text(fb,
                  CMUI_SCREEN_W - CMUI_MARGIN_X - version_w,
                  badge_y + badge_h + 36,
                  version,
                  CMUI_COLOR_MUTED,
                  CMUI_COLOR_BG,
                  CMUI_SMALL_SCALE);
    }

    fb16_fill_rect(fb,
                   CMUI_MARGIN_X,
                   divider_y,
                   CMUI_SCREEN_W - (2 * CMUI_MARGIN_X),
                   2,
                   CMUI_COLOR_BORDER_SOFT);
}

void cmui_text(uint16_t *fb,
               int x,
               int y,
               const char *text,
               uint32_t fg,
               uint32_t bg,
               int scale)
{
    if (text == NULL || text[0] == '\0') {
        return;
    }
    fb16_string_scaled(fb, x, y, text, fg, bg, cmui_scale(scale));
}

void cmui_text_clipped(uint16_t *fb,
                       int x,
                       int y,
                       int w,
                       const char *text,
                       uint32_t fg,
                       uint32_t bg,
                       int scale)
{
    char clipped[192];
    uint32_t max_chars;
    int cell_w;

    scale = cmui_scale(scale);
    if (w <= 0) {
        return;
    }

    cell_w = FB16_BUILTIN_FONT_ADVANCE_X * scale;
    max_chars = (cell_w > 0) ? (uint32_t)(w / cell_w) : 0U;
    cmui_copy_clipped(clipped, sizeof(clipped), text, max_chars);
    cmui_text(fb, x, y, clipped, fg, bg, scale);
}

int cmui_text_width(const char *text, int scale)
{
    if (text == NULL) {
        return 0;
    }
    return (int)strlen(text) * FB16_BUILTIN_FONT_ADVANCE_X * cmui_scale(scale);
}

void cmui_title(uint16_t *fb, int x, int y, const char *text)
{
    cmui_text(fb, x, y, text, CMUI_COLOR_TEXT, CMUI_COLOR_BG, CMUI_TITLE_SCALE);
}

void cmui_caption(uint16_t *fb, int x, int y, int w, const char *text)
{
    cmui_text_clipped(fb, x, y, w, text, CMUI_COLOR_MUTED, CMUI_COLOR_BG,
                      CMUI_SMALL_SCALE);
}

void cmui_help_panel(uint16_t *fb,
                     const cmui_rect_t *rect,
                     const char *title,
                     const char * const *lines,
                     uint32_t line_count)
{
    const int pad_x = 18;
    const int pad_y = 14;
    const int line_h = 28;
    int text_y;

    if (rect == NULL || rect->w <= 0 || rect->h <= 0) {
        return;
    }

    fb16_fill_rect(fb, rect->x, rect->y, rect->w, rect->h, CMUI_COLOR_PANEL);
    fb16_rect(fb, rect->x, rect->y, rect->w, rect->h, CMUI_COLOR_BORDER_SOFT);
    fb16_fill_rect(fb, rect->x, rect->y, 5, rect->h, CMUI_COLOR_ACCENT_2);

    if (title != NULL && title[0] != '\0') {
        cmui_text(fb,
                  rect->x + pad_x,
                  rect->y + pad_y,
                  title,
                  CMUI_COLOR_ACCENT,
                  CMUI_COLOR_PANEL,
                  CMUI_BODY_SCALE);
        text_y = rect->y + pad_y + 34;
    } else {
        text_y = rect->y + pad_y;
    }

    for (uint32_t i = 0U; i < line_count; ++i) {
        if (text_y + FB16_BUILTIN_FONT_HEIGHT > rect->y + rect->h - pad_y) {
            break;
        }
        cmui_text_clipped(fb,
                          rect->x + pad_x,
                          text_y,
                          rect->w - (2 * pad_x),
                          lines[i],
                          CMUI_COLOR_MUTED,
                          CMUI_COLOR_PANEL,
                          CMUI_SMALL_SCALE);
        text_y += line_h;
    }
}

void cmui_nav_item(uint16_t *fb,
                   const cmui_rect_t *rect,
                   const char *label,
                   uint8_t active,
                   uint8_t enabled)
{
    uint32_t bg;
    uint32_t fg;

    if (rect == NULL || rect->w <= 0 || rect->h <= 0) {
        return;
    }

    bg = (enabled == 0U) ? CMUI_COLOR_ROW_DISABLED :
         ((active != 0U) ? CMUI_COLOR_ROW_ACTIVE : CMUI_COLOR_BG);
    fg = (enabled == 0U) ? CMUI_COLOR_DIM :
         ((active != 0U) ? CMUI_COLOR_TEXT : CMUI_COLOR_MUTED);
    fb16_fill_rect(fb, rect->x, rect->y, rect->w, rect->h, bg);
    if (active != 0U) {
        fb16_fill_rect(fb, rect->x, rect->y, 5, rect->h, CMUI_COLOR_ACCENT);
        fb16_rect(fb, rect->x, rect->y, rect->w, rect->h,
                  CMUI_COLOR_BORDER);
    }
    cmui_text_fit_row(fb,
                      rect->x + 22,
                      rect->y,
                      rect->w - 34,
                      label,
                      fg,
                      bg,
                      CMUI_BODY_SCALE);
}

void cmui_row(uint16_t *fb,
              int x,
              int y,
              int w,
              uint8_t focused,
              uint8_t dimmed,
              const char *text)
{
    const uint32_t bg = (dimmed != 0U) ? CMUI_COLOR_ROW_DISABLED :
                        ((focused != 0U) ? CMUI_COLOR_ROW_ACTIVE :
                         CMUI_COLOR_ROW);
    const uint32_t fg = (dimmed != 0U) ? CMUI_COLOR_DIM :
                        ((focused != 0U) ? CMUI_COLOR_TEXT : CMUI_COLOR_MUTED);

    if (w <= 0) {
        return;
    }

    fb16_fill_rect(fb, x, y, w, CMUI_ROW_H, bg);
    if (focused != 0U) {
        fb16_fill_rect(fb, x, y, 5, CMUI_ROW_H,
                       (dimmed != 0U) ? CMUI_COLOR_DISABLED_EDGE :
                       CMUI_COLOR_ACCENT);
        fb16_rect(fb, x, y, w, CMUI_ROW_H, CMUI_COLOR_BORDER);
    } else if (dimmed != 0U) {
        fb16_fill_rect(fb, x, y, 5, CMUI_ROW_H, CMUI_COLOR_DISABLED_EDGE);
    }
    cmui_text_fit_row(fb, x + 18, y, w - 36, text, fg, bg,
                      CMUI_BODY_SCALE);
}

void cmui_value_row(uint16_t *fb,
                    int x,
                    int y,
                    int w,
                    uint8_t focused,
                    uint8_t dimmed,
                    const char *label,
                    const char *value)
{
    const uint32_t bg = (dimmed != 0U) ? CMUI_COLOR_ROW_DISABLED :
                        ((focused != 0U) ? CMUI_COLOR_ROW_ACTIVE :
                         CMUI_COLOR_ROW);
    const uint32_t label_fg = (dimmed != 0U) ? CMUI_COLOR_DIM :
                              ((focused != 0U) ? CMUI_COLOR_TEXT :
                               CMUI_COLOR_MUTED);
    const uint32_t value_fg = (dimmed != 0U) ? CMUI_COLOR_DIM :
                              ((focused != 0U) ? CMUI_COLOR_ACCENT :
                               CMUI_COLOR_TEXT);
    const int label_w = (w >= 900) ? CMUI_VALUE_LABEL_W : ((w * 46) / 100);
    const int value_w = w - label_w - 48;
    const int value_x = x + 18 + label_w + 30;

    if (w <= 0) {
        return;
    }

    fb16_fill_rect(fb, x, y, w, CMUI_ROW_H, bg);
    if (focused != 0U) {
        fb16_fill_rect(fb, x, y, 5, CMUI_ROW_H,
                       (dimmed != 0U) ? CMUI_COLOR_DISABLED_EDGE :
                       CMUI_COLOR_ACCENT);
        fb16_rect(fb, x, y, w, CMUI_ROW_H, CMUI_COLOR_BORDER);
    } else if (dimmed != 0U) {
        fb16_fill_rect(fb, x, y, 5, CMUI_ROW_H, CMUI_COLOR_DISABLED_EDGE);
    }
    cmui_text_fit_row(fb, x + 18, y, label_w, label, label_fg, bg,
                      CMUI_BODY_SCALE);
    cmui_text_fit_row(fb, value_x, y, value_w, value, value_fg, bg,
                      CMUI_BODY_SCALE);
}

void cmui_check_row_ex(uint16_t *fb,
                       int x,
                       int y,
                       int w,
                       uint8_t focused,
                       uint8_t checked,
                       uint8_t dimmed,
                       const char *label)
{
    const uint32_t bg = (dimmed != 0U) ? CMUI_COLOR_ROW_DISABLED :
                        ((focused != 0U) ? CMUI_COLOR_ROW_ACTIVE : CMUI_COLOR_ROW);
    const uint32_t fg = (dimmed != 0U) ? CMUI_COLOR_DIM :
                        ((focused != 0U) ? CMUI_COLOR_TEXT : CMUI_COLOR_MUTED);
    const uint32_t edge = (dimmed != 0U) ? CMUI_COLOR_DISABLED_EDGE :
                          CMUI_COLOR_BORDER;
    const uint32_t accent = (dimmed != 0U) ? CMUI_COLOR_DISABLED_EDGE :
                            CMUI_COLOR_ACCENT;
    const int box = 22;
    const int box_x = x + 18;
    const int box_y = y + ((CMUI_ROW_H - box) / 2);

    if (w <= 0) {
        return;
    }

    fb16_fill_rect(fb, x, y, w, CMUI_ROW_H, bg);
    if (focused != 0U) {
        fb16_fill_rect(fb, x, y, 5, CMUI_ROW_H, accent);
        fb16_rect(fb, x, y, w, CMUI_ROW_H, CMUI_COLOR_BORDER);
    } else if (dimmed != 0U) {
        fb16_fill_rect(fb, x, y, 5, CMUI_ROW_H, CMUI_COLOR_DISABLED_EDGE);
    }
    fb16_fill_rect(fb, box_x + 1, box_y + 1, box - 2, box - 2, bg);
    fb16_rect(fb, box_x, box_y, box, box,
              (checked != 0U) ? accent : edge);
    if (checked != 0U) {
        fb16_line(fb, box_x + 4, box_y + 12,
                  box_x + 9, box_y + 17,
                  accent);
        fb16_line(fb, box_x + 9, box_y + 17,
                  box_x + 18, box_y + 5,
                  accent);
    }
    cmui_text_fit_row(fb, x + 56, y, w - 74, label, fg, bg,
                      CMUI_BODY_SCALE);
}

void cmui_check_row(uint16_t *fb,
                    int x,
                    int y,
                    int w,
                    uint8_t focused,
                    uint8_t checked,
                    const char *label)
{
    cmui_check_row_ex(fb, x, y, w, focused, checked, 0U, label);
}

void cmui_lock(uint16_t *fb,
               int x,
               int y,
               uint8_t locked,
               uint8_t focused,
               uint32_t bg)
{
    const uint32_t fg = (focused != 0U) ? CMUI_COLOR_ACCENT : CMUI_COLOR_MUTED;

    if (locked == 0U) {
        return;
    }
    fb16_fill_rect(fb, x, y + 11, 18, 15, bg);
    fb16_rect(fb, x + 2, y + 17, 14, 12, fg);
    fb16_hline(fb, x + 6, y + 22, 6, fg);
    fb16_vline(fb, x + 8, y + 22, 5, fg);
    fb16_hline(fb, x + 6, y + 13, 6, fg);
    fb16_vline(fb, x + 4, y + 14, 5, fg);
    fb16_vline(fb, x + 14, y + 14, 5, fg);
}

void cmui_slider(uint16_t *fb,
                 int x,
                 int y,
                 int w,
                 uint8_t focused,
                 uint8_t dimmed,
                 const char *label,
                 const char *left_text,
                 const char *right_text,
                 uint32_t value,
                 uint32_t max_value,
                 uint32_t center_value,
                 const char *value_text)
{
    const uint32_t bg = (dimmed != 0U) ? CMUI_COLOR_ROW_DISABLED :
                        ((focused != 0U) ? CMUI_COLOR_ROW_ACTIVE : CMUI_COLOR_ROW);
    const uint32_t fg = (dimmed != 0U) ? CMUI_COLOR_DIM :
                        ((focused != 0U) ? CMUI_COLOR_TEXT : CMUI_COLOR_MUTED);
    const uint32_t accent = (dimmed != 0U) ? CMUI_COLOR_DISABLED_EDGE :
                            CMUI_COLOR_ACCENT;
    const int label_w = (w >= 900) ? CMUI_SLIDER_LABEL_W : 152;
    const int value_w = 88;
    const int bar_x = x + 18 + label_w + 56;
    const int value_x = x + w - value_w - 18;
    const int left_text_w = cmui_text_width(left_text, CMUI_SMALL_SCALE);
    const int right_text_w = cmui_text_width(right_text, CMUI_SMALL_SCALE);
    const int left_text_x = bar_x - left_text_w - 16;
    const int right_text_x = value_x - 72 - right_text_w;
    const int bar_w = right_text_x - 16 - bar_x;
    const int bar_y = y + 23;
    int marker_x;

    if (w <= 0) {
        return;
    }
    if (max_value == 0U) {
        max_value = 1U;
    }
    if (value > max_value) {
        value = max_value;
    }

    fb16_fill_rect(fb, x, y, w, CMUI_ROW_H, bg);
    if (focused != 0U) {
        fb16_fill_rect(fb, x, y, 5, CMUI_ROW_H, accent);
        fb16_rect(fb, x, y, w, CMUI_ROW_H, CMUI_COLOR_BORDER);
    } else if (dimmed != 0U) {
        fb16_fill_rect(fb, x, y, 5, CMUI_ROW_H, CMUI_COLOR_DISABLED_EDGE);
    }
    cmui_text_fit_row(fb, x + 18, y, label_w, label, fg, bg,
                      CMUI_BODY_SCALE);
    if (bar_w <= 24) {
        cmui_text_fit_row(fb, x + 18 + label_w + 24, y,
                          w - label_w - value_w - 60,
                          value_text,
                          (dimmed != 0U) ? CMUI_COLOR_DIM : CMUI_COLOR_ACCENT,
                          bg,
                          CMUI_SMALL_SCALE);
        return;
    }
    cmui_text(fb, left_text_x, y + 15, left_text, fg, bg, CMUI_SMALL_SCALE);
    cmui_text(fb, right_text_x, y + 15, right_text, fg, bg,
              CMUI_SMALL_SCALE);
    fb16_fill_rect(fb, bar_x, bar_y, bar_w, 3,
                   (dimmed != 0U) ? CMUI_COLOR_DISABLED_EDGE :
                   CMUI_COLOR_BORDER);
    if (center_value <= max_value) {
        const int cx = bar_x + (int)((uint64_t)center_value *
                                     (uint64_t)bar_w / max_value);
        fb16_fill_rect(fb, cx - 1, bar_y - 7, 3, 17,
                       (dimmed != 0U) ? CMUI_COLOR_DISABLED_EDGE :
                       CMUI_COLOR_BORDER);
    }
    marker_x = bar_x + (int)((uint64_t)value * (uint64_t)bar_w / max_value);
    fb16_fill_rect(fb, marker_x - 6, bar_y - 8, 12, 19, accent);
    cmui_text_clipped(fb, value_x, y + 15, value_w,
                      value_text,
                      (dimmed != 0U) ? CMUI_COLOR_DIM : CMUI_COLOR_ACCENT,
                      bg,
                      CMUI_SMALL_SCALE);
}

void cmui_footer(uint16_t *fb,
                 const cmui_rect_t *footer,
                 const char *status,
                 uint8_t warning,
                 uint8_t usb_owned)
{
    uint32_t status_color;
    int x;

    static const char * const apple_keys[] = {
        "Tab/Del", "<>", "Enter", "Esc"
    };
    static const char * const apple_labels[] = {
        "Navigate", "Change", "Select", "Close"
    };
    static const char * const usb_keys[] = {
        "USB", "ACTIVE"
    };
    static const char * const usb_labels[] = {
        "Navigate", "USB device"
    };

    if (footer == NULL || footer->w <= 0 || footer->h <= 0) {
        return;
    }

    fb16_fill_rect(fb, footer->x, footer->y, footer->w, 2,
                   CMUI_COLOR_BORDER_SOFT);
    x = footer->x;
    if (usb_owned != 0U) {
        for (uint32_t i = 0U; i < (sizeof(usb_keys) / sizeof(usb_keys[0])); ++i) {
            const int key_w = cmui_text_width(usb_keys[i], CMUI_SMALL_SCALE) + 18;
            fb16_fill_rect(fb, x, footer->y + 22, key_w, 28,
                           CMUI_COLOR_ROW_ACTIVE);
            fb16_rect(fb, x, footer->y + 22, key_w, 28,
                      CMUI_COLOR_BORDER_SOFT);
            cmui_text(fb, x + 9, footer->y + 28, usb_keys[i],
                      (i == 1U) ? CMUI_COLOR_WARN : CMUI_COLOR_TEXT,
                      CMUI_COLOR_ROW_ACTIVE, CMUI_SMALL_SCALE);
            cmui_text(fb, x + key_w + 8, footer->y + 28, usb_labels[i],
                      CMUI_COLOR_MUTED, CMUI_COLOR_BG, CMUI_SMALL_SCALE);
            x += key_w + 8 + cmui_text_width(usb_labels[i], CMUI_SMALL_SCALE) + 24;
        }
    } else {
        for (uint32_t i = 0U; i < (sizeof(apple_keys) / sizeof(apple_keys[0])); ++i) {
            const int key_w = cmui_text_width(apple_keys[i], CMUI_SMALL_SCALE) + 18;
            fb16_fill_rect(fb, x, footer->y + 22, key_w, 28,
                           CMUI_COLOR_ROW_ACTIVE);
            fb16_rect(fb, x, footer->y + 22, key_w, 28,
                      CMUI_COLOR_BORDER_SOFT);
            cmui_text(fb, x + 9, footer->y + 28, apple_keys[i],
                      CMUI_COLOR_TEXT, CMUI_COLOR_ROW_ACTIVE,
                      CMUI_SMALL_SCALE);
            cmui_text(fb, x + key_w + 8, footer->y + 28, apple_labels[i],
                      CMUI_COLOR_MUTED, CMUI_COLOR_BG, CMUI_SMALL_SCALE);
            x += key_w + 8 + cmui_text_width(apple_labels[i], CMUI_SMALL_SCALE) + 24;
        }
    }

    if (status == NULL || status[0] == '\0') {
        return;
    }

    status_color = (warning != 0U) ? CMUI_COLOR_WARN : CMUI_COLOR_SUCCESS;
    {
        const int max_status_w = footer->w / 2;
        int status_w = cmui_text_width(status, CMUI_SMALL_SCALE);
        if (status_w > max_status_w) {
            status_w = max_status_w;
        }
        cmui_text_clipped(fb,
                          footer->x + footer->w - status_w,
                          footer->y + 28,
                          status_w,
                          status,
                          status_color,
                          CMUI_COLOR_BG,
                          CMUI_SMALL_SCALE);
    }
}
