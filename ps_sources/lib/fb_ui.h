#ifndef FB_UI_H
#define FB_UI_H

#include <stdint.h>

typedef struct {
    int x;
    int y;
    int w;
    int h;
} fb_ui_rect_t;

typedef enum {
    FB_UI_COL_L = 0,
    FB_UI_COL_R
} fb_ui_col_t;

typedef struct {
    fb_ui_rect_t left;
    fb_ui_rect_t right;
    uint32_t line_h;
    int font_scale;
} fb_ui_two_col_layout_t;

#define FB_UI_INDEX_NONE 0xFFFFFFFFU

typedef struct {
    uint32_t bg;
    uint32_t fg;
    uint32_t active_bg;
    uint32_t active_fg;
    uint32_t border;
    uint32_t accent;
    uint32_t selected_fg;
} fb_ui_menu_style_t;

extern const fb_ui_menu_style_t fb_ui_default_menu_style;

void fb_ui_text_clipped(uint16_t *fb,
                        int x,
                        int y,
                        int w,
                        const char *text,
                        uint32_t fg,
                        uint32_t bg,
                        int scale);

void fb_ui_column_line(uint16_t *fb,
                       const fb_ui_two_col_layout_t *layout,
                       fb_ui_col_t col,
                       uint32_t row,
                       const char *text,
                       uint32_t fg,
                       uint32_t bg);

void fb_ui_inset_border(uint16_t *fb,
                        const fb_ui_rect_t *rect,
                        uint32_t outer,
                        uint32_t inner,
                        int inset);

void fb_ui_footer_band(uint16_t *fb, int height, uint32_t bg);

int fb_ui_key_badge(uint16_t *fb,
                    int x,
                    int y,
                    const char *key,
                    const char *label,
                    uint32_t key_bg,
                    uint32_t label_bg,
                    uint32_t fg,
                    uint32_t border,
                    int scale);

int fb_ui_menu_row_height(int scale);

void fb_ui_highlight_rect(uint16_t *fb,
                          const fb_ui_rect_t *rect,
                          const fb_ui_menu_style_t *style,
                          uint8_t active);

void fb_ui_checkbox(uint16_t *fb,
                    int x,
                    int y,
                    int w,
                    const char *label,
                    uint8_t checked,
                    uint8_t active,
                    const fb_ui_menu_style_t *style,
                    int scale);

void fb_ui_radio_button(uint16_t *fb,
                        int x,
                        int y,
                        int w,
                        const char *label,
                        uint8_t selected,
                        uint8_t active,
                        const fb_ui_menu_style_t *style,
                        int scale);

void fb_ui_listbox_entry(uint16_t *fb,
                         const fb_ui_rect_t *rect,
                         const char *label,
                         uint8_t selected,
                         uint8_t active,
                         const fb_ui_menu_style_t *style,
                         int scale);

void fb_ui_listbox(uint16_t *fb,
                   const fb_ui_rect_t *rect,
                   const char * const *items,
                   uint32_t item_count,
                   uint32_t top_index,
                   uint32_t selected_index,
                   uint32_t active_index,
                   const fb_ui_menu_style_t *style,
                   int scale);

#endif
