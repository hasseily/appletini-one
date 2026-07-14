#include "fb_ui.h"

#include <stddef.h>
#include <string.h>

#include "fb16.h"

#define FB_UI_TEXT_CELL_W 8
#define FB_UI_MIN_CONTROL_PX 10

const fb_ui_menu_style_t fb_ui_default_menu_style = {
    FB16_COLOR_BLACK,
    FB16_COLOR_WHITE,
    FB16_COLOR_NAVY,
    FB16_COLOR_WHITE,
    FB16_COLOR_DARK_GRAY,
    FB16_COLOR_CYAN,
    FB16_COLOR_GREEN
};

static const fb_ui_menu_style_t *fb_ui_style_or_default(const fb_ui_menu_style_t *style)
{
    return (style != NULL) ? style : &fb_ui_default_menu_style;
}

static int fb_ui_scale_or_default(int scale)
{
    return (scale > 0) ? scale : 1;
}

static int fb_ui_text_cell_w(int scale)
{
    return FB_UI_TEXT_CELL_W * fb_ui_scale_or_default(scale);
}

static int fb_ui_control_size(int row_h)
{
    int size = row_h - 4;

    if (size < FB_UI_MIN_CONTROL_PX) {
        size = FB_UI_MIN_CONTROL_PX;
    }
    return size;
}

static void fb_ui_copy_clipped(char *dst,
                               size_t dst_size,
                               const char *text,
                               uint32_t max_chars)
{
    uint32_t i = 0U;

    if (dst == NULL || dst_size == 0U) {
        return;
    }
    if (text == NULL) {
        text = "";
    }
    if (max_chars > (uint32_t)(dst_size - 1U)) {
        max_chars = (uint32_t)(dst_size - 1U);
    }
    if (max_chars == 0U) {
        dst[0] = '\0';
        return;
    }

    while (text[i] != '\0' && i < max_chars) {
        dst[i] = text[i];
        i++;
    }

    if (text[i] != '\0' && max_chars >= 3U) {
        dst[max_chars - 3U] = '.';
        dst[max_chars - 2U] = '.';
        dst[max_chars - 1U] = '.';
        dst[max_chars] = '\0';
    } else {
        dst[i] = '\0';
    }
}

void fb_ui_text_clipped(uint16_t *fb,
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
    uint32_t cell_w;

    if (w <= 0) {
        return;
    }
    scale = fb_ui_scale_or_default(scale);

    cell_w = (uint32_t)fb_ui_text_cell_w(scale);
    max_chars = cell_w != 0U ? ((uint32_t)w / cell_w) : 0U;
    fb_ui_copy_clipped(clipped, sizeof(clipped), text, max_chars);
    if (clipped[0] == '\0') {
        return;
    }

    fb16_string_scaled(fb, x, y, clipped, fg, bg, scale);
}

void fb_ui_column_line(uint16_t *fb,
                       const fb_ui_two_col_layout_t *layout,
                       fb_ui_col_t col,
                       uint32_t row,
                       const char *text,
                       uint32_t fg,
                       uint32_t bg)
{
    const fb_ui_rect_t *rect;
    int scale;
    int text_w;
    uint32_t line_h;

    if (layout == NULL) {
        return;
    }

    rect = (col == FB_UI_COL_R) ? &layout->right : &layout->left;
    scale = fb_ui_scale_or_default(layout->font_scale);
    line_h = layout->line_h != 0U ?
             layout->line_h :
             (uint32_t)(FB16_BUILTIN_FONT_HEIGHT * scale);
    text_w = rect->w > fb_ui_text_cell_w(scale) ?
             rect->w - fb_ui_text_cell_w(scale) :
             rect->w;

    fb_ui_text_clipped(fb,
                       rect->x,
                       rect->y + (int)(row * line_h),
                       text_w,
                       text,
                       fg,
                       bg,
                       scale);
}

void fb_ui_inset_border(uint16_t *fb,
                        const fb_ui_rect_t *rect,
                        uint32_t outer,
                        uint32_t inner,
                        int inset)
{
    if (rect == NULL || rect->w <= 0 || rect->h <= 0) {
        return;
    }

    fb16_rect(fb, rect->x, rect->y, rect->w, rect->h, outer);
    if (inset > 0 && rect->w > (inset * 2) && rect->h > (inset * 2)) {
        fb16_rect(fb,
                  rect->x + inset,
                  rect->y + inset,
                  rect->w - (inset * 2),
                  rect->h - (inset * 2),
                  inner);
    }
}

void fb_ui_footer_band(uint16_t *fb, int height, uint32_t bg)
{
    if (height <= 0) {
        return;
    }
    if (height > FB16_HEIGHT) {
        height = FB16_HEIGHT;
    }

    fb16_fill_rect(fb, 0, FB16_HEIGHT - height, FB16_WIDTH, height, bg);
}

int fb_ui_key_badge(uint16_t *fb,
                    int x,
                    int y,
                    const char *key,
                    const char *label,
                    uint32_t key_bg,
                    uint32_t label_bg,
                    uint32_t fg,
                    uint32_t border,
                    int scale)
{
    int key_w;
    int key_h;
    int label_cell_w;

    if (key == NULL) {
        key = "";
    }
    if (label == NULL) {
        label = "";
    }
    scale = fb_ui_scale_or_default(scale);

    label_cell_w = fb_ui_text_cell_w(scale);
    key_w = 8 + (int)(strlen(key) * (size_t)label_cell_w);
    key_h = 6 + (FB16_BUILTIN_FONT_HEIGHT * scale);

    fb16_fill_rect(fb, x, y, key_w, key_h, key_bg);
    fb16_rect(fb, x, y, key_w, key_h, border);
    fb16_string_scaled(fb, x + 4, y + 3, key, fg, key_bg, scale);

    x += key_w + 6;
    fb16_string_scaled(fb, x, y + 3, label, fg, label_bg, scale);
    return x + ((int)strlen(label) * label_cell_w) + 12;
}

int fb_ui_menu_row_height(int scale)
{
    scale = fb_ui_scale_or_default(scale);
    return 6 + (FB16_BUILTIN_FONT_HEIGHT * scale);
}

void fb_ui_highlight_rect(uint16_t *fb,
                          const fb_ui_rect_t *rect,
                          const fb_ui_menu_style_t *style,
                          uint8_t active)
{
    const fb_ui_menu_style_t *s = fb_ui_style_or_default(style);

    if (rect == NULL || rect->w <= 0 || rect->h <= 0) {
        return;
    }

    fb16_fill_rect(fb, rect->x, rect->y, rect->w, rect->h,
                   active ? s->active_bg : s->bg);
    if (active) {
        fb16_rect(fb, rect->x, rect->y, rect->w, rect->h, s->accent);
    }
}

void fb_ui_checkbox(uint16_t *fb,
                    int x,
                    int y,
                    int w,
                    const char *label,
                    uint8_t checked,
                    uint8_t active,
                    const fb_ui_menu_style_t *style,
                    int scale)
{
    const fb_ui_menu_style_t *s = fb_ui_style_or_default(style);
    const int row_h = fb_ui_menu_row_height(scale);
    const int box = fb_ui_control_size(row_h);
    const int box_x = x + 4;
    const int box_y = y + ((row_h - box) / 2);
    const int text_x = box_x + box + 8;
    fb_ui_rect_t row = { x, y, w, row_h };
    uint32_t fg;

    if (w <= 0) {
        return;
    }
    scale = fb_ui_scale_or_default(scale);
    fg = active ? s->active_fg : s->fg;

    fb_ui_highlight_rect(fb, &row, s, active);
    fb16_fill_rect(fb, box_x + 1, box_y + 1, box - 2, box - 2,
                   active ? s->active_bg : s->bg);
    fb16_rect(fb, box_x, box_y, box, box, active ? s->accent : s->border);
    if (checked) {
        fb16_line(fb, box_x + 2, box_y + (box / 2),
                  box_x + (box / 3), box_y + box - 3, s->accent);
        fb16_line(fb, box_x + (box / 3), box_y + box - 3,
                  box_x + box - 3, box_y + 2, s->accent);
    }
    fb_ui_text_clipped(fb, text_x, y + 3, w - (text_x - x) - 4, label, fg,
                       active ? s->active_bg : s->bg, scale);
}

void fb_ui_radio_button(uint16_t *fb,
                        int x,
                        int y,
                        int w,
                        const char *label,
                        uint8_t selected,
                        uint8_t active,
                        const fb_ui_menu_style_t *style,
                        int scale)
{
    const fb_ui_menu_style_t *s = fb_ui_style_or_default(style);
    const int row_h = fb_ui_menu_row_height(scale);
    const int box = fb_ui_control_size(row_h);
    const int r = box / 2;
    const int cx = x + 4 + r;
    const int cy = y + (row_h / 2);
    const int text_x = x + 4 + box + 8;
    fb_ui_rect_t row = { x, y, w, row_h };
    uint32_t fg;

    if (w <= 0) {
        return;
    }
    scale = fb_ui_scale_or_default(scale);
    fg = active ? s->active_fg : s->fg;

    fb_ui_highlight_rect(fb, &row, s, active);
    fb16_circle(fb, cx, cy, r, active ? s->accent : s->border);
    if (selected && r > 3) {
        fb16_fill_circle(fb, cx, cy, r - 3, s->accent);
    }
    fb_ui_text_clipped(fb, text_x, y + 3, w - (text_x - x) - 4, label, fg,
                       active ? s->active_bg : s->bg, scale);
}

void fb_ui_listbox_entry(uint16_t *fb,
                         const fb_ui_rect_t *rect,
                         const char *label,
                         uint8_t selected,
                         uint8_t active,
                         const fb_ui_menu_style_t *style,
                         int scale)
{
    const fb_ui_menu_style_t *s = fb_ui_style_or_default(style);
    const uint32_t bg = active ? s->active_bg : s->bg;
    uint32_t fg;
    int text_x;

    if (rect == NULL || rect->w <= 0 || rect->h <= 0) {
        return;
    }
    scale = fb_ui_scale_or_default(scale);
    fg = selected ? s->selected_fg : s->fg;
    if (active) {
        fg = s->active_fg;
    }

    fb_ui_highlight_rect(fb, rect, s, active);
    text_x = rect->x + 8;
    if (selected) {
        fb16_fill_rect(fb, rect->x + 2, rect->y + 2, 4, rect->h - 4, s->accent);
        text_x += 6;
    }
    fb_ui_text_clipped(fb, text_x,
                       rect->y + ((rect->h - (FB16_BUILTIN_FONT_HEIGHT * scale)) / 2),
                       rect->w - (text_x - rect->x) - 4,
                       label,
                       fg,
                       bg,
                       scale);
}

void fb_ui_listbox(uint16_t *fb,
                   const fb_ui_rect_t *rect,
                   const char * const *items,
                   uint32_t item_count,
                   uint32_t top_index,
                   uint32_t selected_index,
                   uint32_t active_index,
                   const fb_ui_menu_style_t *style,
                   int scale)
{
    const fb_ui_menu_style_t *s = fb_ui_style_or_default(style);
    const int row_h = fb_ui_menu_row_height(scale);
    const int inner_x = rect != NULL ? rect->x + 1 : 0;
    const int inner_y = rect != NULL ? rect->y + 1 : 0;
    const int inner_w = rect != NULL ? rect->w - 2 : 0;
    const int inner_h = rect != NULL ? rect->h - 2 : 0;
    uint32_t visible_rows;
    uint32_t row;

    if (rect == NULL || rect->w <= 2 || rect->h <= 2 || row_h <= 0) {
        return;
    }

    fb16_fill_rect(fb, rect->x, rect->y, rect->w, rect->h, s->bg);
    fb16_rect(fb, rect->x, rect->y, rect->w, rect->h,
              active_index != FB_UI_INDEX_NONE ? s->accent : s->border);

    visible_rows = (uint32_t)(inner_h / row_h);
    for (row = 0U; row < visible_rows; ++row) {
        const uint32_t item_index = top_index + row;
        const uint8_t item_valid = (uint8_t)(item_index < item_count);
        const char *label = "";
        fb_ui_rect_t entry = {
            inner_x,
            inner_y + (int)(row * (uint32_t)row_h),
            inner_w,
            row_h
        };

        if (item_valid && items != NULL && items[item_index] != NULL) {
            label = items[item_index];
        }
        fb_ui_listbox_entry(fb,
                            &entry,
                            label,
                            (uint8_t)(item_valid && item_index == selected_index),
                            (uint8_t)(item_valid && item_index == active_index),
                            s,
                            scale);
    }
}
