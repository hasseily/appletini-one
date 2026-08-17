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
            "FTP must use port 21 and one fixed passive data port")
    require("peer_on_local_subnet(peer) == 0U" in source and
            "memcmp(peer, g_ftp.control_peer, sizeof(peer)) != 0" in source,
            "control and data connections must enforce the local subnet and same peer")
    require("Active mode is disabled; use PASV or EPSV" in source and
            "strcmp(line, \"PASV\")" in source and
            "strcmp(line, \"EPSV\")" in source,
            "the server must be passive-only")
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


def test_directory_listing_finishes_with_tcp_eof() -> None:
    source = read("ps_sources/frontend/ftp_sd_service.c")

    start = source.index("static void ftp_finish_transfer")
    end = source.index("static int normalize_virtual_path", start)
    finish = source[start:end]
    require("socket_disconnect(FTP_DATA_SOCKET);" in finish and
            "socket_close(FTP_DATA_SOCKET);" not in finish and
            "socket_command(socket, W5100_CR_DISCON)" in source,
            "completed data transfers must send TCP FIN instead of dropping the socket")
    require('strcmp(virtual_path, "/") != 0' in source,
            "CWD / must accept the FTP volume root without relying on FatFs f_stat")


def test_proven_direct_transfer_path() -> None:
    source = read("ps_sources/frontend/ftp_sd_service.c")
    control = read("ps_sources/frontend/uthernet2_control.c")
    regs = read("ps_sources/frontend/card_control_regs.h")
    hdl_sources = read("hdl/hdl_sources.txt")

    require("FTP_DATA_BUFFER_LEN      1024U" in source and
            "FTP_SEND_CHUNK_LEN" not in source,
            "FTP must use the proven 1KB transfer buffer")
    require("W5100_SOCKET_MEM_4_2_1_1 0x06U" in source and
            "W5100_SOCKET_MEM_2_4_1_1" not in source and
            "W5100_TX_BASE_S1         0x5000U" in source and
            "W5100_RX_BASE_S1         0x7000U" in source and
            "W5100_MASK_S0            0x0FFFU" in source and
            "W5100_MASK_S1            0x07FFU" in source,
            "FTP must keep the proven 4+2+1+1KB socket map")
    require("for (uint16_t i = 0U; i < len; ++i)" in control and
            "CARD_CTRL_ETH_FIFO_" not in control and
            "CARD_CTRL_ETH_FIFO_" not in regs and
            "apple/uthernet2_host_fifo.sv" not in hdl_sources,
            "FTP must use direct byte W5100S access with no FIFO")


def test_exclusive_ethernet_and_sd_ownership() -> None:
    main = read("ps_sources/frontend/main.c")
    smartport = read("ps_sources/frontend/smartport_service.c")

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


def test_ethernet_tab_modal_contract() -> None:
    menu = read("ps_sources/frontend/config_menu.c")
    tabs = read("ps_sources/frontend/config_menu_device_tabs.c")
    help_text = read("ps_sources/frontend/config_menu_help.c")
    internal = read("ps_sources/frontend/config_menu_internal.h")
    main = read("ps_sources/frontend/main.c")
    vitis = read("scripts/create_vitis_workspace.py")

    require("#define CONFIG_ETHERNET_ITEM_FTP_SD 11U" in internal and
            "#define CONFIG_ETHERNET_ITEM_COUNT 12U" in internal,
            "FTP sharing must be the final Ethernet-tab row")
    require('"Test link"' in tabs and '"SD Card FTP Sharing"' in tabs and
            tabs.index('"Test link"') < tabs.index('"SD Card FTP Sharing"'),
            "FTP sharing must render below the existing Ethernet actions")
    require("y + (12 * row_h)" in tabs,
            "FTP sharing must leave one blank row after the regular Ethernet actions")
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
        test_proven_direct_transfer_path,
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
