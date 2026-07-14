#ifndef BOOT_UPDATER_SERIAL_BOOT_H
#define BOOT_UPDATER_SERIAL_BOOT_H

#include <stdint.h>

#include "updater_layout.h"

typedef enum {
    SERIAL_BOOT_ACTION_NONE = 0,
    SERIAL_BOOT_ACTION_CONTINUE,
    SERIAL_BOOT_ACTION_RUN_UPDATER,
    SERIAL_BOOT_ACTION_BOOT_FIRMWARE,
    SERIAL_BOOT_ACTION_REBOOT_GOLDEN
} serial_boot_action_t;

serial_boot_action_t serial_boot_menu(const flash_layout_t *layout,
                                      uint32_t auto_timeout_seconds);

#endif
