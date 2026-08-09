#include "config_menu_internal.h"

#include <stdio.h>

extern uint8_t boot_menu_service_aux_card_present(void);
extern uint8_t boot_menu_service_machine_mode(void);
#include "card_control_regs.h"   /* CARD_MACHINE_MODE_* */

void config_menu_draw_smartport(uint16_t *fb,
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

    if (menu->supersprite_enabled != 0U) {
        hgr_draw_check_item_dimmed(fb, x, y, w,
                                   (uint8_t)(menu->item_focus == 0U),
                                   menu->disk2_activity_visible,
                                   "Show drive activity overlay");
    } else {
        hgr_draw_check_item(fb, x, y, w,
                            (uint8_t)(menu->item_focus == 0U),
                            menu->disk2_activity_visible,
                            "Show drive activity overlay");
    }

    for (uint32_t i = 0U; i < SMARTPORT_DEVICE_COUNT; ++i) {
        if (menu->smartport_slots[i] != 0U &&
            menu->smartport_disk_paths[i][0] != '\0') {
            (void)snprintf(line,
                           sizeof(line),
                           "SP%u: %s",
                           (unsigned)i + 1U,
                           config_menu_basename(menu->smartport_disk_paths[i]));
        } else {
            (void)snprintf(line,
                           sizeof(line),
                           "SP%u: %s",
                           (unsigned)i + 1U,
                           "[EMPTY]");
        }
        if (menu->supersprite_enabled != 0U) {
            hgr_draw_item_dimmed(fb,
                                 x,
                                 y + ((int)i + 1) * row_h,
                                 w,
                                 (uint8_t)(menu->item_focus == (i + 1U)),
                                 line);
        } else {
            hgr_draw_item(fb,
                          x,
                          y + ((int)i + 1) * row_h,
                          w,
                          (uint8_t)(menu->item_focus == (i + 1U)),
                          line,
                          HGR_WHITE);
        }
    }
    if (menu->supersprite_enabled != 0U) {
        hgr_draw_check_item_dimmed(
            fb,
            x,
            y + ((int)SMARTPORT_DEVICE_COUNT + 1) * row_h,
            w,
            (uint8_t)(menu->item_focus == SMARTPORT_DEVICE_COUNT + 1U),
            menu->sp_ramdisk_enabled,
            "RAM32: 32MB volatile ram disk");
    } else {
        hgr_draw_check_item(
            fb,
            x,
            y + ((int)SMARTPORT_DEVICE_COUNT + 1) * row_h,
            w,
            (uint8_t)(menu->item_focus == SMARTPORT_DEVICE_COUNT + 1U),
            menu->sp_ramdisk_enabled,
            "RAM32: 32MB volatile ram disk");
    }

    hgr_draw_check_item(
        fb,
        x,
        y + ((int)SMARTPORT_DEVICE_COUNT + 3) * row_h,
        w,
        (uint8_t)(menu->item_focus == SMARTPORT_DEVICE_COUNT + 2U),
        menu->supersprite_enabled,
        "SuperSprite VDP + PSG (Slot 7, disables SmartPort)");
}

void config_menu_draw_usb(uint16_t *fb,
                          const config_menu_t *menu,
                          int x,
                          int y,
                          int w)
{
    const int row_h = CMUI_ROW_H + CMUI_ROW_GAP;

    if (menu == NULL) {
        return;
    }

    if (menu->sdd_stream_enabled != 0U) {
        hgr_draw_item_dimmed(fb, x, y, w,
                             (uint8_t)(menu->item_focus == 0U),
                             "SD Card Remote Mounting");
    } else {
        hgr_draw_item(fb, x, y, w,
                      (uint8_t)(menu->item_focus == 0U),
                      menu->usb0_sd_remote_active ?
                          "SD Card Remote Mounting [ACTIVE]" :
                          "SD Card Remote Mounting",
                      HGR_WHITE);
    }
    hgr_draw_check_item(fb, x, y + row_h, w,
                        (uint8_t)(menu->item_focus == 1U),
                        menu->sdd_stream_enabled,
                        "SuperDuperDisplay stream (USB0)");
    /* USB1 host (keyboards/mice): a full re-enumeration for devices that
     * were too slow to enumerate at power-on. */
    hgr_draw_item(fb, x, y + (2 * row_h), w,
                  (uint8_t)(menu->item_focus == 2U),
                  "Refresh USB1 devices (re-scan)",
                  HGR_WHITE);
}

void config_menu_draw_applicard(uint16_t *fb,
                                const config_menu_t *menu,
                                int x,
                                int y,
                                int w)
{
    const int row_h = CMUI_ROW_H + CMUI_ROW_GAP;

    if (menu == NULL) {
        return;
    }

    hgr_draw_check_item(fb, x, y, w, (uint8_t)(menu->item_focus == 0U),
                        menu->applicard_slot5_enabled, "Enable in Slot 5");
    hgr_draw_value_item(fb, x, y + row_h, w,
                        (uint8_t)(menu->item_focus == 1U),
                        "Processor card:",
                        menu->slot5_processor == CONFIG_SLOT5_PROCESSOR_AD8088 ?
                            "ALF AD8088 Plus (640K)" : "Z80 Appli-Card");
    hgr_draw_value_item(fb, x, y + (2 * row_h), w,
                        (uint8_t)(menu->item_focus == 2U),
                        "Resource usage:",
                        menu->applicard_resource_max != 0U ?
                            "Maximum" : "Standard");
}

static void hgr_draw_disk2_sound_volume_item(uint16_t *fb,
                                             int x,
                                             int y,
                                             int w,
                                             uint8_t focused,
                                             uint8_t volume)
{
    char value[8];

    if (volume > 10U) {
        volume = 10U;
    }

    (void)snprintf(value, sizeof(value), "%u", (unsigned)volume);
    cmui_slider(fb,
                x,
                y,
                w,
                focused,
                0U,
                "Sound Volume",
                "0",
                "10",
                volume,
                10U,
                5U,
                value);
}

static uint8_t config_menu_disk2_read_only(const config_menu_t *menu, uint8_t drive)
{
    if (menu == NULL ||
        drive >= 2U ||
        menu->disk2_disk_paths[drive][0] == '\0' ||
        menu->platform.get_disk2_image_read_only == NULL) {
        return 0U;
    }
    return (menu->platform.get_disk2_image_read_only(menu->platform.ctx, drive) != 0U) ?
        1U : 0U;
}

static void hgr_draw_disk2_image_item(uint16_t *fb,
                                      int x,
                                      int y,
                                      int w,
                                      uint8_t focused,
                                      const char *label,
                                      const char *name,
                                      uint8_t locked)
{
    const uint32_t bg = focused ? CMUI_COLOR_ROW_ACTIVE : CMUI_COLOR_ROW;
    const uint32_t fg = focused ? CMUI_COLOR_TEXT : CMUI_COLOR_MUTED;

    hgr_focus_band(fb, x, y, w, focused);
    cmui_text(fb, x + 18, y + 11, label, fg, bg, CMUI_BODY_SCALE);
    cmui_lock(fb, x + 138, y, locked, focused, bg);
    cmui_text_clipped(fb, x + 178, y + 11, w - 196, name,
                      focused ? CMUI_COLOR_ACCENT : CMUI_COLOR_TEXT, bg,
                      CMUI_BODY_SCALE);
}

void config_menu_draw_disk2(uint16_t *fb,
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
                        menu->disk2_slot6_enabled, "Enable in Slot 6");
    hgr_draw_check_item(fb, x, y + row_h, w, (uint8_t)(menu->item_focus == 1U),
                        menu->disk2_activity_visible,
                        "Show drive activity overlay");

    if (menu->disk2_disk_paths[0][0] != '\0') {
        hgr_draw_disk2_image_item(fb,
                                  x,
                                  y + (row_h * 2),
                                  w,
                                  (uint8_t)(menu->item_focus == 2U),
                                  "Disk 1:",
                                  config_menu_basename(menu->disk2_disk_paths[0]),
                                  config_menu_disk2_read_only(menu, 0U));
    } else {
        (void)snprintf(line, sizeof(line), "Disk 1: %s", "[EMPTY]");
        hgr_draw_item(fb,
                      x,
                      y + (row_h * 2),
                      w,
                      (uint8_t)(menu->item_focus == 2U),
                      line,
                      HGR_WHITE);
    }

    if (menu->disk2_disk_paths[1][0] != '\0') {
        hgr_draw_disk2_image_item(fb,
                                  x,
                                  y + (row_h * 3),
                                  w,
                                  (uint8_t)(menu->item_focus == 3U),
                                  "Disk 2:",
                                  config_menu_basename(menu->disk2_disk_paths[1]),
                                  config_menu_disk2_read_only(menu, 1U));
    } else {
        (void)snprintf(line, sizeof(line), "Disk 2: %s", "[EMPTY]");
        hgr_draw_item(fb,
                      x,
                      y + (row_h * 3),
                      w,
                      (uint8_t)(menu->item_focus == 3U),
                      line,
                      HGR_WHITE);
    }

    hgr_draw_disk2_sound_volume_item(fb,
                                     x,
                                     y + (row_h * 4),
                                     w,
                                     (uint8_t)(menu->item_focus == 4U),
                                     menu->disk2_sound_volume);
}

void config_menu_draw_mouse(uint16_t *fb,
                            const config_menu_t *menu,
                            int x,
                            int y,
                            int w)
{
    if (menu == NULL) {
        return;
    }

    hgr_draw_check_item(fb, x, y, w, (uint8_t)(menu->item_focus == 0U),
                        menu->mouse_slot2_enabled, "Enable in Slot 2");
    hgr_draw_mouse_sensitivity_item(fb,
                                    x,
                                    y + (CMUI_ROW_H + CMUI_ROW_GAP),
                                    w,
                                    (uint8_t)(menu->item_focus == 1U),
                                    menu->mouse_sensitivity);
}

void config_menu_draw_ethernet(uint16_t *fb,
                               const config_menu_t *menu,
                               int x,
                               int y,
                               int w)
{
    const int row_h = CMUI_ROW_H + CMUI_ROW_GAP;
    char mac[18];
    char ip[16];
    char subnet[16];
    char gateway[16];
    char label[40];

    if (menu == NULL) {
        return;
    }

    hgr_draw_check_item(fb, x, y, w,
                        (uint8_t)(menu->item_focus == CONFIG_ETHERNET_ITEM_SLOT),
                        menu->ethernet_slot1_enabled, "Enable in Slot 1");
    if (menu->ethernet_slot1_enabled == 0U) {
        return;
    }

    config_menu_format_mac(menu->ethernet_config.mac, mac, sizeof(mac));
    config_menu_format_ipv4(menu->ethernet_config.ip, ip, sizeof(ip));
    config_menu_format_ipv4(menu->ethernet_config.subnet, subnet, sizeof(subnet));
    config_menu_format_ipv4(menu->ethernet_config.gateway, gateway, sizeof(gateway));

    hgr_draw_check_item(fb, x, y + row_h, w,
                        (uint8_t)(menu->item_focus ==
                                  CONFIG_ETHERNET_ITEM_CONFIG_ENABLED),
                        menu->ethernet_config_enabled,
                        "Configure network at boot");
    hgr_draw_value_item(fb, x, y + (2 * row_h), w,
                        (uint8_t)(menu->item_focus == CONFIG_ETHERNET_ITEM_MODE),
                        "Address Mode",
                        config_menu_ethernet_address_mode_text(
                            menu->ethernet_address_mode));

    (void)snprintf(label,
                   sizeof(label),
                   "MAC [%u/6]",
                   (unsigned)config_menu_ethernet_selected_index(menu,
                                                                 UTHERNET2_MAC_LEN) +
                   1U);
    hgr_draw_value_item(fb, x, y + (3 * row_h), w,
                        (uint8_t)(menu->item_focus == CONFIG_ETHERNET_ITEM_MAC),
                        label,
                        mac);

    (void)snprintf(label,
                   sizeof(label),
                   "IP [%u/4]",
                   (unsigned)config_menu_ethernet_selected_index(menu,
                                                                 UTHERNET2_IPV4_LEN) +
                   1U);
    hgr_draw_value_item(fb, x, y + (4 * row_h), w,
                        (uint8_t)(menu->item_focus == CONFIG_ETHERNET_ITEM_IP),
                        label,
                        ip);

    (void)snprintf(label,
                   sizeof(label),
                   "Subnet [%u/4]",
                   (unsigned)config_menu_ethernet_selected_index(menu,
                                                                 UTHERNET2_IPV4_LEN) +
                   1U);
    hgr_draw_value_item(fb, x, y + (5 * row_h), w,
                        (uint8_t)(menu->item_focus ==
                                  CONFIG_ETHERNET_ITEM_SUBNET),
                        label,
                        subnet);

    (void)snprintf(label,
                   sizeof(label),
                   "Gateway [%u/4]",
                   (unsigned)config_menu_ethernet_selected_index(menu,
                                                                 UTHERNET2_IPV4_LEN) +
                   1U);
    hgr_draw_value_item(fb, x, y + (6 * row_h), w,
                        (uint8_t)(menu->item_focus ==
                                  CONFIG_ETHERNET_ITEM_GATEWAY),
                        label,
                        gateway);

    hgr_draw_item(fb, x, y + (7 * row_h), w,
                  (uint8_t)(menu->item_focus == CONFIG_ETHERNET_ITEM_READ),
                  "Read config from card",
                  HGR_WHITE);
    hgr_draw_item(fb, x, y + (8 * row_h), w,
                  (uint8_t)(menu->item_focus == CONFIG_ETHERNET_ITEM_WRITE),
                  "Write config to card",
                  HGR_WHITE);
    hgr_draw_item(fb, x, y + (9 * row_h), w,
                  (uint8_t)(menu->item_focus == CONFIG_ETHERNET_ITEM_DHCP),
                  "Get IP from network (DHCP)",
                  HGR_WHITE);
    hgr_draw_item(fb, x, y + (10 * row_h), w,
                  (uint8_t)(menu->item_focus == CONFIG_ETHERNET_ITEM_TEST),
                  "Test link",
                  HGR_WHITE);
}

void config_menu_draw_transwarp(uint16_t *fb,
                                const config_menu_t *menu,
                                int x,
                                int y,
                                int w)
{
    const int row_h = CMUI_ROW_H + CMUI_ROW_GAP;
    const int option_gap = 8;
    const int option_w = (w - option_gap) / 2;
    const int option_x2 = x + option_w + option_gap;
    const int option_w2 = w - option_w - option_gap;
    const int slot_gap = 8;
    const int slot_w =
        (w - ((CONFIG_TRANSWARP_SLOT_COUNT - 1U) * slot_gap)) /
        CONFIG_TRANSWARP_SLOT_COUNT;
    uint8_t machine_blocks;

    if (menu == NULL) {
        return;
    }

    /* Dim on a POSITIVE unsupported identification (IIgs); before the
     * boot ROM reports (UNKNOWN) the setting is a valid intent that
     * engages later. //e and II/II+ both accelerate. Also dimmed
     * outside the boot menu: sessions start and stop only from BOOT
     * mode (the toggle handler enforces it; speed stays live). */
    machine_blocks =
        (boot_menu_service_machine_mode() == CARD_MACHINE_MODE_IIGS) ? 1U : 0U;

    if (machine_blocks != 0U) {
        hgr_draw_check_item_dimmed(fb, x, y, w,
                                   (uint8_t)(menu->item_focus ==
                                             CONFIG_TRANSWARP_ITEM_ENABLE),
                                   0U, "Accelerate (not on a IIgs)");
    } else if (menu->usb_owned != 0U) {
        hgr_draw_check_item_dimmed(fb, x, y, w,
                                   (uint8_t)(menu->item_focus ==
                                             CONFIG_TRANSWARP_ITEM_ENABLE),
                                   menu->vtw_enabled,
                                   "Accelerate the Apple II (BOOT mode)");
    } else {
        hgr_draw_check_item(fb, x, y, w,
                            (uint8_t)(menu->item_focus ==
                                      CONFIG_TRANSWARP_ITEM_ENABLE),
                            menu->vtw_enabled,
                            "Accelerate the Apple II");
    }
    hgr_draw_value_item(fb, x, y + row_h, w,
                        (uint8_t)(menu->item_focus ==
                                  CONFIG_TRANSWARP_ITEM_SPEED),
                        "Speed:",
                        config_menu_vtw_speed_label(menu));
    hgr_draw_check_item(fb, x, y + (2 * row_h), option_w,
                        (uint8_t)(menu->item_focus ==
                                  CONFIG_TRANSWARP_ITEM_IGNORE_C074),
                        menu->vtw_ignore_c074,
                        "Ignore $C074 Speed Switch");
    hgr_draw_check_item(fb, option_x2, y + (2 * row_h), option_w2,
                        (uint8_t)(menu->item_focus ==
                                  CONFIG_TRANSWARP_ITEM_DISABLE_D2),
                        menu->vtw_disable_disk2_accel,
                        "Disable DiskII Acceleration");
    /* Per-region slowdown (TransWarp DIP block 2): each enabled region
     * drops the core to 1 MHz for a window after it is touched, so
     * timing-sensitive I/O keeps working at high core speeds. */
    hgr_draw_check_item(fb, x, y + (3 * row_h), option_w,
                        (uint8_t)(menu->item_focus ==
                                  CONFIG_TRANSWARP_ITEM_FLOATBUS),
                        (uint8_t)((menu->vtw_slowdown_mask & (1U << 7)) != 0U),
                        "Slow Floating bus ($C019,$C030-$C05F)");
    hgr_draw_check_item(fb, option_x2, y + (3 * row_h), option_w2,
                        (uint8_t)(menu->item_focus ==
                                  CONFIG_TRANSWARP_ITEM_PADDLE),
                        (uint8_t)((menu->vtw_slowdown_mask & (1U << 8)) != 0U),
                        "Slow Paddles/joystick ($C064-$C070)");
    hgr_draw_check_item(fb, x, y + (4 * row_h), w,
                        (uint8_t)(menu->item_focus ==
                                  CONFIG_TRANSWARP_ITEM_SLUG),
                        menu->vtw_slug_key_enabled,
                        "Enable 0.05 MHz slug debug key");

    cmui_caption(fb, x + 18, y + (5 * row_h) + 10, w - 36,
                 "Slow down physical slots:");
    for (uint8_t slot = 1U; slot <= CONFIG_TRANSWARP_SLOT_COUNT; ++slot) {
        const uint32_t item = CONFIG_TRANSWARP_ITEM_SLOT_FIRST + slot - 1U;
        const int slot_x = x + ((int)(slot - 1U) * (slot_w + slot_gap));
        char slot_label[4];

        (void)snprintf(slot_label, sizeof(slot_label), "%u", (unsigned)slot);
        hgr_draw_check_item(
            fb, slot_x, y + (6 * row_h), slot_w,
            (uint8_t)(menu->item_focus == item),
            (uint8_t)((menu->vtw_slowdown_mask & (1U << (slot - 1U))) != 0U),
            slot_label);
    }
    {
        char win_val[16];
        (void)snprintf(win_val, sizeof(win_val), "%u cyc",
                       (unsigned)menu->vtw_slowdown_cycles);
        hgr_draw_value_item(fb, x, y + (7 * row_h), w,
                            (uint8_t)(menu->item_focus ==
                                      CONFIG_TRANSWARP_ITEM_WINDOW),
                            "Slowdown window:", win_val);
    }
}

void config_menu_draw_ram(uint16_t *fb,
                          const config_menu_t *menu,
                          int x,
                          int y,
                          int w)
{
    const int row_h = CMUI_ROW_H + CMUI_ROW_GAP;
    const uint8_t focused =
        (uint8_t)(menu != NULL && menu->item_focus == 0U);

    if (menu == NULL) {
        return;
    }

    if (menu->usb_owned != 0U) {
        hgr_draw_check_item_dimmed(fb, x, y, w, focused,
                                   menu->ram_enabled,
                                   "Provide 64K aux + 8MB RamWorks");
    } else if (boot_menu_service_aux_card_present() != 0U) {
        hgr_draw_check_item_dimmed(fb, x, y, w, focused,
                                   0U, "Physical aux card detected (Appletini RAM off)");
    } else {
        hgr_draw_check_item(fb, x, y, w, focused,
                            menu->ram_enabled,
                            "Provide 64K aux + 8MB RamWorks");
    }
    if (menu->vtw_enabled != 0U) {
        /* Informational only (not a focusable item): the accelerated
         * machine always has 128K from shadow memory; with Appletini
         * RAM enabled (no physical aux card) the accelerator serves the
         * full 8MB RamWorks from PSRAM at speed. */
        hgr_draw_item_dimmed(fb, x, y + row_h, w, 0U,
                             menu->ram_enabled != 0U
                                 ? "TransWarp on: 128K + 8MB RamWorks, accelerated"
                                 : "TransWarp on: 128K built in");
    }
}
