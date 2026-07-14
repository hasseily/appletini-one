#ifndef CONFIG_MENU_UI_H
#define CONFIG_MENU_UI_H

#include <stdint.h>

#include "../lib/fb16.h"

#define CMUI_SCREEN_W FB16_WIDTH
#define CMUI_SCREEN_H FB16_HEIGHT

#define CMUI_MARGIN_X 48
#define CMUI_MARGIN_Y 38
#define CMUI_NAV_W 300
#define CMUI_NAV_GAP 32
#define CMUI_HEADER_H 116
#define CMUI_FOOTER_H 58
#define CMUI_ROW_H 34
#define CMUI_ROW_GAP 8
#define CMUI_BRAND_SCALE 3
#define CMUI_BODY_SCALE 2
#define CMUI_SMALL_SCALE 2
#define CMUI_TITLE_SCALE 2
#define CMUI_VALUE_LABEL_W 360
#define CMUI_SLIDER_LABEL_W 320

#define CMUI_COLOR_BG FB16_RGB(0x08, 0x0A, 0x0D)
#define CMUI_COLOR_PANEL FB16_RGB(0x10, 0x14, 0x19)
#define CMUI_COLOR_PANEL_2 FB16_RGB(0x15, 0x1B, 0x22)
#define CMUI_COLOR_ROW FB16_RGB(0x12, 0x17, 0x1D)
#define CMUI_COLOR_ROW_DISABLED FB16_RGB(0x0B, 0x0E, 0x12)
#define CMUI_COLOR_ROW_ACTIVE FB16_RGB(0x1E, 0x2A, 0x32)
#define CMUI_COLOR_BORDER FB16_RGB(0x31, 0x3A, 0x43)
#define CMUI_COLOR_BORDER_SOFT FB16_RGB(0x20, 0x28, 0x30)
#define CMUI_COLOR_TEXT FB16_RGB(0xE8, 0xEE, 0xF2)
#define CMUI_COLOR_MUTED FB16_RGB(0xA6, 0xB3, 0xBD)
#define CMUI_COLOR_DIM FB16_RGB(0x3E, 0x46, 0x4E)
#define CMUI_COLOR_DISABLED_EDGE FB16_RGB(0x27, 0x2D, 0x34)
#define CMUI_COLOR_ACCENT FB16_RGB(0x39, 0xC7, 0xB8)
#define CMUI_COLOR_ACCENT_2 FB16_RGB(0x6D, 0x9D, 0xFF)
#define CMUI_COLOR_WARN FB16_RGB(0xF2, 0xB5, 0x5B)
#define CMUI_COLOR_SUCCESS FB16_RGB(0x8E, 0xD6, 0x8A)

typedef struct {
    int x;
    int y;
    int w;
    int h;
} cmui_rect_t;

void cmui_screen_rects(cmui_rect_t *nav,
                       cmui_rect_t *body,
                       cmui_rect_t *footer);
void cmui_clear(uint16_t *fb);
void cmui_panel(uint16_t *fb, const cmui_rect_t *rect, uint32_t bg);
void cmui_header(uint16_t *fb,
                 const char *brand,
                 const char *version,
                 uint8_t usb_owned);
void cmui_text(uint16_t *fb,
               int x,
               int y,
               const char *text,
               uint32_t fg,
               uint32_t bg,
               int scale);
void cmui_text_clipped(uint16_t *fb,
                       int x,
                       int y,
                       int w,
                       const char *text,
                       uint32_t fg,
                       uint32_t bg,
                       int scale);
int cmui_text_width(const char *text, int scale);
void cmui_title(uint16_t *fb, int x, int y, const char *text);
void cmui_caption(uint16_t *fb, int x, int y, int w, const char *text);
void cmui_help_panel(uint16_t *fb,
                     const cmui_rect_t *rect,
                     const char *title,
                     const char * const *lines,
                     uint32_t line_count);
void cmui_nav_item(uint16_t *fb,
                   const cmui_rect_t *rect,
                   const char *label,
                   uint8_t active,
                   uint8_t enabled);
void cmui_row(uint16_t *fb,
              int x,
              int y,
              int w,
              uint8_t focused,
              uint8_t dimmed,
              const char *text);
void cmui_value_row(uint16_t *fb,
                    int x,
                    int y,
                    int w,
                    uint8_t focused,
                    uint8_t dimmed,
                    const char *label,
                    const char *value);
void cmui_check_row(uint16_t *fb,
                    int x,
                    int y,
                    int w,
                    uint8_t focused,
                    uint8_t checked,
                    const char *label);
void cmui_check_row_ex(uint16_t *fb,
                       int x,
                       int y,
                       int w,
                       uint8_t focused,
                       uint8_t checked,
                       uint8_t dimmed,
                       const char *label);
void cmui_lock(uint16_t *fb,
               int x,
               int y,
               uint8_t locked,
               uint8_t focused,
               uint32_t bg);
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
                 const char *value_text);
void cmui_footer(uint16_t *fb,
                 const cmui_rect_t *footer,
                 const char *status,
                 uint8_t warning,
                 uint8_t usb_owned);

#endif
