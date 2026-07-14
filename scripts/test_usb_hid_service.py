#!/usr/bin/env python3
"""Source-level regression tests for USB1 CherryUSB HID integration."""

from __future__ import annotations

import re
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
FRONTEND = REPO_ROOT / "ps_sources" / "frontend"
USB_HID_C = FRONTEND / "usb_hid_service.c"
USB_HID_H = FRONTEND / "usb_hid_service.h"
USB_CONFIG_H = FRONTEND / "usb_config.h"
CHERRY_PLATFORM_H = FRONTEND / "cherryusb_platform.h"
CHERRY_OSAL_C = FRONTEND / "cherryusb_baremetal_osal.c"
CHERRY_ZYNQ_C = FRONTEND / "cherryusb_zynq_hc.c"
CHERRY_HUB_C = FRONTEND / "cherryusb_usbh_hub_poll.c"
FRONTEND_MAIN_C = FRONTEND / "main.c"
UART_CONTROL_C = FRONTEND / "uart_control.c"
CLASS_STORAGE_C = FRONTEND / "xusbps_class_storage.c"
CLASS_STORAGE_H = FRONTEND / "xusbps_class_storage.h"
VITIS_SCRIPT = REPO_ROOT / "scripts" / "create_vitis_workspace.py"


class TestFailure(Exception):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise TestFailure(message)


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def macro_int(text: str, name: str) -> int:
    match = re.search(rf"^\s*#define\s+{re.escape(name)}\s+([0-9A-Fa-fx]+)\s*$",
                      text,
                      re.MULTILINE)
    require(match is not None, f"{name} must be defined")
    return int(match.group(1), 0)


def function_body(text: str, name: str) -> str:
    marker = f"{name}("
    search_start = 0
    while True:
        start = text.find(marker, search_start)
        require(start >= 0, f"{name} must exist")
        brace = text.find("{", start)
        semicolon = text.find(";", start)
        require(brace >= 0, f"{name} must have a body")
        if semicolon < 0 or brace < semicolon:
            break
        search_start = semicolon + 1
    depth = 0
    for idx in range(brace, len(text)):
        if text[idx] == "{":
            depth += 1
        elif text[idx] == "}":
            depth -= 1
            if depth == 0:
                return text[brace: idx + 1]
    raise TestFailure(f"{name} body is unterminated")


def test_slot_setting_does_not_control_physical_usb_mouse() -> None:
    header = read(USB_HID_H)
    frontend_main = read(FRONTEND_MAIN_C)
    source = read(USB_HID_C)

    require("usb_hid_service_set_enabled" not in header + frontend_main,
            "virtual mousecard slot setting must not power-gate the physical USB mouse")
    require("g_enabled" not in source,
            "USB HID service must not have a slot-derived enable gate")
    require("cherryusb_host_poll(CHERRYUSB_USB1_BUSID)" in source,
            "poll loop must run the CherryUSB host independent of slot settings")


def test_cherryusb_hid_backend_replaces_custom_enumerator() -> None:
    source = read(USB_HID_C)

    for token in [
        '#include "usbh_core.h"',
        '#include "usbh_hid.h"',
        '#include "cherryusb_platform.h"',
        "usbh_initialize(CHERRYUSB_USB1_BUSID, USB1_BASE, cherry_event_handler)",
        "usbh_deinitialize(CHERRYUSB_USB1_BUSID)",
        "int usb_hid_service_start(void)",
        "void usb_hid_service_stop(void)",
        "void usbh_hid_run(struct usbh_hid *hid_class)",
        "void usbh_hid_stop(struct usbh_hid *hid_class)",
        "usbh_int_urb_fill",
        "usbh_submit_urb",
        "mouse_publish_state",
        "MOUSE_REG_STATUS",
        "[usb1] CherryUSB HID service init (stopped)",
        "[usb1] CherryUSB host start",
        "[usb1] CherryUSB host stop",
    ]:
        require(token in source, f"CherryUSB HID backend must contain {token}")

    for token in [
        "static usb_hid_slot_t g_hid_slots[USB_HID_SLOT_COUNT]",
        "hid_class->user_data = slot",
        "hid_process_boot_mouse_report",
        "hid_process_boot_keyboard_report",
        "hid_process_report_protocol_report",
        "hid_parse_report_descriptor(slot)",
        "hid_report_info_has_relative_mouse(slot)",
        "if (slot->mouse_capable != 0U) {\n        mouse_mark_connected(slot);",
        "uint8_t apple_buttons;",
        "mouse_card_active_count",
        "mouse_card_aggregate_buttons",
        "mouse_slot_contributes(slot)",
        "slot->apple_buttons = (uint8_t)(buttons & MOUSE_BUTTON_APPLE_MASK);",
    ]:
        require(token in source, f"USB1 HID service must support multi-HID frontend routing: {token}")

def test_usb_keyboard_and_keypad_emit_bindable_sources() -> None:
    header = read(USB_HID_H)
    source = read(USB_HID_C)

    for token in [
        "typedef uint16_t usb_hid_menu_source_t;",
        "USB_HID_MENU_SOURCE_KEY_BASE",
        "usb_hid_menu_source_from_keyboard_usage(uint8_t usage)",
        "usb_hid_menu_source_is_keyboard(usb_hid_menu_source_t source)",
        "const char *usb_hid_menu_source_text(usb_hid_menu_source_t source)",
    ]:
        require(token in header, f"USB HID header must expose keyboard source API: {token}")

    for token in [
        "usb_hid_menu_source_t prev_sources[HID_SOURCE_TRACK_COUNT]",
        "keyboard_menu_push_source(next_sources[i]);",
        "usb_hid_menu_source_from_keyboard_usage(key)",
        "HID_KBD_USAGE_KPD1",
        "HID_KBD_USAGE_KPD0",
        "HID_KBD_USAGE_F13",
        "HID_KBD_USAGE_F24",
    ]:
        require(token in source, f"USB HID service must capture keyboard/keypad sources: {token}")

def test_cherryusb_config_supports_hubs_and_zynq_ehci() -> None:
    config = read(USB_CONFIG_H)

    require(macro_int(config, "CONFIG_USBHOST_MAX_BUS") == 1,
            "frontend should expose one CherryUSB host bus for USB1")
    require(macro_int(config, "CONFIG_USBHOST_MAX_RHPORTS") == 1,
            "USB1 root hub should have one root port")
    require(macro_int(config, "CONFIG_USBHOST_MAX_EXTHUBS") >= 2,
            "CherryUSB must allow nested external hubs")
    require(macro_int(config, "CONFIG_USBHOST_MAX_EHPORTS") >= 4,
            "external hub port table must handle common 4-port hubs")
    require(macro_int(config, "CONFIG_USBHOST_MAX_HID_CLASS") >= 8,
            "composite mouse receivers may expose several HID interfaces")
    require(macro_int(config, "CONFIG_USB_HID_MAX_REPORT_ITEMS") >= 48,
            "generic HID report-protocol devices need enough parsed input items")
    require(macro_int(config, "CONFIG_USB_EHCI_HCCR_OFFSET") == 0x100,
            "Zynq USBPS EHCI capability registers are at base + 0x100")
    require("CONFIG_USB_DCACHE_ENABLE" in config,
            "CherryUSB EHCI transfers must use dcache hooks on Zynq")
    require("CONFIG_USB_EHCI_DESC_DCACHE_ENABLE" in config,
            "EHCI descriptor pools must be cache managed")
    require("CONFIG_USB_PRINTF(...) cherryusb_printf(__VA_ARGS__)" in config,
            "CherryUSB logs must route to the firmware UART")


def test_baremetal_osal_pumps_polled_irq_during_waits() -> None:
    osal = read(CHERRY_OSAL_C)

    for token in [
        "usb_osal_sem_take",
        "usb_osal_mq_recv",
        "usb_osal_timer_create",
        "cherryusb_baremetal_osal_poll",
        "cherryusb_baremetal_poll_irq",
        "CONFIG_USB_ALIGN_SIZE",
    ]:
        require(token in osal, f"bare-metal OSAL must provide {token}")

    sem_take = function_body(osal, "usb_osal_sem_take")
    require("cherryusb_baremetal_poll_irq();" in sem_take,
            "blocking CherryUSB transfers must pump the polled EHCI IRQ")
    require("USB_OSAL_WAITING_FOREVER" in sem_take,
            "OSAL waits must preserve CherryUSB's forever timeout semantics")
    require("const size_t padded = USB_ALIGN_UP(size, CONFIG_USB_ALIGN_SIZE);" in osal and
            '#include "usb_util.h"' in osal and
            "malloc(padded + align - 1U + sizeof(void *))" in osal,
            "CherryUSB heap buffers must include trailing cache-line padding for EHCI DMA")


def test_zynq_glue_uses_usb1_host_mode_and_cache_hooks() -> None:
    glue = read(CHERRY_ZYNQ_C)

    for token in [
        "usb_phy_early_init();",
        "XUsbPs_ResetHw(base);",
        "XUSBPS_MODE_CM_HOST_MASK",
        "XUSBPS_PORTSCR_PP_MASK",
        "XUSBPS_PORTSCR_PHCD_MASK",
        "portsc &= ~XUSBPS_PORTSCR_PP_MASK;",
        "portsc |= XUSBPS_PORTSCR_PHCD_MASK;",
        "USBH_IRQHandler(CHERRYUSB_USB1_BUSID)",
        "usbh_get_port_speed",
        "usb_dcache_clean",
        "usb_dcache_invalidate",
        "cherryusb_usb1_portsc",
    ]:
        require(token in glue, f"Zynq CherryUSB glue must include {token}")


def test_hub_source_is_pollable_not_threaded() -> None:
    hub = read(CHERRY_HUB_C)
    initialize = function_body(hub, "usbh_hub_initialize")
    deinitialize = function_body(hub, "usbh_hub_deinitialize")

    require("cherryusb_host_poll" in hub,
            "copied CherryUSB hub source must expose a frontend poll hook")
    require("usb_hc_init(bus)" in initialize,
            "polling hub init must start the host controller synchronously")
    require("already connected; synthesizing connect change" in hub,
            "polling hub path must enumerate devices already present at firmware start")
    require("hub->child[0].connected == false" in hub and
            "(portsc & 0x00000001U) != 0U" in hub,
            "host poll must actively synthesize root-port connect events")
    require("root_probe_attempts++" in hub and
            "last_get_status_ret" in hub and
            "last_enumerate_ret" in hub and
            "cherryusb_host_debug_note_control" in hub,
            "host poll must expose root event debug counters and return codes")
    require("hub->int_buffer[0] = 0U" in hub,
            "hub event processing should clear stale interrupt bits after copying them")
    require("usb_osal_thread_create" not in initialize,
            "polling hub init must not create an OS thread")
    require("usb_osal_mq_recv(bus->hub_mq, &hub_value, 0U)" in hub,
            "frontend poll hook must drain hub events without blocking")
    require("usb_hc_deinit(bus)" in deinitialize,
            "polling hub deinit must stop the host controller synchronously")


def test_vitis_registers_cherryusb_sources_and_linker_section() -> None:
    script = read(VITIS_SCRIPT)

    for token in [
        "ensure_userconfig_include_dirs",
        "patch_usbh_class_info_section",
        "__usbh_class_info_start__",
        "KEEP(*(.usbh_class_info))",
        "../../../third_party/CherryUSB/common",
        "../../../third_party/CherryUSB/core/usbh_core.c",
        "../../../third_party/CherryUSB/class/hid/usbh_hid.c",
        "../../../third_party/CherryUSB/port/ehci/usb_hc_ehci.c",
        "../../../ps_sources/frontend/cherryusb_baremetal_osal.c",
        "../../../ps_sources/frontend/cherryusb_usbh_hub_poll.c",
        "../../../ps_sources/frontend/cherryusb_zynq_hc.c",
    ]:
        require(token in script, f"Vitis generator must register {token}")


def test_usb_hid_uart_diagnostics() -> None:
    header = read(USB_HID_H)
    source = read(USB_HID_C)
    uart_control = read(UART_CONTROL_C)
    platform = read(CHERRY_PLATFORM_H)

    require("usb_hid_service_dump_status(uint32_t uart_base)" in header,
            "USB HID service must expose a compact live status dump")
    require("int usb_hid_service_start(void);" in header and
            "void usb_hid_service_stop(void);" in header,
            "USB1 host must be runtime-controllable for USB0 isolation")
    require('str_ieq(argv[0], "usb1")' in uart_control and
            "usb_hid_service_dump_status(control->control_uart_base)" in uart_control,
            "control UART must expose :usb1 status for live CherryUSB debugging")
    require("usb1 [status|start|stop]" in uart_control and
            "usb_hid_service_start()" in uart_control and
            "usb_hid_service_stop()" in uart_control,
            "control UART must expose USB1 start/stop isolation commands")
    for token in [
        "report_pending",
        "PORTSC=0x",
        "mouse_regs: status=0x",
        "HID_REPORT_BUFFER_BYTES USB_ALIGN_UP",
        "active_hids",
        "mousecard=",
        "submit_errs",
        "xfer_errs",
        "hid: aggregate_mice=",
        "aggregate_buttons=0x",
        "intin: ep=0x",
        "cherryusb_host_poll(CHERRYUSB_USB1_BUSID)",
    ]:
        require(token in source, f"status dump must include {token}")
    require("cherryusb_printf" in platform and "cherryusb_host_debug_snapshot" in platform,
            "CherryUSB printf/debug hooks must be part of the frontend platform contract")
    core = read(REPO_ROOT / "third_party" / "CherryUSB" / "core" / "usbh_core.c")
    require('cherryusb_host_debug_note_enum_stage(hport, 1U)' in core and
            "cherryusb_host_debug_note_control(hport, setup, ret)" in core,
            "CherryUSB core must report enumeration stage and EP0 control results")
    require("cherryusb_host_debug_note_class_connect(hport, i, class_driver->driver_name, ret, class_started)" in core and
            "if (class_started > 0U)" in core,
            "CherryUSB core must report class-connect results and keep partially usable composites alive")
    ehci = read(REPO_ROOT / "third_party" / "CherryUSB" / "port" / "ehci" / "usb_hc_ehci.c")
    require("ehci_zlp_buf" in ehci and
            "ehci_qtd_fill(qtd_status, (uintptr_t)ehci_zlp_buf[bus->hcd.hcd_id], 0, token)" in ehci,
            "EHCI zero-length status qTDs must use a valid DMA buffer on Zynq")
    require("usb_dcache_flush((uintptr_t)setup, USB_ALIGN_UP(sizeof(*setup), CONFIG_USB_ALIGN_SIZE))" in ehci,
            "EHCI control transfers must clean the setup packet before controller DMA")
    require("ehci_dcache_prepare_urb_buffer" in ehci and
            "ehci_dcache_finish_urb_buffer" in ehci and
            "USB_EP_DIR_IS_IN(urb->ep->bEndpointAddress)" in ehci and
            "usb_dcache_invalidate((uintptr_t)urb->transfer_buffer" in ehci,
            "EHCI must invalidate device-to-host transfer buffers before callbacks")
    require("cherryusb_host_debug_note_ehci_qtd" in ehci and
            "QTD_TOKEN_NBYTES_MASK" in ehci,
            "EHCI scanner must expose the qTD token that failed")
    require("(token & QTD_TOKEN_PID_MASK) != QTD_TOKEN_PID_SETUP" in ehci,
            "EHCI actual_length must not include the setup qTD bytes")
    require("cherryusb_host_debug_note_ehci_zlp_babble_ignored" not in ehci,
            "EHCI BABBLE status must remain visible instead of being ignored")


def test_usb0_storage_priority_over_usb1_hid_poll() -> None:
    frontend_main = read(FRONTEND_MAIN_C)
    storage_h = read(FRONTEND / "usb_storage_service.h")
    storage_c = read(FRONTEND / "usb_storage_service.c")
    class_h = read(FRONTEND / "xusbps_class_storage.h")
    class_c = read(FRONTEND / "xusbps_class_storage.c")
    hid_status = function_body(read(USB_HID_C), "usb_hid_service_dump_status")

    loop_start = frontend_main.find("while (1) {")
    require(loop_start >= 0, "frontend main loop must be present")
    main_loop = frontend_main[loop_start:]
    modal_start = main_loop.find("if (config_menu_usb0_sd_remote_active(&config_menu) != 0U) {")
    require(modal_start >= 0, "main loop must have the modal USB0 SD remote-mount path")
    modal_end = main_loop.find("continue;", modal_start)
    require(modal_end > modal_start, "modal USB0 SD remote-mount path must continue early")
    modal_loop = main_loop[modal_start:modal_end]
    require("usb_storage_service_poll();\n"
            "            if (usb_storage_service_consume_host_eject_request() != 0U) {\n"
            "                config_menu_usb0_sd_remote_host_ejected(&config_menu);\n"
            "                usb0_modal_redraw_pending = 1U;\n"
            "            }" in modal_loop and
            "if (config_menu_usb0_sd_remote_active(&config_menu) != 0U) {\n"
            "                usb_hid_service_poll();\n"
            "                usb1_boot_settle_poll(&config_menu);\n"
            "            }" in modal_loop,
            "modal SD remote mount must service the USB bridge, close on host eject, then handle USB input and boot-prompt settle")
    require("static uint8_t ui_config_menu_has_close_consumer(const config_menu_t *menu)" in frontend_main and
            "config_menu_usb0_sd_remote_active(menu) != 0U ||\n"
            "            menu->usb_binding_capture != CONFIG_MENU_USB_BIND_CAPTURE_NONE ||\n"
            "            menu->browser_active != 0U ||\n"
            "            menu->profile_carousel_active != 0U ||\n"
            "            menu->profile_name_editor_active != 0U" in frontend_main and
            "static void ui_close_config_menu_child(ui_state_t *s, config_menu_t *menu)" in frontend_main,
            "frontend close routing must recognize child menu contexts")
    require("case BOOT_MENU_EVENT_CLOSE:\n"
            "                        config_menu_apply_runtime(&config_menu);\n"
            "                        ui_set_boot_menu_visible(&ui, &config_menu, 0U);\n"
            "                        usb0_modal_redraw_pending = 1U;\n"
            "                        break;" in modal_loop and
            "static void ui_close_config_menu_child(ui_state_t *s, config_menu_t *menu)" in frontend_main,
            "ROM close events must hide the parent menu while child ESC stays local")
    require("uint8_t usb0_modal_redraw_pending = 0U;" in frontend_main and
            "usb0_modal_redraw_pending != 0U &&\n"
            "                (config_menu_usb0_sd_remote_active(&config_menu) == 0U ||\n"
            "                 usb_storage_service_needs_attention() == 0)" in modal_loop and
            "compositor_request_full_refresh();" in modal_loop,
            "modal SD remote mount must redraw only on state/input changes and wait for quiet USB0")
    for skipped in [
        "ui_poll_sensors",
        "smartport_service_poll",
        "disk2_service_poll",
        "screenshot_service_poll",
    ]:
        require(skipped not in modal_loop,
                f"modal SD remote mount must skip heavy foreground service: {skipped}")

    normal_start = main_loop.find("ui_handle_apple_reset(&ui, &config_menu);", modal_end)
    require(normal_start > modal_end, "normal frontend loop must resume after the modal path")
    normal_loop = main_loop[normal_start:]
    storage_pos = normal_loop.find("usb0_priority_checkpoint();")
    gate_pos = normal_loop.find("usb_storage_service_needs_attention() == 0")
    hid_pos = normal_loop.find("usb_hid_service_poll();")
    require(0 <= storage_pos < gate_pos < hid_pos,
            "USB0 storage must be serviced and checked before USB1 mouse polling")
    require("if (usb_sdd_service_active() ||\n"
            "            usb_storage_service_needs_attention() == 0) {\n"
            "            usb_hid_service_poll();" in normal_loop,
            "USB1 polling must run while SDD owns USB0 and skip while USB0 storage has pending work")
    require(main_loop.count("usb0_priority_checkpoint();") >= 8,
            "main loop should checkpoint USB0 storage around other frontend services")
    require("(void)usb_hid_service_init();" in frontend_main and
            "int hid_rc = usb_hid_service_start();" in frontend_main and
            "usb1 start: ok" in frontend_main,
            "USB1 HID service should start at boot while foreground polling remains USB0-gated")
    require("case USB_HID_MENU_ACTION_CLOSE:\n"
            "        if (config_menu_is_active(menu)) {\n"
            "            if (ui_config_menu_has_close_consumer(menu) != 0U) {\n"
            "                ui_close_config_menu_child(s, menu);\n"
            "                return;\n"
            "            }" in frontend_main,
            "USB close/ESC actions must exit active child UI before closing the config menu")
    settle_poll = function_body(frontend_main, "usb1_boot_settle_poll")
    require("#define USB1_BOOT_SETTLE_QUIET_US 750000U" in frontend_main and
            "#define USB1_BOOT_HOLD_TIMEOUT_TICKS 0xFFFFFFFFU" in frontend_main and
            "static void usb1_boot_settle_begin(void)" in frontend_main and
            "boot_menu_service_set_timeout_ticks(USB1_BOOT_HOLD_TIMEOUT_TICKS);" in frontend_main and
            "usb_hid_service_activity_count()" in frontend_main and
            "config_menu_apply_boot_runtime(menu);" in settle_poll,
            "USB1 enumeration must settle during the boot prompt and restore the configured timeout afterward")
    require(frontend_main.find("usb1_boot_settle_begin();") <
            frontend_main.find("card_control_mark_cpu0_ready();"),
            "USB1 prompt settling must begin before releasing the Apple CPU")
    require("static uart_control_t g_uart0_control;" in frontend_main and
            "uart_control_init(&g_uart0_control, UART0_BASE, UART0_BASE)" in frontend_main and
            "uart_control_poll(&g_uart0_control, &g_uart_control_ops)" in frontend_main and
            "Control UART: UART1 + UART0" in frontend_main,
            "UART0 debug output port must also accept commands for live USB0 diagnostics")
    require("int usb_storage_service_needs_attention(void);" in storage_h and
            "uint8_t usb_storage_service_consume_host_eject_request(void);" in storage_h and
            "XUsbPs_StorageHasPending()" in storage_c and
            "USB_STORAGE_HOST_EJECT_GRACE_TICKS" in storage_c and
            "HostEjectPendingDetach" in storage_c,
            "USB0 storage service must expose deferred queue/write-pipe pressure and delay host-eject detach")
    require("int XUsbPs_StorageHasPending(void);" in class_h and
            "int XUsbPs_StorageHostEjectRequested(void);" in class_h and
            "void XUsbPs_StorageClearHostEjectRequested(void);" in class_h and
            "writePipeQueued != 0U" in class_c and
            "phase != USB_EP_STATE_COMMAND" in class_c,
            "storage class must report pending mass-storage work and host-eject requests")
    require("cherryusb_host_poll" not in hid_status,
            "USB1 HID status command must be passive so diagnostics cannot starve USB0")


def test_usb0_storage_enumeration_diagnostics() -> None:
    storage_h = read(FRONTEND / "usb_storage_service.h")
    storage_c = read(FRONTEND / "usb_storage_service.c")
    uart_control = read(UART_CONTROL_C)
    ch9 = read(FRONTEND / "xusbps_ch9.c")
    ch9_storage = read(FRONTEND / "xusbps_ch9_storage.c")
    class_h = read(FRONTEND / "xusbps_class_storage.h")
    class_storage = read(FRONTEND / "xusbps_class_storage.c")
    phy_init = read(FRONTEND / "usb_phy_init.c")

    for token in [
        "u64 starts;",
        "u64 irq_count;",
        "u64 reset_irqs;",
        "u64 ui_irqs;",
        "u64 ue_irqs;",
        "u64 pc_irqs;",
        "u64 soft_disconnects;",
        "u64 soft_connects;",
        "u64 ep0_setup_events;",
        "u64 ep0_data_rx_events;",
        "u64 ep0_rx_failures;",
        "u64 ep0_other_events;",
        "u64 ep0_stalls;",
        "u64 setup_errors;",
        "u64 ep0_send_failures;",
        "u64 class_requests;",
        "u64 set_address_requests;",
        "u64 set_configuration_requests;",
        "u64 get_descriptor_device;",
        "u64 get_descriptor_config;",
        "u64 get_descriptor_string;",
        "u64 get_descriptor_qualifier;",
        "u64 get_descriptor_other;",
        "u64 ep1_rx_events;",
        "u64 ep1_tx_events;",
        "u64 ep1_rx_failures;",
        "u64 ep1_other_events;",
        "u64 ep0_prime_count;",
        "u64 ep0_prime_failures;",
        "u64 ep1_prime_count;",
        "u64 ep1_prime_failures;",
        "u32 current_config;",
        "u32 need_ep0_prime;",
        "u32 last_irq_mask;",
        "u32 last_setup_bm_request_type;",
        "u32 last_setup_b_request;",
        "u32 last_setup_w_value;",
        "u32 last_setup_w_index;",
        "u32 last_setup_w_length;",
        "u32 last_desc_type;",
        "u32 last_desc_request_len;",
        "u32 last_desc_reply_len;",
        "u32 last_ep0_event;",
        "u32 last_ep1_event;",
        "u32 last_prime_ep;",
        "u32 last_prime_dir;",
        "u32 last_prime_status;",
        "u32 last_send_ep;",
        "u32 last_send_len;",
        "u32 last_send_status;",
        "u32 last_usb_cmd;",
        "u32 last_usb_sts;",
        "u32 last_usb_intr;",
        "u32 last_deviceaddr;",
        "u32 last_eplistaddr;",
        "u32 last_usb_mode;",
        "u32 last_portsc;",
        "u32 last_otgsc;",
        "u32 last_epstat;",
        "u32 last_epprime;",
        "u32 last_eprdy;",
        "u32 last_epcomplete;",
        "u32 last_epcr0;",
        "u32 last_epcr1;",
        "u32 ep1in_dqh;",
        "u32 ep1in_dqh_cfg;",
        "u32 ep1in_dqh_cptr;",
        "u32 ep1in_dqh_next;",
        "u32 ep1in_dqh_token;",
        "u32 ep1in_dtds;",
        "u32 ep1in_head;",
        "u32 ep1in_head_next;",
        "u32 ep1in_head_token;",
        "u32 ep1in_head_buf;",
        "u32 ep1in_tail;",
        "u32 ep1in_tail_next;",
        "u32 ep1in_tail_token;",
        "u32 ep1in_tail_buf;",
        "u32 ep1in_requested_bytes;",
        "u32 ep1in_bytes_txed;",
        "u32 ep1in_buffer_ptr;",
        "u32 last_slcr_usb0_clk_ctrl;",
        "u32 last_slcr_usb1_clk_ctrl;",
        "u32 last_slcr_usb_rst_ctrl;",
        "u32 last_usb0_mio[12];",
        "u32 last_ulpi_view;",
    ]:
        require(token in storage_h, f"USB0 diagnostic stats must expose {token}")

    require("#define USB0_GIC_PRIORITY XINTERRUPT_DEFAULT_PRIORITY" in storage_c and
            "XScuGic_SetPriorityTriggerType(Gic, GicIntrId,\n"
            "\t\t\t\t       USB0_GIC_PRIORITY,\n"
            "\t\t\t\t       XINTR_IS_LEVEL_TRIGGERED);" in storage_c and
            "USB_STORAGE_IRQ_MASK" in storage_c and
            "XUSBPS_IXR_UE_MASK" in storage_c and
            "XUSBPS_IXR_PC_MASK" in storage_c,
            "USB0 must use explicit SD-safe, error-aware interrupt setup")
    connect_body = function_body(storage_c, "usb_storage_service_connect")
    disconnect_body = function_body(storage_c, "usb_storage_service_disconnect")
    poll_body = function_body(storage_c, "usb_storage_service_poll")
    configure_body = function_body(storage_c, "UsbConfigureStorageDevice")
    reconfigure_body = function_body(storage_c, "UsbReconfigureAttachedDevice")
    enum_reset_body = function_body(storage_c, "UsbResetEnumerationState")
    ep0_prime_body = function_body(storage_c, "UsbRequestEp0Prime")
    request_reconfigure_body = function_body(storage_c, "UsbRequestDeviceReconfigure")
    port_change_body = function_body(storage_c, "UsbHandlePortChangeIrq")
    intr_body = function_body(storage_c, "UsbIntrHandler")
    set_configuration_body = function_body(storage_c, "usb_storage_debug_note_set_configuration")
    class_reset_body = function_body(class_storage, "ResetStorageState")
    require("XUsbPs_Stop" not in disconnect_body and
            "XUsbPs_IntrDisable" not in disconnect_body and
            "UsbSoftDisconnect(&UsbInstance);" in disconnect_body and
            "usleep(250000);" in disconnect_body,
            "local USB0 detach must drop the pullup without tearing down controller/IRQ setup")
    require("UsbConfigureStorageDevice(&UsbInstance, 250000U);" in connect_body and
            "NeedEndpointReset = 0;" in connect_body and
            "XUsbPs_IntrEnable(&UsbInstance, USB_STORAGE_IRQ_MASK);" in connect_body and
            "UsbSoftConnect(&UsbInstance);" in connect_body,
            "USB0 reconnect must run a full detached device configure before soft-connecting")
    require("static XUsbPs_Local UsbLocalData;" in storage_c and
            "UsbInstancePtr->UserDataPtr = &UsbLocalData;" in configure_body and
            "XUsbPs_ConfigureDevice(UsbInstancePtr, &DeviceConfig)" in configure_body and
            "UsbRegisterDeviceHandlers(UsbInstancePtr)" in configure_body and
            "XUsbPs_IntrDisable(UsbInstancePtr, XUSBPS_IXR_ALL);" in configure_body and
            "UsbSoftDisconnect(UsbInstancePtr);" in configure_body,
            "USB0 full configure must reset controller DMA state and restore Chapter 9/endpoint handlers")
    require("if (NeedFullReconfigure != 0)" in poll_body and
            "UsbReconfigureAttachedDevice()" in poll_body and
            "UsbSoftDisconnect(&UsbInstance);" in reconfigure_body and
            "UsbConfigureStorageDevice(&UsbInstance, 0U);" in reconfigure_body and
            "UsbSoftConnect(&UsbInstance);" in reconfigure_body and
            "UsbGuardedEpPrime(&UsbInstance, 0," in reconfigure_body,
            "USB0 configured cable reset/replug must reattach through the full configure path")
    require("NeedFullReconfigure = 1;" in request_reconfigure_body and
            "XUSBPS_PORTSCR_CCS_MASK" in port_change_body and
            "UsbSawPhysicalDisconnect = 1U;" in port_change_body and
            "UsbRequestDeviceReconfigure();" in port_change_body and
            "UsbHandlePortChangeIrq();" in intr_body and
            "UsbRequestDeviceReconfigure();" not in intr_body and
            "UsbConfiguredOnce = (Config != 0U) ? 1U : 0U;" in set_configuration_body,
            "USB0 must schedule full reconfiguration only after a real port disconnect/reconnect")
    require("UsbLocalPtr->CurrentConfig = 0U;" in enum_reset_body and
            "UsbInstancePtr->CurrentAltSetting = XUSBPS_DEFAULT_ALT_SETTING;" in enum_reset_body and
            "XUSBPS_DEVICEADDR_OFFSET, 0U" in enum_reset_body and
            "XUSBPS_EPFLUSH_OFFSET, XUSBPS_EP_ALL_MASK" in enum_reset_body and
            enum_reset_body.count("XUsbPs_ReconfigureEp(UsbInstancePtr,") >= 4 and
            "XUsbPs_EpDisable(UsbInstancePtr, 1," in enum_reset_body,
            "USB0 enumeration reset must clear address/config and rebuild EP0/EP1 rings")
    require("NeedEndpointReset = 1;" in ep0_prime_body and
            "NeedEp0Prime = 1;" in ep0_prime_body and
            "if (NeedEndpointReset != 0)" in poll_body and
            "UsbResetEnumerationState(&UsbInstance);" in poll_body,
            "USB0 bus reset IRQs must reset endpoint state before the deferred EP0 prime")
    require("XUsbPs_EpGetSetupData" in storage_c,
            "USB0 EP0 setup path must use the driver callback")
    require("UsbStorageSnapshotRegs" in storage_c and
            "UsbStorageSnapshotEp1In" in storage_c and
            "UsbSnapshotPhyAndUlpi" in storage_c and
            "XUSBPS_PORTSCR1_OFFSET" in storage_c and
            "XUSBPS_OTGCSR_OFFSET" in storage_c and
            "XUSBPS_EPCR1_OFFSET" in storage_c and
            "XUsbPs_dQHInvalidateCache(EpIn->dQH)" in storage_c and
            "XUsbPs_ReaddQH(EpIn->dQH, XUSBPS_dQHdTDTOKEN)" in storage_c and
            "XUsbPs_ReaddTD(EpIn->dTDTail, XUSBPS_dTDTOKEN)" in storage_c,
            "USB0 status must snapshot core, OTG, port, endpoint, PHY, and ULPI registers")
    require("UsbUlpiRead" not in storage_c and
            "USB_ULPI_TIMEOUT_LOOPS" not in storage_c,
            "USB0 UART status must not run active ULPI transactions that can stall the foreground loop")
    require("usb_storage_debug_note_setup(&SetupData)" in storage_c and
            "StorageStats.ep0_rx_failures++" in storage_c and
            "StorageStats.ep1_rx_failures++" in storage_c,
            "USB0 endpoint callbacks must count setup and RX failures")
    for token in [
        "u64 cbw_packets;",
        "u64 scsi_cmds;",
        "u64 ep1_in_sends;",
        "u64 ep1_in_failures;",
        "u64 data_in_sends;",
        "u64 data_in_failures;",
        "u64 csw_sends;",
        "u32 phase;",
        "u32 rx_bytes_left;",
        "u32 last_cbw_len;",
        "u32 last_cbw_signature;",
        "u32 last_cbw_tag;",
        "u32 last_cbw_transfer_len;",
        "u32 last_cbw_flags;",
        "u32 last_cbw_lun;",
        "u32 last_cbw_cb_len;",
        "u32 last_scsi_opcode;",
        "u32 last_ep1_send_len;",
        "u32 last_ep1_send_status;",
        "u32 last_data_in_len;",
        "u32 last_data_in_status;",
        "u32 last_status_residue;",
        "u32 last_status_code;",
    ]:
        require(token in class_h, f"USB0 class diagnostics must expose {token}")
    for token in [
        "StorageNoteScsiCommand",
        "StorageEp1Send",
        "ClassStats.last_cbw_transfer_len",
        "ClassStats.last_status_residue",
        "int XUsbPs_StorageHasPending(void)",
        "writePipeQueued != 0U",
        "phase != USB_EP_STATE_COMMAND",
    ]:
        require(token in class_storage, f"USB0 class layer must record {token}")
    require("XUSBPS_EP_DIRECTION_OUT" in storage_c and
            "XUSBPS_EP_DIRECTION_IN" in storage_c and
            "case XUSBPS_EP_EVENT_DATA_TX:" in storage_c,
            "USB0 EP1 must register and handle bulk IN TX completion events")
    require("if (writePipeQueued == 0U)" in class_reset_body and
            "asyncWriteFailed = 0U;" in class_reset_body and
            "SENSE_CLEAR();" in class_reset_body,
            "USB0 MSC reset must clear stale clean-eject/write-failure state once writes are drained")
    require("case USB_RBC_TEST_UNIT_READY:\n\t\t\tcase USB_RBC_VERIFY:" in class_storage and
            "case USB_RBC_MEDIUM_REMOVAL:" in class_storage and
            "PREVENT/ALLOW MEDIUM REMOVAL" in class_storage,
            "USB0 PREVENT/ALLOW MEDIUM REMOVAL must not share TEST UNIT READY failure handling")
    require("hostEjectRequested = 1U;" in class_storage and
            "(start & 0x03U) == 0x02U" in class_storage and
            "XUsbPs_StorageClearHostEjectRequested();" in storage_c and
            "HostEjectRequestTick" in storage_c and
            "(Now - HostEjectRequestTick) < USB_STORAGE_HOST_EJECT_GRACE_TICKS" in storage_c and
            "usb_storage_service_consume_host_eject_request" in storage_c,
            "USB0 START STOP UNIT eject must latch a host-eject request, finish the CSW, then let the modal detach")

    for token in [
        "usb0: starts=",
        "ep0: setup=",
        "setup: bm=0x",
        "desc: dev=",
        "ep1: rx=%llu tx=",
        "regs: cmd=0x",
        "mode=0x",
        "port: ccs=",
        "otg: id=",
        "phy: usb0clk=",
        "mio28-31:",
        "mio32-39:",
        "ulpi: view=",
        "epregs: stat=0x",
        "ep1inq: dqh=0x",
        "ep1intd: hnext=0x",
        "tact=%lu",
        "tlen=%lu",
        "cbw: pkts=",
        "cbw2: flags=0x",
        "cmd2: getcap=",
        "attach: disc=",
        "need_ep0_prime=",
        "ep1in: sends=",
        "usb status | usb resetstats",
    ]:
        require(token in uart_control, f":usb status must print {token}")
    require("usb reconnect" not in uart_control and
            "usb phyreset" not in uart_control,
            "USB0 UART commands must not include active reconnect/reset controls")

    require("usb_storage_debug_note_ep_send_failure(0," in ch9 and
            "(u32)ReplyLen" in ch9 and
            "usb_storage_debug_note_descriptor(" in ch9 and
            "if (SetupData->wLength > XUSBPS_REQ_REPLY_LEN)" in ch9,
            "Chapter 9 must passively report descriptor/send failures")
    require("XUSBPS_CLASSREQ_GET_MAX_LUN" in class_storage and
            "usb_storage_debug_note_ch9_error(SetupData)" in class_storage,
            "mass-storage class requests must report EP0 status failures")
    require("usb_storage_debug_note_ep_prime(1, XUSBPS_EP_DIRECTION_OUT, Status)" in ch9_storage,
            "SET_CONFIGURATION must report whether EP1 OUT was primed")
    require("static int g_usb_phy_initialized" in phy_init and
            "if (g_usb_phy_initialized != 0)" in phy_init,
            "shared USB PHY clock/reset init must be one-shot")
    require("void usb_phy_get_status(usb_phy_status_t *status)" in phy_init and
            "SLCR_MIO_PIN_28" in phy_init,
            "shared USB PHY helper must expose passive snapshot APIs")


def test_usb0_scsi_identity_and_format_capacities() -> None:
    class_h = read(CLASS_STORAGE_H)
    class_storage = read(CLASS_STORAGE_C)
    send_data_body = function_body(class_storage, "SendDataAndStatus")

    require('{"Xilinx  "}' in class_storage and
            '{"PS USB VirtDisk"}' in class_storage,
            "SCSI INQUIRY must expose the configured vendor and product")
    require("0x00,\n\t0x01,\n\t0x1f," in class_storage,
            "SCSI INQUIRY response must use the expected format")
    require("u8  blockLengthMSB;" in class_h and
            "u16 blockLength;" in class_h,
            "READ FORMAT CAPACITIES layout must expose the block length fields")
    for token in [
        "CapList->descCode\t= 3;",
        "CapList->blockLength\t= htons(USB_STORAGE_BLOCK_SIZE);",
    ]:
        require(token in class_storage,
                "READ FORMAT CAPACITIES must publish the formatted-media descriptor")
    require("Status = StorageEp1Send(InstancePtr, Data, Length);" in send_data_body and
            "SendStatus(InstancePtr, CBW, CbwRequestedBytes(CBW), 1);" in send_data_body and
            "SendStatus(InstancePtr, CBW, Residue, 0);" in send_data_body,
            "BOT IN responses must use direct send-and-status control flow")


TESTS = [
    test_slot_setting_does_not_control_physical_usb_mouse,
    test_cherryusb_hid_backend_replaces_custom_enumerator,
    test_usb_keyboard_and_keypad_emit_bindable_sources,
    test_cherryusb_config_supports_hubs_and_zynq_ehci,
    test_baremetal_osal_pumps_polled_irq_during_waits,
    test_zynq_glue_uses_usb1_host_mode_and_cache_hooks,
    test_hub_source_is_pollable_not_threaded,
    test_vitis_registers_cherryusb_sources_and_linker_section,
    test_usb_hid_uart_diagnostics,
    test_usb0_storage_priority_over_usb1_hid_poll,
    test_usb0_storage_enumeration_diagnostics,
    test_usb0_scsi_identity_and_format_capacities,
]


def main() -> int:
    failures = []
    for test in TESTS:
        try:
            test()
        except TestFailure as exc:
            failures.append((test.__name__, str(exc)))
            print(f"FAIL {test.__name__}: {exc}")
        else:
            print(f"PASS {test.__name__}")
    if failures:
        print(f"{len(TESTS) - len(failures)} of {len(TESTS)} USB HID tests passed; "
              f"{len(failures)} failed")
        return 1
    print(f"{len(TESTS)} USB HID tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
