#ifndef BOOT_MENU_SERVICE_H
#define BOOT_MENU_SERVICE_H

#include <stdint.h>

#include "uart_control.h"

typedef enum {
    BOOT_MENU_EVENT_NONE = 0,
    BOOT_MENU_EVENT_OPEN,
    BOOT_MENU_EVENT_INPUT,
    BOOT_MENU_EVENT_CLOSE
} boot_menu_event_type_t;

typedef struct {
    boot_menu_event_type_t type;
    ui_input_t input;
    uint8_t raw_key;
} boot_menu_event_t;

typedef enum {
    BOOT_MENU_HANDOFF_SMARTPORT = 1,
    BOOT_MENU_HANDOFF_DISK2 = 2
} boot_menu_handoff_t;

typedef struct {
    uint32_t status_word;
    uint32_t timeout_ticks;
    uint32_t handoff_mode_word;
    uint32_t apple_status_byte;
} boot_menu_debug_snapshot_t;

void boot_menu_service_init(void);
void boot_menu_service_apply_video_rom_patch(void);
uint8_t boot_menu_service_is_apple_video_50hz(void);
void boot_menu_service_set_timeout_ticks(uint32_t ticks);
void boot_menu_service_set_handoff(boot_menu_handoff_t handoff);
void boot_menu_service_request_rom_close(void);
void boot_menu_service_debug_snapshot(boot_menu_debug_snapshot_t *snapshot);
boot_menu_event_t boot_menu_service_poll(void);
void boot_menu_service_refresh_machine_policy(void);

/* The boot ROM reports the host machine through the boot-menu command channel.
 * The service maps it to a CARD_MACHINE_MODE_* and programs the PL
 * interlock register. Raw ids: 0=none yet, 1=II/II+, 2=IIe,
 * 3=enhanced IIe, 4=IIgs, 5=IIc. */
uint8_t boot_menu_service_machine_id(void);
uint8_t boot_menu_service_machine_mode(void);
const char *boot_menu_service_machine_name(void);
/* mode >= 0: force that CARD_MACHINE_MODE_* (bench testing);
 * mode < 0: return to automatic (reported-id-driven) policy. */
void boot_menu_service_force_machine_mode(int mode);
uint8_t boot_menu_service_machine_forced(void);

#endif
