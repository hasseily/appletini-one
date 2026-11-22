#!/usr/bin/env python3
"""Source-contract checks for exclusive SD-card FTP sharing."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def test_ftp_protocol_and_subnet_gate() -> None:
    source = read("ps_sources/frontend/ftp_sd_service.c")

    require("FTP_CONTROL_PORT         21U" in source and
            "FTP_PASSIVE_PORT         50000U" in source,
            "FTP must use port 21 and passive data ports from 50000")
    require("peer_on_local_subnet(peer) == 0U" in source and
            "memcmp(peer, s->control_peer, sizeof(peer)) != 0" in source,
            "control and data connections must enforce the local subnet and same peer")
    require("strcmp(line, \"PASV\")" in source and
            "strcmp(line, \"EPSV\")" in source and
            "strcmp(line, \"PORT\")" in source and
            "strcmp(line, \"EPRT\")" in source,
            "the server must accept passive and active mode")
    require("v[0] != s->control_peer[0]" in source and
            "v[3] != s->control_peer[3]" in source,
            "active mode must connect only to the control client")
    require("FTP_SESSION_COUNT        3U" in source and
            "static void ftp_listener_poll" in source and
            "static uint8_t data_socket_alloc" in source and
            "static void socket_release" in source and
            "FTP_LINGER_MS" in source,
            "FTP must serve three clients on the four shared sockets")
    require("%s-Waiting for the data socket." in source,
            "a passive reply that waits for a socket must keep the client alive")
    require("const size_t room = sizeof(s->control_out) - s->control_out_len;"
            in source,
            "a new reply must go behind a reply that waits")
    for command in ("USER", "PASS", "LIST", "NLST", "RETR", "STOR",
                    "DELE", "MKD", "RMD", "RNFR", "RNTO"):
        require(f'strcmp(line, "{command}")' in source,
                f"FTP read/write command {command} must be implemented")
    require("Anonymous read/write access granted" in source,
            "FTP login must expose anonymous read/write access")
    require("segment[0] == '.' && segment[1] == '.'" in source and
            "if (len > 1U)" in source and
            "*p == ':'" in source,
            "FTP paths must normalize parent traversal and reject drive prefixes")

    prepare_start = source.index("static int ftp_prepare_transfer")
    prepare_end = source.index("static void command_upper", prepare_start)
    prepare = source[prepare_start:prepare_end]
    accept_start = source.index("static uint8_t ftp_data_connect_poll")
    accept_end = source.index("static int ftp_format_list_line", accept_start)
    accept = source[accept_start:accept_end]
    peer_check = accept.index("memcmp(peer, s->control_peer")
    store_open = accept.index("ftp_open_store_file(s)", peer_check)
    require("FA_CREATE_ALWAYS" not in prepare and
            "pending_stor_path" in prepare and
            peer_check < store_open,
            "STOR must create its file only after the data peer is verified")
    reject = accept[peer_check:store_open]
    require("socket_listen(s->data_socket, s->data_port)" in reject and
            "ftp_abort_transfer(s);" not in reject.split(
                "if (socket_listen", 1)[0],
            "a wrong data peer must be dropped and the passive transfer kept")


def test_directory_listing_finishes_with_tcp_eof() -> None:
    source = read("ps_sources/frontend/ftp_sd_service.c")

    start = source.index("static void ftp_finish_transfer")
    end = source.index("static int normalize_virtual_path", start)
    finish = source[start:end]
    require("socket_release(s->data_socket);" in finish and
            "socket_close(" not in finish and
            "uthernet2_w5100_socket_command(socket, W5100_CR_DISCON)" in source,
            "completed data transfers must send TCP FIN instead of dropping the socket")
    require('strcmp(virtual_path, "/") != 0' in source,
            "CWD / must accept the FTP volume root without relying on FatFs f_stat")

    poll_start = source.index("static void ftp_data_poll")
    poll_end = source.index("int ftp_sd_service_start", poll_start)
    poll = source[poll_start:poll_end]
    require("status != W5100_SR_ESTABLISHED && "
            "status != W5100_SR_CLOSE_WAIT" in poll and
            "ftp_send_transfer_poll(s);" in poll and
            "else if (status == W5100_SR_CLOSE_WAIT)" not in poll,
            "RETR and LIST must keep sending after the client half-closes")


def test_direct_transfer_path_with_tagged_completion() -> None:
    source = read("ps_sources/frontend/ftp_sd_service.c")
    control = read("ps_sources/frontend/uthernet2_control.c")
    control_h = read("ps_sources/frontend/uthernet2_control.h")
    regs = read("ps_sources/frontend/card_control_regs.h")
    top = read("hdl/apple/apple_top.sv")
    hdl_sources = read("hdl/hdl_sources.txt")

    require("FTP_DATA_BUFFER_LEN      1024U" in source and
            "FTP_STORE_BUFFER_LEN     32768U" in source and
            "store_buffer[FTP_STORE_BUFFER_LEN] __attribute__((aligned(32)))"
            in source and
            "return socket <= 1U ? 1460U : 1024U;" in source and
            "chunk > socket_tx_size(s->data_socket)" in source,
            "FTP must move file data in 32KB SD blocks and send no more "
            "than one TCP segment for each SEND (4KB sends gave 100 kB/s)")
    require("W5100_SOCKET_MEM_4_2_1_1 0x06U" in control_h and
            "0x4000U, 0x5000U, 0x5800U, 0x5C00U" in control and
            "0x6000U, 0x7000U, 0x7800U, 0x7C00U" in control and
            "0x0FFFU, 0x07FFU, 0x03FFU, 0x03FFU" in control,
            "the shared driver must keep the proven 4+2+1+1KB socket map")
    require("static int w5100_read16" not in source and
            "static int socket_command" not in source and
            "static int socket_ring_read" not in source and
            "uthernet2_w5100_read16_stable" in source and
            "uthernet2_w5100_socket_ring_read" in source and
            "uthernet2_w5100_socket_command" in source,
            "FTP must use the shared W5100 word, ring, and command helpers")
    require("for (uint16_t i = 0U; i < len; ++i)" in control and
            "CARD_CTRL_ETH_FIFO_" not in control and
            "CARD_CTRL_ETH_FIFO_" not in regs and
            "apple/uthernet2_host_fifo.sv" not in hdl_sources,
            "FTP must use direct byte W5100S access with no FIFO")
    require("CARD_CTRL_ETH_STATUS_SEQ_SHIFT" in regs and
            "logic [7:0]  eth_host_seq_q" in top and
            "sequence != start_sequence" in control,
            "Each direct W5100S command must wait for its own tagged completion")
    require("data_buffer[FTP_DATA_BUFFER_LEN] __attribute__((aligned(32)))" in source and
            "read_verify_buffer" not in source and
            "ftp_read_verified" not in source and
            "f_read(&s->file" in source and
            "FTP_TX_VERIFY_ATTEMPTS" not in source,
            "FTP must read each block once into a cache-line-aligned DMA buffer")
    require("transfer_eof" not in source and
            source.count("socket_send_start(") == 3 and
            "static int socket_send_buffer" in source and
            "static void ftp_send_data_buffer" in source and
            source.count("ftp_reset_transfer_state(s);") == 2,
            "FTP must share send/reset helpers and remove the dead EOF flag")


def test_exclusive_ethernet_and_sd_ownership() -> None:
    main = read("ps_sources/frontend/main.c")
    smartport = read("ps_sources/frontend/smartport_service.c")
    menu = read("ps_sources/frontend/config_menu.c")

    start = main.index("static int control_set_ethernet_ftp_sd_remote")
    end = main.index("static void control_set_applicard_resource_max", start)
    body = main[start:end]
    require("disk2_service_flush_dirty_now()" in body and
            "smartport_service_suspend_sd();" in body and
            "CARD_CTRL_SLOT_BIT(CARD_CTRL_SLOT_ETHERNET)" in body and
            "CARD_CTRL_SLOT_BIT(CARD_CTRL_SLOT_DISK2)" in body and
            "card_control_write_slot_mask(g_card_slot_enable_mask & ~suppressed_slots);" in body and
            "ftp_sd_service_start(detail, detail_len)" in body,
            "FTP start must flush Disk II, suspend SmartPort, hide Ethernet and Disk II, and then listen")
    require(body.index("smartport_service_suspend_sd();") <
            body.index("ftp_sd_service_start(detail, detail_len)"),
            "all local SD file handles must close before FTP mounts the volume")
    require("ftp_sd_service_stop();" in body and
            "control_restore_ftp_slots();" in body and
            "control_restore_ftp_sd_media();" in body,
            "FTP stop must release TCP and restore local media and card slots")
    require(body.index("control_restore_ftp_sd_media();", body.index("ftp_sd_service_stop();")) <
            body.index("control_restore_ftp_slots();", body.index("ftp_sd_service_stop();")),
            "FTP stop must reload Disk II before Apple-side slots return")
    require("if (g_devices[i].is_ram == 0U)" in smartport and
            "int smartport_service_resume_sd(void)" in smartport,
            "SmartPort SD suspension must preserve the volatile RAM disk")

    poll_start = menu.index("void config_menu_poll_onee_mode")
    poll_end = menu.index("void config_menu_set_active", poll_start)
    poll = menu[poll_start:poll_end]
    update_pos = poll.index("menu->onee_persisted_enabled =")
    ftp_gate_pos = poll.index("menu->ethernet_ftp_sd_remote_active != 0U")
    save_pos = poll.index("config_menu_save_settings_to_path(")
    require(update_pos < ftp_gate_pos < save_pos and
            "ack_onee_mode_persist_update" in poll[save_pos:],
            "ONE//e persistence must update RAM but defer SD save and ack "
            "while FTP owns the volume")


def test_ethernet_tab_modal_contract() -> None:
    menu = read("ps_sources/frontend/config_menu.c")
    tabs = read("ps_sources/frontend/config_menu_device_tabs.c")
    help_text = read("ps_sources/frontend/config_menu_help.c")
    internal = read("ps_sources/frontend/config_menu_internal.h")
    main = read("ps_sources/frontend/main.c")
    vitis = read("scripts/create_vitis_workspace.py")

    require("#define CONFIG_ETHERNET_ITEM_DNS 7U" in internal and
            "#define CONFIG_ETHERNET_ITEM_FTP_SD 12U" in internal and
            "#define CONFIG_ETHERNET_ITEM_COUNT 13U" in internal,
            "FTP sharing must be the final Ethernet-tab row")
    require('"Test link"' in tabs and '"SD Card FTP Sharing"' in tabs and
            tabs.index('"Test link"') < tabs.index('"SD Card FTP Sharing"'),
            "FTP sharing must render below the existing Ethernet actions")
    require("y + (12 * row_h)" in tabs,
            "DNS uses the former spacer; FTP must stay within the existing tab height")
    require("config_menu_start_ethernet_ftp_sd_remote(menu);" in menu and
            "WAIT FOR DHCP TO FINISH" in menu and
            "config_menu_stop_ethernet_ftp_sd_remote(menu);" in menu,
            "the menu must start, guard, and stop the FTP modal")
    require("config_menu_draw_ethernet_ftp_sd_modal" in menu and
            '"SD Card FTP Sharing"' in menu and
            "menu->ethernet_ftp_sd_remote_active != 0U" in menu,
            "active FTP sharing must replace the tab page with a modal panel")
    require("menu->usb_owned != 0U &&" in menu and
            "config_menu_onee_fixed_bindings_active(menu) == 0U" in menu and
            '"CARD ACCESS ONLY FROM BOOT MENU"' in menu,
            "active ONE//e must bypass the USB-owned boot-menu access gate")
    require("FTP owns Ethernet and the SD card until Enter" in help_text and
            "FTP sends file data without encryption" in help_text,
            "the Ethernet help must explain exclusive ownership and cleartext FTP")
    require("config_menu_ethernet_ftp_sd_remote_active(&config_menu) != 0U" in main and
            main.count("ftp_sd_service_poll();") >= 6,
            "the main loop must give the modal frequent FTP service points")
    require('"../../../ps_sources/frontend/ftp_sd_service.c"' in vitis,
            "the Vitis frontend build must include the FTP service")


def test_uart_ftp_control() -> None:
    menu = read("ps_sources/frontend/config_menu.c")
    menu_h = read("ps_sources/frontend/config_menu.h")
    uart = read("ps_sources/frontend/uart_control.c")

    require("void config_menu_start_ethernet_ftp_sd_remote" in menu and
            "void config_menu_start_ethernet_ftp_sd_remote" in menu_h,
            "UART control needs a public config-menu FTP start path")
    require('str_ieq(argv[0], "ftp")' in uart and
            '"  ftp [status|on|off]\\r\\n"' in uart and
            "config_menu_start_ethernet_ftp_sd_remote(g_config_menu);" in uart and
            "config_menu_stop_ethernet_ftp_sd_remote(g_config_menu);" in uart and
            "config_menu_ethernet_ftp_sd_remote_active(" in uart,
            "UART must provide FTP status, start, and stop commands")


def main() -> None:
    tests = [
        test_ftp_protocol_and_subnet_gate,
        test_directory_listing_finishes_with_tcp_eof,
        test_direct_transfer_path_with_tagged_completion,
        test_exclusive_ethernet_and_sd_ownership,
        test_ethernet_tab_modal_contract,
        test_uart_ftp_control,
    ]
    for test in tests:
        test()
        print(f"PASS {test.__name__}")
    print(f"FTP SD service tests: {len(tests)} passed")


if __name__ == "__main__":
    main()
