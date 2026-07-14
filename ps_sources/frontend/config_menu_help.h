/******************************************************************************
 * config_menu_help.h -- help-panel text for the config menu.
 *
 * All the human-readable help text lives in config_menu_help.c. This header
 * is only the tiny lookup interface the menu uses to fetch the right block
 * of lines for the tab/item the user is currently on.
 *
 * To EDIT the help text, open config_menu_help.c -- everything is grouped
 * tab-by-tab there with instructions at the top. You should not need to
 * touch this header or config_menu.c to change wording or add a per-item
 * override.
 ******************************************************************************/

#ifndef CONFIG_MENU_HELP_H_INCLUDED
#define CONFIG_MENU_HELP_H_INCLUDED

#include <stdint.h>

/* A block of help lines: an array of strings and how many there are. */
typedef struct {
    const char * const *lines;
    uint32_t count;
} config_menu_help_block_t;

/* Return the help block to show for (tab, item).
 *
 * Resolution order:
 *   1. If the tab defines a per-item override for `item`, return it.
 *   2. Otherwise return the tab's default block.
 *
 * `tab` is a config_tab_t value; `item` is the focused item index within
 * the tab (menu->item_focus). Unknown tabs return an empty block
 * ({ NULL, 0 }). The returned line pointers are static and remain valid
 * for the lifetime of the program.
 */
config_menu_help_block_t config_menu_help_resolve(uint32_t tab, uint32_t item);

#endif /* CONFIG_MENU_HELP_H_INCLUDED */
