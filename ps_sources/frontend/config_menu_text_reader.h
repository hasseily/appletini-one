#ifndef CONFIG_MENU_TEXT_READER_H
#define CONFIG_MENU_TEXT_READER_H

#include "config_menu.h"
#include "config_menu_ui.h"

void config_menu_text_reader_open(config_menu_t *menu,
                                  const char *name,
                                  const char *path);
void config_menu_text_reader_close(void);
uint8_t config_menu_text_reader_active(void);
uint8_t config_menu_text_reader_handle_input(ui_input_t input);
void config_menu_text_reader_draw(uint16_t *fb, const cmui_rect_t *body);

#endif
