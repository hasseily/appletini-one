#!/usr/bin/env python3
"""Source-level regression tests for the SuperDuperDisplay USB0 stream.

These tests run without Vivado or hardware:

    python scripts/test_sdd_stream.py
"""

from __future__ import annotations

import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
SDD_TAP_SV = REPO_ROOT / "hdl" / "apple" / "sdd_bus_tap.sv"
APPLE_TOP_SV = REPO_ROOT / "hdl" / "apple" / "apple_top.sv"
YARZ_TOP_SV = REPO_ROOT / "hdl" / "appletini_yarz_top.sv"
HDL_SOURCES = REPO_ROOT / "hdl" / "hdl_sources.txt"
PERSONALITY_C = REPO_ROOT / "ps_sources" / "frontend" / "usb0_personality.c"
SDD_SERVICE_C = REPO_ROOT / "ps_sources" / "frontend" / "usb_sdd_service.c"
SDD_SERVICE_H = REPO_ROOT / "ps_sources" / "frontend" / "usb_sdd_service.h"
FRONTEND_MAIN_C = REPO_ROOT / "ps_sources" / "frontend" / "main.c"
UART_CONTROL_C = REPO_ROOT / "ps_sources" / "frontend" / "uart_control.c"
CONFIG_MENU_INTERNAL_H = REPO_ROOT / "ps_sources" / "frontend" / "config_menu_internal.h"
WORKSPACE_PY = REPO_ROOT / "scripts" / "create_vitis_workspace.py"


class TestFailure(Exception):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise TestFailure(message)


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def test_tap_event_word_matches_sdd_parser() -> None:
    """SDD unpacks reg 0x1004 words as addr[15:0], misc[19:16]
    (bit16=rw, bit17=res), data[27:20], valid marker bit 28, and
    route flags in bits 29..31; the tap must pack exactly that."""
    tap = read(SDD_TAP_SV)
    require("assign event_word = {route_info, 1'b1," in tap,
            "tap event word must carry the route flags in [31:29] and "
            "the nonzero event marker in bit 28")
    require("[28]    always 1" in tap and
            "[29]    route_rom" in tap and
            "[30]    bank_nonzero" in tap and
            "[31]    route_cache" in tap,
            "tap event layout comments must document the high-nibble "
            "route/valid fields consumed by SDD")
    require("ab_read.m2b0, ab_read.m2sel, ab_read.res, ab_read.rw," in tap,
            "tap misc nibble must be {m2b0, m2sel, res, rw} so rw lands at "
            "bit 16 and res at bit 17 (what SDD decodes)")
    require("ab_read.addr}" in tap and "ab_read.data," in tap,
            "tap event word must carry addr in [15:0] and data in [27:20]")
    require("SDD_RECORD_KIND = 3'd3" in tap,
            "tap records must use kind 3 so gap markers (all-zero) stay "
            "distinguishable")
    require("enable && ab_read.data_en" in tap,
            "tap must capture every bus cycle (data_en), gated only by "
            "the PS enable")
    require("(~resetn || !enable)" in tap,
            "tap FIFO must reset while disabled so re-enables start clean")


def test_apple_top_second_egress_on_hp2() -> None:
    top = read(APPLE_TOP_SV)
    require("sdd_bus_tap sdd_bus_tap_i" in top,
            "apple_top must instantiate the SDD tap")
    require("apple_cycle_egress sdd_cycle_egress_i" in top,
            "apple_top must instantiate a dedicated egress for the SDD ring")
    require(".axi_hp0_write             (axi_sdd_write)" in top,
            "the SDD egress must own the HP2 write master (axi_sdd_write)")
    require("8'h50:" in top and "8'h55: sdd_cfg_reset_pulse" in top and
            "8'h5A:   as_client_rdata_q <= sdd_stat_full_stall_cycles;" in top,
            "SDD cfg/stat registers must live at card-control 0x50..0x5A")
    yarz = read(YARZ_TOP_SV)
    require(".axi_sdd_write(s_axi_hp2_write)" in yarz,
            "top must route HP2 write into apple_top")
    require("assign s_axi_hp2_write.awvalid = 1'b0;" not in yarz,
            "HP2 write must remain connected to the SDD egress")
    require("apple/sdd_bus_tap.sv" in read(HDL_SOURCES),
            "sdd_bus_tap.sv must be in hdl_sources.txt")


def test_vendor_identity_and_pipes() -> None:
    h = read(REPO_ROOT / "ps_sources" / "frontend" / "usb_sdd_vendor.h")
    c = read(REPO_ROOT / "ps_sources" / "frontend" / "xusbps_ch9_sddvendor.c")
    require("#define SDDV_VID                0x1209U" in h and
            "#define SDDV_PID                0xA271U" in h,
            "native identity: pid.codes VID 0x1209 with project PID")
    require("#define SDDV_EP_DATA            1U" in h,
            "single data channel on EP1 (the storage-proven layout)")
    require('"WINUSB' in c and "0x0201" in c and
            "DeviceInterfaceGUIDs" in c and
            "{F5A31C8E-7D3B-4E1C-9A64-52AA35C10B71}" in c,
            "MS OS 2.0 descriptors must auto-bind WinUSB and register the "
            "device interface GUID SuperDuperDisplay opens")
    require("cfg->NumEndpoints = 2;" in read(SDD_SERVICE_C) and
            "NumBufs       = 16" in read(SDD_SERVICE_C),
            "device config must mirror the storage EP1 layout")


def test_personality_dispatch() -> None:
    p = read(PERSONALITY_C)
    for fn in ("XUsbPs_Ch9SetupDevDescReply", "XUsbPs_Ch9SetupCfgDescReply",
               "XUsbPs_Ch9SetupStrDescReply", "XUsbPs_SetConfiguration",
               "XUsbPs_ClassReq"):
        require(f"{fn}(" in p, f"personality must own {fn}")
    require("XUsbPs_Ch9SetupDevDescReply_Storage" in p and
            "XUsbPs_Ch9SetupDevDescReply_SddVendor" in p,
            "personality must dispatch between storage and FT60x")
    ch9 = read(REPO_ROOT / "ps_sources" / "frontend" / "xusbps_ch9.c")
    require("usb0_personality_vendor_req(InstancePtr, SetupData," in ch9,
            "ch9 must offer vendor requests to the personality first")


def test_sdd_service_protocol() -> None:
    c = read(SDD_SERVICE_C)
    require("#define SDD_EVENTS_PER_MSG      4000U" in c,
            "event batches must fill 16 KB transfers (the v5 250-event size "
            "was an FT601 FIFO limit and throttles WinUSB ~16x)")
    require("SDD_ADDR_EVENTS         0x1004U" in c and
            "SDD_ADDR_STATUS         0x1000U" in c,
            "stream address 0x1004, status address 0x1000")
    require("SDD_ADDR_NSC_TIME_0     0x0014U" in c,
            "SDD's set-time messages land at 0x14/0x18")
    require("no_slot_clock_control_publish_rtc(&t);" in c,
            "host time writes must reach the no-slot-clock")
    require("SddOverflowLatched = 1U;" in c and
            "SddForwarding = 0U;" in c and
            "SddStatusPending = 1U;" in c,
            "gap markers must trigger the SDD overflow protocol "
            "(stop + status message; host re-enable resumes)")
    h = read(SDD_SERVICE_H)
    require("SDD_RING_BASE           0x3F040000U" in h and
            "SDD_PRODUCER_PTR_ADDR   0x3F030000U" in h,
            "SDD ring lives in the free NONCACHE window")


def test_usb_tab_sdd_and_sd_remote_mount() -> None:
    c = read(REPO_ROOT / "ps_sources" / "frontend" / "config_menu.c")
    tabs = read(REPO_ROOT / "ps_sources" / "frontend" /
                "config_menu_device_tabs.c")
    help_c = read(REPO_ROOT / "ps_sources" / "frontend" /
                  "config_menu_help.c")
    h = read(REPO_ROOT / "ps_sources" / "frontend" / "config_menu.h")
    fb = read(FRONTEND_MAIN_C)
    internal = read(CONFIG_MENU_INTERNAL_H)
    sdd = read(SDD_SERVICE_C)
    require("uint8_t sdd_stream_enabled;" in h and
            "set_sdd_stream_enabled" in h and
            "set_usb0_sd_remote_mount" in h and
            "uint8_t usb0_sd_remote_active;" in h,
            "config menu must carry USB0 SDD and modal remote-mount state + platform ops")
    require("CONFIG_TAB_USB" in internal,
            "config menu must define the USB tab")
    require('"USB"' in c,
            "config menu tab labels must expose USB")
    require("return 2U;                          /* SD remote mount + SDD stream */"
            in c,
            "USB tab must have SDD stream and SD remote-mount items")
    require('"usb.sdd.stream.enabled"' in c and
            '"usb.sdd.stream.enabled=%s\\n"' in c and
            '"sdd_stream_enable"' not in c,
            "SDD stream setting must persist in appletini_cfg.txt dot notation")
    require('"SuperDuperDisplay stream (USB0)"' in tabs and
            '"SD Card Remote Mounting"' in tabs and
            "menu->usb0_sd_remote_active ?" in tabs,
            "USB tab must draw the SDD checkbox and modal SD-card remote-mount action")
    require(tabs.find('"SD Card Remote Mounting"') <
            tabs.find('"SuperDuperDisplay stream (USB0)"') and
            "case CONFIG_TAB_USB:\n"
            "        if (menu->item_focus == 0U) {\n"
            "            config_menu_start_usb0_sd_remote(menu);\n"
            "        } else if (menu->item_focus == 1U) {\n"
            "            config_menu_set_sdd_stream(menu, menu->sdd_stream_enabled ? 0U : 1U);"
            in c and
            "OVERRIDE(0, usb_sd_remote)" in help_c and
            "OVERRIDE(1, usb_sdd)" in help_c,
            "USB tab must put SD remote mounting on row 0 and SDD on row 1")
    require("SDD STREAM ON - USB0 STREAMS TO SDD" in c and
            "SDD STREAM OFF - USB0 DETACHED" in c,
            "USB tab SDD toggle must switch the USB0 personality and leave USB0 detached when off")
    require("config_menu_start_usb0_sd_remote" in c and
            "config_menu_stop_usb0_sd_remote" in c and
            "config_menu_usb0_sd_remote_host_ejected" in c and
            "SD CARD REMOTE MOUNTING - ENTER/ESC EXITS" in c and
            "HOST EJECTED SD REMOTE MOUNT" in c and
            "SD Card Remote Mounting" in c and
            "Appletini is servicing the USB mass-storage bridge only." in c,
            "SD-card remote mounting must be an explicit modal state")
    require("control_set_sdd_stream_enabled" in fb and
            "control_set_usb0_sd_remote_mount" in fb and
            "usb_storage_service_connect();" in fb and
            "usb_storage_service_disconnect();" in fb and
            "usb_storage_service_consume_host_eject_request() != 0U" in fb,
            "frontend must attach USB0 storage only through the USB remote-mount platform op and close on host eject")
    require("if (!usb_sdd_service_active()) {\n        usb_storage_service_connect();"
            not in fb and
            "USB0 is detached by default" in fb,
            "boot must not attach mass storage by default when SDD is off")
    require("SDD: USB0 detached" in sdd and
            "usb_storage_service_connect()" not in sdd,
            "stopping SDD must leave USB0 detached instead of auto-reconnecting storage")


def test_usb0_single_owner_routing() -> None:
    fb = read(FRONTEND_MAIN_C)
    require("if (usb_sdd_service_active()) {\n        usb_sdd_service_poll();"
            in fb,
            "usb0 poll must route to exactly one personality")
    uart = read(UART_CONTROL_C)
    require('str_ieq(argv[0], "sdd")' in uart and
            "usb_sdd_service_start()" in uart and
            "usb_sdd_service_stop()" in uart,
            "uart control must expose sdd on/off/status")
    ws = read(WORKSPACE_PY)
    for src in ("usb_sdd_service.c", "usb0_personality.c",
                "xusbps_ch9_sddvendor.c"):
        require(src in ws, f"workspace generator must list {src}")


TESTS = [
    test_tap_event_word_matches_sdd_parser,
    test_apple_top_second_egress_on_hp2,
    test_vendor_identity_and_pipes,
    test_personality_dispatch,
    test_sdd_service_protocol,
    test_usb_tab_sdd_and_sd_remote_mount,
    test_usb0_single_owner_routing,
]


def main() -> int:
    failures = []
    for test in TESTS:
        try:
            test()
            print(f"PASS {test.__name__}")
        except TestFailure as exc:
            failures.append(test.__name__)
            print(f"FAIL {test.__name__}: {exc}")
    if failures:
        print(f"{len(TESTS) - len(failures)} of {len(TESTS)} SDD stream "
              f"tests passed; {len(failures)} failed")
        return 1
    print(f"{len(TESTS)} SDD stream tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
